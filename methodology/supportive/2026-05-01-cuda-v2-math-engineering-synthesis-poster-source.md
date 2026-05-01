---
title: "Poster 3 Source: TileOp Math to CUDA Engineering Synthesis"
date: 2026-05-01
type: poster-source
status: source-artifact
scope: tiles-maxxing/cuda-campaign-v2-sqrt-36
---

# Poster 3 Source: TileOp Math to CUDA Engineering Synthesis

## Authority And Evidence Boundary

This poster-source synthesizes the two existing source artifacts:

- `/Users/otonashi/thinking/building/gaussian-moat-cuda/methodology/supportive/2026-05-01-cuda-v2-engineering-poster-source.md`
- `/Users/otonashi/thinking/building/gaussian-moat-cuda/methodology/supportive/2026-05-01-tile-operator-math-poster-source.md`

Original canon/source paths cited by those artifacts:

- math canon: `/Users/otonashi/thinking/building/gaussian-moat-cuda/methodology/tile-operator-definition-v-claude.md`
- active CUDA implementation surface: `/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/cuda-campaign-v2-sqrt-36/`
- CPU reference implementation surface: `/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/cpp-campaign-v2/`
- repo operating instruction: `/Users/otonashi/thinking/building/gaussian-moat-cuda/AGENTS.md`

Status labels used here:

- **Canon:** mathematical definitions and obligations from the TileOp methodology
  as summarized in Poster 2.
- **Implementation evidence:** code architecture, file/module references, tests,
  gates, profile data, and current constants as summarized in Poster 1.
- **Inferred design intent:** synthesis language, visual organization, and
  crosswalk framing in this artifact.

No CUDA build, benchmark, test, or original code inspection was executed for
this artifact. Code references below are inherited from Poster 1 unless noted.
This is a poster-production source, not a new proof and not a new correctness
claim.

## Poster Thesis

**Canon:** TileOp replaces an infeasible global Gaussian-prime union-find with
local halo union-finds, canonical face ports, stitched port graphs, and UF-group
boundary flags.

**Implementation evidence:** CUDA v2 materializes that proof shape as a
deterministic TileOp factory. The CPU constructs the snapped grid, drives the
campaign, stitches TileOps, and writes the verdict artifact. The GPU fills the
local witness object: one tile enters as `TileCoord`; one 256-byte `TileOp`
comes out after K1-K5.

Poster headline options:

- "From Theorem To TileOp: How The Math Becomes CUDA Bytes"
- "The Proof Object Is 256 Bytes"
- "Local Graphs, Fixed Bytes, Global Verdict"

Best compact thesis line:

> The mathematics says what must be preserved; the CUDA path exists to
> manufacture that preservation, tile by tile, byte for byte.

## Visual Metaphor

Use a **blueprint-to-foundry** metaphor:

- left side: blueprint/math layer, drawn as thin blue linework and equations;
- center: foundry/conveyor layer, drawn as kernel chevrons and memory buffers;
- right side: inspection layer, drawn as green gates, hashes, and byte-layout
  gauges.

Primary object: the `TileOp{256 bytes}` should sit in the center as the shared
contract between math and engineering.

Avoid suggesting that the GPU proves the theorem. The GPU manufactures local
witness objects; the proof obligations and CPU compositor define how those
objects become a moat verdict.

## Color And Shape System

Suggested palette:

- **Canon blue:** definitions, lemmas, theorem labels, annulus geometry.
- **Implementation graphite:** structs, buffers, host orchestration, file refs.
- **Kernel amber:** K1-K5 execution blocks.
- **Gate green:** static asserts, hashes, parity tests, goldens, known-answer
  gates.
- **Caution red:** overflow, open obligations, improvement areas, drift risks.

Shape language:

- circles or annular bands for `G_full`, `geo_I`, `geo_O`;
- square grids for closed tiles, halos, towers, and face strips;
- rectangles for data structures: `Grid`, `TileCoord`, `CampaignConstants`,
  `TileOp`, buffers;
- chevrons for kernels K1-K5;
- bridges for shared-face port stitching;
- shields/checkpoints for validation gates;
- small red triangles for capacity/overflow and proof-hardening notes.

## One-Screen Main Diagram

Candidate poster diagram:

