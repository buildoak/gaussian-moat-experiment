#include <cuda_runtime.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
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
#include "compact_types.cuh"
#include "face_extract.cuh"
#include "face_port_io.h"
#include "gpu_uf.cuh"
#include "row_sieve.cuh"
#include "tile_kernel.cuh"
#include "topology_prepass.cuh"
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
    bool compact_merge = false; // --compact-merge: compose from compact blob
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
        "Usage: %s --jobs PATH --output PATH [--batch-size N] [--device N] [--gpu-uf] [--gpu-boundary-merge] [--compact-merge]\n",
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
        } else if (std::strcmp(argv[i], "--compact-merge") == 0) {
            cfg.compact_merge = true;
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

struct CompactAccumulator {
    std::vector<uint8_t> blob;
    std::vector<uint64_t> tile_offsets;
    std::vector<uint32_t> component_counts;
    uint32_t total_components = 0;
    uint64_t total_primes = 0;
};

struct CampaignTopology {
    gm::TopologyPrepass topo;
    std::vector<uint8_t> manifest_exposed_face_masks;
};

struct GpuUfCompactStaging {
    uint8_t* d_exposed_face_masks = nullptr;
    uint8_t* d_compact_output = nullptr;
    uint8_t* d_compact_status = nullptr;

    ~GpuUfCompactStaging() {
        if (d_exposed_face_masks != nullptr) cudaFree(d_exposed_face_masks);
        if (d_compact_output != nullptr) cudaFree(d_compact_output);
        if (d_compact_status != nullptr) cudaFree(d_compact_status);
    }
};

template <typename Context>
auto bind_exposed_face_masks(Context& ctx, uint8_t* ptr, int)
    -> decltype(ctx.d_exposed_face_masks = ptr, void()) {
    ctx.d_exposed_face_masks = ptr;
}

template <typename Context>
void bind_exposed_face_masks(Context&, uint8_t*, long) {}

template <typename Context>
auto bind_compact_output(Context& ctx, uint8_t* ptr, int)
    -> decltype(ctx.d_compact_output = ptr, void()) {
    ctx.d_compact_output = ptr;
}

template <typename Context>
void bind_compact_output(Context&, uint8_t*, long) {}

template <typename Context>
auto bind_compact_slots(Context& ctx, uint8_t* ptr, int)
    -> decltype(ctx.d_compact_slots = ptr, void()) {
    ctx.d_compact_slots = ptr;
}

template <typename Context>
void bind_compact_slots(Context&, uint8_t*, long) {}

template <typename Context>
auto bind_compact_status(Context& ctx, uint8_t* ptr, int)
    -> decltype(ctx.d_compact_status = ptr, void()) {
    ctx.d_compact_status = ptr;
}

template <typename Context>
void bind_compact_status(Context&, uint8_t*, long) {}

template <typename Context>
void bind_gpu_uf_compact_buffers(
    Context& ctx,
    uint8_t* d_exposed_face_masks,
    uint8_t* d_compact_output,
    uint8_t* d_compact_status
) {
    bind_exposed_face_masks(ctx, d_exposed_face_masks, 0);
    bind_compact_output(ctx, d_compact_output, 0);
    bind_compact_slots(ctx, d_compact_output, 0);
    bind_compact_status(ctx, d_compact_status, 0);
}

template <typename... Args>
auto run_gpu_uf_dispatch(int, Args&&... args)
    -> decltype(gm::run_gpu_uf(std::forward<Args>(args)...)) {
    return gm::run_gpu_uf(std::forward<Args>(args)...);
}

inline cudaError_t run_gpu_uf_dispatch(
    long,
    gm::GpuUfContext& ctx,
    const uint32_t* d_bitmaps,
    const TileJob* d_jobs,
    uint32_t /*tile_base*/,
    const uint8_t* /*d_exposed_face_masks*/,
    uint32_t num_tiles,
    uint32_t tile_side,
    int64_t collar,
    uint64_t k_sq,
    uint64_t side_exp,
    size_t bitmap_words
) {
    return gm::run_gpu_uf(
        ctx,
        d_bitmaps,
        d_jobs,
        num_tiles,
        tile_side,
        collar,
        k_sq,
        side_exp,
        bitmap_words);
}

CampaignTopology compute_campaign_topology(
    const std::vector<TileJob>& jobs,
    uint32_t tile_side,
    uint64_t k_sq
) {
    CampaignTopology campaign{};
    campaign.manifest_exposed_face_masks.resize(jobs.size(), 0u);
    if (jobs.empty()) {
        return campaign;
    }
    if (k_sq > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        fail(kExitBadArgs, "k_sq exceeds topology prepass uint32_t range");
    }

    int64_t r_min = static_cast<int64_t>(jobs.front().a_lo);
    int64_t r_max = r_min + static_cast<int64_t>(tile_side);
    int64_t b_min = static_cast<int64_t>(jobs.front().b_lo);
    int64_t b_max = b_min + static_cast<int64_t>(tile_side);
    for (const TileJob& job : jobs) {
        const int64_t a_lo = static_cast<int64_t>(job.a_lo);
        const int64_t b_lo = static_cast<int64_t>(job.b_lo);
        r_min = std::min(r_min, a_lo);
        r_max = std::max(r_max, a_lo + static_cast<int64_t>(tile_side));
        b_min = std::min(b_min, b_lo);
        b_max = std::max(b_max, b_lo + static_cast<int64_t>(tile_side));
    }

    campaign.topo = gm::compute_topology(
        r_min,
        r_max,
        b_min,
        b_max,
        tile_side,
        static_cast<uint32_t>(k_sq));

    if (campaign.topo.exposed_face_masks.empty()) {
        return campaign;
    }

    const int64_t side = static_cast<int64_t>(campaign.topo.grid.tile_side);
    if (side <= 0) {
        fail(kExitIoError, "invalid topology tile_side");
    }

    for (size_t i = 0; i < jobs.size(); ++i) {
        const TileJob& job = jobs[i];
        const int64_t a_delta = static_cast<int64_t>(job.a_lo) - campaign.topo.grid.r_min;
        const int64_t b_delta = static_cast<int64_t>(job.b_lo) - campaign.topo.grid.b_min;
        if (a_delta < 0 || b_delta < 0 || (a_delta % side) != 0 || (b_delta % side) != 0) {
            fail(kExitIoError, "job manifest does not align to topology prepass tile grid");
        }

        const uint32_t r_tile = static_cast<uint32_t>(a_delta / side);
        const uint32_t b_tile = static_cast<uint32_t>(b_delta / side);
        if (r_tile >= campaign.topo.grid.tiles_r || b_tile >= campaign.topo.grid.tiles_b) {
            fail(kExitIoError, "job manifest tile falls outside topology prepass bounds");
        }

        const uint32_t topo_idx = r_tile * campaign.topo.grid.tiles_b + b_tile;
        campaign.manifest_exposed_face_masks[i] = campaign.topo.exposed_face_masks[topo_idx];
    }

    return campaign;
}

uint32_t checked_compact_tile_size(const gm::CompactTileHeader& header) {
    if (header.num_components > gm::kMaxPrimesPerTile) {
        fail(kExitIoError, "compact tile num_components exceeds kMaxPrimesPerTile");
    }
    if (header.num_ports > gm::kMaxBoundaryPortsPerTile) {
        fail(kExitIoError, "compact tile num_ports exceeds kMaxBoundaryPortsPerTile");
    }

    const uint32_t size = gm::compact_tile_size(header.num_components, header.num_ports);
    if (size > gm::kMaxCompactTileBytes) {
        fail(kExitIoError, "compact tile size exceeds kMaxCompactTileBytes");
    }
    return size;
}

void append_compact_batch(
    CompactAccumulator& accumulator,
    uint32_t batch_start,
    uint32_t batch_count,
    const std::vector<uint8_t>& compact_slots
) {
    const size_t slot_stride = static_cast<size_t>(gm::kMaxCompactTileBytes);
    const size_t required_bytes = static_cast<size_t>(batch_count) * slot_stride;
    if (compact_slots.size() < required_bytes) {
        fail(kExitIoError, "compact slot host staging buffer is smaller than the batch payload");
    }

    for (uint32_t i = 0; i < batch_count; ++i) {
        const uint32_t global_tile_idx = batch_start + i;
        const uint8_t* slot =
            compact_slots.data() + static_cast<size_t>(i) * slot_stride;
        const auto* header = reinterpret_cast<const gm::CompactTileHeader*>(slot);
        const uint32_t tile_bytes = checked_compact_tile_size(*header);
        const size_t dst_offset = accumulator.blob.size();

        accumulator.tile_offsets[global_tile_idx] = static_cast<uint64_t>(dst_offset);
        accumulator.component_counts[global_tile_idx] = header->num_components;
        accumulator.blob.resize(dst_offset + tile_bytes);
        std::memcpy(accumulator.blob.data() + dst_offset, slot, tile_bytes);
    }
}

void finalize_compact_accumulator(CompactAccumulator& accumulator) {
    uint32_t component_base = 0u;
    for (size_t tile_idx = 0; tile_idx < accumulator.tile_offsets.size(); ++tile_idx) {
        const uint64_t tile_offset = accumulator.tile_offsets[tile_idx];
        if (tile_offset > static_cast<uint64_t>(accumulator.blob.size())) {
            fail(kExitIoError, "compact tile offset exceeds packed compact blob");
        }

        auto* header = reinterpret_cast<gm::CompactTileHeader*>(
            accumulator.blob.data() + static_cast<size_t>(tile_offset));
        const uint32_t tile_bytes = checked_compact_tile_size(*header);
        if (tile_offset + tile_bytes > accumulator.blob.size()) {
            fail(kExitIoError, "packed compact blob contains a truncated tile");
        }
        if (component_base >
            std::numeric_limits<uint32_t>::max() - accumulator.component_counts[tile_idx]) {
            fail(kExitIoError, "global component count exceeds uint32_t range");
        }

        header->tile_idx = static_cast<uint32_t>(tile_idx);
        header->component_base = component_base;

        gm::CompactPortRecord* ports = gm::compact_ports(header, header->num_components);
        for (uint32_t p = 0; p < header->num_ports; ++p) {
            ports[p].comp_id += component_base;
        }

        component_base += accumulator.component_counts[tile_idx];
    }

    accumulator.total_components = component_base;
}

void validate_finalized_compact_blob(const CompactAccumulator& accumulator) {
    if (accumulator.tile_offsets.size() != accumulator.component_counts.size()) {
        fail(kExitIoError, "compact accumulator tile_offsets/component_counts size mismatch");
    }

    uint32_t expected_component_base = 0u;
    for (size_t tile_idx = 0; tile_idx < accumulator.tile_offsets.size(); ++tile_idx) {
        const uint64_t tile_offset = accumulator.tile_offsets[tile_idx];
        if (tile_offset > static_cast<uint64_t>(accumulator.blob.size())) {
            fail(kExitIoError, "finalized compact tile offset exceeds blob size");
        }

        const auto* header = reinterpret_cast<const gm::CompactTileHeader*>(
            accumulator.blob.data() + static_cast<size_t>(tile_offset));
        const uint32_t tile_bytes = checked_compact_tile_size(*header);
        if (tile_offset + tile_bytes > accumulator.blob.size()) {
            fail(kExitIoError, "finalized compact tile extends past blob end");
        }
        if (header->tile_idx != static_cast<uint32_t>(tile_idx)) {
            fail(kExitIoError, "finalized compact tile_idx mismatch");
        }
        if (header->num_components != accumulator.component_counts[tile_idx]) {
            fail(kExitIoError, "finalized compact component count mismatch");
        }
        if (header->component_base != expected_component_base) {
            fail(kExitIoError, "finalized compact component_base prefix mismatch");
        }

        const gm::CompactPortRecord* ports = gm::compact_ports(
            const_cast<void*>(static_cast<const void*>(header)),
            header->num_components);
        for (uint32_t p = 0; p < header->num_ports; ++p) {
            const uint32_t comp_id = ports[p].comp_id;
            if (comp_id < header->component_base ||
                comp_id >= header->component_base + header->num_components) {
                fail(kExitIoError, "finalized compact port comp_id is not globalized");
            }
        }

        if (expected_component_base >
            std::numeric_limits<uint32_t>::max() - header->num_components) {
            fail(kExitIoError, "finalized compact blob exceeds uint32_t component space");
        }
        expected_component_base += header->num_components;
    }

    if (expected_component_base != accumulator.total_components) {
        fail(kExitIoError, "finalized compact total_components mismatch");
    }
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
void merge_flatten_kernel(uint32_t* parent, uint32_t count) {
    const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) {
        parent[idx] = merge_uf_find(parent, idx);
    }
}

__device__ __forceinline__
uint32_t compact_merge_collar(uint32_t k_sq) {
    uint32_t collar = static_cast<uint32_t>(sqrt(static_cast<double>(k_sq)));
    while (static_cast<uint64_t>(collar) * static_cast<uint64_t>(collar) < k_sq) {
        ++collar;
    }
    while (collar > 0u &&
           static_cast<uint64_t>(collar - 1u) * static_cast<uint64_t>(collar - 1u) >= k_sq) {
        --collar;
    }
    return collar;
}

__device__ __forceinline__
bool compact_port_on_face(
    const gm::CompactPortRecord& port,
    uint8_t face,
    int64_t tile_side,
    uint32_t collar
) {
    switch (face) {
        case gm::FACE_INNER:
            return static_cast<uint64_t>(port.x) <= static_cast<uint64_t>(collar);
        case gm::FACE_OUTER:
            return tile_side >= static_cast<int64_t>(port.x) &&
                   static_cast<uint64_t>(tile_side - static_cast<int64_t>(port.x)) <=
                       static_cast<uint64_t>(collar);
        case gm::FACE_LEFT:
            return static_cast<uint64_t>(port.y) <= static_cast<uint64_t>(collar);
        case gm::FACE_RIGHT:
            return tile_side >= static_cast<int64_t>(port.y) &&
                   static_cast<uint64_t>(tile_side - static_cast<int64_t>(port.y)) <=
                       static_cast<uint64_t>(collar);
        default:
            return false;
    }
}

__global__
void merge_seams_compact_kernel(
    const uint8_t* __restrict__ compact_blob,
    const uint64_t* __restrict__ tile_offsets,
    const gm::SeamPair* __restrict__ seam_pairs,
    const gm::TileOrigin* __restrict__ tile_origins,
    uint32_t num_seams,
    uint32_t* __restrict__ merge_parent,
    uint32_t k_sq
) {
    const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_seams) {
        return;
    }

    const gm::SeamPair seam = seam_pairs[idx];
    const uint8_t* tile_a_data = compact_blob + tile_offsets[seam.tile_a];
    const auto* hdr_a = reinterpret_cast<const gm::CompactTileHeader*>(tile_a_data);
    const gm::CompactPortRecord* ports_a = gm::compact_ports(
        const_cast<void*>(static_cast<const void*>(tile_a_data)),
        hdr_a->num_components);

    const uint8_t* tile_b_data = compact_blob + tile_offsets[seam.tile_b];
    const auto* hdr_b = reinterpret_cast<const gm::CompactTileHeader*>(tile_b_data);
    const gm::CompactPortRecord* ports_b = gm::compact_ports(
        const_cast<void*>(static_cast<const void*>(tile_b_data)),
        hdr_b->num_components);

    const gm::TileOrigin origin_a = tile_origins[seam.tile_a];
    const gm::TileOrigin origin_b = tile_origins[seam.tile_b];

    const uint32_t collar = compact_merge_collar(k_sq);
    const uint8_t face_a = seam.axis == 0u ? gm::FACE_RIGHT : gm::FACE_OUTER;
    const uint8_t face_b = seam.axis == 0u ? gm::FACE_LEFT : gm::FACE_INNER;
    const int64_t tile_side =
        seam.axis == 0u ? (origin_b.b_lo - origin_a.b_lo) : (origin_b.a_lo - origin_a.a_lo);
    if (tile_side <= 0) {
        return;
    }

    for (uint32_t i = 0; i < hdr_a->num_ports; ++i) {
        const gm::CompactPortRecord pa = ports_a[i];
        if (!compact_port_on_face(pa, face_a, tile_side, collar)) {
            continue;
        }

        const int64_t pa_abs_a = origin_a.a_lo + static_cast<int64_t>(pa.x);
        const int64_t pa_abs_b = origin_a.b_lo + static_cast<int64_t>(pa.y);
        for (uint32_t j = 0; j < hdr_b->num_ports; ++j) {
            const gm::CompactPortRecord pb = ports_b[j];
            if (!compact_port_on_face(pb, face_b, tile_side, collar)) {
                continue;
            }

            const int64_t pb_abs_a = origin_b.a_lo + static_cast<int64_t>(pb.x);
            const int64_t pb_abs_b = origin_b.b_lo + static_cast<int64_t>(pb.y);
            const int64_t da = pa_abs_a - pb_abs_a;
            const int64_t db = pa_abs_b - pb_abs_b;
            const uint64_t dist_sq =
                static_cast<uint64_t>(da * da) + static_cast<uint64_t>(db * db);
            if (dist_sq <= static_cast<uint64_t>(k_sq)) {
                merge_uf_union(merge_parent, pa.comp_id, pb.comp_id);
            }
        }
    }
}

