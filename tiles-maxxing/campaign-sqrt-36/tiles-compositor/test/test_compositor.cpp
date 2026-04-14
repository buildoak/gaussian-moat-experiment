#ifdef NDEBUG
#undef NDEBUG
#endif

#include "compositor.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct PortSpec {
    uint8_t group = 0;
    uint16_t h1 = 0;
};

using Tile128 = std::array<uint8_t, TILEOP_SIZE>;
using Tile256 = std::array<uint8_t, TILEOP_EXT_SIZE>;
using TowerTiles = std::array<Tile128, TILES_PER_TOWER>;
using TowerBuffer = std::array<uint8_t, TILES_PER_TOWER * TILEOP_SIZE>;

void make_tileop_with_budget(uint8_t* out,
                             int total_size,
                             int payload_budget,
                             const std::vector<uint8_t>& o_groups,
                             const std::vector<uint8_t>& i_groups,
                             const std::vector<PortSpec>& l_ports,
                             const std::vector<PortSpec>& r_ports) {
    assert(out != nullptr);
    assert(total_size == payload_budget + TILEOP_HEADER_BYTES);

    std::memset(out, 0, static_cast<std::size_t>(total_size));

    const int o_cnt = static_cast<int>(o_groups.size());
    const int i_cnt = static_cast<int>(i_groups.size());
    const int l_cnt = static_cast<int>(l_ports.size());
    const int r_cnt_formula = (payload_budget - o_cnt - i_cnt - (2 * l_cnt)) / 2;
    const int r_actual = static_cast<int>(r_ports.size());

    assert(payload_budget - o_cnt - i_cnt - (2 * l_cnt) >= 0);
    assert(r_cnt_formula >= 0);
    assert(r_actual <= r_cnt_formula);

    const uint8_t off_I = static_cast<uint8_t>(TILEOP_HEADER_BYTES + o_cnt);
    const uint8_t off_L = static_cast<uint8_t>(off_I + i_cnt);
    const uint8_t off_R = static_cast<uint8_t>(off_L + l_cnt);

    out[0] = off_I;
    out[1] = off_L;
    out[2] = off_R;

    for (int i = 0; i < o_cnt; ++i) {
        out[TILEOP_HEADER_BYTES + i] = o_groups[static_cast<std::size_t>(i)];
    }
    for (int i = 0; i < i_cnt; ++i) {
        out[off_I + i] = i_groups[static_cast<std::size_t>(i)];
    }
    for (int i = 0; i < l_cnt; ++i) {
        const PortSpec& port = l_ports[static_cast<std::size_t>(i)];
        out[off_L + i] = static_cast<uint8_t>((port.group & 0x7FU) |
                                              (((port.h1 >> 8) & 0x1U) << 7));
    }
    for (int i = 0; i < r_actual; ++i) {
        const PortSpec& port = r_ports[static_cast<std::size_t>(i)];
        out[off_R + i] = static_cast<uint8_t>((port.group & 0x7FU) |
                                              (((port.h1 >> 8) & 0x1U) << 7));
    }

    const int h_start = static_cast<int>(off_R) + r_cnt_formula;
    for (int i = 0; i < l_cnt; ++i) {
        out[h_start + i] = static_cast<uint8_t>(l_ports[static_cast<std::size_t>(i)].h1 & 0xFFU);
    }
    for (int i = 0; i < r_actual; ++i) {
        out[h_start + l_cnt + i] =
            static_cast<uint8_t>(r_ports[static_cast<std::size_t>(i)].h1 & 0xFFU);
    }
}

Tile128 make_tileop(const std::vector<uint8_t>& o_groups,
                    const std::vector<uint8_t>& i_groups,
                    const std::vector<PortSpec>& l_ports = {},
                    const std::vector<PortSpec>& r_ports = {}) {
    Tile128 tile{};
    make_tileop_with_budget(tile.data(), TILEOP_SIZE, TILEOP_PAYLOAD_BYTES,
                            o_groups, i_groups, l_ports, r_ports);
    return tile;
}

Tile256 make_extended_tileop(const std::vector<uint8_t>& o_groups,
                             const std::vector<uint8_t>& i_groups,
                             const std::vector<PortSpec>& l_ports = {},
                             const std::vector<PortSpec>& r_ports = {}) {
    Tile256 tile{};
    make_tileop_with_budget(tile.data(), TILEOP_EXT_SIZE, TILEOP_EXT_PAYLOAD_BYTES,
                            o_groups, i_groups, l_ports, r_ports);
    return tile;
}

Tile128 make_dead_tileop() {
    Tile128 tile{};
    std::memset(tile.data(), 0, tile.size());
    tile[0] = EMPTY_OFFSET;
    tile[1] = EMPTY_OFFSET;
    tile[2] = EMPTY_OFFSET;
    tile[3] = 0;
    return tile;
}

Tile128 make_overflow_tileop() {
    Tile128 tile{};
    std::memset(tile.data(), 0xFF, tile.size());
    return tile;
}

TowerBuffer make_tower(const TowerTiles& tileops) {
    TowerBuffer tower{};
    for (int row = 0; row < TILES_PER_TOWER; ++row) {
        std::memcpy(tower.data() + static_cast<std::size_t>(row) * TILEOP_SIZE,
                    tileops[static_cast<std::size_t>(row)].data(),
                    TILEOP_SIZE);
    }
    return tower;
}

