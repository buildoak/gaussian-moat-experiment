#pragma once

#include "gpu_constants.cuh"

struct TileCoord {
    int64_t a_lo;
    int64_t b_lo;
};

struct TileOp {
    uint8_t bytes[TILEOP_SIZE];
};

struct PortGPU {
    uint8_t face;
    uint8_t group;
    uint16_t h1;
};

struct FaceCellGPU {
    uint16_t roots[7];
    uint8_t mask;
    uint8_t pad;
};

struct FacePrimeGPU {
    uint16_t h;
    uint16_t root;
    uint8_t depth;
    uint8_t pad1;
    uint16_t pad2;
};

struct RawPortGPU {
    uint16_t root;
    uint16_t h1;
    uint8_t face;
    uint8_t live;
    uint16_t pad;
};

struct GroupEntryGPU {
    uint16_t root;
    uint8_t face_mask;
    uint8_t port_count;
    uint8_t new_group;
    uint8_t dead;
    uint16_t pad;
};

struct FaceScratchGPU {
    RawPortGPU raw_ports[MAX_TOTAL_PORTS_GPU];
    GroupEntryGPU group_entries[MAX_GROUPS_GPU];
    uint16_t face_port_counts[NUM_FACES];
    uint16_t raw_port_count;
    uint16_t overflow;
};

struct FaceDataGPU {
    PortGPU ports[MAX_PORTS_GPU];
    int port_count;
    int group_count;
};

struct SplitPrimeBarrettGPU {
    uint16_t p;
    uint16_t root;
    uint32_t mu;
};
static_assert(sizeof(SplitPrimeBarrettGPU) == 8, "Barrett split prime must be 8 bytes");

struct InertPrimeBarrettGPU {
    uint16_t p;
    uint16_t pad;
    uint32_t mu;
};
static_assert(sizeof(InertPrimeBarrettGPU) == 8, "Barrett inert prime must be 8 bytes");

struct SieveTablesBarrett {
    SplitPrimeBarrettGPU split_table[SPLIT_PRIMES_COUNT];
    InertPrimeBarrettGPU inert_primes[INERT_PRIMES_COUNT];
    int split_count;
    int inert_count;
};

struct TileResult {
    TileOp tileop;
    uint32_t prime_count;
    uint32_t group_count;
};

struct PhaseTimingGPU {
    int64_t phase1a_cycles;
    int64_t phase1b_cycles;
    int64_t phase1c_cycles;
    int64_t phase2_cycles;
    int64_t phase3_cycles;
    int64_t phase45_cycles;
    int64_t total_cycles;
    int32_t tile_idx;
    int32_t prime_count;
};

// Inter-kernel batch buffers
struct TileBatchBuffers {
    TileCoord*  d_coords;
    uint32_t*   d_cand_list;     // [N * MAX_CANDIDATES_GPU] K1->K2
    uint32_t*   d_total_cands;   // [N] K1->K2
    uint32_t*   d_bitmap;        // [N * BITMAP_WORDS] K2->K3,K4
    uint16_t*   d_row_prefix;    // [N * (ACTIVE_ROWS+1)] K3->K4,K5
    uint32_t*   d_prime_pos;     // [N * MAX_PRIMES_GPU] K3->K4,K5
    uint32_t*   d_prime_count;   // [N] K3->K4,K5
    uint16_t*   d_parent;        // [N * MAX_PRIMES_GPU] K4->K5
    TileOp*     d_output;        // [N] K5->host
    uint32_t*   d_prime_counts_out; // [N] K5->host
};

static_assert(sizeof(TileOp) == 256, "TileOp must be 256 bytes");
static_assert(TILEOP_SIZE == TILEOP_HEADER_BYTES + TILEOP_PAYLOAD_BYTES,
              "TILEOP_SIZE must equal header + payload");
static_assert(MAX_TOTAL_PORTS_GPU >= 4 * MAX_FACE_PORTS_GPU,
              "MAX_TOTAL_PORTS_GPU must hold all 4 faces at max capacity");
static_assert(sizeof(PortGPU) == 4, "PortGPU ABI changed");
static_assert(sizeof(FaceCellGPU) == 16, "FaceCellGPU ABI changed");
static_assert(sizeof(FacePrimeGPU) == 8, "FacePrimeGPU ABI changed");
static_assert(sizeof(RawPortGPU) == 8, "RawPortGPU ABI changed");
static_assert(sizeof(GroupEntryGPU) == 8, "GroupEntryGPU ABI changed");
