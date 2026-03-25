use fxhash::FxHashMap;

use crate::kernel::{TileKernel, TileResult};
use crate::primality::{is_gaussian_prime, sieve_row};
use crate::tile::{
    build_tile_from_primes, FacePort, FaceSet, TileOperator, FACE_INNER_BIT, FACE_LEFT_BIT,
    FACE_OUTER_BIT, FACE_RIGHT_BIT,
};

pub fn precompute_backward_offsets(k_sq: u64) -> Vec<(i32, i32)> {
    let collar = (k_sq as f64).sqrt().ceil() as i32;
    let mut offsets = Vec::new();

    for da in -collar..=0 {
        for db in -collar..=collar {
            if da > 0 || (da == 0 && db >= 0) {
                continue;
            }

            let da64 = i64::from(da);
            let db64 = i64::from(db);
            let dist_sq = (da64 * da64 + db64 * db64) as u64;
            if dist_sq <= k_sq {
                offsets.push((da, db));
            }
        }
    }

    offsets
}

// ---------------------------------------------------------------------------
// Dense union-find helpers (used by the dense path)
// ---------------------------------------------------------------------------

fn find(parent: &mut [u32], mut x: usize) -> usize {
    while parent[x] as usize != x {
        let next = parent[x] as usize;
        parent[x] = parent[next];
        x = next;
    }
    x
}

fn union(parent: &mut [u32], rank: &mut [u8], a: usize, b: usize) {
    let ra = find(parent, a);
    let rb = find(parent, b);
    if ra == rb {
        return;
    }

    if rank[ra] < rank[rb] {
        parent[ra] = rb as u32;
    } else if rank[ra] > rank[rb] {
        parent[rb] = ra as u32;
    } else {
        parent[rb] = ra as u32;
        rank[ra] += 1;
    }
}

fn component_id_for_root(
    root: usize,
    root_map: &mut FxHashMap<usize, usize>,
    component_faces: &mut Vec<FaceSet>,
) -> usize {
    if let Some(&component) = root_map.get(&root) {
        component
    } else {
        let component = component_faces.len();
        root_map.insert(root, component);
        component_faces.push(0);
        component
    }
}

// ---------------------------------------------------------------------------
// Sparse path: sieve rows → compact primes list → reuse build_tile_from_primes
// ---------------------------------------------------------------------------

/// Run the scanline sieve+MR to produce a compacted list of all Gaussian primes
/// in the expanded region [ea_lo..ea_hi] × [eb_lo..eb_hi].
///
/// This is the first half of the sparse UF path. The result is fed directly
/// into `build_tile_from_primes()` which handles UF + face-port extraction.
fn sieve_to_prime_list(
    ea_lo: i64,
    ea_hi: i64,
    eb_lo: i64,
    eb_hi: i64,
    use_row_sieve: bool,
) -> Vec<(i64, i64)> {
    let h = usize::try_from(ea_hi - ea_lo + 1).expect("expanded height must fit usize");
    let w = usize::try_from(eb_hi - eb_lo + 1).expect("expanded width must fit usize");

    let mut primes: Vec<(i64, i64)> = Vec::new();
    let mut row_sieve_marks = vec![false; w];

    for row in 0..h {
        let a = ea_lo + row as i64;
        if use_row_sieve {
            row_sieve_marks.fill(false);
            sieve_row(a, eb_lo, w, &mut row_sieve_marks);

            for (col, &marked_composite) in row_sieve_marks.iter().enumerate() {
                let b = eb_lo + col as i64;
                if marked_composite && a != 0 && b != 0 {
                    continue;
                }
                if is_gaussian_prime(a, b) {
                    primes.push((a, b));
                }
            }
        } else {
            for col in 0..w {
                let b = eb_lo + col as i64;
                if is_gaussian_prime(a, b) {
                    primes.push((a, b));
                }
            }
        }
    }

    primes
}

// ---------------------------------------------------------------------------
// Dense path (kept for benchmarking / regression)
// ---------------------------------------------------------------------------

