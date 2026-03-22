pub const SMALL_PRIMES: [u64; 168] = [
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89,
    97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191,
    193, 197, 199, 211, 223, 227, 229, 233, 239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293,
    307, 311, 313, 317, 331, 337, 347, 349, 353, 359, 367, 373, 379, 383, 389, 397, 401, 409, 419,
    421, 431, 433, 439, 443, 449, 457, 461, 463, 467, 479, 487, 491, 499, 503, 509, 521, 523, 541,
    547, 557, 563, 569, 571, 577, 587, 593, 599, 601, 607, 613, 617, 619, 631, 641, 643, 647, 653,
    659, 661, 673, 677, 683, 691, 701, 709, 719, 727, 733, 739, 743, 751, 757, 761, 769, 773, 787,
    797, 809, 811, 821, 823, 827, 829, 839, 853, 857, 859, 863, 877, 881, 883, 887, 907, 911, 919,
    929, 937, 941, 947, 953, 967, 971, 977, 983, 991, 997,
];

pub const MR_WITNESSES_9: [u64; 9] = [2, 3, 5, 7, 11, 13, 17, 19, 23];

#[inline(always)]
pub fn mulmod(a: u64, b: u64, m: u64) -> u64 {
    ((a as u128) * (b as u128) % (m as u128)) as u64
}

#[inline(always)]
pub fn powmod(base: u64, exp: u64, m: u64) -> u64 {
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
    use super::{has_small_factor, is_gaussian_prime, miller_rabin_9, mulmod, powmod};

    #[test]
    fn test_mulmod_basic() {
        assert_eq!(mulmod(7, 8, 13), (7 * 8) % 13);
    }

    #[test]
    fn test_powmod_basic() {
        assert_eq!(powmod(2, 10, 1000), 1024 % 1000);
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
}