```text
MATH OBJECT                    DATA STRUCTURE                CUDA / HOST MATERIALIZATION                 VALIDATION GATE
--------------------------------------------------------------------------------------------------------------------------------
G_full over annular octant  -> CampaignConstants          -> apps/campaign_main_cuda.cpp validation   -> R/K/thickness checks
closed snapped tile grid    -> Grid + TileCoord[]         -> CPU active-tile enumeration              -> grid invariants/order
G_tile in each halo         -> bitmap + prime_pos[]       -> K1 sieve, K2 MR, K3 compact              -> FJ64 hash + parity
ufs_local + geo flags       -> parent[], labels, flags    -> K4 local UF + dense remap + geo bits     -> CPU/CUDA debug parity
face-strip ports            -> face reps + counts         -> K5a mini-DSU per face                    -> deterministic ordinals
TileOp_T                    -> 256-byte TileOp            -> K5b sort/pack                            -> byte parity + SHA
G_ports_grid                -> compositor DSU             -> CPU column ingest + finalize             -> Tsuchimura known answer
```

Caption:

> Every row is a preservation step: a mathematical object becomes a data
> structure, then a kernel or host responsibility, then a gate that prevents the
> implementation from quietly changing the theorem.

## Compact Materialization Chain

This is the poster's exact engineering+math crosswalk:

| Math object | Data structure | Kernel / host implementation | Validation gate | Improvement surface |
|---|---|---|---|---|
| Octant annulus and `G_full` | `CampaignConstants` | CPU CLI validation and constant upload | radius/K/thickness checks; constant static asserts | clearer executable assertions for deployment assumptions |
| Closed snapped tile grid | `Grid`, `TileCoord[]` | CPU grid build, region clip, canonical tile enumeration | grid invariants; stable tile order | defensive Lemma 4 prerequisite checks |
| Halo prime set `V(G_tile)` | `d_bitmap`, `d_prime_pos[]`, `d_prime_count[]` | K1 sieve, K2 MR, K3 compaction | FJ64 hash; CPU/CUDA prime-stream parity | MR hot path specialization; memory overlay |
| Local connectivity `ufs_local` | `d_parent[]`, `d_wire_label_by_raw_root[]` | K4 backward-offset UF, path compression, dense remap | parent/debug parity; deterministic dense labels | evidence-led UF tuning only if labels stay byte-stable |
| Boundary sets `geo_I`, `geo_O` | `d_prime_geo_bits`, `d_group_flags`, TileOp flag bitsets | K4 norm-form geo tests; K5 flag unpack | parity and BZ deployment awareness | make boundary reconciliation more visible in run artifacts |
| Face-strip ports | `d_face_reps`, `d_face_rep_counts` | K5a face filtering, mini-DSU, representative selection | deterministic port ordinals | specialize face encode without changing port semantics |
| `TileOp_T` | 256-byte `TileOp` | K5b sort and pack | layout asserts; full byte parity; snapshot SHA | keep capacity docs aligned with current caps and overflow behavior |
| `G_ports_grid` verdict | CPU compositor DSU | CPU column ingest, shared-face bridges, finalize | Tsuchimura known-answer gate | overlap compositor with GPU slabs while preserving canonical order |

## Crosswalk: Math Object To Code Surface

