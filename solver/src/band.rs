use std::collections::VecDeque;

use crate::sieve::GaussianPrime;
use crate::union_find::UnionFind;
use fxhash::{FxHashMap, FxHashSet};
use smallvec::SmallVec;

const NO_SLOT: u32 = u32::MAX;
const NO_GEN: u32 = 0;

#[inline]
fn canonical(a: i32, b: i32) -> (i32, i32) {
    let (x, y) = (a.abs(), b.abs());
    (x.max(y), x.min(y))
}

#[inline]
fn isqrt_u128(n: u128) -> u128 {
    if n < 2 {
        return n;
    }

    let mut x = (n as f64).sqrt() as u128;
    if x == 0 {
        x = 1;
    }

    while (x + 1) <= n / (x + 1) {
        x += 1;
    }
    while x > n / x {
        x -= 1;
    }
    x
}

#[inline]
fn ceil_sqrt_u64(n: u64) -> u64 {
    let floor = isqrt_u128(n as u128) as u64;
    if (floor as u128) * (floor as u128) == n as u128 {
        floor
    } else {
        floor + 1
    }
}

#[inline]
fn ceil_two_sqrt_product(a: u64, b: u64) -> u128 {
    if a == 0 || b == 0 {
        return 0;
    }

    let prod = (a as u128) * (b as u128);
    let floor = isqrt_u128(prod);
    let floor_sq = floor * floor;

    if floor_sq == prod {
        floor * 2
    } else if prod <= floor_sq + floor {
        floor * 2 + 1
    } else {
        floor * 2 + 2
    }
}

#[inline]
fn ceil_radius_sum_sq(norm: u64, k_squared: u64) -> u64 {
    let base = norm as u128 + k_squared as u128;
    let cross = ceil_two_sqrt_product(norm, k_squared);
    base.saturating_add(cross).min(u64::MAX as u128) as u64
}

#[inline]
fn floor_radius_diff_sq(norm: u64, k_squared: u64) -> u64 {
    if norm <= k_squared {
        return 0;
    }

    let base = norm as u128 + k_squared as u128;
    let cross = ceil_two_sqrt_product(norm, k_squared);
    base.saturating_sub(cross).min(u64::MAX as u128) as u64
}

pub struct MoatResult {
    pub farthest_a: i32,
    pub farthest_b: i32,
    pub farthest_distance: f64,
    pub component_size: u64,
}

pub struct ProcessResult {
    pub moat: Option<MoatResult>,
    pub slot: u32,
    pub gen: u32,
}

pub struct BandStats {
    pub band_size: usize,
    pub origin_component_size: u64,
}

pub struct BandProcessor {
    k_squared: u64,
    cell_size: i32,

    grid: FxHashMap<(i32, i32), SmallVec<[u32; 8]>>,

    node_a: Vec<i32>,
    node_b: Vec<i32>,
    node_norm: Vec<u64>,
    node_gen: Vec<u32>,
    component_far_a: Vec<i32>,
    component_far_b: Vec<i32>,
    component_far_norm: Vec<u64>,

    uf: UnionFind,
    eviction_queue: VecDeque<(u32, u32)>,
    pinned_slots: FxHashSet<(u32, u32)>,

    origin_slot: u32,
    origin_gen: u32,
    farthest_a: i32,
    farthest_b: i32,
    farthest_dist_sq: u64,
    origin_component_size: u64,
    moat_threshold_norm: u64,

    upper_bound: bool,
    boundary_norm: u64,
    boundary_plus_k: u64,

    primes_processed: u64,
}

impl BandProcessor {
    pub fn new(k_squared: u64) -> Self {
        Self::build(k_squared, false, 0)
    }

    pub fn new_upper_bound(k_squared: u64, boundary_distance: u64) -> Self {
        Self::build(k_squared, true, boundary_distance)
    }

    pub fn new_with_capacity(k_squared: u64, capacity: usize) -> Self {
        Self::build_with_capacity(k_squared, false, 0, capacity)
    }

    pub fn new_upper_bound_with_capacity(
        k_squared: u64,
        boundary_distance: u64,
        capacity: usize,
    ) -> Self {
        Self::build_with_capacity(k_squared, true, boundary_distance, capacity)
    }

