#pragma once

#include <cuda_runtime.h>
#include <stdint.h>

#include "gpu_constants.cuh"
#include "gpu_math.cuh"
#include "gpu_types.cuh"

namespace gpu_face_encode_detail {

struct FacePrimeGPU {
    uint16_t uf_index;
    uint16_t h;
    uint16_t tile_row;
    uint16_t tile_col;
};

struct TempPortGPU {
    uint16_t component_root;
    uint16_t h1;
};

struct FaceCountsGPU {
    uint8_t counts[NUM_FACES];
};

static __device__ __forceinline__ int count_faces(uint8_t mask) {
    int count = 0;
    while (mask != 0u) {
        count += static_cast<int>(mask & 1u);
        mask = static_cast<uint8_t>(mask >> 1);
    }
    return count;
}

static __device__ __forceinline__ uint16_t boundary_depth(int face, uint16_t tile_row, uint16_t tile_col) {
    switch (face) {
        case FACE_I:
            return tile_row;
        case FACE_O:
            return static_cast<uint16_t>(TILE_SIDE - static_cast<int>(tile_row));
        case FACE_L:
            return tile_col;
        case FACE_R:
            return static_cast<uint16_t>(TILE_SIDE - static_cast<int>(tile_col));
        default:
            return 0;
    }
}

static __device__ __forceinline__ bool face_prime_less(
    const FacePrimeGPU& lhs, const FacePrimeGPU& rhs, int face) {
    if (lhs.h != rhs.h) {
        return lhs.h < rhs.h;
    }

    const uint16_t lhs_depth = boundary_depth(face, lhs.tile_row, lhs.tile_col);
    const uint16_t rhs_depth = boundary_depth(face, rhs.tile_row, rhs.tile_col);
    if (lhs_depth != rhs_depth) {
        return lhs_depth < rhs_depth;
    }
    if (lhs.tile_row != rhs.tile_row) {
        return lhs.tile_row < rhs.tile_row;
    }
    return lhs.tile_col < rhs.tile_col;
}

static __device__ void sort_face_primes(FacePrimeGPU* primes, int count, int face) {
    for (int i = 1; i < count; ++i) {
        const FacePrimeGPU key = primes[i];
        int j = i - 1;
        while (j >= 0 && face_prime_less(key, primes[j], face)) {
            primes[j + 1] = primes[j];
            --j;
        }
        primes[j + 1] = key;
    }
}

static __device__ __forceinline__ int face_encode_uf_index(
    int row, int col, const uint32_t* bitmap, const uint32_t* row_prefix) {
    uint32_t idx = row_prefix[row];
    const int full_words = col >> 5;
    for (int w = 0; w < full_words; ++w) {
        idx += __popc(bitmap[row * BITMAP_WORDS_PER_ROW + w]);
    }

    const uint32_t bit_mask = (col & 31) == 0 ? 0u : ((1u << (col & 31)) - 1u);
    idx += __popc(bitmap[row * BITMAP_WORDS_PER_ROW + full_words] & bit_mask);
    return static_cast<int>(idx);
}

static __device__ int collect_face_primes(
    int face,
    const uint32_t* bitmap,
    const uint32_t* row_prefix,
    const uint32_t* prime_pos,
    int prime_count,
    FacePrimeGPU* out_primes) {
    int count = 0;
    const int bounded_prime_count = prime_count < MAX_PRIMES_GPU ? prime_count : MAX_PRIMES_GPU;

    for (int i = 0; i < bounded_prime_count; ++i) {
        const uint32_t packed = prime_pos[i];
        const int row = static_cast<int>(packed >> 16);
        const int col = static_cast<int>(packed & 0xFFFFu);
        const int tile_row = row - COLLAR;
        const int tile_col = col - COLLAR;

        if (tile_row < 0 || tile_row > TILE_SIDE || tile_col < 0 || tile_col > TILE_SIDE) {
            continue;
        }

        bool on_face = false;
        uint16_t h = 0;
        switch (face) {
            case FACE_I:
                on_face = tile_row < COLLAR;
                h = static_cast<uint16_t>(tile_col);
                break;
            case FACE_O:
                on_face = tile_row >= (TILE_SIDE - COLLAR + 1);
                h = static_cast<uint16_t>(tile_col);
                break;
            case FACE_L:
                on_face = tile_col < COLLAR;
                h = static_cast<uint16_t>(tile_row);
                break;
            case FACE_R:
                on_face = tile_col >= (TILE_SIDE - COLLAR + 1);
                h = static_cast<uint16_t>(tile_row);
                break;
            default:
                break;
        }

        if (!on_face) {
            continue;
        }

        out_primes[count].uf_index = static_cast<uint16_t>(face_encode_uf_index(row, col, bitmap, row_prefix));
        out_primes[count].h = h;
        out_primes[count].tile_row = static_cast<uint16_t>(tile_row);
        out_primes[count].tile_col = static_cast<uint16_t>(tile_col);
        ++count;
    }

    sort_face_primes(out_primes, count, face);
    return count;
}

static __device__ int cluster_face_ports(
    int face,
    const FacePrimeGPU* face_primes,
    int face_prime_count,
    const uint32_t* component_parent,
    TempPortGPU* out_ports) {
    if (face_prime_count == 0) {
        return 0;
    }

    int port_count = 0;
    if (port_count >= MAX_PORTS_GPU) {
        return port_count;
    }
    out_ports[port_count].component_root =
        static_cast<uint16_t>(component_parent[face_primes[0].uf_index]);
    out_ports[port_count].h1 = face_primes[0].h;
    ++port_count;

    for (int i = 1; i < face_prime_count; ++i) {
        const int dx = static_cast<int>(face_primes[i].tile_col) -
                       static_cast<int>(face_primes[i - 1].tile_col);
        const int dy = static_cast<int>(face_primes[i].tile_row) -
                       static_cast<int>(face_primes[i - 1].tile_row);
        if (dx * dx + dy * dy > K_SQ) {
            if (port_count >= MAX_PORTS_GPU) {
                return port_count;
            }
            out_ports[port_count].component_root =
                static_cast<uint16_t>(component_parent[face_primes[i].uf_index]);
            out_ports[port_count].h1 = face_primes[i].h;
            ++port_count;
        }
    }

    (void)face;
    return port_count;
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

}  // namespace gpu_face_encode_detail

__device__ void extract_faces_gpu(
    const uint32_t* bitmap,
    const uint32_t* row_prefix,
    const uint32_t* prime_pos,
    int prime_count,
    const uint32_t* parent,
    FaceDataGPU* face_data) {
    for (int i = 0; i < MAX_PORTS_GPU; ++i) {
        face_data->ports[i].face = 0;
        face_data->ports[i].group = 0;
        face_data->ports[i].h1 = 0;
    }
    face_data->port_count = 0;
    face_data->group_count = 0;

    uint16_t root_to_group[MAX_PRIMES_GPU];
    for (int i = 0; i < MAX_PRIMES_GPU; ++i) {
        root_to_group[i] = 0;
    }

    gpu_face_encode_detail::FacePrimeGPU face_primes[MAX_PRIMES_GPU];
    gpu_face_encode_detail::TempPortGPU face_ports[MAX_PORTS_GPU];
    int next_group = 1;

    for (int face = 0; face < NUM_FACES; ++face) {
        const int face_prime_count = gpu_face_encode_detail::collect_face_primes(
            face, bitmap, row_prefix, prime_pos, prime_count, face_primes);
        const int face_port_count = gpu_face_encode_detail::cluster_face_ports(
            face, face_primes, face_prime_count, parent, face_ports);

        for (int i = 0; i < face_port_count && face_data->port_count < MAX_PORTS_GPU; ++i) {
            const int component_root = static_cast<int>(face_ports[i].component_root);
            if (component_root < 0 || component_root >= MAX_PRIMES_GPU) {
                continue;
            }
            if (root_to_group[component_root] == 0) {
                root_to_group[component_root] = static_cast<uint16_t>(next_group++);
            }

            PortGPU& port = face_data->ports[face_data->port_count++];
            port.face = static_cast<uint8_t>(face);
            port.group = static_cast<uint8_t>(root_to_group[component_root]);
            port.h1 = face_ports[i].h1;
        }
    }

    face_data->group_count = next_group - 1;
}

__device__ void prune_dead_ends_gpu(FaceDataGPU* face_data) {
    FaceDataGPU pruned;
    for (int i = 0; i < MAX_PORTS_GPU; ++i) {
        pruned.ports[i].face = 0;
        pruned.ports[i].group = 0;
        pruned.ports[i].h1 = 0;
    }
    pruned.port_count = 0;
    pruned.group_count = 0;

    uint8_t group_faces[MAX_PORTS_GPU + 1];
    uint16_t group_ports[MAX_PORTS_GPU + 1];
    uint8_t dead_end[MAX_PORTS_GPU + 1];
    int remap[MAX_PORTS_GPU + 1];
    for (int i = 0; i <= MAX_PORTS_GPU; ++i) {
        group_faces[i] = 0;
        group_ports[i] = 0;
        dead_end[i] = 0;
        remap[i] = 0;
    }

    for (int i = 0; i < face_data->port_count; ++i) {
        const PortGPU& port = face_data->ports[i];
        if (port.group == 0) {
            continue;
        }
        group_faces[port.group] = static_cast<uint8_t>(group_faces[port.group] | (1u << port.face));
        ++group_ports[port.group];
    }

    for (int group = 1; group <= face_data->group_count && group <= MAX_PORTS_GPU; ++group) {
        if (group_ports[group] == 1u && gpu_face_encode_detail::count_faces(group_faces[group]) == 1) {
            dead_end[group] = 1;
        }
    }

    int next_group = 1;
    for (int i = 0; i < face_data->port_count && pruned.port_count < MAX_PORTS_GPU; ++i) {
        const PortGPU& port = face_data->ports[i];
        if (port.group == 0 || dead_end[port.group] != 0) {
            continue;
        }
        if (remap[port.group] == 0) {
            remap[port.group] = next_group++;
        }

        PortGPU& out = pruned.ports[pruned.port_count++];
        out = port;
        out.group = static_cast<uint8_t>(remap[port.group]);
    }

    pruned.group_count = next_group - 1;
    *face_data = pruned;
}

__device__ void encode_tileop_gpu(const FaceDataGPU* face_data, TileOp* output) {
    for (int i = 0; i < TILEOP_SIZE; ++i) {
        output->bytes[i] = 0;
    }
    output->bytes[0] = EMPTY_OFFSET;
    output->bytes[1] = EMPTY_OFFSET;
    output->bytes[2] = EMPTY_OFFSET;

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
