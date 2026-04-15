# tile-tales — Tile Composition Verification Suite

Proves that partitioning a lattice graph into tiles, computing per-tile connectivity, and composing tiles via union-find produces the **same spanning verdict** as brute-force union-find on the unpartitioned graph.

This is the ground truth test for the Gaussian moat computation pipeline: if both paths agree on every input, the tile abstraction is lossless.

## What it tests

Two independent paths to the same answer:

1. **Tiled path** (Campaign): graph → tile grid → per-tile groups + face ports → cross-tile composition → verdict
2. **Direct path** (DirectVerifier): graph → full union-find → verdict

Six test cases exercise spanning, moat, bridged moat, random removal, and different connectivity radii.

## Running

```bash
python run.py          # fast mode: 8x4 tiles, ~2M points
python run.py --full   # production: 64x32 tiles, ~134M points
```

Output PNGs are saved to `output/`.

## Dependencies

Python 3.10+, numpy, matplotlib.

## Architecture

| File | Role |
|------|------|
| `uf.py` | Union-find with path compression and union by rank |
| `graph.py` | 2D lattice graph with distance-based implicit edges |
| `tile.py` | Per-tile analysis: connected components + face ports |
| `grid.py` | Tower-based tile grid with neighbor iteration |
| `compositor.py` | Cross-tile group merging via port matching |
| `campaign.py` | Full tiled pipeline orchestrator |
| `direct.py` | Brute-force oracle (ground truth) |
| `visualize.py` | Matplotlib rendering for graphs, tiles, and verdicts |
| `run.py` | Entry point with six verification cases |

## Key conventions

- **Tile size**: 256 lattice units = 257 lattice points per axis (fencepost)
- **Shared boundaries**: adjacent tiles share boundary points (closed interval `[0, 256]`, not half-open)
- **Face naming**: I (inner/bottom), O (outer/top), L (left), R (right)
- **Spanning**: connected path from any bottom-edge point to any top-edge point