    fn build(k_squared: u64, upper_bound: bool, boundary_distance: u64) -> Self {
        Self::build_with_capacity(k_squared, upper_bound, boundary_distance, 16_000_000)
    }

    fn build_with_capacity(
        k_squared: u64,
        upper_bound: bool,
        boundary_distance: u64,
        uf_capacity: usize,
    ) -> Self {
        let cell_size = ceil_sqrt_u64(k_squared).max(1).min(i32::MAX as u64) as i32;

        let boundary_norm = ((boundary_distance as u128) * (boundary_distance as u128))
            .min(u64::MAX as u128) as u64;
        let boundary_plus_k = ceil_radius_sum_sq(boundary_norm, k_squared);

        let mut processor = Self {
            k_squared,
            cell_size,
            grid: FxHashMap::default(),
            node_a: Vec::new(),
            node_b: Vec::new(),
            node_norm: Vec::new(),
            node_gen: Vec::new(),
            component_far_a: Vec::new(),
            component_far_b: Vec::new(),
            component_far_norm: Vec::new(),
            uf: UnionFind::new(uf_capacity),
            eviction_queue: VecDeque::new(),
            pinned_slots: FxHashSet::default(),
            origin_slot: NO_SLOT,
            origin_gen: NO_GEN,
            farthest_a: 0,
            farthest_b: 0,
            farthest_dist_sq: 0,
            origin_component_size: 0,
            moat_threshold_norm: 0,
            upper_bound,
            boundary_norm,
            boundary_plus_k,
            primes_processed: 0,
        };

        if upper_bound {
            let (slot, gen) = processor.push_node(0, 0, 0);
            processor.origin_slot = slot;
            processor.origin_gen = gen;
            processor.moat_threshold_norm = processor.k_squared;
        }

        processor
    }

    #[inline]
    pub fn process_prime(&mut self, prime: &GaussianPrime) -> Option<MoatResult> {
        self.process_prime_ext(prime).moat
    }

    #[inline]
    pub fn process_prime_ext(&mut self, prime: &GaussianPrime) -> ProcessResult {
        self.primes_processed += 1;
        debug_assert!(self.boundary_plus_k >= self.boundary_norm);

        let (a, b) = canonical(prime.a, prime.b);
        let (slot, gen) = self.push_node(a, b, prime.norm);

        let cell = self.cell_key(a, b);
        self.grid.entry(cell).or_default().push(slot);

        self.connect_neighbors(slot, gen, a, b);

        if !self.upper_bound && self.origin_slot == NO_SLOT && prime.norm == 2 && a == 1 && b == 1 {
            self.origin_slot = slot;
            self.origin_gen = gen;
            self.farthest_a = a;
            self.farthest_b = b;
            self.farthest_dist_sq = prime.norm;
            self.moat_threshold_norm = self.compute_moat_threshold(self.farthest_dist_sq);
        }

        if self.upper_bound
            && self.origin_slot != NO_SLOT
            && self.origin_gen == self.uf.generation(self.origin_slot)
            && prime.norm < self.boundary_plus_k
        {
            self.union_components(self.origin_slot, slot);
        }

        self.refresh_origin_tracking(slot, a, b, prime.norm);
        self.evict(prime.norm);

        if self.origin_slot != NO_SLOT && prime.norm > self.moat_threshold_norm {
            return ProcessResult {
                moat: Some(MoatResult {
                    farthest_a: self.farthest_a,
                    farthest_b: self.farthest_b,
                    farthest_distance: (self.farthest_dist_sq as f64).sqrt(),
                    component_size: self.origin_component_size,
                }),
                slot,
                gen,
            };
        }

        ProcessResult {
            moat: None,
            slot,
            gen,
        }
    }

    #[inline]
    pub fn band_size(&self) -> usize {
        self.uf.live_count()
    }

    pub fn farthest_a(&self) -> i32 {
        self.farthest_a
    }

    pub fn farthest_b(&self) -> i32 {
        self.farthest_b
    }

