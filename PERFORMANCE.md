# Performance Measurements

All numbers are Gaussian primes/second unless noted otherwise.
Measured February-March 2026.

## CUDA Sieve on Jetson Orin (8 GB, 1024 CUDA cores)

| Scenario | Throughput | Config |
|----------|-----------|--------|
| Near origin (1B norms, norm range [0, 1e9)) | 6.67M primes/sec | `--mode sieve --norm-lo 0 --norm-hi 1000000000` |
| At sqrt(36) scale (10^15 norms) | 1.45M primes/sec | `--mode sieve` at high norm offsets |
| MR baseline (100M norms) | 481K primes/sec | `--mode mr --norm-hi 100000000` |
| MR optimized (100M norms) | 1.06M primes/sec | `--mode mr` with tuned batch/block sizes |

Hardware: NVIDIA Jetson Orin Nano 8GB, JetPack 6.x, CUDA 12.x
Date: February 2026

## Rust Angular Connector on Jetson Orin

| Scenario | Throughput | Config |
|----------|-----------|--------|
| With GPRF file | 1.35-1.78M primes/sec | `--angular 0 --prime-file <path>.gprf` |
| With internal sieve (no GPRF) | ~197K primes/sec | `--angular 0` (sieve on 6 ARM cores) |

Hardware: NVIDIA Jetson Orin Nano 8GB, 6-core ARM A78AE
Date: March 2026

## Cloud Baselines (vast.ai, for reference)

| Scenario | Throughput | Config |
|----------|-----------|--------|
| Norm-stream initial burst | ~617K primes/sec | Rust solver, norm-stream mode, first batch |
| Norm-stream steady state | ~307K primes/sec | Rust solver, sustained throughput |
| Autoresearch best | ~630K primes/sec | Automated pipeline best observed |

Hardware: vast.ai GPU instances (various), x86_64
Date: February-March 2026

## Key Takeaways

- CUDA sieve is 3-10x faster than Rust internal sieve for prime generation
- The pipeline (CUDA sieve -> GPRF file -> Rust angular) achieves peak throughput
  by offloading prime generation to GPU and connectivity analysis to CPU
- At high norm scales the sieve degrades gracefully (6.67M -> 1.45M at 10^15)
- Jetson with GPRF file matches or exceeds cloud baseline throughput
