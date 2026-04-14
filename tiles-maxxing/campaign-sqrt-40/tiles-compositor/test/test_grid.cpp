#ifdef NDEBUG
#undef NDEBUG
#endif

#include "grid.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int64_t isqrt_test(int64_t n) {
    if (n <= 0) {
        return 0;
    }

    int64_t x = static_cast<int64_t>(std::sqrt(static_cast<double>(n)));
    while (static_cast<__int128>(x) * static_cast<__int128>(x) > n) {
        --x;
    }
    while (static_cast<__int128>(x + 1) * static_cast<__int128>(x + 1) <= n) {
        ++x;
    }
    return x;
}

const Grid& large_grid() {
    static const Grid grid = [] {
        Grid value{};
        compute_grid(850000000LL, value);
        return value;
    }();
    return grid;
}

const Grid& small_grid() {
    static const Grid grid = [] {
        Grid value{};
        compute_grid(10000, value);
        return value;
    }();
    return grid;
}

void assert_monotonic_non_increasing(const Grid& grid) {
    for (int j = 1; j < grid.num_towers; ++j) {
        assert(grid.base_y[static_cast<std::size_t>(j - 1)] >=
               grid.base_y[static_cast<std::size_t>(j)]);
    }
}

void assert_delta_consistency(const Grid& grid) {
    assert(grid.delta.size() == grid.base_y.size() - 1);
    for (int j = 0; j < grid.num_towers - 1; ++j) {
        const int64_t expected = grid.base_y[static_cast<std::size_t>(j)] -
                                 grid.base_y[static_cast<std::size_t>(j + 1)];
        assert(grid.delta[static_cast<std::size_t>(j)] == expected);
        assert(grid.delta[static_cast<std::size_t>(j)] >= 0);
    }
}

void assert_variable_height_properties(const Grid& grid, bool check_gentle_ramp = false) {
    // All tiles_per_tower values in [32, 46]
    for (int j = 0; j < grid.num_towers; ++j) {
        const uint32_t h = grid.tiles_per_tower[static_cast<std::size_t>(j)];
        assert(h >= 32U);
        assert(h <= 46U);
    }

    // Gentle ramp: adjacent towers differ by at most 1 tile.
    // This is an asymptotic property for large R (spec S4.2a).
    if (check_gentle_ramp) {
        for (int j = 0; j < grid.num_towers - 1; ++j) {
            const int diff = static_cast<int>(grid.tiles_per_tower[static_cast<std::size_t>(j + 1)]) -
                             static_cast<int>(grid.tiles_per_tower[static_cast<std::size_t>(j)]);
            assert(diff >= -1 && diff <= 1);
        }
    }

    // tower_offset is prefix sum
    assert(grid.tower_offset.size() == static_cast<std::size_t>(grid.num_towers));
    uint64_t running = 0;
    for (int j = 0; j < grid.num_towers; ++j) {
        assert(grid.tower_offset[static_cast<std::size_t>(j)] == running);
        running += grid.tiles_per_tower[static_cast<std::size_t>(j)];
    }
    assert(grid.total_tiles == running);

    // tile_index uses tower_offset
    if (grid.num_towers > 0) {
        assert(tile_index(grid, 0, 0) == 0U);
        if (grid.num_towers > 1) {
            assert(tile_index(grid, 1, 0) == grid.tiles_per_tower[0]);
        }
    }
}

