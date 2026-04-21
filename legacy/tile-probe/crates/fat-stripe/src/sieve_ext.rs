//! Extended sieve helpers for fat-stripe processing.
//! Wraps moat-kernel's sieve_row with chunk-wide row processing.

use moat_kernel::primality::{is_gaussian_prime, sieve_row, SieveTable};
use rayon::prelude::*;

/// Process all rows in a chunk, producing a list of Gaussian prime coordinates.
pub fn sieve_chunk_rows(
    a_lo: i64,
    a_hi: i64,
    b_lo: i64,
    b_hi: i64,
    sieve_limit: u32,
) -> Vec<(i64, i64)> {
    if a_hi <= a_lo || b_hi <= b_lo {
        return Vec::new();
    }

    let table = SieveTable::new(sieve_limit);
    let width = (b_hi - b_lo) as usize;

    let rows: Vec<i64> = (a_lo..a_hi).collect();

    rows.par_iter()
        .flat_map(|&a| {
            let mut sieve_buf = vec![false; width];
            sieve_row(a, b_lo, width, &table, &mut sieve_buf);

            let mut primes = Vec::new();
            for (col, &marked_composite) in sieve_buf.iter().enumerate() {
                let b = b_lo + col as i64;
                if marked_composite && a != 0 && b != 0 {
                    continue;
                }
                if is_gaussian_prime(a, b) {
                    primes.push((a, b));
                }
            }
            primes
        })
        .collect()
}
