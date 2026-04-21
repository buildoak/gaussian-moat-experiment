use std::f64::consts::FRAC_PI_4;

use crate::sieve::GaussianPrime;

#[derive(Clone)]
pub struct WedgeBuffer {
    pub primes: Vec<GaussianPrime>,
    /// For each prime, true if this wedge is the prime's primary assignment.
    pub is_native: Vec<bool>,
    /// For each prime, true if the prime is also present in wedge_id - 1.
    pub shared_with_left: Vec<bool>,
    /// For each prime, true if the prime is also present in wedge_id + 1.
    pub shared_with_right: Vec<bool>,
    pub angle_lo: f64,
    pub angle_hi: f64,
}

#[inline]
fn canonical(a: i32, b: i32) -> (i32, i32) {
    let (x, y) = (a.abs(), b.abs());
    (x.max(y), x.min(y))
}

pub fn route_primes(
    primes: &[GaussianPrime],
    num_wedges: u32,
    k_squared: u64,
) -> Vec<WedgeBuffer> {
    let wedges = num_wedges.max(1) as usize;
    let wedge_width = FRAC_PI_4 / wedges as f64;

    let mut buffers: Vec<WedgeBuffer> = (0..wedges)
        .map(|w| {
            let angle_lo = w as f64 * wedge_width;
            let angle_hi = if w + 1 == wedges {
                FRAC_PI_4
            } else {
                (w + 1) as f64 * wedge_width
            };
            WedgeBuffer {
                primes: Vec::new(),
                is_native: Vec::new(),
                shared_with_left: Vec::new(),
                shared_with_right: Vec::new(),
                angle_lo,
                angle_hi,
            }
        })
        .collect();

    if wedges == 1 {
        let n = primes.len();
        buffers[0].primes.extend_from_slice(primes);
        buffers[0].is_native.resize(n, true);
        buffers[0].shared_with_left.resize(n, false);
        buffers[0].shared_with_right.resize(n, false);
        return buffers;
    }

    for prime in primes {
        let (a, b) = canonical(prime.a, prime.b);
        let theta = (b as f64).atan2(a as f64);
        let prime_norm = prime.norm.max(1) as f64;
        let overlap_radians = (k_squared as f64).sqrt() / prime_norm.sqrt();

        let primary = ((theta / wedge_width).floor() as i64).clamp(0, wedges as i64 - 1) as usize;

        let lo_theta = (theta - overlap_radians).max(0.0);
        let hi_theta = (theta + overlap_radians).min(FRAC_PI_4);
        let lo_wedge = ((lo_theta / wedge_width).floor() as i64).clamp(0, wedges as i64 - 1) as usize;
        let hi_wedge = if hi_theta >= FRAC_PI_4 {
            wedges - 1
        } else {
            ((hi_theta / wedge_width).floor() as i64).clamp(0, wedges as i64 - 1) as usize
        };

        let start = lo_wedge.min(primary);
        let end = hi_wedge.max(primary);
        for w in start..=end {
            buffers[w].primes.push(*prime);
            buffers[w].is_native.push(w == primary);
            // This prime is shared with left neighbor (w-1) if w-1 >= start
            buffers[w].shared_with_left.push(w > 0 && w - 1 >= start);
            // This prime is shared with right neighbor (w+1) if w+1 <= end
            buffers[w].shared_with_right.push(w + 1 < wedges && w + 1 <= end);
        }
    }

    buffers
}

/// Route a batch of primes into per-wedge vectors for streaming parallel processing.
/// Vecs are cleared and refilled each call (no reallocation thanks to capacity reuse).
/// Returns nothing — results are written into the mutable slice arguments.
pub fn route_batch(
    batch: &[GaussianPrime],
    num_wedges: u32,
    k_squared: u64,
    wedge_assignments: &mut Vec<Vec<(usize, bool, bool, bool)>>,
) {
    let wedges = num_wedges.max(1) as usize;
    let wedge_width = FRAC_PI_4 / wedges as f64;

    for assignments in wedge_assignments.iter_mut() {
        assignments.clear();
    }

    if wedges == 1 {
        for (idx, _) in batch.iter().enumerate() {
            wedge_assignments[0].push((idx, true, false, false));
        }
        return;
    }

    let k_sqrt = (k_squared as f64).sqrt();
    for (idx, prime) in batch.iter().enumerate() {
        let (a, b) = canonical(prime.a, prime.b);
        let theta = (b as f64).atan2(a as f64);
        let prime_norm = prime.norm.max(1) as f64;
        let overlap_radians = k_sqrt / prime_norm.sqrt();

        let primary = ((theta / wedge_width).floor() as i64).clamp(0, wedges as i64 - 1) as usize;

        let lo_theta = (theta - overlap_radians).max(0.0);
        let hi_theta = (theta + overlap_radians).min(FRAC_PI_4);
        let lo_wedge = ((lo_theta / wedge_width).floor() as i64).clamp(0, wedges as i64 - 1) as usize;
        let hi_wedge = if hi_theta >= FRAC_PI_4 {
            wedges - 1
        } else {
            ((hi_theta / wedge_width).floor() as i64).clamp(0, wedges as i64 - 1) as usize
        };

        let start = lo_wedge.min(primary);
        let end = hi_wedge.max(primary);
        for w in start..=end {
            wedge_assignments[w].push((
                idx,
                w == primary,
                w > 0 && w - 1 >= start,
                w + 1 < wedges && w + 1 <= end,
            ));
        }
    }
}
