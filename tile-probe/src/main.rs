mod compose;
mod primes;
mod probe;
mod profile;
mod tile;

use clap::Parser;

#[derive(Parser)]
#[command(name = "tile-probe", about = "Tile-based Gaussian moat prober")]
struct Args {
    #[arg(long)]
    k_squared: u64,

    #[arg(long, default_value = "0")]
    r_min: f64,

    #[arg(long)]
    r_max: f64,

    #[arg(long, default_value = "128")]
    strip_width: f64,

    #[arg(long, default_value = "16")]
    num_strips: usize,

    #[arg(long, default_value = "1000")]
    tile_depth: f64,

    #[arg(long)]
    profile: bool,

    #[arg(long)]
    trace: bool,

    #[arg(long)]
    validate: bool,
}

fn main() {
    let _ = Args::parse();
}
