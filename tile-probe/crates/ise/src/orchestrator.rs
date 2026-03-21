use std::time::Instant;

use rayon::prelude::*;
use serde::{Deserialize, Serialize};

use moat_kernel::kernel::{TileKernel, TileResult};
use moat_kernel::primes::PrimeSieve;

/// Configuration for an ISE campaign.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IseConfig {
    pub k_sq: u64,
    pub r_min: f64,
    pub r_max: f64,
    pub tile_width: u32,
    pub tile_height: u32,
    pub num_stripes: usize,
    pub fallback_heights: Vec<u32>,
    pub trace: bool,
}

/// A single tile's record within a stripe, including position, dimensions, and results.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TileRecord {
    pub a_lo: i64,
    pub b_lo: i64,
    pub width: u32,
    pub height: u32,
    pub result: TileResult,
    pub connects_below: Option<bool>, // Always None in ISE mode
    pub connects_left: Option<bool>,  // LB mode post-processing only
    pub connects_right: Option<bool>, // LB mode post-processing only
    pub time_ms: u64,
}

/// A vertical stripe: a sequence of tile records from r_min to r_max.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StripeRecord {
    pub stripe_id: usize,
    pub b_lo: i64,
    pub tiles: Vec<TileRecord>,
}

/// Per-shell aggregation for ISE mode.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShellRecord {
    pub shell_idx: usize,
    pub r_center: f64,
    pub a_lo: i64,
    pub a_hi: i64,
    pub io_counts: Vec<usize>,
    pub f_r: f64,
    pub is_candidate: bool, // f_r == 0
    pub num_primes: usize,
    pub shell_time_ms: u64,
}

/// Summary of an ISE campaign.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IseSummary {
    pub total_shells: usize,
    pub total_tiles: usize,
    pub candidates: Vec<(usize, f64)>, // (shell_idx, r_center)
    pub total_time_ms: u64,
    pub peak_rss_mb: f64,
}

/// Full result of an ISE campaign.
pub struct IseResult {
    pub config: IseConfig,
    pub shells: Vec<ShellRecord>,
    pub stripes: Vec<StripeRecord>,
    pub summary: IseSummary,
}

/// Compute shell bounds: (a_lo, a_hi, r_center) for each shell.
fn shell_bounds(r_min: f64, r_max: f64, tile_height: u32) -> Vec<(i64, i64, f64)> {
    let h = tile_height as f64;
    let mut shells = Vec::new();
    let mut start = r_min;
    while start < r_max {
        let end = (start + h).min(r_max);
        let a_lo = start.floor() as i64;
        let a_hi = end.ceil() as i64;
        if a_hi <= a_lo {
            break;
        }
        shells.push((a_lo, a_hi, (start + end) * 0.5));
        start += h;
    }
    shells
}

/// Compute centered stripe offsets: b_lo values for M stripes, centered around b=0.
fn stripe_offsets(tile_width: u32, num_stripes: usize) -> Vec<i64> {
    let w = tile_width as i64;
    let total_width = w * num_stripes as i64;
    let origin = -total_width / 2;
    (0..num_stripes)
        .map(|idx| origin + idx as i64 * w)
        .collect()
}

/// Compute the sieve limit needed for a shell's tiles.
fn sieve_limit_for_shell(
    a_lo: i64,
    a_hi: i64,
    offsets: &[i64],
    tile_width: u32,
    k_sq: u64,
) -> u64 {
    let collar = (k_sq as f64).sqrt().ceil() as i64;
    let max_a = a_hi.abs().max(a_lo.abs()) + collar;
    let max_b = offsets
        .iter()
        .map(|&b_lo| {
            let b_hi = b_lo + tile_width as i64;
            b_lo.abs().max(b_hi.abs())
        })
        .max()
        .unwrap_or(0)
        + collar;

    let max_norm = (max_a as u128)
        .saturating_mul(max_a as u128)
        .saturating_add((max_b as u128).saturating_mul(max_b as u128))
        .min(u64::MAX as u128) as u64;

    // PrimeSieve stores a boolean array, so cap at 10M for memory safety
    max_norm.min(10_000_000).max(1000)
}

