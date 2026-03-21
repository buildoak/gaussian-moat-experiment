use serde::{Deserialize, Serialize};

use crate::primes::PrimeSieve;
use crate::tile::{
    build_tile_with_sieve, TileOperator, FACE_INNER_BIT, FACE_LEFT_BIT, FACE_OUTER_BIT,
    FACE_RIGHT_BIT,
};

/// Result of running the kernel on a single tile.
/// Contains full face connectivity -- mode-agnostic.
/// ISE reads io_count. LB reads face component lists.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TileResult {
    // --- Cross-face component counts ---
    pub io_count: usize, // components touching both I-face and O-face
    pub il_count: usize, // components touching both I-face and Left-face
    pub ir_count: usize, // components touching both I-face and Right-face
    pub ol_count: usize, // components touching both O-face and Left-face
    pub or_count: usize, // components touching both O-face and Right-face
    pub lr_count: usize, // components touching both Left-face and Right-face

    // --- Per-face component ID lists (for adjacency matching in LB mode) ---
    pub i_face_components: Vec<usize>, // component IDs touching I-face
    pub o_face_components: Vec<usize>, // component IDs touching O-face
    pub l_face_components: Vec<usize>, // component IDs touching Left-face
    pub r_face_components: Vec<usize>, // component IDs touching Right-face

    pub num_primes: usize,
}

impl TileResult {
    /// Build a TileResult from a TileOperator by inspecting component_faces
    /// and collecting per-face component ID sets.
    pub fn from_tile_operator(tile: &TileOperator) -> Self {
        let mut io = 0;
        let mut il = 0;
        let mut ir = 0;
        let mut ol = 0;
        let mut or_ = 0;
        let mut lr = 0;
        let mut i_comps = Vec::new();
        let mut o_comps = Vec::new();
        let mut l_comps = Vec::new();
        let mut r_comps = Vec::new();

        for (id, &faces) in tile.component_faces.iter().enumerate() {
            let has_i = faces & FACE_INNER_BIT != 0;
            let has_o = faces & FACE_OUTER_BIT != 0;
            let has_l = faces & FACE_LEFT_BIT != 0;
            let has_r = faces & FACE_RIGHT_BIT != 0;

            if has_i && has_o {
                io += 1;
            }
            if has_i && has_l {
                il += 1;
            }
            if has_i && has_r {
                ir += 1;
            }
            if has_o && has_l {
                ol += 1;
            }
            if has_o && has_r {
                or_ += 1;
            }
            if has_l && has_r {
                lr += 1;
            }

            if has_i {
                i_comps.push(id);
            }
            if has_o {
                o_comps.push(id);
            }
            if has_l {
                l_comps.push(id);
            }
            if has_r {
                r_comps.push(id);
            }
        }

        TileResult {
            io_count: io,
            il_count: il,
            ir_count: ir,
            ol_count: ol,
            or_count: or_,
            lr_count: lr,
            i_face_components: i_comps,
            o_face_components: o_comps,
            l_face_components: l_comps,
            r_face_components: r_comps,
            num_primes: tile.num_primes,
        }
    }
}

/// The kernel trait. CPU implementation today, CUDA tomorrow.
/// Mode-agnostic: computes full face connectivity for every tile.
///
/// The trait does NOT take a sieve/prime-source parameter. The sieve strategy
/// is an implementation detail of each backend:
/// - CpuKernel holds a &PrimeSieve internally.
/// - CudaKernel uses device-side sieve in shared memory.
/// This keeps the trait clean and compatible with all backends.
pub trait TileKernel: Send + Sync {
    /// Run the kernel on a single tile. Returns rich TileResult with all face connectivity.
    fn run_tile(
        &self,
        a_lo: i64,
        a_hi: i64,
        b_lo: i64,
        b_hi: i64,
        k_sq: u64,
    ) -> TileResult;
}

/// CPU kernel backed by a PrimeSieve (legacy/Day-0 path).
pub struct CpuKernel<'a> {
    sieve: &'a PrimeSieve,
}

impl<'a> CpuKernel<'a> {
    pub fn new(sieve: &'a PrimeSieve) -> Self {
        Self { sieve }
    }
}

impl<'a> TileKernel for CpuKernel<'a> {
    fn run_tile(
        &self,
        a_lo: i64,
        a_hi: i64,
        b_lo: i64,
        b_hi: i64,
        k_sq: u64,
    ) -> TileResult {
        let tile = build_tile_with_sieve(a_lo, a_hi, b_lo, b_hi, k_sq, self.sieve, false);
        TileResult::from_tile_operator(&tile)
    }
}