    pub fn farthest_distance(&self) -> f64 {
        (self.farthest_dist_sq as f64).sqrt()
    }

    pub fn origin_component_size(&self) -> u64 {
        self.origin_component_size
    }

    pub fn pin_slot(&mut self, slot: u32, gen: u32) {
        self.pinned_slots.insert((slot, gen));
    }

    pub fn find_root(&mut self, slot: u32) -> u32 {
        self.uf.find(slot)
    }

    pub fn origin_find_root(&mut self) -> u32 {
        if self.origin_slot == NO_SLOT || self.origin_gen != self.uf.generation(self.origin_slot) {
            return NO_SLOT;
        }
        self.uf.find(self.origin_slot)
    }

    pub fn component_size(&mut self, root: u32) -> u64 {
        self.uf.size(root)
    }

    pub fn component_farthest(&mut self, root: u32) -> (i32, i32, u64) {
        let canonical_root = self.uf.find(root) as usize;
        (
            self.component_far_a[canonical_root],
            self.component_far_b[canonical_root],
            self.component_far_norm[canonical_root],
        )
    }

    pub fn stats(&self) -> BandStats {
        BandStats {
            band_size: self.band_size(),
            origin_component_size: self.origin_component_size,
        }
    }

    #[inline]
    fn push_node(&mut self, a: i32, b: i32, norm: u64) -> (u32, u32) {
        let (slot, gen) = self.uf.make_set();
        let idx = slot as usize;

        if idx >= self.node_a.len() {
            self.node_a.push(a);
            self.node_b.push(b);
            self.node_norm.push(norm);
            self.node_gen.push(gen);
            self.component_far_a.push(a);
            self.component_far_b.push(b);
            self.component_far_norm.push(norm);
        } else {
            self.node_a[idx] = a;
            self.node_b[idx] = b;
            self.node_norm[idx] = norm;
            self.node_gen[idx] = gen;
            self.component_far_a[idx] = a;
            self.component_far_b[idx] = b;
            self.component_far_norm[idx] = norm;
        }

        self.eviction_queue.push_back((slot, gen));
        (slot, gen)
    }

    #[inline]
    fn connect_neighbors(&mut self, node_id: u32, gen: u32, a: i32, b: i32) {
        debug_assert_eq!(self.node_gen[node_id as usize], gen);

        let (cx, cy) = self.cell_key(a, b);

        // Two-phase: collect then union (required by borrow checker —
        // can't mutate self via union_components while iterating self.grid).
        // Filter by generation + distance inline during collection.
        // No hard cap — Vec grows if needed. No silent neighbor drops.
        let mut to_union: SmallVec<[u32; 32]> = SmallVec::new();

        for dy in -1i32..=1 {
            for dx in -1i32..=1 {
                if let Some(bucket) = self.grid.get(&(cx + dx, cy + dy)) {
                    for &candidate in bucket.iter() {
                        if candidate == node_id {
                            continue;
                        }

                        let idx = candidate as usize;
                        if self.node_gen[idx] != self.uf.generation(candidate) {
                            continue;
                        }

                        let da = (a - self.node_a[idx]) as i64;
                        let db = (b - self.node_b[idx]) as i64;
                        let dist_sq = (da * da + db * db) as u64;
                        if dist_sq <= self.k_squared {
                            to_union.push(candidate);
                        }
                    }
                }
            }
        }

        for &candidate in &to_union {
            self.union_components(node_id, candidate);
        }
    }

    fn refresh_origin_tracking(&mut self, slot: u32, a: i32, b: i32, norm: u64) {
        if self.origin_slot == NO_SLOT {
            return;
        }
        if self.origin_gen != self.uf.generation(self.origin_slot) {
            return;
        }

        let origin_root = self.uf.find(self.origin_slot);

        if self.uf.find(slot) == origin_root && norm > self.farthest_dist_sq {
            self.farthest_a = a;
            self.farthest_b = b;
            self.farthest_dist_sq = norm;
            self.moat_threshold_norm = self.compute_moat_threshold(norm);
        }

        let mut size = self.uf.size(origin_root);
        if self.upper_bound && size > 0 {
            size -= 1;
        }
        self.origin_component_size = size;
    }

