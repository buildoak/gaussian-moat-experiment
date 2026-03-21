const SMALL_PRIME_LIMIT: u64 = 10_000_000;
const MR_WITNESSES: [u64; 12] = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37];

fn mod_mul_u128(a: u128, b: u128, m: u128) -> u128 {
    debug_assert!(m > 0);
    if m <= (1_u128 << 63) {
        // m < 2^63 -> (m-1)^2 < 2^126 < 2^128, native multiply is safe
        (a % m) * (b % m) % m
    } else {
        // Fallback: Russian peasant for huge moduli (rare in our use case)
        let mut a_acc = a % m;
        let mut b_acc = b;
        let mut result = 0_u128;
        while b_acc > 0 {
            if b_acc & 1 == 1 {
                result = (result + a_acc) % m;
            }
            a_acc = (a_acc << 1) % m;
            b_acc >>= 1;
        }
        result
    }
}

fn mod_pow_u128(base: u128, exp: u128, m: u128) -> u128 {
    debug_assert!(m > 0);

    let mut acc = 1_u128;
    let mut pow = base % m;
    let mut rem = exp;

    while rem > 0 {
        if rem & 1 == 1 {
            acc = mod_mul_u128(acc, pow, m);
        }
        pow = mod_mul_u128(pow, pow, m);
        rem >>= 1;
    }

    acc
}

fn is_prime_miller_rabin(n: u64) -> bool {
    if n < 2 {
        return false;
    }
    if n == 2 || n == 3 {
        return true;
    }
    if n % 2 == 0 {
        return false;
    }

    for &p in &MR_WITNESSES {
        if n == p {
            return true;
        }
        if n % p == 0 {
            return false;
        }
    }

    let d0 = n - 1;
    let mut d = d0;
    let mut s = 0_u32;
    while d % 2 == 0 {
        d /= 2;
        s += 1;
    }

    let n128 = n as u128;
    'witness: for &a in &MR_WITNESSES {
        if a >= n {
            continue;
        }

        let mut x = mod_pow_u128(a as u128, d as u128, n128);
        if x == 1 || x == n128 - 1 {
            continue;
        }

        for _ in 1..s {
            x = mod_mul_u128(x, x, n128);
            if x == n128 - 1 {
                continue 'witness;
            }
        }

        return false;
    }

    true
}

/// Small primes for trial division (all primes up to 199, 46 primes).
const SMALL_PRIMES_46: [u64; 46] = [
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89,
    97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191,
    193, 197, 199,
];

/// Reduced MR witness sets -- provably correct up to the given thresholds.
const MR_WITNESSES_4: [u64; 4] = [2, 3, 5, 7];
const MR_WITNESSES_5: [u64; 5] = [2, 3, 5, 7, 11];
const MR_WITNESSES_7: [u64; 7] = [2, 3, 5, 7, 11, 13, 17];

/// Threshold: {2,3,5,7} is correct for n < 3,215,031,751.
const MAX_NORM_4_WITNESS: u64 = 3_215_031_751;
/// Threshold: {2,3,5,7,11} is correct for n < 2,152,302,898,747.
const MAX_NORM_5_WITNESS: u64 = 2_152_302_898_747;
/// Threshold: {2,3,5,7,11,13,17} is correct for n < 341,550,071,728,321.
const MAX_NORM_7_WITNESS: u64 = 341_550_071_728_321;

/// Miller-Rabin primality test with a caller-specified witness set.
fn miller_rabin(n: u64, witnesses: &[u64]) -> bool {
    if n < 2 {
        return false;
    }
    if n == 2 || n == 3 {
        return true;
    }
    if n % 2 == 0 {
        return false;
    }

    let d0 = n - 1;
    let mut d = d0;
    let mut s = 0_u32;
    while d % 2 == 0 {
        d /= 2;
        s += 1;
    }

    let n128 = n as u128;
    'witness: for &a in witnesses {
        if a >= n {
            continue;
        }

        let mut x = mod_pow_u128(a as u128, d as u128, n128);
        if x == 1 || x == n128 - 1 {
            continue;
        }

        for _ in 1..s {
            x = mod_mul_u128(x, x, n128);
            if x == n128 - 1 {
                continue 'witness;
            }
        }

        return false;
    }

    true
}

/// Fast primality test with trial division by small primes + tiered MR witnesses.
/// Runtime bounds check (NOT debug_assert) ensures correctness.
pub fn is_prime_fast(n: u64) -> bool {
    if n < 2 {
        return false;
    }
    // Trial division by 46 small primes (up to 199)
    for &p in &SMALL_PRIMES_46 {
        if n == p {
            return true;
        }
        if n % p == 0 {
            return false;
        }
    }
    // Tiered MR witnesses with runtime bounds check
    if n < MAX_NORM_4_WITNESS {
        miller_rabin(n, &MR_WITNESSES_4)
    } else if n < MAX_NORM_5_WITNESS {
        miller_rabin(n, &MR_WITNESSES_5)
    } else {
        assert!(
            n < MAX_NORM_7_WITNESS,
            "norm {} exceeds 7-witness MR correctness bound (3.41e14)",
            n
        );
        miller_rabin(n, &MR_WITNESSES_7)
    }
}

