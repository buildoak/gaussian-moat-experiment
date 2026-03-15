use gaussian_moat_solver::sieve::PrimeStream;
use std::env;
use std::process;

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() != 2 {
        eprintln!("Usage: {} <norm_bound>", args[0]);
        process::exit(1);
    }

    let norm_bound: u64 = args[1].parse().unwrap_or_else(|e| {
        eprintln!("Error: invalid norm_bound '{}': {}", args[1], e);
        process::exit(1);
    });

    for gp in PrimeStream::new(norm_bound) {
        println!("{} {} {}", gp.a, gp.b, gp.norm);
    }
}
