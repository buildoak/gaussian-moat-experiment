---
date: 2026-03-26
engine: coordinator
status: draft
type: architecture
revision: v2 (post-critique)
---

# fat-stripe-cuda: Architecture and Implementation Plan (v2)

## 0. Design Summary

A persistent CUDA/C++ worker process (`fat_stripe_cuda`) communicates with a Rust orchestrator (`fat-stripe-cuda` crate) over stdin/stdout pipes. The worker accepts tile jobs as a binary stream, processes them on the GPU (sieve + Miller-Rabin -> primality bitmap -> CPU union-find -> face-port extraction), and emits a binary face-port result stream. The Rust orchestrator reads results, constructs `TileOperator`s, and feeds them into the existing `moat-kernel` composition pipeline.

**Key design choices (v2):**

- **Persistent subprocess over pipes**, not per-batch spawn + file I/O. One CUDA context init, one sieve-table init, streaming jobs/results. File mode retained as a debugging fallback only.
- **Phase 1 is a parity harness**, not the production shape. CPU-side UF in the C++ worker deliberately mirrors the Rust `tile.rs` sparse cell-hash UF with origin handling, not the `tile_main.cu` dense bitmap UF. This is slower but guarantees identical semantics. Phase 2 replaces it with GPU UF.
- **Exact inclusive bounds per tile** on the wire. No global `tile_side` -- tiles can be clipped at annular edges. The Rust orchestrator already produces non-square tiles at chunk boundaries (`tb_hi = tb_hi_raw.min(b_chunk_hi)` in `chunk.rs:66`).

---

## 1. Module Structure

### 1.1 Repository Layout

```
gaussian-moat-cuda/
  src/
    tile_main.cu            # existing single-tile CUDA binary (unchanged)
    fat_stripe_cuda.cu      # NEW: persistent worker, main loop on stdin/stdout
    batch_dispatch.cuh      # NEW: multi-tile kernel launch + memory pool
    face_extract.cuh        # NEW: bitmap -> sparse-cell UF -> face ports (CPU-side)
    face_port_io.h          # NEW: binary protocol structs + read/write helpers
    row_sieve.cuh           # existing (reused as-is)
    tile_kernel.cuh         # existing (reused as-is)
    modular_arith.cuh       # existing (reused as-is)
    miller_rabin.cuh        # existing (reused as-is)
    cornacchia.cuh          # existing (reused as-is)
  CMakeLists.txt            # extended: new fat_stripe_cuda target
  tile-probe/
    Cargo.toml              # extended: new fat-stripe-cuda crate
    crates/
      fat-stripe-cuda/      # NEW Rust crate
        Cargo.toml
        src/
          lib.rs            # public API: run_cuda_campaign()
          protocol.rs       # binary face-port reader/writer (de/serialize)
          driver.rs         # manage persistent CUDA child process over pipes
          orchestrator.rs   # campaign loop: stream jobs -> read results -> compose -> verdict
          config.rs         # CudaFatStripeConfig
          main.rs           # CLI binary
      fat-stripe/           # existing CPU crate (unchanged, serves as reference)
      moat-kernel/          # existing (unchanged, reused for composition)
```

### 1.2 Build System

**CMake (CUDA/C++ side):**

```cmake
add_executable(fat_stripe_cuda
    src/fat_stripe_cuda.cu
)
target_include_directories(fat_stripe_cuda PRIVATE src/)
# Build ID embedded for handshake (see Section 2.2)
string(TIMESTAMP BUILD_TS "%Y%m%d%H%M%S" UTC)
target_compile_definitions(fat_stripe_cuda PRIVATE
    FSC_BUILD_ID="${BUILD_TS}"
)
```

**Cargo (Rust side):**

New workspace member in `tile-probe/Cargo.toml`:

```toml
members = ["crates/moat-kernel", "crates/ise", "crates/lb-probe", "crates/fat-stripe", "crates/fat-stripe-cuda"]
```

New crate `fat-stripe-cuda/Cargo.toml`:

```toml
[package]
name = "fat-stripe-cuda"
version = "0.1.0"
edition = "2021"

[[bin]]
name = "fat-stripe-cuda"
path = "src/main.rs"

[dependencies]
moat-kernel = { path = "../moat-kernel" }
clap = { version = "4", features = ["derive"] }
serde = { version = "1", features = ["derive"] }
serde_json = "1"
bytemuck = { version = "1", features = ["derive"] }
```

**Connection:** Cargo does not invoke CMake. The CUDA binary is built separately. The Rust crate locates it via:
1. `FAT_STRIPE_CUDA_BIN` environment variable (explicit path)
2. `../../../build/fat_stripe_cuda` relative to crate root (development default)
3. `fat_stripe_cuda` on PATH (deployed)

**Build skew protection:** The worker emits a `build_id` string in its hello message. The Rust driver checks it against its own expected version at connection time. Mismatched builds are a hard error, not a silent bug (see Section 2.2).

---

## 2. Binary Face-Port Protocol

### 2.1 Overview

The CUDA worker reads tile jobs from stdin and writes tile results to stdout, both as length-prefixed binary messages. Little-endian throughout. No compression.

The protocol is **synchronous and in-order**: tile results are emitted in the same order as tile jobs. The Rust reader relies on this for correct composition sequencing.

### 2.2 Connection Handshake

On startup, the worker writes a hello message to stdout:

```c
struct HelloMsg {            // 32 bytes
    uint8_t  magic[4];      // "GMHI" (Gaussian Moat Hello Init)
    uint16_t protocol_ver;  // 1
    uint16_t flags;          // reserved (0)
    uint64_t k_sq;           // will be set on first batch, 0 in hello
    char     build_id[16];   // null-terminated build timestamp
};
```

The Rust driver reads the hello, validates `magic` and `protocol_ver`, and optionally checks `build_id` against the expected binary version. This catches stale binaries from mismatched CMake/Cargo builds.

