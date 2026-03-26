#include <cuda_runtime.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "batch_dispatch.cuh"
#include "face_extract.cuh"
#include "face_port_io.h"
#include "gpu_uf.cuh"
#include "row_sieve.cuh"
#include "tile_kernel.cuh"
#include "types.h"

namespace {

enum ExitCode : int {
    kExitSuccess = 0,
    kExitBadArgs = 1,
    kExitCudaOom = 2,
    kExitCudaFailure = 3,
    kExitIoError = 4,
};

class AppError final : public std::runtime_error {
public:
    AppError(int code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    int code() const noexcept { return code_; }

private:
    int code_;
};

struct Config {
    std::string jobs_path;
    std::string output_path;
    uint32_t batch_size = 0;
    int device = 0;
    bool have_jobs = false;
    bool have_output = false;
    bool gpu_uf = false;  // --gpu-uf: run UF on GPU, skip D2H bitmap transfer
};

struct FileHandle {
    std::FILE* file = nullptr;
    bool should_close = false;

    FileHandle() = default;
    FileHandle(std::FILE* stream, bool close_stream)
        : file(stream), should_close(close_stream) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept
        : file(other.file), should_close(other.should_close) {
        other.file = nullptr;
        other.should_close = false;
    }
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            if (should_close && file != nullptr) {
                std::fclose(file);
            }
            file = other.file;
            should_close = other.should_close;
            other.file = nullptr;
            other.should_close = false;
        }
        return *this;
    }

    ~FileHandle() {
        if (should_close && file != nullptr) {
            std::fclose(file);
        }
    }
};

[[noreturn]] void fail(int code, const std::string& message) {
    throw AppError(code, message);
}

void check_cuda(cudaError_t status, const char* expr, const char* file, int line) {
    if (status == cudaSuccess) {
        return;
    }

    const int code = status == cudaErrorMemoryAllocation ? kExitCudaOom : kExitCudaFailure;
    fail(
        code,
        std::string("CUDA error at ") + file + ":" + std::to_string(line) +
            " for " + expr + ": " + cudaGetErrorString(status));
}

#define CUDA_CHECK(call) check_cuda((call), #call, __FILE__, __LINE__)

void print_usage(const char* program) {
    std::fprintf(
        stderr,
        "Usage: %s --jobs PATH --output PATH [--batch-size N] [--device N]\n",
        program);
}

uint64_t parse_u64(const char* text, const char* name) {
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        fail(kExitBadArgs, std::string("invalid value for ") + name + ": " + text);
    }
    return static_cast<uint64_t>(value);
}

int parse_int(const char* text, const char* name) {
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        fail(kExitBadArgs, std::string("invalid value for ") + name + ": " + text);
    }
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        fail(kExitBadArgs, std::string("value out of range for ") + name);
    }
    return static_cast<int>(value);
}

Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--jobs") == 0 && i + 1 < argc) {
            cfg.jobs_path = argv[++i];
            cfg.have_jobs = true;
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            cfg.output_path = argv[++i];
            cfg.have_output = true;
        } else if (std::strcmp(argv[i], "--batch-size") == 0 && i + 1 < argc) {
            const uint64_t value = parse_u64(argv[++i], "--batch-size");
            if (value > std::numeric_limits<uint32_t>::max()) {
                fail(kExitBadArgs, "--batch-size exceeds uint32_t range");
            }
            cfg.batch_size = static_cast<uint32_t>(value);
        } else if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            cfg.device = parse_int(argv[++i], "--device");
        } else if (std::strcmp(argv[i], "--gpu-uf") == 0) {
            cfg.gpu_uf = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            std::exit(kExitSuccess);
        } else {
            print_usage(argv[0]);
            fail(kExitBadArgs, std::string("unknown or incomplete argument: ") + argv[i]);
        }
    }

    if (!cfg.have_jobs || !cfg.have_output) {
        print_usage(argv[0]);
        fail(kExitBadArgs, "missing required arguments");
    }
    if (cfg.device < 0) {
        fail(kExitBadArgs, "--device must be non-negative");
    }
    return cfg;
}

uint64_t ceil_sqrt_u64(uint64_t value) {
    if (value == 0) {
        return 0;
    }

    uint64_t root = static_cast<uint64_t>(std::sqrt(static_cast<long double>(value)));
    while (static_cast<unsigned __int128>(root) * root < value) {
        ++root;
    }
    while (root > 0 &&
           static_cast<unsigned __int128>(root - 1) * static_cast<unsigned __int128>(root - 1) >=
               value) {
        --root;
    }
    return root;
}

