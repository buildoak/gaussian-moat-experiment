use moat_kernel::ScanlineKernel;
use moat_kernel::TileKernel;

fn main() {
    let kernel = ScanlineKernel::new(false);
    let reference_tiles: [(i64, i64, i64, u64); 10] = [
        (100, 1000, 200, 36),
        (1000, 1000, 200, 36),
        (10000, 1000, 200, 36),
        (100000, 1000, 200, 36),
        (1000000, 1000, 200, 36),
        (10000000, 1000, 200, 36),
        (80000000, 1000, 200, 36),
        (50000000, 1000, 200, 36),
        (20000000, 1000, 200, 36),
        (5000000, 1000, 200, 36),
    ];

    for (a_lo, b_lo, side, k_sq) in reference_tiles {
        let result = kernel.run_tile(a_lo, a_lo + side, b_lo, b_lo + side, k_sq);
        println!(
            "a_lo={a_lo} b_lo={b_lo} side={side} k_sq={k_sq} num_primes={}",
            result.num_primes
        );
    }
}
