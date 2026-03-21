use std::time::Instant;

use rayon::prelude::*;

use serde::Serialize;

use moat_kernel::compose::{compose_grid, compose_grid_with_seams, compose_vertical, compose_vertical_with_seams, SeamEvent};
use moat_kernel::primes::PrimeSieve;
use moat_kernel::profile::{get_rss_kb, PhaseTimer, ProbeProfile};
use moat_kernel::tile::{build_tile_with_sieve, TileDetail, FACE_INNER_BIT, FACE_OUTER_BIT, TileOperator};

pub struct ProbeConfig {
    pub k_sq: u64,
    pub r_min: f64,
    pub r_max: f64,
    pub strip_width: f64,
    pub num_strips: usize,
    pub tile_depth: f64,
    pub trace: bool,
    pub export_detail: bool,
}

pub struct ProbeResult {
    pub candidates: Vec<(usize, f64)>, // (shell_idx, radius) for each disconnection
    pub shells_processed: usize,
    pub total_primes: usize,
    pub total_tiles: usize,
}

impl ProbeResult {
    pub fn candidate_found(&self) -> bool {
        !self.candidates.is_empty()
    }

    pub fn first_candidate_radius(&self) -> Option<f64> {
        self.candidates.first().map(|&(_, r)| r)
    }
}

/// Detailed export data for a single shell (opt-in via --export-detail).
#[derive(Debug, Clone, Serialize)]
pub struct ShellDetail {
    pub tile_details: Vec<TileDetail>,
    pub seam_events: Vec<SeamEvent>,
    pub vertical_seam_events: Vec<SeamEvent>,
}

pub struct ShellProfile {
    pub shell_idx: usize,
    pub r_center: f64,
    pub primes_in_shell: usize,
    pub tiles_built: usize,
    pub compose_time_ms: u64,
    pub transport_alive: bool,
    pub io_crossing_count: usize,      // accumulated tile: I→O spanning components
    pub band_io_crossings: usize,      // this shell's band ALONE: local I→O connectivity
    pub band_components: usize,        // total components in band before composition
    pub accumulated_components: usize,  // total components in accumulated tile after composition
    pub tile_io_counts: Vec<usize>,    // per-tile I→O crossing counts BEFORE horizontal composition
    pub detail: Option<ShellDetail>,   // populated only when export_detail is true
}

fn centered_strip_bounds(strip_width: f64, num_strips: usize) -> Vec<(i64, i64)> {
    let origin = -(num_strips as f64) * strip_width / 2.0;
    (0..num_strips)
        .map(|idx| {
            let start = origin + idx as f64 * strip_width;
            let end = start + strip_width;
            (start.floor() as i64, end.ceil() as i64 - 1)
        })
        .collect()
}

fn shell_bounds(r_min: f64, r_max: f64, tile_depth: f64) -> Vec<(i64, i64, f64)> {
    let mut shells = Vec::new();
    let mut start = r_min;
    while start < r_max {
        let end = (start + tile_depth).min(r_max);
        let a_lo = start.floor() as i64;
        let a_hi = end.ceil() as i64;
        shells.push((a_lo, a_hi, (start + end) * 0.5));
        start += tile_depth;
        if tile_depth <= 0.0 {
            break;
        }
    }
    shells
}

/// Count components that span both Inner and Outer faces (I→O transport threads).
fn io_crossing_count(tile: &TileOperator) -> usize {
    tile.component_faces
        .iter()
        .filter(|&&faces| faces & FACE_INNER_BIT != 0 && faces & FACE_OUTER_BIT != 0)
        .count()
}

/// Transport is alive if:
/// - origin mode (r_min ≈ 0): origin component reaches outer face
/// - UB mode (r_min > 0): any component spans I→O
fn transport_alive(tile: &TileOperator, ub_mode: bool) -> bool {
    if ub_mode {
        io_crossing_count(tile) > 0
    } else {
        tile.origin_component
            .map(|c| tile.component_faces[c] & FACE_OUTER_BIT != 0)
            .unwrap_or(false)
    }
}

