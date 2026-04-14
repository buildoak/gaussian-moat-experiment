#pragma once
#include "constants.h"
#include <cstdint>
#include <cstring>

struct TileCoord {
    int64_t a_lo;  // tile origin, real axis
    int64_t b_lo;  // tile origin, imaginary axis
};

struct TileOp {
    uint8_t bytes[TILEOP_SIZE];  // 256 bytes, layout per spec Section 8.1
};

struct TileOpFaceView {
    const uint8_t* groups;
    const uint8_t* h1_packed;
    uint8_t count;
};

struct TileOpLayout {
    bool is_valid;
    bool is_empty;
    bool is_overflow;
    uint8_t off_I;
    uint8_t off_L;
    uint8_t off_R;
    uint8_t o_cnt;
    uint8_t i_cnt;
    uint8_t l_cnt;
    uint8_t r_cnt;
    uint8_t h_start;
    uint8_t payload_bytes_used;
    uint8_t payload_slack;
    TileOpFaceView faces[NUM_FACES];
};

// Port: a contiguous cluster of face primes on one face
struct Port {
    int     face;       // FACE_I, FACE_O, FACE_L, FACE_R
    int     group;      // 1-based group label (sequential, I->O->L->R)
    uint16_t h1;        // min along-face coordinate of this port (0..256 under shared boundaries)
    // h1 meaning: for I/O faces = tile_col, for L/R faces = tile_row
};

// Face extraction result, passed to encoder
struct FaceData {
    // Ports are emitted by extraction in face scan order I, O, L, R.
    // TileOp v2 reorders them to packed payload order O, I, L, R.
    Port    ports[MAX_PORTS];
    int     port_count;
    int     group_count;   // total unique groups assigned
};

// Per-phase timing breakdown (nanoseconds)
struct PhaseTimings {
    int64_t sieve_ns;
    int64_t compact_ns;
    int64_t union_find_ns;
    int64_t face_extract_ns;
    int64_t prune_encode_ns;
    int64_t total_ns;
};

// Full tile processing result with diagnostics
struct TileResult {
    TileOp   tileop;
    uint32_t prime_count;
    uint32_t group_count;
    uint32_t ports_before_pruning;
    uint32_t ports_after_pruning;
};
