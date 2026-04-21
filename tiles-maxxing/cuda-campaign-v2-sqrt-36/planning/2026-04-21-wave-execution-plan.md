---
date: 2026-04-21
type: wave-execution-plan
status: ready
---

# Wave Execution Plan — cuda-campaign-v2-sqrt-36

All workers below should run as Codex GPT 5.4 xhigh. Each worker owns its listed files for the wave and must finish with either a commit or a written artifact named in its report. Workers in the same wave are intentionally split by file ownership; if a worker discovers it needs a file owned by another same-wave worker, it should stop and report the dependency instead of editing across the boundary.

## Wave 1: K1/K2 Bring-Up and Independent Harness Scaffolds
**Depends on:** M1 (complete, commit `3aeef39`)

**Workers:**
- Worker 1A: M2 K1+K2 lift
  - Files: `include/cuda_campaign/gpu_math.cuh`, `include/cuda_campaign/fj64_table.cuh`, `include/cuda_campaign/campaign_constants.cuh`, `include/cuda_campaign/constants.cuh`, `src/constants_upload.cu`, `src/kernel_sieve.cu`, `src/kernel_mr.cu`, `include/cuda_campaign/kernels.cuh`
  - Gate: Build succeeds with CUDA enabled; 16-tile K1 bitmap parity at R=1000 passes; full-band FJ64/MR parity over `[R_inner^2-K, R_outer^2+K]` at R=1000 passes; FJ64 table SHA-256 matches the pinned CPU constant.
  - Prompt summary: Lift v1 `kernel_sieve.cu`, `kernel_mr.cu`, and required math/table support into the v2 scaffold. Preserve v1 primality behavior while changing constants to v2 naming and sizing, especially `C=floor_isqrt(K_SQ)` and `MAX_PRIMES_GPU=6144`. Do not touch K3-K5 files.
- Worker 1B: K3/K4 parity harness scaffold
  - Files: `tests/test_k3k4_parent_parity.cpp`, `tests/test_dense_remap_adversarial.cpp`, `tests/test_geo_i128_sweep.cpp`, `tests/support/k3k4_parity_support.*`
  - Gate: Test targets compile or, if kernel entrypoints are not yet present, produce a documented compile-gated artifact listing the exact API symbols expected from K3/K4; no production CUDA files modified.
  - Prompt summary: Build the test-side CPU oracle and fixture machinery for M3/M4 without implementing kernels. Encode the parent-array, dense-remap adversarial, and geo-i128 sweep expectations from the canonical plan so later kernel workers have executable gates ready.
- Worker 1C: K5 parity harness scaffold
  - Files: `tests/test_face_groups_parity.cpp`, `tests/test_port_sort_collision.cpp`, `tests/test_full_tileop_parity.cpp`, `tests/support/k5_parity_support.*`
  - Gate: Test targets compile or produce a compile-gated artifact naming the required K5 debug outputs; CPU-side adversarial fixture generation runs locally.
  - Prompt summary: Prepare the M5-M7 verification harnesses only. Focus on CPU expected data for face-strip filters, face_groups, 3-key port sort collisions, overflow/empty TileOps, and full 256 B TileOp parity.
- Worker 1D: Host pipeline scaffold artifact
  - Files: `planning/2026-04-21-host-pipeline-scaffold.md`
  - Gate: Artifact documents concrete file ownership, buffer interfaces, stream schedule, and two-phase overlay plan for M8; no code files modified.
  - Prompt summary: Convert the M8 host-pipeline requirements into an implementation note that a later worker can apply after K5 exists. Include chunk sizing, stream roles, pinned buffer ownership, snapshot handoff, and the exact dependency on the final K5 TileOp output.

## Wave 2: K3/K4 Independent Lifts
**Depends on:** Wave 1