| Math concept | Canon obligation | Data structure / code surface | Implementation evidence | Validation gate |
|---|---|---|---|---|
| `G_full` over octant annulus | Gaussian-prime graph with edges `||p-q||^2 <= K`; verdict concerns `geo_I !~ geo_O` | `CampaignConstants`, annulus/radius fields | `apps/campaign_main_cuda.cpp:264-389`; `include/cuda_campaign/campaign_constants.cuh:10-33` | input validation; static constant checks; known-answer gate |
| `geo_I`, `geo_O` | Boundary bands are norm-form tests on primes, later reconciled per deployment with witness form | `d_prime_geo_bits`, `d_group_flags`, `inner_flags`, `outer_flags` | K4 geo bits: `src/kernel_uf_v2.cu:84-125`, `276-285`; K5 flag unpack: `src/kernel_face_encode_v2.cu:20-41` | CPU/CUDA parity; no exposed-face shortcut; BZ reconciliation remains deployment-sensitive |
| Closed snapped tiles and halos | Proper tile is closed; collar `C=floor(sqrt(K))` preserves neighbors of proper-region primes | `Grid`, `TileCoord`, compile-time `S`, `C`, `SIDE_EXP` | grid construction: `apps/campaign_main_cuda.cpp:400-433`; constants: `include/cuda_campaign/constants.cuh:9-80` | grid invariants; static asserts for K-specific geometry |
| Active towers | Active tiles form contiguous towers with no diagonal orphan gaps | `Grid` tower offsets and sorted active tile stream | CPU grid/reference surface: `cpp-campaign-v2/include/campaign/grid.h:48-167` | canonical tile order; methodology I1-I4 obligations |
| `G_tile` | Local graph uses primes in a tile halo and edges inherited from `G_full` | `d_bitmap`, `d_prime_pos`, `d_prime_count` | K1/K2/K3 pipeline: `src/kernel_sieve.cu`, `src/kernel_mr.cu`, `src/kernel_compact.cu` | FJ64 witness hash; candidate/bitmap/prime-stream parity |
| `ufs_local` | Local UF labels encode connectivity inside `G_tile` | `d_parent`, `d_wire_label_by_raw_root`, `d_max_label` | K4: `src/kernel_uf_v2.cu:162-349`; CPU reference: `cpp-campaign-v2/src/tileop.cpp:166-232` | smaller-root DSU parity; serial dense-remap preserves byte-stable labels |
| Face-strip ports | Ports are connected components of each face-strip graph, not arbitrary boundary bins | `d_face_indices`, `d_face_roots`, `d_face_reps`, `d_face_rep_counts` | K5a: `src/kernel_face_encode_v2.cu:237-355`; CPU reference: `cpp-campaign-v2/src/tileop.cpp:86-155` | deterministic representative selection and ordering |
| `face_f_groups[p]` | Port stores the local `G_tile` UF group label, not the face-strip label | `TileOp.face_groups` | K5b pack: `src/kernel_face_sort_pack.cu:52-151`; `include/cuda_campaign/tileop.cuh:11-31` | full TileOp byte parity; zeroed unused bytes |
| Shared-face stitching | Same snapped physical face has identical ordered ports on both neighboring tiles | compositor bridge logic over canonical TileOp stream | CPU compositor surface cited in Poster 1; campaign ingest: `apps/campaign_main_cuda.cpp:450-469` | snapshot SHA and known-answer gates; B16 defensive assertion remains an improvement area |
| Theorem 11 verdict | Moat iff no stitched port component carries both inner and outer flags | CPU compositor verdict | CPU owns final stitching/verdict; CUDA returns only TileOps | Tsuchimura K=36 known-answer gate: `80,015,782` spans, `80,015,790` moats |
| Conservative overflow | Engineering caps are not math constraints; overflow must not create false moat optimism | `OVERFLOW_BIT` TileOp, zero payload | K1/K4/K5 overflow paths; `test_k1_overflow_spanning.cpp:41-63` | compositor returns conservative `SPANNING` on overflow |

## Seven-Panel Poster Plan

### Panel A: "The Global Graph Is Too Large"

Visual:

- octant annulus with sparse Gaussian-prime dots;
- blue inner and outer bands;
- faint impossible global UF cloud over the full annulus.

Math copy:

```text
G_full = Gaussian primes in R
E = { {p,q} : ||p-q||^2 <= K }
Moat iff geo_I !~ geo_O
```

Implementation bridge:

- CPU validates radii, K, annulus thickness, and campaign constants.
- `CampaignConstants` carries the arithmetic bounds into CPU and CUDA.

Callout:

> The question is global connectivity; the implementation never tries to hold
> the global graph on the GPU.

### Panel B: "Snap The Proof Onto A Closed Grid"

Visual:

- curved annular octant overlaid with a staircase of closed square tiles;
- one enlarged tile showing proper region, collar, and halo;
- arrows from tile index `(i,j)` to `TileCoord(i,j,a_lo,b_lo)`.

Math copy:

```text
C = floor(sqrt(K))
neighbor of a proper-region prime lies inside the tile halo
```

Implementation bridge:

- `Grid` creates the active snapped tile stream.
- `TileCoord[]` is sorted in canonical flat-index order before CUDA dispatch.
- CUDA receives tile coordinates, not authority to redefine the geometry.

Gate:

> Closed boundaries and snapped offsets are proof assumptions; tile ordering is
> a snapshot and byte-parity contract.

