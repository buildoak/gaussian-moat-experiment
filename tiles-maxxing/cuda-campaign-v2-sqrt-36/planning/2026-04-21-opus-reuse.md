---
date: 2026-04-21
engine: claude-opus-4-7
angle: reuse
status: complete
---

# CUDA port of cpp-campaign-v2 stages 5-9 — REUSE angle

## 1. Executive summary

Four of the six v1 CUDA files LIFT nearly verbatim (`kernel_sieve.cu`, `kernel_mr.cu`, `kernel_compact.cu`, `kernel_uf.cu`) because their algorithms are strictly geometric/number-theoretic against the halo-expanded bitmap — and v2 changed none of that geometry (S=256, C=6 at K=36, SIDE_EXP=269, halo semantics identical; cf. `tile_cuda_multi_kernel/include/gpu_constants.cuh:34-95` vs `cpp-campaign-v2/include/campaign/constants.h:44-114`). The per-tile output of these kernels (DSU `parent[]` over compacted primes) is wire-compatible with v2 if we keep the raw-root convention. The single biggest lever is therefore: **LIFT K1-K4 wholesale and rebuild K5 from scratch.** K5 (`kernel_face_encode.cu`) is a REWRITE — its consecutive-pair-clustering port identity (`kernel_face_encode.cu:256-258`, `start_port = (dx*dx+dy*dy) > K_SQ`) is the **v1 anti-pattern #1** (`v1-prior-art-study.md:110`), its bit-7-stolen L/R encoding (`kernel_face_encode.cu:82-88, 403-409`) violates v3's fixed-offset `n[4] / face_groups[192] / inner_flags[16] / outer_flags[16]` wire format (`cpp-campaign-v2/include/campaign/tileop.h:51-69`), and it emits no geo_I / geo_O flags at all. Stages 5-8 collapse into four kernels that reach `parent[]` parity with the CPU oracle on the same test tile; stage 9 is a new K5 that does `build_tileop_for_primes` from `cpp-campaign-v2/src/tileop.cpp:206-281` on-device. Host is a thin stage-10 wrapper around the existing `write_snapshot`.

**Verdict order (dependency-first, easiest byte-parity gate first):** K1 → K3 → K2 → K4 → K5. K1 is pure sieve-mask output; K3 is pure bitmap → prime-index list; both are independent of K_SQ math. K2 and K4 are LIFT but their outputs depend on the pinned witness table / backward-offset table — parity-gate after. K5 is the long pole; everything else must land first.

## 2. Per-stage plan for stages 5-9

### Stage 5 — per-tile sieve (halo-expanded bitmap of Gaussian-prime positions)

**CPU input/output:** `sieve_tile(coord, constants) -> std::vector<Prime>` (`cpp-campaign-v2/include/campaign/sieve.h:78`). Output is sorted by lex (a, b); `Prime { a, b, norm_sq, packed_pos }` where `packed_pos = row * SIDE_EXP + col`, `col = a - (a_lo - C)`, `row = b - (b_lo - C)` (`cpp-campaign-v2/include/campaign/sieve.h:47-62`). Implementation: layered candidate generation → split-prime residue marking → axis-aligned inert marking → `is_prime(norm)` on survivors (`cpp-campaign-v2/src/sieve.cpp:80-121`).

**Prior CUDA reuse verdict:** **LIFT (K1 Sieve + K2 MR together).** v1 splits this into K1 sieve-mask kernel (Barrett residue marking, emits `d_cand_list` of row×col packed uint32) then K2 Miller-Rabin kernel (verifies each candidate via FJ64 2-round MR, sets bits in `d_bitmap`). v2's CPU sieve does all four layers inline, but the output invariant (halo-expanded bitmap of Gaussian-prime positions) is bit-identical.

Justification: `kernel_sieve.cu:23-58` marks composites via `c_split_barrett` / `c_inert_barrett` residue classes using Barrett reduction; the tables are built on host from the first-10000 prime sieve (`main.cu:100-149`), identical to what a v2 CPU port would compute. `kernel_mr.cu:21-71` runs Sinclair/FJ64 MR on survivors — this is the same witness set v2 pins (`campaign_constants.h:28-29` pins the FJ64 SHA-256). v2's `is_prime(n)` is defined to use the same FJ64 table (per v1-prior-art study §B — amendment 2 pins this). Geometric constants match: v1 uses `COLLAR = ceil_isqrt(K_SQ)` (`gpu_constants.cuh:36`); v2 uses `C = floor_isqrt(K_SQ)` (`constants.h:104`) — but at K_SQ=36 both evaluate to 6. **Risk**: v1's `gpu_constants.cuh:36` labels the value `COLLAR` and derives it via `ceil_isqrt` while v2 uses `floor_isqrt`; at K=40 v1 gives 7 while v2 gives 6. Fix: at port-time, force `COLLAR_GPU = floor_isqrt(K_SQ)` inside the new `gpu_constants.cuh` and re-derive `SIDE_EXP = S + 1 + 2*C`, `NUM_BACKWARD_OFFSETS = count_backward_offsets(K_SQ)` — both functions are pure constexpr at `gpu_constants.cuh:15-32`. The sieve body reads `COLLAR` via the named constant, so a single rename fixes every TU.

