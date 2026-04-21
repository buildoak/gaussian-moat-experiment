use std::time::{Duration, Instant};

use crate::band::BandProcessor;
use crate::sieve::PrimeStream;

const PROGRESS_INTERVAL: u64 = 1_000_000;

pub struct ProbeResult {
    pub farthest_a: i32,
    pub farthest_b: i32,
    pub farthest_distance: f64,
    pub component_size: u64,
    pub primes_processed: u64,
    pub elapsed: Duration,
}

pub fn lower_bound_probe(k_squared: u64, norm_bound: u64) -> ProbeResult {
    let bound = effective_norm_bound(norm_bound, k_squared);
    let mut stream = PrimeStream::new(bound);
    let mut band = BandProcessor::new(k_squared);
    run_probe(&mut stream, &mut band)
}

pub fn upper_bound_probe(k_squared: u64, start_distance: u64, norm_bound: u64) -> ProbeResult {
    let bound = effective_norm_bound(norm_bound, k_squared);
    let start_norm = upper_bound_start_norm(k_squared, start_distance);
    let mut stream = PrimeStream::new_with_start(start_norm, bound);
    let mut band = BandProcessor::new_upper_bound(k_squared, start_distance);
    run_probe(&mut stream, &mut band)
}

#[inline]
fn upper_bound_start_norm(k_squared: u64, start_distance: u64) -> u64 {
    let k_radius = ceil_sqrt_u64(k_squared);
    let r = start_distance.saturating_sub(k_radius);
    r.saturating_mul(r)
}

#[inline]
fn ceil_sqrt_u64(n: u64) -> u64 {
    let mut x = (n as f64).sqrt() as u64;
    while (x as u128 + 1) * (x as u128 + 1) <= n as u128 {
        x += 1;
    }
    while (x as u128) * (x as u128) > n as u128 {
        x -= 1;
    }
    if (x as u128) * (x as u128) == n as u128 {
        x
    } else {
        x + 1
    }
}

fn effective_norm_bound(norm_bound: u64, k_squared: u64) -> u64 {
    if norm_bound != 0 {
        return norm_bound;
    }
    // Auto-compute from known Tsuchimura moat distances with generous safety margin.
    // norm_bound = max_distance², where max_distance >> known moat location.
    let max_distance: f64 = match k_squared {
        0..=4 => 200.0,
        5..=10 => 5_000.0,
        11..=16 => 20_000.0,
        17..=20 => 500_000.0,
        21..=26 => 5_000_000.0,
        27..=32 => 20_000_000.0,
        33..=36 => 200_000_000.0,
        _ => (k_squared as f64).powi(4) * 100.0,
    };
    (max_distance * max_distance).min(u64::MAX as f64) as u64
}

fn run_probe(stream: &mut PrimeStream, band: &mut BandProcessor) -> ProbeResult {
    let start = Instant::now();
    let mut primes_processed = 0_u64;

    for prime in stream.by_ref() {
        primes_processed += 1;

        if let Some(moat) = band.process_prime(&prime) {
            let elapsed = start.elapsed();
            return ProbeResult {
                farthest_a: moat.farthest_a,
                farthest_b: moat.farthest_b,
                farthest_distance: moat.farthest_distance,
                component_size: moat.component_size,
                primes_processed,
                elapsed,
            };
        }

        if primes_processed.is_multiple_of(PROGRESS_INTERVAL) {
            print_progress(primes_processed, start.elapsed(), band);
        }
    }

    let elapsed = start.elapsed();
    eprintln!(
        "warning: prime stream exhausted before proving a moat (processed {}, elapsed {:.2}s)",
        primes_processed,
        elapsed.as_secs_f64()
    );

    ProbeResult {
        farthest_a: band.farthest_a(),
        farthest_b: band.farthest_b(),
        farthest_distance: band.farthest_distance(),
        component_size: band.origin_component_size(),
        primes_processed,
        elapsed,
    }
}

fn print_progress(primes_processed: u64, elapsed: Duration, band: &BandProcessor) {
    let stats = band.stats();
    let elapsed_secs = elapsed.as_secs_f64();
    let throughput = if elapsed_secs > 0.0 {
        primes_processed as f64 / elapsed_secs
    } else {
        0.0
    };

    eprintln!(
        "progress: primes={} elapsed={:.2}s throughput={:.0}/s band_size={} origin_component={}",
        primes_processed, elapsed_secs, throughput, stats.band_size, stats.origin_component_size
    );
}

#[cfg(test)]
mod tests {
    use super::{ceil_sqrt_u64, upper_bound_start_norm};

    #[test]
    fn ceil_sqrt_handles_square_and_non_square_inputs() {
        assert_eq!(ceil_sqrt_u64(0), 0);
        assert_eq!(ceil_sqrt_u64(1), 1);
        assert_eq!(ceil_sqrt_u64(9), 3);
        assert_eq!(ceil_sqrt_u64(10), 4);
        assert_eq!(ceil_sqrt_u64(u64::MAX), 4_294_967_296);
    }

    #[test]
    fn upper_bound_start_norm_handles_edge_cases() {
        assert_eq!(upper_bound_start_norm(0, 0), 0);
        assert_eq!(upper_bound_start_norm(0, 17), 289);
        assert_eq!(upper_bound_start_norm(2, 1), 0);
        assert_eq!(upper_bound_start_norm(9, 10), 49);
        assert_eq!(upper_bound_start_norm(10, 10), 36);
        assert_eq!(upper_bound_start_norm(u64::MAX, u64::MAX), u64::MAX);
    }
}
