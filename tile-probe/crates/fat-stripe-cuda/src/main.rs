use std::path::PathBuf;

use clap::Parser;

use fat_stripe_cuda::config::CudaFatStripeConfig;

#[derive(Debug, Parser)]
#[command(name = "fat-stripe-cuda", about = "CUDA-backed fat-stripe driver stub")]
struct Args {
    #[arg(long)]
    k_squared: u64,

    #[arg(long)]
    tile_side: u32,

    #[arg(long)]
    r_min: f64,

    #[arg(long)]
    r_max: f64,

    #[arg(long, default_value = "0")]
    b_min: i64,

    #[arg(long)]
    b_max: i64,

    #[arg(long)]
    cuda_binary: PathBuf,

    #[arg(long, default_value = "0")]
    cuda_device: u32,

    #[arg(long, default_value = "1024")]
    cuda_batch_size: u32,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();
    let config = CudaFatStripeConfig {
        k_sq: args.k_squared,
        tile_side: args.tile_side,
        collar: (args.k_squared as f64).sqrt().ceil() as u32,
        r_min: args.r_min,
        r_max: args.r_max,
        b_min: args.b_min,
        b_max: args.b_max,
        cuda_binary: args.cuda_binary,
        cuda_device: args.cuda_device,
        cuda_batch_size: args.cuda_batch_size,
    };

    println!("{}", serde_json::to_string_pretty(&config)?);
    eprintln!("orchestrator not implemented yet");
    Ok(())
}
