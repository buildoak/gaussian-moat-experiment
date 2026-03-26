#ifndef GM_TYPES_H
#define GM_TYPES_H

#include <cstdint>

struct GaussianPrime {
    int32_t a;
    int32_t b;
    uint64_t norm;
};

struct TileGeometry {
    uint64_t k_sq;
    int64_t collar;
    int64_t a_lo;
    int64_t a_hi;
    int64_t b_lo;
    int64_t b_hi;
    int64_t expanded_a_lo;
    int64_t expanded_b_lo;
    uint64_t nominal_extent;
    uint64_t side_exp;
    uint64_t total_points;
};

#endif // GM_TYPES_H