struct CompactMergeDeviceBuffers {
    uint8_t* d_compact_blob = nullptr;
    uint64_t* d_tile_offsets = nullptr;
    gm::SeamPair* d_seam_pairs = nullptr;
    gm::TileOrigin* d_tile_origins = nullptr;
    uint32_t* d_parent = nullptr;

    ~CompactMergeDeviceBuffers() {
        if (d_compact_blob != nullptr) cudaFree(d_compact_blob);
        if (d_tile_offsets != nullptr) cudaFree(d_tile_offsets);
        if (d_seam_pairs != nullptr) cudaFree(d_seam_pairs);
        if (d_tile_origins != nullptr) cudaFree(d_tile_origins);
        if (d_parent != nullptr) cudaFree(d_parent);
    }
};

void validate_compact_merge_topology(
    const CompactAccumulator& accumulator,
    const gm::TopologyPrepass& topo
) {
    if (accumulator.tile_offsets.size() != topo.tile_origins.size()) {
        fail(
            kExitIoError,
            "compact merge requires manifest tile order to match topology tile order");
    }
    if (topo.tile_origins.size() != topo.grid.total_tiles) {
        fail(kExitIoError, "topology tile_origins size mismatch");
    }

    for (size_t i = 0; i < topo.seam_pairs.size(); ++i) {
        const gm::SeamPair seam = topo.seam_pairs[i];
        if (seam.tile_a >= topo.tile_origins.size() || seam.tile_b >= topo.tile_origins.size()) {
            fail(kExitIoError, "topology seam pair references tile outside compact blob");
        }
    }
}

