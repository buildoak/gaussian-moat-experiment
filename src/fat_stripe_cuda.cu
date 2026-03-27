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
#include <type_traits>
#include <unordered_map>
#include <utility>
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
    bool gpu_boundary_merge = false; // --gpu-boundary-merge: compose campaign on GPU
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
        "Usage: %s --jobs PATH --output PATH [--batch-size N] [--device N] [--gpu-uf] [--gpu-boundary-merge]\n",
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
        } else if (std::strcmp(argv[i], "--gpu-boundary-merge") == 0) {
            cfg.gpu_boundary_merge = true;
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

struct TileFaceSpan {
    uint32_t inner_offset = 0;
    uint32_t inner_count = 0;
    uint32_t outer_offset = 0;
    uint32_t outer_count = 0;
    uint32_t left_offset = 0;
    uint32_t left_count = 0;
    uint32_t right_offset = 0;
    uint32_t right_count = 0;
    uint32_t num_components = 0;
    uint32_t num_primes = 0;
};

struct MergePort {
    int32_t a;
    int32_t b;
    uint32_t node_id;
};

struct SeamPair {
    uint32_t tile_a;
    uint32_t tile_b;
};

struct PreparedMergeData {
    std::vector<MergePort> inner_ports;
    std::vector<MergePort> outer_ports;
    std::vector<MergePort> left_ports;
    std::vector<MergePort> right_ports;
    std::vector<uint32_t> inner_offsets;
    std::vector<uint32_t> outer_offsets;
    std::vector<uint32_t> left_offsets;
    std::vector<uint32_t> right_offsets;
    std::vector<SeamPair> horizontal_seams;
    std::vector<SeamPair> vertical_seams;
    std::vector<uint8_t> node_face_bits;
    uint64_t total_primes = 0;
    uint32_t num_tiles = 0;
    int64_t a_min = 0;
    int64_t a_max = 0;
    int64_t b_min = 0;
    int64_t b_max = 0;
};

uint64_t pack_tile_key(int32_t a_lo, int32_t b_lo) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(a_lo)) << 32u) |
           static_cast<uint32_t>(b_lo);
}

void append_records(
    std::vector<FacePortRecord>& dst,
    const FacePortRecord* src,
    uint32_t count
) {
    if (count == 0u) {
        return;
    }
    dst.insert(dst.end(), src, src + static_cast<size_t>(count));
}

