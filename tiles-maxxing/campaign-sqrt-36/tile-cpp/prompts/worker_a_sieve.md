# Worker A: Phase 1 — Prime Sieve Implementation

## Context

You are implementing the row-sieve phase of a Gaussian moat tile processor.
The project lives at `tiles-maxxing/tile-cpp/` inside a CUDA Gaussian moat solver.
Your code is pure C++17 — no CUDA dependency. It will be ported to CUDA later,
so avoid exceptions, RTTI, virtual dispatch, and STL containers in hot paths.

The tile processor takes coordinates `(a_lo, b_lo)` and produces a bitmap of
Gaussian primes in a 271×271 sieve domain. Your sieve is Phase 1 of a 5-phase
pipeline. Downstream phases (compact, union-find, face extract, encode) consume
your bitmap output.

## Read First

- **Spec (your bible):** `tiles-maxxing/docs/tile_operations.md` — Sections 2, 3, 4, 12
- **CUDA reference (algorithm patterns, NOT to be #included):**
  - `src/modular_arith.cuh` — mulmod_small, powmod_small, mulmod64, powmod64
  - `src/miller_rabin.cuh` — is_prime(), tiered witnesses, trial division
  - `src/cornacchia.cuh` — fast_sqrt_neg1(), tonelli_shanks(), isqrt64()
  - `src/row_sieve.cuh` — row_sieve_mod_euclid(), mark_residue_class(), kernel structure
  - `src/tile_kernel.cuh` — abs_i64_to_u64(), gaussian_norm_u64(), is_gaussian_prime_point()

Read ALL of these files before writing any code. The CUDA code is the proven
reference — reimplement the same algorithms in clean C++17.

## Your Deliverables

Create these files inside `tiles-maxxing/tile-cpp/`:

### `include/sieve.h`
```cpp
#pragma once
#include "constants.h"
#include <cstdint>

struct SieveTables {
    uint32_t split_table[SPLIT_PRIMES_COUNT];  // packed: (root << 16) | p
    uint16_t inert_primes[INERT_PRIMES_COUNT];
    int split_count;
    int inert_count;
};

// Call once at startup. Computes sqrt(-1) mod p for all split primes,
// packs sieve table. Returns false on error.
bool init_sieve_tables(SieveTables& tables);

// Sieve the 271x271 domain for tile at (coord.a_lo, coord.b_lo).
// Output: bitmap[BITMAP_WORDS] with 1 = Gaussian prime, 0 = not prime.
// bitmap must be zero-initialized by caller.
void sieve_tile(const TileCoord& coord, const SieveTables& tables, uint32_t* bitmap);
```

### `src/sieve.cpp`
Full implementation including:

1. **Modular arithmetic** (reimplement from modular_arith.cuh):
   - `euclidean_mod(int64_t value, uint32_t modulus) -> int64_t`
   - `mulmod_small(uint64_t a, uint64_t b, uint64_t m) -> uint64_t` (for m < 2^32)
   - `powmod_small(uint64_t base, uint64_t exp, uint64_t m) -> uint64_t`

2. **Primality testing** (reimplement from miller_rabin.cuh):
   - `is_prime(uint64_t n) -> bool` — deterministic Miller-Rabin
   - Trial division by primes up to 97, then tiered witnesses:
     - n < 25,326,001: witnesses {2, 3, 5}
     - n < 3,215,031,751: witnesses {2, 3, 5, 7}
     - n < 2^32: witnesses {2, 3, 5, 7, 11}
   - Use mulmod_small for n < 2^32 (no 128-bit math needed)
   - For n >= 2^32 (won't happen at our operating point but implement for correctness):
     use __int128 mulmod

3. **Cornacchia / sqrt(-1)** (reimplement from cornacchia.cuh):
   - `tonelli_shanks(uint64_t n, uint64_t p) -> uint64_t`
   - `fast_sqrt_neg1(uint64_t p) -> uint64_t` — fast path for p≡5 mod 8
   - `isqrt64(uint64_t n) -> uint64_t`

4. **Gaussian primality**:
   - `is_gaussian_prime_point(int64_t a, int64_t b) -> bool`
   - Off-axis: norm = a²+b², check is_prime(norm)
   - Axis: |coord| ≡ 3 mod 4 AND is_prime(|coord|)
   - Handle (0,0) -> false, (1,1) -> true (norm=2 is prime)

5. **Sieve table init** (`init_sieve_tables`):
   - Enumerate primes up to 10,000 (sieve of Eratosthenes)
   - For p ≡ 1 mod 4: compute r = fast_sqrt_neg1(p), take min(r, p-r), pack as (r << 16) | p
   - For p ≡ 3 mod 4: store in inert_primes array
   - Verify: exactly 609 split primes, 619 inert primes

6. **Per-row sieve** (`sieve_tile`):
   For each row r in [0, 271):
   ```
   a = a_lo - COLLAR + r
   b_start = b_lo - COLLAR
   working_sieve[9 words] = 0  // 1 = composite

   Step 1: Parity — if (a ^ b) & 1 == 0, mark composite
   Step 2: Split sieve — for each (root, p), compute residue, mark two residue classes
   Step 3: Inert sieve — for each p≡3 mod 4, if p|a then mark b≡0 mod p
   Step 4: Small-norm rescue — if |a| <= 100, un-mark false positives
   Step 5: MR confirmation — for surviving candidates, test primality
           axis points use is_gaussian_prime_point directly
           off-axis: if not sieve-marked AND is_prime(a²+b²), set output bitmap bit
   ```

### `tests/test_sieve.cpp`
A standalone test that:
1. Calls `init_sieve_tables()`, verifies 609 split + 619 inert primes
2. Sieves tile (100, 100) — count primes in bitmap, print result
3. Sieves tile (10000, 10000) — count primes, print result
4. Checks a few known Gaussian primes are in the bitmap (e.g., if 100+100i area has known primes)
5. Checks axis prime handling if tile includes an axis

## Critical Implementation Details

**Euclidean mod (NOT C++ %):**
```cpp
int64_t euclidean_mod(int64_t value, uint32_t modulus) {
    int64_t mod = static_cast<int64_t>(modulus);
    int64_t rem = value % mod;
    if (rem < 0) rem += mod;
    return rem;
}
```

**mark_residue_class pattern:**
```cpp
void mark_residue_class(uint32_t* sieve, int width, int64_t b_start,
                        uint32_t p, int64_t residue) {
    int64_t b_start_mod = euclidean_mod(b_start, p);
    uint32_t first = static_cast<uint32_t>(euclidean_mod(residue - b_start_mod, p));
    for (uint32_t idx = first; idx < static_cast<uint32_t>(width); idx += p) {
        sieve[idx >> 5] |= 1U << (idx & 31);
    }
}
```

**Output bitmap convention:** 1 = prime, 0 = not prime. The working sieve uses
1 = composite (inverted). Convert at the end of each row.

**Bit layout:** bit at position `row * SIDE_EXP + col` in the output bitmap.
Word index = pos >> 5, bit index = pos & 31.

## Verification

After building, run `test_sieve`. Expected:
- init_sieve_tables succeeds with exactly 609 split, 619 inert primes
- Tile (100, 100) prime count should be in range [1500, 2500] (exact count TBD)
- No crashes, no sanitizer warnings in debug build

Build commands:
```bash
cd tiles-maxxing/tile-cpp
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make test_sieve 2>&1  # Note: will fail to link other phases — that's OK
# For standalone compilation:
cd .. && g++ -std=c++17 -O2 -Wall -Iinclude src/sieve.cpp tests/test_sieve.cpp -o test_sieve && ./test_sieve
```

## Style

- C++17, no CUDA headers, no STL containers in hot paths
- Use `static` or anonymous namespace for internal functions
- Keep function signatures compatible with future `__host__ __device__` annotation
- Prefer stack arrays over heap allocation where sizes are bounded
- All integer types explicit (uint32_t, int64_t, etc.)