void test_compute_grid_large() {
    std::printf("  test_compute_grid_large... ");
    const Grid& grid = large_grid();

    assert(grid.R == 850000000LL);
    assert(grid.S == TILE_SIDE);
    // Variable tower heights: first tower should be 32 (near y-axis)
    assert(grid.tiles_per_tower[0] == 32U);
    assert(grid.num_towers >= 2340000);
    assert(grid.num_towers <= 2360000);
    assert(!grid.base_y.empty());
    assert(grid.base_y.front() <= grid.R);
    assert(grid.base_y.front() >= grid.R - TILE_SIDE);

    assert_monotonic_non_increasing(grid);
    assert_delta_consistency(grid);
    assert_variable_height_properties(grid, true);  // gentle ramp holds at large R

    // Tower near 45 degrees should have height close to 46
    const int mid = grid.num_towers / 2;
    const int last = grid.num_towers - 1;
    assert(grid.tiles_per_tower[static_cast<std::size_t>(mid)] > 32U);
    assert(grid.tiles_per_tower[static_cast<std::size_t>(last)] >= 44U);

    std::printf("base_y[0]=%lld num_towers=%d total_tiles=%llu tpt[0]=%u tpt[last]=%u OK\n",
                static_cast<long long>(grid.base_y.front()),
                grid.num_towers,
                static_cast<unsigned long long>(grid.total_tiles),
                grid.tiles_per_tower[0],
                grid.tiles_per_tower[static_cast<std::size_t>(last)]);
}

void test_compute_grid_small() {
    std::printf("  test_compute_grid_small... ");
    const Grid& grid = small_grid();

    assert(grid.R == 10000);
    assert(grid.num_towers >= 27);
    assert(grid.num_towers <= 45);
    assert(!grid.base_y.empty());
    assert(grid.base_y.front() <= 10000);
    assert(grid.base_y.front() >= 10000 - TILE_SIDE);

    assert_variable_height_properties(grid);

    std::printf("base_y[0]=%lld num_towers=%d total_tiles=%llu OK\n",
                static_cast<long long>(grid.base_y.front()),
                grid.num_towers,
                static_cast<unsigned long long>(grid.total_tiles));
}

void test_delta_consistency() {
    std::printf("  test_delta_consistency... ");
    assert_delta_consistency(large_grid());
    assert_delta_consistency(small_grid());
    std::printf("OK\n");
}

void test_dead_tile_predicate() {
    std::printf("  test_dead_tile_predicate... ");
    const Grid& grid = large_grid();
    const int last_tower = grid.num_towers - 1;
    const int last_tower_height = static_cast<int>(grid.tiles_per_tower[static_cast<std::size_t>(last_tower)]);

    int dead_rows_last_tower = 0;
    for (int r = 0; r < last_tower_height; ++r) {
        if (is_tile_dead(grid, last_tower, r)) {
            ++dead_rows_last_tower;
        }
    }

    // With G5 fix (compare against j*S not (j+1)*S), the last tower
    // which extends past the diagonal should have dead tiles
    assert(dead_rows_last_tower > 0);
    assert(is_tile_dead(grid, last_tower, last_tower_height - 1));
    assert(!is_tile_dead(grid, 0, 0));

    std::printf("dead_rows_last_tower=%d row0_last=%d OK\n",
                dead_rows_last_tower,
                is_tile_dead(grid, last_tower, 0) ? 1 : 0);
}

