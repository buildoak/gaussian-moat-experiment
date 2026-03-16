use std::collections::HashMap;
use std::f64::consts::FRAC_PI_4;
use std::io;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use rayon::prelude::*;

use crate::band::BandProcessor;
use crate::gprf_reader::GprfStreamReader;
use crate::prime_router::{route_primes, WedgeBuffer};
use crate::sieve::{GaussianPrime, PrimeStream};
use crate::stitcher::{stitch, OverlapPrime, WedgeResult};

const PROGRESS_INTERVAL: u64 = 1_000_000;
const NO_SLOT: u32 = u32::MAX;

/// Where primes come from: file, stdin pipe, or internal Rust sieve.
pub enum PrimeSource {
    File(String),
    Stdin,
    InternalSieve,
}

pub struct AngularConfig {
    pub k_squared: u64,
    pub num_wedges: u32,
    pub upper_bound: bool,
    pub boundary_distance: u64,
    pub norm_bound: u64,
    pub prime_source: PrimeSource,
    /// Resume LB continuation: origin component already reaches at least this norm.
    /// Primes with norm <= resume_farthest_norm auto-connect to origin.
    pub resume_farthest_norm: u64,
}

pub struct AngularResult {
    pub farthest_a: i32,
    pub farthest_b: i32,
    pub farthest_distance: f64,
    pub component_size: u64,
    pub primes_processed: u64,
    pub wedges_used: u32,
    pub elapsed: Duration,
    /// True when the solver proved a moat (origin component stopped growing).
    pub moat_found: bool,
}

pub fn run_angular(config: &AngularConfig) -> AngularResult {
    let start = Instant::now();
    let wedges_used = effective_wedge_count(config.num_wedges);
    let norm_bound = effective_norm_bound(config.norm_bound, config.k_squared);

    // Parallel path: collect all primes into memory, route to per-wedge buffers,
    // process wedges in parallel via rayon. Used for File and InternalSieve sources
    // when num_wedges > 1. Stdin stays sequential (streaming + parallel needs channels).
    match &config.prime_source {
        PrimeSource::Stdin => {
            // Stdin: streaming-only (can't rewind or collect efficiently)
            let stdin = io::stdin().lock();
            let stream_reader = GprfStreamReader::from_raw(stdin);
            eprintln!("angular: streaming from stdin (pipe mode, sequential)");
            stream_primes(stream_reader, config, wedges_used, norm_bound, &start)
        }
        PrimeSource::File(path) => {
            let stream_reader = GprfStreamReader::open_file(path).unwrap_or_else(|e| {
                panic!("Failed to open prime file '{}': {}", path, e);
            });
            eprintln!(
                "angular: GPRF file '{}': header_count={}, norm range [{}, {}]",
                path,
                stream_reader.header_count,
                stream_reader.norm_min,
                stream_reader.norm_max
            );
            if wedges_used <= 1 {
                eprintln!("angular: single wedge — streaming path");
                stream_primes(stream_reader, config, wedges_used, norm_bound, &start)
            } else {
                // Collect all primes, then parallel wedge processing
                let primes = collect_primes(stream_reader, norm_bound, &start);
                parallel_wedges(primes, config, wedges_used, norm_bound, &start)
            }
        }
        PrimeSource::InternalSieve => {
            let start_norm = if config.upper_bound {
                upper_bound_start_norm(config.k_squared, config.boundary_distance)
            } else {
                2
            };
            let stream = if config.upper_bound {
                PrimeStream::new_with_start(start_norm, norm_bound)
            } else {
                PrimeStream::new(norm_bound)
            };
            if wedges_used <= 1 {
                eprintln!("angular: single wedge — streaming path");
                stream_primes(stream, config, wedges_used, norm_bound, &start)
            } else {
                let primes = collect_primes(stream, norm_bound, &start);
                parallel_wedges(primes, config, wedges_used, norm_bound, &start)
            }
        }
    }
}

