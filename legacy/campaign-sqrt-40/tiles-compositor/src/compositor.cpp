#include "compositor.h"

#include <algorithm>

namespace {

void assert_not_overflow(const uint8_t* tile) {
    assert(tile != nullptr);
    assert(!is_overflow(tile));
    if (tile == nullptr || is_overflow(tile)) {
        std::abort();
    }
}

}  // namespace

void Compositor::init(const Grid& grid) {
    grid_ = &grid;
    last_ingested_ = -1;
    parent_.clear();
    root_reach_.clear();
    spanning_detected_ = false;
    inner_members_.clear();
    outer_members_.clear();
    prev_tower_height_ = 0;
    burst_mode_ = false;

    const std::size_t num_tiles = static_cast<std::size_t>(grid.total_tiles);
    group_offset_.assign(num_tiles + 1U, 0U);
    parent_.reserve(num_tiles * 8U);
    root_reach_.reserve(num_tiles * 8U);
    std::memset(prev_tower_tiles_, 0, sizeof(prev_tower_tiles_));
}

void Compositor::set_burst_mode(bool enabled) {
    burst_mode_ = enabled;
}

void Compositor::ingest_tower(int32_t j, const uint8_t* tower_tileops, const ExtendedTileSideTable* ext) {
    assert(grid_ != nullptr);
    assert(tower_tileops != nullptr);
    assert(j == last_ingested_ + 1);
    if (grid_ == nullptr || tower_tileops == nullptr || j != last_ingested_ + 1) {
        std::abort();
    }

    compute_offsets_for_tower(j, tower_tileops, ext);
    match_io_within_tower(j, tower_tileops, ext);
    pre_flatten_tower(j);
    match_lr_with_previous(j, tower_tileops, ext);
    collect_inner_boundary(j, tower_tileops, ext);
    collect_outer_boundary_ingest(j, tower_tileops, ext);

    // C3: copy current tower into prev buffer using actual height, not fixed constant
    const uint32_t h_j = grid_->tiles_per_tower[static_cast<std::size_t>(j)];
    std::memcpy(prev_tower_tiles_, tower_tileops,
                static_cast<std::size_t>(h_j) * static_cast<std::size_t>(TILEOP_SIZE));
    prev_tower_height_ = h_j;
    last_ingested_ = j;
}

bool Compositor::has_spanning() {
    return spanning_detected_;
}

// C1: Incremental spanning check -- returns cached flag. O(1).
// Called between bursts; does NOT finalize.
bool Compositor::check_spanning_incremental() {
    return spanning_detected_;
}

