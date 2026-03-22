#include <cuda_runtime.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "row_sieve.cuh"

namespace {

#define CUDA_CHECK(call)                                                                  \
    do {                                                                                  \
        cudaError_t cuda_status__ = (call);                                               \
        if (cuda_status__ != cudaSuccess) {                                               \
            std::cerr << "CUDA error at " << __FILE__ << ":" << __LINE__ << ": "         \
                      << cudaGetErrorString(cuda_status__) << std::endl;                  \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

constexpr uint8_t FACE_INNER_BIT = 1u << 0;
constexpr uint8_t FACE_OUTER_BIT = 1u << 1;
constexpr uint8_t FACE_LEFT_BIT = 1u << 2;
constexpr uint8_t FACE_RIGHT_BIT = 1u << 3;

struct Config {
    uint64_t k_sq = 0;
    int64_t a_lo = 0;
    int64_t b_lo = 0;
    uint32_t side = 0;
    bool have_k_sq = false;
    bool have_a_lo = false;
    bool have_b_lo = false;
    bool have_side = false;
};

struct TileGeometry {
    uint64_t k_sq = 0;
    int64_t collar = 0;
    int64_t a_lo = 0;
    int64_t a_hi = 0;
    int64_t b_lo = 0;
    int64_t b_hi = 0;
    int64_t expanded_a_lo = 0;
    int64_t expanded_b_lo = 0;
    uint64_t nominal_extent = 0;
    uint64_t side_exp = 0;
    uint64_t total_points = 0;
};

struct TileResult {
    size_t io_count = 0;
    size_t il_count = 0;
    size_t ir_count = 0;
    size_t ol_count = 0;
    size_t or_count = 0;
    size_t lr_count = 0;
    std::vector<size_t> i_face_components;
    std::vector<size_t> o_face_components;
    std::vector<size_t> l_face_components;
    std::vector<size_t> r_face_components;
    size_t num_primes = 0;
};

struct Timings {
    double time_ms = 0.0;
    double gpu_primality_ms = 0.0;
    double gpu_cc_ms = 0.0;
    double cpu_cc_ms = 0.0;
};

void print_usage(const char* program) {
    std::cerr
        << "Usage: " << program
        << " --k-sq N --a-lo N --b-lo N --side N\n";
}

[[noreturn]] void fail(const std::string& message) {
    std::cerr << message << std::endl;
    std::exit(1);
}

uint64_t parse_u64(const char* text, const char* name) {
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        fail(std::string("invalid value for ") + name + ": " + text);
    }
    return static_cast<uint64_t>(value);
}

int64_t parse_i64(const char* text, const char* name) {
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        fail(std::string("invalid value for ") + name + ": " + text);
    }
    return static_cast<int64_t>(value);
}

Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--k-sq") == 0 && i + 1 < argc) {
            cfg.k_sq = parse_u64(argv[++i], "--k-sq");
            cfg.have_k_sq = true;
        } else if (std::strcmp(argv[i], "--a-lo") == 0 && i + 1 < argc) {
            cfg.a_lo = parse_i64(argv[++i], "--a-lo");
            cfg.have_a_lo = true;
        } else if (std::strcmp(argv[i], "--b-lo") == 0 && i + 1 < argc) {
            cfg.b_lo = parse_i64(argv[++i], "--b-lo");
            cfg.have_b_lo = true;
        } else if (std::strcmp(argv[i], "--side") == 0 && i + 1 < argc) {
            const uint64_t side = parse_u64(argv[++i], "--side");
            if (side > std::numeric_limits<uint32_t>::max()) {
                fail("--side exceeds uint32_t range");
            }
            cfg.side = static_cast<uint32_t>(side);
            cfg.have_side = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            print_usage(argv[0]);
            fail(std::string("unknown or incomplete argument: ") + argv[i]);
        }
    }

    if (!cfg.have_k_sq || !cfg.have_a_lo || !cfg.have_b_lo || !cfg.have_side) {
        print_usage(argv[0]);
        fail("missing required arguments");
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
           static_cast<unsigned __int128>(root - 1) * static_cast<unsigned __int128>(root - 1) >= value) {
        --root;
    }
    return root;
}