pub fn run_probe(config: &ProbeConfig) -> (ProbeResult, Vec<ShellProfile>, ProbeProfile) {
    let started = Instant::now();
    let strip_bounds = centered_strip_bounds(config.strip_width, config.num_strips);
    let shells = shell_bounds(config.r_min, config.r_max, config.tile_depth);
    let ub_mode = config.r_min > 0.0;

    let mut phase_timer = PhaseTimer::new();
    let mut shell_profiles = Vec::with_capacity(shells.len());
    let mut candidates: Vec<(usize, f64)> = Vec::new();
    let mut accumulated: Option<TileOperator> = None;
    let mut total_primes = 0;
    let mut total_tiles = 0;
    let mut peak_rss_kb = get_rss_kb();

    let export_detail = config.export_detail;

    for (shell_idx, &(a_lo, a_hi, r_center)) in shells.iter().enumerate() {
        phase_timer.phase("tile-build");
        let collar = (config.k_sq as f64).sqrt().ceil() as i64;
        let max_b = strip_bounds
            .iter()
            .map(|&(lo, hi)| lo.unsigned_abs().max(hi.unsigned_abs()))
            .max()
            .unwrap_or(0) as i64
            + collar;
        let sieve_limit = ((a_hi + collar) as u64).max(max_b as u64);
        let sieve_limit_norm = {
            let ma = (a_hi.unsigned_abs() + collar as u64) as u128;
            let mb = max_b as u128;
            let max_norm = ma
                .saturating_mul(ma)
                .saturating_add(mb.saturating_mul(mb))
                .min(u64::MAX as u128) as u64;
            max_norm.min(10_000_000)
        };
        let sieve_limit = sieve_limit.max(sieve_limit_norm);
        let sieve = PrimeSieve::new(sieve_limit);
        let tiles: Vec<_> = strip_bounds
            .par_iter()
            .map(|&(b_lo, b_hi)| build_tile_with_sieve(a_lo, a_hi, b_lo, b_hi, config.k_sq, &sieve, export_detail))
            .collect();
        let primes_in_shell = tiles.iter().map(|tile| tile.num_primes).sum::<usize>();
        total_primes += primes_in_shell;
        total_tiles += tiles.len();
        peak_rss_kb = peak_rss_kb.max(get_rss_kb());

        // Per-tile connectivity BEFORE horizontal composition
        let tile_io_counts: Vec<usize> = tiles.iter().map(|t| io_crossing_count(t)).collect();

        // Extract tile details before composition consumes them
        let tile_details: Vec<TileDetail> = if export_detail {
            tiles.iter().filter_map(|t| t.detail.clone()).collect()
        } else {
            Vec::new()
        };

        phase_timer.phase("compose");
        let compose_started = Instant::now();

        let mut seam_events: Vec<SeamEvent> = Vec::new();
        let band = if export_detail {
            compose_grid_with_seams(tiles, config.k_sq, &mut seam_events)
        } else {
            compose_grid(vec![tiles], config.k_sq)
        };

        // Per-shell band metrics BEFORE composition with accumulated
        let band_crossings = io_crossing_count(&band);
        let band_components = band.component_faces.len();

        let mut vertical_seam_events: Vec<SeamEvent> = Vec::new();
        let merged = if let Some(previous) = accumulated.take() {
            if export_detail {
                compose_vertical_with_seams(
                    &previous, &band, config.k_sq,
                    &mut vertical_seam_events,
                    shell_idx.saturating_sub(1), shell_idx,
                )
            } else {
                compose_vertical(&previous, &band, config.k_sq)
            }
        } else {
            band
        };
        let compose_time_ms = compose_started.elapsed().as_millis() as u64;
        let alive = transport_alive(&merged, ub_mode);
        let crossings = io_crossing_count(&merged);
        let acc_components = merged.component_faces.len();

        if !alive {
            candidates.push((shell_idx, r_center));
        }

        let detail = if export_detail {
            Some(ShellDetail {
                tile_details,
                seam_events,
                vertical_seam_events,
            })
        } else {
            None
        };

        shell_profiles.push(ShellProfile {
            shell_idx,
            r_center,
            primes_in_shell,
            tiles_built: strip_bounds.len(),
            compose_time_ms,
            transport_alive: alive,
            io_crossing_count: crossings,
            band_io_crossings: band_crossings,
            band_components,
            accumulated_components: acc_components,
            tile_io_counts,
            detail,
        });

        if config.trace {
            eprintln!(
                "trace shell {shell_idx}: a=[{a_lo}, {a_hi}] primes={primes_in_shell} band_io={band_crossings} acc_io={crossings} alive={alive}"
            );
        }

        peak_rss_kb = peak_rss_kb.max(get_rss_kb());
        // Continue regardless — collect full profile, don't stop on first candidate
        accumulated = Some(merged);
    }

    let result = ProbeResult {
        candidates,
        shells_processed: shells.len(),
        total_primes,
        total_tiles,
    };
    let profile = ProbeProfile {
        total_elapsed: started.elapsed(),
        phase_times: phase_timer.finish(),
        peak_rss_kb,
        total_primes_generated: total_primes,
        total_tiles_built: total_tiles,
    };
    (result, shell_profiles, profile)
}

#[cfg(test)]
mod tests {
    use super::{run_probe, ProbeConfig};

    #[test]
    fn small_k_squared_finds_a_moat() {
        let config = ProbeConfig {
            k_sq: 2,
            r_min: 0.0,
            r_max: 20.0,
            strip_width: 4.0,
            num_strips: 32,
            tile_depth: 1.0,
            trace: false,
            export_detail: false,
        };

        let (result, _, _) = run_probe(&config);
        assert!(result.candidate_found());
    }
}
