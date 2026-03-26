use std::path::PathBuf;

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CudaFatStripeConfig {
    pub k_sq: u64,
    pub tile_side: u32,
    pub collar: u32,
    pub r_min: f64,
    pub r_max: f64,
    pub b_min: i64,
    pub b_max: i64,
    pub cuda_binary: PathBuf,
    pub cuda_device: u32,
    pub cuda_batch_size: u32,
}
