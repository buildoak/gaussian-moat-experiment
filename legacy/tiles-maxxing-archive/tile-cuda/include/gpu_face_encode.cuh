#pragma once

#include <cuda_runtime.h>
#include <stdint.h>

#include "gpu_constants.cuh"
#include "gpu_math.cuh"
#include "gpu_types.cuh"

namespace gpu_face_encode_detail {

struct FaceCountsGPU {
    uint8_t counts[NUM_FACES];
};

static __device__ __forceinline__ int count_faces(uint8_t mask) {
    return __popc(static_cast<unsigned int>(mask));
}

static __device__ __forceinline__ void decode_prime_pos(uint32_t packed, int* row, int* col) {
    *row = static_cast<int>(packed / SIDE_EXP);
    *col = static_cast<int>(packed % SIDE_EXP);
}

static __device__ __forceinline__ bool face_membership(
    int face, int tile_row, int tile_col, uint16_t* h, uint16_t* depth) {
    switch (face) {
        case FACE_I:
            if (tile_row < 0 || tile_row >= COLLAR) {
                return false;
            }
            *h = static_cast<uint16_t>(tile_col);
            *depth = static_cast<uint16_t>(tile_row);
            return true;
        case FACE_O:
            if (tile_row < (TILE_SIDE - COLLAR + 1) || tile_row > TILE_SIDE) {
                return false;
            }
            *h = static_cast<uint16_t>(tile_col);
            *depth = static_cast<uint16_t>(TILE_SIDE - tile_row);
            return true;
        case FACE_L:
            if (tile_col < 0 || tile_col >= COLLAR) {
                return false;
            }
            *h = static_cast<uint16_t>(tile_row);
            *depth = static_cast<uint16_t>(tile_col);
            return true;
        case FACE_R:
            if (tile_col < (TILE_SIDE - COLLAR + 1) || tile_col > TILE_SIDE) {
                return false;
            }
            *h = static_cast<uint16_t>(tile_row);
            *depth = static_cast<uint16_t>(TILE_SIDE - tile_col);
            return true;
        default:
            return false;
    }
}

static __device__ __forceinline__ void face_coords(
    int face, uint16_t h, uint16_t depth, int* tile_row, int* tile_col) {
    switch (face) {
        case FACE_I:
            *tile_row = depth;
            *tile_col = h;
            return;
        case FACE_O:
            *tile_row = TILE_SIDE - static_cast<int>(depth);
            *tile_col = h;
            return;
        case FACE_L:
            *tile_row = h;
            *tile_col = depth;
            return;
        case FACE_R:
            *tile_row = h;
            *tile_col = TILE_SIDE - static_cast<int>(depth);
            return;
        default:
            *tile_row = 0;
            *tile_col = 0;
            return;
    }
}

static __device__ __forceinline__ uint8_t encode_group_byte(uint8_t group_id, uint16_t h1) {
    return static_cast<uint8_t>(((h1 >> 8) << 7) | (group_id & 0x7Fu));
}

static __device__ __forceinline__ uint8_t encode_h1_byte(uint16_t h1) {
    return static_cast<uint8_t>(h1 & 0xFFu);
}

static __device__ FaceCountsGPU count_ports_by_face(const FaceDataGPU* face_data) {
    FaceCountsGPU face_counts{};
    for (int i = 0; i < face_data->port_count; ++i) {
        const int face = static_cast<int>(face_data->ports[i].face);
        if (face >= 0 && face < NUM_FACES) {
            ++face_counts.counts[face];
        }
    }
    return face_counts;
}

static __device__ void append_face_groups(TileOp* tileop, int* cursor, const FaceDataGPU* face_data, int face) {
    for (int i = 0; i < face_data->port_count; ++i) {
        const PortGPU& port = face_data->ports[i];
        if (port.face != face) {
            continue;
        }
        tileop->bytes[*cursor] = (face == FACE_L || face == FACE_R)
            ? encode_group_byte(static_cast<uint8_t>(port.group), port.h1)
            : static_cast<uint8_t>(port.group);
        ++(*cursor);
    }
}

static __device__ void append_face_h1(TileOp* tileop, int* cursor, const FaceDataGPU* face_data, int face) {
    for (int i = 0; i < face_data->port_count; ++i) {
        const PortGPU& port = face_data->ports[i];
        if (port.face != face) {
            continue;
        }
        tileop->bytes[*cursor] = encode_h1_byte(port.h1);
        ++(*cursor);
    }
}

static __device__ __forceinline__ GroupEntryGPU* find_group_entry(
    GroupEntryGPU* entries, uint16_t root) {
    const int start = static_cast<int>(root % MAX_GROUPS_GPU);
    for (int probe = 0; probe < MAX_GROUPS_GPU; ++probe) {
        GroupEntryGPU* entry = &entries[(start + probe) % MAX_GROUPS_GPU];
        if (entry->port_count == 0u || entry->root == root) {
            return entry;
        }
    }
    return nullptr;
}

}  // namespace gpu_face_encode_detail

