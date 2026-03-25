use fxhash::FxHashMap;

use crate::kernel::{TileKernel, TileResult};
use crate::primality::{is_gaussian_prime, sieve_row, SieveTable, DEFAULT_SIEVE_TABLE};
use crate::tile::{
    build_tile_from_primes, FacePort, FaceSet, SimpleUF, TileOperator, FACE_INNER_BIT,
    FACE_LEFT_BIT, FACE_OUTER_BIT, FACE_RIGHT_BIT,
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
/// `table` is shared across all rows — build it once per tile call.
fn sieve_to_prime_list(
    ea_lo: i64,
    ea_hi: i64,
    eb_lo: i64,
    eb_hi: i64,
    use_row_sieve: bool,
    table: &SieveTable,
) -> Vec<(i64, i64)> {
    let h = usize::try_from(ea_hi - ea_lo + 1).expect("expanded height must fit usize");
    let w = usize::try_from(eb_hi - eb_lo + 1).expect("expanded width must fit usize");

    let mut primes: Vec<(i64, i64)> = Vec::new();
    let mut row_sieve_marks = vec![false; w];

    for row in 0..h {
        let a = ea_lo + row as i64;
        if use_row_sieve {
            row_sieve_marks.fill(false);
            sieve_row(a, eb_lo, w, table, &mut row_sieve_marks);

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
    table: &SieveTable,
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
            sieve_row(a, eb_lo, w, table, &mut row_sieve_marks);

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
    table: &SieveTable,
) -> TileResult {
    let collar = (k_sq as f64).sqrt().ceil() as i64;
    let ea_lo = a_lo - collar;
    let ea_hi = a_hi + collar;
    let eb_lo = b_lo - collar;
    let eb_hi = b_hi + collar;

    // Step 1: Sieve → compacted prime list (includes collar region).
    let primes = sieve_to_prime_list(ea_lo, ea_hi, eb_lo, eb_hi, use_row_sieve, table);

    // Step 2: Reuse build_tile_from_primes for UF + face-port extraction.
    let tile = build_tile_from_primes(a_lo, a_hi, b_lo, b_hi, k_sq, primes, export_detail);

    TileResult::from_tile_operator(&tile)
}

// ---------------------------------------------------------------------------
// Bitmap + rank path: O(1) neighbor lookup, O(1) prime-to-index mapping
// ---------------------------------------------------------------------------

/// Pack a row-major bool bitmap into u64 words (1 bit per point).
#[inline]
fn pack_bitmap(bool_bitmap: &[bool]) -> Vec<u64> {
    let total = bool_bitmap.len();
    let num_words = (total + 63) / 64;
    let mut packed = vec![0u64; num_words];
    for i in 0..total {
        if bool_bitmap[i] {
            packed[i / 64] |= 1u64 << (i % 64);
        }
    }
    packed
}

/// Build cumulative popcount table. rank_table[i] = total set bits in bitmap[0..=i].
#[inline]
fn build_rank_table(bitmap: &[u64]) -> Vec<u32> {
    let mut rank = Vec::with_capacity(bitmap.len());
    let mut cumulative = 0u32;
    for &word in bitmap {
        cumulative += word.count_ones();
        rank.push(cumulative);
    }
    rank
}

/// Test whether the point at (row, col) is set in the packed bitmap.
#[inline(always)]
fn bitmap_test(bitmap: &[u64], w: usize, row: usize, col: usize) -> bool {
    let bit_idx = row * w + col;
    let word = bitmap[bit_idx / 64];
    (word >> (bit_idx % 64)) & 1 == 1
}

/// Get the rank (0-based index among set bits) for a known-set bit at (row, col).
/// Caller must ensure the bit is actually set.
#[inline(always)]
fn rank_query(bitmap: &[u64], rank_table: &[u32], w: usize, row: usize, col: usize) -> u32 {
    let bit_idx = row * w + col;
    let word_idx = bit_idx / 64;
    let bit_pos = bit_idx % 64;
    // Cumulative count up to (but not including) this word
    let base = if word_idx > 0 {
        rank_table[word_idx - 1]
    } else {
        0
    };
    // Count bits in current word strictly below bit_pos
    let mask = if bit_pos > 0 {
        (1u64 << bit_pos) - 1
    } else {
        0
    };
    base + (bitmap[word_idx] & mask).count_ones()
}

fn run_scanline_rect_bitmap_rank(
    a_lo: i64,
    a_hi: i64,
    b_lo: i64,
    b_hi: i64,
    k_sq: u64,
    use_row_sieve: bool,
    table: &SieveTable,
) -> TileResult {
    let collar = (k_sq as f64).sqrt().ceil() as i64;
    let ea_lo = a_lo - collar;
    let ea_hi = a_hi + collar;
    let eb_lo = b_lo - collar;
    let eb_hi = b_hi + collar;

    let h = usize::try_from(ea_hi - ea_lo + 1).expect("expanded height must fit usize");
    let w = usize::try_from(eb_hi - eb_lo + 1).expect("expanded width must fit usize");

    // Phase 1: Sieve to bool bitmap (same as dense path), then pack + rank.
    let mut bool_bitmap = vec![false; h * w];
    let mut row_sieve_marks = vec![false; w];

    for row in 0..h {
        let a = ea_lo + row as i64;
        if use_row_sieve {
            row_sieve_marks.fill(false);
            sieve_row(a, eb_lo, w, table, &mut row_sieve_marks);
            for (col, &marked_composite) in row_sieve_marks.iter().enumerate() {
                let b = eb_lo + col as i64;
                if marked_composite && a != 0 && b != 0 {
                    continue;
                }
                if is_gaussian_prime(a, b) {
                    bool_bitmap[row * w + col] = true;
                }
            }
        } else {
            for col in 0..w {
                let b = eb_lo + col as i64;
                if is_gaussian_prime(a, b) {
                    bool_bitmap[row * w + col] = true;
                }
            }
        }
    }

    let bitmap = pack_bitmap(&bool_bitmap);
    let rank_table = build_rank_table(&bitmap);

    // Phase 2: Build prime_list in row-major order (matching rank ordering).
    let mut prime_list: Vec<(i64, i64)> = Vec::new();
    for row in 0..h {
        for col in 0..w {
            if bool_bitmap[row * w + col] {
                let a = ea_lo + row as i64;
                let b = eb_lo + col as i64;
                prime_list.push((a, b));
            }
        }
    }
    let num_primes = prime_list.len();

    // Phase 3: Union-Find with bitmap_test + rank_query for neighbor resolution.
    let offsets = precompute_backward_offsets(k_sq);
    let mut uf = SimpleUF::new(num_primes);

    for (prime_idx, &(a, b)) in prime_list.iter().enumerate() {
        for &(da, db) in &offsets {
            let na = a + i64::from(da);
            let nb = b + i64::from(db);
            // Bounds check against expanded region
            if na < ea_lo || na > ea_hi || nb < eb_lo || nb > eb_hi {
                continue;
            }
            let row = (na - ea_lo) as usize;
            let col = (nb - eb_lo) as usize;
            // Neighbor exists?
            if !bitmap_test(&bitmap, w, row, col) {
                continue;
            }
            // Neighbor's index via rank query
            let neighbor_idx = rank_query(&bitmap, &rank_table, w, row, col) as usize;
            uf.union(prime_idx, neighbor_idx);
        }
    }

    // Phase 4: Extract face ports, component_faces, component_sizes.
    // Only count primes within the non-expanded tile region [a_lo..a_hi] x [b_lo..b_hi].
    let mut face_inner = Vec::new();
    let mut face_outer = Vec::new();
    let mut face_left = Vec::new();
    let mut face_right = Vec::new();
    let mut component_faces: Vec<FaceSet> = Vec::new();
    let mut component_sizes: Vec<u32> = Vec::new();
    let mut root_map: FxHashMap<usize, usize> = FxHashMap::default();

    for (idx, &(a, b)) in prime_list.iter().enumerate() {
        // Skip primes outside the non-expanded tile
        if a < a_lo || a > a_hi || b < b_lo || b > b_hi {
            continue;
        }

        let root = uf.find(idx);
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
// Public entry points
// ---------------------------------------------------------------------------

/// Which union-find strategy to use for the scanline kernel.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UfStrategy {
    /// Dense UF over the full h*w expanded grid (original path, kept for benchmarking).
    Dense,
    /// Sparse UF via HashMap cell grid (build_tile_from_primes). Slow due to hash probes.
    SparseHashMap,
    /// Bitmap + popcount rank table: O(1) neighbor test, O(1) prime-to-index. Default.
    BitmapRank,
}

fn run_scanline_rect(
    a_lo: i64,
    a_hi: i64,
    b_lo: i64,
    b_hi: i64,
    k_sq: u64,
    export_detail: bool,
    use_row_sieve: bool,
    uf_strategy: UfStrategy,
) -> TileResult {
    // Build the SieveTable once per tile call; share across all rows.
    let table = &*DEFAULT_SIEVE_TABLE;

    match uf_strategy {
        UfStrategy::Dense => {
            run_scanline_rect_dense(a_lo, a_hi, b_lo, b_hi, k_sq, use_row_sieve, table)
        }
        UfStrategy::SparseHashMap => {
            run_scanline_rect_sparse(
                a_lo, a_hi, b_lo, b_hi, k_sq, export_detail, use_row_sieve, table,
            )
        }
        UfStrategy::BitmapRank => {
            run_scanline_rect_bitmap_rank(a_lo, a_hi, b_lo, b_hi, k_sq, use_row_sieve, table)
        }
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
    run_scanline_rect(
        a_lo,
        a_hi,
        b_lo,
        b_hi,
        k_sq,
        export_detail,
        true,
        UfStrategy::BitmapRank,
    )
}

pub struct ScanlineKernel {
    pub export_detail: bool,
    pub use_row_sieve: bool,
    /// If true (default), use sparse UF over compacted primes via build_tile_from_primes.
    /// If false, use the original dense UF over the full h*w grid (kept for benchmarking).
    pub use_sparse_uf: bool,
    /// Union-find strategy. Overrides use_sparse_uf when set explicitly.
    pub uf_strategy: Option<UfStrategy>,
}

impl ScanlineKernel {
    pub const fn new(export_detail: bool) -> Self {
        Self {
            export_detail,
            use_row_sieve: true,
            use_sparse_uf: true,
            uf_strategy: None,
        }
    }

    fn resolved_strategy(&self) -> UfStrategy {
        if let Some(s) = self.uf_strategy {
            return s;
        }
        if self.use_sparse_uf {
            UfStrategy::BitmapRank
        } else {
            UfStrategy::Dense
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
            self.resolved_strategy(),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::{
        precompute_backward_offsets, run_scanline_rect, run_scanline_tile, ScanlineKernel,
        UfStrategy,
    };
    use crate::kernel::{CpuKernel, TileKernel};
    use crate::primality::{sieve_row, DEFAULT_SIEVE_TABLE};
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
        sieve_row(5, 0, width, &DEFAULT_SIEVE_TABLE, &mut marks);

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
            uf_strategy: None,
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
            uf_strategy: None,
        };
        let pointwise_kernel = ScanlineKernel {
            export_detail: false,
            use_row_sieve: false,
            use_sparse_uf: true,
            uf_strategy: None,
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
            uf_strategy: None,
        };
        let pointwise_kernel = ScanlineKernel {
            export_detail: false,
            use_row_sieve: false,
            use_sparse_uf: true,
            uf_strategy: None,
        };

        let sieve_result = sieve_kernel.run_tile(0, 24, 0, 24, 26);
        let pointwise_result = pointwise_kernel.run_tile(0, 24, 0, 24, 26);

        assert_eq!(sieve_result, pointwise_result);
    }

    // ---------------------------------------------------------------------------
    // Three-way equivalence tests: bitmap_rank vs dense vs sparse_hashmap
    // ---------------------------------------------------------------------------

    /// Helper: compare two TileResults for structural equality.
    fn assert_results_structurally_equal(
        a: &crate::kernel::TileResult,
        b: &crate::kernel::TileResult,
        label: &str,
    ) {
        assert_eq!(
            a.num_primes, b.num_primes,
            "{label}: num_primes mismatch: a={} b={}",
            a.num_primes, b.num_primes
        );
        assert_eq!(
            a.num_components, b.num_components,
            "{label}: num_components mismatch: a={} b={}",
            a.num_components, b.num_components
        );

        let mut a_sizes = a.component_sizes.clone();
        a_sizes.sort_unstable();
        let mut b_sizes = b.component_sizes.clone();
        b_sizes.sort_unstable();
        assert_eq!(
            a_sizes, b_sizes,
            "{label}: sorted component_sizes mismatch"
        );

        assert_eq!(
            a.face_count_histogram, b.face_count_histogram,
            "{label}: face_count_histogram mismatch"
        );

        assert_eq!(a.io_count, b.io_count, "{label}: io_count mismatch");
        assert_eq!(a.il_count, b.il_count, "{label}: il_count mismatch");
        assert_eq!(a.ir_count, b.ir_count, "{label}: ir_count mismatch");
        assert_eq!(a.ol_count, b.ol_count, "{label}: ol_count mismatch");
        assert_eq!(a.or_count, b.or_count, "{label}: or_count mismatch");
        assert_eq!(a.lr_count, b.lr_count, "{label}: lr_count mismatch");

        assert_eq!(
            a.i_face_components.len(),
            b.i_face_components.len(),
            "{label}: i_face component count mismatch"
        );
        assert_eq!(
            a.o_face_components.len(),
            b.o_face_components.len(),
            "{label}: o_face component count mismatch"
        );
        assert_eq!(
            a.l_face_components.len(),
            b.l_face_components.len(),
            "{label}: l_face component count mismatch"
        );
        assert_eq!(
            a.r_face_components.len(),
            b.r_face_components.len(),
            "{label}: r_face component count mismatch"
        );
    }

    #[test]
    fn bitmap_rank_dense_equivalence_k2_small() {
        let a_lo = 10_i64;
        let b_lo = 10_i64;
        let a_hi = a_lo + 50;
        let b_hi = b_lo + 50;
        let k_sq = 2_u64;

        let bitmap_rank = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::BitmapRank,
        );
        let dense = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::Dense,
        );
        let hashmap = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::SparseHashMap,
        );

        assert_results_structurally_equal(
            &bitmap_rank,
            &dense,
            "bitmap_rank vs dense: k2_small(10,10,side=50)",
        );
        assert_results_structurally_equal(
            &bitmap_rank,
            &hashmap,
            "bitmap_rank vs hashmap: k2_small(10,10,side=50)",
        );
    }

    #[test]
    fn bitmap_rank_dense_equivalence_k40_medium() {
        let a_lo = 1000_i64;
        let b_lo = 1000_i64;
        let a_hi = a_lo + 200;
        let b_hi = b_lo + 200;
        let k_sq = 40_u64;

        let bitmap_rank = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::BitmapRank,
        );
        let dense = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::Dense,
        );
        let hashmap = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::SparseHashMap,
        );

        assert_results_structurally_equal(
            &bitmap_rank,
            &dense,
            "bitmap_rank vs dense: k40_medium(1000,1000,side=200)",
        );
        assert_results_structurally_equal(
            &bitmap_rank,
            &hashmap,
            "bitmap_rank vs hashmap: k40_medium(1000,1000,side=200)",
        );
    }

    /// Perfect-square k^2=4 regression: boundary primes must be included.
    #[test]
    fn bitmap_rank_dense_equivalence_k4_perfect_square() {
        let a_lo = 0_i64;
        let b_lo = 0_i64;
        let a_hi = 20;
        let b_hi = 20;
        let k_sq = 4_u64;

        let bitmap_rank = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::BitmapRank,
        );
        let dense = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::Dense,
        );

        assert_results_structurally_equal(
            &bitmap_rank,
            &dense,
            "bitmap_rank vs dense: k4_perfect_square(0,0,side=20)",
        );
    }

    /// Perfect-square k^2=9 regression: boundary primes must be included.
    #[test]
    fn bitmap_rank_dense_equivalence_k9_perfect_square() {
        let a_lo = 0_i64;
        let b_lo = 0_i64;
        let a_hi = 30;
        let b_hi = 30;
        let k_sq = 9_u64;

        let bitmap_rank = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::BitmapRank,
        );
        let dense = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::Dense,
        );

        assert_results_structurally_equal(
            &bitmap_rank,
            &dense,
            "bitmap_rank vs dense: k9_perfect_square(0,0,side=30)",
        );
    }

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

        // --- Dense UF ---
        let t0 = Instant::now();
        let dense = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::Dense,
        );
        let dense_elapsed = t0.elapsed();
        eprintln!(
            "[timing] dense  UF:        {:?}  (primes={}, components={})",
            dense_elapsed, dense.num_primes, dense.num_components
        );

        // --- Sparse HashMap UF ---
        let t1 = Instant::now();
        let hashmap = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::SparseHashMap,
        );
        let hashmap_elapsed = t1.elapsed();
        eprintln!(
            "[timing] sparse HashMap:   {:?}  (primes={}, components={})",
            hashmap_elapsed, hashmap.num_primes, hashmap.num_components
        );

        // --- Bitmap + Rank UF ---
        let t2 = Instant::now();
        let bitmap_rank = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::BitmapRank,
        );
        let bitmap_elapsed = t2.elapsed();
        eprintln!(
            "[timing] bitmap+rank UF:   {:?}  (primes={}, components={})",
            bitmap_elapsed, bitmap_rank.num_primes, bitmap_rank.num_components
        );

        eprintln!(
            "[timing] bitmap_rank vs dense:   {:.2}x",
            dense_elapsed.as_secs_f64() / bitmap_elapsed.as_secs_f64()
        );
        eprintln!(
            "[timing] bitmap_rank vs hashmap: {:.2}x",
            hashmap_elapsed.as_secs_f64() / bitmap_elapsed.as_secs_f64()
        );

        assert_results_structurally_equal(&bitmap_rank, &dense, "k40_large_timing: bitmap vs dense");
        assert_results_structurally_equal(&bitmap_rank, &hashmap, "k40_large_timing: bitmap vs hashmap");
    }

    /// Larger-scale benchmark at a_lo=1_000_000, side=2000, k²=40.
    /// At this radius the cache pressure on dense UF should be higher.
    #[test]
    #[ignore]
    fn sparse_dense_timing_k40_large_farfield() {
        use std::time::Instant;

        let a_lo = 1_000_000_i64;
        let b_lo = 1_000_000_i64;
        let a_hi = a_lo + 2000;
        let b_hi = b_lo + 2000;
        let k_sq = 40_u64;

        eprintln!(
            "\n[timing-farfield] k²={k_sq}, region=[{a_lo},{a_hi}]×[{b_lo},{b_hi}]"
        );

        // --- Dense UF ---
        let t0 = Instant::now();
        let dense = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::Dense,
        );
        let dense_elapsed = t0.elapsed();
        eprintln!(
            "[timing-farfield] dense  UF:        {:?}  (primes={}, components={})",
            dense_elapsed, dense.num_primes, dense.num_components
        );

        // --- Sparse HashMap UF ---
        let t1 = Instant::now();
        let hashmap = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::SparseHashMap,
        );
        let hashmap_elapsed = t1.elapsed();
        eprintln!(
            "[timing-farfield] sparse HashMap:   {:?}  (primes={}, components={})",
            hashmap_elapsed, hashmap.num_primes, hashmap.num_components
        );

        // --- Bitmap + Rank UF ---
        let t2 = Instant::now();
        let bitmap_rank = run_scanline_rect(
            a_lo, a_hi, b_lo, b_hi, k_sq, false, true, UfStrategy::BitmapRank,
        );
        let bitmap_elapsed = t2.elapsed();
        eprintln!(
            "[timing-farfield] bitmap+rank UF:   {:?}  (primes={}, components={})",
            bitmap_elapsed, bitmap_rank.num_primes, bitmap_rank.num_components
        );

        eprintln!(
            "[timing-farfield] bitmap_rank vs dense:   {:.2}x",
            dense_elapsed.as_secs_f64() / bitmap_elapsed.as_secs_f64()
        );
        eprintln!(
            "[timing-farfield] bitmap_rank vs hashmap: {:.2}x",
            hashmap_elapsed.as_secs_f64() / bitmap_elapsed.as_secs_f64()
        );

        assert_results_structurally_equal(&bitmap_rank, &dense, "k40_farfield: bitmap vs dense");
        assert_results_structurally_equal(&bitmap_rank, &hashmap, "k40_farfield: bitmap vs hashmap");
    }
}