Grid make_simple_grid(int num_towers, const std::vector<int64_t>& deltas = {}) {
    Grid grid{};
    grid.R = static_cast<int64_t>(num_towers + TILES_PER_TOWER + 64) * TILE_SIDE;
    grid.S = TILE_SIDE;
    grid.W = TILES_PER_TOWER;
    grid.num_towers = num_towers;
    grid.tiles_per_tower.assign(static_cast<std::size_t>(num_towers),
                                static_cast<uint32_t>(TILES_PER_TOWER));
    grid.tower_offset.resize(static_cast<std::size_t>(num_towers));
    for (int j = 0; j < num_towers; ++j) {
        grid.tower_offset[static_cast<std::size_t>(j)] =
            static_cast<uint64_t>(j) * static_cast<uint64_t>(TILES_PER_TOWER);
    }
    grid.total_tiles = static_cast<uint64_t>(num_towers) *
                       static_cast<uint64_t>(TILES_PER_TOWER);
    grid.base_y.resize(static_cast<std::size_t>(num_towers));
    grid.base_y[0] = grid.R;
    for (int j = 1; j < num_towers; ++j) {
        const int64_t d = (j - 1 < static_cast<int>(deltas.size()))
                              ? deltas[static_cast<std::size_t>(j - 1)]
                              : 0;
        grid.base_y[static_cast<std::size_t>(j)] =
            grid.base_y[static_cast<std::size_t>(j - 1)] - d;
    }
    grid.delta.resize(static_cast<std::size_t>(num_towers > 0 ? num_towers - 1 : 0));
    for (int j = 0; j < num_towers - 1; ++j) {
        grid.delta[static_cast<std::size_t>(j)] =
            grid.base_y[static_cast<std::size_t>(j)] -
            grid.base_y[static_cast<std::size_t>(j + 1)];
    }

    for (int j = 0; j < num_towers; ++j) {
        for (int row = 0; row < TILES_PER_TOWER; ++row) {
            assert(!is_tile_dead(grid, j, row));
        }
    }

    return grid;
}

CompositorResult run_case(const Grid& grid,
                          const std::vector<TowerBuffer>& towers,
                          const ExtendedTileSideTable* ext = nullptr) {
    assert(static_cast<int>(towers.size()) == grid.num_towers);

    Compositor compositor;
    compositor.init(grid);
    for (int j = 0; j < grid.num_towers; ++j) {
        compositor.ingest_tower(j, towers[static_cast<std::size_t>(j)].data(), ext);
    }

    const bool spanning_before_finalize = compositor.has_spanning();
    const CompositorResult result = compositor.finalize();
    assert(spanning_before_finalize ==
           (result.verdict == CompositorResult::SPANNING));
    return result;
}

TowerTiles filled_tower(uint8_t chain_group,
                        uint8_t row0_i_group,
                        uint8_t row31_o_group) {
    TowerTiles tiles{};
    for (int row = 0; row < TILES_PER_TOWER; ++row) {
        const uint8_t i_group = (row == 0) ? row0_i_group : chain_group;
        const uint8_t o_group = (row == TILES_PER_TOWER - 1) ? row31_o_group : chain_group;
        tiles[static_cast<std::size_t>(row)] = make_tileop({o_group}, {i_group});
    }
    return tiles;
}

void test_io_spanning() {
    std::printf("  test_io_spanning... ");

    TowerTiles tower = filled_tower(1, 1, 1);

    const Grid grid = make_simple_grid(1);
    const CompositorResult result = run_case(grid, {make_tower(tower)});

    assert(result.verdict == CompositorResult::SPANNING);
    assert(result.total_groups == 32U);
    assert(result.inner_root_count == 1U);
    assert(result.outer_root_count == 1U);
    std::printf("OK\n");
}

void test_io_moat() {
    std::printf("  test_io_moat... ");

    TowerTiles tower = filled_tower(2, 1, 4);

    const Grid grid = make_simple_grid(1);
    const CompositorResult result = run_case(grid, {make_tower(tower)});

    assert(result.verdict == CompositorResult::MOAT);
    assert(result.inner_root_count == 1U);
    assert(result.outer_root_count == 1U);
    std::printf("OK\n");
}

void test_lr_spanning_zero_delta() {
    std::printf("  test_lr_spanning_zero_delta... ");

    TowerTiles tower0 = filled_tower(1, 1, 1);
    TowerTiles tower1 = filled_tower(1, 1, 1);
    tower0[0] = make_tileop({1}, {1}, {}, {{1, 50}});
    tower1[0] = make_tileop({1}, {1}, {{1, 50}}, {});

    const Grid grid = make_simple_grid(2, {0});
    const CompositorResult result = run_case(grid, {make_tower(tower0), make_tower(tower1)});

    assert(result.verdict == CompositorResult::SPANNING);
    assert(result.total_groups == 64U);
    assert(result.inner_root_count == 1U);
    assert(result.outer_root_count == 1U);
    std::printf("OK\n");
}

void test_lr_spanning_nonzero_delta() {
    std::printf("  test_lr_spanning_nonzero_delta... ");

    TowerTiles tower0 = filled_tower(1, 1, 1);
    TowerTiles tower1 = filled_tower(1, 1, 1);
    tower0[0] = make_tileop({1}, {1}, {}, {{1, 60}});
    tower1[0] = make_tileop({1}, {1}, {{1, 70}}, {});

    const Grid grid = make_simple_grid(2, {10});
    const CompositorResult result = run_case(grid, {make_tower(tower0), make_tower(tower1)});

    assert(result.verdict == CompositorResult::SPANNING);
    assert(result.total_groups == 64U);
    assert(result.inner_root_count == 1U);
    assert(result.outer_root_count == 1U);
    std::printf("OK\n");
}