**Workers:**
- Worker 2A: M3 K3 compact lift
  - Files: `src/kernel_compact.cu`, `include/cuda_campaign/compact_buffers.cuh`
  - Gate: K1/K2 plus K3 build succeeds; compacted prime positions and prime counts match CPU for the same 16 R=1000 test tiles used in M2.
  - Prompt summary: Lift v1 `kernel_compact.cu` into the v2 buffer layout with `MAX_PRIMES_GPU=6144`. Keep the implementation byte-for-byte close where possible, but adapt constants and public launch signatures to the M1/M2 scaffold.
- Worker 2B: M3 K4 union and compression lift
  - Files: `src/kernel_uf_v2.cu`, `include/cuda_campaign/uf_buffers.cuh`
  - Gate: With synthetic CPU-generated prime-position arrays uploaded directly to K4 input buffers, `d_parent[]` after full compression matches CPU `build_local_dsu(primes)` bit-for-bit; no dependency on Worker 2A output.
  - Prompt summary: Lift v1 `kernel_uf.cu` phases A and B only: backward-offset union and full compression. Do not add dense-remap or geo-flags in this wave; leave clear device-buffer slots or TODO hooks for the later M4 wave.

## Wave 3: M3 Integration, Geo Helper, and Early K5 Skeleton
**Depends on:** Wave 2

**Workers:**
- Worker 3A: M3 launch and API wiring
  - Files: `include/cuda_campaign/kernels.cuh`, `src/host_driver.cpp`, `apps/cuda_vs_cpu_diff.cpp`
  - Gate: The M3 parent-parity test can invoke K1-K4 through the public host API and reports first divergence with tile index and prime index; M2 gates still pass.
  - Prompt summary: Wire K3 and the K4 union/compression phase into the host-side test path. Keep this limited to launch orchestration and debug download plumbing; do not edit kernel implementation files.
- Worker 3B: M4 geo-test staging
  - Files: `include/cuda_campaign/i128_sq_leq.cuh`, `src/kernel_geo_flags.cu`, `tests/test_geo_i128_sweep.cpp`
  - Gate: Geo i128 sweep passes against CPU for inner and outer boundary bands at R=1000; no K4 dense-remap behavior changed.
  - Prompt summary: Implement the signed-epsilon i128 square/compare helper and per-prime `is_inner`/`is_outer` staging logic as an isolated callable device path. Match CPU `geo_tests.cpp` and use the canonical prefilter bound with ceiling sqrt.
- Worker 3C: M5 K5 empty/overflow skeleton and face-strip filter
  - Files: `src/kernel_face_encode_v2.cu`, `include/cuda_campaign/face_encode_buffers.cuh`, `tests/test_face_groups_parity.cpp`
  - Gate: Empty and overflow TileOps match CPU byte-for-byte using synthetic `prime_count` and `remap.overflow` inputs; active-tile face index debug output matches CPU `build_face_ports` intermediates for synthetic prime-position fixtures.
  - Prompt summary: Start K5 before full M4 integration by implementing only the stable skeleton: empty output, overflow output, and face-strip filtering. Read the M4 remap overflow and prime_count contracts, but do not implement face DSU, port sorting, or flags.

## Wave 4: M4 Geo Flags and Dense Remap Integration
**Depends on:** Wave 3

**Workers:**
- Worker 4A: M4 dense-remap and group-flag accumulation
  - Files: `src/kernel_uf_v2.cu`, `include/cuda_campaign/uf_buffers.cuh`, `tests/test_dense_remap_adversarial.cpp`
  - Gate: For 100 test tiles, `{parent[], wire_label_by_raw_root[], max_label, overflow, prime_geo_bits[], group_flags[]}` matches CPU bit-for-bit; adversarial inverted-root-order remap test passes.
  - Prompt summary: Extend K4 after compression with geo staging, thread-0 dense remap in ascending prime-index order, and parallel group-flag accumulation. Do not parallelize dense label assignment; preserve the CPU first-appearance semantics exactly.

## Wave 5: M4 Debug Wiring and K5 Face Port Helpers
**Depends on:** Wave 4