    fn evict(&mut self, current_norm: u64) {
        let min_norm = self.band_min_norm(current_norm);
        let mut iterations = self.eviction_queue.len();

        while iterations > 0 {
            let Some((slot, gen)) = self.eviction_queue.pop_front() else {
                break;
            };
            iterations -= 1;

            if self.node_gen[slot as usize] != gen {
                continue;
            }

            if self.node_norm[slot as usize] >= min_norm {
                self.eviction_queue.push_front((slot, gen));
                break;
            }

            if slot == self.origin_slot && gen == self.origin_gen {
                self.eviction_queue.push_back((slot, gen));
                continue;
            }

            if self.is_pinned(slot, gen) {
                self.eviction_queue.push_back((slot, gen));
                continue;
            }

            self.remove_from_grid(slot);
            let _ = self.uf.recycle(slot);
            self.node_gen[slot as usize] = NO_GEN;
        }
    }

    fn union_components(&mut self, left: u32, right: u32) -> u32 {
        let left_root = self.uf.find(left);
        let right_root = self.uf.find(right);
        if left_root == right_root {
            return left_root;
        }

        let left_idx = left_root as usize;
        let right_idx = right_root as usize;
        let mut best = (
            self.component_far_norm[left_idx],
            self.component_far_a[left_idx],
            self.component_far_b[left_idx],
        );
        let right_best = (
            self.component_far_norm[right_idx],
            self.component_far_a[right_idx],
            self.component_far_b[right_idx],
        );
        if right_best.0 > best.0 {
            best = right_best;
        }

        let merged_root = self.uf.union(left_root, right_root) as usize;
        self.component_far_norm[merged_root] = best.0;
        self.component_far_a[merged_root] = best.1;
        self.component_far_b[merged_root] = best.2;
        merged_root as u32
    }

    fn is_pinned(&self, slot: u32, gen: u32) -> bool {
        self.pinned_slots.contains(&(slot, gen))
    }

    fn remove_from_grid(&mut self, slot: u32) {
        let idx = slot as usize;
        let cell = self.cell_key(self.node_a[idx], self.node_b[idx]);

        let mut empty = false;
        if let Some(bucket) = self.grid.get_mut(&cell) {
            if let Some(pos) = bucket.iter().position(|&id| id == slot) {
                bucket.swap_remove(pos);
            }
            empty = bucket.is_empty();
        }

        if empty {
            self.grid.remove(&cell);
        }
    }

    #[inline]
    fn compute_moat_threshold(&self, farthest_norm: u64) -> u64 {
        ceil_radius_sum_sq(farthest_norm, self.k_squared)
    }

    #[inline]
    fn band_min_norm(&self, current_norm: u64) -> u64 {
        floor_radius_diff_sq(current_norm, self.k_squared)
    }

    #[inline]
    fn cell_key(&self, a: i32, b: i32) -> (i32, i32) {
        (a.div_euclid(self.cell_size), b.div_euclid(self.cell_size))
    }
}

#[cfg(test)]
mod tests {
    use super::{
        ceil_radius_sum_sq, ceil_sqrt_u64, floor_radius_diff_sq, BandProcessor, MoatResult,
    };
    use crate::sieve::GaussianPrime;

    fn is_prime(n: u64) -> bool {
        if n < 2 {
            return false;
        }
        if n == 2 {
            return true;
        }
        if n.is_multiple_of(2) {
            return false;
        }

        let mut d = 3u64;
        while d * d <= n {
            if n.is_multiple_of(d) {
                return false;
            }
            d += 2;
        }
        true
    }

    fn is_gaussian_prime_first_octant(a: i32, b: i32, norm: u64) -> bool {
        if a == 0 && b == 0 {
            return false;
        }
        if b == 0 {
            let p = a as u64;
            return p % 4 == 3 && is_prime(p);
        }
        is_prime(norm)
    }

