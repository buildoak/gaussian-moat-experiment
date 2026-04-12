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
    inner_members_.clear();
    outer_members_.clear();

    const std::size_t num_tiles =
        static_cast<std::size_t>(grid.num_towers) * static_cast<std::size_t>(grid.tiles_per_tower);
    group_offset_.assign(num_tiles + 1U, 0U);
    parent_.reserve(num_tiles * 8U);
    std::memset(prev_tower_tiles_, 0, sizeof(prev_tower_tiles_));
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
    collect_outer_boundary(j, tower_tileops, ext);

    std::memcpy(prev_tower_tiles_, tower_tileops,
                static_cast<std::size_t>(TILES_PER_TOWER) * static_cast<std::size_t>(TILEOP_SIZE));
    last_ingested_ = j;
}

bool Compositor::has_spanning() {
    std::unordered_set<uint32_t> inner_roots;
    inner_roots.reserve(inner_members_.size());
    for (uint32_t member : inner_members_) {
        inner_roots.insert(find(member));
    }

    for (uint32_t member : outer_members_) {
        if (inner_roots.count(find(member)) > 0U) {
            return true;
        }
    }

    return false;
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

    CompositorResult::Verdict verdict = CompositorResult::MOAT;
    for (uint32_t root : outer_roots) {
        if (inner_roots.count(root) > 0U) {
            verdict = CompositorResult::SPANNING;
            break;
        }
    }

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
    parent_[rb] = ra;
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
    for (int row = 0; row < TILES_PER_TOWER; ++row) {
        const uint32_t flat_idx = static_cast<uint32_t>(tile_index(*grid_, j, row));
        const uint8_t* tile_data = get_tile_data(flat_idx, tower_tileops, row, ext);
        const int budget = get_payload_budget(flat_idx, ext);
        assert_not_overflow(tile_data);

        uint8_t max_label = 0;
        if (!is_tile_dead(*grid_, j, row) && !is_dead(tile_data)) {
            max_label = max_group_label(tile_data, budget);
        }

        const uint32_t base = group_offset_[flat_idx];
        group_offset_[flat_idx + 1U] = base + static_cast<uint32_t>(max_label);
        for (uint32_t group = 0; group < static_cast<uint32_t>(max_label); ++group) {
            parent_.push_back(base + group);
        }
    }

    if (parent_.size() > 2000000000ULL) {
        const double avg = static_cast<double>(parent_.size()) /
                           static_cast<double>((static_cast<uint64_t>(j) + 1ULL) *
                                               static_cast<uint64_t>(TILES_PER_TOWER));
        std::fprintf(stderr, "OOM cap: parent_ size %zu at tower %d (%.1f groups/tile avg)\n",
                     parent_.size(), j, avg);
        std::abort();
    }
}

