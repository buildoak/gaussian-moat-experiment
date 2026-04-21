---
date: 2026-03-26
engine: coordinator
status: draft
type: architecture
---

# fat-stripe-cuda: Architecture and Implementation Plan

## 0. Design Summary

A CUDA/C++ batch binary (`fat_stripe_cuda`) processes tiles on the GPU: sieve + Miller-Rabin -> primality bitmap -> CPU union-find -> face-port extraction. It writes a binary face-port stream to stdout or file. A Rust orchestrator (`fat-stripe-cuda` crate) reads that stream, constructs `TileOperator`s, and feeds them into the existing `moat-kernel` composition pipeline. Communication is file I/O, not FFI or IPC daemon.

GPU union-find (Option A from the pipeline architecture doc) is deferred to Phase 2. Phase 1 keeps UF on the CPU side of the CUDA binary, matching the proven `tile_main.cu` logic. This lets us validate the binary protocol and Rust integration before introducing a new GPU algorithm.

---

## 1. Module Structure

### 1.1 Repository Layout

```
gaussian-moat-cuda/
  src/
    tile_main.cu            # existing single-tile CUDA binary (unchanged)
    fat_stripe_cuda.cu      # NEW: batch tile processor, main()
    batch_dispatch.cuh      # NEW: multi-tile kernel launch + memory pool
    face_extract.cuh        # NEW: bitmap -> UF -> face ports (CPU-side, batched)
    face_port_io.h          # NEW: binary protocol structs + write helpers
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
          protocol.rs       # binary face-port reader (deserialize)
          driver.rs         # spawn CUDA binary, manage I/O
          orchestrator.rs   # campaign loop: batch -> read -> compose -> verdict
          config.rs         # CudaFatStripeConfig (superset of FatStripeConfig)
          main.rs           # CLI binary
      fat-stripe/           # existing CPU crate (unchanged, serves as reference)
      moat-kernel/          # existing (unchanged, reused for composition)
```

### 1.2 Build System

**CMake (CUDA/C++ side):**
Add to `CMakeLists.txt`:

```cmake
add_executable(fat_stripe_cuda
    src/fat_stripe_cuda.cu
)
target_include_directories(fat_stripe_cuda PRIVATE src/)
```

No new CUDA source files beyond `.cu`/`.cuh` headers. The batch dispatch and face extraction are header-only (`.cuh`/`.h`), following the existing convention.

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
memmap2 = "0.9"
```

**Connection:** Cargo does not invoke CMake. The CUDA binary is built separately (`cmake --build`). The Rust crate locates the CUDA binary via:
1. `FAT_STRIPE_CUDA_BIN` environment variable (explicit path)
2. `../../../build/fat_stripe_cuda` relative to crate root (development default)
3. `fat_stripe_cuda` on PATH (deployed)

---

## 2. Binary Face-Port Protocol

### 2.1 Overview

The CUDA binary reads a batch manifest from a file (or stdin), processes tiles on the GPU, and writes a binary face-port stream to stdout (or a file). Little-endian throughout. No compression -- the data is already compact.

### 2.2 Stream Header

```c
struct StreamHeader {       // 32 bytes
    uint8_t  magic[4];     // "GMFP" (Gaussian Moat Face Ports)
    uint16_t version;      // 1
    uint16_t flags;        // reserved (0)
    uint64_t k_sq;         // step bound, for verification
    uint32_t tile_side;    // nominal tile side (e.g. 2000)
    uint32_t num_tiles;    // total tiles in this stream
    uint64_t reserved;     // padding to 32 bytes
};
```

### 2.3 Per-Tile Record

```c
struct TileHeader {         // 40 bytes
    uint32_t tile_id;      // sequential, matches input order
    int32_t  a_lo;         // tile lower-left real coordinate
    int32_t  b_lo;         // tile lower-left imaginary coordinate
    uint32_t side;         // tile side length (usually == tile_side from stream header)
    uint32_t num_components;   // component count after UF
    uint32_t num_face_inner;   // port counts per face
    uint32_t num_face_outer;
    uint32_t num_face_left;
    uint32_t num_face_right;
    uint32_t num_primes;   // total primes in tile (diagnostic)
};