void test_lr_no_match_wrong_h1() {
    std::printf("  test_lr_no_match_wrong_h1... ");

    TowerTiles tower0 = filled_tower(1, 1, 4);
    TowerTiles tower1 = filled_tower(2, 5, 6);
    tower0[0] = make_tileop({1}, {1}, {}, {{1, 50}});
    tower1[0] = make_tileop({2}, {5}, {{2, 99}}, {});

    const Grid grid = make_simple_grid(2, {0});
    const CompositorResult result = run_case(grid, {make_tower(tower0), make_tower(tower1)});

    assert(result.verdict == CompositorResult::MOAT);
    assert(result.inner_root_count == 2U);
    assert(result.outer_root_count == 2U);
    std::printf("OK\n");
}

// ---------------------------------------------------------------------------
// Variable-height grid helper
// ---------------------------------------------------------------------------

// Like make_simple_grid but with per-tower heights.
// heights: vector of per-tower tile counts (each in [1..TILES_PER_TOWER_MAX]).
// deltas: inter-tower base_y deltas (size = num_towers - 1). Defaults to all 0.
Grid make_variable_grid(const std::vector<int>& heights,
                        const std::vector<int64_t>& deltas = {}) {
    const int num_towers = static_cast<int>(heights.size());
    Grid grid{};
    grid.R = static_cast<int64_t>(num_towers + TILES_PER_TOWER_MAX + 64) * TILE_SIDE;
    grid.S = TILE_SIDE;
    grid.W = *std::max_element(heights.begin(), heights.end());
    grid.num_towers = num_towers;

    grid.tiles_per_tower.resize(static_cast<std::size_t>(num_towers));
    grid.tower_offset.resize(static_cast<std::size_t>(num_towers));
    uint64_t offset = 0;
    for (int j = 0; j < num_towers; ++j) {
        grid.tiles_per_tower[static_cast<std::size_t>(j)] =
            static_cast<uint32_t>(heights[static_cast<std::size_t>(j)]);
        grid.tower_offset[static_cast<std::size_t>(j)] = offset;
        offset += static_cast<uint64_t>(heights[static_cast<std::size_t>(j)]);
    }
    grid.total_tiles = offset;

    grid.base_y.resize(static_cast<std::size_t>(num_towers));
    grid.base_y[0] = grid.R;
    for (int j = 1; j < num_towers; ++j) {
        const int64_t d = (j - 1 < static_cast<int>(deltas.size()))
                              ? deltas[static_cast<std::size_t>(j - 1)]
                              : 0;
        grid.base_y[static_cast<std::size_t>(j)] =
            grid.base_y[static_cast<std::size_t>(j - 1)] - d;
    }
    grid.delta.resize(static_cast<std::size_t>(num_towers > 0 ? num_towers - 1 : 0));
    for (int j = 0; j < num_towers - 1; ++j) {
        grid.delta[static_cast<std::size_t>(j)] =
            grid.base_y[static_cast<std::size_t>(j)] -
            grid.base_y[static_cast<std::size_t>(j + 1)];
    }

    // Verify all tiles alive
    for (int j = 0; j < num_towers; ++j) {
        const int h = heights[static_cast<std::size_t>(j)];
        for (int row = 0; row < h; ++row) {
            assert(!is_tile_dead(grid, j, row));
        }
    }

    return grid;
}

// Build a tower buffer for a variable-height tower. Only the first `height`
// tiles are meaningful; returns a vector of raw bytes.
std::vector<uint8_t> make_var_tower(const std::vector<Tile128>& tiles, int height) {
    assert(height <= TILES_PER_TOWER_MAX);
    assert(static_cast<int>(tiles.size()) >= height);
    std::vector<uint8_t> buf(static_cast<std::size_t>(height) * TILEOP_SIZE, 0);
    for (int row = 0; row < height; ++row) {
        std::memcpy(buf.data() + static_cast<std::size_t>(row) * TILEOP_SIZE,
                    tiles[static_cast<std::size_t>(row)].data(),
                    TILEOP_SIZE);
    }
    return buf;
}

// Run a variable-height case. tower_bufs are raw byte vectors per tower.
CompositorResult run_variable_case(const Grid& grid,
                                   const std::vector<std::vector<uint8_t>>& tower_bufs,
                                   const ExtendedTileSideTable* ext = nullptr) {
    assert(static_cast<int>(tower_bufs.size()) == grid.num_towers);

    Compositor compositor;
    compositor.init(grid);
    for (int j = 0; j < grid.num_towers; ++j) {
        compositor.ingest_tower(j, tower_bufs[static_cast<std::size_t>(j)].data(), ext);
    }

    const bool spanning_before = compositor.has_spanning();
    const CompositorResult result = compositor.finalize();
    assert(spanning_before == (result.verdict == CompositorResult::SPANNING));
    return result;
}

// Build a filled variable-height tower: all tiles chained via group 1 on I/O.
// row 0 has I-face group = row0_i, top row has O-face group = top_o.
// Optionally add L/R ports to specific rows.
std::vector<Tile128> filled_var_tower(int height,
                                      uint8_t chain_group,
                                      uint8_t row0_i_group,
                                      uint8_t top_o_group) {
    std::vector<Tile128> tiles(static_cast<std::size_t>(height));
    for (int row = 0; row < height; ++row) {
        const uint8_t i_group = (row == 0) ? row0_i_group : chain_group;
        const uint8_t o_group = (row == height - 1) ? top_o_group : chain_group;
        tiles[static_cast<std::size_t>(row)] = make_tileop({o_group}, {i_group});
    }
    return tiles;
}

// ---------------------------------------------------------------------------
// Gate 2 — Variable-height L/R matching
// ---------------------------------------------------------------------------

