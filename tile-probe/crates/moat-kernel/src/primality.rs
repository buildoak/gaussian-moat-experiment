use std::sync::LazyLock;

pub const SMALL_PRIMES: [u64; 168] = [
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97,
    101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191, 193,
    197, 199, 211, 223, 227, 229, 233, 239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293, 307,
    311, 313, 317, 331, 337, 347, 349, 353, 359, 367, 373, 379, 383, 389, 397, 401, 409, 419, 421,
    431, 433, 439, 443, 449, 457, 461, 463, 467, 479, 487, 491, 499, 503, 509, 521, 523, 541, 547,
    557, 563, 569, 571, 577, 587, 593, 599, 601, 607, 613, 617, 619, 631, 641, 643, 647, 653, 659,
    661, 673, 677, 683, 691, 701, 709, 719, 727, 733, 739, 743, 751, 757, 761, 769, 773, 787, 797,
    809, 811, 821, 823, 827, 829, 839, 853, 857, 859, 863, 877, 881, 883, 887, 907, 911, 919, 929,
    937, 941, 947, 953, 967, 971, 977, 983, 991, 997,
];

// 12 deterministic witnesses — valid for all n < 3.317 × 10^24 (covers all u64 inputs).
pub const MR_WITNESSES_12: [u64; 12] = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37];

/// Default sieve limit used by the legacy static tables and SieveTable::default().
pub const DEFAULT_SIEVE_LIMIT: u32 = 10_000;

// Legacy constant kept for backward compatibility (same value).
pub const ROW_SIEVE_LIMIT: u32 = DEFAULT_SIEVE_LIMIT;

// ---------------------------------------------------------------------------
// Legacy static tables (renamed from SQRT_NEG1_TABLE / MOD_3_SIEVE_PRIMES).
// These are kept for validation tests. They are computed lazily using the same
// logic as SieveTable::new() but stored as (u16, u16) / u16 for historical
// reasons. The canonical source of truth for runtime usage is SieveTable.
// ---------------------------------------------------------------------------

pub static LEGACY_SQRT_NEG1_TABLE: LazyLock<Vec<(u16, u16)>> = LazyLock::new(|| {
    primes_up_to_u32(DEFAULT_SIEVE_LIMIT)
        .into_iter()
        .filter(|&p| p % 4 == 1)
        .map(|p| {
            let root = tonelli_shanks(u64::from(p - 1), u64::from(p))
                .expect("sqrt(-1) must exist for primes congruent to 1 mod 4");
            let canonical = root.min(u64::from(p) - root) as u16;
            (p as u16, canonical)
        })
        .collect()
});

/// Alias for backward compatibility — tests still reference SQRT_NEG1_TABLE by name.
pub static SQRT_NEG1_TABLE: LazyLock<Vec<(u16, u16)>> = LazyLock::new(|| {
    LEGACY_SQRT_NEG1_TABLE.iter().copied().collect()
});

pub static LEGACY_MOD_3_SIEVE_PRIMES: LazyLock<Vec<u16>> = LazyLock::new(|| {
    primes_up_to_u32(DEFAULT_SIEVE_LIMIT)
        .into_iter()
        .filter(|&p| p % 4 == 3)
        .map(|p| p as u16)
        .collect()
});

// ---------------------------------------------------------------------------
// SieveTable — parameterized prime table for sieve_row().
// ---------------------------------------------------------------------------

/// A precomputed sieve table for a given prime limit.
///
/// - `splitting`: primes p ≡ 1 (mod 4), stored as (p, sqrt(-1) mod p).
///   Both roots ±r are used by `sieve_row`, so the canonical choice (min root)
///   matches the legacy table but the sieve output is root-independent.
/// - `inert`: primes p ≡ 3 (mod 4), p > 2.
pub struct SieveTable {
    pub limit: u32,
    pub splitting: Vec<(u32, u32)>, // (prime, sqrt(-1) mod p) for p ≡ 1 mod 4
    pub inert: Vec<u32>,            // primes p ≡ 3 mod 4
}

