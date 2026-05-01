#include "grid.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <numeric>

namespace {

[[maybe_unused]] int64_t integer_sqrt(__int128 value) {
    assert(value >= 0);
    if (value == 0) {
        return 0;
    }

    __int128 x = value;
    __int128 y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + value / x) / 2;
    }

    return static_cast<int64_t>(x);
}

// Compute per-tower tile count from base_y and tower index.
// tiles_per_tower[j] = ceil(32.0 / cos_theta_j), clamped to [32, 46].
uint32_t compute_tower_height(int64_t base_y_j, int64_t j, int S) {
    const double xj = static_cast<double>(j) * static_cast<double>(S);
    const double yj = static_cast<double>(base_y_j);
    const double hyp = std::sqrt(xj * xj + yj * yj);
    if (hyp < 1.0) {
        return 32U;  // degenerate case near origin
    }
    const double cos_theta = yj / hyp;
    if (cos_theta <= 0.0) {
        return 46U;
    }
    const double raw = 32.0 / cos_theta;
    auto h = static_cast<uint32_t>(std::ceil(raw));
    if (h < 32U) h = 32U;
    if (h > 46U) h = 46U;
    return h;
}

void finalize_grid_metadata(Grid& grid) {
    grid.num_towers = static_cast<int>(grid.base_y.size());

    // Delta table: delta[j] = base_y[j] - base_y[j+1]
    grid.delta.clear();
    if (grid.num_towers > 1) {
        grid.delta.reserve(static_cast<std::size_t>(grid.num_towers - 1));
        for (int j = 0; j < grid.num_towers - 1; ++j) {
            const int64_t d = grid.base_y[j] - grid.base_y[j + 1];
            assert(d >= 0);
            grid.delta.push_back(d);
        }
    }

    // Prefix sums for flat tile indexing
    grid.tower_offset.resize(static_cast<std::size_t>(grid.num_towers));
    uint64_t running = 0;
    for (int j = 0; j < grid.num_towers; ++j) {
        grid.tower_offset[static_cast<std::size_t>(j)] = running;
        running += grid.tiles_per_tower[static_cast<std::size_t>(j)];
    }
    grid.total_tiles = running;
}

}  // namespace

void compute_grid(int64_t R, Grid& grid) {
    grid.R = R;
    grid.S = TILE_SIDE;
    grid.W = TILES_PER_TOWER;  // kept for backward compat — min tiles per tower
    grid.base_y.clear();
    grid.delta.clear();
    grid.tiles_per_tower.clear();
    grid.tower_offset.clear();
    grid.total_tiles = 0;

    const __int128 R_sq = static_cast<__int128>(R) * static_cast<__int128>(R);
    const int64_t S = static_cast<int64_t>(TILE_SIDE);
    const int64_t MARGIN = 2 * S;

    // Threshold rounding: round y_cont to nearest integer, clamp to prev_y
    // for monotonicity. No accumulated error state.
    // |base_y[j] - y_cont[j]| <= 0.5 by construction (round gives <=0.5,
    // min only reduces the value further toward the arc).
    int64_t prev_y = 0;  // set from first tower

    for (int64_t j = 0;; ++j) {
        const int64_t x_j = j * S;
        const __int128 x_sq = static_cast<__int128>(x_j) * static_cast<__int128>(x_j);
        if (x_sq > R_sq) {
            break;  // tower base is beyond the y-axis intercept
        }

        // Continuous y on inner arc
        const __int128 diff = R_sq - x_sq;
        const double y_cont = std::sqrt(static_cast<double>(diff));

        // Threshold rounding with monotonicity clamp
        int64_t y_j = llround(y_cont);
        if (j > 0 && y_j > prev_y) {
            y_j = prev_y;  // enforce non-increasing base_y
        }
        prev_y = y_j;

        // Per-tower height (G1)
        const uint32_t h = compute_tower_height(y_j, j, TILE_SIDE);

        // Termination predicate (G3):
        // Include tower j only if base_y[j] + tiles_per_tower[j] * S + MARGIN > j * S
        const int64_t top_edge = y_j + static_cast<int64_t>(h) * S + MARGIN;
        if (top_edge <= x_j) {
            break;  // this tower and all subsequent are fully sub-diagonal
        }

        grid.base_y.push_back(y_j);
        grid.tiles_per_tower.push_back(h);
    }

    finalize_grid_metadata(grid);
}

void compute_grid_from_coords(int64_t R, const int64_t* coords, uint32_t n_tiles, Grid& grid) {
    assert(coords != nullptr);
    if (coords == nullptr) {
        std::abort();
    }

    grid.R = R;
    grid.S = TILE_SIDE;
    grid.W = TILES_PER_TOWER;
    grid.base_y.clear();
    grid.delta.clear();
    grid.tiles_per_tower.clear();
    grid.tower_offset.clear();
    grid.total_tiles = 0;

    // In extraction mode, n_tiles is the total number of tiles across all towers.
    // The coords array has 2 entries per tile: (a_lo, b_lo).
    // We scan to find tower boundaries: a new tower starts when a_lo changes.
    // Within each tower, we extract base_y from the first tile (row 0) and
    // count the number of tiles.

    const int64_t S = static_cast<int64_t>(TILE_SIDE);
    uint32_t tile_idx = 0;

    while (tile_idx < n_tiles) {
        const std::size_t offset = static_cast<std::size_t>(2) * static_cast<std::size_t>(tile_idx);
        const int64_t a_lo = coords[offset];
        const int64_t b_lo = coords[offset + 1];

        // Determine tower index from a_lo
        const int64_t j = a_lo / S;
        assert(a_lo == j * S);
        if (a_lo != j * S) {
            std::abort();
        }

        // b_lo of the first tile in this tower is base_y
        grid.base_y.push_back(b_lo);

        // Count how many consecutive tiles belong to this tower
        uint32_t tower_tile_count = 1;
        while (tile_idx + tower_tile_count < n_tiles) {
            const std::size_t next_off = static_cast<std::size_t>(2) *
                                         static_cast<std::size_t>(tile_idx + tower_tile_count);
            if (coords[next_off] != a_lo) {
                break;  // next tower starts
            }
            ++tower_tile_count;
        }

        grid.tiles_per_tower.push_back(tower_tile_count);
        tile_idx += tower_tile_count;
    }

    finalize_grid_metadata(grid);
}
