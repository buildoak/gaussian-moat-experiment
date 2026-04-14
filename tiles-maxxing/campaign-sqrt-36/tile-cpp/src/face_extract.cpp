#include "face_extract.h"

#include "compact.h"
#include "constants.h"

#include <cassert>
#include <cstring>

namespace {

struct FacePrime {
    uint16_t uf_index;
    uint16_t h;
    uint16_t tile_row;
    uint16_t tile_col;
};

struct TempPort {
    uint16_t component_root;
    uint16_t h1;
};

inline uint16_t boundary_depth(int face, uint16_t tile_row, uint16_t tile_col) {
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

inline bool face_prime_less(const FacePrime& lhs, const FacePrime& rhs, int face) {
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

void sort_face_primes(FacePrime* primes, int count, int face) {
    for (int i = 1; i < count; ++i) {
        const FacePrime key = primes[i];
        int j = i - 1;
        while (j >= 0 && face_prime_less(key, primes[j], face)) {
            primes[j + 1] = primes[j];
            --j;
        }
        primes[j + 1] = key;
    }
}

void collect_face_primes(int face,
                         const uint32_t* bitmap,
                         const uint32_t* prefix,
                         const uint32_t* prime_pos,
                         int prime_count,
                         FacePrime* out_primes,
                         int* out_count) {
    int count = 0;
    for (int i = 0; i < prime_count; ++i) {
        const uint32_t pos = prime_pos[i];
        const int row = static_cast<int>(pos / SIDE_EXP);
        const int col = static_cast<int>(pos % SIDE_EXP);
        const int tile_row = row - COLLAR;
        const int tile_col = col - COLLAR;

        if (tile_row < 0 || tile_row > TILE_SIDE || tile_col < 0 || tile_col > TILE_SIDE) {
            continue;
        }

        bool on_face = false;
        uint16_t h = 0;
        switch (face) {
            case FACE_I:
                on_face = tile_row <= COLLAR;
                h = static_cast<uint16_t>(tile_col);
                break;
            case FACE_O:
                on_face = tile_row >= TILE_SIDE - COLLAR;
                h = static_cast<uint16_t>(tile_col);
                break;
            case FACE_L:
                on_face = tile_col <= COLLAR;
                h = static_cast<uint16_t>(tile_row);
                break;
            case FACE_R:
                on_face = tile_col >= TILE_SIDE - COLLAR;
                h = static_cast<uint16_t>(tile_row);
                break;
            default:
                break;
        }

        if (!on_face) {
            continue;
        }

        assert(count < MAX_PRIMES);
        out_primes[count].uf_index = static_cast<uint16_t>(bitmap_pos_to_uf_index(pos, bitmap, prefix));
        out_primes[count].h = h;
        out_primes[count].tile_row = static_cast<uint16_t>(tile_row);
        out_primes[count].tile_col = static_cast<uint16_t>(tile_col);
        ++count;
    }

    sort_face_primes(out_primes, count, face);
    *out_count = count;
}

// Cluster face primes into ports by scanning consecutive pairs in h-sorted order.
// Two consecutive primes belong to the same port iff their squared 2D distance <= K_SQ.
// Transitivity is handled naturally: A-B merge and B-C merge => A,B,C are one port.
int cluster_face_ports(int face,
                       const FacePrime* face_primes,
                       int face_prime_count,
                       const uint16_t* component_parent,
                       TempPort* out_ports) {
    if (face_prime_count == 0) {
        return 0;
    }

    int port_count = 0;

    // First prime starts a new port
    if (port_count >= MAX_PORTS) {
        return port_count;
    }
    out_ports[port_count].component_root = component_parent[face_primes[0].uf_index];
    out_ports[port_count].h1 = face_primes[0].h;
    ++port_count;

    for (int i = 1; i < face_prime_count; ++i) {
        const int dx = static_cast<int>(face_primes[i].tile_col) - static_cast<int>(face_primes[i - 1].tile_col);
        const int dy = static_cast<int>(face_primes[i].tile_row) - static_cast<int>(face_primes[i - 1].tile_row);
        if (dx * dx + dy * dy > K_SQ) {
            // Gap too large — start a new port
            if (port_count >= MAX_PORTS) {
                return port_count;
            }
            out_ports[port_count].component_root = component_parent[face_primes[i].uf_index];
            out_ports[port_count].h1 = face_primes[i].h;
            ++port_count;
        }
    }

    (void)face;
    return port_count;
}

}  // namespace

FaceData extract_faces(const TileCoord& coord,
                       const uint32_t* bitmap, const uint32_t* prefix,
                       const uint32_t* prime_pos, int prime_count,
                       const uint16_t* parent) {
    (void)coord;

    FaceData result;
    std::memset(&result, 0, sizeof(result));

    int root_to_group[MAX_PRIMES];
    std::memset(root_to_group, 0, sizeof(root_to_group));

    FacePrime face_primes[MAX_PRIMES];
    TempPort face_ports[MAX_PORTS];
    int next_group = 1;

    for (int face = 0; face < NUM_FACES; ++face) {
        int face_prime_count = 0;
        collect_face_primes(face, bitmap, prefix, prime_pos, prime_count, face_primes, &face_prime_count);

        const int face_port_count = cluster_face_ports(face, face_primes, face_prime_count, parent, face_ports);
        for (int i = 0; i < face_port_count && result.port_count < MAX_PORTS; ++i) {
            const int component_root = static_cast<int>(face_ports[i].component_root);
            assert(component_root >= 0 && component_root < MAX_PRIMES);
            if (root_to_group[component_root] == 0) {
                root_to_group[component_root] = next_group++;
            }

            Port& port = result.ports[result.port_count++];
            port.face = face;
            port.group = root_to_group[component_root];
            port.h1 = face_ports[i].h1;
        }
    }

    result.group_count = next_group - 1;
    return result;
}