PreparedMergeData prepare_boundary_merge_data(
    const std::vector<TileJob>& jobs,
    uint32_t tile_side,
    const std::vector<TileFaceSpan>& tile_spans,
    const std::vector<FacePortRecord>& all_inner,
    const std::vector<FacePortRecord>& all_outer,
    const std::vector<FacePortRecord>& all_left,
    const std::vector<FacePortRecord>& all_right
) {
    if (jobs.size() != tile_spans.size()) {
        fail(kExitIoError, "tile span count mismatch while preparing boundary merge");
    }

    PreparedMergeData out;
    out.num_tiles = static_cast<uint32_t>(jobs.size());
    out.inner_offsets.resize(jobs.size() + 1u, 0u);
    out.outer_offsets.resize(jobs.size() + 1u, 0u);
    out.left_offsets.resize(jobs.size() + 1u, 0u);
    out.right_offsets.resize(jobs.size() + 1u, 0u);
    out.horizontal_seams.reserve(jobs.size());
    out.vertical_seams.reserve(jobs.size());

    std::unordered_map<uint64_t, uint32_t> tile_lookup;
    tile_lookup.reserve(jobs.size() * 2u);

    int32_t min_a_lo = 0;
    int32_t max_a_lo = 0;
    int32_t min_b_lo = 0;
    int32_t max_b_lo = 0;
    if (!jobs.empty()) {
        min_a_lo = max_a_lo = jobs.front().a_lo;
        min_b_lo = max_b_lo = jobs.front().b_lo;
    }
    for (size_t i = 0; i < jobs.size(); ++i) {
        const TileJob& job = jobs[i];
        tile_lookup.emplace(pack_tile_key(job.a_lo, job.b_lo), static_cast<uint32_t>(i));
        min_a_lo = std::min(min_a_lo, job.a_lo);
        max_a_lo = std::max(max_a_lo, job.a_lo);
        min_b_lo = std::min(min_b_lo, job.b_lo);
        max_b_lo = std::max(max_b_lo, job.b_lo);
    }

    const int64_t side = static_cast<int64_t>(tile_side);
    out.a_min = static_cast<int64_t>(min_a_lo);
    out.a_max = static_cast<int64_t>(max_a_lo) + side;
    out.b_min = static_cast<int64_t>(min_b_lo);
    out.b_max = static_cast<int64_t>(max_b_lo) + side;

    constexpr uint32_t kUnsetNode = 0xFFFFFFFFu;
    for (size_t tile_idx = 0; tile_idx < jobs.size(); ++tile_idx) {
        const TileJob& tile = jobs[tile_idx];
        const TileFaceSpan& span = tile_spans[tile_idx];
        out.total_primes += static_cast<uint64_t>(span.num_primes);

        out.inner_offsets[tile_idx] = static_cast<uint32_t>(out.inner_ports.size());
        out.outer_offsets[tile_idx] = static_cast<uint32_t>(out.outer_ports.size());
        out.left_offsets[tile_idx] = static_cast<uint32_t>(out.left_ports.size());
        out.right_offsets[tile_idx] = static_cast<uint32_t>(out.right_ports.size());

        std::vector<uint32_t> comp_to_node(span.num_components, kUnsetNode);
        auto node_for = [&](uint32_t component_id) -> uint32_t {
            if (component_id >= span.num_components) {
                fail(
                    kExitIoError,
                    "face port component_id out of range during boundary merge prep");
            }
            uint32_t node = comp_to_node[component_id];
            if (node == kUnsetNode) {
                node = static_cast<uint32_t>(out.node_face_bits.size());
                comp_to_node[component_id] = node;
                out.node_face_bits.push_back(0u);
            }
            return node;
        };

        const auto append_face = [&](const std::vector<FacePortRecord>& source,
                                     uint32_t offset,
                                     uint32_t count,
                                     uint8_t boundary_bit,
                                     std::vector<MergePort>& target) {
            const size_t begin = static_cast<size_t>(offset);
            const size_t end = begin + static_cast<size_t>(count);
            if (end > source.size()) {
                fail(kExitIoError, "face span exceeds collected face-port buffer");
            }
            for (size_t i = begin; i < end; ++i) {
                const FacePortRecord& port = source[i];
                const uint32_t node = node_for(port.component_id);
                if (boundary_bit != 0u) {
                    out.node_face_bits[node] |= boundary_bit;
                }
                target.push_back(MergePort{
                    port.a,
                    port.b,
                    node,
                });
            }
        };

        // Preserve seam ports for union, but only expose face bits on the
        // campaign's actual outer rows/columns.
        const bool touches_inner_boundary = tile.a_lo == min_a_lo;
        const bool touches_outer_boundary = tile.a_lo == max_a_lo;
        const bool touches_left_boundary = tile.b_lo == min_b_lo;
        const bool touches_right_boundary = tile.b_lo == max_b_lo;

        append_face(
            all_inner,
            span.inner_offset,
            span.inner_count,
            touches_inner_boundary ? gm::kFaceInnerBit : 0u,
            out.inner_ports);
        append_face(
            all_outer,
            span.outer_offset,
            span.outer_count,
            touches_outer_boundary ? gm::kFaceOuterBit : 0u,
            out.outer_ports);
        append_face(
            all_left,
            span.left_offset,
            span.left_count,
            touches_left_boundary ? gm::kFaceLeftBit : 0u,
            out.left_ports);
        append_face(
            all_right,
            span.right_offset,
            span.right_count,
            touches_right_boundary ? gm::kFaceRightBit : 0u,
            out.right_ports);
    }

    out.inner_offsets[jobs.size()] = static_cast<uint32_t>(out.inner_ports.size());
    out.outer_offsets[jobs.size()] = static_cast<uint32_t>(out.outer_ports.size());
    out.left_offsets[jobs.size()] = static_cast<uint32_t>(out.left_ports.size());
    out.right_offsets[jobs.size()] = static_cast<uint32_t>(out.right_ports.size());

    for (size_t i = 0; i < jobs.size(); ++i) {
        const TileJob& tile = jobs[i];
        const int64_t a_lo = static_cast<int64_t>(tile.a_lo);
        const int64_t b_lo = static_cast<int64_t>(tile.b_lo);

        const int64_t right_b = b_lo + side;
        if (right_b >= std::numeric_limits<int32_t>::min() &&
            right_b <= std::numeric_limits<int32_t>::max()) {
            const auto it = tile_lookup.find(pack_tile_key(tile.a_lo, static_cast<int32_t>(right_b)));
            if (it != tile_lookup.end()) {
                out.horizontal_seams.push_back(SeamPair{
                    static_cast<uint32_t>(i),
                    it->second,
                });
            }
        }

        const int64_t top_a = a_lo + side;
        if (top_a >= std::numeric_limits<int32_t>::min() &&
            top_a <= std::numeric_limits<int32_t>::max()) {
            const auto it = tile_lookup.find(pack_tile_key(static_cast<int32_t>(top_a), tile.b_lo));
            if (it != tile_lookup.end()) {
                out.vertical_seams.push_back(SeamPair{
                    static_cast<uint32_t>(i),
                    it->second,
                });
            }
        }
    }

    return out;
}

