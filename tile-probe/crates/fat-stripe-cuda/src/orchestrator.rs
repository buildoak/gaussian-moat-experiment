use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

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

/// Tracks the grid position of each tile so we can reconstruct
/// composition order after a single batched CUDA call.
#[derive(Debug, Clone, Copy)]
struct TilePosition {
    stripe_idx: usize,
    col_idx: usize,
}

pub fn run_campaign(config: &CudaFatStripeConfig) -> Result<CampaignResult, CudaError> {
    validate_config(config)?;

    let stripes = enumerate_stripes(config)?;
    let num_stripes = stripes.len();
    let num_tiles: usize = stripes.iter().map(Vec::len).sum();
    if num_tiles == 0 {
        return Ok(CampaignResult {
            blocked: true,
            num_tiles: 0,
            total_primes: 0,
            spanning_component: None,
        });
    }

    // Flatten all tiles into a single list, recording grid positions.
    let (all_specs, positions) = flatten_tiles(&stripes);
    let cols_per_stripe: Vec<usize> = stripes.iter().map(Vec::len).collect();

    eprintln!(
        "Campaign: {} stripes, {} total tiles, batching into single CUDA call",
        num_stripes, num_tiles
    );

    let driver = CudaDriver {
        binary_path: config.cuda_binary.clone(),
        device: config.cuda_device,
        batch_size: config.cuda_batch_size,
    };
    let session_work_dir = session_work_dir(&config.work_dir);
    fs::create_dir_all(&session_work_dir)?;

    let result = if config.gpu_boundary_merge {
        run_campaign_gpu_merge(config, &driver, &session_work_dir, &all_specs)
    } else {
        run_campaign_batched(
            config,
            &driver,
            &session_work_dir,
            &all_specs,
            &positions,
            &cols_per_stripe,
            num_stripes,
        )
    };
    let cleanup = cleanup_dir(&session_work_dir);

    match (result, cleanup) {
        (Ok(result), Ok(())) => Ok(result),
        (Err(err), _) => Err(err),
        (Ok(_), Err(err)) => Err(err),
    }
}

fn run_campaign_gpu_merge(
    config: &CudaFatStripeConfig,
    driver: &CudaDriver,
    session_work_dir: &Path,
    all_specs: &[TileSpec],
) -> Result<CampaignResult, CudaError> {
    let jobs = tile_jobs_global(all_specs, 0)?;
    let batch_dir = session_work_dir.join("gpu-merge");
    let t0 = Instant::now();
    let summary = driver.process_campaign_merge(
        &batch_dir,
        config.k_sq,
        config.tile_side,
        &jobs,
        config.gpu_uf || config.gpu_boundary_merge,
        config.compact_merge,
    )?;
    let elapsed = t0.elapsed();
    let cleanup = cleanup_dir(&batch_dir);
    if let Err(err) = cleanup {
        return Err(err);
    }

    eprintln!(
        "CUDA merged campaign: {} tiles, {} components, {} primes, {:.1}s",
        summary.num_tiles,
        summary.num_components,
        summary.total_primes,
        elapsed.as_secs_f64()
    );

    Ok(CampaignResult {
        blocked: summary.spanning_component.is_none(),
        num_tiles: summary.num_tiles,
        total_primes: summary.total_primes,
        spanning_component: summary.spanning_component,
    })
}

