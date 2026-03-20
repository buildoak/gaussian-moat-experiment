use std::time::Instant;

use rayon::prelude::*;

use crate::compose::{compose_grid, compose_vertical};
use crate::profile::{get_rss_kb, PhaseTimer, ProbeProfile};
use crate::tile::{build_tile, FACE_OUTER_BIT, TileOperator};

pub struct ProbeConfig {
    pub k_sq: u64,
    pub r_min: f64,
    pub r_max: f64,
    pub strip_width: f64,
    pub num_strips: usize,
    pub tile_depth: f64,
    pub trace: bool,
}

pub struct ProbeResult {
    pub moat_found: bool,
    pub moat_radius: Option<f64>,
    pub shells_processed: usize,
    pub total_primes: usize,
    pub total_tiles: usize,
}

pub struct ShellProfile {
    pub shell_idx: usize,
    pub r_center: f64,
    pub primes_in_shell: usize,
    pub tiles_built: usize,
    pub compose_time_ms: u64,
    pub origin_reaches_outer: bool,
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

fn origin_reaches_outer(tile: &TileOperator) -> bool {
    tile.origin_component
        .map(|component| tile.component_faces[component] & FACE_OUTER_BIT != 0)
        .unwrap_or(false)
}

pub fn run_probe(config: &ProbeConfig) -> (ProbeResult, Vec<ShellProfile>, ProbeProfile) {
    let started = Instant::now();
    let strip_bounds = centered_strip_bounds(config.strip_width, config.num_strips);
    let shells = shell_bounds(config.r_min, config.r_max, config.tile_depth);

    let mut phase_timer = PhaseTimer::new();
    let mut shell_profiles = Vec::with_capacity(shells.len());
    let mut accumulated: Option<TileOperator> = None;
    let mut total_primes = 0;
    let mut total_tiles = 0;
    let mut peak_rss_kb = get_rss_kb();

    for (shell_idx, &(a_lo, a_hi, r_center)) in shells.iter().enumerate() {
        phase_timer.phase("tile-build");
        let tiles: Vec<_> = strip_bounds
            .par_iter()
            .map(|&(b_lo, b_hi)| build_tile(a_lo, a_hi, b_lo, b_hi, config.k_sq))
            .collect();
        let primes_in_shell = tiles.iter().map(|tile| tile.num_primes).sum::<usize>();
        total_primes += primes_in_shell;
        total_tiles += tiles.len();
        peak_rss_kb = peak_rss_kb.max(get_rss_kb());

        phase_timer.phase("compose");
        let compose_started = Instant::now();
        let band = compose_grid(vec![tiles], config.k_sq);
        let merged = if let Some(previous) = accumulated.take() {
            compose_vertical(&previous, &band, config.k_sq)
        } else {
            band
        };
        let compose_time_ms = compose_started.elapsed().as_millis() as u64;
        let reaches_outer = origin_reaches_outer(&merged);

        shell_profiles.push(ShellProfile {
            shell_idx,
            r_center,
            primes_in_shell,
            tiles_built: strip_bounds.len(),
            compose_time_ms,
            origin_reaches_outer: reaches_outer,
        });

        if config.trace {
            eprintln!(
                "trace shell {shell_idx}: a=[{a_lo}, {a_hi}] primes={primes_in_shell} origin_to_outer={reaches_outer}"
            );
        }

        peak_rss_kb = peak_rss_kb.max(get_rss_kb());
        if !reaches_outer {
            let result = ProbeResult {
                moat_found: true,
                moat_radius: Some(r_center),
                shells_processed: shell_idx + 1,
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
            return (result, shell_profiles, profile);
        }

        accumulated = Some(merged);
    }

    let result = ProbeResult {
        moat_found: false,
        moat_radius: None,
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
        };

        let (result, _, _) = run_probe(&config);
        assert!(result.moat_found);
    }
}