### 2.3 Job Message (Rust -> Worker, via stdin)

```c
struct BatchHeader {         // 16 bytes
    uint8_t  magic[4];      // "GMTJ" (Gaussian Moat Tile Jobs)
    uint32_t num_jobs;       // tiles in this batch
    uint64_t k_sq;           // step bound (verified against hello)
};

struct TileJob {             // 36 bytes
    uint32_t tile_id;        // sequential, globally unique in campaign
    int64_t  a_min;          // inclusive lower real bound
    int64_t  a_max;          // inclusive upper real bound
    int64_t  b_min;          // inclusive lower imaginary bound
    int64_t  b_max;          // inclusive upper imaginary bound
};
```

**Key change from v1:** Jobs carry exact inclusive bounds `{a_min, a_max, b_min, b_max}` as i64, not `{a_lo, side}` as i32. This matches the Rust `TileOperator` convention and correctly represents clipped edge tiles that are not square. The i64 type matches the Rust codebase end-to-end -- no narrowing.

### 2.4 Result Message (Worker -> Rust, via stdout)

```c
struct TileResult {          // 56 bytes
    uint32_t tile_id;        // matches input job
    int64_t  a_min;          // echo back bounds (for verification)
    int64_t  a_max;
    int64_t  b_min;
    int64_t  b_max;
    uint32_t num_components;
    uint32_t num_face_inner;
    uint32_t num_face_outer;
    uint32_t num_face_left;
    uint32_t num_face_right;
    uint32_t num_primes;     // total primes in expanded region (diagnostic)
    int32_t  origin_component; // -1 if origin not in this tile
};

struct FacePortRecord {      // 20 bytes
    int64_t  a;              // absolute real coordinate
    int64_t  b;              // absolute imaginary coordinate
    uint32_t component_id;   // tile-local component ID (0-indexed)
};
```

After each `TileResult`, face ports appear in order:
```
FacePortRecord[num_face_inner]
FacePortRecord[num_face_outer]
FacePortRecord[num_face_left]
FacePortRecord[num_face_right]
uint32_t component_sizes[num_components]   // size of each component (in-bounds primes)
```

**Port coordinates are absolute (a, b) in i64.** While relative offsets (da, db from tile corner) would save bytes, the absolute coordinates are what `compose.rs` needs for distance checks. The conversion would happen on every port read anyway. At ~2500 ports per tile and 20 bytes each, port payload is ~50 KB/tile. For 90K tiles that is ~4.5 GB of pipe throughput, well within sustained pipe bandwidth.

**Component sizes are included.** The composition pipeline in `moat-kernel` does not currently use `component_sizes`, but the Rust `TileOperator` carries them and `build_tile_from_primes` populates them. Including them costs 4 bytes per component (~200 components/tile = ~800 bytes/tile) and enables future optimizations (pruning small components during composition) without protocol changes.

### 2.5 Shutdown

The Rust driver signals end-of-work by closing stdin. The worker reads EOF, performs cleanup, and exits 0. Non-zero exit codes signal errors:
- 1: general error
- 2: CUDA OOM
- 3: kernel launch failure
- 4: protocol violation (bad magic, etc.)

### 2.6 Wire Invariants

The Rust reader validates on every tile result:

1. **Byte count**: total face port bytes = `(num_face_inner + num_face_outer + num_face_left + num_face_right) * 20`
2. **Ordered tile_id**: tile_ids arrive in strictly increasing order matching dispatch
3. **component_id < num_components**: every port's component_id is in range
4. **origin_component in range**: if >= 0, must be < num_components
5. **Port face correctness**: each inner port has `a - a_min <= collar`, each outer port has `a_max - a <= collar`, each left port has `b - b_min <= collar`, each right port has `b_max - b <= collar`
6. **Bounds echo**: echoed bounds match the job that was sent

Violations are hard errors that abort the campaign with diagnostic output. These are cheap checks that catch protocol drift immediately.

### 2.7 Rust Reader Strategy

**Primary: buffered read over `ChildStdout`.** Use `bytemuck` for zero-copy struct reinterpretation of fixed-size headers. Read face ports in bulk into `Vec<FacePortRecord>` then convert to `Vec<FacePort>`.

**Fallback: file mode.** For debugging, the worker can be invoked with `--input <file> --output <file>` to read/write files instead of pipes. Same protocol, different transport. Not used in production.

### 2.8 Payload Size Estimates

At R ~ 1e9, prime density ~ 1/ln(R) ~ 1/20.7. For k_sq=40, collar=7. Face zone area per face ~ side * collar. For a 2000x2000 tile:
- Face zone width = collar = 7
- Face zone length = side = 2000 (for inner/outer) or side = 2000 (for left/right)
- Points in face zone per face = 2000 * 7 = 14000
- Expected primes per face ~ 14000 / 20.7 ~ 676
- Corner overlaps: primes within collar of two edges. Corner area ~ collar^2 = 49, ~2.4 primes per corner, 4 corners
- Total unique port records ~ 4 * 676 - 4 * 2.4 ~ 2694, call it ~2700
- But per the critique, the face band is wider: primes at distance <= collar from the edge, not < collar. This adds one extra row, making it ~770 per face, ~3080 total. Use ~3000 as the estimate.

**Per-tile payload: 56 + 20 * 3000 + 4 * 200 = ~61 KB.**
**90K tiles: ~5.5 GB pipe throughput.** Sustained pipe bandwidth on Linux/macOS is well above 1 GB/s.

---

## 3. CUDA Batch Pipeline

### 3.1 Persistent Worker Lifecycle