**Axis-prime handling:** `kernel_mr.cu:56-61` routes `ca==0 || cb==0` candidates through `is_axis_gaussian_prime_gpu` (`gpu_math.cuh:224-234`): `|coord| ≡ 3 (mod 4) ∧ is_prime(|coord|)`. Matches v2 `src/sieve.cpp:97-99` exactly: `(q & 3ULL) != 3ULL || !is_prime(q)`. LIFT.

**Proposed CUDA impl:**
- `kernel_sieve` (K1): 288 threads/block, 1 block/tile. `--maxrregcount=40`. One thread per row of the 269-row bitmap (only first 269 threads active; remainder idle — v1 matches at `kernel_sieve.cu:138`). Per-thread local `uint32_t ws[9]` (BITMAP_WORDS_PER_ROW = `(269+31)/32 = 9`). Scatter survivors to `d_cand_list` via `atomicAdd` on shared counter (`:133-152`). Output: `d_cand_list[num_tiles * MAX_CANDIDATES_GPU]` + `d_total_cands[num_tiles]`.
- `kernel_mr` (K2): 288 threads/block, no register cap (v1 MR+44 got measured +2.2% — re-run nvcc tuning on v2). Each thread strides through candidate list; writes to `d_bitmap[num_tiles * BITMAP_WORDS]` via `atomicOr` (`kernel_mr.cu:14-17`). Reads FJ64 table from global memory (262144 * uint16 = 512 KB, L2-cached).

**Launch config:** `<<< num_tiles, 288 >>>`. All per-tile state is slice-indexed into flat global buffers — no per-tile shared memory needed for K1/K2.

**Memory layout:** Reuse v1's flat `TileBatchBuffers` exactly (`gpu_types.cuh:104-116`): `d_coords[N]`, `d_cand_list[N * 6144]`, `d_total_cands[N]`, `d_bitmap[N * BITMAP_WORDS]`. These are opaque intermediates — no parity concern.

**Byte-parity gate:** After K1+K2, download `d_bitmap` for one test tile. Compare against CPU-side halo bitmap built from `sieve_tile(coord, constants)` by setting `bitmap[p.packed_pos]` for each `Prime` in the result. Must be bit-identical. This is the stage-5 gate — it fully validates sieve correctness against v2 semantics without touching UF or encoding.

### Stage 6 — per-prime geo_I / geo_O flags

**CPU input/output:** Per `Prime`, compute `PrimeGeoFlags { bool inner; bool outer }` via `is_inner_prime(norm_sq, constants)` / `is_outer_prime(norm_sq, constants)` (`cpp-campaign-v2/src/process_tile.cpp:22-28`, `src/geo_tests.cpp:22-67`). Integer-only: `(norm_sq - R_inner_sq - K)^2 <= 4 * R_inner_sq * K` in `__int128`.

**Prior CUDA reuse verdict:** **REWRITE (NEW KERNEL — not present in v1).** v1 has no geo-flag kernel because v1's TileOp format had no `inner_flags[16] / outer_flags[16]`; it encoded "is-inner" / "is-outer" via implicit face-membership (`FACE_I / FACE_O`), which was semantically wrong (see `tile-operator-definition-v-claude.md:704`: a geo_I prime in a tile's interior still contributes to I_ports if its UF component reaches face-I, but is not itself on face-I). v2 makes flags first-class at a fixed TileOp offset, and they are indexed by the tile's DSU component label, not by face.

Justification for REWRITE rather than LIFT: v1 has no equivalent; the logic is inherently new. But the math is small and closed-form — 5 `__int128` multiplies per prime per face. Trivial kernel.

**Proposed CUDA impl:** Fold into K5 rather than spin up a dedicated kernel. After K4 produces `d_parent[]`, and before face-strip UF, one thread-per-prime sweep computes flags and ORs them into `d_inner_flags_bits[N * 16] / d_outer_flags_bits[N * 16]`:

```
for prime_idx in stride:
  norm = norm_sq_from_bitmap(d_prime_pos[idx], coord)  // reconstruct a,b,norm_sq
  label = remap[parent[prime_idx]]                      // 1..128 wire label
  if is_inner_gpu(norm): atomicOr(inner_flags[label>>3], 1<<(label&7))
  if is_outer_gpu(norm): atomicOr(outer_flags[label>>3], 1<<(label&7))
```

The constants `R_inner_sq`, `R_outer_sq`, `K_SQ`, `four_rin_sq_k_hi/lo`, `four_rout_sq_k_hi/lo` all come from `CampaignConstants` (`cpp-campaign-v2/include/campaign/campaign_constants.h:55-73`) and upload cleanly to `__constant__` memory — the struct is already `standard-layout` and designed for GPU upload (`campaign_constants.h:39, 155-156`). Reassemble i128 via `{hi, lo} -> __int128` on device with `__umul64hi` helpers already present in `gpu_math.cuh:56-65`.

**Launch config:** Done inside K5 body — no separate launch. Alternative if K5 register pressure forces a split: 288 threads, one block per tile, one thread per prime — launch_bounds(288, 4) for occupancy.

**Byte-parity gate:** After K5 emits `inner_flags[16]` and `outer_flags[16]` at TileOp offsets 196 and 212, compare against CPU golden bit-for-bit. At this point K1-K4 must already be landed so the upstream UF labels match.

### Stage 7 — local UF over tile primes + dense-remap

