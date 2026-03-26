use std::path::PathBuf;

use serde::{Deserialize, Serialize};

fn default_work_dir() -> PathBuf {
    std::env::temp_dir()
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CudaFatStripeConfig {
    pub k_sq: u64,
    pub tile_side: u32,
    pub collar: u32,
    pub r_min: u64,
    pub r_max: u64,
    pub b_min: i64,
    pub b_max: i64,
    pub cuda_binary: PathBuf,
    pub cuda_device: u32,
    pub cuda_batch_size: u32,
    #[serde(default = "default_work_dir")]
    pub work_dir: PathBuf,
}

impl CudaFatStripeConfig {
    pub fn new(
        k_sq: u64,
        tile_side: u32,
        collar: u32,
        r_min: u64,
        r_max: u64,
        b_min: i64,
        b_max: i64,
        cuda_binary: PathBuf,
        cuda_device: u32,
        cuda_batch_size: u32,
    ) -> Self {
        Self {
            k_sq,
            tile_side,
            collar,
            r_min,
            r_max,
            b_min,
            b_max,
            cuda_binary,
            cuda_device,
            cuda_batch_size,
            work_dir: default_work_dir(),
        }
    }
}
