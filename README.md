# Gaussian Moat CUDA

CUDA implementation work for Gaussian moat computation.

Current status: the `k^2 = 36` pipeline is mathematically grounded in the
project methodology and verified against Tsuchimura's known moat boundary:

- `R_inner=80,000,000`, `R_outer=80,015,782` => `SPANNING`
- `R_inner=80,000,000`, `R_outer=80,015,790` => `MOAT`

On an RTX 4090, the current full CUDA pipeline runs at about `40k tiles/s`
end-to-end. The GPU TileOp stage is around `70k tiles/s`; the full pipeline is
currently bounded by CUDA work plus CPU streaming composition.

Paper writeup, further performance optimization, `sqrt(40)`, and larger moat
campaigns are work in progress.

See:

- `AGENTS.md` for project rules and correctness hierarchy.
- `reference/current-gate-board.md` for the current verified baseline.
- `methodology/tile-operator-definition-v-claude.md` for the mathematical
  implementation contract.
