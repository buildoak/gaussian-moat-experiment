#!/usr/bin/env python3
"""
compare.py — Diff GPU output vs Rust reference.

Placeholder for Wave 2+. Will compare:
  1. GPU-generated Gaussian primes against reference output from the Rust solver
  2. Timing benchmarks between CPU and GPU pipelines
  3. Norm-sorted order verification

Usage (planned):
  python3 compare.py --gpu-output gpu_primes.bin --ref-output ref_primes.bin
  python3 compare.py --gpu-output gpu_primes.csv --ref-output ref_primes.csv --format csv
"""

import sys

def main():
    print("compare.py — placeholder for GPU vs reference diffing")
    print("Not yet implemented. Will be filled in Wave 2+.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