void test_lr_variable_height_shared_rows() {
    std::printf("  test_lr_variable_height_shared_rows... ");

    // 3 towers: heights [34, 38, 34]
    // All tiles chain vertically via group 1.
    // L/R matching at row 0 connects adjacent towers → spanning.
    // Rows 34-37 of tower 1 have L/R ports that would create a false
    // spanning if they were incorrectly matched against a non-existent neighbor.
    const int H0 = 34, H1 = 38, H2 = 34;

    auto tower0_tiles = filled_var_tower(H0, 1, 1, 1);
    auto tower1_tiles = filled_var_tower(H1, 1, 1, 1);
    auto tower2_tiles = filled_var_tower(H2, 1, 1, 1);

    // Connect tower 0 ↔ tower 1 at row 0 via L/R with matching h1=50
    tower0_tiles[0] = make_tileop({1}, {1}, {}, {{1, 50}});
    tower1_tiles[0] = make_tileop({1}, {1}, {{1, 50}}, {{1, 50}});
    // Connect tower 1 ↔ tower 2 at row 0
    tower2_tiles[0] = make_tileop({1}, {1}, {{1, 50}}, {});

    // Rows 34-37 of tower 1: add L/R ports with group 2 that have no partner
    // These would create a false spanning if matched incorrectly.
    // Group 2 is a separate group not connected to inner/outer in tower 1.
    for (int row = H0; row < H1; ++row) {
        tower1_tiles[static_cast<std::size_t>(row)] =
            make_tileop({1}, {1}, {{2, 100}}, {{2, 100}});
    }

    const Grid grid = make_variable_grid({H0, H1, H2}, {0, 0});
    const auto buf0 = make_var_tower(tower0_tiles, H0);
    const auto buf1 = make_var_tower(tower1_tiles, H1);
    const auto buf2 = make_var_tower(tower2_tiles, H2);

    const CompositorResult result = run_variable_case(grid, {buf0, buf1, buf2});

    assert(result.verdict == CompositorResult::SPANNING);
    std::printf("OK\n");
}

void test_lr_variable_height_no_false_match() {
    std::printf("  test_lr_variable_height_no_false_match... ");

    // 3 towers: heights [32, 40, 32]
    // Rows 0-31: group 1 chains vertically, but inner group (group 1) and outer
    // group (group 2) are separate — no spanning through shared rows.
    // Rows 32-39 of tower 1: group 3 on all faces including L/R, AND group 3
    // is connected to both inner and outer within tower 1 — but it has no L/R
    // neighbor since towers 0 and 2 are only 32 rows tall. So: MOAT.
    const int H0 = 32, H1 = 40, H2 = 32;

    // Tower 0: group 1 on I (inner), group 2 on O (outer), group 3 chains mid
    auto tower0_tiles = filled_var_tower(H0, 3, 1, 2);
    // Tower 1: same disconnect — group 1 inner, group 2 outer, group 3 chain
    auto tower1_tiles = filled_var_tower(H1, 3, 1, 2);
    // Tower 2: same
    auto tower2_tiles = filled_var_tower(H2, 3, 1, 2);

    // No L/R connections in shared rows — towers are isolated radial columns.
    // Each tower independently has inner (group 1) disconnected from outer (group 2).

    // Rows 32-39 of tower 1: use group 4 which WOULD span if matched.
    // Give it both inner-touching and outer-touching properties within tower 1.
    // But since there's no L/R neighbor at these rows, it cannot help.
    // We make group 4 appear on L and R faces.
    for (int row = H0; row < H1; ++row) {
        tower1_tiles[static_cast<std::size_t>(row)] =
            make_tileop({3}, {3}, {{4, 80}}, {{4, 80}});
    }

    const Grid grid = make_variable_grid({H0, H1, H2}, {0, 0});
    const auto buf0 = make_var_tower(tower0_tiles, H0);
    const auto buf1 = make_var_tower(tower1_tiles, H1);
    const auto buf2 = make_var_tower(tower2_tiles, H2);

    const CompositorResult result = run_variable_case(grid, {buf0, buf1, buf2});

    assert(result.verdict == CompositorResult::MOAT);
    std::printf("OK\n");
}

// ---------------------------------------------------------------------------
// Gate 3 — Bump tower outer boundary (staircase risers)
// ---------------------------------------------------------------------------

