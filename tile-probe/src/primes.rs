const SMALL_PRIME_LIMIT: u64 = 10_000_000;
const MR_WITNESSES: [u64; 12] = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37];

fn mod_mul_u128(a: u128, b: u128, m: u128) -> u128 {
    debug_assert!(m > 0);

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

fn abs_u64(value: i64) -> u64 {
    value.unsigned_abs()
}

fn is_prime_lookup(value: u64, small_primes: &[bool]) -> bool {
    if (value as usize) < small_primes.len() {
        small_primes[value as usize]
    } else {
        is_prime_miller_rabin(value)
    }
}

pub fn gaussian_primes_in_rect(a_min: i64, a_max: i64, b_min: i64, b_max: i64) -> Vec<(i64, i64)> {
    if a_min > a_max || b_min > b_max {
        return Vec::new();
    }

    let max_a = a_min.unsigned_abs().max(a_max.unsigned_abs());
    let max_b = b_min.unsigned_abs().max(b_max.unsigned_abs());
    let max_axis = max_a.max(max_b);
    let max_norm = (max_a as u128)
        .saturating_mul(max_a as u128)
        .saturating_add((max_b as u128).saturating_mul(max_b as u128))
        .min(u64::MAX as u128) as u64;
    let sieve_limit = max_axis.max(max_norm.min(SMALL_PRIME_LIMIT));
    let small_primes = simple_sieve(sieve_limit.max(2));

    let mut primes = Vec::new();

    if b_min <= 0 && 0 <= b_max {
        for a in a_min..=a_max {
            let aa = abs_u64(a);
            if aa >= 2 && aa % 4 == 3 && is_prime_lookup(aa, &small_primes) {
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
            if bb >= 2 && bb % 4 == 3 && is_prime_lookup(bb, &small_primes) {
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
            if is_prime_lookup(norm, &small_primes) {
                primes.push((a, b));
            }
        }
    }

    primes
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;

    use super::{gaussian_primes_in_rect, is_prime_miller_rabin};

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
}
