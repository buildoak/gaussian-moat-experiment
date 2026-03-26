use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process;
use std::time::{SystemTime, UNIX_EPOCH};

use moat_kernel::compose::{compose_horizontal, compose_vertical};
use moat_kernel::tile::{TileOperator, FACE_INNER_BIT, FACE_OUTER_BIT};
use serde::Serialize;

use crate::config::CudaFatStripeConfig;
use crate::driver::{CudaDriver, CudaError};
use crate::protocol::{ProtocolError, TileJob};

#[derive(Debug, Clone, Serialize)]
pub struct CampaignResult {
    pub blocked: bool,
    pub num_tiles: usize,
    pub total_primes: u64,
    pub spanning_component: Option<usize>,
}

#[derive(Debug, Clone, Copy)]
struct TileSpec {
    a_lo: i64,
    b_lo: i64,
}

pub fn run_campaign(config: &CudaFatStripeConfig) -> Result<CampaignResult, CudaError> {
    validate_config(config)?;

    let stripes = enumerate_stripes(config)?;
    let num_tiles: usize = stripes.iter().map(Vec::len).sum();
    if num_tiles == 0 {
        return Ok(CampaignResult {
            blocked: true,
            num_tiles: 0,
            total_primes: 0,
            spanning_component: None,
        });
    }

    let batch_limit = batch_limit(config.cuda_batch_size);
    let total_batches = stripes
        .iter()
        .map(|stripe| stripe.len().div_ceil(batch_limit))
        .sum();

    let driver = CudaDriver {
        binary_path: config.cuda_binary.clone(),
        device: config.cuda_device,
        batch_size: config.cuda_batch_size,
    };
    let session_work_dir = session_work_dir(&config.work_dir);
    fs::create_dir_all(&session_work_dir)?;

    let result = run_campaign_inner(config, &driver, &session_work_dir, &stripes, total_batches);
    let cleanup = cleanup_dir(&session_work_dir);

    match (result, cleanup) {
        (Ok(result), Ok(())) => Ok(result),
        (Err(err), _) => Err(err),
        (Ok(_), Err(err)) => Err(err),
    }
}

fn run_campaign_inner(
    config: &CudaFatStripeConfig,
    driver: &CudaDriver,
    session_work_dir: &Path,
    stripes: &[Vec<TileSpec>],
    total_batches: usize,
) -> Result<CampaignResult, CudaError> {
    let batch_limit = batch_limit(config.cuda_batch_size);
    let mut global_batch_idx = 0usize;
    let mut total_primes = 0u64;
    let mut full_op: Option<TileOperator> = None;

    for (stripe_idx, stripe) in stripes.iter().enumerate() {
        let mut stripe_op: Option<TileOperator> = None;

        for jobs_in_batch in stripe.chunks(batch_limit) {
            let jobs = tile_jobs(jobs_in_batch)?;
            let batch_dir = session_work_dir.join(format!("batch-{:06}", global_batch_idx + 1));

            let batch_result =
                driver.process_batch(&batch_dir, config.k_sq, config.tile_side, &jobs);
            let cleanup = cleanup_dir(&batch_dir);
            let operators = match (batch_result, cleanup) {
                (Ok(operators), Ok(())) => operators,
                (Err(err), _) => return Err(err),
                (Ok(_), Err(err)) => return Err(err),
            };

            let batch_primes = operators.iter().map(|op| op.num_primes as u64).sum::<u64>();
            total_primes += batch_primes;

            for tile_op in operators {
                stripe_op = Some(match stripe_op {
                    None => tile_op,
                    Some(acc) => compose_horizontal(&acc, &tile_op, config.k_sq),
                });
            }

            global_batch_idx += 1;
            eprintln!(
                "Batch {}/{}: processed {} tiles, {} primes",
                global_batch_idx,
                total_batches,
                jobs_in_batch.len(),
                batch_primes
            );
        }

        if let Some(stripe_op) = stripe_op {
            full_op = Some(match full_op {
                None => stripe_op,
                Some(acc) => compose_vertical(&acc, &stripe_op, config.k_sq),
            });
        }

        eprintln!(
            "Stripe {}/{}: composed {} tiles",
            stripe_idx + 1,
            stripes.len(),
            stripe.len()
        );
    }

    let spanning_component = full_op
        .as_ref()
        .and_then(|op| spanning_component(op.component_faces.as_slice()));

    Ok(CampaignResult {
        blocked: spanning_component.is_none(),
        num_tiles: stripes.iter().map(Vec::len).sum(),
        total_primes,
        spanning_component,
    })
}

