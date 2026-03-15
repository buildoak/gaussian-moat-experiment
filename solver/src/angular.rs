use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use rayon::prelude::*;

use crate::band::BandProcessor;
use crate::prime_router::{route_primes, WedgeBuffer};
use crate::progress::ProgressSignal;
use crate::sieve::{GaussianPrime, PrimeStream};
use crate::stitcher::{stitch, OverlapPrime, WedgeResult};

const PROGRESS_INTERVAL: u64 = 1_000_000;
const NO_SLOT: u32 = u32::MAX;

pub struct AngularConfig {
    pub k_squared: u64,
    pub num_wedges: u32,
    pub upper_bound: bool,
    pub boundary_distance: u64,
    pub norm_bound: u64,
    pub prime_file: Option<String>,
}

pub struct AngularResult {
    pub farthest_a: i32,
    pub farthest_b: i32,
    pub farthest_distance: f64,
    pub component_size: u64,
    pub primes_processed: u64,
    pub wedges_used: u32,
    pub elapsed: Duration,
}

pub fn run_angular(config: &AngularConfig) -> AngularResult {
    let start = Instant::now();
    let wedges_used = effective_wedge_count(config.num_wedges);
    let norm_bound = effective_norm_bound(config.norm_bound, config.k_squared);
    let start_norm = if config.upper_bound {
        upper_bound_start_norm(config.k_squared, config.boundary_distance)
    } else {
        2
    };

    // Phase 1: Collect all primes
    let primes: Vec<GaussianPrime> = if let Some(ref path) = config.prime_file {
        use crate::gprf_reader::PrimeFileReader;
        let reader = PrimeFileReader::open(path).unwrap_or_else(|e| {
            panic!("Failed to open prime file '{}': {}", path, e);
        });
        eprintln!(
            "angular: loaded GPRF file '{}': {} primes, norm range [{}, {}]",
            path,
            reader.len(),
            reader.norm_min,
            reader.norm_max
        );
        if norm_bound > 0 && norm_bound < u64::MAX {
            reader.iter_norm_range(start_norm, norm_bound).collect()
        } else {
            if start_norm > 2 {
                reader.iter_norm_range(start_norm, u64::MAX).collect()
            } else {
                reader.iter().collect()
            }
        }
    } else {
        let mut stream = if config.upper_bound {
            PrimeStream::new_with_start(start_norm, norm_bound)
        } else {
            PrimeStream::new(norm_bound)
        };
        let mut v: Vec<GaussianPrime> = Vec::new();
        for prime in stream.by_ref() {
            v.push(prime);
            if (v.len() as u64) % PROGRESS_INTERVAL == 0 {
                eprintln!(
                    "angular progress: phase=collect primes={} wedges={} elapsed={:.2}s",
                    v.len(),
                    wedges_used,
                    start.elapsed().as_secs_f64()
                );
            }
        }
        v
    };
    let primes_processed = primes.len() as u64;

    // Phase 2: Route primes into per-wedge buffers
    let overlap_start_norm = start_norm.max(1);
    let buffers = route_primes(&primes, wedges_used, config.k_squared, overlap_start_norm);
    let progress = ProgressSignal::new(wedges_used);
    let uf_capacity = estimate_uf_capacity(primes_processed, wedges_used);

    let parallel_processed = AtomicU64::new(0);
    let next_report = AtomicU64::new(PROGRESS_INTERVAL);

    // Phase 3: Process wedges in parallel
    let mut wedge_results: Vec<WedgeResult> = buffers
        .into_par_iter()
        .enumerate()
        .map(|(wedge_id, buffer)| {
            process_wedge(
                wedge_id as u32,
                wedges_used,
                buffer,
                config,
                uf_capacity,
                &progress,
                &parallel_processed,
                &next_report,
                &start,
            )
        })
        .collect();
    wedge_results.sort_unstable_by_key(|wr| wr.wedge_id);

    // Phase 4: Stitch boundaries
    let stitched = stitch(&wedge_results, wedges_used);

    AngularResult {
        farthest_a: stitched.farthest_a,
        farthest_b: stitched.farthest_b,
        farthest_distance: stitched.farthest_distance,
        component_size: stitched.total_component_size,
        primes_processed,
        wedges_used,
        elapsed: start.elapsed(),
    }
}

