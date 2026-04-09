#include "sieve.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

__extension__ typedef unsigned __int128 u128;

constexpr uint32_t kRowWords = (SIDE_EXP + 31) / 32;
constexpr uint64_t kLargeWitnesses[] = {
    2ULL, 3ULL, 5ULL, 7ULL, 11ULL, 13ULL, 17ULL, 19ULL, 23ULL, 29ULL, 31ULL, 37ULL
};
constexpr uint32_t kTrialPrimes[] = {
    3U, 5U, 7U, 11U, 13U, 17U, 19U, 23U, 29U, 31U, 37U, 41U, 43U,
    47U, 53U, 59U, 61U, 67U, 71U, 73U, 79U, 83U, 89U, 97U
};

inline uint64_t abs_i64_to_u64(int64_t value) {
    return value < 0
        ? static_cast<uint64_t>(-(value + 1)) + 1ULL
        : static_cast<uint64_t>(value);
}

inline uint64_t gaussian_norm_u64(int64_t a, int64_t b) {
    const uint64_t ua = abs_i64_to_u64(a);
    const uint64_t ub = abs_i64_to_u64(b);
    return static_cast<uint64_t>(static_cast<u128>(ua) * ua + static_cast<u128>(ub) * ub);
}

inline uint32_t ctz64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_ctzll(value));
#else
    uint32_t count = 0;
    while ((value & 1ULL) == 0ULL) {
        value >>= 1;
        ++count;
    }
    return count;
#endif
}

int64_t euclidean_mod(int64_t value, uint32_t modulus) {
    const int64_t mod = static_cast<int64_t>(modulus);
    int64_t rem = value % mod;
    if (rem < 0) {
        rem += mod;
    }
    return rem;
}

uint64_t mulmod_small(uint64_t a, uint64_t b, uint64_t m) {
    return (a * b) % m;
}

uint64_t powmod_small(uint64_t base, uint64_t exp, uint64_t m) {
    if (m == 1ULL) {
        return 0ULL;
    }

    uint64_t result = 1ULL;
    base %= m;
    while (exp != 0ULL) {
        if ((exp & 1ULL) != 0ULL) {
            result = mulmod_small(result, base, m);
        }
        base = mulmod_small(base, base, m);
        exp >>= 1;
    }
    return result;
}

uint64_t mulmod64(uint64_t a, uint64_t b, uint64_t m) {
    return static_cast<uint64_t>(static_cast<u128>(a) * static_cast<u128>(b) % static_cast<u128>(m));
}

uint64_t powmod64(uint64_t base, uint64_t exp, uint64_t m) {
    if (m == 1ULL) {
        return 0ULL;
    }

    uint64_t result = 1ULL;
    base %= m;
    while (exp != 0ULL) {
        if ((exp & 1ULL) != 0ULL) {
            result = mulmod64(result, base, m);
        }
        base = mulmod64(base, base, m);
        exp >>= 1;
    }
    return result;
}

bool miller_rabin_witness_small(uint64_t n, uint64_t d, uint32_t s, uint64_t a) {
    if (a >= n) {
        return true;
    }

    uint64_t x = powmod_small(a, d, n);
    if (x == 1ULL || x == n - 1ULL) {
        return true;
    }

    for (uint32_t r = 1; r < s; ++r) {
        x = mulmod_small(x, x, n);
        if (x == n - 1ULL) {
            return true;
        }
    }

    return false;
}

bool miller_rabin_witness(uint64_t n, uint64_t d, uint32_t s, uint64_t a) {
    if (a >= n) {
        return true;
    }

    uint64_t x = powmod64(a, d, n);
    if (x == 1ULL || x == n - 1ULL) {
        return true;
    }

    for (uint32_t r = 1; r < s; ++r) {
        x = mulmod64(x, x, n);
        if (x == n - 1ULL) {
            return true;
        }
    }

    return false;
}