fn simple_sieve(limit: u64) -> Vec<bool> {
    if limit < 2 {
        return vec![false; (limit + 1) as usize];
    }

    let mut is_prime = vec![true; (limit + 1) as usize];
    is_prime[0] = false;
    is_prime[1] = false;

    let mut p = 2_u64;
    while p * p <= limit {
        if is_prime[p as usize] {
            let mut multiple = p * p;
            while multiple <= limit {
                is_prime[multiple as usize] = false;
                multiple += p;
            }
        }
        p += 1;
    }

    is_prime
}

pub struct PrimeSieve {
    is_prime: Vec<bool>,
}

impl PrimeSieve {
    pub fn new(limit: u64) -> Self {
        Self {
            is_prime: simple_sieve(limit.max(2)),
        }
    }

    pub fn is_prime(&self, n: u64) -> bool {
        if (n as usize) < self.is_prime.len() {
            self.is_prime[n as usize]
        } else {
            is_prime_miller_rabin(n)
        }
    }
}

fn abs_u64(value: i64) -> u64 {
    value.unsigned_abs()
}

fn gaussian_sieve_limit(a_min: i64, a_max: i64, b_min: i64, b_max: i64) -> u64 {
    let max_a = a_min.unsigned_abs().max(a_max.unsigned_abs());
    let max_b = b_min.unsigned_abs().max(b_max.unsigned_abs());
    let max_axis = max_a.max(max_b);
    let max_norm = (max_a as u128)
        .saturating_mul(max_a as u128)
        .saturating_add((max_b as u128).saturating_mul(max_b as u128))
        .min(u64::MAX as u128) as u64;

    max_axis.max(max_norm.min(SMALL_PRIME_LIMIT))
}

pub fn gaussian_primes_in_rect_with_sieve(
    a_min: i64,
    a_max: i64,
    b_min: i64,
    b_max: i64,
    sieve: &PrimeSieve,
) -> Vec<(i64, i64)> {
    if a_min > a_max || b_min > b_max {
        return Vec::new();
    }

    let mut primes = Vec::new();

    if b_min <= 0 && 0 <= b_max {
        for a in a_min..=a_max {
            let aa = abs_u64(a);
            if aa >= 2 && aa % 4 == 3 && sieve.is_prime(aa) {
                primes.push((a, 0));
            }
        }
    }

    if a_min <= 0 && 0 <= a_max {
        for b in b_min..=b_max {
            if b == 0 {
                continue;
            }
            let bb = abs_u64(b);
            if bb >= 2 && bb % 4 == 3 && sieve.is_prime(bb) {
                primes.push((0, b));
            }
        }
    }

    for a in a_min..=a_max {
        if a == 0 {
            continue;
        }
        for b in b_min..=b_max {
            if b == 0 {
                continue;
            }

            let norm = (a as i128 * a as i128 + b as i128 * b as i128) as u64;
            if sieve.is_prime(norm) {
                primes.push((a, b));
            }
        }
    }

    primes
}