// ---------------------------------------------------------------------------
// Per-face insertion sort (serial, one thread).  ~100-300 entries per face.
// Sort key: (h, depth) — matches the C++ reference (h primary, depth secondary,
// then tile_row/tile_col which we derive from face_coords).
// ---------------------------------------------------------------------------
namespace gpu_face_encode_detail {

static __device__ void sort_face_primes(FacePrimeGPU* list, int count, int face) {
    for (int i = 1; i < count; ++i) {
        const FacePrimeGPU key = list[i];
        int j = i - 1;
        while (j >= 0) {
            const FacePrimeGPU& prev = list[j];
            // Primary: h ascending
            if (prev.h < key.h) break;
            if (prev.h == key.h) {
                // Secondary: depth ascending
                if (prev.depth < key.depth) break;
                if (prev.depth == key.depth) {
                    // Tertiary: tile_row, tile_col (derive from face_coords)
                    int kr, kc, pr, pc;
                    face_coords(face, key.h, key.depth, &kr, &kc);
                    face_coords(face, prev.h, prev.depth, &pr, &pc);
                    if (pr < kr) break;
                    if (pr == kr && pc <= kc) break;
                }
            }
            list[j + 1] = list[j];
            --j;
        }
        list[j + 1] = key;
    }
}

}  // namespace gpu_face_encode_detail (sort helper)