bool is_prime(uint64_t n) {
    if (n < 2ULL) {
        return false;
    }
    if (n == 2ULL || n == 3ULL) {
        return true;
    }
    if ((n & 1ULL) == 0ULL) {
        return false;
    }

    for (uint32_t p : kTrialPrimes) {
        const uint64_t p64 = static_cast<uint64_t>(p);
        if (n == p64) {
            return true;
        }
        if (n % p64 == 0ULL) {
            return false;
        }
    }

    if (n < 97ULL * 97ULL) {
        return true;
    }

    uint64_t d = n - 1ULL;
    const uint32_t s = ctz64(d);
    d >>= s;

    if (n < 0x100000000ULL) {
        if (n < 25326001ULL) {
            return miller_rabin_witness_small(n, d, s, 2ULL) &&
                   miller_rabin_witness_small(n, d, s, 3ULL) &&
                   miller_rabin_witness_small(n, d, s, 5ULL);
        }
        if (n < 3215031751ULL) {
            return miller_rabin_witness_small(n, d, s, 2ULL) &&
                   miller_rabin_witness_small(n, d, s, 3ULL) &&
                   miller_rabin_witness_small(n, d, s, 5ULL) &&
                   miller_rabin_witness_small(n, d, s, 7ULL);
        }
        return miller_rabin_witness_small(n, d, s, 2ULL) &&
               miller_rabin_witness_small(n, d, s, 3ULL) &&
               miller_rabin_witness_small(n, d, s, 5ULL) &&
               miller_rabin_witness_small(n, d, s, 7ULL) &&
               miller_rabin_witness_small(n, d, s, 11ULL);
    }

    for (uint64_t witness : kLargeWitnesses) {
        if (!miller_rabin_witness(n, d, s, witness)) {
            return false;
        }
    }
    return true;
}

[[maybe_unused]] uint64_t isqrt64(uint64_t n) {
    if (n == 0ULL) {
        return 0ULL;
    }

    uint64_t x = static_cast<uint64_t>(std::sqrt(static_cast<long double>(n)));
    while (static_cast<u128>(x + 1ULL) * static_cast<u128>(x + 1ULL) <= static_cast<u128>(n)) {
        ++x;
    }
    while (static_cast<u128>(x) * static_cast<u128>(x) > static_cast<u128>(n)) {
        --x;
    }
    return x;
}

uint64_t tonelli_shanks(uint64_t n, uint64_t p) {
    if (p == 2ULL) {
        return n & 1ULL;
    }

    n %= p;
    if (n == 0ULL) {
        return 0ULL;
    }

    const bool small_p = p < 0x100000000ULL;
    const uint64_t euler = small_p ? powmod_small(n, (p - 1ULL) / 2ULL, p)
                                   : powmod64(n, (p - 1ULL) / 2ULL, p);
    if (euler != 1ULL) {
        return std::numeric_limits<uint64_t>::max();
    }

    if ((p & 3ULL) == 3ULL) {
        return small_p ? powmod_small(n, (p + 1ULL) / 4ULL, p)
                       : powmod64(n, (p + 1ULL) / 4ULL, p);
    }

    uint64_t q = p - 1ULL;
    uint32_t s = 0;
    while ((q & 1ULL) == 0ULL) {
        q >>= 1;
        ++s;
    }

    uint64_t z = 2ULL;
    while (true) {
        const uint64_t check = small_p ? powmod_small(z, (p - 1ULL) / 2ULL, p)
                                       : powmod64(z, (p - 1ULL) / 2ULL, p);
        if (check == p - 1ULL) {
            break;
        }
        ++z;
    }

    uint32_t m = s;
    uint64_t c = small_p ? powmod_small(z, q, p) : powmod64(z, q, p);
    uint64_t t = small_p ? powmod_small(n, q, p) : powmod64(n, q, p);
    uint64_t r = small_p ? powmod_small(n, (q + 1ULL) / 2ULL, p)
                         : powmod64(n, (q + 1ULL) / 2ULL, p);

    if (small_p) {
        while (t != 1ULL) {
            uint32_t i = 1U;
            uint64_t t2 = mulmod_small(t, t, p);
            while (t2 != 1ULL) {
                t2 = mulmod_small(t2, t2, p);
                ++i;
                if (i >= m) {
                    return std::numeric_limits<uint64_t>::max();
                }
            }

            const uint64_t b = powmod_small(c, 1ULL << (m - i - 1U), p);
            r = mulmod_small(r, b, p);
            c = mulmod_small(b, b, p);
            t = mulmod_small(t, c, p);
            m = i;
        }
    } else {
        while (t != 1ULL) {
            uint32_t i = 1U;
            uint64_t t2 = mulmod64(t, t, p);
            while (t2 != 1ULL) {
                t2 = mulmod64(t2, t2, p);
                ++i;
                if (i >= m) {
                    return std::numeric_limits<uint64_t>::max();
                }
            }

            const uint64_t b = powmod64(c, 1ULL << (m - i - 1U), p);
            r = mulmod64(r, b, p);
            c = mulmod64(b, b, p);
            t = mulmod64(t, c, p);
            m = i;
        }
    }

    return r;
}

