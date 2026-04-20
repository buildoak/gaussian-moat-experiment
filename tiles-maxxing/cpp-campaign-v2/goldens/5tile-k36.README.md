# 5-Tile Golden — `cpp-campaign-v2` Phase 3.1

Reference snapshot at **project scale** (`R_inner = 80_000_000`,
`R_outer = 80_008_192`, `K_SQ = 36`, offset `(1, 1)`). Five active tiles
selected to exercise the full TileOp pipeline: Gaussian-prime halo sieve,
local `G_tile` UF, dense-remap, face-strip UF, canonical positional port
sort, 256 B wire encode, snapshot header, and (downstream) compositor
stitching along both shared-face types.

**This is the correctness anchor.** The CPU reference and the future
CUDA port are both validated byte-for-byte against
`5tile-k36.snapshot.bin`.

## Contents

| File | Purpose |
|---|---|
| `5tile-k36.reference.py` | Self-contained Python 3.11+ reference implementation (PEP 723, `gmpy2`). Regenerates every other file in this directory. |
| `5tile-spec.json` | Region spec consumable by `campaign_main --region`. |
| `5tile-k36.snapshot.bin` | The locked golden bytes (1400 B = 120 B header + 5 × 256 B payload). |
| `5tile-k36.manifest.json` | Human-readable sidecar with hashes and per-tile metadata. |

Regenerate:

```bash
cd goldens
uv run 5tile-k36.reference.py
```

## Tile selection

Five tiles in the deep interior of the canonical octant
`R = { x ≥ 0, y ≥ x, R_inner² ≤ x² + y² ≤ R_outer² }`, centered around
`x ≈ 30 000 000`. At this `x` the inner arc sits at
`y ≈ 74 161 985` and the outer arc at `y ≈ 74 170 820` — an annulus
span of ~8 835 lattice rows, well outside any single tile's 256-row
reach. The five picks deliberately split into one cluster near the
inner arc, one middle-band probe, and one tile near the outer arc.

| Key | `(i, j)` | `(a_lo, b_lo)` | Role |
|---|---|---|---|
| T1 | `(117187, 289695)` | `(29 999 873, 74 161 921)` | Straddles inner arc — geo_I flag present |
| T2 | `(117187, 289696)` | `(29 999 873, 74 162 177)` | Just above inner arc — **adj T1 via face_I/O** |
| T3 | `(117187, 289710)` | `(29 999 873, 74 165 761)` | Mid-band — no geo flags |
| T4 | `(117187, 289729)` | `(29 999 873, 74 170 625)` | Straddles outer arc — geo_O flag present |
| T5 | `(117188, 289696)` | `(30 000 129, 74 162 177)` | Adjacent column — **adj T2 via face_L/R** |

### Why these five

- **Adjacency.** T1↔T2 share a horizontal face (T1's `face_O` ↔ T2's
  `face_I`) so within-tower `I/O` stitching is exercised. T2↔T5 share a
  vertical face (T2's `face_R` ↔ T5's `face_L`) so between-tower `L/R`
  stitching is exercised. Both stitching flavors are therefore covered
  by a single golden run.
- **Mixed flagged.** T1 has at least one UF component carrying
  `geo_inner`; T4 has at least one with `geo_outer`; T2, T3, T5 have
  neither band set — satisfying "at least one tile has primes only in
  geo_I band / only geo_O / neither" per the Phase 3.1 spec. At this
  annulus width no single tile can straddle both arcs simultaneously,
  so "both" is structurally impossible; the "neither" branch stands in.
- **Non-degenerate.** Every tile has ≥ 1 880 Gaussian primes in its
  halo (clipped to octant + annulus), producing non-trivial DSU
  topology and non-zero port counts on every face.
- **Interior.** All `i ≥ 117 187`, so side-exposed axis cases (BACKLOG
  B1) are avoided by construction — the golden does not test axis
  primes, which are a separate correctness story.
- **Project scale.** `R_inner = 80 000 000` is the production value;
  there is no "small scale" cheating.

## Per-tile stats

Computed by `5tile-k36.reference.py`:

| Tile | `(i, j)` | `N_primes` | `N_components` | `n[I,O,L,R]` | `sum(n)` | `has_inner` | `has_outer` | `tile_flags` |
|---|---|---|---|---|---|---|---|---|
| T1 | `(117187, 289695)` | 1880 | 74  | `[0, 17, 7, 13]` | 37 | True  | False | 0x00 |
| T2 | `(117187, 289696)` | 2551 | 101 | `[17, 28, 13, 19]` | 77 | False | False | 0x00 |
| T3 | `(117187, 289710)` | 2477 | 120 | `[18, 16, 16, 24]` | 74 | False | False | 0x00 |
| T4 | `(117187, 289729)` | 1934 | 86  | `[21, 0, 19, 8]` | 48 | False | True  | 0x00 |
| T5 | `(117188, 289696)` | 2491 | 104 | `[26, 21, 19, 21]` | 87 | False | False | 0x00 |

Lemma 4 (identical ordered ports on shared faces) holds at construction:

- T1.n_O = 17 = T2.n_I
- T2.n_R = 19 = T5.n_L

No tile hits the 128-group or 192-port budgets; `OVERFLOW_BIT` is zero
on every tile. T4's `face_O` is empty (`n[1] = 0`) — it sits above the
outer arc at this column's local `x`, so the upper face strip has no
octant+annulus primes.

