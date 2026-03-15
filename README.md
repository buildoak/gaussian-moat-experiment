# gaussian-moat-cuda

Consolidated Jetson pipeline for Gaussian moat computation. Two components work together: a CUDA-accelerated prime sieve generates Gaussian primes at GPU speed, and a Rust solver analyzes their angular connectivity to find (or bound) moat distances.

## Directory Structure

```
gaussian-moat-cuda/
  src/              CUDA sieve source (.cu, .cuh)
  tests/            CUDA test files
  tools/            Python comparison/analysis scripts
  solver/           Rust angular connectivity solver
    src/            Rust source (angular, band, sieve, union-find, ...)
    Cargo.toml
    Cargo.lock
  CMakeLists.txt    Build config for CUDA sieve
  run-pipeline.sh   Two-stage pipeline runner
  PERFORMANCE.md    Measured throughput numbers
```

## Building

### CUDA sieve (requires CUDA toolkit + Jetson or NVIDIA GPU)

```bash
mkdir -p build && cd build
cmake ..
make
```

Produces `build/gm_cuda_primes`.

### Rust solver (requires Rust toolchain)

```bash
cd solver
cargo build --release
```

Produces `solver/target/release/gaussian-moat-solver`.

## Running the Pipeline

The two-stage pipeline chains CUDA sieve output into the Rust solver:

```bash
./run-pipeline.sh \
    --k-squared 36 \
    --norm-hi 1000000000 \
    --output-dir ./output \
    --wedges 0
```

Parameters:
- `--k-squared K` -- Jump distance squared (e.g., 36 for the sqrt(36) moat problem)
- `--norm-hi N` -- Upper norm bound for prime generation
- `--norm-lo N` -- Lower norm bound (default 0)
- `--output-dir D` -- Where to write GPRF and result files
- `--wedges W` -- Angular wedge count (0 = auto-detect from CPU cores)

### Running components individually

**CUDA sieve only** (generate GPRF file):
```bash
./build/gm_cuda_primes \
    --norm-lo 0 --norm-hi 1000000000 \
    --output primes.gprf --mode sieve
```

**Rust solver with GPRF file** (angular mode):
```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 36 --angular 0 --prime-file primes.gprf --profile
```

**Rust solver with internal sieve** (norm-stream mode, no GPU needed):
```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 26 --norm-bound 25000000000000
```

## How It Works

1. **CUDA sieve** generates all Gaussian primes in a norm range using a segmented sieve on GPU, writes them to a GPRF (Gaussian Prime Record Format) binary file.

2. **Rust solver** memory-maps the GPRF file, routes primes into angular wedges covering the first octant, processes each wedge in parallel with a sliding-band union-find, then stitches wedge boundaries to determine the connected component containing the origin.

3. The origin component's farthest point gives the moat distance lower bound for the given k-squared.

## GPRF Format

Binary format with 64-byte header:
- Magic: `0x47505246` ("GPRF")
- Version: 1
- Record count, norm_min, norm_max
- Each record: `{i32 a, i32 b, u64 norm}` = 16 bytes

The CUDA sieve writes GPRF natively; the Rust solver reads it via memory-mapped I/O.
