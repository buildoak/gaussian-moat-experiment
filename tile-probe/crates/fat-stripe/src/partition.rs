//! Bitmap partitioning: assign primes to virtual tiles with collar overlap

/// Partition a chunk-wide prime list into per-tile prime lists.
/// Primes within `collar` of a tile boundary are duplicated to both adjacent tiles.
pub fn partition_primes(
    primes: &[(i64, i64)],
    b_chunk_lo: i64,
    tile_width: u32,
    collar: u32,
    num_tiles: u32,
) -> Vec<Vec<(i64, i64)>> {
    todo!("Wave 2: implement prime partitioning")
}