TileGeometry make_geometry(const Config& cfg) {
    TileGeometry geom;
    geom.k_sq = cfg.k_sq;
    geom.collar = static_cast<int64_t>(ceil_sqrt_u64(cfg.k_sq));
    geom.a_lo = cfg.a_lo;
    geom.a_hi = cfg.a_lo + static_cast<int64_t>(cfg.side);
    geom.b_lo = cfg.b_lo;
    geom.b_hi = cfg.b_lo + static_cast<int64_t>(cfg.side);
    geom.expanded_a_lo = geom.a_lo - geom.collar;
    geom.expanded_b_lo = geom.b_lo - geom.collar;
    geom.nominal_extent = static_cast<uint64_t>(cfg.side) + 1ULL;
    geom.side_exp = geom.nominal_extent + 2ULL * static_cast<uint64_t>(geom.collar);
    if (geom.side_exp == 0 || geom.side_exp > std::numeric_limits<uint64_t>::max() / geom.side_exp) {
        fail("expanded tile dimensions overflow");
    }
    geom.total_points = geom.side_exp * geom.side_exp;
    if (geom.total_points > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        fail("expanded tile exceeds uint32_t union-find address space");
    }
    return geom;
}

std::vector<std::pair<int32_t, int32_t>> precompute_backward_offsets(uint64_t k_sq) {
    const int64_t collar = static_cast<int64_t>(ceil_sqrt_u64(k_sq));
    std::vector<std::pair<int32_t, int32_t>> offsets;
    for (int64_t da = -collar; da <= 0; ++da) {
        for (int64_t db = -collar; db <= collar; ++db) {
            if (da > 0 || (da == 0 && db >= 0)) {
                continue;
            }
            const uint64_t dist_sq = static_cast<uint64_t>(da * da + db * db);
            if (dist_sq <= k_sq) {
                offsets.emplace_back(static_cast<int32_t>(da), static_cast<int32_t>(db));
            }
        }
    }
    return offsets;
}

inline bool bitmap_test(const std::vector<uint32_t>& bitmap, uint64_t idx) {
    return ((bitmap[idx >> 5] >> (idx & 31ULL)) & 1U) != 0U;
}

size_t count_bits(const std::vector<uint32_t>& bitmap) {
    size_t total = 0;
    for (uint32_t word : bitmap) {
        total += static_cast<size_t>(__builtin_popcount(word));
    }
    return total;
}

size_t uf_find(std::vector<uint32_t>& parent, size_t x) {
    while (static_cast<size_t>(parent[x]) != x) {
        const size_t next = static_cast<size_t>(parent[x]);
        parent[x] = parent[next];
        x = next;
    }
    return x;
}

void uf_union(std::vector<uint32_t>& parent, std::vector<uint8_t>& rank, size_t a, size_t b) {
    const size_t ra = uf_find(parent, a);
    const size_t rb = uf_find(parent, b);
    if (ra == rb) {
        return;
    }

    if (rank[ra] < rank[rb]) {
        parent[ra] = static_cast<uint32_t>(rb);
    } else if (rank[ra] > rank[rb]) {
        parent[rb] = static_cast<uint32_t>(ra);
    } else {
        parent[rb] = static_cast<uint32_t>(ra);
        ++rank[ra];
    }
}

size_t component_id_for_root(
    size_t root,
    std::unordered_map<size_t, size_t>& root_map,
    std::vector<uint8_t>& component_faces
) {
    const auto it = root_map.find(root);
    if (it != root_map.end()) {
        return it->second;
    }

    const size_t component = component_faces.size();
    root_map.emplace(root, component);
    component_faces.push_back(0);
    return component;
}