// ---------------------------------------------------------------------------
// O(P) per-prime face extraction — replaces the old O(B x P) brute force.
//
// Stage 1: All 288 threads scan primes in stride. For each prime, O(1) check
//          against all 4 faces. Collect face primes via atomicAdd per face.
// Stage 2: Serial insertion sort per face (one thread per face, 4 threads).
// Stage 3: Linear gap scan per face → emit raw ports.
// ---------------------------------------------------------------------------
__device__ void extract_faces_gpu_parallel(
    const uint32_t* prime_pos,
    int prime_count,
    const uint16_t* parent,
    FacePrimeGPU* face_prime_lists,
    uint32_t* face_prime_counts,
    FaceScratchGPU* scratch,
    int tid) {

    // --- Zero scratch and face_prime_counts ---
    const int scratch_words =
        static_cast<int>((sizeof(FaceScratchGPU) + sizeof(uint32_t) - 1) / sizeof(uint32_t));
    uint32_t* const scratch_words_ptr = reinterpret_cast<uint32_t*>(scratch);
    for (int i = tid; i < scratch_words; i += BLOCK_THREADS) {
        scratch_words_ptr[i] = 0u;
    }
    if (tid < NUM_FACES) {
        face_prime_counts[tid] = 0u;
    }
    __syncthreads();

    // ===== STAGE 1: Collect face primes (all 288 threads) =====
    const int bounded_prime_count = prime_count < MAX_PRIMES_GPU ? prime_count : MAX_PRIMES_GPU;

    for (int i = tid; i < bounded_prime_count; i += BLOCK_THREADS) {
        int row = 0;
        int col = 0;
        gpu_face_encode_detail::decode_prime_pos(prime_pos[i], &row, &col);

        const int tile_row = row - COLLAR;
        const int tile_col = col - COLLAR;
        if (tile_row < 0 || tile_row > TILE_SIDE || tile_col < 0 || tile_col > TILE_SIDE) {
            continue;
        }

        const uint16_t prime_root = parent[i];

        // Check all 4 faces — a prime near a corner can belong to 2 faces
        #pragma unroll
        for (int face = 0; face < NUM_FACES; ++face) {
            uint16_t h = 0;
            uint16_t depth = 0;
            if (gpu_face_encode_detail::face_membership(face, tile_row, tile_col, &h, &depth)) {
                const uint32_t slot = atomicAdd(&face_prime_counts[face], 1u);
                if (slot < MAX_FACE_PRIMES_PER_FACE) {
                    FacePrimeGPU& fp = face_prime_lists[face * MAX_FACE_PRIMES_PER_FACE + slot];
                    fp.h = h;
                    fp.root = prime_root;
                    fp.depth = static_cast<uint8_t>(depth);
                    fp.pad1 = 0;
                    fp.pad2 = 0;
                }
            }
        }
    }
    __syncthreads();

    // Clamp counts to capacity
    if (tid < NUM_FACES) {
        if (face_prime_counts[tid] > MAX_FACE_PRIMES_PER_FACE) {
            face_prime_counts[tid] = MAX_FACE_PRIMES_PER_FACE;
        }
    }
    __syncthreads();

    // ===== STAGE 2: Sort face primes by (h, depth) per face =====
    // 4 threads each sort one face — serial insertion sort, ~100-300 entries
    if (tid < NUM_FACES) {
        const int face = tid;
        const int count = static_cast<int>(face_prime_counts[face]);
        FacePrimeGPU* list = &face_prime_lists[face * MAX_FACE_PRIMES_PER_FACE];
        gpu_face_encode_detail::sort_face_primes(list, count, face);
    }
    __syncthreads();

    // ===== STAGE 3: Port detection via gap test (one thread per face) =====
    if (tid < NUM_FACES) {
        const int face = tid;
        const int count = static_cast<int>(face_prime_counts[face]);
        const FacePrimeGPU* list = &face_prime_lists[face * MAX_FACE_PRIMES_PER_FACE];

        int port_count = 0;
        bool have_prev = false;
        int prev_row = 0;
        int prev_col = 0;

        for (int j = 0; j < count; ++j) {
            int tile_row = 0;
            int tile_col = 0;
            gpu_face_encode_detail::face_coords(
                face, list[j].h, static_cast<uint16_t>(list[j].depth), &tile_row, &tile_col);

            bool start_port = false;
            if (!have_prev) {
                start_port = true;
            } else {
                const int dx = tile_col - prev_col;
                const int dy = tile_row - prev_row;
                start_port = (dx * dx + dy * dy) > K_SQ;
            }

            if (start_port) {
                const int slot = face * MAX_FACE_PORTS_GPU + port_count;
                if (port_count < MAX_FACE_PORTS_GPU && slot < MAX_TOTAL_PORTS_GPU) {
                    RawPortGPU& out = scratch->raw_ports[slot];
                    out.root = list[j].root;
                    out.h1 = list[j].h;
                    out.face = static_cast<uint8_t>(face);
                    out.live = 1u;
                    out.pad = 0u;
                }
                ++port_count;
            }

            prev_row = tile_row;
            prev_col = tile_col;
            have_prev = true;
        }

        if (port_count > MAX_FACE_PORTS_GPU) {
            scratch->overflow = 1u;
        }
        scratch->face_port_counts[face] = static_cast<uint16_t>(port_count);
    }
    __syncthreads();

    // Sum face port counts
    if (tid == 0) {
        scratch->raw_port_count = 0u;
        for (int face = 0; face < NUM_FACES; ++face) {
            scratch->raw_port_count = static_cast<uint16_t>(
                scratch->raw_port_count + scratch->face_port_counts[face]);
        }
        if (scratch->raw_port_count > MAX_TOTAL_PORTS_GPU) {
            scratch->overflow = 1u;
        }
    }
    __syncthreads();
}

