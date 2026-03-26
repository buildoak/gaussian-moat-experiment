#ifndef GM_FACE_EXTRACT_CUH
#define GM_FACE_EXTRACT_CUH

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "face_port_io.h"
#include "tile_kernel.cuh"
#include "types.h"

namespace gm {

constexpr uint8_t kFaceInnerBit = 1u << 0;
constexpr uint8_t kFaceOuterBit = 1u << 1;
constexpr uint8_t kFaceLeftBit = 1u << 2;
constexpr uint8_t kFaceRightBit = 1u << 3;

struct TileFacePorts {
    std::vector<FacePortRecord> face_inner;
    std::vector<FacePortRecord> face_outer;
    std::vector<FacePortRecord> face_left;
    std::vector<FacePortRecord> face_right;
    uint32_t num_components = 0;
    uint32_t num_primes = 0;
    int32_t origin_component = -1;
};

inline std::vector<std::pair<int32_t, int32_t>> precompute_backward_offsets(uint64_t k_sq) {
    const int64_t collar = static_cast<int64_t>(std::sqrt(static_cast<long double>(k_sq)));
    int64_t adjusted = collar;
    while (static_cast<unsigned __int128>(adjusted) * static_cast<unsigned __int128>(adjusted) < k_sq) {
        ++adjusted;
    }
    while (adjusted > 0 &&
           static_cast<unsigned __int128>(adjusted - 1) *
                   static_cast<unsigned __int128>(adjusted - 1) >=
               k_sq) {
        --adjusted;
    }

    std::vector<std::pair<int32_t, int32_t>> offsets;
    for (int64_t da = -adjusted; da <= 0; ++da) {
        for (int64_t db = -adjusted; db <= adjusted; ++db) {
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

inline bool bitmap_test(const uint32_t* bitmap, uint64_t idx) {
    return ((bitmap[idx >> 5] >> (idx & 31ULL)) & 1U) != 0U;
}

inline uint32_t count_bits(const uint32_t* bitmap, size_t word_count) {
    uint32_t total = 0;
    for (size_t i = 0; i < word_count; ++i) {
        total += static_cast<uint32_t>(__builtin_popcount(bitmap[i]));
    }
    return total;
}

inline size_t uf_find(std::vector<uint32_t>& parent, size_t x) {
    while (static_cast<size_t>(parent[x]) != x) {
        const size_t next = static_cast<size_t>(parent[x]);
        parent[x] = parent[next];
        x = next;
    }
    return x;
}

inline void uf_union(std::vector<uint32_t>& parent, std::vector<uint8_t>& rank, size_t a, size_t b) {
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

inline uint32_t component_id_for_root(
    size_t root,
    std::unordered_map<size_t, uint32_t>& root_map,
    std::vector<uint8_t>& component_faces
) {
    const auto it = root_map.find(root);
    if (it != root_map.end()) {
        return it->second;
    }

    const uint32_t component = static_cast<uint32_t>(component_faces.size());
    root_map.emplace(root, component);
    component_faces.push_back(0);
    return component;
}

inline TileFacePorts extract_face_ports(const TileGeometry& geom, const uint32_t* bitmap, uint64_t k_sq) {
    std::vector<uint32_t> parent(static_cast<size_t>(geom.total_points));
    for (uint64_t i = 0; i < geom.total_points; ++i) {
        parent[static_cast<size_t>(i)] = static_cast<uint32_t>(i);
    }
    std::vector<uint8_t> rank(static_cast<size_t>(geom.total_points), 0);

    const auto offsets = precompute_backward_offsets(k_sq);
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

    TileFacePorts result;
    result.num_primes = count_bits(bitmap, gm::bitmap_word_count(geom.total_points));

    std::vector<uint8_t> component_faces;
    std::unordered_map<size_t, uint32_t> root_map;
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
            const uint32_t component = component_id_for_root(root, root_map, component_faces);
            const FacePortRecord record{
                static_cast<int32_t>(a),
                static_cast<int32_t>(b),
                component,
            };

            if (result.origin_component < 0 && gaussian_norm_u64(a, b) <= k_sq) {
                result.origin_component = static_cast<int32_t>(component);
            }

            if (a - geom.a_lo <= geom.collar) {
                component_faces[component] |= kFaceInnerBit;
                result.face_inner.push_back(record);
            }
            if (geom.a_hi - a <= geom.collar) {
                component_faces[component] |= kFaceOuterBit;
                result.face_outer.push_back(record);
            }
            if (b - geom.b_lo <= geom.collar) {
                component_faces[component] |= kFaceLeftBit;
                result.face_left.push_back(record);
            }
            if (geom.b_hi - b <= geom.collar) {
                component_faces[component] |= kFaceRightBit;
                result.face_right.push_back(record);
            }
        }
    }

    result.num_components = static_cast<uint32_t>(component_faces.size());
    return result;
}

} // namespace gm

#endif // GM_FACE_EXTRACT_CUH