TileResult classify_components(const TileGeometry& geom, const std::vector<uint32_t>& bitmap) {
    std::vector<uint32_t> parent(geom.total_points);
    for (uint64_t i = 0; i < geom.total_points; ++i) {
        parent[static_cast<size_t>(i)] = static_cast<uint32_t>(i);
    }
    std::vector<uint8_t> rank(static_cast<size_t>(geom.total_points), 0);

    const auto offsets = precompute_backward_offsets(geom.k_sq);
    for (uint64_t row = 0; row < geom.side_exp; ++row) {
        for (uint64_t col = 0; col < geom.side_exp; ++col) {
            const uint64_t idx = row * geom.side_exp + col;
            if (!bitmap_test(bitmap, idx)) {
                continue;
            }

            for (const auto& offset : offsets) {
                const int64_t nr = static_cast<int64_t>(row) + offset.first;
                const int64_t nc = static_cast<int64_t>(col) + offset.second;
                if (nr < 0 || nc < 0 ||
                    nr >= static_cast<int64_t>(geom.side_exp) ||
                    nc >= static_cast<int64_t>(geom.side_exp)) {
                    continue;
                }

                const uint64_t nidx =
                    static_cast<uint64_t>(nr) * geom.side_exp + static_cast<uint64_t>(nc);
                if (bitmap_test(bitmap, nidx)) {
                    uf_union(parent, rank, static_cast<size_t>(idx), static_cast<size_t>(nidx));
                }
            }
        }
    }

    TileResult result;
    result.num_primes = count_bits(bitmap);

    std::vector<uint8_t> component_faces;
    std::unordered_map<size_t, size_t> root_map;
    for (uint64_t row = 0; row < geom.side_exp; ++row) {
        const int64_t a = geom.expanded_a_lo + static_cast<int64_t>(row);
        for (uint64_t col = 0; col < geom.side_exp; ++col) {
            const uint64_t idx = row * geom.side_exp + col;
            if (!bitmap_test(bitmap, idx)) {
                continue;
            }

            const int64_t b = geom.expanded_b_lo + static_cast<int64_t>(col);
            if (a < geom.a_lo || a > geom.a_hi || b < geom.b_lo || b > geom.b_hi) {
                continue;
            }

            const size_t root = uf_find(parent, static_cast<size_t>(idx));
            const size_t component = component_id_for_root(root, root_map, component_faces);

            if (a - geom.a_lo <= geom.collar) {
                component_faces[component] |= FACE_INNER_BIT;
            }
            if (geom.a_hi - a <= geom.collar) {
                component_faces[component] |= FACE_OUTER_BIT;
            }
            if (b - geom.b_lo <= geom.collar) {
                component_faces[component] |= FACE_LEFT_BIT;
            }
            if (geom.b_hi - b <= geom.collar) {
                component_faces[component] |= FACE_RIGHT_BIT;
            }
        }
    }

    for (size_t id = 0; id < component_faces.size(); ++id) {
        const uint8_t faces = component_faces[id];
        const bool has_i = (faces & FACE_INNER_BIT) != 0;
        const bool has_o = (faces & FACE_OUTER_BIT) != 0;
        const bool has_l = (faces & FACE_LEFT_BIT) != 0;
        const bool has_r = (faces & FACE_RIGHT_BIT) != 0;

        if (has_i && has_o) {
            ++result.io_count;
        }
        if (has_i && has_l) {
            ++result.il_count;
        }
        if (has_i && has_r) {
            ++result.ir_count;
        }
        if (has_o && has_l) {
            ++result.ol_count;
        }
        if (has_o && has_r) {
            ++result.or_count;
        }
        if (has_l && has_r) {
            ++result.lr_count;
        }

        if (has_i) {
            result.i_face_components.push_back(id);
        }
        if (has_o) {
            result.o_face_components.push_back(id);
        }
        if (has_l) {
            result.l_face_components.push_back(id);
        }
        if (has_r) {
            result.r_face_components.push_back(id);
        }
    }

    return result;
}

void print_json_array(const std::vector<size_t>& values) {
    std::cout << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            std::cout << ", ";
        }
        std::cout << values[i];
    }
    std::cout << "]";
}