__device__ __forceinline__
uint32_t merge_uf_find(uint32_t* parent, uint32_t x) {
    while (true) {
        const uint32_t p = parent[x];
        if (p == x) {
            return x;
        }
        const uint32_t gp = parent[p];
        if (gp != p) {
            atomicCAS(&parent[x], p, gp);
        }
        x = gp;
    }
}

__device__ __forceinline__
void merge_uf_union(uint32_t* parent, uint32_t a, uint32_t b) {
    while (true) {
        uint32_t ra = merge_uf_find(parent, a);
        uint32_t rb = merge_uf_find(parent, b);
        if (ra == rb) {
            return;
        }
        if (ra > rb) {
            const uint32_t tmp = ra;
            ra = rb;
            rb = tmp;
        }
        if (atomicCAS(&parent[rb], rb, ra) == rb) {
            return;
        }
    }
}

__global__
void merge_init_parent_kernel(uint32_t* parent, uint32_t count) {
    const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) {
        parent[idx] = idx;
    }
}

__global__
void merge_seams_kernel(
    const MergePort* ports_a,
    const uint32_t* offsets_a,
    const MergePort* ports_b,
    const uint32_t* offsets_b,
    const SeamPair* seams,
    uint32_t num_seams,
    uint64_t k_sq,
    uint32_t* parent
) {
    const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_seams) {
        return;
    }

    const SeamPair seam = seams[idx];
    const uint32_t a_start = offsets_a[seam.tile_a];
    const uint32_t a_end = offsets_a[seam.tile_a + 1u];
    const uint32_t b_start = offsets_b[seam.tile_b];
    const uint32_t b_end = offsets_b[seam.tile_b + 1u];

    for (uint32_t i = a_start; i < a_end; ++i) {
        const MergePort pa = ports_a[i];
        for (uint32_t j = b_start; j < b_end; ++j) {
            const MergePort pb = ports_b[j];
            const int64_t da = static_cast<int64_t>(pa.a) - static_cast<int64_t>(pb.a);
            const int64_t db = static_cast<int64_t>(pa.b) - static_cast<int64_t>(pb.b);
            const uint64_t dist_sq =
                static_cast<uint64_t>(da * da) + static_cast<uint64_t>(db * db);
            if (dist_sq <= k_sq) {
                merge_uf_union(parent, pa.node_id, pb.node_id);
            }
        }
    }
}

__global__
void merge_flatten_kernel(uint32_t* parent, uint32_t count) {
    const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) {
        parent[idx] = merge_uf_find(parent, idx);
    }
}

struct MergeDeviceBuffers {
    MergePort* d_inner_ports = nullptr;
    MergePort* d_outer_ports = nullptr;
    MergePort* d_left_ports = nullptr;
    MergePort* d_right_ports = nullptr;
    uint32_t* d_inner_offsets = nullptr;
    uint32_t* d_outer_offsets = nullptr;
    uint32_t* d_left_offsets = nullptr;
    uint32_t* d_right_offsets = nullptr;
    SeamPair* d_horizontal_seams = nullptr;
    SeamPair* d_vertical_seams = nullptr;
    uint32_t* d_parent = nullptr;

