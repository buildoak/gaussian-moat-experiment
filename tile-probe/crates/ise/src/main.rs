pub mod orchestrator;
pub mod output;

use clap::Parser;

use orchestrator::{run_ise, IseConfig};
use output::{print_summary, write_csv_summary, write_json_trace};

#[derive(Parser)]
#[command(
    name = "ise",
    about = "Independent Strip Ensemble -- Gaussian moat candidate detector"
)]
struct Args {
    /// Squared step bound k^2 (threshold for adjacency in the Gaussian prime graph).
    #[arg(long)]
    k_squared: Option<u64>,

    /// Minimum radius to scan.
    #[arg(long, default_value = "0")]
    r_min: f64,

    /// Maximum radius to scan.
    #[arg(long)]
    r_max: Option<f64>,

    /// Tile width W (lateral extent). Overrides --tile-size and --preset.
    #[arg(long)]
    tile_width: Option<u32>,

    /// Tile height H (radial extent). Overrides --tile-size and --preset.
    #[arg(long)]
    tile_height: Option<u32>,

    /// Square tile shorthand: W = H = S. Overridden by explicit --tile-width/--tile-height.
    #[arg(long)]
    tile_size: Option<u32>,

    /// Tile size preset: "screen" (200x200), "balanced" (500x500), "deep" (2000x2000).
    #[arg(long, default_value = "deep")]
    preset: String,

    /// Number of independent stripes M.
    #[arg(long, default_value = "32")]
    stripes: usize,

    /// Number of Rayon threads (0 = all cores).
    #[arg(long, default_value = "0")]
    threads: usize,

    /// Write JSON trace to this path.
    #[arg(long)]
    json_trace: Option<String>,

    /// Write CSV summary to this path.
    #[arg(long)]
    csv: Option<String>,

    /// Print per-shell trace to stderr.
    #[arg(long)]
    trace: bool,

    /// Print timing profile at end.
    #[arg(long)]
    profile: bool,

    /// Fallback tile heights for ISE retry.
    #[arg(long, value_delimiter = ',')]
    fallback_heights: Option<Vec<u32>>,

    /// Run validation against known moats.
    #[arg(long)]
    validate: bool,
}

/// Resolve tile size from CLI args.
fn resolve_tile_size(args: &Args) -> (u32, u32) {
    let preset = match args.preset.as_str() {
        "screen" => (200, 200),
        "balanced" => (500, 500),
        _ => (2000, 2000), // "deep" is default
    };

    if let (Some(w), Some(h)) = (args.tile_width, args.tile_height) {
        return (w, h);
    }
    if let Some(s) = args.tile_size {
        return (s, s);
    }
    // Partial specification: fill missing dimension from preset.
    let w = args.tile_width.unwrap_or(preset.0);
    let h = args.tile_height.unwrap_or(preset.1);
    (w, h)
}

fn validation_cases() -> Vec<(u64, f64, &'static str)> {
    vec![
        (2, 30.0, "k^2=2: moat at R~8-10"),
    ]
}

fn run_validation() -> bool {
    let mut all_passed = true;

    for (k_sq, r_max, description) in validation_cases() {
        let config = IseConfig {
            k_sq,
            r_min: 0.0,
            r_max,
            tile_width: 4,
            tile_height: 4,
            num_stripes: 8,
            fallback_heights: vec![],
            trace: false,
        };

        let result = run_ise(&config);
        let has_candidate = !result.summary.candidates.is_empty();

        println!(
            "validate {}: {}",
            description,
            if has_candidate { "PASS" } else { "FAIL" }
        );
        all_passed &= has_candidate;
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

    let k_sq = args
        .k_squared
        .expect("--k-squared is required unless --validate is set");
    let r_max = args
        .r_max
        .expect("--r-max is required unless --validate is set");

    let (tile_width, tile_height) = resolve_tile_size(&args);

    // Configure Rayon thread pool
    if args.threads > 0 {
        rayon::ThreadPoolBuilder::new()
            .num_threads(args.threads)
            .build_global()
            .ok();
    }

    let config = IseConfig {
        k_sq,
        r_min: args.r_min,
        r_max,
        tile_width,
        tile_height,
        num_stripes: args.stripes,
        fallback_heights: args.fallback_heights.unwrap_or_default(),
        trace: args.trace,
    };

    let result = run_ise(&config);

    // Output
    print_summary(&result.config, &result.shells, &result.summary);

    if let Some(ref json_path) = args.json_trace {
        write_json_trace(
            json_path,
            &result.config,
            &result.shells,
            &result.stripes,
            &result.summary,
        )
        .expect("Failed to write JSON trace");
        println!("  JSON trace written to {json_path}");
    }

    if let Some(ref csv_path) = args.csv {
        write_csv_summary(csv_path, &result.shells).expect("Failed to write CSV summary");
        println!("  CSV summary written to {csv_path}");
    }

    if args.profile {
        println!(
            "  Profile: {:.2}s total, {:.1} MB peak RSS",
            result.summary.total_time_ms as f64 / 1000.0,
            result.summary.peak_rss_mb
        );
    }
}
