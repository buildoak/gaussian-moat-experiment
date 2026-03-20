mod compose;
mod primes;
mod probe;
mod profile;
mod tile;

use clap::Parser;

use crate::probe::{run_probe, ProbeConfig};

#[derive(Parser)]
#[command(name = "tile-probe", about = "Tile-based Gaussian moat prober")]
struct Args {
    #[arg(long)]
    k_squared: Option<u64>,

    #[arg(long, default_value = "0")]
    r_min: f64,

    #[arg(long)]
    r_max: Option<f64>,

    #[arg(long, default_value = "128")]
    strip_width: f64,

    #[arg(long, default_value = "16")]
    num_strips: usize,

    #[arg(long, default_value = "1000")]
    tile_depth: f64,

    #[arg(long)]
    profile: bool,

    #[arg(long)]
    trace: bool,

    #[arg(long)]
    validate: bool,

    /// List known large-scale validation targets (Tsuchimura data)
    #[arg(long)]
    known_targets: bool,
}

fn validation_cases() -> Vec<(u64, f64)> {
    vec![(2, 20.0), (4, 40.0), (6, 50.0)]
}

/// Large-scale validation targets with Tsuchimura-known moat distances.
/// These are NOT run by --validate (too slow); use --k-squared directly.
fn known_targets() -> Vec<(u64, f64, &'static str)> {
    vec![
        // Tsuchimura (2004): R_moat = 1,015,639
        (26, 1_100_000.0, "Tsuchimura exact: 1,015,639"),
    ]
}

fn validation_config(k_sq: u64, r_max: f64) -> ProbeConfig {
    ProbeConfig {
        k_sq,
        r_min: 0.0,
        r_max,
        strip_width: 4.0,
        num_strips: 8,
        tile_depth: 1.0,
        trace: false,
    }
}

fn large_target_config(k_sq: u64, r_max: f64) -> ProbeConfig {
    // Scale strip/depth parameters for large runs.
    // At R~1M, step sqrt(26)~5.1 can drift laterally ~5 per shell.
    // With 1000-deep shells and ~1000 shells, lateral drift can reach ~5000.
    // Use 64 strips of 256 width = 16K lateral coverage (generous).
    let collar = (k_sq as f64).sqrt().ceil();
    let strip_width = (collar * 40.0).max(128.0);
    let num_strips = 64;
    let tile_depth = (r_max / 100.0).max(1000.0).min(10000.0);

    ProbeConfig {
        k_sq,
        r_min: 0.0,
        r_max,
        strip_width,
        num_strips,
        tile_depth,
        trace: false,
    }
}

fn print_probe_result(config: &ProbeConfig, args: &Args) -> bool {
    let (result, shells, profile) = run_probe(config);

    println!(
        "tile-probe k²={} r=[{}, {}] strips={}×{} depth={}",
        config.k_sq, config.r_min, config.r_max, config.num_strips, config.strip_width, config.tile_depth
    );

    if args.trace || shells.len() <= 32 {
        for shell in &shells {
            println!(
                "  Shell {}: R≈{:.1} {} primes, {} tiles, compose {} ms, origin→outer: {}",
                shell.shell_idx,
                shell.r_center,
                shell.primes_in_shell,
                shell.tiles_built,
                shell.compose_time_ms,
                if shell.origin_reaches_outer { "YES" } else { "NO" }
            );
        }
    }

    if result.candidate_found {
        println!(
            "  Result: CANDIDATE at R ≈ {:.1} after {} shells ({} primes, {} tiles)",
            result.candidate_radius.unwrap_or(config.r_max),
            result.shells_processed,
            result.total_primes,
            result.total_tiles
        );
    } else {
        println!(
            "  Result: no candidate up to R = {:.1} after {} shells ({} primes, {} tiles)",
            config.r_max, result.shells_processed, result.total_primes, result.total_tiles
        );
    }

    if args.profile {
        println!(
            "  Profile: {:.2}s, {:.1} MB RSS, {} primes, {} tiles",
            profile.total_elapsed.as_secs_f64(),
            profile.peak_rss_kb as f64 / 1024.0,
            profile.total_primes_generated,
            profile.total_tiles_built
        );
        if !profile.phase_times.is_empty() {
            let phase_count = profile.phase_times.len();
            let total_phase_ms: f64 = profile
                .phase_times
                .iter()
                .map(|(_, duration)| duration.as_secs_f64() * 1000.0)
                .sum();
            println!("  Phases: {phase_count} segments, {:.2} ms tracked", total_phase_ms);
        }
    }

    result.candidate_found
}

fn run_validation() -> bool {
    let mut all_passed = true;
    for (k_sq, r_max) in validation_cases() {
        let config = validation_config(k_sq, r_max);
        let (result, _, _) = run_probe(&config);
        let passed = result.candidate_found;
        println!(
            "validate k²={} r_max={:.0}: {}",
            k_sq,
            r_max,
            if passed { "PASS" } else { "FAIL" }
        );
        all_passed &= passed;
    }
    all_passed
}

fn main() {
    let args = Args::parse();

    if args.known_targets {
        println!("Known large-scale validation targets:");
        for (k_sq, r_max, note) in known_targets() {
            let config = large_target_config(k_sq, r_max);
            println!(
                "  k²={} r_max={:.0} strips={}×{:.0} depth={:.0} — {}",
                k_sq, r_max, config.num_strips, config.strip_width, config.tile_depth, note
            );
        }
        println!("\nRun with: tile-probe --k-squared 26 --r-max 1100000 --num-strips 64 --strip-width 240 --tile-depth 10000 --profile");
        return;
    }

    if args.validate {
        if !run_validation() {
            std::process::exit(1);
        }
        return;
    }

    let config = ProbeConfig {
        k_sq: args
            .k_squared
            .expect("--k-squared is required unless --validate is set"),
        r_min: args.r_min,
        r_max: args.r_max.expect("--r-max is required unless --validate is set"),
        strip_width: args.strip_width,
        num_strips: args.num_strips,
        tile_depth: args.tile_depth,
        trace: args.trace,
    };

    print_probe_result(&config, &args);
}
