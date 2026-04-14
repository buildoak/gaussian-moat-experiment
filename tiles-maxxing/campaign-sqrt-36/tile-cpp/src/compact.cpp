#include "compact.h"

#include <cassert>
#include <cstdint>

namespace {

constexpr uint32_t kLastWordValidBits = static_cast<uint32_t>(BITMAP_BITS & 31);
constexpr uint32_t kLastWordMask =
    (kLastWordValidBits == 0U) ? 0xFFFFFFFFU : ((1U << kLastWordValidBits) - 1U);

inline uint32_t masked_bitmap_word(const uint32_t* bitmap, int word_index) {
    uint32_t word = bitmap[word_index];
    if (word_index == (BITMAP_WORDS - 1)) {
        word &= kLastWordMask;
    }
    return word;
}

}  // namespace

int compact_primes(const uint32_t* bitmap, uint32_t* prefix, uint32_t* prime_pos) {
    uint32_t counts[BITMAP_WORDS];

    for (int w = 0; w < BITMAP_WORDS; ++w) {
        counts[w] = static_cast<uint32_t>(__builtin_popcount(masked_bitmap_word(bitmap, w)));
    }

    prefix[0] = 0U;
    for (int w = 1; w < BITMAP_WORDS; ++w) {
        prefix[w] = prefix[w - 1] + counts[w - 1];
    }

    const uint32_t prime_count_u32 = prefix[BITMAP_WORDS - 1] + counts[BITMAP_WORDS - 1];
    assert(prime_count_u32 <= static_cast<uint32_t>(MAX_PRIMES));

    for (int w = 0; w < BITMAP_WORDS; ++w) {
        uint32_t word = masked_bitmap_word(bitmap, w);
        uint32_t idx = prefix[w];
        while (word != 0U) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(word));
            prime_pos[idx] = static_cast<uint32_t>(w * 32) + bit;
            ++idx;
            word &= (word - 1U);
        }
    }

    return static_cast<int>(prime_count_u32);
}
