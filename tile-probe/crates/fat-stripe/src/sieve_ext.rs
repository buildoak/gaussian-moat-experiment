//! Extended sieve helpers for fat-stripe processing.
//! Wraps moat-kernel's sieve_row with chunk-wide row processing.

/// Process all rows in a chunk, producing a list of Gaussian prime coordinates.
pub fn sieve_chunk_rows(
    a_lo: i64,
    a_hi: i64,
    b_lo: i64,
    b_hi: i64,
    sieve_limit: u32,
) -> Vec<(i64, i64)> {
    todo!("Wave 2: implement chunk-wide sieve")
}
