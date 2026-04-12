#ifdef NDEBUG
#undef NDEBUG
#endif

#include "compositor.h"

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
    grid.tiles_per_tower = TILES_PER_TOWER;
    grid.num_towers = num_towers;
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

}  // namespace

int main() {
    std::printf("=== test_compositor ===\n");
    test_io_spanning();
    test_io_moat();
    test_lr_spanning_zero_delta();
    test_lr_spanning_nonzero_delta();
    test_lr_no_match_wrong_h1();
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