void Compositor::match_io_within_tower(int32_t j, const uint8_t* tower_tileops,
                                       const ExtendedTileSideTable* ext) {
    // Within a tower, adjacent rows share a b-direction boundary.
    // FACE_O (top, high b) of row r connects to FACE_I (bottom, low b) of row r+1.
    // I/O faces do NOT carry h1. Both tiles share the exact same boundary row,
    // so matching is by shared-prime identity: slot-by-slot positional pairing.
    for (int row = 0; row < TILES_PER_TOWER - 1; ++row) {
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

        // O-face of row r (top of tile = high b boundary)
        const FaceSlice o_slice = face_slice(top_data, FACE_O, top_budget);
        // I-face of row r+1 (bottom of tile = low b boundary)
        const FaceSlice i_slice = face_slice(bottom_data, FACE_I, bottom_budget);

        // Slot-by-slot positional matching up to min count.
        // Shared-prime identity guarantees port[s] on O-face and port[s]
        // on I-face refer to the same boundary primes.
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
    const uint32_t start = group_offset_[static_cast<uint32_t>(tile_index(*grid_, j, 0))];
    const uint32_t end = group_offset_[static_cast<uint32_t>(tile_index(*grid_, j, TILES_PER_TOWER - 1)) + 1U];
    for (uint32_t id = start; id < end; ++id) {
        (void)find(id);
    }
}

void Compositor::match_lr_with_previous(int32_t j, const uint8_t* tower_tileops,
                                        const ExtendedTileSideTable* ext) {
    // Between adjacent towers, the shared boundary is in the a-direction.
    // FACE_R (right, high a) of previous tower j-1 connects to FACE_L (left, low a)
    // of current tower j. L/R faces carry h1 (b-offset from tile origin).
    // h1-based matching is required because adjacent towers may be at different
    // base_y values (delta[j-1] = base_y[j-1] - base_y[j]).
    //
    // Row correspondence: d = delta[j-1] gives the b-offset.
    // q = d / TILE_SIDE whole-tile rows, f = d % TILE_SIDE fractional pixels.
    // Tiles stack upward (row 0 = bottom, row 31 = top). Previous tower
    // starts HIGHER (base_y[j-1] >= base_y[j]), so current row r at
    // absolute b ≈ base_y[j] + r*S maps to previous row r - q.
    // Current rows 0..q-1 have no matching tile in the previous tower.
    if (j <= 0) {
        return;
    }

    const int64_t d = (*grid_).delta[static_cast<std::size_t>(j - 1)];
    const int q = static_cast<int>(d / TILE_SIDE);
    const int f = static_cast<int>(d % TILE_SIDE);

    for (int row = 0; row < TILES_PER_TOWER; ++row) {
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

        // Current tower tile's L-face (left side, low a, faces previous tower)
        const FaceSlice l_slice = face_slice(cur_data, FACE_L, cur_budget);

        // Primary match: previous tower row (row - q)
        const int primary_prev_row = row - q;
        if (primary_prev_row >= 0 && primary_prev_row < TILES_PER_TOWER) {
            const uint32_t prev_flat = static_cast<uint32_t>(tile_index(*grid_, j - 1, primary_prev_row));
            if (!is_tile_dead(*grid_, j - 1, primary_prev_row)) {
                const uint8_t* prev_data = get_tile_data(prev_flat, prev_tower_tiles_, primary_prev_row, ext);
                const int prev_budget = get_payload_budget(prev_flat, ext);
                assert_not_overflow(prev_data);
                if (!is_dead(prev_data)) {
                    // Previous tower tile's R-face (right side, high a, faces current tower)
                    const FaceSlice r_slice = face_slice(prev_data, FACE_R, prev_budget);

                    // h1-based matching with delta offset.
                    // Match predicate: h1_l == h1_r + f
                    // (tower j-1 is shifted UP by f relative to current tower's tile origin)
                    for (uint8_t li = 0; li < l_slice.count; ++li) {
                        const uint8_t gl = decode_group_id(l_slice.groups[li]);
                        if (gl == 0U) continue;  // zero-padding
                        const uint16_t hl = decode_h1(l_slice.groups[li],
                                                       l_slice.h1_bytes[li]);

                        for (uint8_t ri = 0; ri < r_slice.count; ++ri) {
                            const uint8_t gr = decode_group_id(r_slice.groups[ri]);
                            if (gr == 0U) continue;  // zero-padding
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
            if (secondary_prev_row >= 0 && secondary_prev_row < TILES_PER_TOWER) {
                const uint32_t prev_flat =
                    static_cast<uint32_t>(tile_index(*grid_, j - 1, secondary_prev_row));
                if (!is_tile_dead(*grid_, j - 1, secondary_prev_row)) {
                    const uint8_t* prev_data =
                        get_tile_data(prev_flat, prev_tower_tiles_, secondary_prev_row, ext);
                    const int prev_budget = get_payload_budget(prev_flat, ext);
                    assert_not_overflow(prev_data);
                    if (!is_dead(prev_data)) {
                        const FaceSlice r_slice = face_slice(prev_data, FACE_R, prev_budget);

                        // Secondary match predicate: h1_l + (TILE_SIDE - f) == h1_r
                        // (tower j-1 at row-q-1 is one row below the primary in prev tower)
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
                const uint8_t group = decode_group_id(i_slice.groups[slot]);
                if (group > 0U) {
                    inner_members_.push_back(global_id(row0_flat, group));
                }
            }
        }
    }

    if (j <= 0) {
        return;
    }

    const int64_t d_prev = (*grid_).delta[static_cast<std::size_t>(j - 1)];
    const int q_prev = static_cast<int>(d_prev / TILE_SIDE);
    const int f_prev = static_cast<int>(d_prev % TILE_SIDE);

    if (q_prev > 0) {
        const int end_row = std::min(q_prev, TILES_PER_TOWER);
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
                inner_members_.push_back(global_id(flat, group));
            }
        }
    }

    if (f_prev > 0 && q_prev < TILES_PER_TOWER) {
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
                        inner_members_.push_back(global_id(flat, group));
                    }
                }
            }
        }
    }
}

void Compositor::collect_outer_boundary(int32_t j, const uint8_t* tower_tileops,
                                        const ExtendedTileSideTable* ext) {
    const int last_row = TILES_PER_TOWER - 1;
    const uint32_t row_last_flat = static_cast<uint32_t>(tile_index(*grid_, j, last_row));
    if (!is_tile_dead(*grid_, j, last_row)) {
        const uint8_t* row_last_data = get_tile_data(row_last_flat, tower_tileops, last_row, ext);
        const int row_last_budget = get_payload_budget(row_last_flat, ext);
        assert_not_overflow(row_last_data);
        if (!is_dead(row_last_data)) {
            const FaceSlice o_slice = face_slice(row_last_data, FACE_O, row_last_budget);
            for (uint8_t slot = 0; slot < o_slice.count; ++slot) {
                const uint8_t group = decode_group_id(o_slice.groups[slot]);
                if (group > 0U) {
                    outer_members_.push_back(global_id(row_last_flat, group));
                }
            }
        }
    }

    if (j >= grid_->num_towers - 1) {
        return;
    }

    const int64_t d = (*grid_).delta[static_cast<std::size_t>(j)];
    const int q = static_cast<int>(d / TILE_SIDE);
    const int f = static_cast<int>(d % TILE_SIDE);

    if (q > 0) {
        for (int row = TILES_PER_TOWER - q; row < TILES_PER_TOWER; ++row) {
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
                outer_members_.push_back(global_id(flat, group));
            }
        }
    }

    if (f > 0 && (last_row - q) >= 0) {
        const int row = last_row - q;
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
                    if (h >= static_cast<uint16_t>(TILE_SIDE - f)) {
                        outer_members_.push_back(global_id(flat, group));
                    }
                }
            }
        }
    }
}