// C1: Explicit outer boundary collection for tower j.
// Called by campaign runner when tower j is confirmed as rightmost.
// Collects R-face risers (inner-staircase and outer-staircase) plus
// outer-staircase L-face risers. O-face tread was already collected
// during ingest_tower.
void Compositor::collect_outer_boundary(int32_t j) {
    assert(grid_ != nullptr);
    assert(j >= 0 && j <= last_ingested_);

    const int h_j = static_cast<int>(grid_->tiles_per_tower[static_cast<std::size_t>(j)]);

    // --- Inner-staircase R-face risers (from base_y differences) ---
    if (j < grid_->num_towers - 1) {
        const int64_t d = grid_->delta[static_cast<std::size_t>(j)];
        const int q = static_cast<int>(d / TILE_SIDE);
        const int f = static_cast<int>(d % TILE_SIDE);

        // R-face ports of rows (H_j-q)..H_j-1 when q > 0
        if (q > 0) {
            for (int row = h_j - q; row < h_j; ++row) {
                if (row < 0) continue;
                const uint32_t flat = static_cast<uint32_t>(tile_index(*grid_, j, row));
                if (is_tile_dead(*grid_, j, row)) continue;

                // We need tile data to read R-face. For the last ingested tower,
                // the data is in prev_tower_tiles_ (copied at end of ingest_tower).
                // For earlier towers we don't have the raw data anymore.
                // But since this method is only called for the rightmost ingested tower,
                // j == last_ingested_, so the data is in prev_tower_tiles_.
                const uint8_t* data = prev_tower_tiles_ +
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(TILEOP_SIZE);
                const int budget = get_payload_budget(flat, nullptr);
                assert_not_overflow(data);
                if (is_dead(data)) continue;

                const FaceSlice r_slice = face_slice(data, FACE_R, budget);
                for (uint8_t ri = 0; ri < r_slice.count; ++ri) {
                    const uint8_t group = decode_group_id(r_slice.groups[ri]);
                    if (group == 0U) continue;
                    const uint32_t gid = global_id(flat, group);
                    outer_members_.push_back(gid);
                    mark_outer(gid);
                }
            }
        }

        // R-face ports of row (H_j-1-q) with h1 >= S - f when f > 0
        if (f > 0 && (h_j - 1 - q) >= 0) {
            const int row = h_j - 1 - q;
            const uint32_t flat = static_cast<uint32_t>(tile_index(*grid_, j, row));
            if (!is_tile_dead(*grid_, j, row)) {
                const uint8_t* data = prev_tower_tiles_ +
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(TILEOP_SIZE);
                const int budget = get_payload_budget(flat, nullptr);
                assert_not_overflow(data);
                if (!is_dead(data)) {
                    const TileOpCounts counts = parse_counts(data, budget);
                    const FaceSlice r_slice = face_slice(data, FACE_R, budget);
                    const uint8_t* r_h1 = data + counts.h_start + counts.l_cnt;

                    for (uint8_t ri = 0; ri < r_slice.count; ++ri) {
                        const uint8_t group = decode_group_id(r_slice.groups[ri]);
                        if (group == 0U) continue;
                        const uint16_t h = decode_h1(r_slice.groups[ri], r_h1[ri]);
                        if (h >= static_cast<uint16_t>(TILE_SIDE - f)) {
                            const uint32_t gid = global_id(flat, group);
                            outer_members_.push_back(gid);
                            mark_outer(gid);
                        }
                    }
                }
            }
        }
    }

    // --- Outer-staircase R-face risers (from variable tower height) ---
    if (j < grid_->num_towers - 1) {
        const int h_next = static_cast<int>(
            grid_->tiles_per_tower[static_cast<std::size_t>(j + 1)]);
        if (h_j > h_next) {
            for (int row = h_next; row < h_j; ++row) {
                const uint32_t flat = static_cast<uint32_t>(tile_index(*grid_, j, row));
                if (is_tile_dead(*grid_, j, row)) continue;

                const uint8_t* data = prev_tower_tiles_ +
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(TILEOP_SIZE);
                const int budget = get_payload_budget(flat, nullptr);
                assert_not_overflow(data);
                if (is_dead(data)) continue;

                const FaceSlice r_slice = face_slice(data, FACE_R, budget);
                for (uint8_t ri = 0; ri < r_slice.count; ++ri) {
                    const uint8_t group = decode_group_id(r_slice.groups[ri]);
                    if (group == 0U) continue;
                    const uint32_t gid = global_id(flat, group);
                    outer_members_.push_back(gid);
                    mark_outer(gid);
                }
            }
        }
    }

    // --- Outer-staircase L-face risers (from variable tower height) ---
    if (j > 0) {
        const int h_prev = static_cast<int>(
            grid_->tiles_per_tower[static_cast<std::size_t>(j - 1)]);
        if (h_j > h_prev) {
            for (int row = h_prev; row < h_j; ++row) {
                const uint32_t flat = static_cast<uint32_t>(tile_index(*grid_, j, row));
                if (is_tile_dead(*grid_, j, row)) continue;

                const uint8_t* data = prev_tower_tiles_ +
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(TILEOP_SIZE);
                const int budget = get_payload_budget(flat, nullptr);
                assert_not_overflow(data);
                if (is_dead(data)) continue;

                const FaceSlice l_slice = face_slice(data, FACE_L, budget);
                for (uint8_t li = 0; li < l_slice.count; ++li) {
                    const uint8_t group = decode_group_id(l_slice.groups[li]);
                    if (group == 0U) continue;
                    const uint32_t gid = global_id(flat, group);
                    outer_members_.push_back(gid);
                    mark_outer(gid);
                }
            }
        }
    }
}