void test_bump_tower_outer_boundary() {
    std::printf("  test_bump_tower_outer_boundary... ");

    // 3 towers: heights [32, 40, 32]
    // The middle tower is taller. Its rows 32-39 have L-faces and R-faces
    // exposed as outer boundary (staircase risers from C4).
    //
    // Construction:
    // - All tiles use group 1 on all faces → everything in each tower is connected.
    // - Tower 0 and tower 2: 32 rows, group 1 everywhere → inner connected to
    //   chain but NOT to outer (O-face of top row is outer, I-face of row 0 is inner).
    //   Actually: group 1 chains everything, so inner IS connected to outer
    //   within each tower.
    // - L/R matching at shared rows (0-31) connects all three towers.
    // - The question: is the path from inner to outer actually spanning?
    //   With group 1 everywhere: inner (I-face row 0) → chain → outer (O-face top).
    //   Plus L/R connects towers. So spanning is trivially true.
    //
    // Better construction: make spanning REQUIRE the staircase risers.
    // - Tower 0 (H=32): group 1 on I-face row 0 (inner boundary).
    //   Group 1 chains vertically. Group 1 on R-face row 0 with h1=50.
    //   O-face top row: group 2 (different — NOT outer-connected to group 1).
    // - Tower 1 (H=40): group 1 chains rows 0-39.
    //   L-face row 0 matches tower 0 R-face (h1=50). → group 1 connected across.
    //   O-face row 39 (top): group 1 → outer boundary.
    //   But we want spanning to require the staircase riser, not the O-face tread.
    //
    // Revised: make the O-face of tower 1's top row a DIFFERENT group (group 2).
    // Make the L-face staircase risers (rows 32-39) use group 1.
    // Then the path is: inner (tower 0 row 0 I-face, group 1) → chain up tower 0
    // → L/R to tower 1 → chain up tower 1 → at rows 32-39 L-face exposed as
    // outer boundary (group 1) → SPANNING.
    // Without C4: those L-face risers would NOT be collected → MOAT.

    const int H0 = 32, H1 = 40, H2 = 32;

    // Tower 0: group 1 chains everything, I-face row0 = group 1 (inner),
    // O-face top = group 1, R-face row 0 with h1=50 to connect to tower 1.
    auto tower0_tiles = filled_var_tower(H0, 1, 1, 1);
    tower0_tiles[0] = make_tileop({1}, {1}, {}, {{1, 50}});

    // Tower 1: group 1 chains rows 0-39. L-face row 0 matches tower 0.
    // O-face of top row (row 39): group 2 (NOT connected to group 1).
    // The L-face of rows 32-39 has group 1 → exposed as outer staircase riser.
    auto tower1_tiles = filled_var_tower(H1, 1, 1, 2);
    // Row 0: L-face to match tower 0, R-face to match tower 2
    tower1_tiles[0] = make_tileop({1}, {1}, {{1, 50}}, {{1, 50}});
    // Rows 32-39: group 1 chains, but also has L-face group 1 (will be outer boundary)
    for (int row = H0; row < H1; ++row) {
        tower1_tiles[static_cast<std::size_t>(row)] =
            make_tileop({1}, {1}, {{1, 80}}, {{1, 80}});
    }

    // Tower 2: group 1 chains. L-face row 0 matches tower 1.
    // O-face top: group 2 (not connected to inner).
    auto tower2_tiles = filled_var_tower(H2, 1, 1, 2);
    tower2_tiles[0] = make_tileop({1}, {1}, {{1, 50}}, {});

    const Grid grid = make_variable_grid({H0, H1, H2}, {0, 0});
    const auto buf0 = make_var_tower(tower0_tiles, H0);
    const auto buf1 = make_var_tower(tower1_tiles, H1);
    const auto buf2 = make_var_tower(tower2_tiles, H2);

    const CompositorResult result = run_variable_case(grid, {buf0, buf1, buf2});

    // Spanning requires the C4 staircase risers: L-face of tower 1 rows 32-39
    // collected as outer boundary.
    assert(result.verdict == CompositorResult::SPANNING);
    std::printf("OK\n");
}

// ---------------------------------------------------------------------------
// Gate 4 — Incremental spanning (burst mode)
// ---------------------------------------------------------------------------

void test_incremental_spanning_early_exit() {
    std::printf("  test_incremental_spanning_early_exit... ");

    // 5 towers, uniform height 32, all deltas = 0.
    // Design: each tower alone does NOT span. Inner and outer are separate
    // groups within each tower. Spanning requires a cross-tower bridge.
    //
    // Tower 0: I-face row0 = group 1 (inner). Chain group 3. O-face top = group 2.
    //          R-face row 0: group 1, h1=50 → connects inner group across to tower 1.
    //          Inner touches group 1. Outer touches group 2. Group 1 != group 2 → no span.
    //
    // Tower 1: I-face row0 = group 4 (just a group, NOT inner — only tower 0 row 0
    //          contributes inner boundary since all towers have the same base_y with
    //          delta=0, so every tower's row 0 I-face IS inner boundary).
    //          Wait — every tower's I-face row 0 is inner boundary.
    //
    // Revised design: leverage the fact that inner boundary = I-face of row 0
    //   of EVERY tower. So I need to make inner groups disconnected from outer
    //   in each tower, and have the spanning path go:
    //   inner (tower 0 I-face) → group 1 → R/L to tower 1 → group 1 → R/L to tower 2
    //   → group 1 → somehow reach outer.
    //
    // Tower 2 is the one that bridges to outer: its group 1 (connected from
    //   tower 0 via L/R chain) is also on the O-face of its top row.
    //
    // But tower 0's O-face top is group 2 (not group 1), so tower 0 alone
    //   does NOT span. Tower 1's O-face top is group 2 — no span.
    //   Tower 2's O-face top is group 1 — once group 1 is connected across
    //   all three towers via L/R, inner connects to outer through tower 2.

    const int H = TILES_PER_TOWER;  // 32

    // Tower 0: inner group 1, chain group 1, O-face top = group 2.
    // Row 0 I-face=1, O-face=1. Top row O-face=2, I-face=1.
    // But if group 1 chains the whole tower, then the top row's I-face (group 1)
    // connects to group 1 throughout, but O-face (group 2) is separate.
    // Inner boundary has group 1. Outer boundary has group 2. No span.
    auto tower0_tiles = filled_var_tower(H, 1, 1, 2);  // chain=1, row0_i=1, top_o=2
    tower0_tiles[0] = make_tileop({1}, {1}, {}, {{1, 50}});

    // Tower 1: same structure — chain group 1, O-face top = group 2.
    auto tower1_tiles = filled_var_tower(H, 1, 1, 2);
    tower1_tiles[0] = make_tileop({1}, {1}, {{1, 50}}, {{1, 50}});

    // Tower 2: chain group 1, O-face top = group 1. This is the bridge.
    // Once tower 2's group 1 is connected to towers 0/1 via L/R, inner reaches outer.
    auto tower2_tiles = filled_var_tower(H, 1, 1, 1);  // top_o=1 → outer boundary
    tower2_tiles[0] = make_tileop({1}, {1}, {{1, 50}}, {{1, 50}});

    // Towers 3-4: isolated. Chain group 5, i=5, o=5 — self-spanning per tower,
    // but that's fine; they just prove we don't need to process them.
    auto tower3_tiles = filled_var_tower(H, 5, 5, 5);
    tower3_tiles[0] = make_tileop({5}, {5}, {{5, 50}}, {{5, 50}});
    auto tower4_tiles = filled_var_tower(H, 5, 5, 5);
    tower4_tiles[0] = make_tileop({5}, {5}, {{5, 50}}, {});

    const Grid grid = make_simple_grid(5, {0, 0, 0, 0});
    const auto buf0 = make_var_tower(tower0_tiles, H);
    const auto buf1 = make_var_tower(tower1_tiles, H);
    const auto buf2 = make_var_tower(tower2_tiles, H);
    const auto buf3 = make_var_tower(tower3_tiles, H);
    const auto buf4 = make_var_tower(tower4_tiles, H);

    Compositor compositor;
    compositor.init(grid);
    compositor.set_burst_mode(true);

    // Ingest tower 0: inner = {group 1}, outer = {group 2}. No span.
    compositor.ingest_tower(0, buf0.data());
    assert(!compositor.check_spanning_incremental());

    // Ingest tower 1: inner adds tower 1's I-face group 1 (already same root).
    // Outer adds tower 1's O-face group 2 (tower 1's group 2). L/R connects
    // tower 0 group 1 ↔ tower 1 group 1. But outer is still group 2. No span.
    compositor.ingest_tower(1, buf1.data());
    assert(!compositor.check_spanning_incremental());

    // Ingest tower 2: inner adds tower 2's I-face group 1. L/R connects group 1
    // across all towers. Outer now includes tower 2's O-face group 1.
    // Group 1 is in both inner and outer → spanning!
    compositor.ingest_tower(2, buf2.data());
    // O-face tread is always collected during ingest, so spanning should be
    // detectable even without explicit collect_outer_boundary.
    assert(compositor.check_spanning_incremental());

    const CompositorResult result = compositor.finalize();
    assert(result.verdict == CompositorResult::SPANNING);

    std::printf("OK\n");
}

