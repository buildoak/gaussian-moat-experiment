# Gaussian Moat Verification

This directory is the independent verification surface for current
static-annulus CUDA campaign claims.

The code here is intentionally separate from `tiles-maxxing/` campaign code.
It may read JSON artifacts emitted by the campaign, but it must not link
against campaign `Grid`, `TileOp`, sieve, compositor, CUDA kernels, or
validator implementation. Shared concepts are duplicated in small, auditable
forms so the checks attack different failure modes.

Current first target:

- `K=36`, `W=32768`
- `73,339,843..73,372,611 SPANNING`
- `73,359,375..73,392,143 MOAT`

## Build

```bash
cmake -S verification -B verification/build
cmake --build verification/build -j
ctest --test-dir verification/build --output-on-failure
```

## Tools

| Tool | Role |
|---|---|
| `bz_exact_check` | Exact integer BZ check for square `K` values, currently used for K36 rows. |
| `boundary_semantics_test` | Standalone assertions for axis, diagonal, collars, clipping, and geo predicates. |
| `exact_global_uf` | Independent bounded global-UF oracle for small/medium annuli. |
| `tile_sample_check` | Independent production tile sample checker. |
| `span_cert_check` | Independent coordinate path certificate checker. |

`tile_sample_check` strengthens confidence in sampled production TileOps. It is
not a global proof of a negative MOAT row.
