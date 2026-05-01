---
title: Group Overflow Analysis
date: 2026-04-22
engine: codex
type: investigation
status: complete
refs: [tiles-maxxing/cuda-campaign-v2-sqrt-36/src/host_driver.cpp, tiles-maxxing/cuda-campaign-v2-sqrt-36/src/kernel_uf_v2.cu]
---

# Summary

Investigated the K=36, R=80M group-count overflow on vast.ai instance
35425891 (`ssh7.vast.ai:25890`, RTX 4090, `sm_89`). The first captured
overflow tile is the reported sample:

| Field | Value |
|---|---:|
| tile | `(0, 312509)` |
| origin | `(a_lo=1, b_lo=80002305)` |
| K1 raw candidates | 5,605 |
| K3 primes | 2,364 |
| K4 reported `max_label` | 128 |

Root cause: **dense remap labels every local UF component, including internal
components that are not represented in TileOp output**. The tile has 130 real
local UF components, but only 54 are TileOp-visible through a face strip or
geo flag. The current first-appearance dense remap spends labels on 76
irrelevant internal components, reaches the 128-label cap, and leaves two
visible face components unlabeled.

This is not a GPU UF merge bug and not a sparse-label compaction bug.

# Instrumentation

`host_driver.cpp` was instrumented via:

- `CUDA_CAMPAIGN_GROUP_DUMP=/tmp/tile_dump.txt`
- `CUDA_CAMPAIGN_GROUP_DUMP_ABORT=1`

On the first K4 group overflow, the driver dumps:

- tile coordinates and origin,
- candidate count,
- prime count,
- reported `max_label`,
- every prime `(a, b)`,
- norm squared,
- packed position / `(row, col)`,
- compressed UF parent,
- final wire group label.

Run command:

```bash
CUDA_CAMPAIGN_GROUP_DUMP=/tmp/tile_dump.txt \
CUDA_CAMPAIGN_GROUP_DUMP_ABORT=1 \
./campaign_main_cuda \
  --k-sq=36 \
  --r-inner=80000000 \
  --r-outer=80008192 \
  --region full-octant \
  --out /tmp/group_overflow_snapshot.bin \
  --chunk-size=1000
```

The campaign aborted as intended:

```text
Error in CUDA TileOp processing: captured K4 group overflow dump at /tmp/tile_dump.txt
EXIT:5
```

# Connectivity Check

I recomputed the full local graph from the dumped prime coordinates, adding
an edge for every pair with squared distance `<= 36`.

| Check | Result |
|---|---:|
| valid local edges recomputed from dump | 4,405 |
| host-recomputed connected components | 130 |
| distinct GPU parent roots | 130 |
| adjacent prime pairs with different GPU roots | 0 |
| host components split across GPU roots | 0 |
| GPU roots spanning multiple host components | 0 |

Conclusion: K4 UF is merging every neighbor edge present in the dumped prime
set. The component count of 130 is real for the current local graph.

# Label Compaction Check

The final labels are dense until the cap:

| Metric | Value |
|---|---:|
| nonzero final labels | 128 |
| zero-labeled primes | 2 |
| zero-labeled roots | 2 |
| reported `max_label` | 128 |

The two zero-labeled roots are not gaps in label compaction; they are the two
roots encountered after the 128-label cap was reached.

# TileOp Visibility

Classifying each UF root by whether it has a face-strip prime or a geo flag:

| Root class | Count |
|---|---:|
| total UF roots | 130 |
| roots with any face port | 54 |
| roots with inner/outer geo flag | 0 |
| TileOp-visible roots | 54 |
| internal, TileOp-invisible roots | 76 |

Face component counts:

| Face | Components |
|---|---:|
| I | 19 |
| O | 16 |
| L | 14 |
| R | 12 |

The current remap order is first appearance by prime index, across all local
components. That means internal components consume wire labels even though
they do not appear in `face_groups` and do not carry `inner_flags` or
`outer_flags`.

In this tile, both zero-labeled roots are actually visible:

| Root | Prime count | Visibility |
|---|---:|---|
| 2352 | 1 | `O`, `L` face strips |
| 2353 | 1 | `O` face strip |

They were not unlabeled because they are irrelevant; they were unlabeled
because 76 earlier irrelevant components had already consumed labels.

# Cause

The overflow predicate is too broad. It currently asks:

> Does the tile have more than 128 local UF components?

The TileOp budget really needs to ask:

> Does the tile need more than 128 wire-visible groups?

For TileOp v3, a group is wire-visible if it is referenced by at least one
face port or carries an inner/outer geo flag. A fully internal component with
no geo flag has no serialized representation and should not consume a group
label.

# Recommended Fix

Change K4/K5 dense labeling to assign wire labels only to roots that are
TileOp-visible:

1. Build local UF exactly as today.
2. Compute per-root `inner|outer` geo flags.
3. Compute whether each root appears in any face strip.
4. Dense-remap only roots with `geo_flags != 0 || has_face_port`.
5. Treat overflow as `visible_root_count > 128`.

This should turn the captured tile from overflow to normal: 54 visible groups
is below the 128-group budget.

The CPU reference path has the same broad remap behavior in
`tileop.cpp::dense_remap_roots`; it should be changed in lockstep or the CUDA
path will intentionally diverge from the current CPU byte oracle.