struct FacePortRecord {     // 12 bytes
    int32_t  a;            // absolute real coordinate
    int32_t  b;            // absolute imaginary coordinate
    uint32_t component_id; // tile-local component ID (0-indexed)
};
```

After each `TileHeader`, face ports appear in order: inner[num_face_inner], outer[num_face_outer], left[num_face_left], right[num_face_right]. No padding between sections -- records are naturally aligned at 4-byte boundaries.

### 2.4 Per-Tile Supplementary: Component Metadata

```c
struct ComponentRecord {    // 8 bytes
    uint32_t component_id;
    uint8_t  face_bits;    // FACE_INNER_BIT | FACE_OUTER_BIT | FACE_LEFT_BIT | FACE_RIGHT_BIT
    uint8_t  reserved[3];
};
```

After the face ports, `num_components` ComponentRecords follow. This carries `component_faces` which the Rust `TileOperator` needs but which cannot be inferred from face ports alone (a component can touch a face without having ports on it if all its primes in the face zone are interior to the tile and only connected to face-zone primes through collar paths).

Wait -- actually, by definition, face ports ARE the primes in the face zone. A component touches a face if and only if it has at least one face port on that face. So `component_faces` is derivable from the face port lists. The Rust reader can reconstruct it. Drop `ComponentRecord` from the protocol.

**Revised per-tile layout:**

```
TileHeader                          (40 bytes)
FacePortRecord[num_face_inner]      (12 * N_i bytes)
FacePortRecord[num_face_outer]      (12 * N_o bytes)
FacePortRecord[num_face_left]       (12 * N_l bytes)
FacePortRecord[num_face_right]      (12 * N_r bytes)
```

Total per tile: `40 + 12 * (N_i + N_o + N_l + N_r)` bytes.

At R ~ 1e9, prime density ~ 1/ln(R) ~ 1/20.7. Face zone area ~ side * collar = 2000 * 7 = 14000 points. Expected ports per face ~ 676. Four faces ~ 2700 ports, but with overlaps at corners (a prime can be on two faces). Estimate ~2500 unique port records.

**Per-tile payload: ~40 + 12 * 2500 = ~30 KB.**
**90K tiles: ~2.7 GB.** For 500K tiles: ~15 GB.

For the 90K-tile case (600K x 600K probe), this is manageable as a file. For 500K+ tiles, streaming (pipe or chunked file) is necessary.

### 2.5 Endianness and Alignment

- Little-endian (matches x86, ARM in LE mode, and CUDA device memory layout).
- All fields are naturally aligned (int32 at 4-byte, uint64 at 8-byte).
- Stream header is 32 bytes (8-byte aligned).
- TileHeader is 40 bytes (4-byte aligned, 8-byte aligned at start of record due to stream header).

### 2.6 Rust Reader Strategy

**Primary: buffered read.** `BufReader` over `File` or `ChildStdout`. Use `bytemuck` for zero-copy struct reinterpretation of the fixed-size headers. Read face ports in bulk into a `Vec<FacePortRecord>` then convert to `Vec<FacePort>`.

**Alternative: mmap.** Use `memmap2` for the file-backed case. Advantageous when multiple passes are needed (e.g., verification). The stream is sequential and append-only, so mmap is straightforward.

Decision: start with buffered read (simpler, works with pipes). Add mmap option behind a flag for large campaigns where random access to earlier tiles aids debugging.

---

## 3. CUDA Batch Pipeline

### 3.1 Input Format

The CUDA binary reads a job manifest -- a binary file listing tiles to process:

```c
struct JobHeader {          // 24 bytes
    uint8_t  magic[4];     // "GMTJ" (Gaussian Moat Tile Jobs)
    uint16_t version;      // 1
    uint16_t flags;        // reserved
    uint64_t k_sq;
    uint32_t tile_side;
    uint32_t num_jobs;
};

struct TileJob {            // 16 bytes
    uint32_t tile_id;
    int32_t  a_lo;
    int32_t  b_lo;
    uint32_t reserved;     // padding / future flags
};
```

CLI interface:

```
fat_stripe_cuda --jobs <input.bin> --output <output.bin> [--batch-size N] [--device 0]
```

With `--jobs -` and `--output -` for stdin/stdout pipe mode.

### 3.2 GPU Memory Management

**Per-tile working set:**
- Primality bitmap: `(side_exp)^2 / 32 * 4` bytes. For side=2000, k_sq=40: side_exp=2015, total_points=4,060,225, bitmap = ~508 KB.
- No GPU-side UF in Phase 1. Bitmap is the only per-tile GPU allocation.

**Batch sizing:**

| Platform | Usable VRAM | Bitmap/tile | Max tiles/batch | Conservative batch |
|----------|-------------|-------------|-----------------|-------------------|
| Jetson 8GB | ~6 GB | 508 KB | ~12,000 | 4,000 |
| 3090 24GB | ~22 GB | 508 KB | ~44,000 | 16,000 |
| 4090 24GB | ~22 GB | 508 KB | ~44,000 | 16,000 |
| A100 80GB | ~75 GB | 508 KB | ~150,000 | 60,000 |

At 90K tiles: Jetson needs ~23 batches, desktop GPUs need ~6 batches, A100 does it in 2.

**Allocation strategy:**
1. At startup: query `cudaMemGetInfo`, compute max batch size from free memory.
2. Allocate one contiguous device buffer: `d_bitmaps = cudaMalloc(batch_size * bitmap_bytes)`.
3. Allocate one contiguous host buffer (pinned): `h_bitmaps = cudaMallocHost(batch_size * bitmap_bytes)`.
4. Reuse across batches. No per-batch malloc/free.

### 3.3 Kernel Launch: Multi-Tile

The existing `tile_sieved_primality_bitmap_kernel` processes one tile: grid = `side_exp` blocks, one block per row. For multi-tile:

**Option A: Sequential tile launches.** Loop over tiles, launch one kernel per tile. Simple but forgoes inter-tile parallelism on the GPU. Kernel launch overhead: ~5-10 us per launch, negligible for 2000-block grids.

**Option B: Single launch, tile ID in grid.** Grid = `num_tiles_in_batch * side_exp` blocks. Block `b` processes tile `b / side_exp`, row `b % side_exp`. Each tile writes to its own bitmap slice. Shared memory is per-block (row sieve bitmap), unchanged.

Recommendation: **Option B** for Phase 1. The kernel needs minimal modification:

```cuda
// In fat_stripe_cuda batch kernel:
uint32_t tile_idx = blockIdx.x / side_exp;
uint32_t row      = blockIdx.x % side_exp;
if (tile_idx >= num_tiles || row >= side_exp) return;