### Panel C: "Discover The Local Vertex Set"

Visual:

- a halo square turning into a bitmap;
- candidates flowing through K1, then MR acceptance through K2, then a compact
  prime-position stream through K3.

Math copy:

```text
V(G_tile) = V(G_full) cap halo(T)
```

Implementation bridge:

- K1: candidate sieve over halo rows.
- K2: octant and annulus gates plus FJ64 Miller-Rabin writes the prime bitmap.
- K3: bitmap compaction emits deterministic `packed_pos = row * SIDE_EXP + col`.

Gate:

> Prime discovery is fenced by witness-table hash checks and CPU/CUDA parity,
> not by trust in a fast kernel.

### Panel D: "Turn Local Geometry Into Wire Labels"

Visual:

- colored prime components inside the halo;
- UF parent arrows compressing into dense labels;
- small badges on components for `geo_I` and `geo_O`.

Math copy:

```text
ufs_local(w; G_tile) = local component label
inner_flag_T(g) = exists w in geo_I with ufs_local(w)=g
outer_flag_T(g) = exists w in geo_O with ufs_local(w)=g
```

Implementation bridge:

- K4 walks backward neighbor offsets against the bitmap.
- It performs smaller-root union, path compression, serial dense visible-root
  remap, and group-flag OR.
- Serial dense remap is intentionally byte-stability preserving.

Gate:

> Boundary meaning attaches to UF groups, not to exposed faces. That is the
> implementation form of Theorem 11's boundary-flag obligation.

### Panel E: "Ports Are Face-Strip Components"

Visual:

- four face strips highlighted on a tile;
- each face strip segmented into connected components;
- ordered representative dots with ordinal numbers.

Math copy:

```text
Ports(face_f_primes) = connected components of G_facestrip_f
face_f_groups[p] = ufs_local(w; G_tile), w in port_p
```

Implementation bridge:

- K5a filters face-strip primes, builds a per-face mini DSU, and selects
  representatives.
- K5b sorts by `(h, p_perp, global_wire_label)` and packs face labels.

Gate:

> The face-strip UF defines port identity. The tile UF label defines how ports
> connect through the tile.

### Panel F: "The 256-Byte TileOp Is The Contract"

Visual:

- central byte-layout object with labels:

```text
[0..3]     n[4]
[4..195]   face_groups[192]
[196..211] inner_flags[16]
[212..227] outer_flags[16]
[228]      tile_flags
[229..255] reserved zero
```

Implementation bridge:

- CPU and CUDA both lock `sizeof(TileOp) == 256` and field offsets.
- Host dispatch asserts CPU/CUDA layout parity before running.
- Overflow and empty TileOps zero payloads and set explicit flags.

Callout:

> Performance is allowed to change how quickly this object is made. It is not
> allowed to change what the object means.

### Panel G: "Stitch, Verify, Improve"

Visual:

- TileOps arranged in columns;
- bridge edges between matching shared-face port ordinals;
- CPU compositor DSU coloring connected port components;
- final green/red verdict badge.

Math copy:

```text
Theorem 9: port paths agree with prime paths for port primes
Theorem 11: no mixed I/O port component iff moat
Theorem 12: octant verdict folds to full annulus by D4 symmetry
```

Implementation bridge:

- Host driver returns a canonical TileOp vector.
- `campaign_main_cuda` feeds the CPU compositor after dispatch.
- Snapshot writer and printed hashes provide durable evidence.

Known-answer gate:

| K | R_inner | R_outer | Expected |
|---|---:|---:|---|
| 36 | 80,000,000 | 80,015,782 | `SPANNING` |
| 36 | 80,000,000 | 80,015,790 | `MOAT` |

Gate copy:

> The known-answer gate validates the implementation route. It does not replace
> the methodology as the source of truth.

## Implementation Flow Diagram

Use this as a middle-strip swimlane:

```text
CPU: CLI + constants
  -> validate K/radii/thickness
  -> build Grid + active TileCoord[]
  -> dispatch_tile_batch(...)

GPU: per slab, one block per tile
  -> K1 candidate sieve
  -> K2 prime bitmap
  -> K3 compact prime stream
  -> K4 local UF + dense labels + geo flags
  -> K5a face-strip ports
  -> K5b TileOp pack

CPU: compositor + snapshot
  -> ingest canonical TileOp stream
  -> stitch same-ordinal shared-face ports
  -> test mixed inner/outer components
  -> write snapshot + hashes + verdict
```