void print_json(const TileResult& result, const Timings& timings) {
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "{\n";
    std::cout << "  \"io_count\": " << result.io_count << ",\n";
    std::cout << "  \"il_count\": " << result.il_count << ",\n";
    std::cout << "  \"ir_count\": " << result.ir_count << ",\n";
    std::cout << "  \"ol_count\": " << result.ol_count << ",\n";
    std::cout << "  \"or_count\": " << result.or_count << ",\n";
    std::cout << "  \"lr_count\": " << result.lr_count << ",\n";
    std::cout << "  \"i_face_components\": ";
    print_json_array(result.i_face_components);
    std::cout << ",\n";
    std::cout << "  \"o_face_components\": ";
    print_json_array(result.o_face_components);
    std::cout << ",\n";
    std::cout << "  \"l_face_components\": ";
    print_json_array(result.l_face_components);
    std::cout << ",\n";
    std::cout << "  \"r_face_components\": ";
    print_json_array(result.r_face_components);
    std::cout << ",\n";
    std::cout << "  \"num_primes\": " << result.num_primes << ",\n";
    std::cout << "  \"time_ms\": " << timings.time_ms << ",\n";
    std::cout << "  \"gpu_primality_ms\": " << timings.gpu_primality_ms << ",\n";
    std::cout << "  \"gpu_cc_ms\": " << timings.gpu_cc_ms << ",\n";
    std::cout << "  \"cpu_cc_ms\": " << timings.cpu_cc_ms << "\n";
    std::cout << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    const auto wall_start = std::chrono::high_resolution_clock::now();
    const Config cfg = parse_args(argc, argv);
    const TileGeometry geom = make_geometry(cfg);

    int device_count = 0;
    CUDA_CHECK(cudaGetDeviceCount(&device_count));
    if (device_count == 0) {
        fail("no CUDA devices found");
    }

    const size_t bitmap_words = gm::bitmap_word_count(geom.total_points);
    const size_t bitmap_bytes = bitmap_words * sizeof(uint32_t);
    const size_t row_sieve_bytes = gm::row_sieve_shared_bytes(geom.side_exp);
    std::vector<uint32_t> bitmap(bitmap_words, 0);
    uint32_t* d_bitmap = nullptr;
    CUDA_CHECK(cudaMalloc(&d_bitmap, bitmap_bytes));

    cudaEvent_t gpu_start;
    cudaEvent_t gpu_stop;
    CUDA_CHECK(cudaEventCreate(&gpu_start));
    CUDA_CHECK(cudaEventCreate(&gpu_stop));
    CUDA_CHECK(gm::init_row_sieve_tables());
    CUDA_CHECK(gm::copy_tile_k_sq(cfg.k_sq));
    CUDA_CHECK(cudaEventRecord(gpu_start));
    CUDA_CHECK(cudaMemset(d_bitmap, 0, bitmap_bytes));

    const uint64_t blocks64 = geom.side_exp;
    if (blocks64 > static_cast<uint64_t>(std::numeric_limits<unsigned int>::max())) {
        fail("grid size exceeds CUDA launch limits");
    }

    gm::tile_sieved_primality_bitmap_kernel<<<
        static_cast<unsigned int>(blocks64),
        gm::kTileRowSieveBlockSize,
        row_sieve_bytes>>>(
        geom.a_lo,
        geom.b_lo,
        geom.collar,
        geom.side_exp,
        d_bitmap
    );
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaEventRecord(gpu_stop));
    CUDA_CHECK(cudaEventSynchronize(gpu_stop));

    float gpu_primality_ms = 0.0f;
    CUDA_CHECK(cudaEventElapsedTime(&gpu_primality_ms, gpu_start, gpu_stop));
    CUDA_CHECK(cudaMemcpy(bitmap.data(), d_bitmap, bitmap_bytes, cudaMemcpyDeviceToHost));

    const auto cc_start = std::chrono::high_resolution_clock::now();
    const TileResult result = classify_components(geom, bitmap);
    const auto cc_stop = std::chrono::high_resolution_clock::now();
    const auto wall_stop = std::chrono::high_resolution_clock::now();

    Timings timings;
    timings.gpu_primality_ms = static_cast<double>(gpu_primality_ms);
    timings.cpu_cc_ms =
        std::chrono::duration<double, std::milli>(cc_stop - cc_start).count();
    timings.time_ms =
        std::chrono::duration<double, std::milli>(wall_stop - wall_start).count();

    print_json(result, timings);

    CUDA_CHECK(cudaEventDestroy(gpu_start));
    CUDA_CHECK(cudaEventDestroy(gpu_stop));
    CUDA_CHECK(cudaFree(d_bitmap));
    return 0;
}