fn validate_config(config: &CudaFatStripeConfig) -> Result<(), CudaError> {
    if config.tile_side == 0 {
        return Err(ProtocolError::InvalidData("tile_side must be positive".to_string()).into());
    }
    if config.r_min >= config.r_max {
        return Err(ProtocolError::InvalidData("r_min must be less than r_max".to_string()).into());
    }
    if config.b_min >= config.b_max {
        return Err(ProtocolError::InvalidData("b_min must be less than b_max".to_string()).into());
    }
    Ok(())
}

fn enumerate_stripes(config: &CudaFatStripeConfig) -> Result<Vec<Vec<TileSpec>>, CudaError> {
    let tile_side = i64::from(config.tile_side);
    let a_start = i64::try_from(config.r_min).map_err(|_| {
        ProtocolError::InvalidData(format!("r_min {} does not fit in i64", config.r_min))
    })?;
    let a_end = i64::try_from(config.r_max).map_err(|_| {
        ProtocolError::InvalidData(format!("r_max {} does not fit in i64", config.r_max))
    })?;

    let mut stripes = Vec::new();
    let mut a_lo = a_start;
    while a_lo < a_end {
        let mut stripe = Vec::new();
        let mut b_lo = config.b_min;
        while b_lo < config.b_max {
            stripe.push(TileSpec { a_lo, b_lo });
            b_lo += tile_side;
        }
        stripes.push(stripe);
        a_lo += tile_side;
    }

    Ok(stripes)
}

fn tile_jobs(specs: &[TileSpec]) -> Result<Vec<TileJob>, CudaError> {
    specs
        .iter()
        .enumerate()
        .map(|(idx, spec)| {
            Ok(TileJob {
                tile_id: u32::try_from(idx).map_err(|_| ProtocolError::CountTooLarge {
                    field: "tile_id",
                    value: idx as u64,
                })?,
                a_lo: checked_i32(spec.a_lo, "a_lo")?,
                b_lo: checked_i32(spec.b_lo, "b_lo")?,
                reserved: 0,
            })
        })
        .collect()
}

fn checked_i32(value: i64, field: &'static str) -> Result<i32, CudaError> {
    i32::try_from(value).map_err(|_| {
        ProtocolError::InvalidData(format!("{field} {value} does not fit in i32")).into()
    })
}

fn batch_limit(cuda_batch_size: u32) -> usize {
    if cuda_batch_size == 0 {
        usize::MAX
    } else {
        cuda_batch_size as usize
    }
}

fn spanning_component(component_faces: &[u8]) -> Option<usize> {
    component_faces
        .iter()
        .position(|faces| faces & FACE_INNER_BIT != 0 && faces & FACE_OUTER_BIT != 0)
}

fn session_work_dir(base: &Path) -> PathBuf {
    base.join(format!(
        "fat-stripe-cuda-{}-{}",
        process::id(),
        unix_timestamp_nanos()
    ))
}

fn unix_timestamp_nanos() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos()
}

fn cleanup_dir(path: &Path) -> Result<(), CudaError> {
    match fs::remove_dir_all(path) {
        Ok(()) => Ok(()),
        Err(err) if err.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(err) => Err(err.into()),
    }
}