TileGeometry make_tile_geometry(uint64_t k_sq, int32_t a_lo, int32_t b_lo, uint32_t tile_side) {
    TileGeometry geom{};
    geom.k_sq = k_sq;
    geom.collar = static_cast<int64_t>(ceil_sqrt_u64(k_sq));
    geom.a_lo = static_cast<int64_t>(a_lo);
    geom.a_hi = geom.a_lo + static_cast<int64_t>(tile_side);
    geom.b_lo = static_cast<int64_t>(b_lo);
    geom.b_hi = geom.b_lo + static_cast<int64_t>(tile_side);
    geom.expanded_a_lo = geom.a_lo - geom.collar;
    geom.expanded_b_lo = geom.b_lo - geom.collar;
    geom.nominal_extent = static_cast<uint64_t>(tile_side) + 1ULL;
    geom.side_exp = geom.nominal_extent + 2ULL * static_cast<uint64_t>(geom.collar);
    if (geom.side_exp == 0 ||
        geom.side_exp > std::numeric_limits<uint64_t>::max() / geom.side_exp) {
        fail(kExitIoError, "expanded tile dimensions overflow");
    }
    geom.total_points = geom.side_exp * geom.side_exp;
    if (geom.total_points > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        fail(kExitIoError, "expanded tile exceeds uint32_t union-find address space");
    }
    return geom;
}

FileHandle open_input_file(const std::string& path) {
    if (path == "-") {
        return FileHandle{stdin, false};
    }

    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        fail(kExitIoError, "failed to open input file: " + path);
    }
    return FileHandle{file, true};
}

FileHandle open_output_file(const std::string& path) {
    if (path == "-") {
        return FileHandle{stdout, false};
    }

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        fail(kExitIoError, "failed to open output file: " + path);
    }
    return FileHandle{file, true};
}

template <typename T>
void read_exact(std::FILE* file, T* value, const char* what) {
    if (std::fread(value, sizeof(T), 1, file) != 1) {
        fail(kExitIoError, std::string("failed to read ") + what);
    }
}

template <typename T>
void read_exact_array(std::FILE* file, T* values, size_t count, const char* what) {
    if (count == 0) {
        return;
    }
    if (std::fread(values, sizeof(T), count, file) != count) {
        fail(kExitIoError, std::string("failed to read ") + what);
    }
}

template <typename T>
void write_exact(std::FILE* file, const T& value, const char* what) {
    if (std::fwrite(&value, sizeof(T), 1, file) != 1) {
        fail(kExitIoError, std::string("failed to write ") + what);
    }
}

template <typename T>
void write_exact_array(std::FILE* file, const std::vector<T>& values, const char* what) {
    if (values.empty()) {
        return;
    }
    if (std::fwrite(values.data(), sizeof(T), values.size(), file) != values.size()) {
        fail(kExitIoError, std::string("failed to write ") + what);
    }
}

void validate_manifest_header(const FatStripeJobHeader& header) {
    static const uint8_t kExpectedMagic[4] = {'G', 'M', 'T', 'J'};
    if (std::memcmp(header.magic, kExpectedMagic, sizeof(kExpectedMagic)) != 0) {
        fail(kExitIoError, "invalid job manifest magic");
    }
    if (header.version != 1) {
        fail(kExitIoError, "unsupported job manifest version");
    }
    if (header.flags != 0) {
        fail(kExitIoError, "unsupported job manifest flags");
    }
    if (header.tile_side == 0) {
        fail(kExitIoError, "job manifest tile_side must be non-zero");
    }
}

std::vector<TileJob> read_manifest_jobs(std::FILE* file, const FatStripeJobHeader& header) {
    std::vector<TileJob> jobs(static_cast<size_t>(header.num_jobs));
    read_exact_array(file, jobs.data(), jobs.size(), "tile jobs");
    return jobs;
}

void write_tile_result(
    std::FILE* output,
    const TileJob& job,
    uint32_t tile_side,
    const gm::TileFacePorts& ports
) {
    const TileResultHeader header{
        job.tile_id,
        job.a_lo,
        job.b_lo,
        tile_side,
        ports.num_components,
        static_cast<uint32_t>(ports.face_inner.size()),
        static_cast<uint32_t>(ports.face_outer.size()),
        static_cast<uint32_t>(ports.face_left.size()),
        static_cast<uint32_t>(ports.face_right.size()),
        ports.num_primes,
        ports.origin_component,
    };

    write_exact(output, header, "tile result header");
    write_exact_array(output, ports.face_inner, "inner face ports");
    write_exact_array(output, ports.face_outer, "outer face ports");
    write_exact_array(output, ports.face_left, "left face ports");
    write_exact_array(output, ports.face_right, "right face ports");
}