**Workers:**
- Worker 5A: M4 debug download and parity wiring
  - Files: `include/cuda_campaign/kernels.cuh`, `src/host_driver.cpp`, `apps/cuda_vs_cpu_diff.cpp`
  - Gate: The M4 parity tests can download `prime_geo_bits`, `wire_label_by_raw_root`, `max_label`, `overflow`, and `group_flags`; M2-M3 gates still pass.
  - Prompt summary: Wire the new M4 debug outputs into the host test path without changing kernel algorithms. Keep this focused on launch signatures, downloads, and first-divergence reporting.
- Worker 5B: M6 face-strip DSU and representative selection
  - Files: `src/kernel_face_encode_v2.cu`, `include/cuda_campaign/face_encode_buffers.cuh`
  - Gate: Debug output for per-face DSU roots and selected representatives matches CPU for 100 test tiles plus adversarial face-port fixtures.
  - Prompt summary: Add K5 Phase 2 and Phase 3: per-face DSU over face-strip primes using the CPU nested-loop pair order, then canonical representative selection with strict lexicographic minimum. Stop before final port sorting and byte packing.
- Worker 5C: M6 canonical port sort and face_groups packing
  - Files: `src/kernel_face_sort_pack.cu`, `include/cuda_campaign/face_sort_pack.cuh`, `tests/test_port_sort_collision.cpp`
  - Gate: With synthetic per-face representative records, `n[4]` and `face_groups[192]` match CPU for 100 fixtures and the port-sort collision adversarial case; padding bytes after `sum(n)` are zero.
  - Prompt summary: Implement the warp-local bitonic sort and face-order byte packing as a separate K5 helper. The comparator must be `(h, p_perp, global_wire_label)`, including the third-key label tiebreak.

## Wave 6: K5 Face Port Integration
**Depends on:** Wave 5

**Workers:**
- Worker 6A: M6 K5 integration wiring
  - Files: `include/cuda_campaign/kernels.cuh`, `src/host_driver.cpp`, `apps/cuda_vs_cpu_diff.cpp`
  - Gate: The face_groups parity test invokes the full K5 skeleton plus DSU/sort/pack path through the public host API; M2-M4 gates still pass.
  - Prompt summary: Connect the K5 helper outputs to the host-visible TileOp debug path and test runner. Keep this worker focused on launch and download plumbing, leaving algorithm files to Workers 5B and 5C.

## Wave 7: Terminal TileOp Parity
**Depends on:** Wave 6

**Workers:**
- Worker 7A: M7 flag remap and overflow finalization
  - Files: `src/kernel_face_encode_v2.cu`, `include/cuda_campaign/tileop.cuh`
  - Gate: Full 256 B TileOp parity passes for 1024 tiles at R=10000, K=36; overflow path writes exactly zero payload plus `tile_flags=0x01`; empty path writes exactly zero payload plus `tile_flags=0x02`.
  - Prompt summary: Finish K5 by unpacking dense group flags into `inner_flags` and `outer_flags`, and by enforcing the final `n[f] > 255` or `sum(n) > 192` overflow gate before writing any normal payload. Preserve the 256 B TileOp v3 layout exactly.
- Worker 7B: M7 full parity CLI hardening
  - Files: `apps/cuda_vs_cpu_diff.cpp`, `tests/test_full_tileop_parity.cpp`
  - Gate: `cuda_vs_cpu_diff --verbose` reports tile index, byte offset, CPU byte, and GPU byte for a synthetic injected mismatch; the existing M6 parity path still exits zero.
  - Prompt summary: Harden the existing M1 diff CLI for the M7 acceptance gate. Keep all behavior test-facing; do not modify kernel algorithms.

## Wave 8: Campaign Host Pipeline
**Depends on:** Wave 7

