use std::collections::HashMap;
use std::f64::consts::FRAC_PI_4;
use std::io;
use std::time::{Duration, Instant};

use crate::band::BandProcessor;
use crate::gprf_reader::GprfStreamReader;
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

    // Streaming architecture: route each prime to wedge BandProcessors on arrival.
    // No bulk collection phase — primes flow through routing into processors immediately.
    // Memory stays bounded by the band eviction window (~500K live primes per wedge).
    match &config.prime_source {
        PrimeSource::File(path) => {
            let stream_reader = GprfStreamReader::open_file(path).unwrap_or_else(|e| {
                panic!("Failed to open prime file '{}': {}", path, e);
            });
            eprintln!(
                "angular: streaming GPRF file '{}': header_count={}, norm range [{}, {}]",
                path,
                stream_reader.header_count,
                stream_reader.norm_min,
                stream_reader.norm_max
            );
            stream_primes(stream_reader, config, wedges_used, norm_bound, &start)
        }
        PrimeSource::Stdin => {
            let stdin = io::stdin().lock();
            let stream_reader = GprfStreamReader::from_raw(stdin);
            eprintln!("angular: streaming from stdin (pipe mode)");
            stream_primes(stream_reader, config, wedges_used, norm_bound, &start)
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
            stream_primes(stream, config, wedges_used, norm_bound, &start)
        }
    }
}

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

    // Initialize per-wedge BandProcessors and tracking state
    let initial_uf_cap = 500_000usize; // conservative; band eviction keeps live count bounded
    let mut bands: Vec<BandProcessor> = (0..num_wedges)
        .map(|_| {
            if config.upper_bound {
                BandProcessor::new_upper_bound_with_capacity(
                    config.k_squared,
                    config.boundary_distance,
                    initial_uf_cap,
                )
            } else {
                BandProcessor::new_with_capacity(config.k_squared, initial_uf_cap)
            }
        })
        .collect();

    // Per-wedge overlap tracking for stitching
    let mut left_slots: Vec<Vec<(i32, i32, u64, u32)>> = vec![Vec::new(); num_wedges];
    let mut right_slots: Vec<Vec<(i32, i32, u64, u32)>> = vec![Vec::new(); num_wedges];
    let mut non_native_slots: Vec<Vec<u32>> = vec![Vec::new(); num_wedges];

    let mut primes_processed: u64 = 0;

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

        // Route this prime to wedge(s) using angle-based routing
        let (ca, cb) = canonical(prime.a, prime.b);
        let theta = (cb as f64).atan2(ca as f64);
        let prime_norm = prime.norm.max(1) as f64;
        let overlap_radians = (config.k_squared as f64).sqrt() / prime_norm.sqrt();

        let primary = ((theta / wedge_width).floor() as i64).clamp(0, num_wedges as i64 - 1) as usize;

        if num_wedges == 1 {
            // Fast path: single wedge, no overlap logic needed
            let pr = bands[0].process_prime_ext(&prime);
            let _ = pr; // no stitching needed for single wedge
        } else {
            // Multi-wedge: compute overlap range and feed each relevant wedge
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

                // Pin shared primes so they survive eviction (needed for stitching)
                if shared_left || shared_right {
                    bands[w].pin_slot(pr.slot, pr.gen);
                }
                if shared_left {
                    left_slots[w].push((prime.a, prime.b, prime.norm, pr.slot));
                }
                if shared_right {
                    right_slots[w].push((prime.a, prime.b, prime.norm, pr.slot));
                }

                // Track non-native primes for origin size correction
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

    // Build wedge results for stitching
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

    // Stitch boundaries
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

    // Compute native origin size
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

    // Build a count of non-native primes per component root
    let mut non_native_per_root: HashMap<u32, u64> = HashMap::new();
    for &slot in non_native_slot_data {
        let root = band.find_root(slot);
        *non_native_per_root.entry(root).or_insert(0) += 1;
    }

    // Build overlap component info with correct native sizes
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
    // Each wedge allocates its own BandProcessor with grid hash map,
    // node vectors, and union-find — overhead is ~O(primes/wedges) per
    // wedge but with significant constant factors. At >32 wedges the
    // per-wedge allocation overhead dominates: 124 wedges on A100 used
    // 82.6 GB RSS for 13.7M primes (~6KB/prime vs expected ~16B).
    // Cap at 32 to keep memory sane while still saturating cores.
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