Caption:

> The handoff happens at the TileOp boundary: GPU produces local witnesses; CPU
> performs global stitching and durable verdict emission.

## Proof Obligation To Engineering Gate Map

| Proof obligation | Why it matters | Engineering manifestation | Poster-safe caution |
|---|---|---|---|
| Snapped grid with closed boundaries | Shared faces must be identical physical strips | CPU `Grid`, `TileCoord`, canonical tile ordering | Do not imply unique ownership of boundary lattice points |
| Collar coverage | A tile proper prime's neighbors must be inside its halo | `C`, `SIDE_EXP`, halo coordinate reconstruction in K1/K2/K4 | Non-square K prefilters need `ceil(sqrt(K))` where canon requires it |
| Deterministic port enumeration | Bridge edges connect ordinal-to-ordinal across shared faces | K5 representative selection and sort order | Port order is semantic, not cosmetic |
| Local UF labels on ports | Same labels become within-tile port graph edges | K4 dense labels; K5 writes `face_groups` | Face-strip labels alone are insufficient |
| Boundary flags on groups | Interior boundary-band primes must not be lost | K4 geo bits and group flags; K5 flag unpack | Exposed face class is explanatory, not the verdict definition |
| Diagonal transition closure | Whole-grid graph uses face bridges, not diagonal bridges | Grid topology/tower obligations inherited from canon | I4/Lemma 8 must remain visible in proof-facing explanations |
| Conservative overflow | Caps must not create false moat results | zero-payload `OVERFLOW_BIT`; compositor treats overflow as spanning | Capacity is engineering, not mathematical truth |
| D4 full-annulus closure | One octant must represent the full annulus | CPU campaign semantics and Theorem 12 framing | Side-exposed faces are handled by symmetry, not ordinary moat boundaries |

## Callout Library

Primary callouts:

- "The theorem does not run on the GPU. The theorem constrains the object the GPU may emit."
- "A `TileOp` is a local proof witness with a fixed byte layout."
- "Ports are face-strip connected components, not boundary pixels."
- "Geo flags live on UF groups, so interior boundary-band primes stay visible."
- "Shared-face stitching is exact only because snapped-grid port lists are identical."
- "Overflow is a conservative signal, not an absence of evidence."
- "Trust accumulates through static layout checks, witness hashes, byte parity, snapshot SHA, and known answers."

Short copy fragments:

- "Math object -> buffer -> kernel -> byte contract -> gate."
- "One tile in. One deterministic witness out."
- "The proof obligation becomes a data-layout obligation."
- "The compositor is where local witnesses become a global verdict."
- "The fastest wrong label is still a failed artifact."
- "CUDA accelerates the local graph; it does not relax the global argument."

## Diagram Descriptions For Designer

1. **Blueprint left rail:** equations for `G_full`, `G_tile`, `Ports`, and
   Theorem 11. Draw as thin blue annotations linked by arrows to code objects.

2. **Foundry center rail:** K1-K5 chevrons moving downward. Each chevron should
   show one data representation: candidates, bitmap, compact stream, UF labels,
   face reps, TileOp bytes.

3. **Contract centerpiece:** large `TileOp{256}` byte ruler. Use crisp block
   segmentation so the poster can explain that correctness is materially packed
   into offsets and counts.

4. **Stitching right rail:** two neighboring tiles with matching ordinal ports,
   then many TileOps feeding a CPU compositor DSU. Use bridge lines only across
   face-adjacent tiles.

5. **Gate footer:** horizontal trust ladder:

```text
static_assert layout
  -> witness SHA
  -> CPU/CUDA parity
  -> snapshot SHA
  -> goldens
  -> Tsuchimura known answers
```

6. **Caution badges:** small red annotations for open/proof-hardening issues:
   BZ reconciliation, non-square K prefilter, deterministic stitching
   prerequisite, engineering caps, and tower/corner proof notes.

## Improvement Areas Without Weakening Correctness

These are poster-safe because they preserve the correctness posture: optimize or
clarify only after the same gates still pass.