**Workers:**
- Worker 8A: M8 campaign_main_cuda fork and snapshot handoff
  - Files: `apps/campaign_main_cuda.cpp`, `CMakeLists.txt`
  - Gate: CUDA campaign app builds against the existing dispatcher interface; with a small stub/debug TileOp source it writes a snapshot through the unmodified CPU `write_snapshot` path and matches CPU manifest metadata.
  - Prompt summary: Fork the CPU campaign main into a CUDA campaign app and replace only the tile-processing loop with calls into the GPU dispatcher. Preserve CPU grid construction, compositor, and snapshot writer behavior.
- Worker 8B: M8 three-stream dispatcher and buffer overlay
  - Files: `src/host_driver.cpp`, `include/cuda_campaign/host_driver.h`
  - Gate: Small-region async dispatch produces the same TileOp bytes as `cuda_vs_cpu_diff`; CUDA memory peak reflects the two-phase overlay design; sanitizer or CUDA error checks report no stream misuse.
  - Prompt summary: Implement the chunked GPU dispatcher with three pinned buffer sets and three streams for H2D, compute, and D2H overlap. Reuse K1/K2 buffer space for K3/K4/K5 buffers after the phase boundary.
- Worker 8C: M8 snapshot SHA gate harness
  - Files: `tests/test_snapshot_sha_R80M.cpp`, `scripts/run_snapshot_sha_gate.sh`
  - Gate: Harness can run CPU and CUDA snapshot generation, compute SHA-256 for both outputs, and fail with paths and hashes on mismatch; it supports the full R=80M gate without changing production code.
  - Prompt summary: Build the acceptance harness for the M8 ship gate. The full run may be long, but the harness must support both smoke-scale and R=80M K=36 invocations with identical comparison logic.

## Wave 9: Full Snapshot Acceptance
**Depends on:** Wave 8

**Workers:**
- Worker 9A: M8 full K=36 R=80M byte-parity run
  - Files: `artifacts/2026-04-21-m8-r80m-sha-report.md`
  - Gate: Full K=36 R=80M CUDA snapshot SHA-256 equals CPU golden SHA-256; if it fails, the artifact includes first-divergence output from `cuda_vs_cpu_diff --verbose`.
  - Prompt summary: Run the full M8 acceptance gate on the target GPU environment. Produce an artifact with command lines, host/GPU identity, snapshot paths, hashes, elapsed time, and pass/fail result.

## Wave 10: Performance Tuning
**Depends on:** Wave 9

**Workers:**
- Worker 10A: M9 register-cap and kernel-profile tuning
  - Files: `CMakeLists.txt`, `planning/2026-04-21-m9-register-tuning.md`
  - Gate: Nsight or nvcc resource report compares K2 uncapped vs `--maxrregcount=44`, K4 cap 40 vs 44, and records the fastest passing configuration; M8 snapshot SHA still matches.
  - Prompt summary: Tune per-TU register caps without changing algorithms. Document measured occupancy, register count, throughput, and the selected compile flags.
- Worker 10B: M9 K5 shared-memory and bank-conflict audit
  - Files: `src/kernel_face_encode_v2.cu`, `src/kernel_face_sort_pack.cu`, `planning/2026-04-21-m9-k5-smem-audit.md`
  - Gate: K5 shared-memory audit shows no material bank-conflict bottleneck or includes a minimal passing fix; full 256 B M7 parity still passes.
  - Prompt summary: Profile K5 shared-memory access patterns and apply only local layout fixes if they are proven necessary. Do not alter TileOp semantics or face ordering.

## Wave 11: Performance Acceptance
**Depends on:** Wave 10

**Workers:**
- Worker 11A: M9 end-to-end throughput validation
  - Files: `artifacts/2026-04-21-m9-throughput-report.md`
  - Gate: Full K=36 R=80M run reaches `tiles/sec >= 155000` on 4090 and snapshot SHA still matches M8; if below target, artifact includes the bottleneck profile and no speculative code changes.
  - Prompt summary: Run the final performance gate using the best build from Workers 10A and 10B. Report wall-clock, compute throughput, stream overlap, compositor ingest timing, snapshot I/O timing, and SHA result.