int64_t a_lo = jobs[tile_idx].a_lo;
int64_t b_lo = jobs[tile_idx].b_lo;
uint32_t* bitmap = d_bitmaps + tile_idx * bitmap_words;
// ... rest identical to existing tile_sieved_primality_bitmap_kernel
```

Constant memory for k_sq and sieve tables: set once per batch (they are the same for all tiles in a campaign). Job table (`TileJob[]`) copied to device global memory.

**Grid/block dimensions:**
- Grid: `num_tiles_in_batch * side_exp` blocks (e.g., 4000 * 2015 = 8,060,000 blocks on Jetson). CUDA supports grids up to 2^31-1 blocks in x-dimension. Fine.
- Block: 256 threads (matches `kTileRowSieveBlockSize`).
- Shared memory: `row_sieve_shared_bytes(side_exp)` = 253 bytes. Shared memory does not scale with batch size.

### 3.4 D2H Transfer Strategy

**Per-batch transfer.** After the kernel completes for a batch, copy all bitmaps D2H in one `cudaMemcpy`:

```
cudaMemcpy(h_bitmaps, d_bitmaps, batch_size * bitmap_bytes, cudaMemcpyDeviceToHost)
```

For 4000 tiles on Jetson: 4000 * 508 KB = ~2 GB. On unified memory (Jetson), this is a zero-copy access -- no explicit transfer needed, just ensure GPU kernel completion.

For discrete GPUs (3090/4090/A100): PCIe bandwidth is ~25 GB/s (Gen4 x16). 2 GB transfer: ~80 ms. This is small relative to the kernel time for 4000 tiles.

**Double-buffering (Phase 2 optimization):** While GPU processes batch N+1, CPU processes bitmaps from batch N. Overlaps D2H with CPU UF. Not needed in Phase 1 where the pipeline is sequential.

### 3.5 CPU-Side Post-Processing: Bitmap -> UF -> Face Ports

After D2H, the CUDA binary processes each tile's bitmap on the CPU to produce face ports. This is the existing `classify_components()` logic from `tile_main.cu`, but:

1. Refactored into `face_extract.cuh` as a reusable function.
2. Extended to emit `FacePortRecord` structs instead of JSON.
3. Parallelized across tiles using OpenMP or `std::thread`.

**Threading model:**

Each tile's UF is independent. Use a thread pool (OpenMP `#pragma omp parallel for` or C++ `std::async`) to process tiles within a batch concurrently.

On Jetson (6 ARM cores): 6 tiles in parallel. Each tile UF takes ~200-800 ms (from existing benchmarks). 4000 tiles / 6 cores = ~667 serial iterations * ~500 ms = ~333 seconds. This is the dominant bottleneck.

On desktop (16+ cores): 4000 tiles / 16 cores = 250 iterations * ~100 ms = ~25 seconds.

This CPU UF bottleneck is why GPU UF (Phase 2) matters. But for Phase 1, the correctness infrastructure is more important than peak throughput.

**Face-port extraction:**

Modify `classify_components()` to:
1. Accept a pointer to the bitmap (not `std::vector`).
2. Return a `TileFacePorts` struct containing four `std::vector<FacePortRecord>` plus `num_components` and `num_primes`.
3. Include the full `(a, b, component_id)` coordinate data that the Rust compositor needs.

The existing `tile_main.cu` only tracks `component_faces` (bitmask), not individual face port coordinates. This is the main extension needed. The extraction loop in `classify_components()` already iterates over in-bounds primes and checks face membership -- it just needs to emit `(a, b, component)` instead of just `component_faces[component] |= face_bit`.

### 3.6 Error Handling

**MR failure:** Not possible. `is_prime()` is deterministic and cannot fail -- it returns true or false.

**OOM at cudaMalloc:** Detected at startup. If requested batch size exceeds available VRAM, reduce batch size automatically. Report to stderr. If even 1 tile doesn't fit, exit with error code 2.

