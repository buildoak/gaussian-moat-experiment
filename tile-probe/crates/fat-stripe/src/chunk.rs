//! Per-chunk processor: sieve rows → bitmap → partition → sparse UF → compose

use crate::config::FatStripeConfig;
use moat_kernel::tile::{FacePort, TileOperator};

/// Boundary ports saved for inter-chunk composition
pub struct ChunkBoundary {
    pub left_ports: Vec<FacePort>,
    pub right_ports: Vec<FacePort>,
    pub inner_ports: Vec<FacePort>,
    pub outer_ports: Vec<FacePort>,
    pub num_components: usize,
}

/// Process one column-chunk: C virtual tiles at given radial position
pub fn process_chunk(
    config: &FatStripeConfig,
    a_lo: i64,
    b_chunk_lo: i64,
    b_chunk_hi: i64,
) -> (Vec<TileOperator>, ChunkBoundary) {
    todo!("Wave 2: implement chunk processor")
}