/// Collect all primes from an iterator into a Vec, applying norm_bound filter.
fn collect_primes(
    iter: impl Iterator<Item = GaussianPrime>,
    norm_bound: u64,
    start: &Instant,
) -> Vec<GaussianPrime> {
    let apply_bound = norm_bound > 0 && norm_bound < u64::MAX;
    let mut primes = Vec::new();
    for prime in iter {
        if apply_bound && prime.norm > norm_bound {
            continue;
        }
        primes.push(prime);
        if primes.len() % (PROGRESS_INTERVAL as usize) == 0 {
            eprintln!(
                "angular progress: phase=collect primes={} elapsed={:.2}s",
                primes.len(),
                start.elapsed().as_secs_f64()
            );
        }
    }
    eprintln!(
        "angular: collected {} primes in {:.2}s",
        primes.len(),
        start.elapsed().as_secs_f64()
    );
    primes
}

/// Parallel wedge processing: route primes to per-wedge buffers, then process
/// each wedge independently in parallel using rayon. This is the production path
/// for file/sieve sources with multiple wedges.
fn parallel_wedges(
    primes: Vec<GaussianPrime>,
    config: &AngularConfig,
    wedges_used: u32,
    _norm_bound: u64,
    start: &Instant,
) -> AngularResult {
    let primes_processed = primes.len() as u64;

    // Phase 1: Route primes into per-wedge buffers (single-threaded, fast)
    let buffers = route_primes(&primes, wedges_used, config.k_squared);
    drop(primes); // free the original Vec — buffers own their copies now

    let route_elapsed = start.elapsed().as_secs_f64();
    eprintln!(
        "angular: routed to {} wedges in {:.2}s",
        wedges_used, route_elapsed
    );

    // Estimate per-wedge UF capacity from prime distribution
    let uf_capacity = estimate_uf_capacity(primes_processed, wedges_used);

    // Parallel progress tracking
    let parallel_processed = AtomicU64::new(0);
    let next_report = AtomicU64::new(PROGRESS_INTERVAL);

    // Phase 2: Process wedges in parallel
    let mut wedge_results: Vec<WedgeResult> = buffers
        .into_par_iter()
        .enumerate()
        .map(|(wedge_id, buffer)| {
            process_wedge_parallel(
                wedge_id as u32,
                wedges_used,
                buffer,
                config,
                uf_capacity,
                &parallel_processed,
                &next_report,
                start,
            )
        })
        .collect();
    wedge_results.sort_unstable_by_key(|wr| wr.wedge_id);

    let process_elapsed = start.elapsed().as_secs_f64();
    eprintln!(
        "angular: parallel processing complete — {} primes across {} wedges in {:.2}s ({:.0} primes/s)",
        primes_processed,
        wedges_used,
        process_elapsed,
        primes_processed as f64 / process_elapsed.max(0.001)
    );

    // Phase 3: Stitch boundaries
    let stitched = stitch(&wedge_results, wedges_used);

    AngularResult {
        farthest_a: stitched.farthest_a,
        farthest_b: stitched.farthest_b,
        farthest_distance: stitched.farthest_distance,
        component_size: stitched.total_component_size,
        primes_processed,
        wedges_used,
        elapsed: start.elapsed(),
        moat_found: false, // parallel path does not do early moat detection per-wedge
    }
}