uint64_t fast_sqrt_neg1(uint64_t p) {
    if (p <= 2ULL || (p & 3ULL) != 1ULL) {
        return std::numeric_limits<uint64_t>::max();
    }

    const bool small_p = p < 0x100000000ULL;
    if ((p & 7ULL) == 5ULL) {
        const uint64_t r = small_p ? powmod_small(2ULL, (p - 1ULL) >> 2U, p)
                                   : powmod64(2ULL, (p - 1ULL) >> 2U, p);
        const uint64_t r2 = small_p ? mulmod_small(r, r, p) : mulmod64(r, r, p);
        if (r2 == p - 1ULL) {
            return r;
        }
    }

    return tonelli_shanks(p - 1ULL, p);
}

bool is_axis_gaussian_prime(int64_t coord) {
    const uint64_t magnitude = abs_i64_to_u64(coord);
    return magnitude != 0ULL && (magnitude & 3ULL) == 3ULL && is_prime(magnitude);
}

bool is_gaussian_prime_point(int64_t a, int64_t b) {
    if (a == 0) {
        return is_axis_gaussian_prime(b);
    }
    if (b == 0) {
        return is_axis_gaussian_prime(a);
    }

    const uint64_t norm = gaussian_norm_u64(a, b);
    if (norm < 2ULL) {
        return false;
    }
    if ((norm & 1ULL) == 0ULL && norm > 2ULL) {
        return false;
    }
    return is_prime(norm);
}

inline bool is_marked(const uint32_t* sieve, uint32_t col) {
    return ((sieve[col >> 5] >> (col & 31U)) & 1U) != 0U;
}

inline void clear_mark(uint32_t* sieve, uint32_t col) {
    sieve[col >> 5] &= ~(1U << (col & 31U));
}

inline void set_output_bit(uint32_t* bitmap, uint32_t row, uint32_t col) {
    const uint32_t pos = row * static_cast<uint32_t>(SIDE_EXP) + col;
    bitmap[pos >> 5] |= 1U << (pos & 31U);
}

void mark_residue_class(uint32_t* sieve, int width, int64_t b_start, uint32_t p, int64_t residue) {
    const int64_t b_start_mod = euclidean_mod(b_start, p);
    const uint32_t first = static_cast<uint32_t>(euclidean_mod(residue - b_start_mod, p));
    for (uint32_t idx = first; idx < static_cast<uint32_t>(width); idx += p) {
        sieve[idx >> 5] |= 1U << (idx & 31U);
    }
}

}  // namespace

