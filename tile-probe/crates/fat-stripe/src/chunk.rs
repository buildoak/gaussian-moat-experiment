//! Per-chunk processor: sieve rows -> partition -> UF per tile -> compose

use crate::config::FatStripeConfig;
use crate::partition::partition_primes;
use crate::sieve_ext::sieve_chunk_rows;
use moat_kernel::compose::compose_horizontal;
use moat_kernel::tile::{build_tile_from_primes, FacePort, TileOperator};
use rayon::prelude::*;

/// Boundary ports saved for inter-chunk composition
pub struct ChunkBoundary {
    pub left_ports: Vec<FacePort>,
    pub right_ports: Vec<FacePort>,
    pub inner_ports: Vec<FacePort>,
    pub outer_ports: Vec<FacePort>,
    pub num_components: usize,
}

/// Process one column-chunk of virtual tiles.
///
/// Steps:
/// 1. Sieve expanded region to find all Gaussian primes (with collar padding)
/// 2. Partition primes into virtual tiles (with collar overlap)
/// 3. Build TileOperator per virtual tile (parallel via Rayon)
/// 4. Compose all tiles horizontally L->R (sequential)
/// 5. Return composed TileOperator
pub fn process_chunk(
    config: &FatStripeConfig,
    a_lo: i64,
    a_hi: i64,
    b_chunk_lo: i64,
    b_chunk_hi: i64,
) -> TileOperator {
    let collar = config.collar;
    let tile_width = config.tile_width;
    let k_sq = config.k_sq;

    let chunk_b_span = (b_chunk_hi - b_chunk_lo) as u32;
    let num_tiles = (chunk_b_span + tile_width - 1) / tile_width;

    if num_tiles == 0 {
        return empty_tile_operator(a_lo, a_hi - 1, b_chunk_lo, b_chunk_hi - 1);
    }

    let sieve_a_lo = a_lo - collar as i64;
    let sieve_a_hi = a_hi + collar as i64;
    let sieve_b_lo = b_chunk_lo - collar as i64;
    let sieve_b_hi = b_chunk_hi + collar as i64;

    let all_primes = sieve_chunk_rows(
        sieve_a_lo,
        sieve_a_hi,
        sieve_b_lo,
        sieve_b_hi,
        config.sieve_limit,
    );

    let tile_primes = partition_primes(&all_primes, b_chunk_lo, tile_width, collar, num_tiles);

    let tile_ops: Vec<TileOperator> = tile_primes
        .into_par_iter()
        .enumerate()
        .map(|(tid, primes)| {
            let tb_lo = b_chunk_lo + tid as i64 * tile_width as i64;
            let tb_hi_raw = b_chunk_lo + (tid as i64 + 1) * tile_width as i64;
            let tb_hi = tb_hi_raw.min(b_chunk_hi);

            build_tile_from_primes(a_lo, a_hi - 1, tb_lo, tb_hi - 1, k_sq, primes, false)
        })
        .collect();

    if tile_ops.is_empty() {
        return empty_tile_operator(a_lo, a_hi - 1, b_chunk_lo, b_chunk_hi - 1);
    }

    let mut iter = tile_ops.into_iter();
    let mut result = iter.next().unwrap();
    for right in iter {
        result = compose_horizontal(&result, &right, k_sq);
    }

    result
}

fn empty_tile_operator(a_min: i64, a_max: i64, b_min: i64, b_max: i64) -> TileOperator {
    TileOperator {
        a_min,
        a_max,
        b_min,
        b_max,
        face_inner: Vec::new(),
        face_outer: Vec::new(),
        face_left: Vec::new(),
        face_right: Vec::new(),
        num_components: 0,
        component_faces: Vec::new(),
        component_sizes: Vec::new(),
        origin_component: None,
        num_primes: 0,
        detail: None,
    }
}