/// Process a single wedge's primes within its own BandProcessor.
/// Called from rayon par_iter — each invocation runs on its own thread.
#[allow(clippy::too_many_arguments)]
fn process_wedge_parallel(
    wedge_id: u32,
    num_wedges: u32,
    buffer: WedgeBuffer,
    config: &AngularConfig,
    uf_capacity: usize,
    parallel_processed: &AtomicU64,
    next_report: &AtomicU64,
    start: &Instant,
) -> WedgeResult {
    let resume_norm = config.resume_farthest_norm;
    let mut band = if config.upper_bound {
        BandProcessor::new_upper_bound_with_capacity(
            config.k_squared,
            config.boundary_distance,
            uf_capacity,
        )
    } else {
        BandProcessor::new_with_capacity(config.k_squared, uf_capacity)
    };
    if resume_norm > 0 && !config.upper_bound {
        band.set_resume_farthest_norm(resume_norm);
    }

    // Track shared primes for stitching
    let mut left_slots: Vec<(i32, i32, u64, u32)> = Vec::new();
    let mut right_slots: Vec<(i32, i32, u64, u32)> = Vec::new();
    let mut non_native_slots: Vec<u32> = Vec::new();

    for (idx, prime) in buffer.primes.iter().enumerate() {
        let processed = parallel_processed.fetch_add(1, Ordering::Relaxed) + 1;
        report_parallel_progress(processed, next_report, num_wedges, start);

        let pr = band.process_prime_ext(prime);

        let is_shared_left = buffer.shared_with_left[idx];
        let is_shared_right = buffer.shared_with_right[idx];

        if is_shared_left || is_shared_right {
            band.pin_slot(pr.slot, pr.gen);
        }
        if is_shared_left {
            left_slots.push((prime.a, prime.b, prime.norm, pr.slot));
        }
        if is_shared_right {
            right_slots.push((prime.a, prime.b, prime.norm, pr.slot));
        }

        if !buffer.is_native[idx] {
            non_native_slots.push(pr.slot);
        }
    }

    build_wedge_result(
        wedge_id,
        num_wedges,
        &mut band,
        &left_slots,
        &right_slots,
        &non_native_slots,
        config,
    )
}

fn report_parallel_progress(
    processed: u64,
    next_report: &AtomicU64,
    num_wedges: u32,
    start: &Instant,
) {
    let mut target = next_report.load(Ordering::Relaxed);
    while processed >= target {
        match next_report.compare_exchange(
            target,
            target.saturating_add(PROGRESS_INTERVAL),
            Ordering::Relaxed,
            Ordering::Relaxed,
        ) {
            Ok(_) => {
                eprintln!(
                    "angular progress: phase=parallel wedge_primes={} wedges={} elapsed={:.2}s",
                    processed,
                    num_wedges,
                    start.elapsed().as_secs_f64()
                );
                break;
            }
            Err(observed) => target = observed,
        }
    }
}

fn estimate_uf_capacity(primes_processed: u64, num_wedges: u32) -> usize {
    let wedges = num_wedges.max(1) as usize;
    let per_wedge = (primes_processed as usize).saturating_div(wedges).max(1);
    // 2x headroom for overlap primes that appear in multiple wedges
    (per_wedge * 2).clamp(8_192, 16_000_000)
}

// ---------------------------------------------------------------------------
// Sequential streaming path (stdin + single-wedge fallback)
// ---------------------------------------------------------------------------