CompositorResult Compositor::finalize() {
    std::unordered_set<uint32_t> inner_roots;
    std::unordered_set<uint32_t> outer_roots;
    inner_roots.reserve(inner_members_.size());
    outer_roots.reserve(outer_members_.size());

    for (uint32_t member : inner_members_) {
        inner_roots.insert(find(member));
    }
    for (uint32_t member : outer_members_) {
        outer_roots.insert(find(member));
    }

    // Use the incrementally-maintained flag for the verdict.
    // Cross-check against set intersection in debug builds.
    CompositorResult::Verdict verdict = spanning_detected_
        ? CompositorResult::SPANNING
        : CompositorResult::MOAT;

#ifndef NDEBUG
    bool set_check = false;
    for (uint32_t root : outer_roots) {
        if (inner_roots.count(root) > 0U) {
            set_check = true;
            break;
        }
    }
    assert(set_check == spanning_detected_);
#endif

    return CompositorResult{
        verdict,
        static_cast<uint32_t>(parent_.size()),
        static_cast<uint32_t>(inner_roots.size()),
        static_cast<uint32_t>(outer_roots.size()),
    };
}

uint32_t Compositor::find(uint32_t x) {
    while (parent_[x] != x) {
        parent_[x] = parent_[parent_[x]];
        x = parent_[x];
    }
    return x;
}

void Compositor::unite(uint32_t a, uint32_t b) {
    uint32_t ra = find(a);
    uint32_t rb = find(b);
    if (ra == rb) {
        return;
    }
    if (ra > rb) {
        const uint32_t tmp = ra;
        ra = rb;
        rb = tmp;
    }
    // Merge reachability: new root ra inherits both sides' flags
    const uint8_t merged = root_reach_[ra] | root_reach_[rb];
    parent_[rb] = ra;
    root_reach_[ra] = merged;
    // root_reach_[rb] is now stale (rb is no longer a root), but harmless
    if (merged == REACH_BOTH) {
        spanning_detected_ = true;
    }
}

void Compositor::mark_inner(uint32_t member) {
    const uint32_t root = find(member);
    root_reach_[root] |= REACH_INNER;
    if (root_reach_[root] == REACH_BOTH) {
        spanning_detected_ = true;
    }
}

void Compositor::mark_outer(uint32_t member) {
    const uint32_t root = find(member);
    root_reach_[root] |= REACH_OUTER;
    if (root_reach_[root] == REACH_BOTH) {
        spanning_detected_ = true;
    }
}

const uint8_t* Compositor::get_tile_data(uint32_t flat_idx, const uint8_t* base_ptr, int row,
                                         const ExtendedTileSideTable* ext) const {
    assert(base_ptr != nullptr);
    if (ext != nullptr && ext->is_extended(flat_idx)) {
        return ext->extended_ops.at(flat_idx).data();
    }
    return base_ptr + static_cast<std::size_t>(row) * static_cast<std::size_t>(TILEOP_SIZE);
}

int Compositor::get_payload_budget(uint32_t flat_idx, const ExtendedTileSideTable* ext) const {
    if (ext != nullptr && ext->is_extended(flat_idx)) {
        return TILEOP_EXT_PAYLOAD_BYTES;
    }
    return TILEOP_PAYLOAD_BYTES;
}

void Compositor::compute_offsets_for_tower(int32_t j, const uint8_t* tower_tileops,
                                           const ExtendedTileSideTable* ext) {
    // C2: use per-tower height
    const int h_j = static_cast<int>(grid_->tiles_per_tower[static_cast<std::size_t>(j)]);
    for (int row = 0; row < h_j; ++row) {
        const uint32_t flat_idx = static_cast<uint32_t>(tile_index(*grid_, j, row));
        const uint8_t* tile_data = get_tile_data(flat_idx, tower_tileops, row, ext);
        const int budget = get_payload_budget(flat_idx, ext);
        assert_not_overflow(tile_data);

        uint8_t max_label = 0;
        if (!is_tile_dead(*grid_, j, row) && !is_dead(tile_data)) {
            max_label = max_group_label(tile_data, budget);
        }

        // Defensive: L/R face encoding uses only 7 bits for the group ID
        // (bit 7 is stolen for h1 MSB).  If a tile somehow has >= 128 groups,
        // its L/R port data is structurally unreliable -- treat it as dead.
        // The encoder should already have poisoned such tiles, but we guard
        // against upstream bugs to prevent silent verdict corruption.
        if (max_label >= 128U) {
            max_label = 0;
        }

        const uint32_t base = group_offset_[flat_idx];
        group_offset_[flat_idx + 1U] = base + static_cast<uint32_t>(max_label);
        for (uint32_t group = 0; group < static_cast<uint32_t>(max_label); ++group) {
            parent_.push_back(base + group);
            root_reach_.push_back(0U);
        }
    }

    if (parent_.size() > 2000000000ULL) {
        const double avg = static_cast<double>(parent_.size()) /
                           static_cast<double>((static_cast<uint64_t>(j) + 1ULL) *
                                               static_cast<uint64_t>(h_j));
        std::fprintf(stderr, "OOM cap: parent_ size %zu at tower %d (%.1f groups/tile avg)\n",
                     parent_.size(), j, avg);
        std::abort();
    }
}

