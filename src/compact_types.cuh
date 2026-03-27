#pragma once

#include <cstdint>
#include <cuda_runtime.h>

namespace gm {

struct CompactPortRecord {
    uint16_t x;       // position relative to tile origin (0..tile_side+2*collar)
    uint16_t y;       // position relative to tile origin
    uint32_t comp_id; // local component ID (host patches to global later)
}; // exactly 8 bytes
static_assert(sizeof(CompactPortRecord) == 8, "CompactPortRecord must be 8 bytes");

struct CompactTileHeader {
    uint32_t tile_idx;       // tile index in campaign
    uint32_t num_components; // number of distinct components in this tile
    uint32_t num_ports;      // total boundary ports across all 4 faces
    uint32_t component_base; // global component ID offset (set by host, 0 during kernel)
}; // exactly 16 bytes
static_assert(sizeof(CompactTileHeader) == 16, "CompactTileHeader must be 16 bytes");

// Max compact output per tile: header + face_bits + ports
// header(16) + face_bits(kMaxPrimesPerTile * 1) + ports(kMaxBoundaryPorts * 8)
static constexpr uint32_t kMaxBoundaryPortsPerTile = 1024; // 4 faces × 256 max
static constexpr uint32_t kMaxCompactTileBytes =
    sizeof(CompactTileHeader) +
    8192 * sizeof(uint8_t) +
    kMaxBoundaryPortsPerTile * sizeof(CompactPortRecord);

// Get pointer to face_bits array within a compact tile slot
__host__ __device__ inline uint8_t* compact_face_bits(void* slot) {
    return reinterpret_cast<uint8_t*>(slot) + sizeof(CompactTileHeader);
}

// Get pointer to port records within a compact tile slot
__host__ __device__ inline CompactPortRecord* compact_ports(void* slot, uint32_t num_components) {
    return reinterpret_cast<CompactPortRecord*>(
        reinterpret_cast<uint8_t*>(slot) + sizeof(CompactTileHeader) + num_components * sizeof(uint8_t));
}

// Actual size of a compact tile result
__host__ __device__ inline uint32_t compact_tile_size(uint32_t num_components, uint32_t num_ports) {
    return sizeof(CompactTileHeader) +
        num_components * sizeof(uint8_t) +
        num_ports * sizeof(CompactPortRecord);
}

} // namespace gm
