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
}

fn validation_cases() -> Vec<(u64, f64)> {
    vec![(2, 20.0), (4, 40.0), (6, 50.0)]
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

    if result.moat_found {
        println!(
            "  Result: MOAT FOUND at R ≈ {:.1} after {} shells ({} primes, {} tiles)",
            result.moat_radius.unwrap_or(config.r_max),
            result.shells_processed,
            result.total_primes,
            result.total_tiles
        );
    } else {
        println!(
            "  Result: no moat found up to R = {:.1} after {} shells ({} primes, {} tiles)",
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

    result.moat_found
}

fn run_validation() -> bool {
    let mut all_passed = true;
    for (k_sq, r_max) in validation_cases() {
        let config = validation_config(k_sq, r_max);
        let (result, _, _) = run_probe(&config);
        let passed = result.moat_found;
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

    if !print_probe_result(&config, &args) && args.trace {
        std::process::exit(0);
    }
}