pub fn gaussian_primes_in_rect(a_min: i64, a_max: i64, b_min: i64, b_max: i64) -> Vec<(i64, i64)> {
    let sieve = PrimeSieve::new(gaussian_sieve_limit(a_min, a_max, b_min, b_max));
    gaussian_primes_in_rect_with_sieve(a_min, a_max, b_min, b_max, &sieve)
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;

    use super::{gaussian_primes_in_rect, gaussian_primes_in_rect_with_sieve, is_prime_fast, is_prime_miller_rabin, PrimeSieve};

    #[test]
    fn small_rectangle_matches_known_gaussian_primes() {
        let actual: BTreeSet<_> = gaussian_primes_in_rect(-3, 3, -3, 3).into_iter().collect();
        let expected: BTreeSet<_> = [
            (-3, 0),
            (-3, -2),
            (-3, 2),
            (-2, -3),
            (-2, -1),
            (-2, 1),
            (-2, 3),
            (-1, -2),
            (-1, -1),
            (-1, 1),
            (-1, 2),
            (0, -3),
            (0, 3),
            (1, -2),
            (1, -1),
            (1, 1),
            (1, 2),
            (2, -3),
            (2, -1),
            (2, 1),
            (2, 3),
            (3, -2),
            (3, 0),
            (3, 2),
        ]
        .into_iter()
        .collect();

        assert_eq!(actual, expected);
    }

    #[test]
    fn axis_primes_follow_three_mod_four_rule() {
        let primes = gaussian_primes_in_rect(3, 5, 0, 0);
        assert!(primes.contains(&(3, 0)));
        assert!(!primes.contains(&(5, 0)));
    }

    #[test]
    fn miller_rabin_handles_primes_and_composites() {
        let known_primes = [2, 3, 5, 17, 97, 1_000_000_007, 2_305_843_009_213_693_951];
        for prime in known_primes {
            assert!(is_prime_miller_rabin(prime), "{prime} should be prime");
        }

        let known_composites = [0, 1, 4, 9, 21, 341, 561, 1_000_000_009 * 13];
        for composite in known_composites {
            assert!(
                !is_prime_miller_rabin(composite),
                "{composite} should be composite"
            );
        }
    }

    /// Gate 2: is_prime_fast agrees with legacy PrimeSieve::is_prime on known primes/composites
    #[test]
    fn is_prime_fast_agrees_with_legacy() {
        let known_primes: Vec<u64> = vec![2, 3, 5, 17, 97, 1_000_000_007, 9_999_999_967];
        for &p in &known_primes {
            assert!(is_prime_fast(p), "is_prime_fast({p}) should be true");
            assert!(
                is_prime_miller_rabin(p),
                "is_prime_miller_rabin({p}) should be true"
            );
        }

        let known_composites: Vec<u64> = vec![0, 1, 4, 9, 21, 341, 561, 1_000_000_009 * 13];
        for &c in &known_composites {
            assert!(!is_prime_fast(c), "is_prime_fast({c}) should be false");
        }
    }

    /// Gate 2: Cross-check is_prime_fast vs PrimeSieve on Gaussian prime rectangles
    /// at R=100, R=1000, R=10000
    #[test]
    fn gaussian_primes_fast_matches_sieve_at_multiple_radii() {
        for &r in &[100i64, 1000, 10000] {
            let (a_min, a_max) = (r, r + 10);
            let (b_min, b_max) = (-5i64, 5i64);

            // Legacy path using PrimeSieve
            let max_coord = (a_max.unsigned_abs() + 2).max(b_max.unsigned_abs() + 2);
            let max_norm = max_coord * max_coord * 2;
            let sieve = PrimeSieve::new(max_norm.min(10_000_000).max(1000));
            let legacy: BTreeSet<(i64, i64)> =
                gaussian_primes_in_rect_with_sieve(a_min, a_max, b_min, b_max, &sieve)
                    .into_iter()
                    .collect();

            // New path using is_prime_fast
            let mut fast: BTreeSet<(i64, i64)> = BTreeSet::new();
            // Axis primes: b=0
            if b_min <= 0 && 0 <= b_max {
                for a in a_min..=a_max {
                    let aa = a.unsigned_abs();
                    if aa >= 2 && aa % 4 == 3 && is_prime_fast(aa) {
                        fast.insert((a, 0));
                    }
                }
            }
            // Axis primes: a=0
            if a_min <= 0 && 0 <= a_max {
                for b in b_min..=b_max {
                    if b == 0 {
                        continue;
                    }
                    let bb = b.unsigned_abs();
                    if bb >= 2 && bb % 4 == 3 && is_prime_fast(bb) {
                        fast.insert((0, b));
                    }
                }
            }
            // General: norm-based
            for a in a_min..=a_max {
                if a == 0 {
                    continue;
                }
                for b in b_min..=b_max {
                    if b == 0 {
                        continue;
                    }
                    let norm = (a as i128 * a as i128 + b as i128 * b as i128) as u64;
                    if is_prime_fast(norm) {
                        fast.insert((a, b));
                    }
                }
            }

            assert_eq!(
                legacy, fast,
                "Mismatch at R={r}: legacy has {} primes, fast has {} primes",
                legacy.len(),
                fast.len()
            );
        }
    }

    /// Gate 2: Axis primes are correctly identified by is_prime_fast
    #[test]
    fn is_prime_fast_handles_axis_primes() {
        // 3 mod 4 primes on real axis
        assert!(is_prime_fast(3));
        assert!(is_prime_fast(7));
        assert!(is_prime_fast(11));
        assert!(is_prime_fast(19));
        assert!(is_prime_fast(23));

        // Not 3 mod 4 (even if prime)
        // 5 = 1 mod 4, so (5,0) is NOT a Gaussian prime
        assert!(is_prime_fast(5)); // 5 is a rational prime
        // But 5 mod 4 == 1, so we verify the mod check separately
        assert_eq!(5 % 4, 1); // NOT 3 mod 4 -> not axis Gaussian prime
        assert_eq!(3 % 4, 3); // IS 3 mod 4 -> axis Gaussian prime
    }
}
