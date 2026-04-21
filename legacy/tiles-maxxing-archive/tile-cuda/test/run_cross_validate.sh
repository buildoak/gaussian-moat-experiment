#!/bin/bash
# Run cross-validation: build, execute both pipelines, compare results.
# Usage: ./test/run_cross_validate.sh [num_tiles]

set -euo pipefail

NUM_TILES="${1:-1000}"
cd "$(dirname "$0")/.."

echo "=== Building test binaries ==="
make test

echo ""
echo "=== Running C++ reference pipeline (${NUM_TILES} tiles) ==="
./cpp_dump -n "${NUM_TILES}" -o cpp_tileops.bin

echo ""
echo "=== Running CUDA kernel pipeline (${NUM_TILES} tiles) ==="
./cuda_dump -n "${NUM_TILES}" -o cuda_tileops.bin

echo ""
echo "=== Comparing results ==="
./cross_compare --cpp cpp_tileops.bin --cuda cuda_tileops.bin -v

echo ""
echo "Done."
