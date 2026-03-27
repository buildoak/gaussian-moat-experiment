---
date: 2026-03-27
engine: coordinator
status: complete
---

# Compact-Merge Benchmark Results — RTX 3090

## Summary

The compact-merge pipeline (`--gpu-uf --gpu-boundary-merge --compact-merge`) is validated up to 5.5M tiles (600K×600K window) on RTX 3090. This is the production configuration for k²=40 campaign sweeps.

## Key Commit

`b275dc7` — "fix: complete compact-merge migration — stream seam batches, remove dead allocs, delete legacy path"

- Streaming seam batches (fixes host OOM)
- Removed root_to_dense dense vector (660MB at scale)
- Removed dead allocations (d_comp_counter, d_origin_set)
- Deleted legacy PreparedMergeData merge path (648 lines removed)

## Benchmark Table (k²=40, tile-side=256, RTX 3090, compact-merge)

| Window | Tiles | Primes | Wall time | Merge time | Seam batches | Result |
|--------|-------|--------|-----------|------------|--------------|--------|
| 128K | 250,000 | 1.03B | 42.7s | 2.4s | 1 | spanning=true |
| 192K | 564,001 | 2.24B | 88.6s | 5.1s | 1 | spanning=true |
| 256K | 1,000,000 | 3.87B | 153.1s | 9.1s | 1 | spanning=true |
| 600K | 5,494,336 | 19.9B | 1027.4s | 64.7s | 9 | spanning=true |

## Scaling Notes

- Per-tile UF cost: 114.0 KB + compact: 16.0 KB = 130 KB/tile
- GPU batch cap: ~183K tiles per UF dispatch
- Seam batching kicks in above ~1.3M tiles per merge batch
- The 600K run used 9 seam batches, confirming streaming works end-to-end
- 24,954,467 global components before merge → 16,638,038 after merge

## Operational Ceiling

- **600K windows are proven safe.** 5.5M tiles, 1027s, no OOM.
- **320K was interrupted but was running successfully at 1.57M tiles before being killed.**
- k²=40 at higher radii may produce different tile densities — monitor component counts for uint32 ceiling (~4.29B).

## Campaign Configuration

Recommended for k²=40 campaign:

```
fat-stripe-cuda --k-sq 40 --tile-side 256 \
  --r-min <radius> --r-max <radius + 256000> --b-max 256000 \
  --cuda-binary /gaussian-moat-cuda/build/fat_stripe_cuda \
  --gpu-uf --gpu-boundary-merge --compact-merge
```

256K windows give ~1M tiles at ~153s each — the sweet spot for rapid sweeps.
