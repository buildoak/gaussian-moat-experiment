---
date: 2026-04-21
engine: claude-opus-4-7
angle: correctness
status: complete
---

# CUDA Port Plan — Stages 5-9 — Correctness / Math Faithfulness

## 1. Executive summary

**Thesis.** Byte parity is a deterministic-order problem, not a math problem. The math is already settled — Theorem 11, the 256 B layout, the canonical port sort, and the integer geo tests are all proven at the spec level and anchored in `cpp-campaign-v2` with `static_assert(sizeof(TileOp) == 256)` + `static_assert(offsetof(...) == N)` + byte-golden regression. The CUDA port's entire correctness surface reduces to: **reproduce the CPU's total ordering of every operation that assigns a label or a position, at every stage.** If the order is wrong, the bytes are wrong. If the order is right, the bytes are right — the math takes care of itself.

**Stages 5-9 correctness hierarchy (load-bearing, in order):**

1. **Prime master order** (Stage 5 sieve): `std::sort` by `(a, b, norm_sq, packed_pos)` (`cpp-campaign-v2/src/tileop.cpp:39-44`, applied at `:220-223`). Every subsequent per-prime index — UF parent slot, dense label assignment order, face-strip index, bit position — is a function of this index. Get this wrong and every downstream byte drifts.
2. **DSU construction order** (Stage 7): serial `for i=0..N; for j=i+1..N` pair scan (`tileop.cpp:161-168`). Smaller-root-wins makes the final root *set* order-invariant, but the `find(i)` value for each `i` depends on the exact parent-array mutation sequence — and the dense-label assignment loop then reads `find(i)` in ascending-`i` order (`tileop.cpp:172-179`), so any per-element `find()` divergence poisons the labels.
3. **Dense-label assignment order** (Stage 7/8 seam): loop `for i=0..N; take find(i); if first-seen → label := max_label+1; ++max_label` (`tileop.cpp:187-201`). The OUTPUT labels on the wire are a permutation of `{1..max_label}` defined purely by "first prime index (in sort order) that resolves to this root". Any GPU scheme that assigns labels by root-ID instead of first-appearance-index produces a different permutation — and every `face_groups` byte plus every `inner_flags` / `outer_flags` bit shifts.
4. **Face-strip UF + canonical port sort** (Stage 8): per-face sub-DSU over face-strip primes in ascending `face_indices` order (`tileop.cpp:92-114`), then `std::sort` by `(h, p_perp, global_wire_label)` (`tileop.cpp:143-147`). The third tiebreak is non-obvious and non-negotiable.
5. **Overflow semantics** (Stage 8): `OVERFLOW_BIT` forces `face_groups`, `inner_flags`, `outer_flags`, `reserved[27]` all zero (`tileop.cpp:151-155`). Any partial fill before overflow is detected = byte mismatch.