void test_compute_grid_from_coords() {
    std::printf("  test_compute_grid_from_coords... ");

    constexpr int64_t R = 10000;
    // Build coords with variable tower heights (like extraction mode)
    // Use 5 towers, each with height computed from isqrt base_y
    constexpr int kTowerCount = 5;
    std::vector<int64_t> coords;
    std::vector<int64_t> expected_base_y;
    std::vector<uint32_t> expected_tpt;

    for (int j = 0; j < kTowerCount; ++j) {
        const int64_t a_lo = static_cast<int64_t>(j) * TILE_SIDE;
        const int64_t b_lo = isqrt_test(R * R - a_lo * a_lo);
        expected_base_y.push_back(b_lo);
        // Compute expected tower height for this tower
        const double xj = static_cast<double>(a_lo);
        const double yj = static_cast<double>(b_lo);
        const double hyp = std::sqrt(xj * xj + yj * yj);
        uint32_t h = 32U;
        if (hyp >= 1.0 && yj > 0.0) {
            const double cos_theta = yj / hyp;
            h = static_cast<uint32_t>(std::ceil(32.0 / cos_theta));
            if (h < 32U) h = 32U;
            if (h > 46U) h = 46U;
        }
        expected_tpt.push_back(h);
        // In extraction mode, coords are (a_lo, b_lo) for each tile in the tower
        for (uint32_t r = 0; r < h; ++r) {
            coords.push_back(a_lo);
            coords.push_back(b_lo - static_cast<int64_t>(r) * TILE_SIDE);
        }
    }

    uint32_t total_tiles = 0;
    for (auto h : expected_tpt) total_tiles += h;

    Grid from_coords{};
    compute_grid_from_coords(R, coords.data(), total_tiles, from_coords);

    assert(from_coords.R == R);
    assert(from_coords.S == TILE_SIDE);
    assert(from_coords.num_towers == kTowerCount);
    for (int j = 0; j < kTowerCount; ++j) {
        assert(from_coords.base_y[static_cast<std::size_t>(j)] ==
               expected_base_y[static_cast<std::size_t>(j)]);
        assert(from_coords.tiles_per_tower[static_cast<std::size_t>(j)] ==
               expected_tpt[static_cast<std::size_t>(j)]);
    }
    assert_delta_consistency(from_coords);
    assert_variable_height_properties(from_coords);

    // Production grid uses threshold rounding, so base_y values should differ
    Grid production{};
    compute_grid(R, production);

    bool found_difference = false;
    const int limit = std::min(kTowerCount, production.num_towers);
    for (int j = 0; j < limit; ++j) {
        if (from_coords.base_y[static_cast<std::size_t>(j)] !=
            production.base_y[static_cast<std::size_t>(j)]) {
            found_difference = true;
            break;
        }
    }
    if (!found_difference) {
        std::printf("note=matched_sampled_towers ");
    }
    assert(found_difference);

    std::printf("difference_found=%d OK\n", found_difference ? 1 : 0);
}

void test_arc_deviation_bound() {
    std::printf("  test_arc_deviation_bound... ");
    // Verify threshold rounding produces base_y values within 0.5 of
    // the continuous arc at every tower.
    constexpr int64_t R = 10000;
    Grid grid{};
    compute_grid(R, grid);

    const __int128 R_sq = static_cast<__int128>(R) * R;
    double max_dev = 0.0;
    for (int j = 0; j < grid.num_towers; ++j) {
        const int64_t xj = static_cast<int64_t>(j) * TILE_SIDE;
        const double y_cont = std::sqrt(static_cast<double>(R_sq - static_cast<__int128>(xj) * xj));
        const double dev = std::abs(static_cast<double>(grid.base_y[static_cast<std::size_t>(j)]) - y_cont);
        if (dev > max_dev) max_dev = dev;
    }
    // Threshold rounding: |base_y[j] - y_cont[j]| <= 0.5 always
    assert(max_dev <= 0.5 + 1e-9);

    std::printf("max_dev=%.6f OK\n", max_dev);
}

void test_termination_past_diagonal() {
    std::printf("  test_termination_past_diagonal... ");
    const Grid& grid = large_grid();

    // The last tower should extend past y=x (G3)
    const int last = grid.num_towers - 1;
    const int64_t last_x = static_cast<int64_t>(last) * TILE_SIDE;
    const int64_t last_base_y = grid.base_y[static_cast<std::size_t>(last)];
    const uint32_t last_h = grid.tiles_per_tower[static_cast<std::size_t>(last)];
    const int64_t last_top = last_base_y + static_cast<int64_t>(last_h) * TILE_SIDE + 2 * TILE_SIDE;

    // This tower should satisfy the termination predicate (top + margin > x)
    assert(last_top > last_x);

    // The would-be next tower should fail the predicate
    // (or be beyond the arc entirely)
    std::printf("last_tower=%d last_x=%lld last_base_y=%lld last_top=%lld OK\n",
                last, static_cast<long long>(last_x),
                static_cast<long long>(last_base_y),
                static_cast<long long>(last_top));
}

}  // namespace

int main() {
    std::printf("=== test_grid ===\n");
    test_compute_grid_large();
    test_compute_grid_small();
    test_delta_consistency();
    test_dead_tile_predicate();
    test_compute_grid_from_coords();
    test_arc_deviation_bound();
    test_termination_past_diagonal();
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