    ~MergeDeviceBuffers() {
        if (d_inner_ports != nullptr) cudaFree(d_inner_ports);
        if (d_outer_ports != nullptr) cudaFree(d_outer_ports);
        if (d_left_ports != nullptr) cudaFree(d_left_ports);
        if (d_right_ports != nullptr) cudaFree(d_right_ports);
        if (d_inner_offsets != nullptr) cudaFree(d_inner_offsets);
        if (d_outer_offsets != nullptr) cudaFree(d_outer_offsets);
        if (d_left_offsets != nullptr) cudaFree(d_left_offsets);
        if (d_right_offsets != nullptr) cudaFree(d_right_offsets);
        if (d_horizontal_seams != nullptr) cudaFree(d_horizontal_seams);
        if (d_vertical_seams != nullptr) cudaFree(d_vertical_seams);
        if (d_parent != nullptr) cudaFree(d_parent);
    }
};

CampaignSummary run_gpu_boundary_merge(const PreparedMergeData& data, uint64_t k_sq) {
    CampaignSummary summary{};
    summary.total_primes = data.total_primes;
    summary.num_tiles = data.num_tiles;
    summary.num_components = 0u;
    summary.spanning_component = -1;
    summary.reserved = 0u;

    const uint32_t num_nodes = static_cast<uint32_t>(data.node_face_bits.size());
    if (num_nodes == 0u) {
        return summary;
    }

    MergeDeviceBuffers buffers;
    const auto alloc_and_copy = [](auto** dst, const auto& src) {
        using Ptr = std::remove_reference_t<decltype(*dst)>;
        using T = std::remove_pointer_t<Ptr>;
        if (src.empty()) {
            return;
        }
        CUDA_CHECK(cudaMalloc(dst, src.size() * sizeof(T)));
        CUDA_CHECK(cudaMemcpy(
            *dst,
            src.data(),
            src.size() * sizeof(T),
            cudaMemcpyHostToDevice));
    };

    alloc_and_copy(&buffers.d_inner_ports, data.inner_ports);
    alloc_and_copy(&buffers.d_outer_ports, data.outer_ports);
    alloc_and_copy(&buffers.d_left_ports, data.left_ports);
    alloc_and_copy(&buffers.d_right_ports, data.right_ports);
    alloc_and_copy(&buffers.d_inner_offsets, data.inner_offsets);
    alloc_and_copy(&buffers.d_outer_offsets, data.outer_offsets);
    alloc_and_copy(&buffers.d_left_offsets, data.left_offsets);
    alloc_and_copy(&buffers.d_right_offsets, data.right_offsets);
    alloc_and_copy(&buffers.d_horizontal_seams, data.horizontal_seams);
    alloc_and_copy(&buffers.d_vertical_seams, data.vertical_seams);

    CUDA_CHECK(cudaMalloc(&buffers.d_parent, static_cast<size_t>(num_nodes) * sizeof(uint32_t)));

    constexpr uint32_t kBlock = 256u;
    const uint32_t node_blocks = (num_nodes + kBlock - 1u) / kBlock;
    merge_init_parent_kernel<<<node_blocks, kBlock>>>(buffers.d_parent, num_nodes);
    CUDA_CHECK(cudaGetLastError());

    if (!data.horizontal_seams.empty()) {
        const uint32_t seam_blocks =
            (static_cast<uint32_t>(data.horizontal_seams.size()) + kBlock - 1u) / kBlock;
        merge_seams_kernel<<<seam_blocks, kBlock>>>(
            buffers.d_right_ports,
            buffers.d_right_offsets,
            buffers.d_left_ports,
            buffers.d_left_offsets,
            buffers.d_horizontal_seams,
            static_cast<uint32_t>(data.horizontal_seams.size()),
            k_sq,
            buffers.d_parent);
        CUDA_CHECK(cudaGetLastError());
    }

    if (!data.vertical_seams.empty()) {
        const uint32_t seam_blocks =
            (static_cast<uint32_t>(data.vertical_seams.size()) + kBlock - 1u) / kBlock;
        merge_seams_kernel<<<seam_blocks, kBlock>>>(
            buffers.d_outer_ports,
            buffers.d_outer_offsets,
            buffers.d_inner_ports,
            buffers.d_inner_offsets,
            buffers.d_vertical_seams,
            static_cast<uint32_t>(data.vertical_seams.size()),
            k_sq,
            buffers.d_parent);
        CUDA_CHECK(cudaGetLastError());
    }

    for (int pass = 0; pass < 3; ++pass) {
        merge_flatten_kernel<<<node_blocks, kBlock>>>(buffers.d_parent, num_nodes);
        CUDA_CHECK(cudaGetLastError());
    }
    CUDA_CHECK(cudaDeviceSynchronize());

    std::vector<uint32_t> parent(num_nodes);
    CUDA_CHECK(cudaMemcpy(
        parent.data(),
        buffers.d_parent,
        static_cast<size_t>(num_nodes) * sizeof(uint32_t),
        cudaMemcpyDeviceToHost));

    auto host_find = [&](uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    for (uint32_t i = 0; i < num_nodes; ++i) {
        parent[i] = host_find(i);
    }

    constexpr uint32_t kUnset = 0xFFFFFFFFu;
    std::vector<uint32_t> root_to_dense(num_nodes, kUnset);
    std::vector<uint8_t> dense_face_bits;
    dense_face_bits.reserve(num_nodes);

    for (uint32_t node = 0; node < num_nodes; ++node) {
        const uint32_t root = parent[node];
        uint32_t dense = root_to_dense[root];
        if (dense == kUnset) {
            dense = static_cast<uint32_t>(dense_face_bits.size());
            root_to_dense[root] = dense;
            dense_face_bits.push_back(0u);
        }
        dense_face_bits[dense] |= data.node_face_bits[node];
    }

    summary.num_components = static_cast<uint32_t>(dense_face_bits.size());

    // Match the Rust moat verdict by testing merged face-port coordinates
    // against radial thresholds instead of rectangular INNER|OUTER bits.
    const long double collar = std::ceil(std::sqrt(static_cast<long double>(k_sq)));
    const long double a_start = static_cast<long double>(data.a_min);
    const long double a_end = static_cast<long double>(data.a_max);
    const long double b_min = static_cast<long double>(data.b_min);
    const long double b_max = static_cast<long double>(data.b_max);
    const bool off_axis = data.b_min > 0;
    const long double r_inner_geom =
        off_axis ? std::sqrt(a_start * a_start + b_min * b_min) : a_start;
    const long double r_outer_geom =
        off_axis ? std::sqrt(a_end * a_end + b_max * b_max) : a_end;
    const long double r_inner_thresh = r_inner_geom + collar;
    const long double r_outer_thresh = std::max(r_outer_geom - collar, 0.0L);
    const long double r_inner_sq = r_inner_thresh * r_inner_thresh;
    const long double r_outer_sq = r_outer_thresh * r_outer_thresh;

    std::vector<uint8_t> has_inner(summary.num_components, 0u);
    std::vector<uint8_t> has_outer(summary.num_components, 0u);

    const auto mark_ports = [&](const std::vector<MergePort>& ports) {
        for (const MergePort& port : ports) {
            const uint32_t root = parent[port.node_id];
            const uint32_t dense = root_to_dense[root];
            const long double a = static_cast<long double>(port.a);
            const long double b = static_cast<long double>(port.b);
            const long double r_sq = a * a + b * b;
            if (r_sq <= r_inner_sq) {
                has_inner[dense] = 1u;
            }
            if (r_sq >= r_outer_sq) {
                has_outer[dense] = 1u;
            }
        }
    };

    mark_ports(data.inner_ports);
    mark_ports(data.outer_ports);
    mark_ports(data.left_ports);
    mark_ports(data.right_ports);

    for (uint32_t i = 0; i < summary.num_components; ++i) {
        if (has_inner[i] != 0u && has_outer[i] != 0u) {
            summary.spanning_component = static_cast<int32_t>(i);
            break;
        }
    }
    return summary;
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

        if (cfg.gpu_boundary_merge && !cfg.gpu_uf) {
            fail(kExitBadArgs, "--gpu-boundary-merge requires --gpu-uf");
        }

        const TileGeometry sample_geom = make_tile_geometry(manifest.k_sq, 0, 0, manifest.tile_side);

        uint32_t joint_batch_size = cfg.batch_size;
        if (cfg.gpu_uf && cfg.batch_size == 0 && manifest.num_jobs > 0u) {
            size_t free_mem = 0;
            size_t total_mem = 0;
            CUDA_CHECK(cudaMemGetInfo(&free_mem, &total_mem));
            (void)total_mem;

            const size_t reserve_bytes = 512ULL << 20;
            const size_t usable = free_mem > reserve_bytes ? free_mem - reserve_bytes : 0u;

            const size_t bitmap_words_local =
                (static_cast<size_t>(sample_geom.total_points) + 31u) / 32u;
            const size_t per_tile_bitmap =
                bitmap_words_local * sizeof(uint32_t) * 2u; // device + pinned host
            const size_t per_tile_uf =
                static_cast<size_t>(sample_geom.total_points) * (4u + 4u) + // parent + comp_id
                static_cast<size_t>(sample_geom.total_points) * 1u +         // rank
                4u * static_cast<size_t>(gm::kMaxFacePortsPerFace) * sizeof(FacePortRecord) * 2u +
                64u;
            const size_t per_tile_total = per_tile_bitmap + per_tile_uf;

            if (per_tile_total > 0u && usable >= per_tile_total) {
                const size_t tiles_by_vram = usable / per_tile_total;
                joint_batch_size = static_cast<uint32_t>(std::min<size_t>(
                    std::min<size_t>(tiles_by_vram, manifest.num_jobs),
                    std::numeric_limits<uint32_t>::max()));
                if (joint_batch_size == 0u) {
                    joint_batch_size = 1u;
                }
            } else {
                joint_batch_size = 1u;
            }

            std::fprintf(
                stderr,
                "gpu-uf: free VRAM %.1f GB, per-tile cost bitmap+uf=%.1f KB, joint_batch_cap=%u\n",
                static_cast<double>(free_mem) / (1024.0 * 1024.0 * 1024.0),
                static_cast<double>(per_tile_total) / 1024.0,
                joint_batch_size);
        }

        gm::BatchContext batch_ctx;
        CUDA_CHECK(gm::create_batch_context(joint_batch_size, sample_geom.side_exp, &batch_ctx));

        gm::GpuUfContext gpu_uf_ctx{};
        uint32_t effective_batch_capacity = batch_ctx.batch_capacity;
        if (cfg.gpu_uf && manifest.num_jobs > 0u) {
            CUDA_CHECK(gm::create_gpu_uf_context(
                effective_batch_capacity,
                sample_geom.total_points,
                &gpu_uf_ctx));
        }

        FileHandle output = open_output_file(cfg.output_path);
        if (!cfg.gpu_boundary_merge) {
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
        }

        std::vector<TileFaceSpan> merge_tile_spans;
        std::vector<FacePortRecord> merge_all_inner;
        std::vector<FacePortRecord> merge_all_outer;
        std::vector<FacePortRecord> merge_all_left;
        std::vector<FacePortRecord> merge_all_right;
        if (cfg.gpu_boundary_merge) {
            merge_tile_spans.resize(static_cast<size_t>(manifest.num_jobs));
            const size_t reserve_ports = static_cast<size_t>(manifest.num_jobs) * 16u;
            merge_all_inner.reserve(reserve_ports);
            merge_all_outer.reserve(reserve_ports);
            merge_all_left.reserve(reserve_ports);
            merge_all_right.reserve(reserve_ports);
        }

        const int64_t collar = sample_geom.collar;
        for (uint32_t batch_start = 0;
             batch_start < manifest.num_jobs;
             batch_start += effective_batch_capacity) {
            const uint32_t batch_count =
                std::min(effective_batch_capacity, manifest.num_jobs - batch_start);

            CUDA_CHECK(gm::launch_batch_sieve(
                batch_ctx,
                jobs.data() + batch_start,
                batch_count,
                manifest.tile_side,
                collar));

            if (cfg.gpu_uf) {
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

                if (cfg.gpu_boundary_merge) {
                    for (uint32_t i = 0; i < batch_count; ++i) {
                        const size_t global_idx =
                            static_cast<size_t>(batch_start) + static_cast<size_t>(i);
                        TileFaceSpan& span = merge_tile_spans[global_idx];
                        const uint32_t* fc = gpu_uf_ctx.h_face_counts + i * 4u;
                        const uint64_t off =
                            static_cast<uint64_t>(i) * gm::kMaxFacePortsPerFace;

                        span.inner_offset = static_cast<uint32_t>(merge_all_inner.size());
                        span.inner_count = fc[0];
                        span.outer_offset = static_cast<uint32_t>(merge_all_outer.size());
                        span.outer_count = fc[1];
                        span.left_offset = static_cast<uint32_t>(merge_all_left.size());
                        span.left_count = fc[2];
                        span.right_offset = static_cast<uint32_t>(merge_all_right.size());
                        span.right_count = fc[3];
                        span.num_components = gpu_uf_ctx.h_num_components[i];
                        span.num_primes = gpu_uf_ctx.h_num_primes[i];

                        append_records(
                            merge_all_inner,
                            gpu_uf_ctx.h_face_inner + off,
                            fc[0]);
                        append_records(
                            merge_all_outer,
                            gpu_uf_ctx.h_face_outer + off,
                            fc[1]);
                        append_records(
                            merge_all_left,
                            gpu_uf_ctx.h_face_left + off,
                            fc[2]);
                        append_records(
                            merge_all_right,
                            gpu_uf_ctx.h_face_right + off,
                            fc[3]);
                    }
                } else {
                    for (uint32_t i = 0; i < batch_count; ++i) {
                        const TileJob& job =
                            jobs[static_cast<size_t>(batch_start) + static_cast<size_t>(i)];
                        write_tile_result(
                            output.file,
                            job,
                            manifest.tile_side,
                            tile_face_ports_from_gpu_uf(gpu_uf_ctx, i, job, manifest.tile_side, collar));
                    }
                }
            } else {
                CUDA_CHECK(gm::transfer_batch_bitmaps(batch_ctx, batch_count));
                std::vector<gm::TileFacePorts> batch_results(static_cast<size_t>(batch_count));

#if defined(_OPENMP)
#pragma omp parallel for schedule(dynamic)
#endif
                for (int64_t i = 0; i < static_cast<int64_t>(batch_count); ++i) {
                    const TileJob& job = jobs[static_cast<size_t>(batch_start) + static_cast<size_t>(i)];
                    const TileGeometry geom =
                        make_tile_geometry(manifest.k_sq, job.a_lo, job.b_lo, manifest.tile_side);
                    batch_results[static_cast<size_t>(i)] =
                        gm::extract_face_ports(
                            geom,
                            gm::host_bitmap_slice(batch_ctx, static_cast<uint32_t>(i)),
                            manifest.k_sq);
                }

                if (cfg.gpu_boundary_merge) {
                    for (uint32_t i = 0; i < batch_count; ++i) {
                        const size_t global_idx =
                            static_cast<size_t>(batch_start) + static_cast<size_t>(i);
                        const gm::TileFacePorts& ports = batch_results[static_cast<size_t>(i)];
                        TileFaceSpan& span = merge_tile_spans[global_idx];
                        span.inner_offset = static_cast<uint32_t>(merge_all_inner.size());
                        span.inner_count = static_cast<uint32_t>(ports.face_inner.size());
                        span.outer_offset = static_cast<uint32_t>(merge_all_outer.size());
                        span.outer_count = static_cast<uint32_t>(ports.face_outer.size());
                        span.left_offset = static_cast<uint32_t>(merge_all_left.size());
                        span.left_count = static_cast<uint32_t>(ports.face_left.size());
                        span.right_offset = static_cast<uint32_t>(merge_all_right.size());
                        span.right_count = static_cast<uint32_t>(ports.face_right.size());
                        span.num_components = ports.num_components;
                        span.num_primes = ports.num_primes;

                        merge_all_inner.insert(
                            merge_all_inner.end(),
                            ports.face_inner.begin(),
                            ports.face_inner.end());
                        merge_all_outer.insert(
                            merge_all_outer.end(),
                            ports.face_outer.begin(),
                            ports.face_outer.end());
                        merge_all_left.insert(
                            merge_all_left.end(),
                            ports.face_left.begin(),
                            ports.face_left.end());
                        merge_all_right.insert(
                            merge_all_right.end(),
                            ports.face_right.begin(),
                            ports.face_right.end());
                    }
                } else {
                    for (uint32_t i = 0; i < batch_count; ++i) {
                        write_tile_result(
                            output.file,
                            jobs[static_cast<size_t>(batch_start) + static_cast<size_t>(i)],
                            manifest.tile_side,
                            batch_results[static_cast<size_t>(i)]);
                    }
                }
            }
        }

        if (cfg.gpu_boundary_merge) {
            const PreparedMergeData prepared = prepare_boundary_merge_data(
                jobs,
                manifest.tile_side,
                merge_tile_spans,
                merge_all_inner,
                merge_all_outer,
                merge_all_left,
                merge_all_right);
            const CampaignSummary summary = run_gpu_boundary_merge(prepared, manifest.k_sq);
            const FacePortStreamHeader stream_header{
                {'G', 'M', 'F', 'P'},
                1,
                kFacePortStreamFlagCampaignSummary,
                manifest.k_sq,
                manifest.tile_side,
                0u,
                0u,
            };
            write_exact(output.file, stream_header, "campaign summary stream header");
            write_exact(output.file, summary, "campaign summary");
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
