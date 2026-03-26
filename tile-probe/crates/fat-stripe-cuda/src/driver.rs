use std::fmt;
use std::fs::{self, File};
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

use moat_kernel::tile::TileOperator;

use crate::bridge::tile_operator_from_raw;
use crate::protocol::{read_all_tiles, write_job_manifest, ProtocolError, TileJob};

#[derive(Debug, Clone)]
pub struct CudaDriver {
    pub binary_path: PathBuf,
    pub device: u32,
    pub batch_size: u32,
}

#[derive(Debug)]
pub enum CudaError {
    IoError(io::Error),
    BinaryNotFound(PathBuf),
    CudaFailed {
        status_code: Option<i32>,
        binary_path: PathBuf,
    },
    ProtocolError(ProtocolError),
}

impl fmt::Display for CudaError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IoError(err) => write!(f, "I/O error: {err}"),
            Self::BinaryNotFound(path) => {
                write!(f, "CUDA binary not found: {}", path.display())
            }
            Self::CudaFailed {
                status_code,
                binary_path,
            } => {
                write!(
                    f,
                    "CUDA binary {} failed with status {:?}",
                    binary_path.display(),
                    status_code
                )
            }
            Self::ProtocolError(err) => write!(f, "protocol error: {err}"),
        }
    }
}

impl std::error::Error for CudaError {}

impl From<io::Error> for CudaError {
    fn from(value: io::Error) -> Self {
        Self::IoError(value)
    }
}

impl From<ProtocolError> for CudaError {
    fn from(value: ProtocolError) -> Self {
        Self::ProtocolError(value)
    }
}

impl CudaDriver {
    pub fn process_batch(
        &self,
        work_dir: &Path,
        k_sq: u64,
        tile_side: u32,
        jobs: &[TileJob],
    ) -> Result<Vec<TileOperator>, CudaError> {
        if !self.binary_path.is_file() {
            return Err(CudaError::BinaryNotFound(self.binary_path.clone()));
        }

        fs::create_dir_all(work_dir)?;

        let jobs_path = work_dir.join("tile_jobs.bin");
        let output_path = work_dir.join("face_ports.bin");

        {
            let mut jobs_file = File::create(&jobs_path)?;
            write_job_manifest(&mut jobs_file, k_sq, tile_side, jobs)?;
        }

        let status = Command::new(&self.binary_path)
            .arg("--jobs")
            .arg(&jobs_path)
            .arg("--output")
            .arg(&output_path)
            .arg("--batch-size")
            .arg(self.batch_size.to_string())
            .arg("--device")
            .arg(self.device.to_string())
            .status()?;

        if !status.success() {
            return Err(CudaError::CudaFailed {
                status_code: status.code(),
                binary_path: self.binary_path.clone(),
            });
        }

        let mut output_file = File::open(&output_path)?;
        let stream = read_all_tiles(&mut output_file)?;

        if stream.header.k_sq != k_sq {
            return Err(ProtocolError::InvalidData(format!(
                "stream k_sq mismatch: expected {k_sq}, got {}",
                stream.header.k_sq
            ))
            .into());
        }

        if stream.header.tile_side != tile_side {
            return Err(ProtocolError::InvalidData(format!(
                "stream tile_side mismatch: expected {tile_side}, got {}",
                stream.header.tile_side
            ))
            .into());
        }

        if stream.tiles.len() != jobs.len() {
            return Err(ProtocolError::InvalidData(format!(
                "stream tile count mismatch: expected {}, got {}",
                jobs.len(),
                stream.tiles.len()
            ))
            .into());
        }

        let mut operators = Vec::with_capacity(stream.tiles.len());
        for (tile, job) in stream.tiles.into_iter().zip(jobs.iter()) {
            if tile.header.tile_id != job.tile_id {
                return Err(ProtocolError::InvalidData(format!(
                    "tile_id mismatch: expected {}, got {}",
                    job.tile_id, tile.header.tile_id
                ))
                .into());
            }
            operators.push(tile_operator_from_raw(tile.header, tile.ports, k_sq));
        }

        Ok(operators)
    }
}
