use std::cmp::Reverse;
use std::collections::{BinaryHeap, VecDeque};

use rayon::prelude::*;

const DEFAULT_SEGMENT_SPAN: u64 = 1 << 18; // 262_144 norms
const L1_SUBSEGMENT: usize = 32_768; // 32KB working block
const MAX_PARALLEL_BATCH_SIZE: usize = 64;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GaussianPrime {
    pub a: i32,
    pub b: i32,
    pub norm: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct BufferedPrime {
    a: i32,
    b: i32,
    norm: u64,
}

impl BufferedPrime {
    #[inline]
    fn into_gaussian(self) -> GaussianPrime {
        GaussianPrime {
            a: self.a,
            b: self.b,
            norm: self.norm,
        }
    }
}

#[derive(Default)]
struct SegmentBuckets {
    hits: Vec<(u64, usize)>, // (prime, first offset in segment)
}

pub struct PrimeStream {
    norm_bound: u64,
    segment_span: u64,
    next_segment_lo: u64,
    base_primes: Vec<u64>, // cached exactly once at construction
    small_prime_count: usize,
    buffer: VecDeque<BufferedPrime>,
    exhausted: bool,
    parallel_batch_size: usize,
}

impl PrimeStream {
    pub fn new(norm_bound: u64) -> Self {
        Self::new_with_start(0, norm_bound)
    }

    pub fn new_with_start(start_norm: u64, norm_bound: u64) -> Self {
        let segment_span = DEFAULT_SEGMENT_SPAN;
        let base_primes = simple_sieve(isqrt(norm_bound));
        let small_prime_count = base_primes.partition_point(|&p| p <= segment_span);
        let parallel_batch_size = num_cpus::get().clamp(1, MAX_PARALLEL_BATCH_SIZE);

        Self {
            norm_bound,
            segment_span,
            next_segment_lo: start_norm.min(norm_bound.saturating_add(1)),
            base_primes,
            small_prime_count,
            buffer: VecDeque::new(),
            exhausted: start_norm > norm_bound,
            parallel_batch_size,
        }
    }

    fn fill_next_batch(&mut self) {
        if self.exhausted {
            return;
        }
        if self.next_segment_lo > self.norm_bound {
            self.exhausted = true;
            return;
        }

        let mut segment_ranges = Vec::with_capacity(self.parallel_batch_size);
        let mut lo = self.next_segment_lo;
        for _ in 0..self.parallel_batch_size {
            if lo > self.norm_bound {
                break;
            }

            let hi_exclusive = lo
                .saturating_add(self.segment_span)
                .min(self.norm_bound.saturating_add(1));
            if hi_exclusive <= lo {
                break;
            }
            segment_ranges.push((lo, hi_exclusive));
            lo = hi_exclusive;
        }

        if segment_ranges.is_empty() {
            self.exhausted = true;
            return;
        }

        let small_primes = &self.base_primes[..self.small_prime_count];
        let large_primes = &self.base_primes[self.small_prime_count..];
        let buckets = build_large_prime_buckets(large_primes, self.segment_span, &segment_ranges);

        let segment_results: Vec<Vec<BufferedPrime>> = segment_ranges
            .par_iter()
            .zip(buckets.par_iter())
            .map(|(&(lo, hi_exclusive), bucket)| {
                process_segment(small_primes, bucket, lo, hi_exclusive)
            })
            .collect();

        let total = segment_results.iter().map(Vec::len).sum();
        let mut merged = Vec::with_capacity(total);

        #[allow(clippy::type_complexity)]
        let mut heap: BinaryHeap<Reverse<(u64, i32, i32, usize, usize)>> = BinaryHeap::new();
        for (seg_idx, seg) in segment_results.iter().enumerate() {
            if let Some(first) = seg.first() {
                heap.push(Reverse((first.norm, first.a, first.b, seg_idx, 0)));
            }
        }

        while let Some(Reverse((_, _, _, seg_idx, pos))) = heap.pop() {
            let entry = segment_results[seg_idx][pos];
            merged.push(entry);

            let next_pos = pos + 1;
            if let Some(next) = segment_results[seg_idx].get(next_pos) {
                heap.push(Reverse((next.norm, next.a, next.b, seg_idx, next_pos)));
            }
        }

        self.buffer.extend(merged);

        self.next_segment_lo = segment_ranges
            .last()
            .map_or(self.next_segment_lo, |&(_, hi)| hi);
        if self.next_segment_lo > self.norm_bound {
            self.exhausted = true;
        }
    }
}

impl Iterator for PrimeStream {
    type Item = GaussianPrime;

    fn next(&mut self) -> Option<Self::Item> {
        loop {
            if let Some(entry) = self.buffer.pop_front() {
                return Some(entry.into_gaussian());
            }

            if self.exhausted {
                return None;
            }

            self.fill_next_batch();
        }
    }
}

fn build_large_prime_buckets(
    large_primes: &[u64],
    segment_span: u64,
    segment_ranges: &[(u64, u64)],
) -> Vec<SegmentBuckets> {
    let mut buckets: Vec<SegmentBuckets> = (0..segment_ranges.len())
        .map(|_| SegmentBuckets::default())
        .collect();
    if segment_ranges.is_empty() || large_primes.is_empty() {
        return buckets;
    }

    let batch_lo = segment_ranges[0].0.max(2);
    let batch_hi = segment_ranges.last().map_or(batch_lo, |&(_, hi)| hi);
    let first_segment_lo = segment_ranges[0].0;

    for &p in large_primes {
        let p_sq = (p as u128) * (p as u128);
        if p_sq >= batch_hi as u128 {
            break;
        }

        let first = if p_sq >= batch_lo as u128 {
            p_sq as u64
        } else {
            let rem = batch_lo % p;
            if rem == 0 {
                batch_lo
            } else {
                batch_lo + (p - rem)
            }
        };

        let mut m = first;
        while m < batch_hi {
            let seg_idx = ((m - first_segment_lo) / segment_span) as usize;
            if seg_idx >= segment_ranges.len() {
                break;
            }

            let seg_lo = segment_ranges[seg_idx].0.max(2);
            if m >= seg_lo {
                buckets[seg_idx].hits.push((p, (m - seg_lo) as usize));
            }

            if let Some(next) = m.checked_add(p) {
                m = next;
            } else {
                break;
            }
        }
    }

    buckets
}

fn process_segment(
    small_primes: &[u64],
    bucket: &SegmentBuckets,
    lo: u64,
    hi_exclusive: u64,
) -> Vec<BufferedPrime> {
    if hi_exclusive <= lo {
        return Vec::new();
    }

    let mut out = Vec::new();

    for p in sieve_range_bucketed(small_primes, bucket, lo, hi_exclusive) {
        if p == 2 {
            out.push(BufferedPrime {
                a: 1,
                b: 1,
                norm: 2,
            });
        } else if p % 4 == 1 {
            if let Some((a, b)) = cornacchia(p) {
                out.push(BufferedPrime {
                    a: to_i32(a),
                    b: to_i32(b),
                    norm: p,
                });
            }
        }
    }

    let inert_lo = isqrt_ceil(lo.max(1)).max(3);
    let inert_hi = isqrt(hi_exclusive - 1);
    if inert_lo <= inert_hi {
        for p in sieve_range(small_primes, inert_lo, inert_hi.saturating_add(1)) {
            if p % 4 == 3 {
                let norm = (p as u128) * (p as u128);
                if norm >= lo as u128 && norm < hi_exclusive as u128 {
                    out.push(BufferedPrime {
                        a: to_i32(p),
                        b: 0,
                        norm: norm as u64,
                    });
                }
            }
        }
    }

    out.sort_unstable_by_key(|entry| (entry.norm, entry.a, entry.b));
    out
}

fn sieve_range_bucketed(
    small_primes: &[u64],
    bucket: &SegmentBuckets,
    lo: u64,
    hi_exclusive: u64,
) -> Vec<u64> {
    if hi_exclusive <= lo || hi_exclusive <= 2 {
        return Vec::new();
    }

    let seg_lo = lo.max(2);
    if seg_lo >= hi_exclusive {
        return Vec::new();
    }

    let seg_hi = hi_exclusive - 1;
    let len = (hi_exclusive - seg_lo) as usize;
    let mut composite = vec![false; len];

    let mut cursors: Vec<(usize, usize)> = Vec::new();
    for &p in small_primes {
        let p_sq = (p as u128) * (p as u128);
        if p_sq > seg_hi as u128 {
            break;
        }

        let first_offset = if p_sq >= seg_lo as u128 {
            (p_sq as u64 - seg_lo) as usize
        } else {
            let rem = seg_lo % p;
            if rem == 0 {
                0
            } else {
                (p - rem) as usize
            }
        };

        cursors.push((p as usize, first_offset));
    }

    let mut block_lo = 0usize;
    while block_lo < len {
        let block_hi = (block_lo + L1_SUBSEGMENT).min(len);
        for (step, offset) in &mut cursors {
            while *offset < block_hi {
                composite[*offset] = true;
                *offset += *step;
            }
        }
        block_lo = block_hi;
    }

    for &(p, first_offset) in &bucket.hits {
        let mut idx = first_offset;
        let step = p as usize;
        while idx < len {
            composite[idx] = true;
            idx += step;
        }
    }

    let mut primes = Vec::new();
    for (idx, &is_composite) in composite.iter().enumerate() {
        if !is_composite {
            primes.push(seg_lo + idx as u64);
        }
    }
    primes
}

fn sieve_range(base_primes: &[u64], lo: u64, hi_exclusive: u64) -> Vec<u64> {
    if hi_exclusive <= lo || hi_exclusive <= 2 {
        return Vec::new();
    }
    let seg_lo = lo.max(2);
    if seg_lo >= hi_exclusive {
        return Vec::new();
    }

    let seg_hi = hi_exclusive - 1;
    let len = (hi_exclusive - seg_lo) as usize;
    let mut composite = vec![false; len];

    for &p in base_primes {
        let p_sq = (p as u128) * (p as u128);
        if p_sq > seg_hi as u128 {
            break;
        }

        let first = if p_sq >= seg_lo as u128 {
            p_sq as u64
        } else {
            let rem = seg_lo % p;
            if rem == 0 {
                seg_lo
            } else {
                seg_lo + (p - rem)
            }
        };

        let mut m = first;
        while m < hi_exclusive {
            composite[(m - seg_lo) as usize] = true;
            m += p;
        }
    }

    let mut out = Vec::new();
    for (idx, &is_composite) in composite.iter().enumerate() {
        if !is_composite {
            out.push(seg_lo + idx as u64);
        }
    }
    out
}

fn simple_sieve(limit: u64) -> Vec<u64> {
    if limit < 2 {
        return Vec::new();
    }

    let n = limit as usize;
    let mut composite = vec![false; n + 1];
    composite[0] = true;
    composite[1] = true;

    let mut p = 2usize;
    while (p as u128) * (p as u128) <= limit as u128 {
        if !composite[p] {
            let mut m = p * p;
            while m <= n {
                composite[m] = true;
                m += p;
            }
        }
        p += 1;
    }

    let mut primes = Vec::new();
    for (v, &is_composite) in composite.iter().enumerate().skip(2) {
        if !is_composite {
            primes.push(v as u64);
        }
    }
    primes
}

fn cornacchia(p: u64) -> Option<(u64, u64)> {
    if p <= 2 || p % 4 != 1 {
        return None;
    }

    let mut r = fast_sqrt_neg1(p)?;
    if r <= p / 2 {
        r = p - r;
    }

    let limit = isqrt(p);
    let mut r0 = p;
    let mut r1 = r;
    while r1 > limit {
        let tmp = r0 % r1;
        r0 = r1;
        r1 = tmp;
    }

    let a = r1;
    let b_sq = p as u128 - (a as u128) * (a as u128);
    let b = isqrt(b_sq as u64);
    if (b as u128) * (b as u128) != b_sq {
        return None;
    }

    if a >= b {
        Some((a, b))
    } else {
        Some((b, a))
    }
}

fn tonelli_shanks(n: u64, p: u64) -> Option<u64> {
    if p == 2 {
        return Some(n % p);
    }
    let n = n % p;
    if n == 0 {
        return Some(0);
    }
    if mod_pow(n, (p - 1) / 2, p) != 1 {
        return None;
    }
    if p % 4 == 3 {
        return Some(mod_pow(n, (p + 1) / 4, p));
    }

    let mut q = p - 1;
    let mut s = 0u32;
    while q.is_multiple_of(2) {
        q /= 2;
        s += 1;
    }

    let mut z = 2u64;
    while mod_pow(z, (p - 1) / 2, p) != p - 1 {
        z += 1;
    }

    let mut m = s;
    let mut c = mod_pow(z, q, p);
    let mut t = mod_pow(n, q, p);
    let mut r = mod_pow(n, q.div_ceil(2), p);

    while t != 1 {
        let mut i = 1u32;
        let mut t2 = mod_mul(t, t, p);
        while t2 != 1 {
            t2 = mod_mul(t2, t2, p);
            i += 1;
            if i >= m {
                return None;
            }
        }

        let b = mod_pow(c, 1u64 << (m - i - 1), p);
        r = mod_mul(r, b, p);
        c = mod_mul(b, b, p);
        t = mod_mul(t, c, p);
        m = i;
    }

    Some(r)
}

#[inline]
fn mod_mul(a: u64, b: u64, m: u64) -> u64 {
    ((a as u128 * b as u128) % m as u128) as u64
}

#[inline]
fn mod_pow(mut base: u64, mut exp: u64, m: u64) -> u64 {
    if m == 1 {
        return 0;
    }

    let mut result = 1u64;
    base %= m;
    while exp > 0 {
        if exp & 1 == 1 {
            result = mod_mul(result, base, m);
        }
        base = mod_mul(base, base, m);
        exp >>= 1;
    }
    result
}

fn fast_sqrt_neg1(p: u64) -> Option<u64> {
    if p <= 2 || p % 4 != 1 {
        return None;
    }

    let exp = (p - 1) >> 2;
    if p % 8 == 5 {
        let r = mod_pow(2, exp, p);
        if mod_mul(r, r, p) == p - 1 {
            return Some(r);
        }
    }

    tonelli_shanks(p - 1, p)
}

#[inline]
fn to_i32(v: u64) -> i32 {
    i32::try_from(v).unwrap_or_else(|_| panic!("value {v} does not fit in i32"))
}

#[inline]
fn isqrt(n: u64) -> u64 {
    let mut x = (n as f64).sqrt() as u64;
    while (x as u128 + 1) * (x as u128 + 1) <= n as u128 {
        x += 1;
    }
    while (x as u128) * (x as u128) > n as u128 {
        x -= 1;
    }
    x
}

#[inline]
fn isqrt_ceil(n: u64) -> u64 {
    let floor = isqrt(n);
    if (floor as u128) * (floor as u128) == n as u128 {
        floor
    } else {
        floor + 1
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn first_twenty_gaussian_primes_match_reference() {
        let got: Vec<(i32, i32, u64)> = PrimeStream::new(200)
            .take(20)
            .map(|g| (g.a, g.b, g.norm))
            .collect();

        let expected = vec![
            (1, 1, 2),
            (2, 1, 5),
            (3, 0, 9),
            (3, 2, 13),
            (4, 1, 17),
            (5, 2, 29),
            (6, 1, 37),
            (5, 4, 41),
            (7, 0, 49),
            (7, 2, 53),
            (6, 5, 61),
            (8, 3, 73),
            (8, 5, 89),
            (9, 4, 97),
            (10, 1, 101),
            (10, 3, 109),
            (8, 7, 113),
            (11, 0, 121),
            (11, 4, 137),
            (10, 7, 149),
        ];
        assert_eq!(got, expected);
    }

    #[test]
    fn new_with_start_respects_lower_bound() {
        let got: Vec<u64> = PrimeStream::new_with_start(50, 150)
            .map(|g| g.norm)
            .collect();

        assert!(!got.is_empty());
        assert!(got.iter().all(|&n| (50..=150).contains(&n)));
        assert_eq!(got[0], 53);
    }

    #[test]
    fn stream_is_sorted_and_first_octant() {
        let mut prev = (0u64, 0i32, 0i32);
        for gp in PrimeStream::new(10_000).take(1_000) {
            assert!(gp.a >= gp.b && gp.b >= 0);
            let key = (gp.norm, gp.a, gp.b);
            assert!(key >= prev);
            prev = key;
        }
    }

    #[test]
    fn base_primes_are_cached_once() {
        let mut stream = PrimeStream::new(1_000_000);
        let ptr = stream.base_primes.as_ptr();
        let len = stream.base_primes.len();

        for _ in 0..5_000 {
            let _ = stream.next();
        }

        assert_eq!(len, stream.base_primes.len());
        assert_eq!(ptr, stream.base_primes.as_ptr());
    }
}
