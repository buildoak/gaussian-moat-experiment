#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

// Tile coordinate — matches both C++ and CUDA struct layout
struct TestTileCoord {
    int64_t a_lo;
    int64_t b_lo;
};

static const int TILE_ALIGN = 256;

// Generate N tile coordinates at R ~ 860,000,000 with diverse angles.
// All coordinates are multiples of TILE_SIDE (256) and off-axis (a != 0, b != 0).
// Deterministic: uses a simple linear-congruential sequence for reproducibility
// across C++ and CUDA builds (no mt19937 to avoid potential ABI differences).
inline std::vector<TestTileCoord> generate_test_coords(int n) {
    const double R = 860000000.0;
    const double R_WIDTH = 8192.0;

    std::vector<TestTileCoord> coords;
    coords.reserve(n);

    // Simple LCG for reproducibility across compilers
    uint64_t state = 42;
    auto next_u64 = [&]() -> uint64_t {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state;
    };
    auto next_double = [&](double lo, double hi) -> double {
        uint64_t r = next_u64();
        double frac = static_cast<double>(r >> 11) / static_cast<double>(1ULL << 53);
        return lo + frac * (hi - lo);
    };

    for (int i = 0; i < n; ++i) {
        // Angle in (0.05, pi/4 - 0.05) to stay off-axis
        double theta = next_double(0.05, M_PI / 4.0 - 0.05);
        double r = next_double(R, R + R_WIDTH);

        double a = r * std::cos(theta);
        double b = r * std::sin(theta);

        int64_t a_lo = static_cast<int64_t>(std::floor(a / TILE_ALIGN)) * TILE_ALIGN;
        int64_t b_lo = static_cast<int64_t>(std::floor(b / TILE_ALIGN)) * TILE_ALIGN;

        // Ensure off-axis and first octant
        if (a_lo <= 0) a_lo = TILE_ALIGN;
        if (b_lo <= 0) b_lo = TILE_ALIGN;
        if (a_lo < b_lo) {
            int64_t tmp = a_lo;
            a_lo = b_lo;
            b_lo = tmp;
        }

        coords.push_back(TestTileCoord{a_lo, b_lo});
    }

    return coords;
}