CampaignSummary run_compact_merge(
    const CompactAccumulator& accumulator,
    const gm::TopologyPrepass& topo,
    uint64_t k_sq
) {
    if (k_sq > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        fail(kExitBadArgs, "compact merge requires k_sq to fit in uint32_t");
    }

    CampaignSummary summary{};
    summary.total_primes = accumulator.total_primes;
    summary.num_tiles = static_cast<uint32_t>(accumulator.tile_offsets.size());
    summary.num_components = 0u;
    summary.spanning_component = -1;
    summary.reserved = 0u;

    validate_finalized_compact_blob(accumulator);
    validate_compact_merge_topology(accumulator, topo);

    const auto merge_start = std::chrono::steady_clock::now();

    const uint32_t total_global_components = accumulator.total_components;
    if (total_global_components == 0u) {
        const auto merge_end = std::chrono::steady_clock::now();
        const double merge_ms = std::chrono::duration<double, std::milli>(merge_end - merge_start).count();
        std::fprintf(
            stderr,
            "compact-merge: total_global_components=%u num_seams=%u merge_ms=%.3f spanning=false\n",
            total_global_components,
            static_cast<unsigned>(topo.seam_pairs.size()),
            merge_ms);
        return summary;
    }

    // Fix 1: Stream seam batches to avoid OOM from uploading the entire compact
    // blob at once.  Only d_parent lives for the full merge; all other device
    // buffers are scoped to each seam batch.
    //
    // Batch-size heuristic: query free VRAM, reserve 512 MB for the kernel,
    // and divide the remainder by per-tile cost.
    // Per-tile cost = kMaxCompactTileBytes (blob) + sizeof(uint64_t) (offset)
    //               + sizeof(TileOrigin) (origin) + seam overhead (small).
    constexpr size_t kMergeVramReserve = 512ULL << 20;
    constexpr size_t kPerTileBatchBytes =
        static_cast<size_t>(gm::kMaxCompactTileBytes) +
        sizeof(uint64_t) + sizeof(gm::TileOrigin);

    uint32_t max_tiles_per_batch = static_cast<uint32_t>(topo.tile_origins.size());
    {
        size_t free_mem = 0;
        size_t total_mem = 0;
        if (cudaMemGetInfo(&free_mem, &total_mem) == cudaSuccess && free_mem > kMergeVramReserve) {
            const size_t usable = free_mem - kMergeVramReserve;
            // Also subtract d_parent allocation that is about to happen.
            const size_t parent_bytes =
                static_cast<size_t>(total_global_components) * sizeof(uint32_t);
            const size_t for_tiles = usable > parent_bytes ? usable - parent_bytes : 0u;
            if (kPerTileBatchBytes > 0u && for_tiles >= kPerTileBatchBytes) {
                const size_t tiles_by_vram = for_tiles / kPerTileBatchBytes;
                max_tiles_per_batch = static_cast<uint32_t>(
                    std::min<size_t>(tiles_by_vram, std::numeric_limits<uint32_t>::max()));
                if (max_tiles_per_batch == 0u) {
                    max_tiles_per_batch = 1u;
                }
            }
        }
    }

    std::fprintf(
        stderr,
        "compact-merge: total_global_components=%u num_seams=%u max_tiles_per_batch=%u\n",
        total_global_components,
        static_cast<unsigned>(topo.seam_pairs.size()),
        max_tiles_per_batch);

    // Allocate d_parent for the full campaign — the only permanent device allocation.
    uint32_t* d_parent = nullptr;
    CUDA_CHECK(cudaMalloc(
        &d_parent,
        static_cast<size_t>(total_global_components) * sizeof(uint32_t)));

    // RAII guard for d_parent so it is freed on any exception path.
    struct ParentGuard {
        uint32_t* ptr = nullptr;
        ~ParentGuard() { if (ptr != nullptr) { cudaFree(ptr); } }
    } parent_guard;
    parent_guard.ptr = d_parent;

    constexpr uint32_t kBlock = 256u;
    const uint32_t node_blocks = (total_global_components + kBlock - 1u) / kBlock;
    merge_init_parent_kernel<<<node_blocks, kBlock>>>(d_parent, total_global_components);
    CUDA_CHECK(cudaGetLastError());

    if (!topo.seam_pairs.empty()) {
        // Build a global-tile-index → compact-blob-offset map for fast lookup.
        const size_t num_global_tiles = accumulator.tile_offsets.size();

        // Process seam_pairs in batches.  For each batch:
        //   1. Collect unique tile IDs touched by the batch seams.
        //   2. Build batch-local blob (concatenate just those tiles' compact data).
        //   3. Build batch-local tile_offsets and tile_origins, remapped to batch indices.
        //   4. Remap seam_pairs to batch-local tile indices.
        //   5. Upload and launch kernel.
        //   6. Free batch device buffers.

        // Pre-build a tile-index → tile-size table to avoid re-parsing headers.
        std::vector<uint32_t> tile_sizes(num_global_tiles);
        for (size_t t = 0; t < num_global_tiles; ++t) {
            const auto* hdr = reinterpret_cast<const gm::CompactTileHeader*>(
                accumulator.blob.data() +
                static_cast<size_t>(accumulator.tile_offsets[t]));
            tile_sizes[t] = gm::compact_tile_size(hdr->num_components, hdr->num_ports);
        }

        const size_t total_seams = topo.seam_pairs.size();
        size_t seam_batch_start = 0u;
        uint32_t batch_idx = 0u;

        while (seam_batch_start < total_seams) {
            // Greedy: include seams until adding the next seam would exceed max_tiles_per_batch
            // unique tiles.  Use a local set to track which tiles we've included.
            std::unordered_map<uint32_t, uint32_t> global_to_local;
            global_to_local.reserve(static_cast<size_t>(max_tiles_per_batch) * 2u);

            size_t seam_batch_end = seam_batch_start;
            while (seam_batch_end < total_seams) {
                const gm::SeamPair& sp = topo.seam_pairs[seam_batch_end];
                // Count how many new tiles this seam would add.
                const uint32_t count_before =
                    static_cast<uint32_t>(global_to_local.size());
                const bool a_new = (global_to_local.count(sp.tile_a) == 0u);
                const bool b_new = (global_to_local.count(sp.tile_b) == 0u);
                const uint32_t would_add =
                    (a_new ? 1u : 0u) + (b_new && sp.tile_b != sp.tile_a ? 1u : 0u);
                if (count_before + would_add > max_tiles_per_batch && count_before > 0u) {
                    // This seam would exceed the tile budget; stop here.
                    break;
                }
                // Admit the seam: assign local IDs to new tiles.
                if (a_new) {
                    global_to_local[sp.tile_a] =
                        static_cast<uint32_t>(global_to_local.size());
                }
                if (b_new && sp.tile_b != sp.tile_a) {
                    global_to_local[sp.tile_b] =
                        static_cast<uint32_t>(global_to_local.size());
                }
                ++seam_batch_end;
            }

            const uint32_t batch_num_tiles =
                static_cast<uint32_t>(global_to_local.size());
            const uint32_t batch_num_seams =
                static_cast<uint32_t>(seam_batch_end - seam_batch_start);

            std::fprintf(
                stderr,
                "compact-merge: seam batch %u: seams [%zu, %zu) = %u seams, %u tiles\n",
                batch_idx,
                seam_batch_start, seam_batch_end,
                batch_num_seams,
                batch_num_tiles);

            // Build ordered tile list (local_id → global_id).
            std::vector<uint32_t> local_to_global(batch_num_tiles);
            for (const auto& kv : global_to_local) {
                local_to_global[kv.second] = kv.first;
            }

            // Build batch-local compact blob, tile_offsets (relative to batch blob start),
            // and tile_origins.
            std::vector<uint8_t> batch_blob;
            std::vector<uint64_t> batch_tile_offsets(batch_num_tiles);
            std::vector<gm::TileOrigin> batch_tile_origins(batch_num_tiles);
            {
                size_t running_offset = 0u;
                for (uint32_t li = 0; li < batch_num_tiles; ++li) {
                    const uint32_t gi = local_to_global[li];
                    const size_t src_offset =
                        static_cast<size_t>(accumulator.tile_offsets[gi]);
                    const uint32_t sz = tile_sizes[gi];
                    batch_tile_offsets[li] = static_cast<uint64_t>(running_offset);
                    batch_tile_origins[li] = topo.tile_origins[gi];
                    const uint8_t* src = accumulator.blob.data() + src_offset;
                    batch_blob.insert(batch_blob.end(), src, src + sz);
                    running_offset += sz;
                }
            }

            // Build batch-local seam_pairs (remapped tile indices).
            std::vector<gm::SeamPair> batch_seam_pairs(batch_num_seams);
            for (uint32_t si = 0; si < batch_num_seams; ++si) {
                const gm::SeamPair& sp = topo.seam_pairs[seam_batch_start + si];
                batch_seam_pairs[si] = gm::SeamPair{
                    global_to_local.at(sp.tile_a),
                    global_to_local.at(sp.tile_b),
                    sp.axis,
                };
            }

            // Upload batch to device.
            uint8_t* d_batch_blob = nullptr;
            uint64_t* d_batch_offsets = nullptr;
            gm::SeamPair* d_batch_seams = nullptr;
            gm::TileOrigin* d_batch_origins = nullptr;

            // RAII guard for batch allocations.
            struct BatchGuard {
                uint8_t* blob = nullptr;
                uint64_t* offsets = nullptr;
                gm::SeamPair* seams = nullptr;
                gm::TileOrigin* origins = nullptr;
                ~BatchGuard() {
                    if (blob    != nullptr) cudaFree(blob);
                    if (offsets != nullptr) cudaFree(offsets);
                    if (seams   != nullptr) cudaFree(seams);
                    if (origins != nullptr) cudaFree(origins);
                }
            } batch_guard;

            if (!batch_blob.empty()) {
                CUDA_CHECK(cudaMalloc(&d_batch_blob, batch_blob.size()));
                batch_guard.blob = d_batch_blob;
                CUDA_CHECK(cudaMemcpy(
                    d_batch_blob, batch_blob.data(), batch_blob.size(),
                    cudaMemcpyHostToDevice));
            }
            if (!batch_tile_offsets.empty()) {
                CUDA_CHECK(cudaMalloc(
                    &d_batch_offsets,
                    batch_tile_offsets.size() * sizeof(uint64_t)));
                batch_guard.offsets = d_batch_offsets;
                CUDA_CHECK(cudaMemcpy(
                    d_batch_offsets,
                    batch_tile_offsets.data(),
                    batch_tile_offsets.size() * sizeof(uint64_t),
                    cudaMemcpyHostToDevice));
            }
            if (!batch_seam_pairs.empty()) {
                CUDA_CHECK(cudaMalloc(
                    &d_batch_seams,
                    batch_seam_pairs.size() * sizeof(gm::SeamPair)));
                batch_guard.seams = d_batch_seams;
                CUDA_CHECK(cudaMemcpy(
                    d_batch_seams,
                    batch_seam_pairs.data(),
                    batch_seam_pairs.size() * sizeof(gm::SeamPair),
                    cudaMemcpyHostToDevice));
            }
            if (!batch_tile_origins.empty()) {
                CUDA_CHECK(cudaMalloc(
                    &d_batch_origins,
                    batch_tile_origins.size() * sizeof(gm::TileOrigin)));
                batch_guard.origins = d_batch_origins;
                CUDA_CHECK(cudaMemcpy(
                    d_batch_origins,
                    batch_tile_origins.data(),
                    batch_tile_origins.size() * sizeof(gm::TileOrigin),
                    cudaMemcpyHostToDevice));
            }

            // Launch seam kernel for this batch.
            if (batch_num_seams > 0u) {
                const uint32_t seam_blocks =
                    (batch_num_seams + kBlock - 1u) / kBlock;
                merge_seams_compact_kernel<<<seam_blocks, kBlock>>>(
                    d_batch_blob,
                    d_batch_offsets,
                    d_batch_seams,
                    d_batch_origins,
                    batch_num_seams,
                    d_parent,
                    static_cast<uint32_t>(k_sq));
                CUDA_CHECK(cudaGetLastError());
                CUDA_CHECK(cudaDeviceSynchronize());
            }
            // BatchGuard destructor frees d_batch_blob, offsets, seams, origins here.

            seam_batch_start = seam_batch_end;
            ++batch_idx;
        }
    }

    for (int pass = 0; pass < 3; ++pass) {
        merge_flatten_kernel<<<node_blocks, kBlock>>>(d_parent, total_global_components);
        CUDA_CHECK(cudaGetLastError());
    }
    CUDA_CHECK(cudaDeviceSynchronize());

    std::vector<uint32_t> parent(total_global_components);
    CUDA_CHECK(cudaMemcpy(
        parent.data(),
        d_parent,
        static_cast<size_t>(total_global_components) * sizeof(uint32_t),
        cudaMemcpyDeviceToHost));

    auto host_find = [&](uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    for (uint32_t i = 0; i < total_global_components; ++i) {
        parent[i] = host_find(i);
    }

    // Fix 2: replace root_to_dense (4 bytes × total_global_components, ~660 MB at 165M
    // components) with root_flags (1 byte × total_global_components, ~165 MB).
    //
    // Strategy: use 4 bit-flags per root packed into one uint8_t:
    //   bits 0-3 = accumulated tile face-bits (same as before)
    //   bit  4   = kHasInnerBit: at least one port of this root is near the inner boundary
    //   bit  5   = kHasOuterBit: at least one port of this root is near the outer boundary
    //
    // Pass 1: sweep tiles, OR face-bits into root_flags[root].
    //   A root r is live iff root_flags[r] != 0 (it owns ≥1 component).
    // Count distinct live roots → summary.num_components.
    //
    // Pass 2: sweep ports, set kHasInnerBit/kHasOuterBit on root_flags[root].
    //
    // Spanning check: any root where (root_flags[r] & kHasInnerBit) &&
    //                               (root_flags[r] & kHasOuterBit) is a spanning component.
    // No dense-ID array needed.

    // Bit assignments in root_flags[]:
    //   bits 0-3 : accumulated compact face-bits (FACE_INNER/OUTER/LEFT/RIGHT)
    //   bit 4    : kHasInnerBit — at least one port near inner radial boundary
    //   bit 5    : kHasOuterBit — at least one port near outer radial boundary
    //   bit 7    : kLiveBit    — this index is the canonical root of ≥1 component
    constexpr uint8_t kHasInnerBit = 0x10u;
    constexpr uint8_t kHasOuterBit = 0x20u;
    constexpr uint8_t kLiveBit     = 0x80u;

    std::vector<uint8_t> root_flags(total_global_components, 0u);

    // Pass 1: sweep tiles, accumulate face-bits and mark live roots.
    for (size_t tile_idx = 0; tile_idx < accumulator.tile_offsets.size(); ++tile_idx) {
        const uint64_t tile_offset = accumulator.tile_offsets[tile_idx];
        const auto* header = reinterpret_cast<const gm::CompactTileHeader*>(
            accumulator.blob.data() + static_cast<size_t>(tile_offset));
        const uint8_t* face_bits = gm::compact_face_bits(
            const_cast<void*>(static_cast<const void*>(header)));

        for (uint32_t local_comp = 0; local_comp < header->num_components; ++local_comp) {
            const uint32_t global_comp = header->component_base + local_comp;
            const uint32_t root = parent[global_comp];
            root_flags[root] |= face_bits[local_comp] | kLiveBit;
        }
    }

    // Count distinct live roots = number of merged components.
    uint32_t num_dense = 0u;
    for (uint32_t r = 0; r < total_global_components; ++r) {
        if ((root_flags[r] & kLiveBit) != 0u) {
            ++num_dense;
        }
    }

    summary.num_components = num_dense;

    int64_t a_min = topo.grid.r_min;
    int64_t a_max = topo.grid.r_max;
    int64_t b_min = 0;
    int64_t b_max = topo.grid.b_max;
    if (!topo.tile_origins.empty()) {
        a_min = a_max = topo.tile_origins.front().a_lo;
        b_min = b_max = topo.tile_origins.front().b_lo;
        for (const gm::TileOrigin& origin : topo.tile_origins) {
            a_min = std::min(a_min, origin.a_lo);
            a_max = std::max(a_max, origin.a_lo + static_cast<int64_t>(topo.grid.tile_side));
            b_min = std::min(b_min, origin.b_lo);
            b_max = std::max(b_max, origin.b_lo + static_cast<int64_t>(topo.grid.tile_side));
        }
    }

    const long double collar =
        std::ceil(std::sqrt(static_cast<long double>(k_sq)));
    const bool off_axis = b_min > 0;
    const long double a_start = static_cast<long double>(a_min);
    const long double a_end = static_cast<long double>(a_max);
    const long double b_min_f = static_cast<long double>(b_min);
    const long double b_max_f = static_cast<long double>(b_max);
    const long double r_inner_geom =
        off_axis ? std::sqrt(a_start * a_start + b_min_f * b_min_f) : a_start;
    const long double r_outer_geom =
        off_axis ? std::sqrt(a_end * a_end + b_max_f * b_max_f) : a_end;
    const long double r_inner_thresh = r_inner_geom + collar;
    const long double r_outer_thresh = std::max(r_outer_geom - collar, 0.0L);
    const long double r_inner_sq = r_inner_thresh * r_inner_thresh;
    const long double r_outer_sq = r_outer_thresh * r_outer_thresh;

    // Pass 2: sweep ports, set kHasInnerBit/kHasOuterBit on root_flags[root].
    // No dense-ID mapping needed — we operate directly on the canonical root.
    for (size_t tile_idx = 0; tile_idx < accumulator.tile_offsets.size(); ++tile_idx) {
        const uint64_t tile_offset = accumulator.tile_offsets[tile_idx];
        const auto* header = reinterpret_cast<const gm::CompactTileHeader*>(
            accumulator.blob.data() + static_cast<size_t>(tile_offset));
        const gm::CompactPortRecord* ports = gm::compact_ports(
            const_cast<void*>(static_cast<const void*>(header)),
            header->num_components);
        const gm::TileOrigin origin = topo.tile_origins[tile_idx];

        for (uint32_t p = 0; p < header->num_ports; ++p) {
            const gm::CompactPortRecord& port = ports[p];
            const uint32_t root = parent[port.comp_id];
            const long double a =
                static_cast<long double>(origin.a_lo + static_cast<int64_t>(port.x));
            const long double b =
                static_cast<long double>(origin.b_lo + static_cast<int64_t>(port.y));
            const long double r_sq = a * a + b * b;
            if (r_sq <= r_inner_sq) {
                root_flags[root] |= kHasInnerBit;
            }
            if (r_sq >= r_outer_sq) {
                root_flags[root] |= kHasOuterBit;
            }
        }
    }

    // Find a spanning root: any live root with both inner and outer radial bits set.
    // Report as spanning_component = 0 (the API only checks >= 0 for existence).
    for (uint32_t r = 0; r < total_global_components; ++r) {
        if ((root_flags[r] & kLiveBit) != 0u &&
            (root_flags[r] & kHasInnerBit) != 0u &&
            (root_flags[r] & kHasOuterBit) != 0u) {
            summary.spanning_component = 0;
            break;
        }
    }

    const auto merge_end = std::chrono::steady_clock::now();
    const double merge_ms = std::chrono::duration<double, std::milli>(merge_end - merge_start).count();
    std::fprintf(
        stderr,
        "compact-merge: total_global_components=%u num_seams=%u merge_ms=%.3f spanning=%s\n",
        total_global_components,
        static_cast<unsigned>(topo.seam_pairs.size()),
        merge_ms,
        summary.spanning_component >= 0 ? "true" : "false");

    return summary;
}


// run_gpu_boundary_merge() deleted (Fix 5 — legacy path removed).
// Use run_compact_merge() exclusively.




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
        if (cfg.compact_merge && !cfg.gpu_boundary_merge) {
            fail(kExitBadArgs, "--compact-merge requires --gpu-boundary-merge");
        }
        if (cfg.compact_merge && !cfg.gpu_uf) {
            fail(kExitBadArgs, "--compact-merge requires --gpu-uf");
        }

        const TileGeometry sample_geom = make_tile_geometry(manifest.k_sq, 0, 0, manifest.tile_side);
        const CampaignTopology campaign_topology =
            cfg.gpu_uf ? compute_campaign_topology(jobs, manifest.tile_side, manifest.k_sq)
                       : CampaignTopology{};

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
                static_cast<size_t>(gm::kMaxPrimesPerTile) * (4u + 4u + 1u) + // parent + comp_id + rank
                4u * static_cast<size_t>(gm::kMaxFacePortsPerFace) * sizeof(FacePortRecord) * 2u +
                64u;
            const size_t per_tile_compact =
                static_cast<size_t>(gm::kMaxCompactTileBytes) + sizeof(uint8_t);
            const size_t fixed_topology_bytes =
                static_cast<size_t>(campaign_topology.manifest_exposed_face_masks.size()) *
                sizeof(uint8_t);
            const size_t per_tile_total = per_tile_bitmap + per_tile_uf + per_tile_compact;
            const size_t usable_for_batches =
                usable > fixed_topology_bytes ? usable - fixed_topology_bytes : 0u;

            if (per_tile_total > 0u && usable_for_batches >= per_tile_total) {
                const size_t tiles_by_vram = usable_for_batches / per_tile_total;
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
                "gpu-uf: free VRAM %.1f GB, per-tile cost uf=%.1f KB + compact=%.1f KB, batch_cap=%u\n",
                static_cast<double>(free_mem) / (1024.0 * 1024.0 * 1024.0),
                static_cast<double>(per_tile_bitmap + per_tile_uf) / 1024.0,
                static_cast<double>(per_tile_compact) / 1024.0,
                joint_batch_size);
        }

        gm::BatchContext batch_ctx;
        CUDA_CHECK(gm::create_batch_context(joint_batch_size, sample_geom.side_exp, &batch_ctx));

        gm::GpuUfContext gpu_uf_ctx{};
        GpuUfCompactStaging gpu_uf_compact_staging;
        uint32_t effective_batch_capacity = batch_ctx.batch_capacity;
        std::vector<uint8_t> h_compact_status;
        std::vector<uint8_t> h_compact_output;
        CompactAccumulator compact_accumulator;
        if (cfg.gpu_uf && manifest.num_jobs > 0u) {
            CUDA_CHECK(gm::create_gpu_uf_context(
                effective_batch_capacity,
                sample_geom.total_points,
                &gpu_uf_ctx));

            const size_t compact_output_bytes =
                static_cast<size_t>(effective_batch_capacity) * gm::kMaxCompactTileBytes;
            const size_t compact_status_bytes =
                static_cast<size_t>(effective_batch_capacity) * sizeof(uint8_t);

            CUDA_CHECK(cudaMalloc(
                &gpu_uf_compact_staging.d_compact_output,
                compact_output_bytes));
            CUDA_CHECK(cudaMalloc(
                &gpu_uf_compact_staging.d_compact_status,
                compact_status_bytes));
            CUDA_CHECK(cudaMemset(
                gpu_uf_compact_staging.d_compact_status,
                0,
                compact_status_bytes));

            if (!campaign_topology.manifest_exposed_face_masks.empty()) {
                CUDA_CHECK(cudaMalloc(
                    &gpu_uf_compact_staging.d_exposed_face_masks,
                    campaign_topology.manifest_exposed_face_masks.size() * sizeof(uint8_t)));
                CUDA_CHECK(cudaMemcpy(
                    gpu_uf_compact_staging.d_exposed_face_masks,
                    campaign_topology.manifest_exposed_face_masks.data(),
                    campaign_topology.manifest_exposed_face_masks.size() * sizeof(uint8_t),
                    cudaMemcpyHostToDevice));
            }

            bind_gpu_uf_compact_buffers(
                gpu_uf_ctx,
                gpu_uf_compact_staging.d_exposed_face_masks,
                gpu_uf_compact_staging.d_compact_output,
                gpu_uf_compact_staging.d_compact_status);

            h_compact_status.resize(effective_batch_capacity, 0u);
            h_compact_output.resize(compact_output_bytes, 0u);
            compact_accumulator.tile_offsets.resize(static_cast<size_t>(manifest.num_jobs), 0u);
            compact_accumulator.component_counts.resize(static_cast<size_t>(manifest.num_jobs), 0u);
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
                CUDA_CHECK(cudaMemset(
                    gpu_uf_compact_staging.d_compact_status,
                    0,
                    static_cast<size_t>(batch_count) * sizeof(uint8_t)));

                CUDA_CHECK(run_gpu_uf_dispatch(
                    0,
                    gpu_uf_ctx,
                    batch_ctx.d_bitmaps,
                    batch_ctx.d_jobs,
                    batch_start,
                    gpu_uf_compact_staging.d_exposed_face_masks == nullptr
                        ? nullptr
                        : gpu_uf_compact_staging.d_exposed_face_masks + batch_start,
                    batch_count,
                    manifest.tile_side,
                    collar,
                    manifest.k_sq,
                    sample_geom.side_exp,
                    batch_ctx.bitmap_words));

                CUDA_CHECK(cudaMemcpy(
                    h_compact_status.data(),
                    gpu_uf_compact_staging.d_compact_status,
                    static_cast<size_t>(batch_count) * sizeof(uint8_t),
                    cudaMemcpyDeviceToHost));
                for (uint32_t i = 0; i < batch_count; ++i) {
                    if (h_compact_status[i] != 0u) {
                        fail(
                            kExitCudaFailure,
                            "compact overflow on tile " +
                                std::to_string(static_cast<uint64_t>(batch_start) + i));
                    }
                }

                CUDA_CHECK(cudaMemcpy(
                    h_compact_output.data(),
                    gpu_uf_compact_staging.d_compact_output,
                    static_cast<size_t>(batch_count) * gm::kMaxCompactTileBytes,
                    cudaMemcpyDeviceToHost));
                append_compact_batch(
                    compact_accumulator,
                    batch_start,
                    batch_count,
                    h_compact_output);
                for (uint32_t i = 0; i < batch_count; ++i) {
                    compact_accumulator.total_primes +=
                        static_cast<uint64_t>(gpu_uf_ctx.h_num_primes[i]);
                }

                if (!cfg.gpu_boundary_merge) {
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

                for (uint32_t i = 0; i < batch_count; ++i) {
                    write_tile_result(
                        output.file,
                        jobs[static_cast<size_t>(batch_start) + static_cast<size_t>(i)],
                        manifest.tile_side,
                        batch_results[static_cast<size_t>(i)]);
                }
            }
        }

        if (cfg.gpu_uf) {
            finalize_compact_accumulator(compact_accumulator);
        }

        // Phase 1 (sieve + UF) is complete. Free phase-1 GPU staging before
        // entering phase 2 (merge) so their VRAM is available to the seam kernel.
        if (cfg.compact_merge) {
            bind_gpu_uf_compact_buffers(gpu_uf_ctx, nullptr, nullptr, nullptr);
            gm::destroy_gpu_uf_context(&gpu_uf_ctx);
            gm::destroy_batch_context(&batch_ctx);
            // gpu_uf_compact_staging RAII destructor fires here at scope exit,
            // but we want it freed now — explicitly release via a swap.
            {
                GpuUfCompactStaging tmp;
                std::swap(tmp, gpu_uf_compact_staging);
            } // tmp destructor frees d_exposed_face_masks, d_compact_output, d_compact_status
        }

        if (cfg.gpu_boundary_merge) {
            CampaignSummary summary{};
            summary = run_compact_merge(
                compact_accumulator,
                campaign_topology.topo,
                manifest.k_sq);
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

        if (cfg.gpu_uf && !cfg.compact_merge) {
            // Already freed above for compact_merge path.
            bind_gpu_uf_compact_buffers(gpu_uf_ctx, nullptr, nullptr, nullptr);
            gm::destroy_gpu_uf_context(&gpu_uf_ctx);
        }
        if (!cfg.compact_merge) {
            gm::destroy_batch_context(&batch_ctx);
        }
        return kExitSuccess;
    } catch (const AppError& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return error.code();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return kExitCudaFailure;
    }
}