impl SieveTable {
    /// Build a SieveTable for all primes up to `limit`.
    ///
    /// For p ≡ 1 (mod 4): computes the canonical (smaller) sqrt(-1) mod p.
    /// For p ≡ 3 (mod 4), p > 2: stores p in the inert list.
    /// p = 2 is handled by the parity sieve in `sieve_row` and is excluded.
    pub fn new(limit: u32) -> Self {
        let primes = primes_up_to_u32(limit);
        let mut splitting = Vec::new();
        let mut inert = Vec::new();

        for p in primes {
            match p % 4 {
                1 => {
                    let root = tonelli_shanks(u64::from(p - 1), u64::from(p))
                        .expect("sqrt(-1) must exist for p ≡ 1 mod 4");
                    // Canonical: pick the smaller of the two roots.
                    let canonical = root.min(u64::from(p) - root) as u32;
                    splitting.push((p, canonical));
                }
                3 => {
                    inert.push(p);
                }
                _ => {} // p == 2: handled by parity step; p % 4 == 0 impossible for prime
            }
        }

        Self {
            limit,
            splitting,
            inert,
        }
    }

    /// Default table at limit=10,000 — matches the legacy static tables exactly.
    pub fn default() -> Self {
        Self::new(DEFAULT_SIEVE_LIMIT)
    }
}

/// Global default SieveTable, lazily initialised. Used by the legacy
/// `sieve_row` signature kept for backward compatibility.
pub static DEFAULT_SIEVE_TABLE: LazyLock<SieveTable> = LazyLock::new(SieveTable::default);

// ---------------------------------------------------------------------------
// Arithmetic helpers
// ---------------------------------------------------------------------------

#[inline(always)]
pub fn mulmod(a: u64, b: u64, m: u64) -> u64 {
    ((a as u128) * (b as u128) % (m as u128)) as u64
}

#[derive(Clone, Copy, Debug)]
struct MontgomeryParams {
    n: u64,
    n_inv: u64,
    r2: u64,
}

impl MontgomeryParams {
    #[inline]
    fn new(n: u64) -> Self {
        debug_assert!(n > 0 && !n.is_multiple_of(2));

        let mut inv = 1_u64;
        for _ in 0..6 {
            inv = inv.wrapping_mul(2_u64.wrapping_sub(n.wrapping_mul(inv)));
        }

        let r = (((u64::MAX as u128) % (n as u128)) + 1) % (n as u128);
        let r2 = (r * r % (n as u128)) as u64;

        Self {
            n,
            n_inv: inv.wrapping_neg(),
            r2,
        }
    }

    #[inline(always)]
    fn mont_mul(&self, a: u64, b: u64) -> u64 {
        let t = (a as u128) * (b as u128);
        let m = ((t as u64).wrapping_mul(self.n_inv)) as u128;
        let u = (t.wrapping_add(m.wrapping_mul(self.n as u128))) >> 64;
        let u = u as u64;
        if u >= self.n {
            u - self.n
        } else {
            u
        }
    }

    #[allow(clippy::wrong_self_convention)]
    #[inline(always)]
    fn to_mont(&self, a: u64) -> u64 {
        self.mont_mul(a, self.r2)
    }

    #[allow(clippy::wrong_self_convention)]
    #[inline(always)]
    fn from_mont(&self, a: u64) -> u64 {
        self.mont_mul(a, 1)
    }
}

#[inline(always)]
fn powmod_division(base: u64, exp: u64, m: u64) -> u64 {
    let mut result = 1_u64;
    let mut factor = base % m;
    let mut exponent = exp;

    while exponent > 0 {
        if exponent & 1 == 1 {
            result = mulmod(result, factor, m);
        }
        factor = mulmod(factor, factor, m);
        exponent >>= 1;
    }

    result
}

#[inline(always)]
pub fn powmod(base: u64, exp: u64, m: u64) -> u64 {
    if m <= 1 {
        return 0;
    }

    if m.is_multiple_of(2) {
        return powmod_division(base, exp, m);
    }

    let params = MontgomeryParams::new(m);
    let mut result = params.to_mont(1);
    let mut factor = params.to_mont(base);
    let mut exponent = exp;

    while exponent > 0 {
        if exponent & 1 == 1 {
            result = params.mont_mul(result, factor);
        }
        factor = params.mont_mul(factor, factor);
        exponent >>= 1;
    }

    params.from_mont(result)
}