__device__ void prune_dead_ends_gpu(
    FaceScratchGPU* scratch,
    FaceDataGPU* face_data,
    int tid) {
    const int warp_id = tid / WARP_SIZE;
    const int lane = tid & (WARP_SIZE - 1);

    if (warp_id == 4) {
        for (int i = lane; i < MAX_GROUPS_GPU; i += WARP_SIZE) {
            scratch->group_entries[i].root = 0u;
            scratch->group_entries[i].face_mask = 0u;
            scratch->group_entries[i].port_count = 0u;
            scratch->group_entries[i].new_group = 0u;
            scratch->group_entries[i].dead = 0u;
            scratch->group_entries[i].pad = 0u;
        }
    }
    __syncthreads();

    if (warp_id == 4 && lane == 0) {
        face_data->port_count = 0;
        face_data->group_count = 0;
        for (int i = 0; i < MAX_PORTS_GPU; ++i) {
            face_data->ports[i].face = 0u;
            face_data->ports[i].group = 0u;
            face_data->ports[i].h1 = 0u;
        }

        if (scratch->overflow != 0u) {
            face_data->group_count = MAX_GROUPS_GPU + 1;
            return;
        }

        for (int face = 0; face < NUM_FACES; ++face) {
            const int base = face * MAX_FACE_PORTS_GPU;
            const int count = scratch->face_port_counts[face];
            for (int i = 0; i < count; ++i) {
                const RawPortGPU& raw = scratch->raw_ports[base + i];
                GroupEntryGPU* entry =
                    gpu_face_encode_detail::find_group_entry(scratch->group_entries, raw.root);
                if (entry == nullptr) {
                    face_data->group_count = MAX_GROUPS_GPU + 1;
                    return;
                }
                if (entry->port_count == 0u) {
                    entry->root = raw.root;
                }
                entry->face_mask = static_cast<uint8_t>(entry->face_mask | (1u << raw.face));
                entry->port_count = static_cast<uint8_t>(entry->port_count + 1u);
            }
        }

        for (int i = 0; i < MAX_GROUPS_GPU; ++i) {
            GroupEntryGPU& entry = scratch->group_entries[i];
            if (entry.port_count == 1u &&
                gpu_face_encode_detail::count_faces(entry.face_mask) == 1) {
                entry.dead = 1u;
            }
        }

        int next_group = 1;
        for (int face = 0; face < NUM_FACES && face_data->port_count < MAX_PORTS_GPU; ++face) {
            const int base = face * MAX_FACE_PORTS_GPU;
            const int count = scratch->face_port_counts[face];
            for (int i = 0; i < count && face_data->port_count < MAX_PORTS_GPU; ++i) {
                const RawPortGPU& raw = scratch->raw_ports[base + i];
                GroupEntryGPU* entry =
                    gpu_face_encode_detail::find_group_entry(scratch->group_entries, raw.root);
                if (entry == nullptr || entry->dead != 0u) {
                    continue;
                }
                if (entry->new_group == 0u) {
                    if (next_group > MAX_GROUPS_GPU) {
                        face_data->group_count = MAX_GROUPS_GPU + 1;
                        return;
                    }
                    entry->new_group = static_cast<uint8_t>(next_group++);
                }

                PortGPU& out = face_data->ports[face_data->port_count++];
                out.face = raw.face;
                out.group = entry->new_group;
                out.h1 = raw.h1;
            }
        }
        face_data->group_count = next_group - 1;
    }
    __syncthreads();
}

__device__ void encode_tileop_gpu(const FaceDataGPU* face_data, TileOp* output) {
    for (int i = 0; i < TILEOP_SIZE; ++i) {
        output->bytes[i] = 0;
    }
    output->bytes[0] = EMPTY_OFFSET;
    output->bytes[1] = EMPTY_OFFSET;
    output->bytes[2] = EMPTY_OFFSET;

    // L/R face encoding steals bit 7 of the group byte for the h1 MSB,
    // leaving only 7 bits (0-127) for the group ID.  Tiles with >= 128
    // groups cannot represent all L/R group labels -- poison the TileOp.
    if (face_data->group_count > 127) {
        for (int i = 0; i < TILEOP_SIZE; ++i) {
            output->bytes[i] = OVERFLOW_SENTINEL;
        }
        return;
    }

    const gpu_face_encode_detail::FaceCountsGPU face_counts =
        gpu_face_encode_detail::count_ports_by_face(face_data);
    const int o_cnt = face_counts.counts[FACE_O];
    const int i_cnt = face_counts.counts[FACE_I];
    const int l_cnt = face_counts.counts[FACE_L];
    const int r_cnt = face_counts.counts[FACE_R];

    if (o_cnt + i_cnt + 2 * l_cnt + 2 * r_cnt > TILEOP_PAYLOAD_BYTES) {
        for (int i = 0; i < TILEOP_SIZE; ++i) {
            output->bytes[i] = OVERFLOW_SENTINEL;
        }
        return;
    }

    output->bytes[0] = static_cast<uint8_t>(TILEOP_HEADER_BYTES + o_cnt);
    output->bytes[1] = static_cast<uint8_t>(TILEOP_HEADER_BYTES + o_cnt + i_cnt);
    output->bytes[2] = static_cast<uint8_t>(TILEOP_HEADER_BYTES + o_cnt + i_cnt + l_cnt);

    const int off_r = TILEOP_HEADER_BYTES + o_cnt + i_cnt + l_cnt;
    const int derived_r_cnt = (TILEOP_SIZE - off_r - l_cnt) / 2;
    const int h_start = off_r + derived_r_cnt;

    int cursor = TILEOP_HEADER_BYTES;
    gpu_face_encode_detail::append_face_groups(output, &cursor, face_data, FACE_O);
    gpu_face_encode_detail::append_face_groups(output, &cursor, face_data, FACE_I);
    gpu_face_encode_detail::append_face_groups(output, &cursor, face_data, FACE_L);
    gpu_face_encode_detail::append_face_groups(output, &cursor, face_data, FACE_R);

    cursor = h_start;
    gpu_face_encode_detail::append_face_h1(output, &cursor, face_data, FACE_L);
    gpu_face_encode_detail::append_face_h1(output, &cursor, face_data, FACE_R);
}