// ---------------------------------------------------------------------------
// Gate 5 — Deferred outer boundary (burst mode)
// ---------------------------------------------------------------------------

void test_deferred_outer_boundary() {
    std::printf("  test_deferred_outer_boundary... ");

    // Verify that burst mode defers R-face outer boundary collection, and that
    // collect_outer_boundary(last_ingested) properly adds it.
    //
    // API contract: collect_outer_boundary(j) must be called with j == last_ingested.
    // Burst-mode protocol: ingest burst → collect_outer_boundary(last) → check spanning.
    //
    // 2 towers, uniform height 32, delta[0] = TILE_SIDE.
    // delta = TILE_SIDE → tower 1 shifted down by 1 tile row relative to tower 0.
    // This exposes tower 0's R-face at row 31 as inner-staircase outer boundary riser.
    //
    // Construction: ALL O-face treads use group 2 (not connected to inner group 1).
    // Group 1 chains vertically in both towers. L/R connects them.
    // The ONLY outer boundary connected to inner is tower 0's R-face riser at row 31.
    // In burst mode, this riser is deferred during ingest_tower(0). When we later
    // call collect_outer_boundary on the last ingested tower, it collects it.
    //
    // Problem: collect_outer_boundary reads from prev_tower_tiles_ which holds the
    // last ingested tower's data. We need j == last_ingested for it to work.
    //
    // Solution: ingest only tower 0 in the first burst. Call collect_outer_boundary(0).
    // Actually, collect_outer_boundary(0) looks at delta[0] which needs j < num_towers - 1.
    // With 2 towers and j=0: j < 1 is true, delta[0] = TILE_SIDE, q=1.
    // prev_tower_tiles_ holds tower 0 data (last ingested). R-face riser collected.
    //
    // Flow: ingest tower 0 → check (false) → collect_outer_boundary(0) → check (true).
    // Then ingest tower 1, finalize.
    //
    // Wait: we can't ingest tower 1 after collect_outer_boundary(0) because
    // ingest_tower requires sequential j = last_ingested_ + 1. After ingesting tower 0,
    // last_ingested_ = 0. We can then collect_outer_boundary(0), then ingest tower 1.

    const int H = TILES_PER_TOWER;  // 32

    // Tower 0: chain=1, row0_i=1 (inner), top_o=2 (outer but disconnected from inner)
    auto t0 = filled_var_tower(H, 1, 1, 2);
    t0[0] = make_tileop({1}, {1}, {}, {{1, 50}});
    // Row 31 (top): O-face = group 2, R-face has group 1 port
    t0[static_cast<std::size_t>(H - 1)] = make_tileop({2}, {1}, {}, {{1, 80}});

    // Tower 1: chain=1, row0_i=1, top_o=2
    // delta[0] = TILE_SIDE → q=1. Row r of tower 1 matches prev row (r - 1).
    // Row 1 matches prev row 0.
    auto t1 = filled_var_tower(H, 1, 1, 2);
    t1[1] = make_tileop({1}, {1}, {{1, 50}}, {});

    const Grid grid = make_simple_grid(2, {TILE_SIDE});
    const auto b0 = make_var_tower(t0, H);
    const auto b1 = make_var_tower(t1, H);

    Compositor compositor;
    compositor.init(grid);
    compositor.set_burst_mode(true);

    // Ingest tower 0 only
    compositor.ingest_tower(0, b0.data());

    // Before collect: outer boundary has only O-face tread (group 2, disconnected).
    // In burst mode, inner-staircase R-face risers are deferred.
    const bool before = compositor.check_spanning_incremental();

    // Collect outer boundary for tower 0 (the last ingested tower).
    // This adds R-face riser at row 31 (group 1) to outer boundary.
    compositor.collect_outer_boundary(0);

    const bool after = compositor.check_spanning_incremental();

    // Now ingest tower 1 and finalize
    compositor.ingest_tower(1, b1.data());
    compositor.collect_outer_boundary(1);
    const CompositorResult result = compositor.finalize();

    assert(!before);
    assert(after);
    assert(result.verdict == CompositorResult::SPANNING);

    std::printf("OK\n");
}