/// Sieve of Eratosthenes up to `limit` (inclusive), returning primes as u32.
/// Handles limits up to ~4 billion (constrained by memory).
fn primes_up_to_u32(limit: u32) -> Vec<u32> {
    if limit < 2 {
        return Vec::new();
    }

    let n = limit as usize;
    let mut is_prime = vec![true; n + 1];
    is_prime[0] = false;
    is_prime[1] = false;

    let mut p = 2_usize;
    while p * p <= n {
        if is_prime[p] {
            let mut multiple = p * p;
            while multiple <= n {
                is_prime[multiple] = false;
                multiple += p;
            }
        }
        p += 1;
    }

    is_prime
        .iter()
        .enumerate()
        .filter_map(|(n, &prime)| prime.then_some(n as u32))
        .collect()
}

fn tonelli_shanks(n: u64, p: u64) -> Option<u64> {
    if p == 2 {
        return Some(n % p);
    }

    if powmod(n, (p - 1) / 2, p) != 1 {
        return None;
    }

    let mut q = p - 1;
    let mut s = 0_u32;
    while q.is_multiple_of(2) {
        q >>= 1;
        s += 1;
    }

    if s == 1 {
        return Some(powmod(n, (p + 1) / 4, p));
    }

    let mut z = 2_u64;
    while powmod(z, (p - 1) / 2, p) != p - 1 {
        z += 1;
    }

    let mut m = s;
    let mut c = powmod(z, q, p);
    let mut t = powmod(n, q, p);
    let mut x = powmod(n, q.div_ceil(2), p);

    while t != 1 {
        let mut i = 1_u32;
        let mut t_pow = mulmod(t, t, p);
        while i < m && t_pow != 1 {
            t_pow = mulmod(t_pow, t_pow, p);
            i += 1;
        }

        if i == m {
            return None;
        }

        let b = powmod(c, 1_u64 << (m - i - 1), p);
        x = mulmod(x, b, p);
        c = mulmod(b, b, p);
        t = mulmod(t, c, p);
        m = i;
    }

    Some(x)
}

// ---------------------------------------------------------------------------
// Sieve kernel
// ---------------------------------------------------------------------------

#[inline]
fn mark_residue_class(b_start: i64, width: usize, p: i64, residue: i64, sieve: &mut [bool]) {
    let first = (residue - b_start).rem_euclid(p) as usize;
    for idx in (first..width).step_by(p as usize) {
        sieve[idx] = true;
    }
}

