#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace gm {

enum : uint8_t {
    FACE_INNER = 1u << 0,
    FACE_OUTER = 1u << 1,
    FACE_LEFT  = 1u << 2,
    FACE_RIGHT = 1u << 3,
};

struct TileGridInfo {
    uint32_t tiles_r = 0;      // number of tiles in r-direction
    uint32_t tiles_b = 0;      // number of tiles in b-direction
    uint32_t total_tiles = 0;  // tiles_r * tiles_b
    int64_t r_min = 0;         // campaign r-min
    int64_t r_max = 0;         // campaign r-max
    int64_t b_min = 0;         // campaign b-min (0 for on-axis, >0 for off-axis)
    int64_t b_max = 0;         // campaign b-max
    uint32_t tile_side = 0;    // tile side length
    uint32_t collar = 0;       // collar width (ceil(sqrt(k_sq)))
};

struct SeamPair {
    uint32_t tile_a = 0;  // first tile index
    uint32_t tile_b = 0;  // tile_a's right or next-row neighbor
    uint8_t axis = 0;     // 0 = horizontal seam, 1 = vertical seam
};

struct TileOrigin {
    int64_t a_lo = 0;  // r-coordinate of tile's lower-left corner
    int64_t b_lo = 0;  // b-coordinate of tile's lower-left corner
};

struct TopologyPrepass {
    TileGridInfo grid;
    std::vector<uint8_t> exposed_face_masks;
    std::vector<SeamPair> seam_pairs;
    std::vector<TileOrigin> tile_origins;
};

namespace topology_prepass_detail {

inline uint64_t ceil_div_u64(uint64_t numerator, uint32_t denominator) {
    if (denominator == 0u) {
        return 0u;
    }
    return numerator / denominator + static_cast<uint64_t>(numerator % denominator != 0u);
}

inline uint32_t tile_index(const TileGridInfo& grid, uint32_t r_tile, uint32_t b_tile) {
    return r_tile * grid.tiles_b + b_tile;
}

inline TileGridInfo make_grid_info(
    int64_t r_min, int64_t r_max, int64_t b_min, int64_t b_max, uint32_t tile_side, uint32_t k_sq) {
    TileGridInfo grid{};
    grid.r_min = r_min;
    grid.r_max = r_max;
    grid.b_min = b_min;
    grid.b_max = b_max;
    grid.tile_side = tile_side;
    grid.collar = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(k_sq))));

    if (tile_side == 0u || r_max <= r_min || b_max <= b_min) {
        return grid;
    }

    const uint64_t r_extent = static_cast<uint64_t>(r_max - r_min);
    const uint64_t b_extent = static_cast<uint64_t>(b_max - b_min);

    grid.tiles_r = static_cast<uint32_t>(ceil_div_u64(r_extent, tile_side));
    grid.tiles_b = static_cast<uint32_t>(ceil_div_u64(b_extent, tile_side));
    grid.total_tiles = grid.tiles_r * grid.tiles_b;
    return grid;
}

}  // namespace topology_prepass_detail

inline std::vector<uint8_t> compute_exposed_face_masks(const TileGridInfo& grid) {
    std::vector<uint8_t> exposed_face_masks(grid.total_tiles, 0u);

    for (uint32_t r_tile = 0; r_tile < grid.tiles_r; ++r_tile) {
        for (uint32_t b_tile = 0; b_tile < grid.tiles_b; ++b_tile) {
            uint8_t mask = 0u;
            if (r_tile == 0u) {
                mask |= FACE_INNER;
            }
            if (r_tile + 1u == grid.tiles_r) {
                mask |= FACE_OUTER;
            }
            if (b_tile == 0u) {
                mask |= FACE_LEFT;
            }
            if (b_tile + 1u == grid.tiles_b) {
                mask |= FACE_RIGHT;
            }

            exposed_face_masks[topology_prepass_detail::tile_index(grid, r_tile, b_tile)] = mask;
        }
    }

    return exposed_face_masks;
}

inline std::vector<SeamPair> enumerate_seam_pairs(const TileGridInfo& grid) {
    std::vector<SeamPair> seam_pairs;
    if (grid.total_tiles == 0u) {
        return seam_pairs;
    }

    const uint64_t horizontal_count =
        static_cast<uint64_t>(grid.tiles_r) * static_cast<uint64_t>(grid.tiles_b > 0u ? grid.tiles_b - 1u : 0u);
    const uint64_t vertical_count =
        static_cast<uint64_t>(grid.tiles_b) * static_cast<uint64_t>(grid.tiles_r > 0u ? grid.tiles_r - 1u : 0u);
    seam_pairs.reserve(static_cast<size_t>(horizontal_count + vertical_count));

    for (uint32_t r_tile = 0; r_tile < grid.tiles_r; ++r_tile) {
        for (uint32_t b_tile = 0; b_tile + 1u < grid.tiles_b; ++b_tile) {
            const uint32_t tile_a = topology_prepass_detail::tile_index(grid, r_tile, b_tile);
            const uint32_t tile_b = topology_prepass_detail::tile_index(grid, r_tile, b_tile + 1u);
            seam_pairs.push_back(SeamPair{tile_a, tile_b, 0u});
        }
    }

    for (uint32_t r_tile = 0; r_tile + 1u < grid.tiles_r; ++r_tile) {
        for (uint32_t b_tile = 0; b_tile < grid.tiles_b; ++b_tile) {
            const uint32_t tile_a = topology_prepass_detail::tile_index(grid, r_tile, b_tile);
            const uint32_t tile_b = topology_prepass_detail::tile_index(grid, r_tile + 1u, b_tile);
            seam_pairs.push_back(SeamPair{tile_a, tile_b, 1u});
        }
    }

    return seam_pairs;
}

inline std::vector<TileOrigin> compute_tile_origins(const TileGridInfo& grid) {
    std::vector<TileOrigin> tile_origins;
    tile_origins.reserve(grid.total_tiles);

    const int64_t tile_side = static_cast<int64_t>(grid.tile_side);
    for (uint32_t r_tile = 0; r_tile < grid.tiles_r; ++r_tile) {
        const int64_t a_lo = grid.r_min + static_cast<int64_t>(r_tile) * tile_side;
        for (uint32_t b_tile = 0; b_tile < grid.tiles_b; ++b_tile) {
            const int64_t b_lo = grid.b_min + static_cast<int64_t>(b_tile) * tile_side;
            tile_origins.push_back(TileOrigin{a_lo, b_lo});
        }
    }

    return tile_origins;
}

inline TopologyPrepass compute_topology(
    int64_t r_min, int64_t r_max, int64_t b_min, int64_t b_max, uint32_t tile_side, uint32_t k_sq) {
    TopologyPrepass topo{};
    topo.grid = topology_prepass_detail::make_grid_info(r_min, r_max, b_min, b_max, tile_side, k_sq);
    topo.exposed_face_masks = compute_exposed_face_masks(topo.grid);
    topo.seam_pairs = enumerate_seam_pairs(topo.grid);
    topo.tile_origins = compute_tile_origins(topo.grid);
    return topo;
}

}  // namespace gm