fn run_scanline_rect_dense(
    a_lo: i64,
    a_hi: i64,
    b_lo: i64,
    b_hi: i64,
    k_sq: u64,
    use_row_sieve: bool,
) -> TileResult {
    let collar = (k_sq as f64).sqrt().ceil() as i64;
    let ea_lo = a_lo - collar;
    let ea_hi = a_hi + collar;
    let eb_lo = b_lo - collar;
    let eb_hi = b_hi + collar;

    let h = usize::try_from(ea_hi - ea_lo + 1).expect("expanded height must fit usize");
    let w = usize::try_from(eb_hi - eb_lo + 1).expect("expanded width must fit usize");
    let size = w.checked_mul(h).expect("scanline grid too large");

    let mut bitmap = vec![false; size];
    let mut parent: Vec<u32> = (0..size)
        .map(|idx| u32::try_from(idx).expect("scanline grid exceeds u32 address space"))
        .collect();
    let mut rank = vec![0_u8; size];
    let offsets = precompute_backward_offsets(k_sq);
    let mut num_primes = 0_usize;
    let mut row_sieve_marks = vec![false; w];

    for row in 0..h {
        let a = ea_lo + row as i64;
        if use_row_sieve {
            row_sieve_marks.fill(false);
            sieve_row(a, eb_lo, w, &mut row_sieve_marks);

            for (col, &marked_composite) in row_sieve_marks.iter().enumerate() {
                let b = eb_lo + col as i64;
                if marked_composite && a != 0 && b != 0 {
                    continue;
                }

                let idx = row * w + col;
                if is_gaussian_prime(a, b) {
                    bitmap[idx] = true;
                    num_primes += 1;
                }
            }
        } else {
            for col in 0..w {
                let b = eb_lo + col as i64;
                let idx = row * w + col;
                if is_gaussian_prime(a, b) {
                    bitmap[idx] = true;
                    num_primes += 1;
                }
            }
        }
    }

    for row in 0..h {
        for col in 0..w {
            let idx = row * w + col;
            if !bitmap[idx] {
                continue;
            }

            for &(da, db) in &offsets {
                let nr = row as i64 + i64::from(da);
                let nc = col as i64 + i64::from(db);
                if nr < 0 || nr >= h as i64 || nc < 0 || nc >= w as i64 {
                    continue;
                }

                let nidx = nr as usize * w + nc as usize;
                if bitmap[nidx] {
                    union(&mut parent, &mut rank, idx, nidx);
                }
            }
        }
    }

    let mut face_inner = Vec::new();
    let mut face_outer = Vec::new();
    let mut face_left = Vec::new();
    let mut face_right = Vec::new();
    let mut component_faces = Vec::new();
    let mut component_sizes = Vec::new();
    let mut root_map: FxHashMap<usize, usize> = FxHashMap::default();

    for row in 0..h {
        let a = ea_lo + row as i64;
        for col in 0..w {
            let idx = row * w + col;
            if !bitmap[idx] {
                continue;
            }

            let b = eb_lo + col as i64;
            if a < a_lo || a > a_hi || b < b_lo || b > b_hi {
                continue;
            }

            let root = find(&mut parent, idx);
            let component = component_id_for_root(root, &mut root_map, &mut component_faces);
            if component_sizes.len() < component_faces.len() {
                component_sizes.resize(component_faces.len(), 0);
            }
            component_sizes[component] += 1;

            if a - a_lo <= collar {
                face_inner.push(FacePort { a, b, component });
                component_faces[component] |= FACE_INNER_BIT;
            }
            if a_hi - a <= collar {
                face_outer.push(FacePort { a, b, component });
                component_faces[component] |= FACE_OUTER_BIT;
            }
            if b - b_lo <= collar {
                face_left.push(FacePort { a, b, component });
                component_faces[component] |= FACE_LEFT_BIT;
            }
            if b_hi - b <= collar {
                face_right.push(FacePort { a, b, component });
                component_faces[component] |= FACE_RIGHT_BIT;
            }
        }
    }

    let tile = TileOperator {
        a_min: a_lo,
        a_max: a_hi,
        b_min: b_lo,
        b_max: b_hi,
        face_inner,
        face_outer,
        face_left,
        face_right,
        num_components: component_faces.len(),
        component_faces,
        component_sizes,
        origin_component: None,
        num_primes,
        detail: None,
    };

    TileResult::from_tile_operator(&tile)
}

// ---------------------------------------------------------------------------
// Sparse path: compact primes → build_tile_from_primes (max code reuse)
// ---------------------------------------------------------------------------