/// Core streaming loop: read primes one at a time, route to wedge(s), feed BandProcessors.
/// No bulk Vec<GaussianPrime> collection. Memory is bounded by the sliding band window.
fn stream_primes(
    iter: impl Iterator<Item = GaussianPrime>,
    config: &AngularConfig,
    wedges_used: u32,
    norm_bound: u64,
    start: &Instant,
) -> AngularResult {
    let apply_bound = norm_bound > 0 && norm_bound < u64::MAX;
    let num_wedges = wedges_used as usize;
    let wedge_width = FRAC_PI_4 / num_wedges as f64;
    let k_sqrt = (config.k_squared as f64).sqrt();
    let single_wedge = num_wedges == 1;

    let initial_uf_cap = 500_000usize;
    let resume_norm = config.resume_farthest_norm;
    let mut bands: Vec<BandProcessor> = (0..num_wedges)
        .map(|_| {
            let mut bp = if config.upper_bound {
                BandProcessor::new_upper_bound_with_capacity(
                    config.k_squared,
                    config.boundary_distance,
                    initial_uf_cap,
                )
            } else {
                BandProcessor::new_with_capacity(config.k_squared, initial_uf_cap)
            };
            if resume_norm > 0 && !config.upper_bound {
                bp.set_resume_farthest_norm(resume_norm);
            }
            bp
        })
        .collect();

    let mut left_slots: Vec<Vec<(i32, i32, u64, u32)>> = vec![Vec::new(); num_wedges];
    let mut right_slots: Vec<Vec<(i32, i32, u64, u32)>> = vec![Vec::new(); num_wedges];
    let mut non_native_slots: Vec<Vec<u32>> = vec![Vec::new(); num_wedges];

    let mut primes_processed: u64 = 0;
    let mut moat_found = false;

    for prime in iter {
        if apply_bound && prime.norm > norm_bound {
            continue;
        }

        primes_processed += 1;
        if primes_processed % PROGRESS_INTERVAL == 0 {
            eprintln!(
                "angular progress: phase=stream primes={} wedges={} elapsed={:.2}s",
                primes_processed,
                wedges_used,
                start.elapsed().as_secs_f64()
            );
        }

        if single_wedge {
            let pr = bands[0].process_prime_ext(&prime);
            if pr.moat.is_some() {
                moat_found = true;
                break;
            }
        } else {
            let (ca, cb) = canonical(prime.a, prime.b);
            let theta = (cb as f64).atan2(ca as f64);
            let prime_norm = prime.norm.max(1) as f64;
            let overlap_radians = k_sqrt / prime_norm.sqrt();

            let primary = ((theta / wedge_width).floor() as i64).clamp(0, num_wedges as i64 - 1) as usize;
            let lo_theta = (theta - overlap_radians).max(0.0);
            let hi_theta = (theta + overlap_radians).min(FRAC_PI_4);
            let lo_wedge = ((lo_theta / wedge_width).floor() as i64).clamp(0, num_wedges as i64 - 1) as usize;
            let hi_wedge = if hi_theta >= FRAC_PI_4 {
                num_wedges - 1
            } else {
                ((hi_theta / wedge_width).floor() as i64).clamp(0, num_wedges as i64 - 1) as usize
            };

            let w_start = lo_wedge.min(primary);
            let w_end = hi_wedge.max(primary);

            for w in w_start..=w_end {
                let is_native = w == primary;
                let shared_left = w > 0 && w - 1 >= w_start;
                let shared_right = w + 1 < num_wedges && w + 1 <= w_end;

                let pr = bands[w].process_prime_ext(&prime);

                if shared_left || shared_right {
                    bands[w].pin_slot(pr.slot, pr.gen);
                }
                if shared_left {
                    left_slots[w].push((prime.a, prime.b, prime.norm, pr.slot));
                }
                if shared_right {
                    right_slots[w].push((prime.a, prime.b, prime.norm, pr.slot));
                }

                if !is_native {
                    non_native_slots[w].push(pr.slot);
                }
            }
        }
    }

    eprintln!(
        "angular: stream complete — {} primes in {:.2}s ({:.0} primes/s)",
        primes_processed,
        start.elapsed().as_secs_f64(),
        primes_processed as f64 / start.elapsed().as_secs_f64().max(0.001)
    );

    let mut wedge_results: Vec<WedgeResult> = (0..num_wedges)
        .map(|wedge_id| {
            build_wedge_result(
                wedge_id as u32,
                wedges_used,
                &mut bands[wedge_id],
                &left_slots[wedge_id],
                &right_slots[wedge_id],
                &non_native_slots[wedge_id],
                config,
            )
        })
        .collect();
    wedge_results.sort_unstable_by_key(|wr| wr.wedge_id);

    let stitched = stitch(&wedge_results, wedges_used);

    AngularResult {
        farthest_a: stitched.farthest_a,
        farthest_b: stitched.farthest_b,
        farthest_distance: stitched.farthest_distance,
        component_size: stitched.total_component_size,
        primes_processed,
        wedges_used,
        elapsed: start.elapsed(),
        moat_found,
    }
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

/// Build a WedgeResult from a completed BandProcessor and its overlap tracking data.
fn build_wedge_result(
    wedge_id: u32,
    num_wedges: u32,
    band: &mut BandProcessor,
    left_slot_data: &[(i32, i32, u64, u32)],
    right_slot_data: &[(i32, i32, u64, u32)],
    non_native_slot_data: &[u32],
    config: &AngularConfig,
) -> WedgeResult {
    let origin_root = {
        let root = band.origin_find_root();
        if root == NO_SLOT { None } else { Some(root) }
    };
    let has_origin = if config.upper_bound {
        origin_root.is_some()
    } else {
        wedge_id + 1 == num_wedges && origin_root.is_some()
    };

    let native_origin_size = if has_origin {
        let total_origin = band.origin_component_size();
        if let Some(o_root) = origin_root {
            let origin_canonical = band.find_root(o_root);
            let mut non_native_in_origin = 0u64;
            for &slot in non_native_slot_data {
                if band.find_root(slot) == origin_canonical {
                    non_native_in_origin += 1;
                }
            }
            total_origin.saturating_sub(non_native_in_origin)
        } else {
            total_origin
        }
    } else {
        0
    };

    let mut non_native_per_root: HashMap<u32, u64> = HashMap::new();
    for &slot in non_native_slot_data {
        let root = band.find_root(slot);
        *non_native_per_root.entry(root).or_insert(0) += 1;
    }

    let mut overlap_component_info: HashMap<u32, (u64, i32, i32, u64)> = HashMap::new();

    let overlap_left = build_overlap_primes(
        wedge_id, band, &mut overlap_component_info,
        left_slot_data, &non_native_per_root, config.upper_bound, has_origin, origin_root,
    );
    let overlap_right = build_overlap_primes(
        wedge_id, band, &mut overlap_component_info,
        right_slot_data, &non_native_per_root, config.upper_bound, has_origin, origin_root,
    );

    let (farthest_a, farthest_b, farthest_norm) = if has_origin {
        let a = band.farthest_a();
        let b = band.farthest_b();
        let norm = ((a as i64) * (a as i64) + (b as i64) * (b as i64)) as u64;
        (a, b, norm)
    } else {
        (0, 0, 0)
    };

    WedgeResult {
        wedge_id,
        overlap_left,
        overlap_right,
        has_origin,
        origin_root,
        farthest_a,
        farthest_b,
        farthest_norm,
        native_origin_size,
        overlap_component_info,
    }
}

fn build_overlap_primes(
    wedge_id: u32,
    band: &mut BandProcessor,
    overlap_component_info: &mut HashMap<u32, (u64, i32, i32, u64)>,
    slots: &[(i32, i32, u64, u32)],
    non_native_per_root: &HashMap<u32, u64>,
    upper_bound: bool,
    has_origin: bool,
    origin_root: Option<u32>,
) -> Vec<OverlapPrime> {
    let mut out = Vec::with_capacity(slots.len());
    for &(a, b, norm, slot) in slots {
        let root = band.find_root(slot);

        if !overlap_component_info.contains_key(&root) {
            let total_size = band.component_size(root);
            let non_native = non_native_per_root.get(&root).copied().unwrap_or(0);
            let mut native_size = total_size.saturating_sub(non_native);
            if upper_bound && has_origin && origin_root.map(|r| band.find_root(r)) == Some(root) {
                native_size = native_size.saturating_sub(1);
            }
            let (fa, fb, fn_) = band.component_farthest(root);
            overlap_component_info.insert(root, (native_size, fa, fb, fn_));
        }

        out.push(OverlapPrime {
            a,
            b,
            norm,
            component_root: root,
            wedge_id,
        });
    }
    out
}

#[inline]
fn canonical(a: i32, b: i32) -> (i32, i32) {
    let (x, y) = (a.abs(), b.abs());
    (x.max(y), x.min(y))
}

fn effective_wedge_count(requested: u32) -> u32 {
    if requested > 0 {
        return requested;
    }
    let cores = num_cpus::get();
    // 1 wedge per core: good parallelism without memory explosion.
    let base = cores;
    let ceiling = 32usize;
    base.max(4).min(ceiling) as u32
}

fn effective_norm_bound(norm_bound: u64, k_squared: u64) -> u64 {
    if norm_bound != 0 {
        return norm_bound;
    }

    let max_distance = match k_squared {
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

fn upper_bound_start_norm(k_squared: u64, start_distance: u64) -> u64 {
    let k_radius = ceil_sqrt_u64(k_squared);
    let r = start_distance.saturating_sub(k_radius);
    r.saturating_mul(r)
}

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
