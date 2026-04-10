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
    uint16_t p;       // split prime, < 10000
    uint16_t root;    // sqrt(-1) mod p, < p
    uint32_t mu;      // floor(2^32 / p)
};
static_assert(sizeof(SplitPrimeBarrettGPU) == 8, "Barrett split prime must be 8 bytes");

struct InertPrimeBarrettGPU {
    uint16_t p;       // inert prime, < 10000
    uint16_t pad;     // alignment padding
    uint32_t mu;      // floor(2^32 / p)
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

static_assert(sizeof(TileOp) == TILEOP_SIZE, "TileOp must stay 128 bytes");
static_assert(sizeof(PortGPU) == 4, "PortGPU ABI changed");
static_assert(sizeof(FaceCellGPU) == 16, "FaceCellGPU ABI changed");
static_assert(sizeof(FacePrimeGPU) == 8, "FacePrimeGPU ABI changed");
static_assert(sizeof(RawPortGPU) == 8, "RawPortGPU ABI changed");
static_assert(sizeof(GroupEntryGPU) == 8, "GroupEntryGPU ABI changed");