bool init_sieve_tables(SieveTables& tables) {
    std::memset(&tables, 0, sizeof(tables));

    uint8_t is_prime_table[SIEVE_LIMIT + 1];
    std::memset(is_prime_table, 1, sizeof(is_prime_table));
    is_prime_table[0] = 0U;
    is_prime_table[1] = 0U;

    for (uint32_t p = 2U; p * p <= SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) {
            continue;
        }
        for (uint32_t multiple = p * p; multiple <= SIEVE_LIMIT; multiple += p) {
            is_prime_table[multiple] = 0U;
        }
    }

    for (uint32_t p = 2U; p <= SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) {
            continue;
        }

        if ((p & 3U) == 1U) {
            const uint64_t root_raw = fast_sqrt_neg1(static_cast<uint64_t>(p));
            if (root_raw == std::numeric_limits<uint64_t>::max()) {
                return false;
            }

            uint64_t root = root_raw;
            const uint64_t neg_root = static_cast<uint64_t>(p) - root;
            if (neg_root < root) {
                root = neg_root;
            }

            if (mulmod_small(root, root, static_cast<uint64_t>(p)) != static_cast<uint64_t>(p - 1U)) {
                return false;
            }
            if (tables.split_count >= SPLIT_PRIMES_COUNT) {
                return false;
            }

            tables.split_table[tables.split_count++] =
                (static_cast<uint32_t>(root) << 16) | p;
        } else if ((p & 3U) == 3U) {
            if (tables.inert_count >= INERT_PRIMES_COUNT) {
                return false;
            }
            tables.inert_primes[tables.inert_count++] = static_cast<uint16_t>(p);
        }
    }

    return tables.split_count == SPLIT_PRIMES_COUNT &&
           tables.inert_count == INERT_PRIMES_COUNT;
}

void sieve_tile(const TileCoord& coord, const SieveTables& tables, uint32_t* bitmap) {
    std::memset(bitmap, 0, BITMAP_WORDS * sizeof(uint32_t));

    const int64_t a_start = coord.a_lo - static_cast<int64_t>(COLLAR);
    const int64_t b_start = coord.b_lo - static_cast<int64_t>(COLLAR);

    for (uint32_t row = 0; row < static_cast<uint32_t>(SIDE_EXP); ++row) {
        const int64_t a = a_start + static_cast<int64_t>(row);
        uint32_t working_sieve[kRowWords];
        std::memset(working_sieve, 0, sizeof(working_sieve));

        for (uint32_t col = 0; col < static_cast<uint32_t>(SIDE_EXP); ++col) {
            const int64_t b = b_start + static_cast<int64_t>(col);
            if (((a ^ b) & 1LL) == 0LL) {
                working_sieve[col >> 5] |= 1U << (col & 31U);
            }
        }

        for (int i = 0; i < tables.split_count; ++i) {
            const uint32_t packed = tables.split_table[i];
            const uint32_t p = packed & 0xFFFFU;
            const uint32_t root = packed >> 16;
            const int64_t residue =
                (euclidean_mod(a, p) * static_cast<int64_t>(root)) % static_cast<int64_t>(p);

            mark_residue_class(working_sieve, SIDE_EXP, b_start, p, residue);

            const int64_t neg_residue = euclidean_mod(-residue, p);
            if (neg_residue != residue) {
                mark_residue_class(working_sieve, SIDE_EXP, b_start, p, neg_residue);
            }
        }

        for (int i = 0; i < tables.inert_count; ++i) {
            const uint32_t p = static_cast<uint32_t>(tables.inert_primes[i]);
            if (euclidean_mod(a, p) == 0) {
                mark_residue_class(working_sieve, SIDE_EXP, b_start, p, 0);
            }
        }

        if (abs_i64_to_u64(a) <= static_cast<uint64_t>(SIEVE_SQRT)) {
            const uint64_t a_sq = gaussian_norm_u64(a, 0);
            for (uint32_t col = 0; col < static_cast<uint32_t>(SIDE_EXP); ++col) {
                if (!is_marked(working_sieve, col)) {
                    continue;
                }

                const int64_t b = b_start + static_cast<int64_t>(col);
                const uint64_t norm = a_sq + gaussian_norm_u64(0, b);
                if (norm >= 2ULL &&
                    norm <= static_cast<uint64_t>(SIEVE_LIMIT) &&
                    is_prime(norm)) {
                    clear_mark(working_sieve, col);
                }
            }
        }

        for (uint32_t col = 0; col < static_cast<uint32_t>(SIDE_EXP); ++col) {
            const int64_t b = b_start + static_cast<int64_t>(col);
            if (a == 0 || b == 0) {
                if (is_gaussian_prime_point(a, b)) {
                    set_output_bit(bitmap, row, col);
                }
                continue;
            }

            if (is_marked(working_sieve, col)) {
                continue;
            }

            const uint64_t norm = gaussian_norm_u64(a, b);
            if (is_prime(norm)) {
                set_output_bit(bitmap, row, col);
            }
        }
    }
}