```
fat_stripe_cuda [--device 0] [--batch-capacity N]

1. Init: cudaSetDevice, cudaMemGetInfo, init_row_sieve_tables()
2. Write HelloMsg to stdout
3. Read BatchHeader from stdin
   - On EOF: cleanup and exit 0
4. Validate batch (k_sq consistency, num_jobs > 0)
5. Read TileJob[num_jobs] from stdin
6. Process batch:
   a. Compute geometry per tile (collar, side_exp, etc.)
   b. Allocate/resize device buffers if needed
   c. Launch sieve+MR kernel (multi-tile)
   d. cudaDeviceSynchronize
   e. D2H transfer of bitmaps
   f. CPU-side UF + face-port extraction (parallel across tiles)
   g. Write TileResult + ports for each tile to stdout (in tile_id order)
7. Goto 3
```

The CUDA context, sieve tables, and device buffers persist across batches. Only `cudaMemset` (to zero bitmaps) and job-table upload are per-batch costs.

### 3.2 GPU Memory Management

**Per-tile working set:**
- Primality bitmap: `ceil((side_exp)^2 / 32) * 4` bytes.
- For a 2000x2000 tile with k_sq=40: side_exp = side+1+2*collar = 2001+14 = 2015, total_points = 2015^2 = 4,060,225, bitmap = 507,529 bytes ~ 508 KB.
- Non-square tiles have smaller bitmaps. The worker computes the maximum `side_exp` across jobs in a batch and allocates based on that.

**Batch sizing — host queue depth, not just VRAM:**

The dominant limiter is not device memory but host-side processing throughput: how fast the CPU can run UF on completed bitmaps and the pipe can drain results. Sizing by VRAM alone would create batches so large that the host UF backlog stalls the pipeline.

Approach: size batches so that host UF time for one batch roughly matches GPU sieve time for the next batch. This enables overlap in Phase 2 (double-buffering) and avoids GPU idle gaps in Phase 1.

**Conservative batch sizes (Phase 1, no overlap):**

| Platform | Usable VRAM | Bitmap/tile | CPU UF cores | Batch size (host-limited) |
|----------|-------------|-------------|--------------|---------------------------|
| Jetson 8GB | ~6 GB | 508 KB | 6 | 500 |
| 3090 24GB | ~22 GB | 508 KB | 16 | 2000 |
| 4090 24GB | ~22 GB | 508 KB | 16 | 2000 |
| A100 80GB | ~75 GB | 508 KB | 64 | 4000 |

These are starting points. The Rust orchestrator can auto-tune: if GPU finishes a batch before the previous batch's results are fully composed, increase batch size. If the pipe backs up, decrease.

**Allocation strategy:**
1. At startup: query `cudaMemGetInfo`, compute max batch capacity from free memory.
2. Clamp to host-limited batch size.
3. Allocate one contiguous device buffer: `d_bitmaps = cudaMalloc(batch_capacity * max_bitmap_bytes)`.
4. Allocate one contiguous host buffer (pinned): `h_bitmaps = cudaMallocHost(batch_capacity * max_bitmap_bytes)`.
5. Reuse across batches. Resize only if a later batch has tiles with larger `side_exp`.

### 3.3 Kernel Launch: Multi-Tile

**Option B (single launch, tile ID in grid):**

Grid = `num_tiles * max_side_exp` blocks. Block `b` processes tile `b / max_side_exp`, row `b % max_side_exp`. Each tile writes to its own bitmap slice.

```cuda
uint32_t tile_idx = blockIdx.x / max_side_exp;
uint32_t row      = blockIdx.x % max_side_exp;
if (tile_idx >= num_tiles) return;

// Per-tile geometry from job table
int64_t a_min = jobs[tile_idx].a_min;
int64_t b_min = jobs[tile_idx].b_min;
int64_t a_max = jobs[tile_idx].a_max;
int64_t b_max = jobs[tile_idx].b_max;
uint64_t nominal_a = (uint64_t)(a_max - a_min) + 1;
uint64_t nominal_b = (uint64_t)(b_max - b_min) + 1;
uint64_t side_exp_a = nominal_a + 2 * collar;
uint64_t side_exp_b = nominal_b + 2 * collar;
if (row >= side_exp_a) return;

uint32_t* bitmap = d_bitmaps + tile_idx * max_bitmap_words;
// ... row sieve for this row of this tile's expanded region
```

Non-square tiles naturally handled: `side_exp_a` and `side_exp_b` differ, and the kernel skips rows beyond `side_exp_a` via the early return.

**Grid/block dimensions:**
- Grid: `num_tiles * max_side_exp` blocks (e.g., 500 * 2015 = 1,007,500 blocks on Jetson). Within CUDA limits.
- Block: 256 threads (matches `kTileRowSieveBlockSize`).
- Shared memory: `row_sieve_shared_bytes(max_side_exp_b)` per block.

**Constant memory:** k_sq and sieve tables set once at first batch (they are the same for all tiles in a campaign). Job table copied to device global memory per batch.

### 3.4 D2H Transfer

Per-batch transfer after kernel completes:

```
cudaMemcpy(h_bitmaps, d_bitmaps, num_tiles * max_bitmap_bytes, cudaMemcpyDeviceToHost)
```

On Jetson: unified memory means cache-coherent access, but `cudaMemcpy` is still required to ensure coherence after kernel completion. It is NOT zero-cost -- the driver must invalidate CPU caches and potentially migrate pages. Budget 10-50ms for a 250 MB transfer on Jetson unified memory.

On discrete GPUs (PCIe 4.0 x16): ~25 GB/s. 500 tiles * 508 KB = 254 MB -> ~10 ms.

D2H is not the dominant bottleneck but is not free on any platform.

### 3.5 CPU-Side Post-Processing: Bitmap -> UF -> Face Ports

After D2H, the worker processes each tile's bitmap on the CPU. **This must mirror the Rust `tile.rs` algorithm exactly:**