void Compositor::match_io_within_tower(int32_t j, const uint8_t* tower_tileops,
                                       const ExtendedTileSideTable* ext) {
    // C2: use per-tower height
    const int h_j = static_cast<int>(grid_->tiles_per_tower[static_cast<std::size_t>(j)]);

    // Within a tower, adjacent rows share a b-direction boundary.
    // FACE_O (top, high b) of row r connects to FACE_I (bottom, low b) of row r+1.
    for (int row = 0; row < h_j - 1; ++row) {
        if (is_tile_dead(*grid_, j, row) || is_tile_dead(*grid_, j, row + 1)) {
            continue;
        }

        const uint32_t flat_top = static_cast<uint32_t>(tile_index(*grid_, j, row));
        const uint32_t flat_bottom = static_cast<uint32_t>(tile_index(*grid_, j, row + 1));
        const uint8_t* top_data = get_tile_data(flat_top, tower_tileops, row, ext);
        const uint8_t* bottom_data = get_tile_data(flat_bottom, tower_tileops, row + 1, ext);
        const int top_budget = get_payload_budget(flat_top, ext);
        const int bottom_budget = get_payload_budget(flat_bottom, ext);
        assert_not_overflow(top_data);
        assert_not_overflow(bottom_data);

        if (is_dead(top_data) || is_dead(bottom_data)) {
            continue;
        }

        const FaceSlice o_slice = face_slice(top_data, FACE_O, top_budget);
        const FaceSlice i_slice = face_slice(bottom_data, FACE_I, bottom_budget);

        const uint8_t match_cnt = (o_slice.count < i_slice.count)
                                  ? o_slice.count : i_slice.count;
        for (uint8_t s = 0; s < match_cnt; ++s) {
            const uint8_t go = o_slice.groups[s];
            const uint8_t gi = i_slice.groups[s];
            if (go > 0U && gi > 0U) {
                unite(global_id(flat_top, go), global_id(flat_bottom, gi));
            }
        }
    }
}

void Compositor::pre_flatten_tower(int32_t j) {
    // C2: use per-tower height
    const int h_j = static_cast<int>(grid_->tiles_per_tower[static_cast<std::size_t>(j)]);
    const uint32_t start = group_offset_[static_cast<uint32_t>(tile_index(*grid_, j, 0))];
    const uint32_t end = group_offset_[static_cast<uint32_t>(tile_index(*grid_, j, h_j - 1)) + 1U];
    for (uint32_t id = start; id < end; ++id) {
        (void)find(id);
    }
}