/// Count components that span both Inner and Outer faces (I->O transport threads).
/// Moved here from probe.rs to make it available to both ISE and LB modes.
pub fn io_crossing_count(tile: &TileOperator) -> usize {
    tile.component_faces
        .iter()
        .filter(|&&faces| faces & FACE_INNER_BIT != 0 && faces & FACE_OUTER_BIT != 0)
        .count()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::primes::PrimeSieve;
    use crate::tile::build_tile_with_sieve;

    fn sieve_for_tile(a_hi: i64, b_hi: i64, k_sq: u64) -> PrimeSieve {
        let collar = (k_sq as f64).sqrt().ceil() as i64;
        let max_coord = (a_hi.abs() + collar).max(b_hi.abs() + collar) as u64;
        let max_norm = max_coord * max_coord * 2;
        PrimeSieve::new(max_norm.min(10_000_000).max(1000))
    }

    /// Gate 1, Test A: Small tile around origin at k^2=2 should have io_count > 0
    #[test]
    fn kernel_origin_tile_has_io_connectivity() {
        let sieve = sieve_for_tile(2, 2, 2);
        let kernel = CpuKernel::new(&sieve);
        let result = kernel.run_tile(0, 2, 0, 2, 2);
        assert!(
            result.io_count > 0,
            "Origin tile at k^2=2 should have I->O connectivity, got io_count={}",
            result.io_count
        );
    }

    /// Gate 1, Test B: At R=100, k^2=2 should produce a moat (no I->O connectivity)
    #[test]
    fn kernel_far_tile_has_no_io_at_small_k() {
        let sieve = sieve_for_tile(102, 2, 2);
        let kernel = CpuKernel::new(&sieve);
        let result = kernel.run_tile(100, 102, 0, 2, 2);
        assert_eq!(
            result.io_count, 0,
            "Tile at R=100 with k^2=2 should have no I->O connectivity"
        );
    }

    /// Gate 1, Test C: Kernel wrapper agrees with direct tile build
    #[test]
    fn kernel_matches_direct_tile_build() {
        let sieve = sieve_for_tile(20, 20, 2);
        let kernel = CpuKernel::new(&sieve);
        let result = kernel.run_tile(0, 20, 0, 20, 2);

        // Direct path
        let direct_tile = build_tile_with_sieve(0, 20, 0, 20, 2, &sieve, false);
        let direct_io = io_crossing_count(&direct_tile);

        assert_eq!(
            result.io_count, direct_io,
            "Kernel io_count ({}) must match direct tile io_crossing_count ({})",
            result.io_count, direct_io
        );
    }

    /// Gate 1, Test D: Rich TileResult fields are populated correctly
    #[test]
    fn kernel_rich_fields_populated() {
        let sieve = sieve_for_tile(5, 5, 4);
        let kernel = CpuKernel::new(&sieve);
        let result = kernel.run_tile(0, 5, 0, 5, 4);

        // All counts should be non-negative (trivially true for usize, but no panics)
        let total_cross = result.il_count
            + result.ir_count
            + result.ol_count
            + result.or_count
            + result.lr_count;
        let _ = total_cross; // confirm no panics during computation

        // I-face component list should be at least as long as io_count
        // (io requires I-face membership, but I-face may have non-IO components too)
        assert!(
            result.i_face_components.len() >= result.io_count,
            "I-face components ({}) must be >= io_count ({})",
            result.i_face_components.len(),
            result.io_count
        );
    }

    /// Gate 3: Concurrency -- shared kernel across Rayon tasks
    #[test]
    fn kernel_concurrent_access() {
        use rayon::prelude::*;

        let sieve = sieve_for_tile(20, 20, 2);
        let kernel = CpuKernel::new(&sieve);

        let results: Vec<TileResult> = [(0i64, 5i64), (5, 10), (10, 15), (15, 20)]
            .par_iter()
            .map(|&(b_lo, b_hi)| kernel.run_tile(0, 10, b_lo, b_hi, 2))
            .collect();

        // All results should complete without panic
        assert_eq!(results.len(), 4);

        // Each result should have meaningful data
        for (i, r) in results.iter().enumerate() {
            assert!(
                r.num_primes > 0 || r.io_count == 0,
                "Result {} should have primes or zero io",
                i
            );
        }
    }
}