#[allow(clippy::too_many_arguments)]
fn process_wedge(
    wedge_id: u32,
    num_wedges: u32,
    buffer: WedgeBuffer,
    config: &AngularConfig,
    uf_capacity: usize,
    progress: &ProgressSignal,
    parallel_processed: &AtomicU64,
    next_report: &AtomicU64,
    start: &Instant,
) -> WedgeResult {
    let mut band = if config.upper_bound {
        BandProcessor::new_upper_bound_with_capacity(
            config.k_squared,
            config.boundary_distance,
            uf_capacity,
        )
    } else {
        BandProcessor::new_with_capacity(config.k_squared, uf_capacity)
    };

    // Track shared primes for stitching (primes present in adjacent wedges).
    // "left shared" = primes also in wedge_id-1 (for left boundary stitching)
    // "right shared" = primes also in wedge_id+1 (for right boundary stitching)
    let mut left_slots: Vec<(i32, i32, u64, u32)> = Vec::new();
    let mut right_slots: Vec<(i32, i32, u64, u32)> = Vec::new();

    // Track non-native prime slots for origin size correction
    let mut non_native_slots: Vec<u32> = Vec::new();

    for (idx, prime) in buffer.primes.iter().enumerate() {
        if progress.should_stop() {
            break;
        }

        let processed = parallel_processed.fetch_add(1, Ordering::Relaxed) + 1;
        report_parallel_progress(processed, next_report, num_wedges, start);

        let pr = band.process_prime_ext(prime);

        let is_shared_left = buffer.shared_with_left[idx];
        let is_shared_right = buffer.shared_with_right[idx];

        // Pin shared primes so they survive eviction (needed for stitching)
        if is_shared_left || is_shared_right {
            band.pin_slot(pr.slot, pr.gen);
        }
        if is_shared_left {
            left_slots.push((prime.a, prime.b, prime.norm, pr.slot));
        }
        if is_shared_right {
            right_slots.push((prime.a, prime.b, prime.norm, pr.slot));
        }

        // Track non-native primes for origin size correction
        if !buffer.is_native[idx] {
            non_native_slots.push(pr.slot);
        }
    }

    // Determine origin presence
    let origin_root = {
        let root = band.origin_find_root();
        if root == NO_SLOT { None } else { Some(root) }
    };
    let has_origin = if config.upper_bound {
        origin_root.is_some()
    } else {
        wedge_id + 1 == num_wedges && origin_root.is_some()
    };

    // Compute native origin size:
    // band.origin_component_size() = total primes in origin component (native + non-native).
    // We subtract non-native primes that are in the origin component.
    // Non-native primes are pinned, so their slots are still valid.
    let native_origin_size = if has_origin {
        let total_origin = band.origin_component_size();
        if let Some(o_root) = origin_root {
            let origin_canonical = band.find_root(o_root);
            let mut non_native_in_origin = 0u64;
            for &slot in &non_native_slots {
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

    // Build a count of non-native primes per component root.
    // Non-native primes are pinned, so their slots are alive and queryable.
    let mut non_native_per_root: HashMap<u32, u64> = HashMap::new();
    for &slot in &non_native_slots {
        let root = band.find_root(slot);
        *non_native_per_root.entry(root).or_insert(0) += 1;
    }

    // Build overlap component info with correct native sizes
    let mut overlap_component_info: HashMap<u32, (u64, i32, i32, u64)> = HashMap::new();

    let overlap_left = build_overlap_primes(
        wedge_id, &mut band, &mut overlap_component_info,
        &left_slots, &non_native_per_root, config.upper_bound, has_origin, origin_root,
    );
    let overlap_right = build_overlap_primes(
        wedge_id, &mut band, &mut overlap_component_info,
        &right_slots, &non_native_per_root, config.upper_bound, has_origin, origin_root,
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
            // Compute native size: total UF size minus non-native primes in this component.
            let total_size = band.component_size(root);
            let non_native = non_native_per_root.get(&root).copied().unwrap_or(0);
            let mut native_size = total_size.saturating_sub(non_native);
            // In UB mode, the origin component includes the synthetic origin (0,0)
            // which is not a real prime. Subtract it.
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
                    "angular progress: phase=wedges wedge_primes={} wedges={} elapsed={:.2}s",
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

fn effective_wedge_count(requested: u32) -> u32 {
    if requested > 0 {
        return requested;
    }
    let cores = num_cpus::get();
    // 4 wedges per core: good parallelism without memory explosion.
    // No artificial floor — on 6-core Jetson, 24 wedges is correct.
    // On 108-core A100, gives 432 wedges. Capped at 4096.
    let base = 4 * cores;
    let ceiling = 4096usize;
    base.max(4).min(ceiling) as u32
}

fn estimate_uf_capacity(primes_processed: u64, num_wedges: u32) -> usize {
    let wedges = num_wedges.max(1) as usize;
    let per_wedge = (primes_processed as usize).saturating_div(wedges).max(1);
    (per_wedge * 2).clamp(8_192, 16_000_000)
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
