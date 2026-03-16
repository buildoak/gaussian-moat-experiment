# gaussian-moat-cuda

> **Work in progress.** This is an active research scratchpad. Performance numbers are being validated, architecture is under active optimization, and breaking changes are expected. Contributions welcome.

GPU-accelerated solver for the [Gaussian moat problem](https://en.wikipedia.org/wiki/Gaussian_moat).

The Gaussian moat problem asks: can you walk from the origin to infinity in the complex plane, stepping only on Gaussian primes, with each step at most sqrt(k)? Tsuchimura (2005) proved computationally that for k = 36 the walk gets stuck -- the origin's connected component is finite, bounded at distance ~80M. This project is a modern CUDA + Rust pipeline to reproduce that result and push toward sqrt(40).

## Architecture

Two-stage pipeline: GPU-accelerated prime generation followed by CPU-parallel connectivity analysis.

```
Stage 1: CUDA Sieve                         Stage 2: Rust Angular Connector

Segmented Eratosthenes on GPU               Memory-mapped GPRF reader
+ Cornacchia decomposition (p = a^2 + b^2)  + Parallel angular wedge decomposition
--> GPRF binary file                   -->  + Sliding-band union-find per wedge
    (a:i32, b:i32, norm:u64)                + Boundary stitching --> moat detection
    16 bytes/record
```

**CUDA Segmented Sieve + Cornacchia.** The GPU runs a segmented Eratosthenes sieve to find regular primes, then applies Cornacchia's algorithm to decompose each p = 1 (mod 4) into a sum of two squares. Results are written in GPRF binary format (16 bytes per Gaussian prime record).

**Rust Angular Connector.** Reads GPRF files via memory-mapped I/O. Decomposes the first octant into angular wedges processed in parallel, each using a spatial hash grid and union-find structure (BandProcessor). Supports both lower-bound search (grow from origin) and upper-bound probe mode (Tsuchimura's trick: start from a known boundary distance and verify no path escapes).

**Device Profiles.** Compile-time configuration for Jetson Orin Nano (SM 8.7, 1024 CUDA cores) and A100 (SM 8.0, 108 SMs). Tuned register counts and block sizes per target.

## Performance

### Jetson Orin Nano 8GB (verified)

**CUDA Sieve:**

| Scale | Throughput | Notes |
|-------|-----------|-------|
| Near origin (10^9 norms) | 6.67M primes/sec | `--norm-lo 0 --norm-hi 1000000000` |
| sqrt(36) range (10^15 norms) | 1.45M primes/sec | High norm offsets |

**Rust Angular Connector:**

| Mode | Throughput | Notes |
|------|-----------|-------|
| With GPRF file, angular=6 | 1.35--1.78M primes/sec | 6 ARM cores, 26.4M primes |
| CPU-only fallback (no GPRF) | ~197K primes/sec | Internal sieve on ARM cores |

**Pipeline balanced throughput:** ~1.4M primes/sec (CUDA sieve feeding Rust connector).

Hardware: NVIDIA Jetson Orin Nano 8GB, 6-core ARM A78AE, JetPack 6.x, CUDA 12.x. Measured February--March 2026.

### Cloud: A100-SXM4-40GB (preliminary)

These numbers are from initial A100 benchmarks and are being re-validated. Treat as directional.

**CUDA Sieve (post-D3 segmented base sieve):**

| Scale | Sieve Mode | MR Mode |
|-------|-----------|---------|
| 10^9 (near origin) | 20.7M primes/sec | 9.0M primes/sec |
| 10^15 (sqrt(36)) | 4.0M primes/sec | 1.7M primes/sec |
| 10^18 (sqrt(40) probe) | 1.33M primes/sec | 1.49M primes/sec |

At 10^18, the Miller-Rabin kernel overtakes the sieve (zero CPU base-prime prep). The crossover point is between 10^15 and 10^18.

Hardware: NVIDIA A100-SXM4-40GB, 12-vCPU Intel Xeon, 85 GB RAM, CUDA 12.1. Measured March 2026.

### Verified Results

| k^2 | Moat found at | Origin component size |
|-----|--------------|----------------------|
| 2 | (11, 4) | -- |
| 4 | -- | 92 |
| 20 | -- | 273,791,623 |

## Building

### Prerequisites

- CUDA Toolkit 12.x and an NVIDIA GPU
- Rust toolchain (stable, edition 2021)
- CMake >= 3.18

### CUDA Sieve

```bash
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

By default this targets Jetson Orin Nano (SM 8.7). For A100:

```bash
cmake -DTARGET_DEVICE=a100 .. && make -j$(nproc)
```

### Rust Solver

```bash
cd solver && cargo build --release
```

## Usage

### Full pipeline (sieve + connectivity)

```bash
./run-pipeline.sh --k-squared 36 --norm-hi 1000000000 --output-dir ./output
```

### CUDA sieve only (generate GPRF file)

```bash
./build/gm_cuda_primes --norm-lo 0 --norm-hi 1000000000 --output primes.gprf --mode sieve
```

### Rust solver with GPRF (angular connectivity, lower-bound)

```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 36 --angular 6 --prime-file primes.gprf
```

### Upper-bound probe (Tsuchimura's trick)

```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 36 --angular 6 --prime-file primes.gprf --start-distance 80015782
```

### Norm-stream mode (CPU-only, no GPU required)

```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 26 --norm-bound 25000000000000
```

## Directory Structure

```
gaussian-moat-cuda/
  src/                  CUDA sieve (.cu, .cuh) -- kernels, Cornacchia, Miller-Rabin, GPRF writer
  solver/               Rust angular connectivity solver
    src/                angular, band, sieve, union-find, stitcher, GPRF reader
  tests/                CUDA correctness tests
  tools/                Analysis scripts
  deploy/               A100 deployment and campaign scripts
  run-pipeline.sh       Two-stage pipeline runner
  CMakeLists.txt        CUDA build config (device profiles)
  PERFORMANCE.md        Full benchmark data and bottleneck analysis
```

## License

MIT -- see [LICENSE](LICENSE).