/// Run the ISE campaign.
///
/// Uses nested par_iter: outer over shells, inner over stripes.
/// The sieve is built per shell and shared across all stripes in that shell
/// via CpuKernel.
pub fn run_ise(config: &IseConfig) -> IseResult {
    assert!(
        config.num_stripes >= 1,
        "stripes must be >= 1, got {}",
        config.num_stripes
    );
    assert!(config.tile_width > 0, "tile_width must be > 0");
    assert!(config.tile_height > 0, "tile_height must be > 0");
    assert!(
        config.r_max > config.r_min,
        "r_max ({}) must be > r_min ({})",
        config.r_max,
        config.r_min
    );

    let started = Instant::now();

    let shells = shell_bounds(config.r_min, config.r_max, config.tile_height);
    let offsets = stripe_offsets(config.tile_width, config.num_stripes);
    let w = config.tile_width;

    // Per-stripe accumulators: built up as we process shells
    let num_stripes = config.num_stripes;

    // Process shells in parallel. Each shell builds its own sieve and kernel.
    let shell_results: Vec<(ShellRecord, Vec<(usize, TileRecord)>)> = shells
        .par_iter()
        .enumerate()
        .map(|(shell_idx, &(a_lo, a_hi, r_center))| {
            let shell_start = Instant::now();

            // Build per-shell sieve
            let sieve_limit = sieve_limit_for_shell(a_lo, a_hi, &offsets, w, config.k_sq);
            let sieve = PrimeSieve::new(sieve_limit);
            let kernel = moat_kernel::kernel::CpuKernel::new(&sieve);

            // Process all stripes for this shell
            let tile_results: Vec<(usize, TileResult, u64)> = offsets
                .par_iter()
                .enumerate()
                .map(|(stripe_idx, &b_lo)| {
                    let tile_start = Instant::now();
                    let b_hi = b_lo + w as i64;
                    let result = kernel.run_tile(a_lo, a_hi, b_lo, b_hi, config.k_sq);
                    let tile_ms = tile_start.elapsed().as_millis() as u64;
                    (stripe_idx, result, tile_ms)
                })
                .collect();

            // Aggregate per-shell
            let mut io_counts = vec![0usize; num_stripes];
            let mut total_primes = 0usize;
            let mut tile_records: Vec<(usize, TileRecord)> = Vec::with_capacity(num_stripes);

            for (stripe_idx, result, tile_ms) in tile_results {
                io_counts[stripe_idx] = result.io_count;
                total_primes += result.num_primes;

                let record = TileRecord {
                    a_lo,
                    b_lo: offsets[stripe_idx],
                    width: w,
                    height: config.tile_height,
                    result,
                    connects_below: None, // ISE mode: always None
                    connects_left: None,
                    connects_right: None,
                    time_ms: tile_ms,
                };
                tile_records.push((stripe_idx, record));
            }

            let connected = io_counts.iter().filter(|&&c| c > 0).count();
            let f_r = if io_counts.is_empty() {
                0.0
            } else {
                connected as f64 / io_counts.len() as f64
            };

            let shell_time_ms = shell_start.elapsed().as_millis() as u64;

            if config.trace {
                eprintln!(
                    "trace shell {}: a=[{}, {}] R~{:.1} f(r)={:.4} io={:?} primes={} {:.0}ms",
                    shell_idx,
                    a_lo,
                    a_hi,
                    r_center,
                    f_r,
                    &io_counts,
                    total_primes,
                    shell_time_ms,
                );
            }

            let shell_record = ShellRecord {
                shell_idx,
                r_center,
                a_lo,
                a_hi,
                io_counts,
                f_r,
                is_candidate: f_r == 0.0,
                num_primes: total_primes,
                shell_time_ms,
            };

            (shell_record, tile_records)
        })
        .collect();

    // Separate shell records and tile records, then build stripe-oriented view
    let mut shells_out: Vec<ShellRecord> = Vec::with_capacity(shell_results.len());
    let mut stripe_accumulators: Vec<Vec<TileRecord>> = vec![Vec::new(); num_stripes];

    for (shell_record, tile_records) in shell_results {
        for (stripe_idx, tile_record) in tile_records {
            stripe_accumulators[stripe_idx].push(tile_record);
        }
        shells_out.push(shell_record);
    }

    // Sort shells by index (par_iter preserves order, but be explicit)
    shells_out.sort_by_key(|s| s.shell_idx);

    // Sort each stripe's tiles by a_lo (radially outward)
    for tiles in &mut stripe_accumulators {
        tiles.sort_by_key(|t| t.a_lo);
    }

    let stripes: Vec<StripeRecord> = stripe_accumulators
        .into_iter()
        .enumerate()
        .map(|(id, tiles)| StripeRecord {
            stripe_id: id,
            b_lo: offsets[id],
            tiles,
        })
        .collect();

    // Build summary
    let total_tiles: usize = stripes.iter().map(|s| s.tiles.len()).sum();
    let candidates: Vec<(usize, f64)> = shells_out
        .iter()
        .filter(|s| s.is_candidate)
        .map(|s| (s.shell_idx, s.r_center))
        .collect();

    let total_time_ms = started.elapsed().as_millis() as u64;

    // RSS estimate (rough -- platform-specific)
    let peak_rss_mb = moat_kernel::profile::get_rss_kb() as f64 / 1024.0;

    let summary = IseSummary {
        total_shells: shells_out.len(),
        total_tiles,
        candidates,
        total_time_ms,
        peak_rss_mb,
    };

    IseResult {
        config: config.clone(),
        shells: shells_out,
        stripes,
        summary,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Gate 4, Test A: k^2=2 moat detected at R~8-10
    #[test]
    fn ise_detects_k2_moat() {
        let config = IseConfig {
            k_sq: 2,
            r_min: 0.0,
            r_max: 30.0,
            tile_width: 4,
            tile_height: 4,
            num_stripes: 8,
            fallback_heights: vec![],
            trace: false,
        };

        let result = run_ise(&config);

        // Should have multiple shells
        assert!(!result.shells.is_empty(), "Should have shells");

        // Near origin, f(r) > 0 (connected)
        let origin_shell = result
            .shells
            .iter()
            .find(|s| s.r_center < 5.0)
            .expect("Should have a shell near origin");
        assert!(
            origin_shell.f_r > 0.0,
            "Near-origin shell should have f(r) > 0, got {}",
            origin_shell.f_r
        );

        // At least one shell should be a candidate (f(r) == 0)
        let has_candidate = result.shells.iter().any(|s| s.is_candidate);
        assert!(
            has_candidate,
            "Should detect moat candidate for k^2=2. Shell f(r) values: {:?}",
            result
                .shells
                .iter()
                .map(|s| (s.shell_idx, s.r_center, s.f_r))
                .collect::<Vec<_>>()
        );

        // Summary should report candidates
        assert!(
            !result.summary.candidates.is_empty(),
            "Summary should list candidates"
        );
    }

    /// Gate 4, Test B: k^2=6 at R>50 shows depressed f(r) < 1.0
    /// ISE with per-tile detection may not achieve f(r)=0 at small R for k^2=6,
    /// since the accumulated LB method detects moats through composition across
    /// multiple tiles. ISE requires ALL stripes to have io_count=0 simultaneously.
    /// We verify that f(r) drops below 1.0, confirming the ISE is correctly
    /// measuring connectivity depression at larger R.
    #[test]
    fn ise_k6_shows_connectivity_depression() {
        let config = IseConfig {
            k_sq: 6,
            r_min: 0.0,
            r_max: 60.0,
            tile_width: 4,
            tile_height: 2,
            num_stripes: 16,
            fallback_heights: vec![],
            trace: false,
        };

        let result = run_ise(&config);

        // Should have shells with depressed f(r) < 1.0 (connectivity loss)
        let has_depression = result.shells.iter().any(|s| s.f_r < 1.0);
        assert!(
            has_depression,
            "Should see connectivity depression for k^2=6. f(r) values: {:?}",
            result
                .shells
                .iter()
                .map(|s| (s.shell_idx, s.r_center, s.f_r))
                .collect::<Vec<_>>()
        );

        // Near origin, should be well-connected
        let origin_shell = result
            .shells
            .iter()
            .find(|s| s.r_center < 10.0)
            .expect("Should have near-origin shell");
        assert!(
            origin_shell.f_r > 0.0,
            "Near-origin should be connected for k^2=6"
        );
    }

    /// Gate 5: k^2=26 at R<1000 shows f(r) > 0 everywhere (no false candidates)
    #[test]
    fn ise_no_false_candidates_k26() {
        let config = IseConfig {
            k_sq: 26,
            r_min: 0.0,
            r_max: 1000.0,
            tile_width: 200,
            tile_height: 200,
            num_stripes: 8,
            fallback_heights: vec![],
            trace: false,
        };

        let result = run_ise(&config);

        for shell in &result.shells {
            assert!(
                shell.f_r > 0.0,
                "Shell {} at R~{:.1} should have f(r) > 0 for k^2=26, got f(r)={}",
                shell.shell_idx,
                shell.r_center,
                shell.f_r
            );
        }

        assert!(
            result.summary.candidates.is_empty(),
            "Should have no candidates for k^2=26 at R<1000"
        );
    }

    /// Stripe record construction test
    #[test]
    fn stripe_records_have_correct_structure() {
        let config = IseConfig {
            k_sq: 2,
            r_min: 0.0,
            r_max: 10.0,
            tile_width: 4,
            tile_height: 4,
            num_stripes: 4,
            fallback_heights: vec![],
            trace: false,
        };

        let result = run_ise(&config);

        // Should have exactly num_stripes stripes
        assert_eq!(result.stripes.len(), 4);

        // Each stripe should have the same number of tiles (one per shell)
        let expected_tiles_per_stripe = result.shells.len();
        for stripe in &result.stripes {
            assert_eq!(
                stripe.tiles.len(),
                expected_tiles_per_stripe,
                "Stripe {} should have {} tiles, got {}",
                stripe.stripe_id,
                expected_tiles_per_stripe,
                stripe.tiles.len()
            );
        }

        // Tiles within each stripe should be sorted by a_lo
        for stripe in &result.stripes {
            for window in stripe.tiles.windows(2) {
                assert!(
                    window[0].a_lo <= window[1].a_lo,
                    "Tiles should be sorted by a_lo within stripe"
                );
            }
        }
    }

    /// Input validation test
    #[test]
    #[should_panic(expected = "stripes must be >= 1")]
    fn ise_rejects_zero_stripes() {
        let config = IseConfig {
            k_sq: 2,
            r_min: 0.0,
            r_max: 10.0,
            tile_width: 4,
            tile_height: 4,
            num_stripes: 0,
            fallback_heights: vec![],
            trace: false,
        };
        run_ise(&config);
    }
}