1. **Sparse cell-hash UF** (not the dense bitmap UF from `tile_main.cu`). Build a hash map from cell coordinates to prime indices. Iterate primes, check neighbors via cell grid with 5x5 cell neighborhood. Union primes within distance k_sq.
2. **Origin handling**: if `a_min <= 0 && 0 <= a_max && b_min <= 0 && 0 <= b_max`, find all primes in the expanded region with norm <= k_sq and union them together. This is the `tile.rs:372-388` logic. The origin component can have zero face bits (e.g., if all origin-adjacent primes are deep in the tile interior).
3. **Face-port extraction**: iterate in-bounds primes, check `a - a_min <= collar` (inner), `a_max - a <= collar` (outer), `b - b_min <= collar` (left), `b_max - b <= collar` (right). Uses `<=` not `<` (matching tile.rs and the LaTeX definition).
4. **Component sizes**: count in-bounds primes per component.

**Why mirror tile.rs, not tile_main.cu:**
- `tile_main.cu` uses a dense bitmap-indexed UF over the full expanded grid. This is correct but has different component ID assignment order than the sparse approach.
- The Rust composition pipeline does not care about specific component IDs, but the parity test does. Using identical algorithms makes differential testing straightforward: same inputs must produce identical `TileOperator` structures (modulo component ID relabeling).
- In Phase 2, the GPU UF will be its own algorithm. Phase 1's job is to validate the protocol and integration, not to be fast.

**Threading:** OpenMP `#pragma omp parallel for` across tiles in a batch. Each tile's UF is independent.

### 3.6 Error Handling

- **OOM at cudaMalloc:** Detected at startup/resize. Reduce batch capacity automatically. If even 1 tile doesn't fit, write error to stderr and exit 2.
- **Kernel launch failure:** `cudaGetLastError()` after launch. Exit 3.
- **Protocol violation from stdin:** Bad magic or short read -> exit 4.
- **Output write failure:** Partial result is not valid. Exit 1.

The Rust driver detects non-zero exit or unexpected EOF on stdout and reports the failure with the last stderr output from the child.

---

## 4. Rust Integration

### 4.1 New Crate: `fat-stripe-cuda`

A new crate, not an extension of `fat-stripe`. Rationale:
- `fat-stripe` is CPU-only and remains a standalone reference.
- `fat-stripe-cuda` has different dependencies (no `rayon` for sieving, adds `bytemuck`, manages a child process).
- The two crates share `moat-kernel` for composition but have completely different tile production paths.

### 4.2 Driver: `driver.rs`

```rust
pub struct CudaDriver {
    child: Child,
    stdin: BufWriter<ChildStdin>,
    stdout: BufReader<ChildStdout>,
    build_id: String,
}

impl CudaDriver {
    /// Spawn persistent worker, read hello, validate.
    pub fn connect(binary_path: &Path, device: u32) -> Result<Self>;

    /// Send a batch of jobs, read all results in order.
    pub fn process_batch(&mut self, jobs: &[TileJob]) -> Result<Vec<(TileResult, TilePorts)>>;

    /// Close stdin, wait for clean exit.
    pub fn shutdown(self) -> Result<()>;
}
```

The driver owns the child process for its entire lifetime. No per-batch spawn. CUDA context creation, sieve table init, and device buffer allocation happen exactly once.

### 4.3 Protocol Reader: `protocol.rs`

```rust
#[repr(C)]
#[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
pub struct TileResultRaw {
    pub tile_id: u32,
    pub a_min: i64,
    pub a_max: i64,
    pub b_min: i64,
    pub b_max: i64,
    pub num_components: u32,
    pub num_face_inner: u32,
    pub num_face_outer: u32,
    pub num_face_left: u32,
    pub num_face_right: u32,
    pub num_primes: u32,
    pub origin_component: i32, // -1 if none
}

#[repr(C)]
#[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
pub struct FacePortRaw {
    pub a: i64,
    pub b: i64,
    pub component_id: u32,
}
```

Reader validates wire invariants on every tile (Section 2.6). Any violation is a hard error.

### 4.4 Constructing TileOperators

```rust
fn tile_from_raw(
    header: &TileResultRaw,
    ports: TilePorts,
    component_sizes: Vec<u32>,
) -> TileOperator {
    let collar = (header.k_sq as f64).sqrt().ceil() as i64; // for validation only
    let mut component_faces = vec![0u8; header.num_components as usize];

    for p in &ports.inner {
        component_faces[p.component] |= FACE_INNER_BIT;
    }
    for p in &ports.outer {
        component_faces[p.component] |= FACE_OUTER_BIT;
    }
    for p in &ports.left {
        component_faces[p.component] |= FACE_LEFT_BIT;
    }
    for p in &ports.right {
        component_faces[p.component] |= FACE_RIGHT_BIT;
    }

    TileOperator {
        a_min: header.a_min,
        a_max: header.a_max,
        b_min: header.b_min,
        b_max: header.b_max,
        face_inner: ports.inner,
        face_outer: ports.outer,
        face_left: ports.left,
        face_right: ports.right,
        num_components: header.num_components as usize,
        component_faces,
        component_sizes: component_sizes.iter().map(|&s| s).collect(),
        origin_component: if header.origin_component >= 0 {
            Some(header.origin_component as usize)
        } else {
            None
        },
        num_primes: header.num_primes as usize,
        detail: None,
    }
}
```

**No `side` in the conversion.** Bounds come directly from the echoed `a_min/a_max/b_min/b_max` -- no arithmetic, no off-by-one risk.

### 4.5 Orchestrator: `orchestrator.rs`

Mirrors `fat-stripe/src/orchestrator.rs` structure:

```
1. Connect to CUDA worker (driver.connect())
2. for each stripe (a_lo increments by tile_height):
     for each chunk (b_lo increments by chunk_size * tile_width):
       for each tile in chunk:
         add TileJob to batch buffer
       if batch full OR end of chunk:
         driver.process_batch(batch) -> results
         for each result in results (in order):
           construct TileOperator
           compose_horizontal into stripe_op
     compose_vertical stripe_op into composed_full
3. driver.shutdown()
4. Compute spanning verdict on composed_full
```

