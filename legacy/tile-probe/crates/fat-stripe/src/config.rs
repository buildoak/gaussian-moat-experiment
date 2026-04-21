use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FatStripeConfig {
    /// Squared step bound k²
    pub k_sq: u64,
    /// Virtual tile width W (lattice units)
    pub tile_width: u32,
    /// Stripe height H = W (lattice units, one tile-height per stripe)
    pub tile_height: u32,
    /// Number of virtual tiles per column-chunk
    pub chunk_size: u32,
    /// Sieve limit L for row sieve
    pub sieve_limit: u32,
    /// Collar = ceil(sqrt(k_sq)), computed at construction
    pub collar: u32,
    /// Minimum b coordinate (angular offset, default 0)
    pub b_min: i64,
    /// Maximum b coordinate (first octant: b <= a)
    pub b_max: i64,
    /// Number of Rayon threads (0 = all)
    pub threads: usize,
    /// When true, compute and print degree statistics after the campaign
    pub degree_stats: bool,
    /// Optional override for inner spanning threshold radius
    pub spanning_r_min: Option<f64>,
    /// Optional override for outer spanning threshold radius
    pub spanning_r_max: Option<f64>,
}

impl FatStripeConfig {
    pub fn new(k_sq: u64, tile_width: u32, chunk_size: u32, sieve_limit: u32, b_max: i64) -> Self {
        let collar = (k_sq as f64).sqrt().ceil() as u32;
        Self {
            k_sq,
            tile_width,
            tile_height: tile_width,
            chunk_size,
            sieve_limit,
            collar,
            b_min: 0,
            b_max,
            threads: 0,
            degree_stats: false,
            spanning_r_min: None,
            spanning_r_max: None,
        }
    }

    /// Expanded tile side including collar on both sides
    pub fn expanded_side(&self) -> u32 {
        self.tile_width + 2 * self.collar
    }

    /// Number of column-chunks needed to cover [0, b_max]
    pub fn num_chunks(&self) -> u32 {
        let total_tiles = (self.b_max as u32 + self.tile_width - 1) / self.tile_width;
        (total_tiles + self.chunk_size - 1) / self.chunk_size
    }

    /// Number of virtual tiles total
    pub fn total_tiles(&self) -> u64 {
        let total = (self.b_max as u64 + self.tile_width as u64 - 1) / self.tile_width as u64;
        total
    }
}
