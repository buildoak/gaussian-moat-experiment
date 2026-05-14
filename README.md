# Gaussian Moat CUDA

CUDA/C++ implementation work for Gaussian moat computation.

Current status: the compact verification spine accepts campaign evidence through
four gates only: Exact Profile, Independent Tile Sample, SPANNING Cert, and MOAT
Hardening.

The current K36 hardening anchor is the full-ingest matrix at
`R_inner=80,000,000`, especially the `17k` to `20k` widths, with a wider
`32,768` confirmation row. All current K36 matrix rows postflight as `MOAT`
with exact BZ, zero overflow, stats_v2 telemetry, and persisted 512-tile sample
audit. The adjacent Tsuchimura pair remains useful as a calibration note:
`R_outer=80,015,782` is `SPANNING`; `R_outer=80,015,790` is `MOAT`.

On an RTX 4090, the current full CUDA pipeline runs at about `40k tiles/s`
end-to-end. The GPU TileOp stage is around `70k tiles/s`; the full pipeline is
currently bounded by CUDA work plus CPU streaming composition.

Paper writeup, further performance optimization, and larger moat campaigns are
work in progress. K37-K39 rows are telemetry-only until promoted through the
compact spine.

See:

- `AGENTS.md` for project rules and correctness hierarchy.
- `reference/current-verification-spine.md` for the active verification gates.
- `reference/attached-static-annulus-moats.md` for attached lower-K36 and
  local K34 static-annulus moat evidence.
- `reference/k26-static-annulus-diagnostics.md` for the K26 Tsuchimura-endpoint
  diagnostic campaign and why K32 is the next meaningful lower-K graph target.
- `methodology/tile-operator-definition-v-claude.md` for the mathematical
  implementation contract.