**CPU input/output:** `build_local_dsu(primes) -> DSU` where DSU unites pair (i, j) if `within_k_sq(primes[i], primes[j])` (`cpp-campaign-v2/src/tileop.cpp:159-170`). Within-K test is `(da² + db²) <= K_SQ` in `__int128` (`:46-50`). Deterministic tie-break: smaller-root-wins, `parent[rb] = ra` where `ra ≤ rb` (`cpp-campaign-v2/include/campaign/union_find.h:47-52`). Dense-remap: `dense_remap_roots(dsu, prime_count)` assigns 1-based wire labels in the order primes are visited (`src/tileop.cpp:172-204`). Overflow if `max_label >= MAX_GROUPS_PER_TILE` (128).

**Prior CUDA reuse verdict:** **LIFT (K3 Compact + K4 UF).** v1 has the identical decomposition: K3 converts the bitmap to a compacted list of prime positions + per-row prefix table (`kernel_compact.cu:35-108`), and K4 runs parallel union-find with lower-root-wins atomic CAS (`kernel_uf.cu:43-75`).

Justification: K4's `atomic_union` at `kernel_uf.cu:57-75` implements exactly v2's smaller-root tie-break: "if (rx > ry) { swap(rx, ry); } atomicCAS(&parent[ry], ry, rx)" — byte-identical semantics to CPU `DSU::unite` at `cpp-campaign-v2/src/union_find.cpp` (smaller-root-wins, path compression via `atomicCAS`-driven path splitting). The critical invariant — identical output for identical input irrespective of thread ordering — is proven to hold because the atomicCAS loop always converges to the root with the numerically smallest index.

The within-K neighbor search uses a precomputed `c_bk_dr[NUM_BACKWARD_OFFSETS]` / `c_bk_dc` table: all integer `(dr, dc)` with `dr < 0 || (dr == 0 && dc < 0)` and `dr² + dc² <= K_SQ` (`main.cu:157-178`). At K_SQ=36: 56 offsets. Each prime checks its 56 backward neighbors; if the neighbor bit is set in the bitmap, look up its compacted index via `gpu_uf_index` (`kernel_uf.cu:21-39` — Hamming-weight count of bits-before-me in the bitmap row-major). That predicate is geometry-only: **exactly** what `within_k_sq` in v2 tests (`cpp-campaign-v2/src/tileop.cpp:46-50`). **LIFT.**

K3's exclusive prefix scan (`kernel_compact.cu:13-31`) and scatter-via-`__ffs` (`:81-101`) is the direct GPU evolution of the CPU `compact.cpp` pattern catalogued in `v1-prior-art-study.md:66`.

**Key plumbing change:** v2 gives primes a `packed_pos` field. Verify it agrees with v1's compacted convention: v1 stores `packed = row * SIDE_EXP + col` in `d_prime_pos[]` (`kernel_compact.cu:94-95`). v2 defines the same: `packed_pos = row * SIDE_EXP + col`, `col = a - (a_lo - C)`, `row = b - (b_lo - C)` (`cpp-campaign-v2/include/campaign/sieve.h:52-62`). Identical. LIFT.

**Proposed CUDA impl:** Lift `kernel_compact` and `kernel_uf` verbatim. Only required surgeries:
- Rename TU-local `COLLAR` to match v2 `C` constant (no body change; constant name only).
- Ensure `NUM_BACKWARD_OFFSETS` is re-derived from v2's `floor_isqrt(K_SQ)` rather than v1's `ceil_isqrt` — at K=36 the count differs (v1 claims 56 via ceil_isqrt(36)=6 — matches since 36 is a square; at K=40 v1=64 via ceil_isqrt=7, v2=56 via floor_isqrt=6). For K=36 (our byte-parity target), the numbers coincide; for K=40 the port needs the re-derivation. Put a `static_assert` on the count per K value.
- Update v1's `MAX_PRIMES_GPU = 4096` (`gpu_constants.cuh:52`) to v2's `MAX_PRIMES_GPU = 6144` (`cpp-campaign-v2/include/campaign/constants.h:61`). This feeds `d_prime_pos[]` / `d_parent[]` sizing.