void Compositor::match_lr_with_previous(int32_t j, const uint8_t* tower_tileops,
                                        const ExtendedTileSideTable* ext) {
    if (j <= 0) {
        return;
    }

    const int64_t d = (*grid_).delta[static_cast<std::size_t>(j - 1)];
    const int q = static_cast<int>(d / TILE_SIDE);
    const int f = static_cast<int>(d % TILE_SIDE);

    // C2: iterate only over shared rows = min(curr_height, prev_height)
    const int h_j = static_cast<int>(grid_->tiles_per_tower[static_cast<std::size_t>(j)]);
    const int h_prev = static_cast<int>(grid_->tiles_per_tower[static_cast<std::size_t>(j - 1)]);
    const int shared_rows = std::min(h_j, h_prev);

    for (int row = 0; row < shared_rows; ++row) {
        const uint32_t cur_flat = static_cast<uint32_t>(tile_index(*grid_, j, row));
        if (is_tile_dead(*grid_, j, row)) {
            continue;
        }

        const uint8_t* cur_data = get_tile_data(cur_flat, tower_tileops, row, ext);
        const int cur_budget = get_payload_budget(cur_flat, ext);
        assert_not_overflow(cur_data);
        if (is_dead(cur_data)) {
            continue;
        }

        const FaceSlice l_slice = face_slice(cur_data, FACE_L, cur_budget);

        // Primary match: previous tower row (row - q)
        const int primary_prev_row = row - q;
        if (primary_prev_row >= 0 && primary_prev_row < h_prev) {
            const uint32_t prev_flat = static_cast<uint32_t>(tile_index(*grid_, j - 1, primary_prev_row));
            if (!is_tile_dead(*grid_, j - 1, primary_prev_row)) {
                const uint8_t* prev_data = get_tile_data(prev_flat, prev_tower_tiles_, primary_prev_row, ext);
                const int prev_budget = get_payload_budget(prev_flat, ext);
                assert_not_overflow(prev_data);
                if (!is_dead(prev_data)) {
                    const FaceSlice r_slice = face_slice(prev_data, FACE_R, prev_budget);

                    for (uint8_t li = 0; li < l_slice.count; ++li) {
                        const uint8_t gl = decode_group_id(l_slice.groups[li]);
                        if (gl == 0U) continue;
                        const uint16_t hl = decode_h1(l_slice.groups[li],
                                                       l_slice.h1_bytes[li]);

                        for (uint8_t ri = 0; ri < r_slice.count; ++ri) {
                            const uint8_t gr = decode_group_id(r_slice.groups[ri]);
                            if (gr == 0U) continue;
                            const uint16_t hr = decode_h1(r_slice.groups[ri],
                                                           r_slice.h1_bytes[ri]);

                            if (hl == hr + static_cast<uint16_t>(f)) {
                                unite(global_id(prev_flat, gr),
                                      global_id(cur_flat, gl));
                            }
                        }
                    }
                }
            }
        }

        // Secondary match: previous tower row (row - q - 1) when f > 0
        if (f > 0) {
            const int secondary_prev_row = primary_prev_row - 1;
            if (secondary_prev_row >= 0 && secondary_prev_row < h_prev) {
                const uint32_t prev_flat =
                    static_cast<uint32_t>(tile_index(*grid_, j - 1, secondary_prev_row));
                if (!is_tile_dead(*grid_, j - 1, secondary_prev_row)) {
                    const uint8_t* prev_data =
                        get_tile_data(prev_flat, prev_tower_tiles_, secondary_prev_row, ext);
                    const int prev_budget = get_payload_budget(prev_flat, ext);
                    assert_not_overflow(prev_data);
                    if (!is_dead(prev_data)) {
                        const FaceSlice r_slice = face_slice(prev_data, FACE_R, prev_budget);

                        for (uint8_t li = 0; li < l_slice.count; ++li) {
                            const uint8_t gl = decode_group_id(l_slice.groups[li]);
                            if (gl == 0U) continue;
                            const uint16_t hl = decode_h1(l_slice.groups[li],
                                                           l_slice.h1_bytes[li]);

                            for (uint8_t ri = 0; ri < r_slice.count; ++ri) {
                                const uint8_t gr = decode_group_id(r_slice.groups[ri]);
                                if (gr == 0U) continue;
                                const uint16_t hr = decode_h1(r_slice.groups[ri],
                                                               r_slice.h1_bytes[ri]);

                                if (hl + static_cast<uint16_t>(TILE_SIDE - f) == hr) {
                                    unite(global_id(prev_flat, gr),
                                          global_id(cur_flat, gl));
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void Compositor::collect_inner_boundary(int32_t j, const uint8_t* tower_tileops,
                                        const ExtendedTileSideTable* ext) {
    const uint32_t row0_flat = static_cast<uint32_t>(tile_index(*grid_, j, 0));
    if (!is_tile_dead(*grid_, j, 0)) {
        const uint8_t* row0_data = get_tile_data(row0_flat, tower_tileops, 0, ext);
        const int row0_budget = get_payload_budget(row0_flat, ext);
        assert_not_overflow(row0_data);
        if (!is_dead(row0_data)) {
            const FaceSlice i_slice = face_slice(row0_data, FACE_I, row0_budget);
            for (uint8_t slot = 0; slot < i_slice.count; ++slot) {
                // I-face groups are raw 8-bit labels (no h1 bit-steal).
                // decode_group_id would mask & 0x7F, truncating labels >= 128.
                const uint8_t group = i_slice.groups[slot];
                if (group > 0U) {
                    const uint32_t gid = global_id(row0_flat, group);
                    inner_members_.push_back(gid);
                    mark_inner(gid);
                }
            }
        }
    }

    if (j <= 0) {
        return;
    }

    // C2: use per-tower height for bounds
    const int h_j = static_cast<int>(grid_->tiles_per_tower[static_cast<std::size_t>(j)]);

    const int64_t d_prev = (*grid_).delta[static_cast<std::size_t>(j - 1)];
    const int q_prev = static_cast<int>(d_prev / TILE_SIDE);
    const int f_prev = static_cast<int>(d_prev % TILE_SIDE);

    if (q_prev > 0) {
        const int end_row = std::min(q_prev, h_j);
        for (int row = 0; row < end_row; ++row) {
            const uint32_t flat = static_cast<uint32_t>(tile_index(*grid_, j, row));
            if (is_tile_dead(*grid_, j, row)) {
                continue;
            }

            const uint8_t* data = get_tile_data(flat, tower_tileops, row, ext);
            const int budget = get_payload_budget(flat, ext);
            assert_not_overflow(data);
            if (is_dead(data)) {
                continue;
            }

            const TileOpCounts counts = parse_counts(data, budget);
            const FaceSlice l_slice = face_slice(data, FACE_L, budget);
            const uint8_t* l_h1 = data + counts.h_start;
            (void)l_h1;

            for (uint8_t li = 0; li < l_slice.count; ++li) {
                const uint8_t group = decode_group_id(l_slice.groups[li]);
                if (group == 0U) {
                    continue;
                }
                const uint32_t gid = global_id(flat, group);
                inner_members_.push_back(gid);
                mark_inner(gid);
            }
        }
    }

    if (f_prev > 0 && q_prev < h_j) {
        const uint32_t flat = static_cast<uint32_t>(tile_index(*grid_, j, q_prev));
        if (!is_tile_dead(*grid_, j, q_prev)) {
            const uint8_t* data = get_tile_data(flat, tower_tileops, q_prev, ext);
            const int budget = get_payload_budget(flat, ext);
            assert_not_overflow(data);
            if (!is_dead(data)) {
                const TileOpCounts counts = parse_counts(data, budget);
                const FaceSlice l_slice = face_slice(data, FACE_L, budget);
                const uint8_t* l_h1 = data + counts.h_start;

                for (uint8_t li = 0; li < l_slice.count; ++li) {
                    const uint8_t group = decode_group_id(l_slice.groups[li]);
                    if (group == 0U) {
                        continue;
                    }
                    const uint16_t h = decode_h1(l_slice.groups[li], l_h1[li]);
                    if (h < static_cast<uint16_t>(f_prev)) {
                        const uint32_t gid = global_id(flat, group);
                        inner_members_.push_back(gid);
                        mark_inner(gid);
                    }
                }
            }
        }
    }
}

void Compositor::collect_outer_boundary_ingest(int32_t j, const uint8_t* tower_tileops,
                                               const ExtendedTileSideTable* ext) {
    // C2: use per-tower height
    const int h_j = static_cast<int>(grid_->tiles_per_tower[static_cast<std::size_t>(j)]);
    const int last_row = h_j - 1;

    // O-face of top-row tile (horizontal tread) -- always collected
    const uint32_t row_last_flat = static_cast<uint32_t>(tile_index(*grid_, j, last_row));
    if (!is_tile_dead(*grid_, j, last_row)) {
        const uint8_t* row_last_data = get_tile_data(row_last_flat, tower_tileops, last_row, ext);
        const int row_last_budget = get_payload_budget(row_last_flat, ext);
        assert_not_overflow(row_last_data);
        if (!is_dead(row_last_data)) {
            const FaceSlice o_slice = face_slice(row_last_data, FACE_O, row_last_budget);
            for (uint8_t slot = 0; slot < o_slice.count; ++slot) {
                // O-face groups are raw 8-bit labels (no h1 bit-steal).
                // decode_group_id would mask & 0x7F, truncating labels >= 128.
                const uint8_t group = o_slice.groups[slot];
                if (group > 0U) {
                    const uint32_t gid = global_id(row_last_flat, group);
                    outer_members_.push_back(gid);
                    mark_outer(gid);
                }
            }
        }
    }

    // C4: Outer-staircase L-face risers (from variable tower height).
    // When current tower j is taller than previous tower j-1,
    // rows H_prev..H_j-1 of current tower have L-faces exposed.
    if (j > 0) {
        const int h_prev = static_cast<int>(
            grid_->tiles_per_tower[static_cast<std::size_t>(j - 1)]);
        if (h_j > h_prev) {
            for (int row = h_prev; row < h_j; ++row) {
                const uint32_t flat = static_cast<uint32_t>(tile_index(*grid_, j, row));
                if (is_tile_dead(*grid_, j, row)) continue;

                const uint8_t* data = get_tile_data(flat, tower_tileops, row, ext);
                const int budget = get_payload_budget(flat, ext);
                assert_not_overflow(data);
                if (is_dead(data)) continue;

                const FaceSlice l_slice = face_slice(data, FACE_L, budget);
                for (uint8_t li = 0; li < l_slice.count; ++li) {
                    const uint8_t group = decode_group_id(l_slice.groups[li]);
                    if (group == 0U) continue;
                    const uint32_t gid = global_id(flat, group);
                    outer_members_.push_back(gid);
                    mark_outer(gid);
                }
            }
        }
    }

    // C4: Outer-staircase R-face risers from the PREVIOUS tower (j-1).
    // When processing tower j, if H_{j-1} > H_j, then tower j-1 has rows
    // H_j..H_{j-1}-1 with R-faces exposed (no matching tile in tower j).
    // We collect these now because we now know H_j.
    if (j > 0) {
        const int h_prev = static_cast<int>(
            grid_->tiles_per_tower[static_cast<std::size_t>(j - 1)]);
        if (h_prev > h_j) {
            for (int row = h_j; row < h_prev; ++row) {
                const uint32_t flat = static_cast<uint32_t>(
                    tile_index(*grid_, j - 1, row));
                if (is_tile_dead(*grid_, j - 1, row)) continue;

                // Read from prev_tower_tiles_ buffer
                const uint8_t* data = prev_tower_tiles_ +
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(TILEOP_SIZE);
                const int budget = get_payload_budget(flat, ext);
                assert_not_overflow(data);
                if (is_dead(data)) continue;

                const FaceSlice r_slice = face_slice(data, FACE_R, budget);
                for (uint8_t ri = 0; ri < r_slice.count; ++ri) {
                    const uint8_t group = decode_group_id(r_slice.groups[ri]);
                    if (group == 0U) continue;
                    const uint32_t gid = global_id(flat, group);
                    outer_members_.push_back(gid);
                    mark_outer(gid);
                }
            }
        }
    }

    // BUG-1 FIX: Inner-staircase R-face risers of the PREVIOUS tower (j-1).
    // In burst mode, only the CURRENT tower j's R-face is deferred (collected
    // later by collect_outer_boundary(j)). The previous tower's inner-staircase
    // R-face risers must always be collected here because we now know both
    // H_{j-1} and H_j, and collect_outer_boundary(j-1) will never be called
    // for intermediate towers.
    if (j > 0 && (j - 1) < grid_->num_towers - 1) {
        const int h_prev = static_cast<int>(
            grid_->tiles_per_tower[static_cast<std::size_t>(j - 1)]);
        const int prev_last_row = h_prev - 1;
        const int64_t d_prev = (*grid_).delta[static_cast<std::size_t>(j - 1)];
        const int q_prev = static_cast<int>(d_prev / TILE_SIDE);
        const int f_prev = static_cast<int>(d_prev % TILE_SIDE);

        // R-face ports of rows (H_{j-1} - q_prev)..H_{j-1}-1 when q_prev > 0
        if (q_prev > 0) {
            for (int row = h_prev - q_prev; row < h_prev; ++row) {
                if (row < 0) continue;
                const uint32_t flat = static_cast<uint32_t>(
                    tile_index(*grid_, j - 1, row));
                if (is_tile_dead(*grid_, j - 1, row)) continue;

                const uint8_t* data = prev_tower_tiles_ +
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(TILEOP_SIZE);
                const int budget = get_payload_budget(flat, ext);
                assert_not_overflow(data);
                if (is_dead(data)) continue;

                const FaceSlice r_slice = face_slice(data, FACE_R, budget);
                for (uint8_t ri = 0; ri < r_slice.count; ++ri) {
                    const uint8_t group = decode_group_id(r_slice.groups[ri]);
                    if (group == 0U) continue;
                    const uint32_t gid = global_id(flat, group);
                    outer_members_.push_back(gid);
                    mark_outer(gid);
                }
            }
        }

        // R-face ports of row (H_{j-1}-1-q_prev) with h1 >= S - f_prev when f_prev > 0
        if (f_prev > 0 && (prev_last_row - q_prev) >= 0) {
            const int row = prev_last_row - q_prev;
            const uint32_t flat = static_cast<uint32_t>(
                tile_index(*grid_, j - 1, row));
            if (!is_tile_dead(*grid_, j - 1, row)) {
                const uint8_t* data = prev_tower_tiles_ +
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(TILEOP_SIZE);
                const int budget = get_payload_budget(flat, ext);
                assert_not_overflow(data);
                if (!is_dead(data)) {
                    const TileOpCounts counts = parse_counts(data, budget);
                    const FaceSlice r_slice = face_slice(data, FACE_R, budget);
                    const uint8_t* r_h1 = data + counts.h_start + counts.l_cnt;

                    for (uint8_t ri = 0; ri < r_slice.count; ++ri) {
                        const uint8_t group = decode_group_id(r_slice.groups[ri]);
                        if (group == 0U) continue;
                        const uint16_t h = decode_h1(r_slice.groups[ri], r_h1[ri]);
                        if (h >= static_cast<uint16_t>(TILE_SIDE - f_prev)) {
                            const uint32_t gid = global_id(flat, group);
                            outer_members_.push_back(gid);
                            mark_outer(gid);
                        }
                    }
                }
            }
        }
    }

    // In non-burst mode: collect inner-staircase R-face risers of current tower j.
    // In burst mode: skip current tower's R-face (deferred to collect_outer_boundary(j)).
    if (burst_mode_) {
        return;
    }

    // Inner-staircase R-face risers (from base_y differences) -- same as original
    if (j >= grid_->num_towers - 1) {
        return;
    }

    const int64_t d = (*grid_).delta[static_cast<std::size_t>(j)];
    const int q_d = static_cast<int>(d / TILE_SIDE);
    const int f_d = static_cast<int>(d % TILE_SIDE);

    if (q_d > 0) {
        for (int row = h_j - q_d; row < h_j; ++row) {
            if (row < 0) {
                continue;
            }

            const uint32_t flat = static_cast<uint32_t>(tile_index(*grid_, j, row));
            if (is_tile_dead(*grid_, j, row)) {
                continue;
            }

            const uint8_t* data = get_tile_data(flat, tower_tileops, row, ext);
            const int budget = get_payload_budget(flat, ext);
            assert_not_overflow(data);
            if (is_dead(data)) {
                continue;
            }

            const TileOpCounts counts = parse_counts(data, budget);
            const FaceSlice r_slice = face_slice(data, FACE_R, budget);
            const uint8_t* r_h1 = data + counts.h_start + counts.l_cnt;
            (void)r_h1;

            for (uint8_t ri = 0; ri < r_slice.count; ++ri) {
                const uint8_t group = decode_group_id(r_slice.groups[ri]);
                if (group == 0U) {
                    continue;
                }
                const uint32_t gid = global_id(flat, group);
                outer_members_.push_back(gid);
                mark_outer(gid);
            }
        }
    }

    if (f_d > 0 && (last_row - q_d) >= 0) {
        const int row = last_row - q_d;
        const uint32_t flat = static_cast<uint32_t>(tile_index(*grid_, j, row));
        if (!is_tile_dead(*grid_, j, row)) {
            const uint8_t* data = get_tile_data(flat, tower_tileops, row, ext);
            const int budget = get_payload_budget(flat, ext);
            assert_not_overflow(data);
            if (!is_dead(data)) {
                const TileOpCounts counts = parse_counts(data, budget);
                const FaceSlice r_slice = face_slice(data, FACE_R, budget);
                const uint8_t* r_h1 = data + counts.h_start + counts.l_cnt;

                for (uint8_t ri = 0; ri < r_slice.count; ++ri) {
                    const uint8_t group = decode_group_id(r_slice.groups[ri]);
                    if (group == 0U) {
                        continue;
                    }
                    const uint16_t h = decode_h1(r_slice.groups[ri], r_h1[ri]);
                    if (h >= static_cast<uint16_t>(TILE_SIDE - f_d)) {
                        const uint32_t gid = global_id(flat, group);
                        outer_members_.push_back(gid);
                        mark_outer(gid);
                    }
                }
            }
        }
    }
}