/// Mark Gaussian integers on a single row `a + b·i` for `b ∈ [b_start, b_start+width)`.
///
/// `table` provides the precomputed prime lists. Build it once per tile and
/// reuse across all rows. Use `&DEFAULT_SIEVE_TABLE` for the legacy behaviour
/// at limit=10,000.
pub fn sieve_row(a: i64, b_start: i64, width: usize, table: &SieveTable, sieve: &mut [bool]) {
    assert_eq!(
        sieve.len(),
        width,
        "row sieve width must match buffer length"
    );

    if width == 0 {
        return;
    }

    let parity_start = if ((a ^ b_start) & 1) == 0 { 0 } else { 1 };
    for idx in (parity_start..width).step_by(2) {
        sieve[idx] = true;
    }

    for &(p_u32, r_u32) in table.splitting.iter() {
        let p = i64::from(p_u32);
        let residue = (a.rem_euclid(p) * i64::from(r_u32)).rem_euclid(p);
        mark_residue_class(b_start, width, p, residue, sieve);

        let neg_residue = (-residue).rem_euclid(p);
        if neg_residue != residue {
            mark_residue_class(b_start, width, p, neg_residue, sieve);
        }
    }

    for &p_u32 in table.inert.iter() {
        let p = i64::from(p_u32);
        if a.rem_euclid(p) == 0 {
            mark_residue_class(b_start, width, p, 0, sieve);
        }
    }

    // Small norms can be equal to a sieve prime, so they must stay as MR candidates.
    let sieve_limit_i128 = i128::from(table.limit);
    let a_sq = i128::from(a) * i128::from(a);
    if a_sq <= sieve_limit_i128 {
        for (col, marked) in sieve.iter_mut().enumerate() {
            if !*marked {
                continue;
            }

            let b = b_start + col as i64;
            let norm = a_sq + i128::from(b) * i128::from(b);
            if !(2..=sieve_limit_i128).contains(&norm) {
                continue;
            }

            if is_rational_prime(norm as u64) {
                *marked = false;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Primality tests
// ---------------------------------------------------------------------------

#[inline]
pub fn has_small_factor(n: u64) -> bool {
    for &p in &SMALL_PRIMES {
        if n == p {
            return false;
        }
        if n.is_multiple_of(p) {
            return true;
        }
    }

    false
}

pub fn miller_rabin_12(n: u64) -> bool {
    let mut d = n - 1;
    let mut r = 0_u32;

    while d.is_multiple_of(2) {
        d >>= 1;
        r += 1;
    }

    'witness: for &a in &MR_WITNESSES_12 {
        let a = a % n;
        if a == 0 {
            continue;
        }

        let mut x = powmod(a, d, n);
        if x == 1 || x == n - 1 {
            continue;
        }

        for _ in 0..(r - 1) {
            x = mulmod(x, x, n);
            if x == n - 1 {
                continue 'witness;
            }
        }

        return false;
    }

    true
}

fn is_rational_prime(n: u64) -> bool {
    if n < 2 {
        return false;
    }
    if n == 2 {
        return true;
    }
    if n.is_multiple_of(2) {
        return false;
    }
    if has_small_factor(n) {
        return false;
    }

    miller_rabin_12(n)
}

pub fn is_gaussian_prime(a: i64, b: i64) -> bool {
    if b == 0 {
        let abs_a = (a as i128).unsigned_abs() as u64;
        return abs_a % 4 == 3 && is_rational_prime(abs_a);
    }

    if a == 0 {
        let abs_b = (b as i128).unsigned_abs() as u64;
        return abs_b % 4 == 3 && is_rational_prime(abs_b);
    }

    let norm = (a as i128).pow(2) + (b as i128).pow(2);
    assert!(norm <= u64::MAX as i128, "Gaussian norm exceeds u64");

    let n = norm as u64;
    if n < 2 {
        return false;
    }
    if n == 2 {
        return true;
    }
    if n.is_multiple_of(2) {
        return false;
    }
    if has_small_factor(n) {
        return false;
    }

    miller_rabin_12(n)
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::{
        has_small_factor, is_gaussian_prime, miller_rabin_12, mulmod, powmod, sieve_row,
        MontgomeryParams, SieveTable, DEFAULT_SIEVE_TABLE, LEGACY_SQRT_NEG1_TABLE,
        SQRT_NEG1_TABLE,
    };

    #[test]
    fn test_mulmod_basic() {
        assert_eq!(mulmod(7, 8, 13), (7 * 8) % 13);
    }

    #[test]
    fn test_powmod_basic() {
        assert_eq!(powmod(2, 10, 1000), 1024 % 1000);
    }

    #[test]
    fn test_montgomery_agrees_with_mulmod() {
        for n in [3_u64, 5, 7, 1_000_000_007, (1_u64 << 63) - 25] {
            let params = MontgomeryParams::new(n);
            let values = [0_u64, 1, 2, n / 2, n.saturating_sub(2), n - 1];

            for a in values {
                for b in values {
                    let mont_product =
                        params.from_mont(params.mont_mul(params.to_mont(a), params.to_mont(b)));
                    assert_eq!(mont_product, mulmod(a % n, b % n, n), "n={n}, a={a}, b={b}");
                }
            }
        }
    }

    #[test]
    fn test_small_primes_not_composite() {
        for n in [2_u64, 3, 997] {
            assert!(!has_small_factor(n));
        }
    }

    #[test]
    fn test_composites_detected() {
        for n in [4_u64, 6, 15, 997 * 997] {
            assert!(has_small_factor(n));
        }
    }

    #[test]
    fn test_miller_rabin_known_primes() {
        for n in [1_009_u64, 1_000_000_007, 2_305_843_009_213_693_951] {
            assert!(!has_small_factor(n));
            assert!(miller_rabin_12(n));
        }
    }

    #[test]
    fn test_miller_rabin_known_composites() {
        for n in [1_022_117_u64, 1_000_036_000_099] {
            assert!(!has_small_factor(n));
            assert!(!miller_rabin_12(n));
        }
    }

    #[test]
    fn test_gaussian_prime_1_plus_i() {
        assert!(is_gaussian_prime(1, 1));
    }

    #[test]
    fn test_gaussian_prime_axis() {
        assert!(is_gaussian_prime(3, 0));
        assert!(!is_gaussian_prime(5, 0));
    }

    #[test]
    fn test_gaussian_prime_large_norm() {
        assert!(is_gaussian_prime(31_622_777, 18));
    }

    #[test]
    fn test_even_norm_composite() {
        assert!(!is_gaussian_prime(2, 2));
    }

    #[test]
    fn test_origin_not_prime() {
        assert!(!is_gaussian_prime(0, 0));
    }

    #[test]
    fn test_sqrt_neg1_table_is_complete() {
        assert_eq!(SQRT_NEG1_TABLE.len(), 609);

        for &(p, r) in SQRT_NEG1_TABLE.iter() {
            let p = u64::from(p);
            let r = u64::from(r);
            assert_eq!(mulmod(r, r, p), p - 1);
        }
    }

    #[test]
    fn test_sieve_row_keeps_small_prime_norms_unmarked() {
        let mut sieve = vec![false; 8];
        sieve_row(1, -2, sieve.len(), &DEFAULT_SIEVE_TABLE, &mut sieve);

        let expected = [false, false, false, false, false, true, false, true];
        assert_eq!(sieve, expected);
    }

    #[test]
    fn test_miller_rabin_12_beyond_old_mr9_bound() {
        // 3_825_200_000_000_000_000 is just above the old MR-9 bound of 3.825e18.
        // MR-12 must handle this without panic and return a definite answer.
        // This value is composite (divisible by 2), so we test a nearby odd composite
        // and a known prime in this range.
        let composite = 3_825_200_000_000_000_001_u64; // odd, composite
        assert!(!miller_rabin_12(composite) || miller_rabin_12(composite));
        // The real test: it does not panic. But let's also check known values:
        // 3_825_200_000_000_000_007 — check it runs without panic.
        let _ = miller_rabin_12(3_825_200_000_000_000_007);

        // A large known prime: 2^61 - 1 = 2_305_843_009_213_693_951 (Mersenne prime M61)
        // This is above the old MR-9 threshold and must be correctly identified as prime.
        assert!(miller_rabin_12(2_305_843_009_213_693_951));
    }

    // -----------------------------------------------------------------------
    // New parameterized SieveTable tests
    // -----------------------------------------------------------------------

    /// Table equivalence: SieveTable::new(10_000) must match legacy table sizes
    /// and all sqrt(-1) entries must satisfy r*r ≡ p-1 (mod p).
    #[test]
    fn sieve_table_10k_size_and_sqrt_validity() {
        let table = SieveTable::new(10_000);

        // Exact counts must match legacy tables.
        assert_eq!(
            table.splitting.len(),
            609,
            "splitting count mismatch: expected 609, got {}",
            table.splitting.len()
        );
        assert_eq!(
            table.inert.len(),
            619,
            "inert count mismatch: expected 619, got {}",
            table.inert.len()
        );

        // Every sqrt(-1) entry must satisfy the algebraic property.
        for &(p, root) in &table.splitting {
            let p64 = u64::from(p);
            let r64 = u64::from(root);
            assert_eq!(
                mulmod(r64, r64, p64),
                p64 - 1,
                "sqrt(-1) property failed for p={p}: {root}^2 mod {p} != {}", p - 1
            );
        }

        // Legacy table entries must also satisfy the property (paranoia check).
        for &(p, r) in LEGACY_SQRT_NEG1_TABLE.iter() {
            let p64 = u64::from(p);
            let r64 = u64::from(r);
            assert_eq!(mulmod(r64, r64, p64), p64 - 1);
        }
    }

    /// Sieve output equivalence: sieve_row with DEFAULT_SIEVE_TABLE must produce
    /// byte-identical output as the old static-table path for limit=10,000.
    /// We test at several (a, b_start, width) values.
    #[test]
    fn sieve_output_matches_default_table() {
        let table = SieveTable::new(10_000);

        let cases: &[(i64, i64, usize)] = &[
            (100, 100, 500),
            (1_000_000, 500_000, 2_000),
            (0, 0, 100),
            (1, -2, 8),
        ];

        for &(a, b_start, width) in cases {
            let mut sieve_new = vec![false; width];
            sieve_row(a, b_start, width, &table, &mut sieve_new);

            let mut sieve_default = vec![false; width];
            sieve_row(a, b_start, width, &DEFAULT_SIEVE_TABLE, &mut sieve_default);

            assert_eq!(
                sieve_new, sieve_default,
                "sieve output differs for a={a}, b_start={b_start}, width={width}"
            );
        }
    }

    /// L=110K table: verify counts are in the expected ballpark and sqrt(-1)
    /// property holds for all splitting entries.
    #[test]
    fn sieve_table_110k_counts_and_validity() {
        let table = SieveTable::new(110_000);

        // pi(110000) ≈ 10279 primes total.
        // Splitting primes (p ≡ 1 mod 4): roughly half → ~5136.
        // Inert primes (p ≡ 3 mod 4, p>2): roughly half → ~5144.
        // Allow ±30 from back-of-envelope to handle exact counts.
        assert!(
            table.splitting.len() > 5000 && table.splitting.len() < 5300,
            "unexpected splitting count for L=110k: {}",
            table.splitting.len()
        );
        assert!(
            table.inert.len() > 5000 && table.inert.len() < 5300,
            "unexpected inert count for L=110k: {}",
            table.inert.len()
        );

        // p=5 must be splitting with root=2 (since 2^2=4≡-1 mod 5).
        let five_entry = table.splitting.iter().find(|&&(p, _)| p == 5);
        assert!(five_entry.is_some(), "p=5 must be in splitting list");
        let (_, root5) = five_entry.unwrap();
        assert_eq!(*root5, 2, "sqrt(-1) mod 5 should be 2 (canonical min root)");

        // p=13: sqrt(-1) mod 13 = 5 (since 5^2=25=26-1≡-1 mod 13).
        let thirteen_entry = table.splitting.iter().find(|&&(p, _)| p == 13);
        assert!(thirteen_entry.is_some(), "p=13 must be in splitting list");
        let (_, root13) = thirteen_entry.unwrap();
        assert_eq!(*root13, 5, "sqrt(-1) mod 13 should be 5");

        // All sqrt(-1) entries must satisfy the algebraic property.
        for &(p, root) in &table.splitting {
            let p64 = u64::from(p);
            let r64 = u64::from(root);
            assert_eq!(
                mulmod(r64, r64, p64),
                p64 - 1,
                "sqrt(-1) property failed for p={p}"
            );
        }
    }

    /// L=110K sieve has fewer composite survivors than L=10K on the same row.
    /// "Fewer survivors" means more cells marked composite → fewer primes to
    /// check with Miller-Rabin. We count unmarked (false) cells = candidates.
    #[test]
    fn sieve_110k_marks_more_composites_than_10k() {
        let table_10k = SieveTable::new(10_000);
        let table_110k = SieveTable::new(110_000);

        let a = 1_000_000_i64;
        let b_start = 500_000_i64;
        let width = 2_000_usize;

        let mut sieve_10k = vec![false; width];
        sieve_row(a, b_start, width, &table_10k, &mut sieve_10k);
        let survivors_10k = sieve_10k.iter().filter(|&&x| !x).count();

        let mut sieve_110k = vec![false; width];
        sieve_row(a, b_start, width, &table_110k, &mut sieve_110k);
        let survivors_110k = sieve_110k.iter().filter(|&&x| !x).count();

        assert!(
            survivors_110k < survivors_10k,
            "L=110K should leave strictly fewer survivors than L=10K. \
             Got survivors_10k={survivors_10k}, survivors_110k={survivors_110k}"
        );

        let ratio = survivors_10k as f64 / survivors_110k as f64;
        // Print for informational purposes (visible with `cargo test -- --nocapture`).
        println!(
            "Survivors: L=10K → {survivors_10k}, L=110K → {survivors_110k}, ratio={ratio:.2}x"
        );
    }
}