## What was verified manually vs. derived by the script

- **Manually verified.** (a) The mathematical layer — that is, every
  invariant and definition cited by the script is traced back to a
  specific section of the math SSoT (`tile-operator-definition-v-claude.md`)
  and the blueprint (`campaign-blueprint.md`). Face-strip depth, port
  enumeration rule, `geo_I`/`geo_O` interval form (Model A), collar
  `C = floor_isqrt(K) = 6`, `OFFSET = (1, 1)`, byte offsets at
  `0/4/196/212/228/229`, and 1..128 (not 0..127) wire-label convention
  are all traceable. (b) Tile selection — each tile's arc relationship
  was computed by hand from the radii before running the sieve, so the
  expected flag pattern (T1 geo_I, T4 geo_O, rest neither) was predicted
  a priori and confirmed by the script output. (c) Lemma 4 port-count
  equality on both shared faces was asserted explicitly after running.
- **Computed by the script.** Gaussian-prime enumeration inside each
  tile's halo, the full local UF topology per tile, the dense-label
  assignment, face-strip UF decomposition, canonical port sort,
  per-group inner/outer flag accumulation, 256 B packed bytes, and the
  snapshot header with all three SHA-256 hashes. These were not traced
  by hand — the halo of each tile contains 1 800–2 500 primes; only
  the script can produce the exact bytes. Internal consistency
  (Lemma 4, port-count budget, no OVERFLOW, no EMPTY except where
  expected) was checked post-hoc.

A deliberately narrow hand-trace — the first three primes of T1's
sieve output and their membership in `geo_I` under Model A — was used
to sanity-check the norm-form comparison path during development, but
the committed snapshot bytes are whatever the script produces on the
locked inputs. Any future drift is a correctness bug in the script or
a spec drift; the bytes are not negotiable artifact.

## Wire format summary (for reviewers)

Per blueprint §5.2 and `include/campaign/tileop.h` `static_assert`
chain:

```
offset   0, size   4  : uint8_t n[4]          (N_I, N_O, N_L, N_R)
offset   4, size 192  : uint8_t face_groups   (contiguous, face order I,O,L,R; dense labels 1..128)
offset 196, size  16  : uint8_t inner_flags   (bit (g-1) for label g)
offset 212, size  16  : uint8_t outer_flags   (bit (g-1) for label g)
offset 228, size   1  : uint8_t tile_flags    (bit0=OVERFLOW, bit1=EMPTY, bit2=TOWER_CLOSING)
offset 229, size  27  : uint8_t reserved[27]  (zero)
total 256 B per tile
```

Snapshot header (120 B, per `include/campaign/snapshot.h`):

```
magic[4]            = "CMV2"
version (LE u32)    = 1
grid_params_hash    = SHA-256(canonical grid string)
constants_hash      = SHA-256("K=36;R_inner=80000000;R_outer=80008192;offset=1,1;collar=6")
mr_witness_set_sha  = 92b8b0ea7ae8703a3fae4f7a1581dd0d04e041bde4eb1d23621a8f39846e909c
tile_count (LE u64) = 5
bytes_per_tile      = 256
reserved[4]         = 0x00000000
```

Payload: 5 × 256 B in canonical `(i, j)` lex order.

## Hashes

```
snapshot.bin SHA-256       : f906d3032acde99f06db40dab6e3150a28c9f18c8fdd70fbaa33e11022f37d7d
constants_hash             : 5640cf17cdd59abd73d0d655ad2ff993581df20ff27aa2c5dc2b91f94b0277d7
grid_params_hash           : 30a0b5357c1b09f8d00c919dee6860a50f5f1574ae3d20a7ddfc11a9aa1b6974
mr_witness_set_sha256      : 92b8b0ea7ae8703a3fae4f7a1581dd0d04e041bde4eb1d23621a8f39846e909c
```

## Ambiguity resolved with conservative default

**`grid_params_hash` canonical form.** The execution plan
(`methodology/lemmas_v2/cpp-campaign-v2-execution-plan.md` §Q4) pins
`constants_hash` to the exact string
`"K=<K>;R_inner=<R_i>;R_outer=<R_o>;offset=<ox>,<oy>;collar=<C>"`
but does NOT publish an equivalent canonical serialization for
`grid_params_hash`. The reference script picks a structurally
identical convention:

```
"R_inner=<R_i>;R_outer=<R_o>;K=<K>;S=<S>;C=<C>;"
"offset=<ox>,<oy>;tile_count=<N>;tiles=<i1>,<j1>|<i2>,<j2>|..."
```

serialized as UTF-8 then SHA-256'd. This is the explicit call-out: the
C++ implementation must adopt the same form byte-for-byte for
`grid_params_hash` to match. If the C++ side prefers a different
canonical string, the golden's `grid_params_hash` (and therefore the
snapshot.bin bytes) must be regenerated.

All other pipeline decisions follow the blueprint and math doc
directly and leave no ambiguity.

## Regeneration

```bash
cd tiles-maxxing/cpp-campaign-v2/goldens
uv run 5tile-k36.reference.py
```

Re-running on the same machine yields bit-identical `snapshot.bin`.
The `manifest.json`'s `generated_at` field changes run-to-run; the
`snapshot_sha256` field inside the manifest does not.