1. **Two-phase device memory overlay**

   Current implementation allocates a flat `DeviceWorkspace` containing K1/K2
   and K3/K4/K5 buffers simultaneously. Overlaying earlier buffers with later
   buffers could improve slab size and memory pressure. Correctness condition:
   lifetimes must be proven by stream/event order and byte parity must remain
   unchanged.

2. **MR hot path specialization**

   Profile evidence in Poster 1 says `kernel_mr` is the largest GPU time share.
   Next work should use Nsight Compute to separate integer throughput, occupancy,
   register pressure, and table/cache behavior. Correctness condition: FJ64
   witness hash, CPU/CUDA primality parity, and snapshot gates remain unchanged.

3. **Face encode specialization**

   K5 is semantically dense because face-strip components are the math-defined
   ports. The serial mini-DSU and representative selection may be optimized only
   if deterministic port ordering and `face_f_groups[p] = ufs_local(...)`
   semantics stay byte-identical.

4. **CPU compositor overlap**

   Current top-level flow waits for all TileOps before compositor ingest.
   Column-aligned chunks or completed-column buffering could overlap CPU
   stitching with later GPU slabs. Correctness condition: canonical output order,
   snapshot parity, and final verdict semantics remain identical.

5. **Capacity and documentation alignment**

   CUDA currently uses `MAX_PRIMES_GPU = 8192` and
   `MAX_CANDIDATES_GPU = 16384`; older planning material mentions 6144-prime
   sizing. The poster should present current code facts and frame caps as
   engineering limits guarded by conservative overflow, not mathematical
   constraints.

6. **Defensive proof-to-code assertions**

   Poster 2 notes that stitching should explicitly assert Lemma 4's uniform
   snapped-grid prerequisite, and future deployments must rerun BZ reconciliation.
   These are strengthening moves: they make assumptions executable or visible
   without changing the theorem.

## Source Index For Poster 3

Primary source artifacts:

- `/Users/otonashi/thinking/building/gaussian-moat-cuda/methodology/supportive/2026-05-01-cuda-v2-engineering-poster-source.md`
- `/Users/otonashi/thinking/building/gaussian-moat-cuda/methodology/supportive/2026-05-01-tile-operator-math-poster-source.md`

Original canon/source anchors:

- `/Users/otonashi/thinking/building/gaussian-moat-cuda/methodology/tile-operator-definition-v-claude.md`
- `/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/cuda-campaign-v2-sqrt-36/`
- `/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/cpp-campaign-v2/`

High-value implementation refs inherited from Poster 1:

- CUDA constants: `include/cuda_campaign/constants.cuh`
- CUDA TileOp layout: `include/cuda_campaign/tileop.cuh`
- campaign constants mirror: `include/cuda_campaign/campaign_constants.cuh`
- constants/table upload: `src/constants_upload.cu`
- K1 sieve: `src/kernel_sieve.cu`
- K2 MR: `src/kernel_mr.cu`
- K3 compact: `src/kernel_compact.cu`
- K4 UF/labels/flags: `src/kernel_uf_v2.cu`
- K5 face encode: `src/kernel_face_encode_v2.cu`
- K5 sort/pack: `src/kernel_face_sort_pack.cu`
- host orchestration: `src/host_driver.cpp`
- campaign CLI/compositor/snapshot flow: `apps/campaign_main_cuda.cpp`
- CPU TileOp reference: `../cpp-campaign-v2/include/campaign/tileop.h`,
  `../cpp-campaign-v2/src/tileop.cpp`
- CPU grid reference: `../cpp-campaign-v2/include/campaign/grid.h`
- CPU compositor reference: `../cpp-campaign-v2/include/campaign/compositor.h`
- diff and parity surface: `apps/cuda_vs_cpu_diff.cpp`,
  `tests/test_full_tileop_parity.cpp`
- snapshot/golden gates: `scripts/run_snapshot_sha_gate.sh`,
  `tests/test_snapshot_sha_R80M.cpp`, `golden/manifest.json`,
  `golden/validate_golden.sh`

## Final Poster Takeaway

This poster should make one relationship impossible to miss:

```text
the math defines the invariant
the data structures preserve the invariant
the kernels manufacture the local witness
the host stitches witnesses into the verdict
the gates prove the implementation did not move the invariant
```

Closing line:

> CUDA v2 is credible because it treats the TileOp not as an optimization detail,
> but as the executable boundary between proof obligation and throughput.