**Emission order**: tiles are dispatched in the same order as the CPU orchestrator's nested loop. Results arrive in the same order (protocol guarantees in-order). Composition is order-sensitive (`compose_horizontal` is non-commutative in port content), so this ordering is load-bearing.

### 4.6 Feeding Into Composition Pipeline

The composition pipeline (`compose_horizontal`, `compose_vertical` from `moat-kernel`) is unchanged. It operates on `TileOperator` regardless of provenance.

The spanning verdict logic is copied verbatim from the CPU orchestrator -- it operates on the final composed `TileOperator`.

---

## 5. C++ Implementation: What Changes

### 5.1 Reused As-Is

- `row_sieve.cuh`: sieve kernel, `tile_sieved_primality_bitmap_kernel`. No changes.
- `tile_kernel.cuh`: `is_gaussian_prime_point`, `gaussian_norm_u64`, `bitmap_word_count`, `copy_tile_k_sq`.
- `modular_arith.cuh`, `miller_rabin.cuh`, `cornacchia.cuh`: all reused.
- `init_row_sieve_tables()`: called once at worker startup.

### 5.2 New: `face_extract.cuh` (CPU-side UF + face-port extraction)

This is **not** a refactoring of `tile_main.cu`'s `classify_components()`. It is a new implementation that mirrors the Rust `tile.rs::build_tile_from_primes` algorithm:

1. **Prime enumeration**: scan the bitmap for set bits, converting grid indices back to absolute (a, b) coordinates. Build a prime list.
2. **Sparse cell-hash UF**: partition primes into spatial cells of size `collar`. For each prime, check the 5x5 cell neighborhood for neighbors within distance k_sq. Union connected primes.
3. **Origin handling**: if tile contains origin, find all primes with norm <= k_sq and union them. This matches `tile.rs:372-388`.
4. **Face classification and port emission**: iterate in-bounds primes (those with `a_min <= a <= a_max && b_min <= b <= b_max`), classify face membership using `<=` comparisons against collar distance, emit face port records.
5. **Component sizing**: count in-bounds primes per component.

**Why not refactor classify_components():**
- `classify_components()` uses dense UF over `total_points` entries (4M+ for a 2000-side tile). It checks all backward offsets for every grid cell, prime or not. It has no origin handling. It tracks face-component IDs but not port coordinates.
- Adapting it would require more changes than writing the sparse version from scratch, and the result would not be semantically identical to the Rust code.
- `tile_main.cu` remains unchanged and continues to serve as an independent single-tile tool.

### 5.3 New: `batch_dispatch.cuh`

```cpp
struct BatchContext {
    uint32_t* d_bitmaps;        // device: batch_capacity * max_bitmap_words
    uint32_t* h_bitmaps;        // host (pinned): same
    TileJob*  d_jobs;           // device: batch_capacity jobs
    uint32_t  batch_capacity;
    uint32_t  max_bitmap_words; // per tile
    uint64_t  max_side_exp_a;   // max expanded rows
    uint64_t  max_side_exp_b;   // max expanded cols
};

BatchContext create_batch_context(uint32_t batch_capacity, uint64_t max_side_exp_a, uint64_t max_side_exp_b);
void destroy_batch_context(BatchContext& ctx);
void launch_batch_sieve(const BatchContext& ctx, const TileJob* h_jobs, uint32_t num_tiles, int64_t collar);
void transfer_batch_bitmaps(const BatchContext& ctx, uint32_t num_tiles);
```

Non-square tile support: `side_exp_a` and `side_exp_b` can differ per tile. The batch allocates for the maximum across all tiles in the batch. The kernel skips out-of-bounds rows per tile.

### 5.4 New: `face_port_io.h`

Protocol struct definitions (matching Section 2) and helpers for reading from stdin / writing to stdout. Includes:
- `read_batch_header(FILE* in) -> BatchHeader`
- `read_tile_jobs(FILE* in, uint32_t n) -> vector<TileJob>`
- `write_tile_result(FILE* out, const TileResult& hdr, const FacePorts& ports, const vector<uint32_t>& sizes)`

---

## 6. Correctness Verification

### 6.1 Differential Testing: CUDA vs CPU

**Strategy:** Run both CPU `fat-stripe` and CUDA `fat-stripe-cuda` on identical tile specifications. Compare `TileOperator` output.

**What to compare:**
- `num_components` must match.
- Face port lists must contain the same set of `(a, b)` coordinates per face. Component IDs may differ (arbitrary labels), but the partition-into-components must be identical: two ports in the same component in CPU must be in the same component in CUDA, and vice versa.
- `origin_component` must flag the same tile and label the same component (verified via port membership).
- `component_sizes` must match (after canonical relabeling).

**Canonical component relabeling:** Both outputs are relabeled by sorting components by their minimum port coordinate `(a, b)` lexicographically. After relabeling, component IDs can be compared directly.

### 6.2 Test Tile Suite

