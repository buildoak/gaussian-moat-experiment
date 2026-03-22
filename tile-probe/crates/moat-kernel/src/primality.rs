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

pub const MR_WITNESSES_9: [u64; 9] = [2, 3, 5, 7, 11, 13, 17, 19, 23];
pub const ROW_SIEVE_LIMIT: u16 = 10_000;

pub static SQRT_NEG1_TABLE: LazyLock<Vec<(u16, u16)>> = LazyLock::new(|| {
    primes_up_to(ROW_SIEVE_LIMIT)
        .into_iter()
        .filter(|&p| p % 4 == 1)
        .map(|p| {
            let root = tonelli_shanks(u64::from(p - 1), u64::from(p))
                .expect("sqrt(-1) must exist for primes congruent to 1 mod 4");
            let canonical = root.min(u64::from(p) - root) as u16;
            (p, canonical)
        })
        .collect()
});

static MOD_3_SIEVE_PRIMES: LazyLock<Vec<u16>> = LazyLock::new(|| {
    primes_up_to(ROW_SIEVE_LIMIT)
        .into_iter()
        .filter(|&p| p % 4 == 3)
        .collect()
});

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
        if u >= self.n { u - self.n } else { u }
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

fn primes_up_to(limit: u16) -> Vec<u16> {
    if limit < 2 {
        return Vec::new();
    }

    let mut is_prime = vec![true; usize::from(limit) + 1];
    is_prime[0] = false;
    is_prime[1] = false;

    let mut p = 2_usize;
    while p * p <= usize::from(limit) {
        if is_prime[p] {
            let mut multiple = p * p;
            while multiple <= usize::from(limit) {
                is_prime[multiple] = false;
                multiple += p;
            }
        }
        p += 1;
    }

    is_prime
        .iter()
        .enumerate()
        .filter_map(|(n, &prime)| prime.then_some(n as u16))
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

#[inline]
fn mark_residue_class(b_start: i64, width: usize, p: i64, residue: i64, sieve: &mut [bool]) {
    let first = (residue - b_start).rem_euclid(p) as usize;
    for idx in (first..width).step_by(p as usize) {
        sieve[idx] = true;
    }
}

pub fn sieve_row(a: i64, b_start: i64, width: usize, sieve: &mut [bool]) {
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

    for &(p_u16, r_u16) in SQRT_NEG1_TABLE.iter() {
        let p = i64::from(p_u16);
        let residue = (a.rem_euclid(p) * i64::from(r_u16)).rem_euclid(p);
        mark_residue_class(b_start, width, p, residue, sieve);

        let neg_residue = (-residue).rem_euclid(p);
        if neg_residue != residue {
            mark_residue_class(b_start, width, p, neg_residue, sieve);
        }
    }

    for &p_u16 in MOD_3_SIEVE_PRIMES.iter() {
        let p = i64::from(p_u16);
        if a.rem_euclid(p) == 0 {
            mark_residue_class(b_start, width, p, 0, sieve);
        }
    }

    // Small norms can be equal to a sieve prime, so they must stay as MR candidates.
    let a_sq = i128::from(a) * i128::from(a);
    if a_sq <= i128::from(ROW_SIEVE_LIMIT) {
        for (col, marked) in sieve.iter_mut().enumerate() {
            if !*marked {
                continue;
            }

            let b = b_start + col as i64;
            let norm = a_sq + i128::from(b) * i128::from(b);
            if !(2..=i128::from(ROW_SIEVE_LIMIT)).contains(&norm) {
                continue;
            }

            if is_rational_prime(norm as u64) {
                *marked = false;
            }
        }
    }
}

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

pub fn miller_rabin_9(n: u64) -> bool {
    let mut d = n - 1;
    let mut r = 0_u32;

    while d.is_multiple_of(2) {
        d >>= 1;
        r += 1;
    }

    'witness: for &a in &MR_WITNESSES_9 {
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

    miller_rabin_9(n)
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

    miller_rabin_9(n)
}

#[cfg(test)]
mod tests {
    use super::{
        has_small_factor, is_gaussian_prime, miller_rabin_9, mulmod, powmod, sieve_row,
        MontgomeryParams, SQRT_NEG1_TABLE,
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
            assert!(miller_rabin_9(n));
        }
    }

    #[test]
    fn test_miller_rabin_known_composites() {
        for n in [1_022_117_u64, 1_000_036_000_099] {
            assert!(!has_small_factor(n));
            assert!(!miller_rabin_9(n));
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
        sieve_row(1, -2, sieve.len(), &mut sieve);

        let expected = [false, false, false, false, false, true, false, true];
        assert_eq!(sieve, expected);
    }
}