**Kernel launch failure:** `cudaGetLastError()` after launch. If non-success, abort batch and exit with error code 3. The Rust driver detects nonzero exit and reports the failure.

**Bitmap corruption:** Not directly detectable. Correctness relies on verification tests (Section 6). In production, `num_primes` in the output serves as a sanity check -- the Rust side can compare against density expectations.

**Output write failure:** Check write() return values. Partial output is not valid -- if write fails mid-stream, exit with error code 4.

---

## 4. Rust Integration

### 4.1 New Crate: `fat-stripe-cuda`

A new crate, not an extension of `fat-stripe`. Rationale:
- `fat-stripe` is CPU-only and should remain a standalone reference implementation.
- `fat-stripe-cuda` has a different dependency profile (no `rayon` for sieving, adds `bytemuck`/`memmap2`, needs to spawn a binary).
- The two crates share the `moat-kernel` composition engine but have completely different tile production paths.

### 4.2 Orchestrator: `orchestrator.rs`

The orchestrator mirrors the structure of `fat-stripe/src/orchestrator.rs` but replaces the per-tile sieve+UF with CUDA binary invocation:

```
for each batch of tiles:
    1. Write TileJob manifest to temp file
    2. Spawn fat_stripe_cuda binary with --jobs and --output
    3. Wait for completion, check exit code
    4. Read binary face-port stream
    5. For each tile in batch:
       a. Construct TileOperator from face ports
       b. Feed into horizontal/vertical composition
    6. Advance composed_full
```

**Tile enumeration** follows the same nested loop as the CPU orchestrator:
- Outer loop: stripes (a_lo increments by tile_height)
- Inner loop: chunks (b_lo increments by chunk_size * tile_width)
- Within chunk: individual tiles (b_lo increments by tile_width)

Tiles are batched for CUDA in the order they are enumerated. The composition step processes them in the same order.

### 4.3 Reading the Face-Port Stream: `protocol.rs`

```rust
#[repr(C)]
#[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
pub struct StreamHeader {
    pub magic: [u8; 4],
    pub version: u16,
    pub flags: u16,
    pub k_sq: u64,
    pub tile_side: u32,
    pub num_tiles: u32,
    pub reserved: u64,
}

#[repr(C)]
#[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
pub struct TileHeaderRaw {
    pub tile_id: u32,
    pub a_lo: i32,
    pub b_lo: i32,
    pub side: u32,
    pub num_components: u32,
    pub num_face_inner: u32,
    pub num_face_outer: u32,
    pub num_face_left: u32,
    pub num_face_right: u32,
    pub num_primes: u32,
}

#[repr(C)]
#[derive(Copy, Clone, bytemuck::Pod, bytemuck::Zeroable)]
pub struct FacePortRaw {
    pub a: i32,
    pub b: i32,
    pub component_id: u32,
}
```

Reader function: `read_tile(reader: &mut impl Read) -> io::Result<TileOperator>`.

Conversion to `moat_kernel::tile::TileOperator`:
- `a_min = a_lo`, `a_max = a_lo + side` (inclusive -- matching the existing convention where `tile_main.cu` uses `geom.a_hi = a_lo + side`).
- `b_min = b_lo`, `b_max = b_lo + side`.
- Face ports: `FacePortRaw { a, b, component_id }` -> `FacePort { a: a as i64, b: b as i64, component: component_id as usize }`.
- `component_faces`: reconstructed by iterating face port lists and OR-ing face bits per component.
- `component_sizes`: not available from the binary protocol. Set to `Vec::new()` -- this matches what `compose_horizontal` and `compose_vertical` already produce (they set `component_sizes: Vec::new()`). The composition pipeline does not use `component_sizes`.
- `origin_component`: check if any face port or internal point satisfies `a^2 + b^2 <= k_sq`. In practice, only the tile containing the origin will have this. The CUDA binary can emit a flag for this, or the Rust reader can compute it from the tile coordinates. Decision: add an `origin_component` field to `TileHeader` as `int32_t` (-1 if none). This avoids the Rust side needing to know about origin proximity.

**Revised TileHeader (44 bytes):**

```c
struct TileHeader {         // 44 bytes
    uint32_t tile_id;
    int32_t  a_lo;
    int32_t  b_lo;
    uint32_t side;
    uint32_t num_components;
    uint32_t num_face_inner;
    uint32_t num_face_outer;
    uint32_t num_face_left;
    uint32_t num_face_right;
    uint32_t num_primes;
    int32_t  origin_component; // -1 if origin not in this tile
};
```

### 4.4 Constructing TileOperators