**Minimum test set (expanded from v1's 4 tiles to cover all risky geometry):**

| # | Description | Geometry | Why |
|---|-------------|----------|-----|
| T1 | Origin tile, small | a=[-5,5], b=[-5,5], k_sq=2 | Origin handling, all 4 faces active |
| T2 | Origin tile, medium | a=[-10,10], b=[-10,10], k_sq=40 | Origin with larger collar |
| T3 | Axis tile (a-axis) | a=[100,300], b=[-3,3], k_sq=40 | Primes on/near real axis |
| T4 | Axis tile (b-axis) | a=[-3,3], b=[100,300], k_sq=40 | Primes on/near imaginary axis |
| T5 | Negative coordinates | a=[-300,-100], b=[-300,-100], k_sq=40 | Third quadrant |
| T6 | Campaign-scale | a=[1050000000,1050002000], b=[0,2000], k_sq=40 | Real production geometry |
| T7 | Near-diagonal | a=[742462000,742464000], b=[742462000,742464000], k_sq=40 | Off-axis at 45 degrees |
| T8 | Clipped edge tile (narrow) | a=[0,2000], b=[1990,2003], k_sq=40 | Non-square: width=13, height=2000 |
| T9 | Empty tile (no primes) | a=[10000,10001], b=[10000,10001], k_sq=2 | Tiny tile, likely no primes |
| T10 | Collar boundary exact | a=[0,14], b=[0,14], k_sq=40 | Tile size equals 2*collar, maximal face overlap |

### 6.3 Seam Parity Tests

Run small campaigns (e.g., 4x4 tile grid at r_min=100, r_max=300, k_sq=2) through both paths. Verify:
- Final `blocked` verdict matches.
- Total `num_primes` matches.
- For each tile in the grid, the face port sets match.
- After composition, the composed `TileOperator` has the same number of components and the same spanning analysis.

### 6.4 Randomized Property Tests

For N=100 randomly generated tile specs (random a_min, b_min in [-1000, 1000], random width/height in [10, 500], k_sq in {2, 4, 10, 26, 40}):
1. Run both CPU and CUDA paths.
2. Apply canonical component relabeling.
3. Assert face port sets identical.
4. Assert component structure identical.

This catches edge cases that hand-picked tiles miss: unusual aspect ratios, small tiles where collar exceeds tile dimension, tiles at various coordinates.

### 6.5 Protocol Invariant Validation

On every batch result in both test and production:
- Validate all wire invariants from Section 2.6.
- Log any anomalies to stderr (even if within tolerance).

### 6.6 Bitmap-Level Verification

For test tiles T1-T10: dump the GPU bitmap and compare bit-for-bit against CPU sieve output. This isolates primality testing from UF/face-port extraction.

---

## 7. Performance Model

### 7.1 Per-Tile Breakdown (2000x2000 tile, k_sq=40)

**GPU sieve + MR kernel (from existing tile_main.cu benchmarks):**
- Jetson Orin: ~40 ms/tile
- RTX 3090: ~3 ms/tile
- RTX 4090: ~2 ms/tile
- A100: ~4 ms/tile

In batch mode, tiles fill GPU occupancy. Expect near-linear scaling until saturated.

**D2H bitmap transfer (per batch):**
- 500 tiles * 508 KB = 254 MB
- Jetson unified memory: ~20-50 ms (cache invalidation + page migration, NOT 0 ms)
- Discrete PCIe 4.0 x16: 254 MB / 25 GB/s ~ 10 ms

**CPU UF + face extraction (Phase 1):**

The sparse cell-hash UF is slower than `tile_main.cu`'s dense UF per tile, but this is Phase 1. From Rust `build_tile_from_primes` benchmarks (which use the same algorithm):
- Jetson: ~300-400 ms/tile
- Desktop: ~70-100 ms/tile

With OpenMP parallelism across tiles:
- Jetson (6 cores): ~60 ms/tile amortized
- Desktop (16 cores): ~5-7 ms/tile amortized

**Rust composition:**
Per adjacent tile pair, composition performs |face_right| * |face_left| distance checks. With ~770 ports per face (corrected from v1's ~676), that's ~593K checks per pair.
- Per tile: ~1-2 ms
- 90K tiles: ~90-180 seconds total

### 7.2 End-to-End Estimates (Phase 1)

| Platform | GPU sieve | CPU UF (parallel) | D2H | Composition | Total |
|----------|-----------|--------------------|-----|-------------|-------|
| Jetson 6-core | ~3600 s (40ms * 90K) | ~5400 s (60ms * 90K) | ~4 s | 120 s | ~2.5 hours |
| 3090 16-core | ~270 s | ~540 s (6ms * 90K) | ~1 s | 120 s | ~16 min |
| 4090 16-core | ~180 s | ~540 s | ~1 s | 120 s | ~14 min |
| A100 64-core | ~360 s | ~140 s | ~1 s | 120 s | ~10 min |

**Important:** GPU sieve and CPU UF are **sequential** in Phase 1 (no overlap). The total is roughly the sum, not the max. On Jetson, CPU UF dominates. On desktop, they are roughly comparable.

CPU-only `fat-stripe` reference:
- Jetson: ~90K * 327 ms = ~8.2 hours
- Mac M4 Pro: ~90K * 76 ms = ~1.9 hours

**Phase 1 speedup: ~3.3x on Jetson, ~7-10x on desktop.** Modest but validates the pipeline. Phase 2 is where the real gains arrive.

### 7.3 Phase 2 Projections (for planning, NOT commitments)

Do not publish Phase 2 speedups until measured on real hardware. The estimates below are order-of-magnitude planning targets only.

GPU UF kernel time is unknown and depends on the algorithm (ECL-CC, hook-based, etc.) and the graph structure (sparse primes, variable connectivity). Budget 2-5x the sieve kernel time as a starting guess. Measure three curves separately:
1. Kernel ms/tile (sieve + UF combined)
2. D2H throughput (face ports only, ~50 KB/tile)
3. Host-side seam checks/second (composition rate)

### 7.4 Bottleneck Analysis

**Phase 1:** CPU UF is the bottleneck on Jetson. GPU sieve + CPU UF are roughly balanced on desktop. Composition is secondary but not negligible at ~120s.

**D2H transfer** is never the bottleneck in either phase.

---

## 8. Risk Register

| # | Risk | L | I | Mitigation |
|---|------|---|---|------------|
| R1 | CPU UF in Phase 1 too slow on Jetson | High | Med | Phase 1 is for correctness. Accept slow runs. Phase 2 is the fix. |
| R2 | Binary protocol mismatch (C++ writer vs Rust reader) | Med | High | Wire invariant checks on every tile (Section 2.6). Protocol version in handshake. Exhaustive differential tests. |
| R3 | Component ID relabeling bugs in composition | Med | High | Canonical relabeling in parity tests. Full campaign comparison (not just single tiles). |
| R4 | GPU bitmap corruption from batch kernel race | Low | High | No shared state between tiles (separate bitmap slices). Bitmap-level differential test. |
| R5 | Emission order mismatch under parallel extraction | Med | High | CPU extraction is parallelized but results are collected into a pre-allocated array indexed by tile_id. Stdout writes are serialized in tile_id order after all tiles in a batch complete. |
| R6 | Build skew (stale CUDA binary vs new Rust crate) | Med | Med | Build ID in hello handshake. Mismatch is a hard error. CI builds both from same commit. |
| R7 | int overflow in coordinate arithmetic | Low | High | All coordinates are i64 on the wire and in the C++ worker. No narrowing to i32 anywhere in the protocol or geometry code. The only i32 in the protocol is `origin_component` (which is a small index, not a coordinate). |
| R8 | Jetson thermal throttling on sustained batches | Med | Med | Monitor GPU temp via `tegrastats`. Cap batch rate if temp > 80C. Fan control script. |
| R9 | Pipe backpressure stalling GPU worker | Med | Low | Rust reader drains as fast as possible. BufReader with 64 KB buffer. If still too slow, increase Rust-side read buffer or add a dedicated reader thread. |
| R10 | Missing SM coverage (need sm_80/86/87/89) | Med | Med | CMake sets `CMAKE_CUDA_ARCHITECTURES` to include all target SMs. Tested on actual hardware before deployment. |
| R11 | Origin handling mismatch between C++ and Rust | Med | High | Origin logic is tested separately (tiles T1, T2). The C++ implementation is a direct port of `tile.rs:372-388`, not an approximation. |

### Fallback Paths

**If persistent worker is unstable:** Fall back to per-batch spawn. The protocol is the same -- just add handshake overhead per invocation. Slower but simpler to debug.

**If pipe throughput is insufficient at scale:** Use file mode for large campaigns. Same protocol, different transport.

**If batch kernel has occupancy issues:** Fall back to sequential per-tile kernel launches. Negligible launch overhead for 2000-block grids.

---

## 9. Implementation Phases

### Phase 1a: Persistent Worker + Protocol + Parity (MVP)

**Goal:** CUDA worker produces correct face-port stream; Rust reads it and produces correct campaign verdict. Identical results to CPU `fat-stripe` on all test cases.

**Scope:**
1. `face_port_io.h`: protocol structs (Section 2).
2. `face_extract.cuh`: sparse cell-hash UF + face-port extraction, mirroring `tile.rs`. Origin handling. **Tested separately** with a standalone C++ test harness before integration.
3. `fat_stripe_cuda.cu`: persistent worker. Reads jobs from stdin, launches multi-tile sieve kernel (sequential tile launches for simplicity), runs CPU UF per tile (OpenMP parallel), writes results to stdout.
4. `CMakeLists.txt`: add `fat_stripe_cuda` target with SM coverage and build ID.
5. `fat-stripe-cuda` Rust crate: `protocol.rs` reader + writer, `driver.rs` persistent child manager, `orchestrator.rs` campaign loop, `config.rs`, `main.rs` CLI.
6. Verification: differential tests on T1-T10 (Section 6.2). Wire invariant checks. Bitmap-level comparison for T1-T5.

**Acceptance gates:**
- All 10 test tiles produce identical `TileOperator` (after canonical relabeling) between CPU and CUDA paths.
- A 4x4 campaign grid produces identical verdict and component structure.
- Wire invariants pass on every tile.

**NOT in Phase 1a:** Multi-tile batch kernel (Option B), double-buffering, performance optimization.

**Estimated effort:** 2-3 sessions.

### Phase 1b: Overlapped GPU/CPU Pipeline

**Goal:** Batch sieve kernel (Option B, single launch) + overlap GPU batch N+1 with CPU UF batch N. 2-3 CUDA streams with events feeding a fixed CPU worker pool.

**Scope:**
1. Multi-tile batch kernel (Section 3.3).
2. Double-buffering: two sets of bitmap buffers, alternating between GPU sieve and CPU extraction.
3. Stream-based overlap: GPU stream 1 runs sieve while CPU processes previous batch.
4. Auto-tune batch size based on observed GPU/CPU balance.

**Acceptance gates:**
- All parity tests from Phase 1a still pass (no regressions from parallelism).
- Measurable speedup: GPU and CPU overlap reduces total time vs Phase 1a.

**Estimated effort:** 1-2 sessions.

### Phase 2: GPU UF

**Goal:** Move union-find to GPU. Eliminate CPU UF bottleneck. Only start after Phase 1 protocol and seam logic are stable.

**Prerequisites (must be settled before Phase 2 starts):**
- GPU UF algorithm selection (ECL-CC, hook-based, etc.)
- GPU component relabeling strategy
- GPU face-port scan and compaction kernel design
- GPU output buffer sizing (upper bound on face ports per tile)

**Scope:**
1. GPU connected-components kernel.
2. GPU face-port compaction kernel.
3. D2H transfer of face ports only (not full bitmaps).
4. Remove CPU UF from pipeline.

**Acceptance gates:**
- 100 random tiles at campaign scale match Phase 1 CPU reference face ports exactly.
- Measured kernel ms/tile, D2H throughput, and seam checks/s reported.

**Estimated effort:** 2-3 sessions.

### Phase 3: Full Octant Support + Scale

**Goal:** 500K+ tile campaigns.

**Scope:**
1. Checkpoint/restart.
2. Campaign telemetry.
3. Multi-GPU support.
4. Super-tile aggregation (if needed).

---

## Appendix A: Coordinate Convention Alignment

The fat-stripe orchestrator (`orchestrator.rs:41`) computes `a_hi = (a_lo + tile_height).min(a_end)`. It then calls `process_chunk(config, a_lo, a_hi, ...)`. Inside `chunk.rs:68`, tiles are built with `build_tile_from_primes(a_lo, a_hi - 1, tb_lo, tb_hi - 1, ...)`. So the inclusive range passed to `build_tile_from_primes` is `[a_lo, a_hi - 1]`.

The CUDA binary receives inclusive bounds directly: `a_min` and `a_max` on the wire correspond to the values passed to `build_tile_from_primes`. **No `side` field exists in the protocol.** The Rust orchestrator computes `a_max = a_hi - 1` and `b_max = tb_hi - 1` before writing the job, and the CUDA worker uses these directly.

The `tile_main.cu` convention (`a_hi = a_lo + side`, `nominal_extent = side + 1`) is irrelevant to the new protocol. `tile_main.cu` is unchanged and continues to use its own convention independently.

**Off-by-one is eliminated by design:** the wire carries the same values that `build_tile_from_primes` receives, with no arithmetic transformation.

## Appendix B: Why Persistent Subprocess, Not FFI or Files

**vs. per-batch file I/O (v1 design):**
- Per-batch spawn = repeated CUDA context creation (~100-500 ms) + sieve table init (~50 ms). For 180 batches on Jetson, that is 90+ seconds of pure overhead.
- Persistent worker amortizes init to once.
- Pipe throughput exceeds file I/O (no filesystem overhead, no disk sync).

**vs. FFI:**
- Linking CUDA into Rust requires CUDA toolkit on the Rust build machine. Current repo builds independently.
- Separate binary is independently testable and profileable.
- Migration path: if FFI becomes necessary, protocol structs become FFI struct definitions.

**vs. shared memory IPC:**
- Unnecessary complexity. Pipes provide sufficient bandwidth (~1-5 GB/s) for ~50 KB/tile payloads.

## Appendix C: Changes from v1

| # | Codex Point | v1 Claim | v2 Change | Rationale |
|---|------------|----------|-----------|-----------|
| 1 | Design Summary | File I/O + spawn once per batch, matching tile_main.cu | Persistent worker over stdin/stdout. Phase 1 mirrors tile.rs, not tile_main.cu. | Spawn overhead is real. Dense UF mismatch with Rust is a parity risk. |
| 2 | Module Structure | Square-only `side` | Jobs carry `{a_min, a_max, b_min, b_max}`. No global `tile_side`. | `chunk.rs:66-68` clips tiles at boundaries, producing non-square tiles. |
| 3 | Binary Protocol | i32 coords, `side` field, no component_sizes, underspecified invariants | i64 coords, inclusive bounds, component_sizes included, 6 wire invariants enforced. | i32 narrows the i64 codebase. component_sizes costs ~800 bytes/tile but enables future opts. Wire invariants catch protocol drift early. |
| 4 | Batch Sizing | VRAM-only, Jetson "D2H 0ms" | Size by host queue depth. Jetson unified memory is NOT zero-copy for cudaMemcpy. | Host UF backlog is the real limiter. Unified memory still has cache/page costs. |
| 5 | Kernel details | (No change needed for Phase 1) | Acknowledged as Phase 2 concern. Added to Phase 2 prerequisites. | Shared-memory atomics and bank conflicts are real but Phase 1 uses existing kernel unchanged. Phase 2 must profile ptxas before committing to an approach. |
| 6 | Rust integration lifecycle | Per-batch spawn | Persistent child process with streaming jobs/results. Strict in-order emission. | Eliminates repeated CUDA context creation. In-order emission required for correct composition. |
| 7 | Origin handling | Infer from face ports | Cannot infer. Mirror exact tile.rs:372-388 logic in C++. Test separately. | Valid origin_component can have zero face bits. Inference is impossible. |
| 8 | tile_main.cu refactoring | Refactor classify_components into shared header | New face_extract.cuh that mirrors tile.rs sparse algorithm. tile_main.cu unchanged. | Dense UF lacks origin handling, lacks port coordinates, has different component ID assignment. Writing tile.rs-equivalent from scratch is cleaner. |
| 9 | Verification | 4 golden tiles | 10 tiles covering edge cases + randomized property tests + seam parity tests. | 4 tiles miss: collar boundaries, empty tiles, axis tiles, negative coords, clipped tiles, origin tiles. |
| 10 | Performance model | Conflated GPU/CPU estimates, understated composition | Separated GPU sieve, CPU UF, D2H, and composition. Corrected port count to ~770/face. Phase 2 estimates are planning targets only, not commitments. | Honest accounting. |
| 11 | Risk register | Missing several real risks | Added: emission order, build skew, int overflow, Jetson thermal, SM coverage, origin mismatch. | These are first-class risks, not footnotes. |
| 12 | Phases | Phase 1 builds throwaway, Phase 2 skips dependencies | Phase 1a = parity harness (minimal). Phase 1b = overlapped pipeline. Phase 2 has explicit prerequisites. | Phase 1a builds only what Phase 2 keeps (protocol, Rust crate). No throwaway batch kernel until 1b. Phase 2 blocks on hard design decisions. |
| 13 | Coordinate conventions | `side` on wire, arithmetic to convert | Inclusive bounds on wire, no conversion. | Off-by-one eliminated by carrying the same values tile.rs receives. |
| 14 | FFI vs file | File I/O as primary | Pipe mode primary, file mode debugging fallback. | Real choice was persistent subprocess + pipes vs temp files. Pipes win on latency, FFI premature, files worst middle ground. |

**Points where v1 was actually right:**
- Protocol magic bytes and version fields (good practice, kept).
- `moat-kernel` composition unchanged (correct, the protocol boundary is at TileOperator).
- Separate crate for `fat-stripe-cuda` (correct isolation).
- Error code convention (kept, expanded).
- mmap mention removed from primary path (it was already marked as optional in v1).
