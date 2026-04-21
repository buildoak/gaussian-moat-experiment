//! Bitmap partitioning: assign primes to virtual tiles with collar overlap

/// Partition a chunk-wide prime list into per-tile prime lists.
/// Primes within collar of a tile boundary are duplicated to both adjacent tiles.
pub fn partition_primes(
    primes: &[(i64, i64)],
    b_chunk_lo: i64,
    tile_width: u32,
    collar: u32,
    num_tiles: u32,
) -> Vec<Vec<(i64, i64)>> {
    let tw = tile_width as i64;
    let c = collar as i64;
    let mut tiles: Vec<Vec<(i64, i64)>> = vec![Vec::new(); num_tiles as usize];

    for &(a, b) in primes {
        let rel_b = b - b_chunk_lo;

        for tid in 0..num_tiles {
            let tile_b_lo = tid as i64 * tw;
            let tile_b_hi = (tid as i64 + 1) * tw;

            if rel_b >= tile_b_lo - c && rel_b < tile_b_hi + c {
                tiles[tid as usize].push((a, b));
            }
        }
    }

    tiles
}