// ---------------------------------------------------------------------------
// Gate 5b — Burst mode intermediate R-face (BUG-1 regression test)
// ---------------------------------------------------------------------------

void test_burst_mode_intermediate_rface() {
    std::printf("  test_burst_mode_intermediate_rface... ");

    // 3 towers, uniform height 32, delta = [TILE_SIDE, TILE_SIDE].
    // Burst mode enabled.
    //
    // Design: spanning requires the inner-staircase R-face risers of tower 1
    // (from delta[1] = TILE_SIDE, which means q=1: the top row of tower 1 has
    // its R-face partially exposed as outer boundary).
    //
    // In burst mode, the current tower's inner-staircase R-face is deferred
    // to collect_outer_boundary(j). But for intermediate towers where
    // collect_outer_boundary is never called, those risers must be collected
    // during ingest of the NEXT tower (j+1). This is the BUG-1 fix.
    //
    // Construction:
    //   - delta = [TILE_SIDE, TILE_SIDE] → each tower shifted down by 1 tile.
    //   - Group 1 chains vertically in all towers.
    //   - L/R connections at primary match rows connect towers.
    //   - Inner boundary: I-face row 0 of tower 0 (group 1).
    //   - O-face treads: group 2 (disconnected from group 1).
    //   - Tower 1's top row (row 31) has R-face group 1.
    //     With delta[1] = TILE_SIDE, q=1, the row (H-q)=31 has its R-face
    //     as inner-staircase outer boundary.
    //   - The ONLY path from inner to outer goes through tower 1's R-face riser.
    //
    // Before BUG-1 fix: burst mode skips inner-staircase R-face for current tower
    // during ingest, and collect_outer_boundary(1) is never called → MOAT.
    // After fix: tower 1's inner-staircase R-face risers are collected during
    // ingest of tower 2 → SPANNING.

    const int H = TILES_PER_TOWER;  // 32

    // Tower 0: chain=1, I=1 (inner), O=2 (disconnected)
    // Row 0: R-face group 1 with h1=50 to connect to tower 1.
    // delta[0] = TILE_SIDE → tower 1's row r matches tower 0's row (r-1).
    // So tower 1 row 1 L-face matches tower 0 row 0 R-face.
    auto t0 = filled_var_tower(H, 1, 1, 2);
    t0[0] = make_tileop({1}, {1}, {}, {{1, 50}});

    // Tower 1: chain=1, I=1, O=2 (disconnected from inner)
    // Row 1: L-face matches tower 0 row 0 (h1 = 50 + 0 from primary match with f=0).
    // Wait: delta[0] = TILE_SIDE means d = TILE_SIDE, q = 1, f = 0.
    // primary_prev_row = row - q = row - 1.
    // For tower 1 row 1: primary_prev_row = 0. L-face h1 must equal R-face h1 + f = 50 + 0 = 50.
    //
    // Row 31 (top): R-face group 1 port. With delta[1] = TILE_SIDE, q=1:
    // Inner-staircase R-face collects rows (H-q)..H-1 = row 31.
    // This R-face is the outer boundary path.
    auto t1 = filled_var_tower(H, 1, 1, 2);
    t1[0] = make_tileop({1}, {1}, {{1, 50}}, {{1, 50}});
    t1[static_cast<std::size_t>(H - 1)] = make_tileop({2}, {1}, {}, {{1, 80}});

    // Tower 2: chain=1, I=1, O=2
    // Row 1: L-face matches tower 1 row 0. delta[1] = TILE_SIDE, q=1, f=0.
    // primary_prev_row = 1 - 1 = 0.
    auto t2 = filled_var_tower(H, 1, 1, 2);
    t2[1] = make_tileop({1}, {1}, {{1, 50}}, {});

    const Grid grid = make_simple_grid(3, {TILE_SIDE, TILE_SIDE});
    const auto b0 = make_var_tower(t0, H);
    const auto b1 = make_var_tower(t1, H);
    const auto b2 = make_var_tower(t2, H);

    // --- Burst mode test ---
    Compositor compositor;
    compositor.init(grid);
    compositor.set_burst_mode(true);

    compositor.ingest_tower(0, b0.data());
    assert(!compositor.check_spanning_incremental());

    compositor.ingest_tower(1, b1.data());
    // Tower 1 has R-face riser at row 31, but in burst mode the current tower's
    // inner-staircase R-face is deferred. Without the fix, this stays deferred
    // forever for intermediate towers.
    assert(!compositor.check_spanning_incremental());

    compositor.ingest_tower(2, b2.data());
    // BUG-1 FIX: during ingest of tower 2, the PREVIOUS tower's (tower 1)
    // inner-staircase R-face risers are now collected. Group 1 R-face of
    // tower 1 row 31 enters outer_members_ → spanning detected.
    assert(compositor.check_spanning_incremental());

    compositor.collect_outer_boundary(2);
    const CompositorResult result = compositor.finalize();
    assert(result.verdict == CompositorResult::SPANNING);

    // --- Verify non-burst mode also works (regression) ---
    Compositor compositor2;
    compositor2.init(grid);
    // burst_mode = false (default)

    compositor2.ingest_tower(0, b0.data());
    compositor2.ingest_tower(1, b1.data());
    compositor2.ingest_tower(2, b2.data());

    const CompositorResult result2 = compositor2.finalize();
    assert(result2.verdict == CompositorResult::SPANNING);

    std::printf("OK\n");
}

