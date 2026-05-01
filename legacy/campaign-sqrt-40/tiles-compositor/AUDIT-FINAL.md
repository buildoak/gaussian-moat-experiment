CONDITIONAL

## Findings

### warning
`parse_counts` accepts TileOp layouts that violate the reference format's even-split invariant for the residual payload. The spec says the remaining payload after O/I/L sections "must be evenly split" between R-face groups and h1 bytes, but [src/tileop_parse.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/tileop_parse.cpp:43) only checks `residual >= 0` and then truncates with `counts.r_cnt = residual / 2` at [src/tileop_parse.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/tileop_parse.cpp:50). A malformed header such as `off_I=3, off_L=3, off_R=4` yields `l_cnt=1`, `residual=123` (odd), and is accepted even though one byte is left unaccounted. That is a spec mismatch in the binary parser.

### warning
The bug-fixed compositor paths are still under-tested for the adversarial cases called out in this audit. [test/test_compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/test/test_compositor.cpp:220) covers only `q=0,f=0` and `q=0,f=10` row matching; it does not exercise `q=31`, `q=32`, `f=255`, zero-port faces during compositor matching, or the `secondary_prev_row < 0` boundary. The inner/outer boundary collectors are also not directly regression-tested even though they are part of the final spanning verdict. I did not find an implementation defect in those functions, but the current tests do not prove those edge cases.

## Per-Function Verdicts

| Function / area | Verdict | Evidence |
|---|---|---|
| `match_io_within_tower` | CORRECT | Uses `FACE_O` on row `r` and `FACE_I` on row `r+1`, no `h1`, positional pairing up to `min(counts)` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:164). |
| `match_lr_with_previous` | CORRECT | Uses previous tower `FACE_R` and current tower `FACE_L`; primary row `r-q` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:254), secondary row `r-q-1` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:291), predicates `hl == hr + f` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:281) and `hl + (S-f) == hr` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:319). Negative secondary rows are guarded at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:294). |
| `collect_inner_boundary` | CORRECT | Collects `FACE_I` on row `0` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:334), whole exposed `FACE_L` rows `0..q-1` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:358), and fractional `FACE_L` ports with `h1 < f` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:388). |
| `collect_outer_boundary` | CORRECT | Collects `FACE_O` on row `31` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:416), whole exposed `FACE_R` rows `T-q..T-1` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:441), and fractional `FACE_R` ports with `h1 >= S-f` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:474). |
| Dead-tile predicate in `grid.h` | CORRECT | Matches the reference predicate exactly: `(base_y[j] - r*S) <= (j+1)*S` at [include/grid.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/include/grid.h:14). |
| `campaign.cpp` binary parsing / grid construction / tower feed order | CORRECT | Reads `n_tiles` from header at [src/campaign.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/campaign.cpp:84), extracts first record coordinates per tower at [src/campaign.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/campaign.cpp:93), reconstructs the grid with `compute_grid_from_coords` at [src/campaign.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/campaign.cpp:234), reads each tower's 32 records and strips TileOp bytes from offset 20 at [src/campaign.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/campaign.cpp:117), then feeds towers strictly in ascending order at [src/campaign.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/campaign.cpp:258). |

## Adversarial Edge-Case Check

- `q = 0`: handled by direct primary row match; no exposed whole rows; fractional handling depends on `f > 0`.
- `q = 31`: current rows `0..30` have no primary match; the code exposes those rows through the boundary collectors and still allows row `31` to match.
- `q = 32`: `match_lr_with_previous` produces no valid previous row; `collect_inner_boundary` collects all rows via `min(q, 32)` and `collect_outer_boundary` can expose all rows on the current tower's right side when the next tower is lower by 32 rows.
- `f = 0`: secondary matching and fractional collectors are skipped, as required.
- `f = 255`: arithmetic stays within `uint16_t`; `hr + f` and `hl + (256 - f)` are bounded well below overflow.
- Face with `0` ports: loops over `FaceSlice.count` become no-ops. This is parser-tested in [test/test_tileop.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/test/test_tileop.cpp:131), but not end-to-end in compositor tests.
- Secondary match with `r-q-1 < 0`: explicitly guarded at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:294).
- Integer overflow in `h1` decode / predicates: not observed. `decode_h1` returns `uint16_t` at [include/tileop_parse.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/include/tileop_parse.h:34), and the largest predicate sums remain small.
- First/last tower handling: `match_lr_with_previous` exits on `j <= 0` at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:230); `collect_outer_boundary` exits early on the last tower at [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:433).

## Test Results

Command run:

```bash
cd tiles-compositor && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc) && ./test_compositor && ./test_grid && ./test_tileop
```

Observed result:

- `test_compositor`: PASS
- `test_grid`: PASS
- `test_tileop`: PASS

Build note:

- `make -j$(nproc)` emitted `sysctl: sysctl fmt -1 1024 1: Operation not permitted` in this environment, but the build and all requested tests still completed successfully.

## What I Checked

- Reference document: `docs/supportive/2026-04-12-compositor-logic.md`
- Source files:
  - [src/compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/compositor.cpp:1)
  - [include/compositor.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/include/compositor.h:1)
  - [include/types.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/include/types.h:1)
  - [src/tileop_parse.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/tileop_parse.cpp:1)
  - [include/tileop_parse.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/include/tileop_parse.h:1)
  - [src/grid.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/grid.cpp:1)
  - [include/grid.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/include/grid.h:1)
  - [src/campaign.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/src/campaign.cpp:1)
  - [test/test_compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/test/test_compositor.cpp:1)
- Supporting tests reviewed for coverage gaps:
  - [test/test_grid.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/test/test_grid.cpp:1)
  - [test/test_tileop.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-compositor/test/test_tileop.cpp:1)

## Missing Coverage Recommendations

- Add compositor regression tests for `q=31`, `q=32`, and `f=255`.
- Add a test where `secondary_prev_row < 0` and confirm only the primary/collector paths run.
- Add direct tests for `collect_inner_boundary` and `collect_outer_boundary`, including fractional boundary ports exactly at `h1 = f-1`, `h1 = f`, `h1 = S-f-1`, and `h1 = S-f`.
- Add a parser test that rejects odd residual payload layouts in `parse_counts`.

## Overall Confidence

MEDIUM