**Launch config:** K3 @ 288 threads/block, `--maxrregcount=32` (v1's setting). Dynamic shared `(ACTIVE_ROWS+1) * sizeof(uint16_t)` = 540 bytes. K4 @ 288 threads/block, `--maxrregcount=40`.

**Memory layout:** `d_row_prefix[N * 270]`, `d_prime_pos[N * 6144]`, `d_prime_count[N]`, `d_parent[N * 6144]` — all flat, slice-indexed per tile. No parity concern; opaque.

**Byte-parity gate:** After K4 produces `d_parent[]`, download parent array for test tile. CPU run: call `build_local_dsu(primes)` from `cpp-campaign-v2/src/tileop.cpp:159-170` and then for each prime call `dsu.find(i)` in order. Compare GPU `parent[]` after path compression with CPU `find()` result. Must be identical. This gate fully validates K3+K4; everything downstream consumes only this array.

### Stage 8 — face-strip UF over face primes + port ordinals

**CPU input/output:** Per face F ∈ {I, O, L, R}, extract primes lying in the face strip (`on_face_strip` returns true when `-C <= p_perp <= C` for face F, where `p_perp` is the row or column displacement from the face boundary — `cpp-campaign-v2/src/tileop.cpp:86-90`). Run a fresh DSU over that face-strip subset, union pairs within K (`src/tileop.cpp:105-114`). Each DSU component is one port. Port representative is min-lex `(h, p_perp)` (`:119-141`). Ports sorted by `(h, p_perp, global_wire_label)` (`:143-148`). Port's `global_wire_label` = `remap.wire_label_by_raw_root[local_dsu.find(prime_idx)]` (`:135-137`) — i.e. the tile-level UF label of any member.

**Prior CUDA reuse verdict:** **REWRITE.** v1's K5 does **not** implement face-strip UF. Its "port extraction" at `kernel_face_encode.cu:247-283` uses `start_port = (dx*dx+dy*dy) > K_SQ` — a consecutive-pair scan in h-sorted order that starts a new port whenever the Euclidean gap to the previous prime exceeds K. This is the v1 anti-pattern #1 cited in `v1-prior-art-study.md:110` and the root of git `cc71ab4` "ports being non deterministic in some cases" — because two face-adjacent tiles can disagree on whether a bridge prime belongs to one port or two, depending on whose halo the bridging prime sits in.

v2's spec (`tile-operator-definition-v-claude.md:142-149`) is unambiguous: **ports are connected components of `G_facestrip_f`**, not consecutive-pair clusters. The two algorithms produce identical output only on trivially-separated face strips; on any strip where two primes sit within K via an intermediate prime not on the strip, v1 gets it wrong and v2 is correct (this is exactly the bridge-prime failure mode).

The REWRITE is bounded: within a single face strip, the UF has at most `MAX_FACE_PRIMES_PER_FACE = 256` elements (per v1's sizing at `gpu_constants.cuh:55`) — tiny. Port-rep selection and sort are simple.

**Proposed CUDA impl:** New kernel `kernel_face_uf` OR folded into K5. Folded version is preferable: K5 already has the shared-memory layout for face prime lists (`kernel_face_encode.cu:464-473`). Replace the consecutive-pair loop with a mini DSU:

```
// in shared memory, one sub-DSU per face, capacity 256
// allocate: uint16_t face_parent[4 * 256] + uint8_t in_strip[4 * 256]
//
// step 1: for each prime i in d_prime_pos[], for each face f,
//         if on_face_strip(i, f): append (face_prime_idx_f, i) to face_prime_list_f
//
// step 2: for each face f in parallel (4 warps, one per face):
//         for each pair (face_prime_list_f[a], face_prime_list_f[b]) with a<b:
//           if within_k_sq(primes[a], primes[b]):
//             atomic_union(face_parent_f, a, b)
//
// step 3: collect components: for each a in face_prime_list_f,
//         root = find(face_parent_f, a); record (root, h[a], p_perp[a])
//
// step 4: per-root, pick min-lex (h, p_perp) as port rep;
//         label = remap[local_parent[prime_idx_a]] (wire label 1..128)
//
// step 5: sort ports by (h, p_perp, label) lex
```

For the face-strip within-K test, reuse `kernel_uf.cu`'s `atomic_union` at `:57-75` — identical primitive. The O(n²) scan at n≤256 is fine; 4 faces × 256² = 262K pair-tests per tile, bounded by K_SQ geometry (most pairs fail early on row-diff alone).

**Alternative**: Not O(n²). For within-K on face-strip, each face-prime has only its backward-K offsets to check, same as K4. The strip is 1D-like (perp bounded by 2C+1=13 lattice positions at K=36), so per face-prime check the C_backward offsets × 13-perp = O(56 * 13) = at most 728 candidates per prime, but we can also reuse the bitmap + `gpu_uf_index` machinery from K4 directly. This makes it effectively a copy of K4 gated to face-strip primes only.

**Launch config:** Fold into K5. `<<< num_tiles, 288 >>>`. Shared memory expanded: existing `FacePrimeGPU[4 * 256]` (8 KB), new `uint16_t face_parent[4 * 256]` (2 KB), `uint16_t face_port_labels[4 * 48]` (0.4 KB). Total per-block shared ~12 KB — within 4090's 48 KB default / 100 KB optin.

**Byte-parity gate:** After K5 writes the 192-byte `face_groups[192]` region of each TileOp, compare against CPU golden. Specifically compare just `n[0..3]` (port counts) and `face_groups[0..sum(n)-1]` (the wire labels). This isolates the face-UF + port-sort logic without depending on flags.

### Stage 9 — TileOp 256-byte pack (host-visible byte-parity output)

**CPU input/output:** `build_tileop_for_primes(primes, prime_flags, coord, constants) -> TileOp` (`cpp-campaign-v2/src/tileop.cpp:206-281`). Produces a 256-byte struct:
- `n[4]` at offset 0
- `face_groups[192]` at offset 4 (concatenated I, O, L, R port labels — note v2's Face enum order I=0, O=1, L=2, R=3)
- `inner_flags[16]` at offset 196 (bit `(label-1)` set if any prime in the UF component labeled `label` is `is_inner_prime`)
- `outer_flags[16]` at offset 212
- `tile_flags` at offset 228 (OVERFLOW_BIT=0x01 / EMPTY_BIT=0x02)
- `reserved[27]` at offset 229, zero

Overflow conditions: `remap.overflow` OR `ports.size() > 255` OR `total_ports > MAX_PORTS_PER_TILE` (192). On overflow: emit a TileOp with `tile_flags = OVERFLOW_BIT` and **all other fields zero** (`src/tileop.cpp:151-155, 238-240, 260-262, 268-270`). Not 0xFF-poisoned like v1; zero except the flag.

**Prior CUDA reuse verdict:** **REWRITE.** v1's `encode_tileop_gpu_k5` at `kernel_face_encode.cu:393-442` emits a completely different wire format:
- v1 header: `bytes[0..2]` = variable offsets `off_I / off_L / off_R` (`:425-427`)
- v1 L/R encoding: bit-7 of each group byte stolen for MSB of `h1` (`:82-88, 401-409`). 7-bit group ID limit.
- v1 h1 trailer: L-h1 then R-h1 appended at end (`:439-441`)
- v1 overflow sentinel: all 256 bytes `0xFF` (`:160-165, 405-408`)
- v1 has no inner_flags / outer_flags anywhere — the bytes at v2's offsets 196-227 do not exist in the v1 layout

Every one of these is a v1 anti-pattern listed in `v1-prior-art-study.md:110-119` (items 1-10). v3 TileOp is a wholesale redesign; zero bytes of v1's encode logic are salvageable.

Justification: the static asserts at `cpp-campaign-v2/include/campaign/tileop.h:60-69` lock every field offset. Any deviation is caught at compile time. The CPU reference is 75 lines of code (`src/tileop.cpp:206-281`) — porting it to a single-thread-per-tile device function inside K5 is straightforward, and the output is canonically byte-identical.

**Proposed CUDA impl:** Single-thread-per-tile `encode_tileop_v2` at the tail of K5. Read from shared memory (face ports, flags, remap.overflow), write to `d_output[tile_idx].bytes[256]`. Pseudocode:

```
if tid == 0:
    TileOp out; memset(&out, 0, 256);
    if remap.overflow || total_ports > 192 || any_face_ports > 255:
        out.tile_flags = 0x01;  // OVERFLOW_BIT, rest zero
    else if primes.empty():
        out.tile_flags = 0x02;  // EMPTY_BIT
    else:
        out.n[0..3] = face_port_counts in (I, O, L, R) order
        cursor = 0
        for f in (I, O, L, R):
            for port in ports_by_face[f]:
                out.face_groups[cursor++] = port.global_wire_label  // 1..128, 1-based
        // inner_flags / outer_flags populated by the flags pass (stage 6)
    d_output[tile_idx] = out
```

Keep the flags pass (stage 6) and encode pass coalesced — same `tid==0` serial section, or fan out flag OR-ing to warp with atomicOr on shared before final write.

**Launch config:** Same K5 launch — fold in. `<<< num_tiles, 288 >>>`, block-serializing at tid=0 for the encode. Shared memory unchanged.

**Memory layout:** `d_output[N]` of `TileOp` (256 bytes each). Flat array, index = tile flat-index. Direct DtoH memcpy to host `std::vector<TileOp>` for snapshot writer.

**Byte-parity gate:** `memcmp(gpu_tileop, cpu_tileop, 256) == 0` for every active tile in a full K=36 R=80M run. Snapshot-level: SHA-256 of the concatenated TileOp payload must match the golden committed in `cpp-campaign-v2/goldens/`. If we agree on every byte we agree on the snapshot hash. **This is the terminal success criterion.**

## 3. Repo layout

```
cuda-campaign-v2-sqrt-36/
├── CMakeLists.txt                  # adds cuda subdir, reuses cpp-campaign-v2/src host code
├── include/
│   ├── gpu_constants.cuh           # fork of v1, updated for v2 floor_isqrt + 6144 primes
│   ├── gpu_math.cuh                # LIFT verbatim (Barrett, Montgomery MR, axis-prime)
│   ├── gpu_types.cuh               # trimmed: TileOp struct matches cpp-campaign-v2 wire
│   └── fj64_262k_table.h           # LIFT verbatim (witness table, SHA-256 pinned)
├── src/
│   ├── kernel_sieve.cu             # LIFT from v1 with COLLAR rename
│   ├── kernel_mr.cu                # LIFT verbatim
│   ├── kernel_compact.cu           # LIFT verbatim
│   ├── kernel_uf.cu                # LIFT, update MAX_PRIMES_GPU = 6144
│   ├── kernel_encode_v2.cu         # NEW: face-strip UF + flags + 256 B encode
│   └── main.cu                     # NEW: host orchestration (OR fold into campaign_main_cuda.cpp)
├── apps/
│   └── campaign_main_cuda.cpp      # fork of cpp-campaign-v2/apps/campaign_main.cpp, swaps process_tile() for batched GPU dispatch
├── tests/
│   ├── test_stage5_bitmap_parity.cpp        # gate after K1+K2
│   ├── test_stage7_parent_parity.cpp        # gate after K3+K4
│   ├── test_stage8_face_groups_parity.cpp   # gate after K5 face-groups
│   └── test_stage9_full_tileop_parity.cpp   # terminal gate: memcmp full 256 B
└── planning/
    └── 2026-04-21-opus-reuse.md
```

**CMake strategy:** Two-layer. The new repo's `CMakeLists.txt` does `add_subdirectory(../cpp-campaign-v2 ... EXCLUDE_FROM_ALL)` and `target_link_libraries(campaign_cuda PRIVATE campaign)` — where `campaign` is the CPU `libcampaign.a` built from `cpp-campaign-v2/src/*.cpp`. That way the snapshot writer (`src/snapshot.cpp`), the grid (`src/grid.cpp`), the region parser (`src/region.cpp`), `CampaignConstants::from_radii`, the compositor (`src/compositor.cpp`), and SHA-256 (`src/sha256.cpp`) all come for free with their tests. The new binary only adds CUDA TUs and a thin `apps/campaign_main_cuda.cpp` that calls the GPU dispatch instead of the OMP-parallel `process_tile` loop at `cpp-campaign-v2/apps/campaign_main.cpp:427-432`. No re-implementation of host-side byte-layout code.

**Build knobs:** `-DK_SQ=36` or `-DK_SQ=40` propagates to both CPU and CUDA TUs; v1 Makefile pattern `OBJ_DIR := build-k$(K_SQ)` (cf. `v1-prior-art-study.md:73`) ports to `CMAKE_BINARY_DIR=build-k36`. Per-kernel `-maxrregcount` via `set_source_files_properties(kernel_X.cu PROPERTIES COMPILE_FLAGS "--maxrregcount=40")`.

## 4. Host integration

**Entry point:** `apps/campaign_main_cuda.cpp`. Mirrors `cpp-campaign-v2/apps/campaign_main.cpp:250-494` with one substitution — replace the tile-processing loop at `:426-432`:

```cpp
// CPU version (cpp-campaign-v2)
#pragma omp parallel for schedule(dynamic, 64)
for (std::int64_t k = 0; k < active_tiles.size(); ++k) {
  tileops[k] = campaign::process_tile(active_tiles[k], constants, grid);
}

// CUDA version — swap with chunked batch dispatch
GpuPipeline gpu;
gpu.init(constants, k_sq_u32);
gpu.upload_tables();  // FJ64 + backward_offsets + Barrett
constexpr int CHUNK_SIZE = 20000;  // match v1's main.cu:419
for (chunk_start in [0, active_tiles.size()) step CHUNK_SIZE) {
  gpu.dispatch_chunk(&active_tiles[chunk_start], chunk_tiles,
                     &tileops[chunk_start]);
}
```

**Data movement:**
- One-time upload per run: FJ64 table (512 KB), Barrett split/inert tables (~10 KB), backward-offset LUT (~112 B), `CampaignConstants` to `__constant__`.
- Per-chunk H→D: `TileCoord[CHUNK_SIZE]` = 320 KB at chunk=20000.
- Per-chunk D→H: `TileOp[CHUNK_SIZE]` = 5 MB at chunk=20000. Also `prime_counts` for diagnostics.
- No persistent DtoH for intermediate bitmaps/parents.

**Streams:** Single stream is sufficient at this throughput (v1 hit 155k tiles/s with one stream). Adding a second stream for H→D overlap with compute is a micro-optimization; defer to milestone M7. The v1 `launch_pipeline` at `tile_cuda_multi_kernel/src/main.cu:316-389` is exactly this single-stream pattern — LIFT.

**Compositor:** Unchanged. Sequential DSU on host consumes `tileops[]` column-by-column via `compositor.ingest_column(i, tileops.data() + offset)` (`cpp-campaign-v2/apps/campaign_main.cpp:439-448`). Reuses the v2 compositor exactly.

**Snapshot write:** Unchanged. `campaign::write_snapshot(path, grid, tileops, constants)` (`cpp-campaign-v2/apps/campaign_main.cpp:458`). Emits the 120-byte header with `grid_params_hash / constants_hash / mr_witness_set_sha256` then `total_tiles * 256` payload bytes.

## 5. Milestone sequence

Each milestone has a concrete byte-parity gate. Wall-clock estimates assume single-coder pace with Codex worker support.

**M0 — Scaffolding + repo skeleton. 0.5 day.**
Goal: `cuda-campaign-v2-sqrt-36/` builds, links `libcampaign.a`, runs a no-op `main` that calls `CampaignConstants::from_radii` and `Grid::build`.
Gate: `campaign_main_cuda --help` prints, no CUDA kernels launched yet.
Prior kernel: none.

**M1 — LIFT K1+K2 (sieve + MR), bitmap parity. 1 day.**
Goal: Copy `kernel_sieve.cu` and `kernel_mr.cu` into new repo, rename COLLAR macro usage, update `gpu_constants.cuh` to derive from v2 `K_SQ=36`. Run on 16 test tiles from canonical octant. Download bitmap.
Gate: For each of 16 tiles, reconstruct CPU halo bitmap from `sieve_tile(coord, constants)` and `memcmp` against GPU bitmap. All 16 must match bit-for-bit.
Prior kernel: `kernel_sieve.cu` (LIFT), `kernel_mr.cu` (LIFT), `gpu_math.cuh` (LIFT).

**M2 — LIFT K3+K4 (compact + UF), parent-array parity. 1 day.**
Goal: Lift remaining two kernels. Update `MAX_PRIMES_GPU = 6144`. Download `d_parent[]` for 16 test tiles.
Gate: CPU runs `build_local_dsu(sieve_tile(coord))` + `dsu.find(i)` for each prime. GPU `parent[i] == cpu_find(i)` after K4's final path-compression pass. All 16 tiles must match.
Prior kernel: `kernel_compact.cu` (LIFT), `kernel_uf.cu` (LIFT).

**M3 — REWRITE encode kernel, empty-tile parity. 0.5 day.**
Goal: Stub K5: single thread writes `tile_flags = EMPTY_BIT` if no primes, else `tile_flags = OVERFLOW_BIT` (placeholder). Zero everything else. Download TileOps.
Gate: For tiles where `sieve_tile(coord).empty()`: GPU TileOp matches CPU TileOp (256 bytes of zero + 0x02 at offset 228). Sanity check that the host↔device marshaling of the 256 B struct is correct.
Prior kernel: none (rewrite).

**M4 — REWRITE encode: dense-remap + face_groups byte-parity. 1.5 days.**
Goal: Implement dense_remap in shared memory. Implement face-strip UF per face. Implement port-rep selection (min-lex h, p⊥) and port sort ((h, p⊥, label)). Emit `n[4]` and `face_groups[192]`.
Gate: For every tile in a 1024-tile batch, `memcmp(gpu.n, cpu.n, 4) == 0` AND `memcmp(gpu.face_groups, cpu.face_groups, sum(cpu.n)) == 0`. No overflow tiles yet — pick a low-density test region.
Prior kernel: none (rewrite). Can reuse `atomic_union` primitive from `kernel_uf.cu:57-75`.

**M5 — REWRITE encode: inner/outer flags byte-parity. 0.5 day.**
Goal: Implement stage 6 (geo flags) inside K5. Fold into the prime-iteration pass. Emit `inner_flags[16]`, `outer_flags[16]`.
Gate: For every tile in 1024-tile batch, `memcmp(gpu.inner_flags, cpu.inner_flags, 16) == 0` AND `memcmp(gpu.outer_flags, cpu.outer_flags, 16) == 0`.
Prior kernel: none.

**M6 — REWRITE encode: overflow + full 256 B parity. 1 day.**
Goal: Implement overflow detection (primes > MAX_PRIMES_GPU, max_label >= 128, any n[f] > 255, sum(n) > 192). Emit zero-payload-plus-flag correctly. Handle EMPTY.
Gate: Full K=36 R=80M run. `sha256sum snapshot.bin` from CUDA build equals `sha256sum snapshot.bin` from CPU build. Any mismatch = failure. This is the terminal gate.
Prior kernel: none.

**M7 — Perf: reach 155k tiles/s on 4090. 1-2 days.**
Goal: Profile with Nsight Compute. Tune per-kernel `--maxrregcount`, occupancy, shared-memory banks. Add second CUDA stream for H↔D overlap if throughput < 150k t/s.
Gate: `tiles/sec >= 155000` on bench mode, 4090. Snapshot bytes still match.
Prior kernel: consult `tile_cuda_multi_kernel/Makefile:17-22` reg caps + v1-prior-art `dd56e2e` MR +44 finding.

**Total: ~5-6 engineering days.** K=36 byte-parity at M6. K=40 adds one day for `NUM_BACKWARD_OFFSETS` re-derivation and corresponding tests.

## 6. Risks specific to REUSE angle

**R1 — v1 COLLAR semantics drift.** v1's `gpu_constants.cuh:36` defines `COLLAR = ceil_isqrt(K_SQ)`. v2's `constants.h:104` defines `C = floor_isqrt(K_SQ)`. At K_SQ=36 both equal 6 (36 is a square so ceil==floor). At K_SQ=40, ceil_isqrt=7 and floor_isqrt=6 — v1's `NUM_BACKWARD_OFFSETS=64` vs v2-correct 56. If we LIFT K4 wholesale without re-deriving the backward-offset table host-side using v2's constant, we silently over-union primes at K=40. **Mitigation:** At M1, add `static_assert(COLLAR_GPU == C)` in `gpu_constants.cuh` that fails the build if v1's ceil-derived constant diverges from v2. Add a runtime check that host-built `c_bk_dr / c_bk_dc` arrays have exactly `NUM_BACKWARD_OFFSETS` entries matching v2's `floor_isqrt(K)` geometry.

**R2 — MR witness table drift.** v1 CPU uses 12-base MR; v1 CUDA uses FJ64 (`v1-prior-art-study.md:119` forbidden-list item 10). If we LIFT K2 without verifying the FJ64 table's SHA-256 matches `campaign_constants.h:28-29` (`92b8b0ea7ae8703a3fae4f7a1581dd0d04e041bde4eb1d23621a8f39846e909c`), the snapshot's `mr_witness_set_sha256` field silently lies. **Mitigation:** At M1, compute sha256 of the FJ64 table lifted from `tile_cuda_multi_kernel/include/fj64_262k_table.h` and assert it equals the pinned constant. If it doesn't, v2's CPU side needs a rebuild from the same table or we regenerate CPU goldens — do not proceed past M1 until hashes match.

**R3 — v1 d_parent retains raw roots vs v2 remap expectations.** v1's K5 consumes `d_parent[]` but never applies a dense-remap — it feeds raw roots into the find_group_entry hash probe at `kernel_face_encode.cu:121-131`. v2's TileOp encoder needs dense 1..128 labels, visit-order-deterministic (`cpp-campaign-v2/src/tileop.cpp:181-204`). **Mitigation:** The new K5 builds its own remap table in shared memory on the GPU — do not attempt to piggyback on v1's hash-probe scheme. Visit order must match CPU: primes are already sorted by `(a, b)` before `build_local_dsu` (cf. `tileop.cpp:220-227`), so iterate `d_prime_pos[]` in index order (which is already lex-sorted by K3's row-major scan × within-row left-to-right bit order) and assign labels 1, 2, 3… first-seen-wins.

**R4 — v1's `MAX_FACE_PRIMES_PER_FACE=256` might clip under v2's `MAX_PRIMES_GPU=6144`.** v1 sized the face list for 4096 total primes/tile. At 6144 primes/tile, some extreme tiles may have >256 face-strip primes on a single face. This would silently drop primes if we lift the cap unchanged. **Mitigation:** Audit: in the halo of width 2C+1=13 along a face of length S+1+2C=269, maximum lattice points = 13 × 269 = 3497; Gaussian prime density ~ 1 / ln(R²) ≈ 1/42 at R=80M gives ~83 expected face-primes. 256 is 3× overhead, likely safe. Add overflow detection: if a single face exceeds 256 primes, poison tile with OVERFLOW_BIT. Verify on worst-case high-density test tiles early.

**R5 — K5 REWRITE scope creep.** Stages 6, 8, 9 all fold into K5. Shared memory grows; register pressure grows; the kernel's tid==0 serial section grows. If K5 regresses below 100k tiles/s, the REUSE thesis weakens (K1-K4 were 65% of v1 runtime per `main.cu:980-990` timing prints, so K5 doubling would drop throughput from 155k to ~90k). **Mitigation:** Structure K5 as three `__syncthreads()`-separated passes: (1) flag compute, (2) face-strip UF (warp-per-face), (3) encode (tid=0 serial). If the serial section bottlenecks, split into K5a/K5b kernels and let the driver schedule overlap. The milestone M7 check catches this: don't commit K5 until 155k is demonstrated.

**R6 — v1 `gpu_types.cuh`'s TileOp is `uint8_t bytes[256]`, v2's is a structured POD.** If we LIFT v1's TileOp struct (`gpu_types.cuh:10-12`) and host unpacks to v2's layout, there's an opportunity for silent field-offset drift. **Mitigation:** Replace `struct TileOp { uint8_t bytes[256]; }` in the new `gpu_types.cuh` with a verbatim copy of v2's TileOp struct from `cpp-campaign-v2/include/campaign/tileop.h:51-58`, including all offsetof static_asserts. NVCC accepts the struct; `sizeof == 256` is locked at compile on both sides.

**R7 — v1 TileCoord struct has only `a_lo, b_lo`; v2 has `i, j, a_lo, b_lo`.** Minor — `v1 TileCoord` (`gpu_types.cuh:5-8`) matches v2 `TileCoord` except v2 has extra `i, j` fields unused on GPU. **Mitigation:** Use v2's struct or a GPU-only variant with just `a_lo, b_lo`. Host marshalls, never sends `i, j` to device. Not a parity risk.

## 7. Open questions for the coordinator

1. **K=40 scope.** The context says K=36 is the byte-parity target. Is K=40 in scope for this port (second golden anchor per context) or do we ship K=36 first and handle K=40 as a follow-up? This affects M1-M6 by requiring the `NUM_BACKWARD_OFFSETS` re-derivation from v2's `floor_isqrt(K)` geometry (R1 above). Recommendation: ship K=36 at M6, add K=40 as M6.5 (one engineering day).

2. **Sparse-region support.** `cpp-campaign-v2/include/campaign/grid.h:97` carries `explicit_tiles` for sparse / explicit-tile-list regions. Does the CUDA port need to handle sparse regions, or can we assume full-octant batches for the byte-parity golden? This affects chunking strategy — sparse tile lists may be non-contiguous and have lower GPU utilization per batch. Recommendation: treat sparse regions as unchunked (dispatch all tiles in one launch). At R=80M the full octant is 221K towers per v1-prior-art; sparse sub-runs are much smaller.

3. **Device support.** 4090 is the perf target. Do we need compatibility with Hopper (H100) or Ampere (A100) given vast.ai provisioning? v1 was tuned for sm_89. If the port must run on sm_80 / sm_90, the `cudaFuncAttributeMaxDynamicSharedMemorySize` limits differ and the FJ64 table's 512 KB L2 residency may change behavior. Recommendation: target sm_89 primarily; add a `-DCUDA_ARCH=89` CMake knob with 80/90 fallbacks. No code changes needed, only `-gencode` flags.

4. **Golden regeneration policy.** If the CPU byte-parity check at M6 reveals a CPU bug — say, an edge case in `build_tileop_for_primes` the 90/90 tests didn't cover — do we patch the CPU reference and regenerate goldens, or patch the GPU to match the CPU's exact bytes? `cpp-campaign-v2` is described as "trusted, byte-verified"; ambiguity only matters if the port uncovers a new edge case. Recommendation: any GPU↔CPU mismatch found at M6 is a CPU-investigation-first rule, escalate to coordinator before patching either side.

5. **Per-kernel register-cap profiling.** v1's Makefile has hand-tuned `--maxrregcount` per kernel (v1-prior-art-study.md:74). Do we accept those caps verbatim (LIFT), or re-profile on v2 with Nsight at M7? Recommendation: LIFT for M1-M6 (v2 kernel bodies differ only in constant renames; register pressure unchanged). Re-profile only at M7 if < 155k t/s.

6. **Async dispatch vs blocking.** v1 has three host modes: `test`, `campaign` (file I/O), `stream` (persistent pipe protocol). For the v2 port we want `campaign` mode integrated into `campaign_main_cuda.cpp` as a library call rather than a subprocess. Is that the right call, or do we want to preserve v1's subprocess pattern for some reason? Recommendation: inline library dispatch — eliminates file-I/O serialization at 5 MB/chunk DtoH.

---

*End of REUSE plan.*
