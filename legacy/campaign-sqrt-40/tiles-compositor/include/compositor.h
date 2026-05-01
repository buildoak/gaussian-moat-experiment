#pragma once

#include "grid.h"
#include "tileop_parse.h"
#include "types.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

class Compositor {
public:
    void init(const Grid& grid);
    void ingest_tower(int32_t j, const uint8_t* tower_tileops,
                      const ExtendedTileSideTable* ext = nullptr);
    bool has_spanning();
    CompositorResult finalize();

    // --- Burst-mode API (C1) ---
    void set_burst_mode(bool enabled);
    bool check_spanning_incremental();
    void collect_outer_boundary(int32_t j);

private:
    const Grid* grid_ = nullptr;
    std::vector<uint32_t> parent_;
    std::vector<uint8_t> root_reach_;  // per-element reachability: bit0=inner, bit1=outer
    bool spanning_detected_ = false;   // cached: true once any root has both bits
    std::vector<uint32_t> group_offset_;
    int32_t last_ingested_ = -1;
    uint8_t prev_tower_tiles_[TILES_PER_TOWER_MAX * TILEOP_SIZE];  // C3: sized to max
    uint32_t prev_tower_height_ = 0;  // C3: track previous tower's actual height
    bool burst_mode_ = false;  // C1: when true, skip R-face outer boundary in ingest
    std::vector<uint32_t> inner_members_;
    std::vector<uint32_t> outer_members_;

    static constexpr uint8_t REACH_INNER = 1U;
    static constexpr uint8_t REACH_OUTER = 2U;
    static constexpr uint8_t REACH_BOTH  = 3U;

    uint32_t find(uint32_t x);
    void unite(uint32_t a, uint32_t b);
    void mark_inner(uint32_t member);
    void mark_outer(uint32_t member);

    inline uint32_t global_id(uint32_t flat_idx, uint8_t group_label) const {
        return group_offset_[flat_idx] + static_cast<uint32_t>(group_label) - 1U;
    }

    const uint8_t* get_tile_data(uint32_t flat_idx, const uint8_t* base_ptr, int row,
                                 const ExtendedTileSideTable* ext) const;
    int get_payload_budget(uint32_t flat_idx, const ExtendedTileSideTable* ext) const;

    void compute_offsets_for_tower(int32_t j, const uint8_t* tower_tileops,
                                   const ExtendedTileSideTable* ext);
    void match_io_within_tower(int32_t j, const uint8_t* tower_tileops,
                               const ExtendedTileSideTable* ext);
    void pre_flatten_tower(int32_t j);
    void match_lr_with_previous(int32_t j, const uint8_t* tower_tileops,
                                const ExtendedTileSideTable* ext);
    void collect_inner_boundary(int32_t j, const uint8_t* tower_tileops,
                                const ExtendedTileSideTable* ext);
    void collect_outer_boundary_ingest(int32_t j, const uint8_t* tower_tileops,
                                       const ExtendedTileSideTable* ext);
};