**Single highest-value correctness risk** (a v1-vs-v2 gap the prior CUDA couldn't have caught): **the dense-remap label permutation.** v1 CUDA encoded face-group labels directly from raw UF root IDs (`campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_face_encode.cu`, no compaction pass). v2 mandates dense labels `1..max_label` assigned in *ascending-prime-index-in-sort-order first-appearance order* (cpp-campaign-v2 `dense_remap_raw_roots_for_test` at `tileop.cpp:181-204`). On GPU, a naive approach — warp-scan compact the root-ID set, then index-map — will produce labels ordered by root-ID, not by first-appearance. The v1 golden NEVER exercised this because v1 had no compaction. The v2 golden exercises it on every tile with ≥2 components. **Every single tile in the K=36 R=80M golden with multiple UF components will have byte-different `face_groups` if the dense-remap order is wrong, and the `inner_flags`/`outer_flags` bit assignments will be permuted.** See §2.5 for the enforcement mechanism.

## 2. Per-stage plan for stages 5-9

Stage numbering per the user's prompt: 5=sieve, 6=geo-flags, 7=local UF + dense remap, 8=face-strip UF + port ordinal encoding, 9=TileOp 256-byte pack. Stages 7 and 8 share the dense-remap seam and are tightly coupled.

### 2.1 Stage 5 — Per-tile Gaussian prime sieve

**Determinism invariants.**

1. **MR witness set is a wire-pinned constant.** Host-side: FJ64 hash table (`cpp-campaign-v2/include/campaign/fj64_table.h`), base-2 first round then indexed witness (`primality.cpp:69-75, :107-110`). The witness set's SHA-256 is serialized into every snapshot header (`snapshot.cpp:171, :174, :215`). **Enforcement:** upload the identical `fj64_table.h` as a `__constant__` array; `static_assert(sizeof(kFj64Table) == 262144 * sizeof(uint16_t))` on both sides; CUDA reads `kFj64Table[hash(n) & 0x3FFFF]` with the exact same mixer (`primality.cpp:70-74`: `h = ((h >> 32) ^ h) * 0x45d9f3b3335b369ULL; h = ((h >> 32) ^ h) * 0x3335b36945d9f3bULL; h = ((h >> 32) ^ h)`). Any single literal drift in the multiplication constants = witness divergence = prime-decision divergence.
2. **128-bit modular multiply semantics.** CPU uses `(__int128)a * b % m` at `primality.cpp:16-20`. CUDA has no native `__int128`. **Enforcement:** implement `mul_mod_u64` via `__umul64hi` + Barrett reduction OR via Montgomery form (pre-computed per-`n` reciprocal). The v1 CUDA `gpu_math.cuh:56-65` already does this, but v1 and v2 CPU used *different witness sets* (v1 12-base vs v2 FJ64 — `v1-prior-art-study.md:30-33`), so v1's `mul_mod` was never exercised on v2's witnesses. **The v2 CUDA `mul_mod` must be unit-tested against CPU `(__int128)a*b%m` on the full candidate band for `R_inner² - K` through `R_outer² + K`** — not a sample, the full band, because a single miscarried bit at one witness value breaks one prime decision which cascades through everything downstream.
3. **Prime candidate enumeration order.** CPU `sieve_tile` iterates `for a in max(0, a_begin)..a_end; for b in max(a, b_begin, 0)..b_end` (`sieve.cpp:94-96`) then `std::sort` by lex `(a, b)` at `:116-119`. The output vector order is lex `(a, b)` which matches the sort. **Enforcement:** CUDA kernel produces a `packed_pos`-sorted output, then host-side (or a separate kernel) re-sorts to lex `(a, b)`. Alternatively, emit directly in lex order via a compact pass over a row-major bitmap. The output contract is `std::vector<Prime>` sorted by `(a, b)` — this must be byte-identical before any downstream stage runs.
4. **Annulus filter uses `uint64_t` norm_sq from `checked_norm_sq` with i128 intermediate.** `sieve.cpp:46-53` rejects overflow. **Enforcement:** CUDA computes `norm_sq = a*a + b*b` in `uint64_t` with `__int128` (emulated via `uint2`) overflow check; reject if `> UINT64_MAX`. At project scale this never triggers, but the overflow predicate is part of the contract.
5. **Axis-prime path.** `a == 0` branch (`sieve.cpp:97-99`) emits `(0, q)` iff `q ≡ 3 (mod 4) && is_prime(q)` AND `q² ∈ [R_inner², R_outer²]` (via `norm_in_annulus` at `:109-110`). **Enforcement:** same branch predicate, same order (axis-check before split-prime norm check). Compile-time guard `static_assert(OFFSET_X <= C)` (`sieve.cpp:19-23`) must be mirrored in CUDA.

**CUDA enforcement mechanism.**

- Reuse v1 K1 (`kernel_sieve.cu`) byte-identical — it's already parameterized by `MR_WITNESS` / Barrett tables and produced primes in the same order as v2 CPU. Prior art study `v1-prior-art-study.md:17-21` flags its integer discipline as byte-portable.
- Replace v1's 12-base MR (used by v1 CPU, not its CUDA) with v2's FJ64 hash-lookup — the K2 kernel in v1 CUDA already uses FJ64 so this is *no change* in the CUDA path, but the v1/v2 CPU divergence flags a latent risk: **if anyone re-introduces the 12-base fallback for "robustness", CUDA diverges silently.** Enforcement: hard-remove any `k12Base` array from the v2 CUDA port source tree; grep for `{2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37}` at CI.
- Add a kernel-launched unit test `k1k2_parity_test`: for every prime candidate `n ∈ [R_inner²−K, R_outer²+K]` at some small R, emit `{n, is_prime(n)_gpu, is_prime(n)_cpu}` triples; assert identical.

**Byte-parity test.** Does not directly touch TileOp bytes; it's a prerequisite. Failure here corrupts every downstream byte starting at `face_groups[0]`. Test: snapshot `Prime[]` (pre-sieve-sort) as a secondary debug artifact, SHA-256 the concatenated `{a:i64, b:i64, norm_sq:u64, packed_pos:u32}` records per tile, compare to CPU. This debug snapshot is emitted only under `-DCAMPAIGN_DEBUG_DUMP_PRIMES` to avoid perturbing the primary snapshot byte-identity.

### 2.2 Stage 6 — Geo-flags (is_inner / is_outer per prime)

**Determinism invariants.**

1. **Norm-form test uses `__int128`.** CPU `geo_tests.cpp:35-43` computes `eps = norm - r_sq - k` in `__int128`, then `eps*eps <= four_r_sq_k` in `__int128`. **Enforcement:** CUDA emulates i128 via `uint2`/`ulonglong2`. Critical: signed arithmetic. `eps` can be negative — emulation must handle two's-complement i128 multiply correctly. The v1 CUDA at `campaign-sqrt-36/tile_cuda_multi_kernel` did NOT have geo tests (this is the v2 K4 addition per blueprint §6.2), so this is a net-new CUDA kernel with no prior-art fallback.
2. **Short-circuit order.** CPU `is_inner_prime` returns `false` immediately if `norm < r_sq` (`:37-39`), before i128 multiply. This is a correctness guard — without it, a prime just below the inner radius would test `(norm - R_inner² - K)² <= 4R_inner²K` which *could* evaluate true on the negative-`eps` side and spuriously flag `inner`. **Enforcement:** same early-out, same order. The v1 analogue in `kernel_uf.cu` does not exist (v2 addition); new code must implement and test with a synthetic prime at `norm = R_inner² - 1`.
3. **`four_rin_sq_k` / `four_rout_sq_k` constants.** CPU derives these at `CampaignConstants::from_radii` (`campaign_constants.cpp`, not shown but per-plan §3 C2). **Enforcement:** upload via `cudaMemcpyToSymbol` as `i128` (hi/lo pair per blueprint §4.6: `four_rin_sq_k_hi, four_rin_sq_k_lo`). A single-line hi/lo assembly error at the host-upload site silently zeros the bound.
4. **Geo-flag application to UF groups.** CPU loop at `tileop.cpp:242-252`: for each prime `i`, look up `raw_root = local_dsu.find(i)`, then `label = remap.wire_label_by_raw_root[raw_root]`, then `bit_set(out.inner_flags, label)` and `bit_set(out.outer_flags, label)`. **Enforcement:** the CUDA loop must iterate primes in the same ascending index order (= post-sieve-sort order). Parallel reduction into `inner_flags[16]` via atomic-OR is safe (bit-OR is commutative and idempotent), so the *value* is order-independent, but the SET of labels that get bits depends on correct dense-label assignment (§2.3 inv. 2).

**CUDA enforcement mechanism.**

- New Stage 6 kernel, fused with Stage 7 in the same kernel launch to avoid a round-trip. Per-thread: compute `norm_sq` from `packed_pos + a_lo + b_lo`, call `is_inner` and `is_outer`, stage 2-bit `{inner, outer}` value in shared memory keyed by prime index.
- After dense remap (Stage 7), a second pass reads the staged bits for each prime index `i`, looks up `label = dense_remap[find(i)]`, does `atomicOr(&inner_flags[(label-1) >> 3], 1u << ((label-1) & 7))` and symmetrically for outer.
- `static_assert(sizeof(__int128_emulated) == 16)` to prevent accidental `int64` fallback.

**Byte-parity test.** Exercises `inner_flags[0..15]` and `outer_flags[0..15]` — bytes 196-227 of TileOp. Construct a synthetic tile whose primes sit exactly on the boundary: one prime at `norm² = R_inner²` (floor of band, must pass with `inner=true`), one at `norm² = (R_inner + C)² - 1` (inside band), one at `norm² = (R_inner + C)² + 1` (outside band, `inner=false`). Hand-compute the expected `inner_flags[0]` byte; CUDA must match.

### 2.3 Stage 7 — Local UF over tile primes + dense remap

This is the **single most correctness-sensitive stage.** Dense-remap is the v1-vs-v2 gap called out in §1.

**Determinism invariants.**

1. **DSU construction uses smaller-root-wins.** CPU `DSU::unite` at `union_find.cpp:55-66`: `std::swap(ra, rb)` if `ra > rb`, then `parent[rb] = ra`. Path compression via path-halving in `find` at `:43-53`. The root *set* after all unions is order-invariant (classic DSU result with smaller-id tie-break), but `parent_[i]` for non-root `i` is race-order-dependent under naive parallel unions. **Enforcement:** atomic CAS with retry, smaller-root-wins — v1's `atomic_union` at `kernel_uf.cu:57-75` is byte-correct for the root *set*. HOWEVER, the per-element `find(i)` value can differ because path compression is not re-run synchronously. The subsequent dense-remap consumer runs `find(i)` after all unions complete; if path compression is not fully propagated, two primes in the same component can `find()` to *different* roots transiently. **Enforcement:** a final full-compression pass before dense-remap reads roots. `kernel_uf.cu:134-137` already does this: `tile_parent[i] = atomic_find_root(tile_parent, i)` for every `i`. Confirm this pass runs with a `__syncthreads()` fence before dense-remap begins.
2. **Dense-label assignment order — THE invariant.** CPU `dense_remap_raw_roots_for_test` at `tileop.cpp:181-204` iterates `for raw_root in raw_roots` (a `vector<int32_t>` populated by `dense_remap_roots` at `:172-179` via `for i=0..prime_count; raw_roots.push_back(dsu->find(i))`). This means: **dense label `1` goes to the root of prime index 0 (post-sieve-sort). Dense label `2` goes to the root of the first subsequent prime (in index order) that maps to a *different* root. Etc.** The mapping `wire_label_by_raw_root[root] = max_label + 1 (1-based)` is assigned at *first encounter in ascending prime-index order*, not at first encounter in root-ID order. **Enforcement:** single-threaded compaction after path compression. Thread 0 loops `for i=0..N`, performs atomic-or-equivalent dedup on `wire_label_by_raw_root[raw_root[i]]`. Any multi-threaded compaction via `thrust::unique_by_key` or warp-scan will assign labels by root-ID sort order instead of prime-index scan order → wire-byte divergent. **This is the v1-vs-v2 gap:** v1 CUDA had no such pass; v1 CPU had a different (raw-root-based) encoder. The v2 invariant is new and untested on GPU.
3. **max_label overflow triggers OVERFLOW_BIT.** `tileop.cpp:192-195`: if `max_label >= MAX_GROUPS_PER_TILE` (128) during dense remap, set `overflow = true` and return. **Enforcement:** thread 0 writes `d_overflow[tile_idx] = 1` atomically; the subsequent Stage 9 encode path checks this flag and emits the canonical overflow TileOp (bytes 0-255 zero except `tile_flags = OVERFLOW_BIT = 0x01` at offset 228).
4. **Prime pair iteration order in DSU build.** CPU `build_local_dsu` at `tileop.cpp:161-168`: `for i=0..N; for j=i+1..N; if within_k_sq(primes[i], primes[j]) dsu.unite(i, j)`. Order of `unite` calls is lexicographic in `(i, j)`. Because smaller-root-wins is order-invariant for the root *set* and we re-run full path compression after all unions, **this order dependence does not propagate past Stage 7**. *But the CPU's `dsu.roots()` (`union_find.cpp:72-84`) sorts roots ascending before returning*, and face-strip UF in Stage 8 iterates this sorted list (`tileop.cpp:117-118`). So indirect order-dependence through `roots()` does matter for Stage 8. Enforcement there (§2.4 inv. 2).
5. **`within_k_sq` predicate uses `__int128`.** `tileop.cpp:46-50`: `(da*da + db*db) <= (__int128)k_sq_value`. At `K=36, 40` this fits in `i64` comfortably, but the predicate is written defensively. **Enforcement:** CUDA version must use the same i128 multiply or a proven-equivalent u64 with overflow guard. Given `|da|, |db| <= C + S = 262` at project scale, `da*da + db*db <= 2 * 262² = 137288 << 2⁶³`, so `int64` is safe in practice — but the v2 spec uses i128 for defense; mirror it.

**CUDA enforcement mechanism.**

Proposed kernel layout (explicit, committing):

```
k_uf_stage7(
    const Prime* primes_sorted,  // per-tile, already (a, b) sorted by k_sieve
    int N,
    uint16_t* parent_out,        // per-tile DSU parent array
    uint8_t* dense_label_out,    // per-tile raw_root -> 1-based label
    uint8_t* max_label_out,      // per-tile max_label (0..128)
    uint8_t* overflow_out        // per-tile overflow bit
)
```

Three phases in one kernel:

**Phase A (Union pass, 288 threads):** Each thread `tid` handles `i = tid, tid+288, ...`. For each `i`, inner loop `j = i+1..N` performs `if within_k_sq: atomic_union(parent, i, j)`. Note: the pair `(i, j)` is visited by thread owning `i`, not by both — avoids double unions. `__syncthreads()`.

**Phase B (Full compression, 288 threads):** `for i = tid; i < N; i += 288: parent[i] = atomic_find_root(parent, i)`. `__syncthreads()`.

**Phase C (Single-threaded dense remap, thread 0 only):** `max_label = 0; for i = 0..N: root = parent[i]; if dense_label[root] == 0: if max_label >= 128: overflow = true; break; else: dense_label[root] = ++max_label;`. `__syncthreads()`.

Phase C runs on thread 0 only. This is the **non-negotiable determinism guarantee.** The performance team will want to parallelize — forbid it at the review gate. Thread 0 serial work on ≤6144 primes at ~1 cycle per comparison is <10 microseconds; imperceptible.

**Byte-parity test.** Exercises every byte of `face_groups[0..sum(n)]` (offsets 4..195) and every bit of `inner_flags` / `outer_flags` (offsets 196..227). Direct test: construct a synthetic tile with 3 UF components, primes in a specific order such that the component containing prime index 0 is NOT the component containing prime index 1 in root-ID order. Hand-derive the expected dense label assignment (label 1 = root of prime 0, label 2 = root of prime 1 if different component, ...). Run CPU, record golden TileOp bytes. Run CUDA, must match.

### 2.4 Stage 8 — Face-strip UF + canonical port sort + group-label lookup

**Determinism invariants.**

1. **Face-strip candidate selection order.** CPU `build_face_ports` at `tileop.cpp:92-103`: `for i=0..N` sequentially, push `i` into `face_indices` iff `on_face_strip(primes[i], coord, face)`. Result: `face_indices` is ascending, contiguous in prime-index space. **Enforcement:** CUDA warp-scan filter producing an ascending-ordered list, or single-threaded for simplicity (face-strip sizes are ≤50 primes per face typically).
2. **Face-strip DSU build order.** `tileop.cpp:106-114`: `for i=0..M; for j=i+1..M; if within_k_sq: face_dsu.unite(i, j)` over `face_indices`. Face DSU uses local indices `0..M-1`, not original prime indices. The *unit* of indexing is `face_indices[k]`, but the *DSU parent slot* is `k`. **Enforcement:** same iteration order. Face-strip primes are small (<50), so single-threaded per face is fine.
3. **Port representative selection — lex `(h, p_perp)` with FIRST-ENCOUNTERED tiebreak on equal `(h, p_perp)`.** `tileop.cpp:119-138`: `for root in face_dsu.roots(): best_h, best_perp, have_rep = 0, 0, false; for k=0..M: if face_dsu.find(k) != root continue; get (h, p_perp); if !have_rep || h < best_h || (h == best_h && p_perp < best_perp): update best and label`. Note: equality `h == best_h && p_perp == best_perp` does NOT update (strict `<`). This means the FIRST prime encountered (in ascending `k` = ascending `face_indices` = ascending original prime index = ascending sieve-sort `(a, b)` order) with the lex-min `(h, p_perp)` wins. **Enforcement:** single-threaded scan over face-DSU roots on GPU, or deterministic atomic-min with a composite key `(h, p_perp, face_index_k)` where `face_index_k` is the tiebreaker. Do NOT use `atomicMin` on a packed `(h, p_perp)` pair — you need the third tiebreaker.
4. **`face_dsu.roots()` returns ascending-sorted roots.** `union_find.cpp:72-84`. **Enforcement:** sorted iteration in CUDA — emit a sorted unique-root list. With ≤50 primes this is trivial.
5. **Final port sort — `std::sort` with explicit `(h, p_perp, global_wire_label)` 3-key comparator.** `tileop.cpp:143-147`. The third key (`global_wire_label`) is NON-OBVIOUS because in most cases `(h, p_perp)` is already unique per port — but if two components happen to share the lex-min representative position (impossible on shared-face Lemma 4 but POSSIBLE within a single tile's face because components' representatives can collide if they don't share primes), the label tiebreak matters. `std::sort` is not stable, so without the third key the sort order would be unspecified. **Enforcement:** CUDA CUB/thrust must use `stable_sort` OR a full 3-key comparator. The `std::sort` contract is equivalent to `stable_sort` *only* when the comparator gives strict total ordering — which adding `global_wire_label` provides. Recommend: full 3-key comparator. `thrust::sort` with a functor implementing `(h, p_perp, label)` comparison.
6. **Port label = `remap.wire_label_by_raw_root[local_dsu.find(prime_idx)]`.** `tileop.cpp:135-136`. The label lookup uses the RAW prime index (from `face_indices[k]`) into the TILE-level DSU, not the face-strip DSU. **Enforcement:** CUDA must keep the face-strip DSU separate from the tile-level DSU; port label comes from tile-level dense-remap, not face-strip roots.

**CUDA enforcement mechanism.**

Per-face, per-tile, per-block: 4 faces × 288-thread block.

Phase 1 (filter): warp-scan over `N` primes producing `face_indices[M]` per face in ascending order.

Phase 2 (face DSU): single warp builds face DSU with same `i, j` nested-loop order as CPU. M≤50, 32 threads, ~1ms total across all four faces.

Phase 3 (port extraction): single thread per port scans `face_indices` ascending, picks lex-min `(h, p_perp)` representative with strict-`<` test (matches CPU). Stores `(h, p_perp, label)` triple.

Phase 4 (sort): one warp per face runs a bitonic sort on the port list using `(h, p_perp, label)` 3-key comparator. M≤50 ports, warp-local.

Phase 5 (write face_groups bytes): thread 0 writes `face_groups[write_offset..write_offset+M]` sequentially, in face order I→O→L→R, accumulating `write_offset = n[0] + n[1] + n[2] + ... ` per `tileop.cpp:272-278`.

**Byte-parity test.** Exercises `n[4]` (bytes 0-3) and `face_groups[sum(n)]` (bytes 4..195). Construct a tile with two components on the same face, with representatives colliding at the same `(h, p_perp)` — verify the third tiebreak on label works. (This is an adversarial case the CPU golden should also exercise; flag to include in the 5-tile golden.)

### 2.5 Stage 9 — TileOp 256-byte pack

**Determinism invariants.**

1. **Fixed-offset layout.** CPU `tileop.h:52-58` + compile-time `static_assert(offsetof(TileOp, n) == 0)` etc. at `:64-69`. **Enforcement:** CUDA `struct TileOp` mirror with identical `offsetof` asserts. Any padding insertion (e.g., compiler `#pragma pack`) breaks wire parity. Use `__attribute__((packed))` or explicit `std::memcpy`-based write.
2. **Zero-init then populate.** `tileop.cpp:229 (TileOp out{};)` zero-initializes every byte, including `reserved[27]`. Subsequent writes only populate `n[4]`, `face_groups[sum(n)]` (NOT all 192 — only the first `sum(n)`), `inner_flags[16]`, `outer_flags[16]`, `tile_flags`. Bytes `face_groups[sum(n)..192]` stay zero. `reserved[27]` stays zero. **Enforcement:** `memset(tileop, 0, 256)` then selective writes. Plausible GPU bug: if Stage 8 writes all 192 `face_groups` bytes unconditionally (padding with stale values or repeat-last-label), the padding region bytes differ.
3. **Overflow emits canonical shape.** `tileop.cpp:151-155, :238-240, :260-262, :268-270`: `overflow_tileop()` returns `TileOp{}` with only `tile_flags = OVERFLOW_BIT = 0x01`. **Enforcement:** upon any overflow detection (dense-remap max_label>=128 at Stage 7, or sum(n)>192 at Stage 8, or individual face port count > 255), immediately switch to overflow emit path — do NOT partially fill face_groups then overlay `tile_flags`.
4. **Empty-tile emits canonical shape.** `tileop.cpp:230-233`: `primes.empty() → tile_flags = EMPTY_BIT = 0x02`, all other zero. **Enforcement:** check `N==0` at Stage 5 output, fast-path emit.
5. **Little-endian byte order assumed.** Snapshot writer (`snapshot.cpp:146-153`) does `out.write(&op.n, 4)` directly — interprets TileOp as raw bytes. Host CPU is little-endian (x86_64, Mac Mini M4 ARM64 also LE). CUDA device is also LE. **Enforcement:** `static_assert(std::endian::native == std::endian::little)` in snapshot writer TU.
6. **`face_groups[off + p]` write order.** `tileop.cpp:272-278`: outer loop face I→O→L→R, inner loop `p=0..n[face]`. Each byte is written exactly once. **Enforcement:** mirror the loop structure in the CUDA writer.

**CUDA enforcement mechanism.**

- Per-tile TileOp is assembled in shared memory, then `memcpy` to global via a single 256-byte vectorized write (`uint4` × 16) at the end of Stage 9.
- `__constant__ uint8_t kFaceOrder[4] = {0, 1, 2, 3}` (I, O, L, R).
- Emit sequence: `reset(256B to 0)`; write `n[4]`; if not overflow/empty: write `face_groups` per loop; write `inner_flags`; write `outer_flags`; write `tile_flags`. Do not touch `reserved[27]`.

**Byte-parity test.** Exercises every one of the 256 bytes. Direct test on the 5-tile K=36 R=80M golden (the CPU hand-traced anchor from `cpp-campaign-v2/goldens/5tile-k36-inner.snapshot.bin`): compute CUDA snapshot, byte-diff against golden. Zero differences = pass.

## 3. Repo layout for `cuda-campaign-v2-sqrt-36/`

```
cuda-campaign-v2-sqrt-36/
├── CMakeLists.txt              # -DK_SQ=36; find_package(CUDA); links cpp-campaign-v2 oracle
├── README.md
├── include/cuda_campaign/
│   ├── constants.cuh           # mirrors cpp-campaign-v2/include/campaign/constants.h
│   ├── tileop.cuh              # mirrors include/campaign/tileop.h — same static_asserts
│   ├── fj64_table.cuh          # verbatim copy of cpp-campaign-v2/include/campaign/fj64_table.h
│   ├── i128_emu.cuh            # uint2/ulonglong2 emulated i128 + signed multiply
│   ├── mul_mod.cuh             # __umul64hi-based mul_mod_u64 for MR
│   └── kernels.cuh             # kernel launch API
├── src/
│   ├── kernel_sieve.cu         # Stage 5 — ported from campaign-sqrt-36/tile_cuda_multi_kernel
│   ├── kernel_mr.cu            # Stage 5b — FJ64 MR, ported verbatim
│   ├── kernel_compact.cu       # Stage 5c — ported
│   ├── kernel_geo.cu           # Stage 6 — NEW, not in v1
│   ├── kernel_uf_stage7.cu     # Stage 7 — local UF + dense remap (CRITICAL)
│   ├── kernel_face_encode.cu   # Stages 8+9 — face-strip UF + port sort + 256B pack
│   ├── host_driver.cu          # orchestration, burst dispatch
│   └── snapshot_writer.cu      # calls into cpp-campaign-v2 snapshot.cpp directly via oracle lib
├── apps/
│   ├── cuda_campaign_main.cu   # CLI driver — same flags as cpp-campaign-v2/apps/campaign_main
│   └── cuda_vs_cpu_diff.cu     # byte-parity tool: runs both, diffs snapshots, reports offset of first diff
├── tests/
│   ├── test_k1k2_parity.cu     # MR witness parity over full candidate band
│   ├── test_geo_i128.cu        # i128 emulation parity for geo tests
│   ├── test_dense_remap.cu     # THE critical test — dense-remap label order
│   ├── test_port_sort.cu       # 3-key comparator tiebreak test
│   └── test_5tile_golden.cu    # snapshot byte-diff vs cpp-campaign-v2/goldens/5tile-k36-inner.snapshot.bin
├── goldens/                    # symlinks to cpp-campaign-v2/goldens/
└── scripts/
    └── run_burst_parity.sh     # N-tile random-region parity against CPU oracle
```

**CMake linkage strategy.** The CPU oracle is built as a static library (`libcampaign.a` from cpp-campaign-v2). This repo's CMake adds it via `add_subdirectory(../cpp-campaign-v2 cpp_campaign_v2_build)` and links `cuda_campaign` against `campaign`. Tests can then call CPU `process_tile(coord, constants, grid)` side-by-side with CUDA kernel launches and diff the resulting TileOps in-memory — no filesystem round-trip needed for unit tests.

The 5-tile golden snapshot (`cpp-campaign-v2/goldens/5tile-k36-inner.snapshot.bin`) is consumed verbatim — same format, same bytes expected.

## 4. Host integration

**Kernel launch points.**

```
cuda_campaign_main
  ├── Grid::build(R_inner, R_outer, K=36, offset=(1,1))   // host, reuse cpp-campaign-v2
  ├── CampaignConstants::from_radii(...)                  // host, reuse
  ├── upload CampaignConstants, fj64_table, bk_offsets to __constant__
  ├── allocate burst buffers: d_inputs[200K * 24B], d_cand[200K * 24KB], ...
  ├── for burst in active_tile_bursts(200K each):
  │    ├── H2D: d_inputs <- TileInput[burst]
  │    ├── launch K1 kernel_sieve       (288 threads/block, 200K blocks)
  │    ├── launch K2 kernel_mr          (same)
  │    ├── launch K3 kernel_compact     (same)
  │    ├── launch K4 kernel_geo         (fused with K5_stage7 in one kernel, per blueprint §6.2)
  │    ├── launch K5_stage7 kernel_uf   (local UF + dense remap)
  │    ├── launch K6 kernel_face_encode (face-strip UF + port sort + 256B pack)
  │    └── D2H: host_tileops[burst] <- d_tileops
  └── write_snapshot(out_path, grid, host_tileops, constants)  // reuse cpp-campaign-v2
```

**Data movement.**
- Pinned host buffer for `TileInput[]` (upload) and `TileOp[]` (download), double-buffered per burst.
- Snapshot assembly is entirely host-side via `cpp-campaign-v2` `write_snapshot` (snapshot.cpp:157-234) — we don't re-implement the SHA-256 or manifest JSON on GPU.

**Snapshot diffing in CI / manual loop.**

Three gates, strictest to weakest:

1. **Byte-diff gate (primary, non-negotiable):** `cuda_vs_cpu_diff --region <region.json>` runs both implementations on the region, captures both snapshots, reports the FIRST byte offset of divergence. Exit 0 = parity pass, non-zero = failure with offset + tile-index localization.
2. **5-tile golden gate:** CUDA snapshot on the hand-golden 5-tile region must byte-match `cpp-campaign-v2/goldens/5tile-k36-inner.snapshot.bin`. This uses the CPU-generated golden as the anchor; CUDA must agree with the CPU-anchored ground truth.
3. **Full-octant K=36 R=80M parity gate (ship gate):** run CPU for ~27h (reference), run CUDA for ~1h target, diff snapshots. Must be byte-identical. This is the ship criterion.

CI runs gate 1 and gate 2 on every commit (small regions, <5 min). Gate 3 runs manually pre-ship on the 4090.

## 5. Milestone sequence

**Wall-clock estimates are Opus's honest take; a coordinator may compress with more swarm.**

### M1 — Scaffold + link to CPU oracle — 4 hours
- Repo skeleton per §3.
- CMake links `cpp-campaign-v2` as static lib.
- `cuda_vs_cpu_diff` stub that runs CPU, runs CUDA (stub = identity passthrough of CPU), diffs, exits 0.
- **Byte-parity gate:** diff tool runs end-to-end on a 1-tile region, 0-byte diff (trivially because CUDA = CPU passthrough).

### M2 — Port K1/K2/K3 kernels verbatim from v1 + FJ64 parity — 1 day
- Copy `kernel_sieve.cu`, `kernel_mr.cu`, `kernel_compact.cu` from `campaign-sqrt-36/tile_cuda_multi_kernel/src/`.
- Update `kernel_mr.cu` to use `cpp-campaign-v2/include/campaign/fj64_table.h` verbatim.
- Unit test: `test_k1k2_parity` — for every `n` in `[R_inner² - K, R_outer² + K]` at R=1000 (small), assert `is_prime_cuda(n) == is_prime(n)` cpp-campaign-v2.
- **Byte-parity gate:** CUDA `Prime[]` output bitwise-matches CPU `sieve_tile()` output on 100 random tiles at R=1000, K=36.

### M3 — Port Stage 6 geo kernel + i128 emulation — 1 day
- Implement `i128_emu.cuh` with signed multiply, signed compare.
- Implement `kernel_geo.cu` that computes `is_inner_prime`, `is_outer_prime` per prime.
- Unit test: `test_geo_i128` — synthetic norm_sq values spanning the band, compare with cpp-campaign-v2 `geo_tests.cpp` output.
- **Byte-parity gate:** staged `{is_inner, is_outer}` array matches CPU for all 100 test tiles.

### M4 — Stage 7 local UF + dense remap — 2 days
- Implement `kernel_uf_stage7.cu` with the 3-phase layout (§2.3).
- Phase C (single-threaded compaction) is the correctness-critical point — review gate here.
- Unit test: `test_dense_remap` — adversarial DSU topologies with 3+ components, verify byte-identical `dense_label[]` array against CPU.
- **Byte-parity gate:** `{parent[], dense_label[], max_label, overflow}` matches CPU across 100 test tiles.

### M5 — Stages 8+9 face-strip UF + port sort + 256B pack — 2 days
- Implement `kernel_face_encode.cu`.
- Critical: 3-key port-sort comparator; `face_dsu.roots()` ascending iteration; first-encountered representative tiebreak.
- **Byte-parity gate:** full TileOp 256 bytes match CPU across 100 random tiles at R=1000, K=36. First byte of divergence reported if any.

### M6 — 5-tile golden parity — 1 day
- Run CUDA on the `cpp-campaign-v2/goldens/5tile-k36-inner` region.
- Byte-diff CUDA snapshot against the committed golden.
- **Byte-parity gate:** 0 bytes diff across 5 × 256 = 1280 bytes (plus header).
- If any diff, localize by tile and by field (use `cuda_vs_cpu_diff` with `--verbose` reporting).

### M7 — Small-scale full pipeline parity at R=1000-10000 — 1 day
- Run CPU + CUDA on the full octant at R=1000, 5000, 10000 (small enough to run CPU in <1min each).
- **Byte-parity gate:** 0 bytes diff across entire octant at each of 3 scales. Overflow rate must be 0 at these scales (per blueprint §10).

### M8 — Project-scale parity at R=80M K=36 — 1 day
- Run CPU reference (est. 27h on Mac Mini 12-core).
- Run CUDA on 4090 (target ~1h).
- Byte-diff both snapshots.
- **Byte-parity gate — ship criterion:** 0 bytes diff. 8.18M tiles × 256B = 2 GB snapshot compared byte-wise.

**Total wall-clock:** ~9 days active work + ~28h CPU reference run. Budget 2.5 weeks with slack.

## 6. Risks specific to the CORRECTNESS angle

**Ranked by likelihood × blast radius.**

1. **R1 — Dense-remap parallelization temptation.** High likelihood. The performance team will see thread 0 doing single-threaded work on N≤6144 primes and want to parallelize via warp-scan, thrust::unique_by_key, or atomic-increment label counter. ANY parallel scheme produces a different label permutation → every tile with ≥2 components has byte-different `face_groups` and permuted flags. **Mitigation:** hard-pin thread 0 compaction in review gate; CI test `test_dense_remap` includes a stress-test tile with 50 components where parallel schemes statistically fail. Add to `kernel_uf_stage7.cu` a `// CORRECTNESS: DO NOT PARALLELIZE` comment block with a link to this planning doc.

2. **R2 — `thrust::sort` default is not stable.** High likelihood. Drop-in use of `thrust::sort` instead of `thrust::stable_sort` for port sort, or use of a 2-key comparator `(h, p_perp)` without the `global_wire_label` tiebreak, produces non-deterministic ordering when two ports share `(h, p_perp)`. Manifests as 1-byte shifts in `face_groups` on adversarial tiles. **Mitigation:** force 3-key comparator at review; `test_port_sort` includes an artificial `(h, p_perp)` collision.

3. **R3 — i128 emulation sign bug in geo tests.** Medium likelihood. `eps` is signed, can be negative; `eps * eps` emulation via `uint2` multiply-add must handle two's-complement properly. A bug here triggers at primes near but just inside one of the arcs — rare in `{inner_flags}` unit tests but common in the full-band sweep at R=80M. **Mitigation:** `test_geo_i128` sweeps `norm_sq` values across `[R_inner² - 2K, R_inner² + 2K]` and `[R_outer² - 2K, R_outer² + 2K]` and matches CPU bit-for-bit.

4. **R4 — `face_groups[sum(n)..192]` padding bytes not zero.** Medium likelihood. CUDA writer helpfully fills all 192 bytes via vectorized copy of the port list, padding with stale shmem. Manifests only under CPU byte-diff — the bits are "unused" semantically, but they're part of the 256 B wire. **Mitigation:** `memset(shmem_tileop, 0, 256)` at Stage 9 entry; `static_assert(offsetof + sizeof)` on every field; explicit write only of used bytes.

5. **R5 — OVERFLOW_BIT not atomically applied.** Medium likelihood. If Stage 8 detects `sum(n) > 192` mid-fill (after some face_groups already written), reverting to zero requires care. **Mitigation:** two-pass: first pass computes `sum(n)` across all four faces and checks `max_label`; if either overflows, skip the fill pass entirely, go directly to the overflow emit (`memset(0); tile_flags = 0x01`).

6. **R6 — FJ64 mixer constant drift.** Low likelihood but high blast. The mixer at `primality.cpp:71-73` uses `0x45d9f3b3335b369ULL` and `0x3335b36945d9f3bULL`. A one-hex-digit typo at the CUDA port site silently divergates every composite witness decision at a statistical rate of ~50%. **Mitigation:** do not re-type the constants; `#include "cuda_campaign/fj64_table.cuh"` which `#include`s `campaign/fj64_table.h` directly. If the CPU header is ever macro-gated, mirror the gate.

7. **R7 — Prime sort order diverges between sieve output and tileop input.** Low likelihood. CPU `sieve_tile()` returns primes sorted lex `(a, b)` (`sieve.cpp:116-119`); tileop.cpp then re-sorts by `(a, b, norm_sq, packed_pos)` (`:220-223`). The extra two keys `(norm_sq, packed_pos)` are deterministic functions of `(a, b)`, so the final order is identical — but ONLY if `Prime` fields are byte-equal. If CUDA computes `packed_pos` with a different `side = S + 1 + 2*C` (say, using `S + 2*C` by accident), `packed_pos` shifts → tiebreak changes → prime order changes → every downstream byte changes. **Mitigation:** `SIDE_EXP` as a `__constant__` and unit-tested equality with CPU `constants::SIDE_EXP`.

8. **R8 — Compositor OVERFLOW handling asymmetry.** Low likelihood at v2 scale (blueprint §10 reports 0/600 sampled tiles overflow), but high blast if it hits. Overflow forces conservative SPANNING on CPU (`compositor.cpp:148-153`). The snapshot byte format is the same, but the *compositor state* differs between CPU and CUDA if overflow is emitted on different tiles. Since the user's success criterion is "byte-identical TileOp snapshots" (not "same verdict"), this is a subtler: if CPU emits OVERFLOW on tile T but CUDA doesn't (due to a parallel race causing max_label miscount), the SNAPSHOTS DIFFER — tile T's bytes are different. **Mitigation:** dense-remap is strictly serial (thread 0 phase C) → overflow detection is deterministic → CPU and CUDA agree on which tiles overflow.

## 7. Open questions for the coordinator

1. **Q1 — Does the CUDA port need to preserve `reserved[27]` byte values in a NON-ZERO way for any future extension?** My read of `cpp-campaign-v2/src/tileop.cpp:229` says zero-init is the contract. Blueprint §5.2 calls it "future extension / alignment slack". Confirm that the CUDA port can hard-zero these 27 bytes. (If the answer is "yes, zero-init", this is trivial. If "reserved for a v4 field", we need to know now.)

2. **Q2 — Is `TOWER_CLOSING_BIT` (`constants.h:149`) emitted by the CPU v2 anywhere?** I can see the constant but didn't find a write site in the codebase scan. If CPU never sets it, neither should CUDA. Confirm.

3. **Q3 — Does the 5-tile K=36 golden already exist at `cpp-campaign-v2/goldens/5tile-k36-inner.snapshot.bin`, or is it scheduled for M5 of the CPU plan?** The cpp-campaign-v2 plan §4 M5 mentions it; I did not verify the file exists. If not yet committed, CUDA M6 must wait on the CPU team shipping the golden.

4. **Q4 — Are we budgeting for a full-octant K=40 parity run, or is K=36 sufficient for the MVP?** The prompt says "K=36 R=80M" but the CPU plan also covers K=40. I assume K=36 only for M8; if the coordinator wants K=40 too, double the M8 estimate.

5. **Q5 — Compositor stages (column-by-column DSU merge, verdict latch) are HOST-SIDE per §1 scope ("9-host, sequential DSU"). Confirm that the snapshot byte parity check targets ONLY the TileOp 256 B records, not the final verdict. The verdict is a function of TileOp bytes plus grid topology; if TileOp bytes are byte-identical, verdict is automatically identical. My plan assumes this; flag if wrong.**

6. **Q6 — On the 4090, Phase C of the Stage 7 kernel runs single-threaded (thread 0 serial over ≤6144 primes). This is correct but wastes 287 threads for ~5μs per tile. Am I authorized to reject any "optimization" PR that parallelizes Phase C, even if the PR passes `test_dense_remap`? My position: yes — the test may not exercise all adversarial topologies. But coordinator call.**

---

*End of correctness plan. Synthesizer input for the canonical merge.*
