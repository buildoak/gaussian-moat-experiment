use clap::Parser;

use fat_stripe::config::FatStripeConfig;
use fat_stripe::orchestrator;

#[derive(Parser)]
#[command(
    name = "fat-stripe",
    about = "Fat-Stripe UB campaign — annular strip Gaussian moat prover"
)]
struct Args {
    /// Squared step bound k² (adjacency threshold in the Gaussian prime graph)
    #[arg(long)]
    k_squared: u64,

    /// Inner radius of the annular strip
    #[arg(long)]
    r_min: f64,

    /// Outer radius of the annular strip
    #[arg(long)]
    r_max: f64,

    /// Virtual tile width W (lattice units)
    #[arg(long, default_value = "2000")]
    tile_width: u32,

    /// Number of virtual tiles per column-chunk
    #[arg(long, default_value = "1000")]
    chunk_size: u32,

    /// Sieve limit L for row sieve
    #[arg(long, default_value = "110000")]
    sieve_limit: u32,

    /// Number of Rayon threads (0 = all cores)
    #[arg(long, default_value = "0")]
    threads: usize,

    /// Output path for JSON results
    #[arg(long)]
    json_trace: Option<String>,

    /// Print verbose progress to stderr
    #[arg(long)]
    verbose: bool,
}

fn main() {
    let args = Args::parse();

    // Derive b_max from the outer radius (first octant: b <= a, so b <= r_max)
    let b_max = args.r_max.ceil() as i64;

    let mut config = FatStripeConfig::new(
        args.k_squared,
        args.tile_width,
        args.chunk_size,
        args.sieve_limit,
        b_max,
    );
    config.threads = args.threads;

    // Configure Rayon thread pool
    if args.threads > 0 {
        rayon::ThreadPoolBuilder::new()
            .num_threads(args.threads)
            .build_global()
            .ok();
    }

    eprintln!("fat-stripe config:");
    eprintln!("  k_sq        = {}", config.k_sq);
    eprintln!("  collar      = {}", config.collar);
    eprintln!("  r_min       = {}", args.r_min);
    eprintln!("  r_max       = {}", args.r_max);
    eprintln!("  tile_width  = {}", config.tile_width);
    eprintln!("  chunk_size  = {}", config.chunk_size);
    eprintln!("  sieve_limit = {}", config.sieve_limit);
    eprintln!("  b_max       = {}", config.b_max);
    eprintln!("  num_chunks  = {}", config.num_chunks());
    eprintln!("  total_tiles = {}", config.total_tiles());
    eprintln!("  threads     = {}", config.threads);
    if let Some(ref path) = args.json_trace {
        eprintln!("  json_trace  = {path}");
    }

    let result = orchestrator::run_campaign(&config, args.r_min, args.r_max);

    println!(
        "campaign: blocked={} stripes={} chunks={} tiles={} elapsed={}ms",
        result.blocked,
        result.num_stripes,
        result.num_chunks_total,
        result.total_tiles,
        result.elapsed_ms,
    );

    if let Some(ref path) = args.json_trace {
        eprintln!("(JSON trace output to {path} not yet implemented — Wave 2)");
    }
}