// Build a TileFacePorts from GPU UF host-mirror results for one tile.
// Defined here (not in gpu_uf.cu) to avoid pulling tile_kernel.cuh into
// that translation unit a second time.
gm::TileFacePorts tile_face_ports_from_gpu_uf(
    const gm::GpuUfContext& ctx,
    uint32_t                tile_idx,
    const TileJob&          job,
    uint32_t                tile_side,
    int64_t                 collar
) {
    const uint32_t* fc  = ctx.h_face_counts + tile_idx * 4u;
    const uint64_t  off = static_cast<uint64_t>(tile_idx) * gm::kMaxFacePortsPerFace;

    gm::TileFacePorts result;
    result.num_components   = ctx.h_num_components[tile_idx];
    result.num_primes       = ctx.h_num_primes[tile_idx];
    result.origin_component = ctx.h_origin_component[tile_idx];

    auto copy_vec = [](const FacePortRecord* src, uint32_t count) {
        return std::vector<FacePortRecord>(src, src + static_cast<size_t>(count));
    };
    result.face_inner = copy_vec(ctx.h_face_inner + off, fc[0]);
    result.face_outer = copy_vec(ctx.h_face_outer + off, fc[1]);
    result.face_left  = copy_vec(ctx.h_face_left  + off, fc[2]);
    result.face_right = copy_vec(ctx.h_face_right + off, fc[3]);
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Config cfg = parse_args(argc, argv);

        FileHandle input = open_input_file(cfg.jobs_path);
        FatStripeJobHeader manifest{};
        read_exact(input.file, &manifest, "job manifest header");
        validate_manifest_header(manifest);
        std::vector<TileJob> jobs = read_manifest_jobs(input.file, manifest);

        int device_count = 0;
        CUDA_CHECK(cudaGetDeviceCount(&device_count));
        if (device_count == 0) {
            fail(kExitCudaFailure, "no CUDA devices found");
        }
        if (cfg.device >= device_count) {
            fail(kExitBadArgs, "requested CUDA device index is out of range");
        }

        CUDA_CHECK(cudaSetDevice(cfg.device));
        CUDA_CHECK(gm::init_row_sieve_tables());
        CUDA_CHECK(gm::copy_tile_k_sq(manifest.k_sq));

        const TileGeometry sample_geom = make_tile_geometry(manifest.k_sq, 0, 0, manifest.tile_side);
        gm::BatchContext batch_ctx;
        CUDA_CHECK(gm::create_batch_context(cfg.batch_size, sample_geom.side_exp, &batch_ctx));

        // Optional GPU UF context (allocated only when --gpu-uf is passed)
        gm::GpuUfContext gpu_uf_ctx{};
        if (cfg.gpu_uf) {
            CUDA_CHECK(gm::create_gpu_uf_context(
                batch_ctx.batch_capacity,
                sample_geom.total_points,
                &gpu_uf_ctx));
        }

        FileHandle output = open_output_file(cfg.output_path);
        const FacePortStreamHeader stream_header{
            {'G', 'M', 'F', 'P'},
            1,
            0,
            manifest.k_sq,
            manifest.tile_side,
            manifest.num_jobs,
            0,
        };
        write_exact(output.file, stream_header, "face port stream header");

        const int64_t collar = sample_geom.collar;
        for (uint32_t batch_start = 0; batch_start < manifest.num_jobs; batch_start += batch_ctx.batch_capacity) {
            const uint32_t batch_count =
                std::min(batch_ctx.batch_capacity, manifest.num_jobs - batch_start);

            CUDA_CHECK(gm::launch_batch_sieve(
                batch_ctx,
                jobs.data() + batch_start,
                batch_count,
                manifest.tile_side,
                collar));

            std::vector<gm::TileFacePorts> batch_results(static_cast<size_t>(batch_count));

            if (cfg.gpu_uf) {
                // GPU UF path: bitmaps stay on device, only face ports come back
                CUDA_CHECK(gm::run_gpu_uf(
                    gpu_uf_ctx,
                    batch_ctx.d_bitmaps,
                    batch_ctx.d_jobs,
                    batch_count,
                    manifest.tile_side,
                    collar,
                    manifest.k_sq,
                    sample_geom.side_exp,
                    batch_ctx.bitmap_words));

                for (uint32_t i = 0; i < batch_count; ++i) {
                    const TileJob& job = jobs[static_cast<size_t>(batch_start) + static_cast<size_t>(i)];
                    batch_results[static_cast<size_t>(i)] =
                        tile_face_ports_from_gpu_uf(gpu_uf_ctx, i, job, manifest.tile_side, collar);
                }
            } else {
                // CPU UF path (original): transfer bitmaps to host, then classify
                CUDA_CHECK(gm::transfer_batch_bitmaps(batch_ctx, batch_count));

#if defined(_OPENMP)
#pragma omp parallel for schedule(dynamic)
#endif
                for (int64_t i = 0; i < static_cast<int64_t>(batch_count); ++i) {
                    const TileJob& job = jobs[static_cast<size_t>(batch_start) + static_cast<size_t>(i)];
                    const TileGeometry geom =
                        make_tile_geometry(manifest.k_sq, job.a_lo, job.b_lo, manifest.tile_side);
                    batch_results[static_cast<size_t>(i)] =
                        gm::extract_face_ports(geom, gm::host_bitmap_slice(batch_ctx, static_cast<uint32_t>(i)), manifest.k_sq);
                }
            }

            for (uint32_t i = 0; i < batch_count; ++i) {
                write_tile_result(
                    output.file,
                    jobs[static_cast<size_t>(batch_start) + static_cast<size_t>(i)],
                    manifest.tile_side,
                    batch_results[static_cast<size_t>(i)]);
            }
        }

        if (std::fflush(output.file) != 0) {
            fail(kExitIoError, "failed to flush output stream");
        }

        if (cfg.gpu_uf) {
            gm::destroy_gpu_uf_context(&gpu_uf_ctx);
        }
        gm::destroy_batch_context(&batch_ctx);
        return kExitSuccess;
    } catch (const AppError& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return error.code();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return kExitCudaFailure;
    }
}
