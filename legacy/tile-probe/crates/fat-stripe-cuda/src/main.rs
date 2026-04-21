use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;

use fat_stripe_cuda::config::CudaFatStripeConfig;
use fat_stripe_cuda::orchestrator::run_campaign;

#[derive(Debug, Parser)]
#[command(name = "fat-stripe-cuda")]
struct Cli {
    #[arg(long)]
    k_sq: u64,

    #[arg(long)]
    tile_side: u32,

    #[arg(long, default_value = "7")]
    collar: u32,

    #[arg(long)]
    r_min: u64,

    #[arg(long)]
    r_max: u64,

    #[arg(long, default_value = "0")]
    b_min: i64,

    #[arg(long)]
    b_max: i64,

    #[arg(long)]
    cuda_binary: PathBuf,

    #[arg(long, default_value = "0")]
    cuda_device: u32,

    #[arg(long, default_value = "0")]
    cuda_batch_size: u32,

    #[arg(long, default_value = "false")]
    gpu_uf: bool,

    #[arg(long, default_value = "false")]
    gpu_boundary_merge: bool,

    #[arg(long, default_value = "false")]
    compact_merge: bool,
}

fn main() -> ExitCode {
    let args = Cli::parse();
    let config = CudaFatStripeConfig::new(
        args.k_sq,
        args.tile_side,
        args.collar,
        args.r_min,
        args.r_max,
        args.b_min,
        args.b_max,
        args.cuda_binary,
        args.cuda_device,
        args.cuda_batch_size,
        args.gpu_uf,
        args.gpu_boundary_merge,
        args.compact_merge,
    );

    match run_campaign(&config) {
        Ok(result) => match serde_json::to_string(&result) {
            Ok(json) => {
                println!("{json}");
                ExitCode::SUCCESS
            }
            Err(err) => {
                eprintln!("failed to serialize result: {err}");
                ExitCode::FAILURE
            }
        },
        Err(err) => {
            eprintln!("{err}");
            ExitCode::FAILURE
        }
    }
}
