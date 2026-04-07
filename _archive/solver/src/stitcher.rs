use std::collections::{HashMap, HashSet};

use crate::union_find::UnionFind;

#[derive(Clone)]
pub struct OverlapPrime {
    pub a: i32,
    pub b: i32,
    pub norm: u64,
    pub component_root: u32,
    pub wedge_id: u32,
}

#[derive(Clone)]
pub struct WedgeResult {
    pub wedge_id: u32,
    pub overlap_left: Vec<OverlapPrime>,
    pub overlap_right: Vec<OverlapPrime>,
    pub has_origin: bool,
    pub origin_root: Option<u32>,
    pub farthest_a: i32,
    pub farthest_b: i32,
    pub farthest_norm: u64,
    /// Number of primes in the origin component that are NATIVE to this wedge
    /// (i.e., this wedge is their primary assignment, not an overlap copy).
    /// Computed for EVERY wedge that has an origin component, not just the
    /// canonical origin wedge (has_origin=true).
    pub native_origin_size: u64,
    /// For each component root that has overlap primes: (native_size, farthest_a, farthest_b, farthest_norm).
    /// native_size counts only primes whose primary wedge is this one.
    pub overlap_component_info: HashMap<u32, (u64, i32, i32, u64)>,
}

pub struct StitchResult {
    pub farthest_a: i32,
    pub farthest_b: i32,
    pub farthest_distance: f64,
    pub total_component_size: u64,
}

