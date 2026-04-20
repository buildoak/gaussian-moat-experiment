// src/sha256.h
//
// Minimal self-contained SHA-256 implementation for canonical hashes in
// campaign_constants and snapshot.
//
// Pure C++20, no external deps, no allocations. Deterministic across
// platforms. Used only for small inputs (fingerprint strings and witness
// tables) — not performance-critical.
//
// Internal header; not exposed under include/campaign/.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace campaign::detail {

// Compute SHA-256 of `data` (n bytes) and return the 32-byte digest.
std::array<std::uint8_t, 32> sha256_bytes(const std::uint8_t* data, std::size_t n);

// Compute SHA-256 of a string and return the lowercase hex digest (64 chars).
std::string sha256_hex(const std::string& input);

// Compute SHA-256 of a byte buffer and return the lowercase hex digest.
std::string sha256_hex(const std::uint8_t* data, std::size_t n);

}  // namespace campaign::detail