```rust
fn tile_from_raw(header: &TileHeaderRaw, ports: TilePorts) -> TileOperator {
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
        a_min: header.a_lo as i64,
        a_max: (header.a_lo + header.side as i32) as i64,
        b_min: header.b_lo as i64,
        b_max: (header.b_lo + header.side as i32) as i64,
        face_inner: ports.inner,
        face_outer: ports.outer,
        face_left: ports.left,
        face_right: ports.right,
        num_components: header.num_components as usize,
        component_faces,
        component_sizes: Vec::new(),
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

### 4.5 Feeding Into Composition Pipeline

The composition pipeline is unchanged. `compose_horizontal` and `compose_vertical` from `moat-kernel` work on `TileOperator` regardless of how it was produced.

The orchestrator accumulates the composed result identically to the CPU version:

```rust
// Within a stripe:
stripe_op = match stripe_op {
    None => tile_op,
    Some(left) => compose_horizontal(&left, &tile_op, k_sq),
};

// Across stripes:
composed_full = match composed_full {
    None => stripe,
    Some(bottom) => compose_vertical(&bottom, &stripe, k_sq),
};
```

The spanning verdict logic in the CPU orchestrator is copied verbatim -- it operates on the final composed `TileOperator` and is independent of how tiles were produced.

### 4.6 Driver: `driver.rs`

```rust
pub struct CudaDriver {
    binary_path: PathBuf,
    device: u32,
    batch_size: u32,
}

impl CudaDriver {
    /// Write job manifest, spawn CUDA binary, return path to output file.
    pub fn process_batch(&self, jobs: &[TileJob], output_path: &Path) -> Result<()> {
        // 1. Write jobs to temp file
        // 2. Spawn: fat_stripe_cuda --jobs <job_file> --output <output_path>
        //           --batch-size <self.batch_size> --device <self.device>
        // 3. Wait, check exit code
        // 4. Validate output file header
    }
}
```

---

## 5. tile_main.cu Refactoring

### 5.1 What Can Be Reused As-Is

- `row_sieve.cuh`: the entire sieve kernel, including `tile_sieved_primality_bitmap_kernel`. No changes needed.
- `tile_kernel.cuh`: `is_gaussian_prime_point`, `gaussian_norm_u64`, `abs_i64_to_u64`, `bitmap_word_count`, `copy_tile_k_sq`. All reused.
- `modular_arith.cuh`: all modular arithmetic. Reused.
- `miller_rabin.cuh`: `is_prime`. Reused.
- `cornacchia.cuh`: `fast_sqrt_neg1`. Reused.
- `init_row_sieve_tables()`: reused, called once at startup.

### 5.2 What Needs to Change

**`classify_components()` in `tile_main.cu`:**
Currently returns `TileResult` with face-component ID lists but not face port coordinates. Must be refactored to:
1. Extract as a standalone function in `face_extract.cuh`.
2. Return face port lists with `(a, b, component_id)`.
3. Track `origin_component`.

The refactored function signature:

```cpp
struct TileFacePorts {
    std::vector<FacePortRecord> face_inner;
    std::vector<FacePortRecord> face_outer;
    std::vector<FacePortRecord> face_left;
    std::vector<FacePortRecord> face_right;
    uint32_t num_components;
    uint32_t num_primes;
    int32_t origin_component; // -1 if none
};

TileFacePorts extract_face_ports(const TileGeometry& geom, const uint32_t* bitmap);
```

This is a restructuring of the existing `classify_components()`, not a rewrite. The UF, backward offsets, and face classification logic are identical.

**`tile_main.cu` itself:** Unchanged. It continues to work as a single-tile tool. The new `fat_stripe_cuda.cu` is a separate binary that uses the same kernel and a refactored extraction function.

### 5.3 New Batch Dispatch Logic: `batch_dispatch.cuh`

```cpp
struct BatchContext {
    uint32_t* d_bitmaps;        // device: batch_size * bitmap_words
    uint32_t* h_bitmaps;        // host (pinned): batch_size * bitmap_words
    TileJob*  d_jobs;           // device: batch_size jobs
    uint32_t  batch_capacity;   // max tiles per batch
    uint32_t  bitmap_words;     // words per tile bitmap
    uint64_t  side_exp;         // expanded side for current k_sq/side
};

// Allocate device + host buffers
BatchContext create_batch_context(uint32_t batch_capacity, uint64_t side_exp);

// Free all buffers
void destroy_batch_context(BatchContext& ctx);

// Launch sieve+MR kernel for a batch of tiles
void launch_batch_sieve(
    const BatchContext& ctx,
    const TileJob* h_jobs,
    uint32_t num_tiles,
    int64_t collar,
    uint64_t side_exp
);