    fn gaussian_primes_up_to_norm(max_norm: u64) -> Vec<GaussianPrime> {
        let limit = (max_norm as f64).sqrt() as i32;
        let mut primes = Vec::new();

        for a in 0..=limit {
            for b in 0..=a {
                let norm = (a as i64 * a as i64 + b as i64 * b as i64) as u64;
                if norm <= max_norm && is_gaussian_prime_first_octant(a, b, norm) {
                    primes.push(GaussianPrime { a, b, norm });
                }
            }
        }

        primes.sort_unstable_by(|l, r| l.norm.cmp(&r.norm).then(l.a.cmp(&r.a)).then(l.b.cmp(&r.b)));
        primes
    }

    #[test]
    fn k2_2_detects_moat_with_expected_farthest_point() {
        let mut processor = BandProcessor::new(2);
        let primes = gaussian_primes_up_to_norm(200);

        let mut moat: Option<MoatResult> = None;
        for prime in &primes {
            if let Some(result) = processor.process_prime(prime) {
                moat = Some(result);
                break;
            }
        }

        let moat = moat.expect("expected moat detection for k^2=2 by norm 200");
        assert_eq!((moat.farthest_a, moat.farthest_b), (11, 4));
        assert!((moat.farthest_distance - 137f64.sqrt()).abs() < 1e-9);
    }

    #[test]
    fn k2_2_upper_bound_origin_component_size_is_10() {
        let mut processor = BandProcessor::new_upper_bound(2, 8);
        let primes = gaussian_primes_up_to_norm(200);

        let mut moat: Option<MoatResult> = None;
        for prime in &primes {
            if prime.norm < 36 {
                continue;
            }
            if let Some(result) = processor.process_prime(prime) {
                moat = Some(result);
                break;
            }
        }

        let _moat = moat.expect("expected moat detection for k^2=2 upper-bound by norm 200");
        let summary = processor.stats();
        assert_eq!(summary.origin_component_size, 10);
        assert_eq!((processor.farthest_a(), processor.farthest_b()), (11, 4));
        assert_eq!(processor.primes_processed, 17);
    }

    #[test]
    fn radius_bounds_match_float_reference_for_small_values() {
        for norm in 0..=1_000u64 {
            for k_squared in 0..=1_000u64 {
                let prod = (norm as u128) * (k_squared as u128);
                let mut slow_ceil_cross = 0u128;
                while slow_ceil_cross * slow_ceil_cross < prod * 4 {
                    slow_ceil_cross += 1;
                }
                let sum_ref = (norm as u128 + k_squared as u128 + slow_ceil_cross) as u64;
                assert_eq!(
                    ceil_radius_sum_sq(norm, k_squared),
                    sum_ref,
                    "sum bound mismatch for norm={norm}, k_squared={k_squared}"
                );

                let diff_ref = if norm <= k_squared {
                    0
                } else {
                    (norm as u128 + k_squared as u128 - slow_ceil_cross) as u64
                };
                assert_eq!(
                    floor_radius_diff_sq(norm, k_squared),
                    diff_ref,
                    "diff bound mismatch for norm={norm}, k_squared={k_squared}"
                );
            }
        }
    }

    #[test]
    fn adjacent_cell_scan_covers_all_points_within_k() {
        for k_squared in 0..=64u64 {
            let cell_size = ceil_sqrt_u64(k_squared).max(1) as i32;
            for a1 in 0..=12i32 {
                for b1 in 0..=12i32 {
                    let cx1 = a1.div_euclid(cell_size);
                    let cy1 = b1.div_euclid(cell_size);
                    for a2 in 0..=12i32 {
                        for b2 in 0..=12i32 {
                            let da = (a1 - a2) as i64;
                            let db = (b1 - b2) as i64;
                            let dist_sq = (da * da + db * db) as u64;
                            if dist_sq > k_squared {
                                continue;
                            }

                            let cx2 = a2.div_euclid(cell_size);
                            let cy2 = b2.div_euclid(cell_size);
                            assert!(
                                (cx1 - cx2).abs() <= 1 && (cy1 - cy2).abs() <= 1,
                                "k^2={} cell_size={} p1=({}, {}) p2=({}, {})",
                                k_squared,
                                cell_size,
                                a1,
                                b1,
                                a2,
                                b2
                            );
                        }
                    }
                }
            }
        }
    }
}
