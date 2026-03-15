# gaussian-moat-cuda

GPU-accelerated solver for the [Gaussian moat problem](https://en.wikipedia.org/wiki/Gaussian_moat).

The Gaussian moat problem asks whether one can walk from the origin to infinity in the complex plane, stepping only on Gaussian primes, with each step bounded by some distance k. Tsuchimura (2005) proved computationally that for step size sqrt(36), the origin's connected component is finite — the walk gets stuck at distance ~80M from the origin. This repo is a modern GPU+CPU pipeline to reproduce and extend that result using CUDA segmented sieving and parallel connectivity analysis.

## Status

Research scratchpad. Active development. Not production code. Performance numbers are real and correctness is verified against known results, but the codebase is evolving.

## Architecture

Two-stage pipeline:

```
┌─────────────────────────────────────────┐     ┌──────────────────────────────────────────────┐
│  Stage 1: CUDA Sieve                    │     │  Stage 2: Rust Angular Connector              │
│                                         │     │                                              │
│  Segmented sieve on GPU                 │     │  Memory-mapped GPRF reader                   │
│  + Cornacchia decomposition             │ ──► │  + Parallel wedge decomposition (first octant)│
│  → GPRF binary file (Gaussian primes)   │     │  + Sliding-band union-find per wedge          │
│                                         │     │  + Boundary stitching → moat detection        │
└─────────────────────────────────────────┘     └──────────────────────────────────────────────┘
```

## Performance (Jetson Orin Nano 8GB)

| Component | Throughput (primes/sec) | Notes |
|-----------|------------------------|-------|
| CUDA sieve near origin | 6.67M | norm range [0, 10^9) |
| CUDA sieve at sqrt(36) scale | 1.45M | 10^15 norms |
| Rust connector + GPRF file | 1.35–1.78M | angular mode, 6 ARM cores |
| Rust connector internal sieve | ~197K | no GPU, CPU-only fallback |
| **Pipeline balanced throughput** | **~1.4M** | CUDA sieve feeding Rust connector |

## Verified Results

| k² | Moat found at | Origin component size |
|----|---------------|-----------------------|
| 2 | (11, 4) | — |
| 4 | — | 92 |
| 20 | — | 273,791,623 |

## Directory Structure

```
gaussian-moat-cuda/
  src/                  CUDA sieve (.cu, .cuh) — kernels, Cornacchia, Miller-Rabin, GPRF writer
  solver/               Rust angular connectivity solver
    src/                angular, band, sieve, union-find, stitcher, GPRF reader
  tests/                CUDA correctness tests
  tools/                Analysis scripts (WIP)
  deploy/               A100 deployment and campaign scripts
  CMakeLists.txt        CUDA build config
  run-pipeline.sh       Two-stage pipeline runner
```

## Building

### CUDA sieve

Requires CUDA toolkit and an NVIDIA GPU.

```bash
mkdir -p build && cd build
cmake -DCMAKE_CUDA_ARCHITECTURES=87 .. && make -j$(nproc)
```

For A100:

```bash
cmake -DTARGET_DEVICE=a100 ..
```

### Rust solver

```bash
cd solver && cargo build --release
```

## Usage

**Run the full pipeline:**

```bash
./run-pipeline.sh --k-squared 36 --norm-hi 1000000000 --output-dir ./output --wedges 0
```

**CUDA sieve only** (generate GPRF file):

```bash
./build/gm_cuda_primes --norm-lo 0 --norm-hi 1000000000 --output primes.gprf --mode sieve
```

**Rust solver with GPRF** (angular connectivity):

```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 36 --angular 0 --prime-file primes.gprf --profile
```

**Rust solver standalone** (norm-stream mode, no GPU needed):

```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 26 --norm-bound 25000000000000
```

## License

MIT — see [LICENSE](LICENSE).
