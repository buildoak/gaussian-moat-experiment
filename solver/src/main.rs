use clap::Parser;
use gaussian_moat_solver::angular::{run_angular, AngularConfig, PrimeSource};
use gaussian_moat_solver::runner::{lower_bound_probe, upper_bound_probe, ProbeResult};

#[derive(Debug, Parser)]
#[command(author, version, about = "Gaussian moat solver — angular and norm-stream modes")]
struct Args {
    #[arg(long = "k-squared")]
    k_squared: u64,
    #[arg(long = "start-distance")]
    start_distance: Option<u64>,
    #[arg(long = "norm-bound", default_value_t = 0)]
    norm_bound: u64,
    #[arg(
        long,
        help = "Number of angular wedges (0 = auto-detect from core count, omit for norm-stream mode)"
    )]
    angular: Option<u32>,
    #[arg(
        long = "prime-file",
        help = "Path to GPRF binary prime file (skips internal sieve)"
    )]
    prime_file: Option<String>,
    #[arg(
        long = "stdin",
        help = "Read raw GPRF records from stdin (pipe mode, mutually exclusive with --prime-file)"
    )]
    use_stdin: bool,
    #[arg(long)]
    verbose: bool,
    #[arg(long, help = "Emit a structured profile block after the run")]
    profile: bool,
}

fn main() {
    let args = Args::parse();

    if args.use_stdin && args.prime_file.is_some() {
        eprintln!("error: --stdin and --prime-file are mutually exclusive");
        std::process::exit(1);
    }

    let prime_source = if args.use_stdin {
        PrimeSource::Stdin
    } else if let Some(ref path) = args.prime_file {
        PrimeSource::File(path.clone())
    } else {
        PrimeSource::InternalSieve
    };

    if let Some(wedges) = args.angular {
        let config = AngularConfig {
            k_squared: args.k_squared,
            num_wedges: wedges,
            upper_bound: args.start_distance.is_some(),
            boundary_distance: args.start_distance.unwrap_or(0),
            norm_bound: args.norm_bound,
            prime_source,
        };
        let result = run_angular(&config);
        println!("=== Angular Probe Result ===");
        println!("farthest point: ({}, {})", result.farthest_a, result.farthest_b);
        println!("farthest distance: {:.6}", result.farthest_distance);
        println!("origin component size: {}", result.component_size);
        println!("primes processed: {}", result.primes_processed);
        println!("wedges used: {}", result.wedges_used);
        println!("elapsed: {:.3}s", result.elapsed.as_secs_f64());
        if args.profile {
            print_profile(result.elapsed.as_secs_f64(), result.primes_processed);
        }
        return;
    }

    // Norm-stream mode (default when --angular is not specified)
    let result = if let Some(start_distance) = args.start_distance {
        upper_bound_probe(args.k_squared, start_distance, args.norm_bound)
    } else {
        lower_bound_probe(args.k_squared, args.norm_bound)
    };
    print_summary(&result);
    if args.profile {
        print_profile(result.elapsed.as_secs_f64(), result.primes_processed);
    }
}

fn get_max_rss_bytes() -> Option<u64> {
    #[cfg(unix)]
    {
        let mut usage: libc::rusage = unsafe { std::mem::zeroed() };
        let ret = unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut usage) };
        if ret == 0 {
            // On macOS, ru_maxrss is in bytes; on Linux, it is in kilobytes.
            let rss = usage.ru_maxrss as u64;
            #[cfg(target_os = "macos")]
            {
                return Some(rss);
            }
            #[cfg(not(target_os = "macos"))]
            {
                return Some(rss * 1024);
            }
        }
        None
    }
    #[cfg(not(unix))]
    {
        None
    }
}

fn print_profile(wall_seconds: f64, items_processed: u64) {
    println!("profile:");
    if let Some(rss) = get_max_rss_bytes() {
        println!("  max_rss_bytes: {}", rss);
    }
    println!("  wall_seconds: {:.6}", wall_seconds);
    if wall_seconds > 0.0 {
        println!(
            "  primes_per_second: {:.0}",
            items_processed as f64 / wall_seconds
        );
    } else {
        println!("  primes_per_second: 0");
    }
}

fn print_summary(result: &ProbeResult) {
    println!("=== Probe Result ===");
    println!(
        "farthest point: ({}, {})",
        result.farthest_a, result.farthest_b
    );
    println!("farthest distance: {:.6}", result.farthest_distance);
    println!("origin component size: {}", result.component_size);
    println!("primes processed: {}", result.primes_processed);
    println!("elapsed: {:.3}s", result.elapsed.as_secs_f64());
}