// ---------------------------------------------------------------------------
// Gate 6 — Known-verdict end-to-end with variable heights
// ---------------------------------------------------------------------------

void test_variable_height_spanning_e2e() {
    std::printf("  test_variable_height_spanning_e2e... ");

    // 5 towers: heights [32, 34, 38, 36, 32]
    // Group 1 chains vertically in all towers. L/R connects towers at row 0.
    // Inner boundary: I-face row 0 of tower 0 (group 1).
    // Outer boundary: O-face of each tower's top row (group 1).
    // Result: SPANNING.

    const std::vector<int> heights = {32, 34, 38, 36, 32};
    const int N = 5;

    std::vector<std::vector<Tile128>> tower_tiles(static_cast<std::size_t>(N));
    for (int j = 0; j < N; ++j) {
        tower_tiles[static_cast<std::size_t>(j)] =
            filled_var_tower(heights[static_cast<std::size_t>(j)], 1, 1, 1);
    }

    // L/R connections at row 0 between adjacent towers (all deltas = 0)
    tower_tiles[0][0] = make_tileop({1}, {1}, {}, {{1, 50}});
    tower_tiles[1][0] = make_tileop({1}, {1}, {{1, 50}}, {{1, 50}});
    tower_tiles[2][0] = make_tileop({1}, {1}, {{1, 50}}, {{1, 50}});
    tower_tiles[3][0] = make_tileop({1}, {1}, {{1, 50}}, {{1, 50}});
    tower_tiles[4][0] = make_tileop({1}, {1}, {{1, 50}}, {});

    const Grid grid = make_variable_grid(heights, {0, 0, 0, 0});

    std::vector<std::vector<uint8_t>> bufs;
    for (int j = 0; j < N; ++j) {
        bufs.push_back(make_var_tower(tower_tiles[static_cast<std::size_t>(j)],
                                       heights[static_cast<std::size_t>(j)]));
    }

    const CompositorResult result = run_variable_case(grid, bufs);

    assert(result.verdict == CompositorResult::SPANNING);
    std::printf("OK\n");
}

void test_variable_height_moat_e2e() {
    std::printf("  test_variable_height_moat_e2e... ");

    // 5 towers: heights [32, 34, 38, 36, 32]
    // Inner group (group 1 on I-face row 0) and outer group (group 2 on O-face top)
    // are disconnected within each tower. Group 3 chains mid-tower but connects
    // to neither inner nor outer.
    // No L/R connections between towers.
    // Result: MOAT.

    const std::vector<int> heights = {32, 34, 38, 36, 32};
    const int N = 5;

    std::vector<std::vector<Tile128>> tower_tiles(static_cast<std::size_t>(N));
    for (int j = 0; j < N; ++j) {
        const int h = heights[static_cast<std::size_t>(j)];
        auto& tiles = tower_tiles[static_cast<std::size_t>(j)];
        tiles.resize(static_cast<std::size_t>(h));

        // Row 0: I-face = group 1 (inner), O-face = group 3 (chain)
        tiles[0] = make_tileop({3}, {1});
        // Rows 1..h-2: chain group 3 on both I and O
        for (int row = 1; row < h - 1; ++row) {
            tiles[static_cast<std::size_t>(row)] = make_tileop({3}, {3});
        }
        // Top row: I-face = group 3 (chain), O-face = group 2 (outer, disconnected)
        tiles[static_cast<std::size_t>(h - 1)] = make_tileop({2}, {3});
    }

    const Grid grid = make_variable_grid(heights, {0, 0, 0, 0});

    std::vector<std::vector<uint8_t>> bufs;
    for (int j = 0; j < N; ++j) {
        bufs.push_back(make_var_tower(tower_tiles[static_cast<std::size_t>(j)],
                                       heights[static_cast<std::size_t>(j)]));
    }

    const CompositorResult result = run_variable_case(grid, bufs);

    assert(result.verdict == CompositorResult::MOAT);
    std::printf("OK\n");
}

}  // namespace

int main() {
    std::printf("=== test_compositor ===\n");
    test_io_spanning();
    test_io_moat();
    test_lr_spanning_zero_delta();
    test_lr_spanning_nonzero_delta();
    test_lr_no_match_wrong_h1();

    std::printf("  --- Gate 2: Variable-height L/R matching ---\n");
    test_lr_variable_height_shared_rows();
    test_lr_variable_height_no_false_match();

    std::printf("  --- Gate 3: Bump tower outer boundary ---\n");
    test_bump_tower_outer_boundary();

    std::printf("  --- Gate 4: Incremental spanning (burst mode) ---\n");
    test_incremental_spanning_early_exit();

    std::printf("  --- Gate 5: Deferred outer boundary (burst mode) ---\n");
    test_deferred_outer_boundary();

    std::printf("  --- Gate 5b: Burst mode intermediate R-face ---\n");
    test_burst_mode_intermediate_rface();

    std::printf("  --- Gate 6: Variable-height end-to-end ---\n");
    test_variable_height_spanning_e2e();
    test_variable_height_moat_e2e();

    std::printf("ALL TESTS PASSED\n");
    return 0;
}
