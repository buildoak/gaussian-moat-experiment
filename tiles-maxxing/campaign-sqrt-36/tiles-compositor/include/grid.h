#pragma once

#include "types.h"

#include <cstdint>

void compute_grid(int64_t R, Grid& grid);
void compute_grid_from_coords(int64_t R, const int64_t* coords, uint32_t n_tiles, Grid& grid);

inline uint64_t tile_index(const Grid& grid, int32_t j, int32_t r) {
    return grid.tower_offset[static_cast<std::size_t>(j)] + static_cast<uint64_t>(r);
}

inline bool is_tile_dead(const Grid& grid, int32_t j, int32_t r) {
    // Tile (j, r) occupies y in [base_y[j] + r*S, base_y[j] + (r+1)*S].
    // Dead when top edge falls more than one tile below the y=x diagonal.
    // The extra row ensures the lowest live tile has a Face I partner,
    // closing diagonal connectivity gaps.
    return (grid.base_y[j] + static_cast<int64_t>(r + 1) * TILE_SIDE) <=
           (static_cast<int64_t>(j) * TILE_SIDE - TILE_SIDE);
}
