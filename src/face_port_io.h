#pragma once

#include <cstdint>

// Input: Job manifest
struct FatStripeJobHeader {    // 24 bytes
    uint8_t  magic[4];         // "GMTJ"
    uint16_t version;          // 1
    uint16_t flags;            // 0
    uint64_t k_sq;
    uint32_t tile_side;
    uint32_t num_jobs;
};

struct TileJob {               // 16 bytes
    uint32_t tile_id;
    int32_t  a_lo;
    int32_t  b_lo;
    uint32_t reserved;         // 0
};

// Output: Face port stream
struct FacePortStreamHeader {  // 32 bytes
    uint8_t  magic[4];         // "GMFP"
    uint16_t version;          // 1
    uint16_t flags;            // 0
    uint64_t k_sq;
    uint32_t tile_side;
    uint32_t num_tiles;
    uint64_t reserved;         // 0
};

struct TileResultHeader {      // 44 bytes
    uint32_t tile_id;
    int32_t  a_lo;
    int32_t  b_lo;
    uint32_t side;
    uint32_t num_components;
    uint32_t num_face_inner;
    uint32_t num_face_outer;
    uint32_t num_face_left;
    uint32_t num_face_right;
    uint32_t num_primes;
    int32_t  origin_component; // -1 if origin not in this tile
};

struct FacePortRecord {        // 12 bytes
    int32_t  a;                // absolute real coordinate
    int32_t  b;                // absolute imaginary coordinate
    uint32_t component_id;     // tile-local component ID
};

static_assert(sizeof(FatStripeJobHeader) == 24, "FatStripeJobHeader size mismatch");
static_assert(sizeof(TileJob) == 16, "TileJob size mismatch");
static_assert(sizeof(FacePortStreamHeader) == 32, "FacePortStreamHeader size mismatch");
static_assert(sizeof(TileResultHeader) == 44, "TileResultHeader size mismatch");
static_assert(sizeof(FacePortRecord) == 12, "FacePortRecord size mismatch");
