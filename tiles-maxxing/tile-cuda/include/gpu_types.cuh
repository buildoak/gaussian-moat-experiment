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

struct FaceDataGPU {
    PortGPU ports[MAX_PORTS_GPU];
    int port_count;
    int group_count;
};

struct SieveTables {
    uint32_t split_table[SPLIT_PRIMES_COUNT];
    uint16_t inert_primes[INERT_PRIMES_COUNT];
    int split_count;
    int inert_count;
};

struct TileResult {
    TileOp tileop;
    uint32_t prime_count;
    uint32_t group_count;
};

static_assert(sizeof(TileOp) == TILEOP_SIZE, "TileOp must stay 128 bytes");
static_assert(sizeof(PortGPU) == 4, "PortGPU ABI changed");