fn run_scanline_rect_sparse(
    a_lo: i64,
    a_hi: i64,
    b_lo: i64,
    b_hi: i64,
    k_sq: u64,
    export_detail: bool,
    use_row_sieve: bool,
) -> TileResult {
    let collar = (k_sq as f64).sqrt().ceil() as i64;
    let ea_lo = a_lo - collar;
    let ea_hi = a_hi + collar;
    let eb_lo = b_lo - collar;
    let eb_hi = b_hi + collar;

    // Step 1: Sieve → compacted prime list (includes collar region).
    let primes = sieve_to_prime_list(ea_lo, ea_hi, eb_lo, eb_hi, use_row_sieve);

    // Step 2: Reuse build_tile_from_primes for UF + face-port extraction.
    // This function already runs SimpleUF over the compacted list and handles
    // all face-membership logic, component counting, and optional detail export.
    let tile = build_tile_from_primes(a_lo, a_hi, b_lo, b_hi, k_sq, primes, export_detail);

    TileResult::from_tile_operator(&tile)
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

fn run_scanline_rect(
    a_lo: i64,
    a_hi: i64,
    b_lo: i64,
    b_hi: i64,
    k_sq: u64,
    export_detail: bool,
    use_row_sieve: bool,
    use_sparse_uf: bool,
) -> TileResult {
    if use_sparse_uf {
        run_scanline_rect_sparse(a_lo, a_hi, b_lo, b_hi, k_sq, export_detail, use_row_sieve)
    } else {
        run_scanline_rect_dense(a_lo, a_hi, b_lo, b_hi, k_sq, use_row_sieve)
    }
}

pub fn run_scanline_tile(
    a_lo: i64,
    b_lo: i64,
    side: u32,
    k_sq: u64,
    export_detail: bool,
) -> TileResult {
    let a_hi = a_lo + i64::from(side);
    let b_hi = b_lo + i64::from(side);
    run_scanline_rect(a_lo, a_hi, b_lo, b_hi, k_sq, export_detail, true, true)
}

pub struct ScanlineKernel {
    pub export_detail: bool,
    pub use_row_sieve: bool,
    /// If true (default), use sparse UF over compacted primes via build_tile_from_primes.
    /// If false, use the original dense UF over the full h*w grid (kept for benchmarking).
    pub use_sparse_uf: bool,
}

impl ScanlineKernel {
    pub const fn new(export_detail: bool) -> Self {
        Self {
            export_detail,
            use_row_sieve: true,
            use_sparse_uf: true,
        }
    }
}

impl TileKernel for ScanlineKernel {
    fn run_tile(&self, a_lo: i64, a_hi: i64, b_lo: i64, b_hi: i64, k_sq: u64) -> TileResult {
        run_scanline_rect(
            a_lo,
            a_hi,
            b_lo,
            b_hi,
            k_sq,
            self.export_detail,
            self.use_row_sieve,
            self.use_sparse_uf,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::{
        precompute_backward_offsets, run_scanline_rect, run_scanline_tile, ScanlineKernel,
    };
    use crate::kernel::{CpuKernel, TileKernel};
    use crate::primality::sieve_row;
    use crate::primes::PrimeSieve;

    fn sieve_for_tile(a_hi: i64, b_hi: i64, k_sq: u64) -> crate::primes::PrimeSieve {
        let collar = (k_sq as f64).sqrt().ceil() as i64;
        let max_coord = (a_hi.abs() + collar).max(b_hi.abs() + collar) as u64;
        let max_norm = max_coord * max_coord * 2;
        PrimeSieve::new(max_norm.min(10_000_000).max(1000))
    }

    #[test]
    fn backward_offsets_k_sq_36_has_expected_count() {
        assert_eq!(precompute_backward_offsets(36).len(), 56);
    }

    fn brute_row_composite(a: i64, b: i64) -> bool {
        let norm = (i128::from(a) * i128::from(a) + i128::from(b) * i128::from(b)) as u64;
        if norm < 2 {
            return true;
        }
        if norm == 2 {
            return false;
        }

        let mut divisor = 2_u64;
        while divisor * divisor <= norm {
            if norm.is_multiple_of(divisor) {
                return true;
            }
            divisor += 1;
        }

        false
    }

    #[test]
    fn sieve_row_unit_test() {
        let width = 100;
        let mut marks = vec![false; width];
        sieve_row(5, 0, width, &mut marks);

        for (idx, &marked) in marks.iter().enumerate() {
            let b = idx as i64;
            assert_eq!(marked, brute_row_composite(5, b), "mismatch at b={b}");
        }
    }

    #[test]
    fn scanline_matches_legacy_counts_on_small_tile() {
        let legacy_sieve = sieve_for_tile(10, 10, 2);
        let legacy = CpuKernel::new(&legacy_sieve);
        let scanline = ScanlineKernel::new(false);

        let legacy_result = legacy.run_tile(0, 10, 0, 10, 2);
        let scanline_result = scanline.run_tile(0, 10, 0, 10, 2);

        assert_eq!(scanline_result.io_count, legacy_result.io_count);
        assert_eq!(scanline_result.il_count, legacy_result.il_count);
        assert_eq!(scanline_result.ir_count, legacy_result.ir_count);
        assert_eq!(scanline_result.ol_count, legacy_result.ol_count);
        assert_eq!(scanline_result.or_count, legacy_result.or_count);
        assert_eq!(scanline_result.lr_count, legacy_result.lr_count);
        assert_eq!(scanline_result.num_primes, legacy_result.num_primes);
    }

    /// Scanline vs legacy parity for component_sizes
    #[test]
    fn scanline_matches_legacy_component_sizes() {
        let legacy_sieve = sieve_for_tile(10, 10, 2);
        let legacy = CpuKernel::new(&legacy_sieve);
        let scanline = ScanlineKernel::new(false);

        let legacy_result = legacy.run_tile(0, 10, 0, 10, 2);
        let scanline_result = scanline.run_tile(0, 10, 0, 10, 2);

        let mut legacy_sizes = legacy_result.component_sizes.clone();
        legacy_sizes.sort_unstable();
        let mut scanline_sizes = scanline_result.component_sizes.clone();
        scanline_sizes.sort_unstable();

        assert_eq!(
            legacy_sizes, scanline_sizes,
            "Sorted component sizes must match between legacy and scanline kernels"
        );
        assert_eq!(legacy_result.num_components, scanline_result.num_components);
        assert_eq!(
            legacy_result.face_count_histogram,
            scanline_result.face_count_histogram
        );
    }

    #[test]
    fn square_wrapper_matches_trait_path() {
        let kernel = ScanlineKernel {
            export_detail: false,
            use_row_sieve: true,
            use_sparse_uf: true,
        };

        let via_wrapper = run_scanline_tile(0, 0, 10, 2, false);
        let via_trait = kernel.run_tile(0, 10, 0, 10, 2);

        assert_eq!(via_wrapper.io_count, via_trait.io_count);
        assert_eq!(via_wrapper.il_count, via_trait.il_count);
        assert_eq!(via_wrapper.ir_count, via_trait.ir_count);
        assert_eq!(via_wrapper.ol_count, via_trait.ol_count);
        assert_eq!(via_wrapper.or_count, via_trait.or_count);
        assert_eq!(via_wrapper.lr_count, via_trait.lr_count);
        assert_eq!(via_wrapper.num_primes, via_trait.num_primes);
    }

    #[test]
    fn row_sieve_cross_validation_k2() {
        let sieve_kernel = ScanlineKernel {
            export_detail: false,
            use_row_sieve: true,
            use_sparse_uf: true,
        };
        let pointwise_kernel = ScanlineKernel {
            export_detail: false,
            use_row_sieve: false,
            use_sparse_uf: true,
        };

        let sieve_result = sieve_kernel.run_tile(0, 12, 0, 12, 2);
        let pointwise_result = pointwise_kernel.run_tile(0, 12, 0, 12, 2);

        assert_eq!(sieve_result, pointwise_result);
    }

    #[test]
    fn row_sieve_cross_validation_k26() {
        let sieve_kernel = ScanlineKernel {
            export_detail: false,
            use_row_sieve: true,
            use_sparse_uf: true,
        };
        let pointwise_kernel = ScanlineKernel {
            export_detail: false,
            use_row_sieve: false,
            use_sparse_uf: true,
        };

        let sieve_result = sieve_kernel.run_tile(0, 24, 0, 24, 26);
        let pointwise_result = pointwise_kernel.run_tile(0, 24, 0, 24, 26);

        assert_eq!(sieve_result, pointwise_result);
    }

    // ---------------------------------------------------------------------------
    // Sparse vs dense equivalence tests
    // ---------------------------------------------------------------------------

    /// Helper: compare two TileResults for structural equality.
    /// Component IDs may differ between paths, so we compare sorted component_sizes
    /// and face-count histograms rather than raw IDs.
    fn assert_results_structurally_equal(
        sparse: &crate::kernel::TileResult,
        dense: &crate::kernel::TileResult,
        label: &str,
    ) {
        assert_eq!(
            sparse.num_primes, dense.num_primes,
            "{label}: num_primes mismatch: sparse={} dense={}",
            sparse.num_primes, dense.num_primes
        );
        assert_eq!(
            sparse.num_components, dense.num_components,
            "{label}: num_components mismatch: sparse={} dense={}",
            sparse.num_components, dense.num_components
        );

        let mut sparse_sizes = sparse.component_sizes.clone();
        sparse_sizes.sort_unstable();
        let mut dense_sizes = dense.component_sizes.clone();
        dense_sizes.sort_unstable();
        assert_eq!(
            sparse_sizes, dense_sizes,
            "{label}: sorted component_sizes mismatch"
        );

        assert_eq!(
            sparse.face_count_histogram, dense.face_count_histogram,
            "{label}: face_count_histogram mismatch"
        );

        // Cross-face component counts must match exactly.
        assert_eq!(
            sparse.io_count, dense.io_count,
            "{label}: io_count mismatch"
        );
        assert_eq!(
            sparse.il_count, dense.il_count,
            "{label}: il_count mismatch"
        );
        assert_eq!(
            sparse.ir_count, dense.ir_count,
            "{label}: ir_count mismatch"
        );
        assert_eq!(
            sparse.ol_count, dense.ol_count,
            "{label}: ol_count mismatch"
        );
        assert_eq!(
            sparse.or_count, dense.or_count,
            "{label}: or_count mismatch"
        );
        assert_eq!(
            sparse.lr_count, dense.lr_count,
            "{label}: lr_count mismatch"
        );

        // Per-face port counts (number of ports per face, not IDs).
        assert_eq!(
            sparse.i_face_components.len(),
            dense.i_face_components.len(),
            "{label}: i_face component count mismatch"
        );
        assert_eq!(
            sparse.o_face_components.len(),
            dense.o_face_components.len(),
            "{label}: o_face component count mismatch"
        );
        assert_eq!(
            sparse.l_face_components.len(),
            dense.l_face_components.len(),
            "{label}: l_face component count mismatch"
        );
        assert_eq!(
            sparse.r_face_components.len(),
            dense.r_face_components.len(),
            "{label}: r_face component count mismatch"
        );
    }

    /// Equivalence test at small scale: k²=2, side=50.
    /// Verifies that the sparse and dense paths produce structurally identical results.
    #[test]
    fn sparse_dense_equivalence_k2_small() {
        let a_lo = 10_i64;
        let b_lo = 10_i64;
        let a_hi = a_lo + 50;
        let b_hi = b_lo + 50;
        let k_sq = 2_u64;

        let sparse = run_scanline_rect(a_lo, a_hi, b_lo, b_hi, k_sq, false, true, true);
        let dense = run_scanline_rect(a_lo, a_hi, b_lo, b_hi, k_sq, false, true, false);

        assert_results_structurally_equal(&sparse, &dense, "k2_small(10,10,side=50)");
    }

    /// Equivalence test at larger scale: k²=40, side=200.
    /// More primes, more components, exercises both paths with non-trivial connectivity.
    #[test]
    fn sparse_dense_equivalence_k40_medium() {
        let a_lo = 1000_i64;
        let b_lo = 1000_i64;
        let a_hi = a_lo + 200;
        let b_hi = b_lo + 200;
        let k_sq = 40_u64;

        let sparse = run_scanline_rect(a_lo, a_hi, b_lo, b_hi, k_sq, false, true, true);
        let dense = run_scanline_rect(a_lo, a_hi, b_lo, b_hi, k_sq, false, true, false);

        assert_results_structurally_equal(&sparse, &dense, "k40_medium(1000,1000,side=200)");
    }

    /// Timing benchmark: k²=40, side=2000.
    /// Prints wall-clock times to stderr so we can observe the sparse speedup.
    /// Run with: cargo test -p moat-kernel sparse_dense_timing -- --nocapture --ignored
    #[test]
    #[ignore]
    fn sparse_dense_timing_k40_large() {
        use std::time::Instant;

        let a_lo = 100_000_i64;
        let b_lo = 100_000_i64;
        let a_hi = a_lo + 2000;
        let b_hi = b_lo + 2000;
        let k_sq = 40_u64;

        eprintln!(
            "\n[timing] k²={k_sq}, region=[{a_lo},{a_hi}]×[{b_lo},{b_hi}]"
        );

        let t0 = Instant::now();
        let sparse = run_scanline_rect(a_lo, a_hi, b_lo, b_hi, k_sq, false, true, true);
        let sparse_elapsed = t0.elapsed();
        eprintln!(
            "[timing] sparse UF: {:?}  (primes={}, components={})",
            sparse_elapsed, sparse.num_primes, sparse.num_components
        );

        let t1 = Instant::now();
        let dense = run_scanline_rect(a_lo, a_hi, b_lo, b_hi, k_sq, false, true, false);
        let dense_elapsed = t1.elapsed();
        eprintln!(
            "[timing] dense  UF: {:?}  (primes={}, components={})",
            dense_elapsed, dense.num_primes, dense.num_components
        );

        let speedup = dense_elapsed.as_secs_f64() / sparse_elapsed.as_secs_f64();
        eprintln!("[timing] speedup: {speedup:.1}x");

        // Structural equivalence check even in the benchmark.
        assert_results_structurally_equal(&sparse, &dense, "k40_large_timing");
    }
}