fn run_campaign_batched(
    config: &CudaFatStripeConfig,
    driver: &CudaDriver,
    session_work_dir: &Path,
    all_specs: &[TileSpec],
    positions: &[TilePosition],
    cols_per_stripe: &[usize],
    num_stripes: usize,
) -> Result<CampaignResult, CudaError> {
    let batch_limit = batch_limit(config.cuda_batch_size);
    let num_tiles = all_specs.len();

    // Split into CUDA calls if we exceed the batch limit.
    let chunks: Vec<(usize, usize)> = {
        let mut c = Vec::new();
        let mut start = 0;
        while start < num_tiles {
            let end = (start + batch_limit).min(num_tiles);
            c.push((start, end));
            start = end;
        }
        c
    };
    let num_cuda_calls = chunks.len();

    eprintln!(
        "Dispatching {} tile(s) in {} CUDA call(s) (batch limit: {})",
        num_tiles,
        num_cuda_calls,
        if batch_limit == usize::MAX {
            "unlimited".to_string()
        } else {
            batch_limit.to_string()
        }
    );

    // Collect all operators indexed by their global tile index.
    let mut all_operators: Vec<Option<TileOperator>> = vec![None; num_tiles];
    let mut total_primes = 0u64;

    for (call_idx, &(start, end)) in chunks.iter().enumerate() {
        let chunk_specs = &all_specs[start..end];
        let jobs = tile_jobs_global(chunk_specs, start)?;
        let batch_dir = session_work_dir.join(format!("batch-{:06}", call_idx + 1));

        let t0 = Instant::now();
        let batch_result = driver.process_batch(
            &batch_dir,
            config.k_sq,
            config.tile_side,
            &jobs,
            config.gpu_uf,
        );
        let cuda_elapsed = t0.elapsed();
        let cleanup = cleanup_dir(&batch_dir);
        let operators = match (batch_result, cleanup) {
            (Ok(operators), Ok(())) => operators,
            (Err(err), _) => return Err(err),
            (Ok(_), Err(err)) => return Err(err),
        };

        let batch_primes = operators.iter().map(|op| op.num_primes as u64).sum::<u64>();
        total_primes += batch_primes;

        eprintln!(
            "CUDA call {}/{}: {} tiles, {} primes, {:.1}s",
            call_idx + 1,
            num_cuda_calls,
            operators.len(),
            batch_primes,
            cuda_elapsed.as_secs_f64()
        );

        for (local_idx, op) in operators.into_iter().enumerate() {
            all_operators[start + local_idx] = Some(op);
        }
    }

    // Compose: group by stripe, horizontal within stripe, vertical across stripes.
    let t_compose = Instant::now();
    let mut full_op: Option<TileOperator> = None;

    for stripe_idx in 0..num_stripes {
        let num_cols = cols_per_stripe[stripe_idx];
        if num_cols == 0 {
            continue;
        }

        // Find the global tile indices for this stripe by scanning positions.
        // They are contiguous in the flattened array because we flatten stripe-by-stripe.
        let mut stripe_ops: Vec<Option<&TileOperator>> = vec![None; num_cols];
        for (global_idx, pos) in positions.iter().enumerate() {
            if pos.stripe_idx == stripe_idx {
                stripe_ops[pos.col_idx] = all_operators[global_idx].as_ref();
            }
        }

        // Horizontal composition (left to right within stripe).
        let mut stripe_op: Option<TileOperator> = None;
        for col_idx in 0..num_cols {
            if let Some(tile_op) = stripe_ops[col_idx] {
                stripe_op = Some(match stripe_op {
                    None => tile_op.clone(),
                    Some(acc) => compose_horizontal(&acc, tile_op, config.k_sq),
                });
            }
        }

        // Vertical composition (top to bottom across stripes).
        if let Some(stripe_op) = stripe_op {
            full_op = Some(match full_op {
                None => stripe_op,
                Some(acc) => compose_vertical(&acc, &stripe_op, config.k_sq),
            });
        }

        eprintln!(
            "Stripe {}/{}: composed {} tiles",
            stripe_idx + 1,
            num_stripes,
            num_cols
        );
    }

    let compose_elapsed = t_compose.elapsed();
    eprintln!("Composition: {:.1}s", compose_elapsed.as_secs_f64());

    let spanning_component = full_op
        .as_ref()
        .and_then(|op| spanning_component(op.component_faces.as_slice()));

    Ok(CampaignResult {
        blocked: spanning_component.is_none(),
        num_tiles: all_specs.len(),
        total_primes,
        spanning_component,
    })
}

/// Flatten the stripe-based tile grid into a single contiguous list,
/// returning both the specs and each tile's grid position.
fn flatten_tiles(stripes: &[Vec<TileSpec>]) -> (Vec<TileSpec>, Vec<TilePosition>) {
    let total: usize = stripes.iter().map(Vec::len).sum();
    let mut specs = Vec::with_capacity(total);
    let mut positions = Vec::with_capacity(total);

    for (stripe_idx, stripe) in stripes.iter().enumerate() {
        for (col_idx, spec) in stripe.iter().enumerate() {
            specs.push(*spec);
            positions.push(TilePosition {
                stripe_idx,
                col_idx,
            });
        }
    }

    (specs, positions)
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

/// Build TileJob records with globally unique tile_ids.
/// `global_offset` is the index of the first tile in this chunk.
fn tile_jobs_global(specs: &[TileSpec], global_offset: usize) -> Result<Vec<TileJob>, CudaError> {
    specs
        .iter()
        .enumerate()
        .map(|(idx, spec)| {
            let global_idx = global_offset + idx;
            Ok(TileJob {
                tile_id: u32::try_from(global_idx).map_err(|_| ProtocolError::CountTooLarge {
                    field: "tile_id",
                    value: global_idx as u64,
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