pub fn stitch(wedge_results: &[WedgeResult], num_wedges: u32, verbose: bool) -> StitchResult {
    if num_wedges <= 1 {
        // Single wedge: no stitching needed
        if let Some(wr) = wedge_results.first() {
            if wr.has_origin {
                return StitchResult {
                    farthest_a: wr.farthest_a,
                    farthest_b: wr.farthest_b,
                    farthest_distance: (wr.farthest_norm as f64).sqrt(),
                    total_component_size: wr.native_origin_size,
                };
            }
        }
        return StitchResult {
            farthest_a: 0,
            farthest_b: 0,
            farthest_distance: 0.0,
            total_component_size: 0,
        };
    }

    // Index wedge results by wedge_id
    let mut by_wedge: Vec<Option<&WedgeResult>> = vec![None; num_wedges as usize];
    for wr in wedge_results {
        if (wr.wedge_id as usize) < by_wedge.len() {
            by_wedge[wr.wedge_id as usize] = Some(wr);
        }
    }

    // Assign global IDs to each unique (wedge_id, component_root) pair
    let mut id_map: HashMap<(u32, u32), u32> = HashMap::new();
    let mut next_id: u32 = 0;

    let mut register = |wedge_id: u32, root: u32| -> u32 {
        if let Some(&id) = id_map.get(&(wedge_id, root)) {
            id
        } else {
            let id = next_id;
            id_map.insert((wedge_id, root), id);
            next_id += 1;
            id
        }
    };

    // Register all component IDs that appear in overlap zones or as origin
    for wr in wedge_results {
        for op in &wr.overlap_left {
            register(op.wedge_id, op.component_root);
        }
        for op in &wr.overlap_right {
            register(op.wedge_id, op.component_root);
        }
        if let Some(root) = wr.origin_root {
            register(wr.wedge_id, root);
        }
        for &root in wr.overlap_component_info.keys() {
            register(wr.wedge_id, root);
        }
    }

    if next_id == 0 {
        // No components found
        return StitchResult {
            farthest_a: 0,
            farthest_b: 0,
            farthest_distance: 0.0,
            total_component_size: 0,
        };
    }

    // Build global union-find
    let mut global_uf = UnionFind::new(next_id as usize);
    for _ in 0..next_id {
        global_uf.make_set();
    }

    // Stitch boundaries: match overlap primes by (a,b) coordinates
    for i in 0..(num_wedges - 1) as usize {
        let Some(left) = by_wedge[i] else { continue };
        let Some(right) = by_wedge[i + 1] else { continue };

        // Build lookup for right wedge's left overlap primes
        let right_lookup: HashMap<(i32, i32), Vec<&OverlapPrime>> = {
            let mut map: HashMap<(i32, i32), Vec<&OverlapPrime>> = HashMap::new();
            for op in &right.overlap_left {
                map.entry((op.a, op.b)).or_default().push(op);
            }
            map
        };

        // Match with left wedge's right overlap primes
        for left_op in &left.overlap_right {
            if let Some(candidates) = right_lookup.get(&(left_op.a, left_op.b)) {
                let left_gid = id_map[&(left_op.wedge_id, left_op.component_root)];
                for right_op in candidates {
                    let right_gid = id_map[&(right_op.wedge_id, right_op.component_root)];
                    global_uf.union(left_gid, right_gid);
                }
            }
        }
    }

    // Unify all wedges' origin components in the global union-find.
    //
    // At high distances (e.g. 800M), the angular overlap zone between adjacent
    // wedges is extremely thin (sqrt(k)/sqrt(norm) ~ 8e-9 radians) — far too
    // narrow to contain shared primes. The boundary stitching above relies on
    // matching overlap primes by (a,b) coordinates, but when no primes span
    // the boundary, adjacent wedges' origin components remain disconnected.
    //
    // This is incorrect: in UB mode, all primes below boundary_norm are
    // auto-connected to the origin, so every wedge's origin IS the same
    // logical component. In resume mode with all-wedge seeding, same logic
    // applies. We explicitly merge all has_origin wedges' origin components.
    //
    // This also handles LB mode where multiple wedges discover the (1,1)
    // origin prime through angular overlap at low norms.
    let mut origin_gid = None;
    for wr in wedge_results {
        if wr.has_origin {
            if let Some(root) = wr.origin_root {
                if let Some(&gid) = id_map.get(&(wr.wedge_id, root)) {
                    if let Some(prev_gid) = origin_gid {
                        // Merge this wedge's origin with the accumulated origin
                        global_uf.union(prev_gid, gid);
                    }
                    origin_gid = Some(gid);
                }
            }
        }
    }

    let Some(origin_gid) = origin_gid else {
        return StitchResult {
            farthest_a: 0,
            farthest_b: 0,
            farthest_distance: 0.0,
            total_component_size: 0,
        };
    };

    let origin_global_root = global_uf.find(origin_gid);

    // Identify all (wedge_id, local_root) pairs in the origin global component
    let mut origin_members: HashSet<(u32, u32)> = HashSet::new();
    for (&(wedge_id, local_root), &gid) in &id_map {
        if global_uf.find(gid) == origin_global_root {
            origin_members.insert((wedge_id, local_root));
        }
    }

    if verbose {
        eprintln!("stitch: origin_gid={} origin_global_root={} origin_members={}", origin_gid, origin_global_root, origin_members.len());
        for wr in wedge_results {
            let overlap_has_origin_root = wr.origin_root
                .map(|r| wr.overlap_component_info.contains_key(&r))
                .unwrap_or(false);
            eprintln!(
                "stitch: wedge={} has_origin={} origin_root={:?} native_origin_size={} \
                 overlap_info_keys={} overlap_has_origin_root={} farthest=({},{}) farthest_norm={} \
                 overlap_left={} overlap_right={}",
                wr.wedge_id, wr.has_origin, wr.origin_root, wr.native_origin_size,
                wr.overlap_component_info.len(), overlap_has_origin_root,
                wr.farthest_a, wr.farthest_b, wr.farthest_norm,
                wr.overlap_left.len(), wr.overlap_right.len(),
            );
        }
    }

    // Aggregate component sizes using NATIVE sizes only (no double-counting).
    // Each wedge reports native_origin_size (primes whose primary wedge is this one
    // that belong to the wedge-local origin component).
    //
    // For each (wedge_id, local_root) in the global origin component:
    // 1. If local_root IS the wedge's origin_root → use native_origin_size
    //    (this covers BOTH the canonical origin wedge and non-origin wedges
    //     whose local origin component was stitched into the global origin).
    // 2. Otherwise → look up overlap_component_info for non-origin components
    //    that were merged into the global origin through boundary stitching.
    //
    // BUG FIX: Previously, only the wedge with has_origin=true used
    // native_origin_size. Non-origin wedges fell through to
    // overlap_component_info.get(&local_root), which only has entries for
    // roots with overlap primes. Interior primes of non-origin wedges were
    // silently dropped, causing ~50% component size at 800M with 6 wedges.
    let mut total_component_size = 0u64;
    let mut farthest_a = 0i32;
    let mut farthest_b = 0i32;
    let mut farthest_norm = 0u64;

    // Track which wedges have contributed to avoid double-counting
    let mut counted_components: HashSet<(u32, u32)> = HashSet::new();

    for &(wedge_id, local_root) in &origin_members {
        if !counted_components.insert((wedge_id, local_root)) {
            continue;
        }
        let Some(wr) = by_wedge.get(wedge_id as usize).and_then(|o| *o) else {
            continue;
        };

        if wr.origin_root == Some(local_root) {
            // This wedge's origin component is part of the global origin.
            // Use native_origin_size — it counts all primes whose primary
            // wedge is this one, including interior primes not in any overlap zone.
            if verbose {
                eprintln!(
                    "stitch: aggregate wedge={} path=origin_root native_origin_size={} farthest_norm={}",
                    wedge_id, wr.native_origin_size, wr.farthest_norm,
                );
            }
            total_component_size += wr.native_origin_size;
            if wr.farthest_norm > farthest_norm {
                farthest_norm = wr.farthest_norm;
                farthest_a = wr.farthest_a;
                farthest_b = wr.farthest_b;
            }
        } else if let Some(&(size, a, b, norm)) = wr.overlap_component_info.get(&local_root) {
            // Non-origin component merged into origin through stitching
            if verbose {
                eprintln!(
                    "stitch: aggregate wedge={} path=overlap_info local_root={} size={} farthest_norm={}",
                    wedge_id, local_root, size, norm,
                );
            }
            total_component_size += size;
            if norm > farthest_norm {
                farthest_norm = norm;
                farthest_a = a;
                farthest_b = b;
            }
        } else if verbose {
            eprintln!(
                "stitch: aggregate wedge={} path=MISS local_root={} (not origin_root {:?}, not in overlap_info)",
                wedge_id, local_root, wr.origin_root,
            );
        }
    }

    if verbose {
        eprintln!(
            "stitch: RESULT total_component_size={} farthest=({},{}) farthest_norm={}",
            total_component_size, farthest_a, farthest_b, farthest_norm,
        );
    }

    StitchResult {
        farthest_a,
        farthest_b,
        farthest_distance: (farthest_norm as f64).sqrt(),
        total_component_size,
    }
}