// Copy bitmaps from device to host
void transfer_batch_bitmaps(const BatchContext& ctx, uint32_t num_tiles);
```

### 5.4 Memory Pool for Bitmaps

Single allocation at startup, sliced per tile:

```cpp
// Tile t's bitmap starts at:
uint32_t* tile_bitmap(BatchContext& ctx, uint32_t t) {
    return ctx.d_bitmaps + t * ctx.bitmap_words;
}
```

Initialized with `cudaMemset` before each batch launch (or kernel-side zeroing -- the existing kernel uses `cudaMemset` externally).

For Phase 1, allocate only bitmap memory on device (no UF arrays). Phase 2 adds `d_parent`, `d_rank` per tile.

---

## 6. Correctness Verification

### 6.1 Differential Testing: CUDA vs CPU

**Strategy:** Run both the CPU `fat-stripe` and the CUDA `fat-stripe-cuda` on the same tile specifications. Compare the resulting `TileOperator` face ports.

**What to compare:**
- `num_components` must match.
- Each face port list (inner, outer, left, right) must contain the same set of `(a, b)` coordinates. Component IDs may differ (they are arbitrary labels), but the partition-into-components must be identical: two ports in the same component in CPU output must be in the same component in CUDA output, and vice versa.
- `origin_component` must be set in the same tile and must label the same component (verified via port membership).

**Test harness:** A Rust integration test in `fat-stripe-cuda/tests/`:
1. Pick a tile spec (a_lo, b_lo, side, k_sq).
2. Run CPU path: `build_tile_from_primes(...)` from `moat-kernel`.
3. Run CUDA path: invoke `fat_stripe_cuda` binary, parse output.
4. Compare.

**Test tiles (ground truth):**
- Small tile at origin: `a_lo=0, b_lo=0, side=10, k_sq=2`. Known from existing `moat-kernel` unit tests.
- Medium tile: `a_lo=1000, b_lo=1000, side=200, k_sq=40`. Exercises sieve + MR at moderate coordinates.
- Large tile at campaign scale: `a_lo=1050000000, b_lo=0, side=2000, k_sq=40`. This is the actual campaign geometry.
- Corner tile near diagonal: `a_lo=742462000, b_lo=742462000, side=2000, k_sq=40`. Off-axis at 45 degrees.

### 6.2 End-to-End Composition Test

Run a small campaign (e.g., `r_min=1000, r_max=1100, k_sq=2, tile_width=50`) through both CPU and CUDA paths. The final `blocked` verdict and total `num_primes` must match.

### 6.3 Regression Test Suite

A script (`tests/verify_cuda_vs_cpu.sh`) that:
1. Builds both binaries.
2. Runs the test tiles above.
3. Compares outputs using the Rust differential test.
4. Exits nonzero on any mismatch.

Integrated into CI (if applicable) or run manually before each deployment.

### 6.4 Bitmap-Level Verification

For the sieve/MR kernel specifically: dump the GPU bitmap for a test tile and compare bit-for-bit against the CPU sieve output. This isolates GPU primality testing from the UF/face-port extraction.

---

## 7. Performance Model

### 7.1 Per-Tile Breakdown (side=2000, k_sq=40)

**GPU sieve + MR kernel:**
From existing benchmarks (`tile_main.cu` at side=2000):
- Jetson: ~40 ms per tile
- 3090: ~3 ms per tile
- 4090: ~2 ms per tile
- A100: ~4 ms per tile

In batch mode, tiles execute concurrently up to GPU occupancy limits. The kernel is compute-bound (MR dominates), so batch mode on large GPUs should achieve near-linear scaling until the GPU is saturated.

**D2H bitmap transfer:**
- Per tile: 508 KB
- Per batch of 4000 tiles: 2 GB
- Jetson (unified memory): 0 ms (no copy)
- 3090/4090 (PCIe 4.0 x16): 2 GB / 25 GB/s = 80 ms for 4000 tiles = 0.02 ms/tile
- A100 (PCIe 4.0 or NVLink): similar

D2H is not the bottleneck.

**CPU UF + face extraction:**
This is the dominant cost in Phase 1. From `tile_main.cu` benchmarks:
- `classify_components()` on side=2000: ~200-800 ms per tile on Jetson, ~50-150 ms on desktop.

With OpenMP parallelism across tiles:
- Jetson (6 cores): effective ~100 ms/tile amortized
- Desktop (16 cores): effective ~10-20 ms/tile amortized

**Rust composition:**
- Per tile: ~1 ms (compose_horizontal dominates, O(|face_right| * |face_left|) distance checks)
- For 90K tiles: ~90 seconds total

### 7.2 End-to-End Estimates

**Phase 1 (CPU UF, batch GPU sieve):**

| Platform | GPU sieve (90K tiles) | CPU UF (90K tiles) | Composition | Total |
|----------|-----------------------|--------------------|-------------|-------|
| Jetson 6-core | ~160 s (23 batches * 7s) | ~9000 s (100ms/tile * 6 cores) | 90 s | ~2.6 hours |
| 3090 16-core | ~20 s (6 batches * 3s) | ~560 s (10ms/tile * 16 cores) | 90 s | ~11 min |
| 4090 16-core | ~13 s | ~560 s | 90 s | ~11 min |
| A100 64-core | ~25 s | ~140 s (10ms/tile * 64 cores) | 90 s | ~4 min |

vs. CPU-only fat-stripe (from existing benchmarks):
- Jetson: ~90K tiles * 327 ms/tile = ~8.2 hours
- Mac M4 Pro: ~90K tiles * 76 ms/tile = ~1.9 hours

**Phase 1 speedup: ~3x on Jetson (GPU sieve eliminates sieve time, but CPU UF still dominates), ~10x on desktop (CPU UF parallelizes better with more cores).**

**Phase 2 (GPU UF, all on device):**

| Platform | GPU sieve+UF (90K tiles) | D2H face ports | Composition | Total |
|----------|--------------------------|----------------|-------------|-------|
| Jetson | ~700 s (70ms/tile in batch) | ~3 s | 90 s | ~13 min |
| 3090 | ~42 s (7ms/tile in batch) | ~3 s | 90 s | ~2.2 min |
| 4090 | ~30 s (5ms/tile in batch) | ~3 s | 90 s | ~2 min |
| A100 | ~48 s (8ms/tile in batch) | ~3 s | 90 s | ~2.4 min |

**Phase 2 speedup vs CPU-only: ~38x on Jetson, ~57x on desktop.**

### 7.3 Bottleneck Analysis

**Phase 1:** CPU UF is the bottleneck on all platforms. GPU sieve is fast but then waits for CPU processing. The pipeline is CPU-bound.

**Phase 2:** On Jetson, GPU compute is the bottleneck. On desktop GPUs, Rust composition becomes the bottleneck at ~90 seconds (fixed, not parallelized across tiles). Composition parallelization (e.g., tree reduction via `compose_grid`) could reduce this, but it's already fast enough for 90K tiles.

**D2H transfer** is never the bottleneck. In Phase 1, full bitmaps are transferred (~508 KB/tile) but this is dwarfed by UF time. In Phase 2, only face ports are transferred (~30 KB/tile), which is negligible.

**Memory bandwidth** on GPU: the sieve kernel reads/writes ~508 KB per tile. At 2015 blocks * 256 threads, each block touches ~253 bytes of shared memory (sieve bitmap) and ~508 KB of global memory. Memory bandwidth utilization is moderate -- the kernel is compute-bound on MR, not memory-bound.

---

## 8. Risk Register

| # | Risk | Likelihood | Impact | Mitigation |
|---|------|-----------|--------|-----------|
| R1 | CPU UF in Phase 1 is too slow on Jetson for large campaigns | High | Medium | Phase 1 is for correctness, not speed. Accept slow Jetson runs. Prioritize Phase 2 (GPU UF) once protocol is validated. |
| R2 | Binary protocol mismatch between C++ writer and Rust reader | Medium | High | Exhaustive differential tests (Section 6). Protocol version field enables graceful detection. Use `bytemuck` for type-safe deserialization. |
| R3 | Component ID remapping between CUDA and Rust introduces subtle composition bugs | Medium | High | Compare full composition results (not just individual tiles) against CPU reference. Property test: CUDA campaign verdict must match CPU campaign verdict on same geometry. |
| R4 | GPU bitmap corruption due to race conditions in batch kernel | Low | High | The existing kernel uses `atomicOr` which is safe for concurrent writes. Batch mode adds a tile-index offset but no new shared state between tiles. Bitmap-level differential test (Section 6.4) catches this. |
| R5 | Jetson unified memory causes unexpected performance degradation at large batch sizes | Medium | Low | Cap batch size conservatively (4000 tiles, not 12000). Profile with `nvprof` / `nsight` to detect thrashing. |
| R6 | Face-port stream exceeds available disk / pipe buffer for large campaigns | Low | Medium | 500K tiles * 30 KB = 15 GB. Manageable on disk. For pipe mode, ensure Rust reader drains fast enough. Add `--max-output-size` guard. |
| R7 | `tile_main.cu` refactoring breaks existing single-tile binary | Medium | Low | The refactoring extracts `classify_components` into a shared header but does not change `tile_main.cu`'s behavior. Regression test: existing `tile_main.cu` tests must still pass. |
| R8 | Origin component tracking incorrect for tiles not containing the origin | Low | Medium | For all tiles where `a_lo > 0 || b_lo > 0` (i.e., all campaign tiles except the origin tile), `origin_component = -1`. Only tiles containing `(0,0)` in their expanded region need origin logic. Test with a grid that includes the origin tile. |

### Fallback Paths

**If GPU UF (Phase 2) proves too complex or incorrect:** Stay on Phase 1 architecture indefinitely. CPU UF is correct and bounded. Use more CPU cores (or multiple CUDA binary instances feeding independent tiles) to parallelize.

**If the binary protocol approach is too slow due to disk I/O:** Switch to pipe mode (already designed). If pipe backpressure is an issue, switch to in-process FFI via `cudarc` or raw `libloading`. The protocol structs remain the same.

**If batch kernel has occupancy issues:** Fall back to sequential per-tile kernel launches (Option A from Section 3.3). Negligible launch overhead for 2000-block grids.

---

## 9. Implementation Phases

### Phase 1: MVP (Minimum Viable Pipeline)

**Goal:** CUDA binary produces correct face-port stream; Rust reads it and produces correct campaign verdict.

**Scope:**
1. `face_extract.cuh`: refactor `classify_components()` to emit face port records with coordinates. Single-tile, CPU-side.
2. `face_port_io.h`: protocol structs and write functions.
3. `fat_stripe_cuda.cu`: batch binary. Reads job manifest, launches multi-tile sieve kernel, runs CPU UF per tile (sequential or OpenMP), writes binary stream.
4. `CMakeLists.txt`: add `fat_stripe_cuda` target.
5. `fat-stripe-cuda` Rust crate: `protocol.rs` reader, `driver.rs` binary spawner, `orchestrator.rs` campaign loop, `config.rs`, `main.rs` CLI.
6. Verification: differential test on 4 ground-truth tiles (Section 6.1).

**Acceptance test:** Run a 10x10 tile grid campaign through both CPU `fat-stripe` and CUDA `fat-stripe-cuda`. Verdicts match. Face port sets are equivalent (modulo component ID relabeling).

**Estimated effort:** 2-3 sessions.

### Phase 2: GPU UF + Performance

**Goal:** Move union-find to GPU. Eliminate CPU UF bottleneck.

**Scope:**
1. GPU connected-components kernel (ECL-CC or hook-based). Operates on the primality bitmap, produces parent array.
2. GPU face-port compaction kernel: scans in-bounds primes, classifies faces, writes compacted face port records to device buffer.
3. D2H transfer of face ports only (not full bitmaps).
4. OpenMP CPU UF removed from the pipeline.
5. Double-buffering: GPU processes batch N+1 while Rust composes batch N.

**Acceptance test:** 100 random tiles at campaign scale match Phase 1 CPU reference face ports exactly.

**Estimated effort:** 2-3 sessions.

### Phase 3: Full Octant Support + Scale

**Goal:** Support 500K+ tile campaigns for full-octant coverage at R=1B.

**Scope:**
1. Streaming output: pipe mode or chunked file output to avoid 15 GB monolithic files.
2. Checkpoint/restart: the Rust orchestrator writes progress markers. On restart, skips completed batches by seeking in the output file.
3. Campaign telemetry: per-batch timing, GPU utilization, composition rate, ETA.
4. Super-tile aggregation (if needed): compose groups of K tiles into super-tiles, reducing the global UF size from 500K to 5K entries.
5. Multi-GPU support: partition tile batches across available GPUs.

**Acceptance test:** Complete a 500K-tile campaign on 4090 in under 1 hour (Phase 2 performance model predicts ~42 minutes). Verdict matches CPU reference on a sampled subset.

**Estimated effort:** 2-4 sessions (super-tile aggregation is the complex part).

---

## Appendix A: Coordinate Convention Alignment

The existing `tile_main.cu` uses an inclusive convention: `a_hi = a_lo + side`, meaning the tile covers `[a_lo, a_lo + side]` inclusive. This means `nominal_extent = side + 1` points per dimension. The Rust `fat-stripe` uses `a_lo..a_hi` where `a_hi` is exclusive in the outer loop but `build_tile_from_primes` receives `(a_min, a_max)` inclusive.

The binary protocol uses `(a_lo, side)` and the convention that the tile covers `[a_lo, a_lo + side]` inclusive, matching `tile_main.cu`. The Rust reader must set `a_max = a_lo + side` (not `a_lo + side - 1`).

**Critical alignment check:** The fat-stripe CPU orchestrator calls `build_tile_from_primes(a_lo, a_hi - 1, ...)` where `a_hi = a_lo + tile_height`. So the inclusive range is `[a_lo, a_lo + tile_height - 1]`. This means `side = tile_height - 1` in tile_main.cu convention, OR the Rust side adjusts by 1. This must be verified empirically in the Phase 1 differential test. The test will catch any off-by-one.

## Appendix B: Why Not FFI

FFI (via `cudarc`, `cc` crate, or `bindgen`) would eliminate process-spawn overhead and file I/O. However:

1. **Build complexity:** Linking CUDA into a Rust binary requires the CUDA toolkit on the Rust build machine. The current repo builds CUDA and Rust independently.
2. **Debugging:** A separate binary is independently testable, loggable, and profileable. GPU bugs are hard enough without Rust FFI layers in the stack trace.
3. **Deployment flexibility:** The CUDA binary can run on a remote GPU node while Rust runs locally. This matters for Jetson (build on host, deploy binary).
4. **Migration path:** If FFI becomes necessary for performance, the binary protocol structs become the FFI struct definitions. No wasted work.

The process-spawn overhead (~10 ms per invocation) is negligible when batches process thousands of tiles. File I/O for 30 KB/tile at SSD speeds (~3 GB/s) adds ~10 us/tile. Neither is a bottleneck.
