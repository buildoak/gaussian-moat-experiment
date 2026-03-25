use clap::Parser;

use fat_stripe::config::FatStripeConfig;
use fat_stripe::orchestrator;
use moat_kernel::tile::compute_degree_stats;

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

    /// Minimum b coordinate (angular offset). Defaults to 0.
    #[arg(long, default_value = "0")]
    b_min: i64,

    /// Maximum b coordinate (angular width limit). Defaults to r_max (full octant).
    #[arg(long)]
    b_max: Option<i64>,

    /// Print verbose progress to stderr
    #[arg(long)]
    verbose: bool,

    /// Compute and print degree statistics for the Gaussian prime graph
    #[arg(long)]
    degree_stats: bool,
}

fn main() {
    let args = Args::parse();

    // b_max: user override or full octant (b <= r_max)
    let b_min = args.b_min;
    let b_max = args.b_max.unwrap_or(b_min + 128_000); // default: 128K angular width from b_min

    let mut config = FatStripeConfig::new(
        args.k_squared,
        args.tile_width,
        args.chunk_size,
        args.sieve_limit,
        b_max,
    );
    config.b_min = b_min;
    config.threads = args.threads;
    config.degree_stats = args.degree_stats;

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

    // Degree statistics: compute on the full sieved region if requested
    if args.degree_stats {
        let a_start = args.r_min.floor() as i64;
        let a_end = args.r_max.ceil() as i64;
        let collar = (config.k_sq as f64).sqrt().ceil() as i64;

        // Sieve expanded region (collar-padded for correct edge detection at boundaries)
        let sieve_a_lo = a_start - collar;
        let sieve_a_hi = a_end + collar;
        let sieve_b_lo = b_min - collar;
        let sieve_b_hi = b_max + collar;

        eprintln!(
            "degree-stats: sieving expanded region a=[{}, {}] b=[{}, {}]...",
            sieve_a_lo, sieve_a_hi, sieve_b_lo, sieve_b_hi
        );

        let primes = fat_stripe::sieve_ext::sieve_chunk_rows(
            sieve_a_lo,
            sieve_a_hi,
            sieve_b_lo,
            sieve_b_hi,
            config.sieve_limit,
        );

        let stats = compute_degree_stats(
            a_start,
            a_end - 1,
            b_min,
            b_max - 1,
            config.k_sq,
            &primes,
        );

        // Compact histogram: show only up to last non-zero entry
        let last_nonzero = stats.degree_histogram.iter().rposition(|&x| x > 0).unwrap_or(0);
        let hist_compact: Vec<usize> = stats.degree_histogram[..=last_nonzero].to_vec();

        println!(
            "DEGREE_STATS: primes={} edges={} mean_total={:.2} mean_bwd={:.2} isolated={:.1}% bwd_offsets={} hist={:?}",
            stats.total_primes,
            stats.total_edges,
            stats.mean_degree,
            stats.mean_bwd_degree,
            stats.isolated_fraction * 100.0,
            stats.num_backward_offsets,
            hist_compact,
        );
    }
}
