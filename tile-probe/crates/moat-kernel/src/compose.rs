use rayon::prelude::*;
use serde::Serialize;

use crate::tile::{face_bit, Face, FacePort, SimpleUF, TileOperator};

/// A single seam connection event during L<->R or I<->O composition.
#[derive(Debug, Clone, Serialize)]
pub struct SeamEvent {
    pub left_strip: usize,
    pub right_strip: usize,
    pub left_port: (i64, i64),
    pub right_port: (i64, i64),
    pub merged_component_before: (usize, usize),
    pub merged_component_after: usize,
}

fn port_distance_sq(left: &FacePort, right: &FacePort) -> u64 {
    let da = left.a - right.a;
    let db = left.b - right.b;
    (da as i128 * da as i128 + db as i128 * db as i128) as u64
}

fn finalize_face_ports(
    ports: &[FacePort],
    offset: usize,
    face: Face,
    uf: &mut SimpleUF,
    root_map: &mut fxhash::FxHashMap<usize, usize>,
    component_faces: &mut Vec<u8>,
) -> Vec<FacePort> {
    let mut out = Vec::with_capacity(ports.len());
    for port in ports {
        let root = uf.find(offset + port.component);
        let component = if let Some(&component) = root_map.get(&root) {
            component
        } else {
            let component = component_faces.len();
            root_map.insert(root, component);
            component_faces.push(0);
            component
        };
        component_faces[component] |= face_bit(face);
        out.push(FacePort {
            a: port.a,
            b: port.b,
            component,
        });
    }
    out
}

fn finalize_origin_component(
    origin_component: Option<usize>,
    uf: &mut SimpleUF,
    root_map: &mut fxhash::FxHashMap<usize, usize>,
    component_faces: &mut Vec<u8>,
) -> Option<usize> {
    origin_component.map(|origin| {
        let root = uf.find(origin);
        if let Some(&component) = root_map.get(&root) {
            component
        } else {
            let component = component_faces.len();
            root_map.insert(root, component);
            component_faces.push(0);
            component
        }
    })
}

pub fn compose_horizontal(left: &TileOperator, right: &TileOperator, k_sq: u64) -> TileOperator {
    compose_horizontal_inner(left, right, k_sq, None, 0, 0)
}

/// Compose horizontally with seam event logging.
/// `left_strip` and `right_strip` are strip indices for the SeamEvent records.
pub fn compose_horizontal_with_seams(
    left: &TileOperator,
    right: &TileOperator,
    k_sq: u64,
    seams: &mut Vec<SeamEvent>,
    left_strip: usize,
    right_strip: usize,
) -> TileOperator {
    compose_horizontal_inner(left, right, k_sq, Some(seams), left_strip, right_strip)
}

fn compose_horizontal_inner(
    left: &TileOperator,
    right: &TileOperator,
    k_sq: u64,
    mut seams: Option<&mut Vec<SeamEvent>>,
    left_strip: usize,
    right_strip: usize,
) -> TileOperator {
    let right_offset = left.num_components;
    let mut uf = SimpleUF::new(left.num_components + right.num_components);

    for left_port in &left.face_right {
        for right_port in &right.face_left {
            if port_distance_sq(left_port, right_port) <= k_sq {
                if let Some(ref mut seams) = seams {
                    let before_l = uf.find(left_port.component);
                    let before_r = uf.find(right_offset + right_port.component);
                    uf.union(left_port.component, right_offset + right_port.component);
                    let after = uf.find(left_port.component);
                    seams.push(SeamEvent {
                        left_strip,
                        right_strip,
                        left_port: (left_port.a, left_port.b),
                        right_port: (right_port.a, right_port.b),
                        merged_component_before: (before_l, before_r),
                        merged_component_after: after,
                    });
                } else {
                    uf.union(left_port.component, right_offset + right_port.component);
                }
            }
        }
    }

    let mut root_map = fxhash::FxHashMap::default();
    let mut component_faces = Vec::new();
    let face_inner = {
        let mut merged = finalize_face_ports(
            &left.face_inner,
            0,
            Face::Inner,
            &mut uf,
            &mut root_map,
            &mut component_faces,
        );
        merged.extend(finalize_face_ports(
            &right.face_inner,
            right_offset,
            Face::Inner,
            &mut uf,
            &mut root_map,
            &mut component_faces,
        ));
        merged
    };
    let face_outer = {
        let mut merged = finalize_face_ports(
            &left.face_outer,
            0,
            Face::Outer,
            &mut uf,
            &mut root_map,
            &mut component_faces,
        );
        merged.extend(finalize_face_ports(
            &right.face_outer,
            right_offset,
            Face::Outer,
            &mut uf,
            &mut root_map,
            &mut component_faces,
        ));
        merged
    };
    let face_left = finalize_face_ports(
        &left.face_left,
        0,
        Face::Left,
        &mut uf,
        &mut root_map,
        &mut component_faces,
    );
    let face_right = finalize_face_ports(
        &right.face_right,
        right_offset,
        Face::Right,
        &mut uf,
        &mut root_map,
        &mut component_faces,
    );

    let origin_component = finalize_origin_component(
        left.origin_component
            .or_else(|| right.origin_component.map(|component| right_offset + component)),
        &mut uf,
        &mut root_map,
        &mut component_faces,
    );

    TileOperator {
        a_min: left.a_min.min(right.a_min),
        a_max: left.a_max.max(right.a_max),
        b_min: left.b_min.min(right.b_min),
        b_max: left.b_max.max(right.b_max),
        face_inner,
        face_outer,
        face_left,
        face_right,
        num_components: component_faces.len(),
        component_faces,
        origin_component,
        num_primes: left.num_primes + right.num_primes,
        detail: None,
    }
}

pub fn compose_vertical(bottom: &TileOperator, top: &TileOperator, k_sq: u64) -> TileOperator {
    compose_vertical_inner(bottom, top, k_sq, None, 0, 0)
}

/// Compose vertically with seam event logging.
/// `bottom_shell` and `top_shell` are shell indices for the SeamEvent records.
pub fn compose_vertical_with_seams(
    bottom: &TileOperator,
    top: &TileOperator,
    k_sq: u64,
    seams: &mut Vec<SeamEvent>,
    bottom_shell: usize,
    top_shell: usize,
) -> TileOperator {
    compose_vertical_inner(bottom, top, k_sq, Some(seams), bottom_shell, top_shell)
}

fn compose_vertical_inner(
    bottom: &TileOperator,
    top: &TileOperator,
    k_sq: u64,
    mut seams: Option<&mut Vec<SeamEvent>>,
    bottom_shell: usize,
    top_shell: usize,
) -> TileOperator {
    let top_offset = bottom.num_components;
    let mut uf = SimpleUF::new(bottom.num_components + top.num_components);

    for bottom_port in &bottom.face_outer {
        for top_port in &top.face_inner {
            if port_distance_sq(bottom_port, top_port) <= k_sq {
                if let Some(ref mut seams) = seams {
                    let before_b = uf.find(bottom_port.component);
                    let before_t = uf.find(top_offset + top_port.component);
                    uf.union(bottom_port.component, top_offset + top_port.component);
                    let after = uf.find(bottom_port.component);
                    seams.push(SeamEvent {
                        left_strip: bottom_shell,
                        right_strip: top_shell,
                        left_port: (bottom_port.a, bottom_port.b),
                        right_port: (top_port.a, top_port.b),
                        merged_component_before: (before_b, before_t),
                        merged_component_after: after,
                    });
                } else {
                    uf.union(bottom_port.component, top_offset + top_port.component);
                }
            }
        }
    }

    let mut root_map = fxhash::FxHashMap::default();
    let mut component_faces = Vec::new();
    let face_inner = finalize_face_ports(
        &bottom.face_inner,
        0,
        Face::Inner,
        &mut uf,
        &mut root_map,
        &mut component_faces,
    );
    let face_outer = finalize_face_ports(
        &top.face_outer,
        top_offset,
        Face::Outer,
        &mut uf,
        &mut root_map,
        &mut component_faces,
    );
    let face_left = {
        let mut merged = finalize_face_ports(
            &bottom.face_left,
            0,
            Face::Left,
            &mut uf,
            &mut root_map,
            &mut component_faces,
        );
        merged.extend(finalize_face_ports(
            &top.face_left,
            top_offset,
            Face::Left,
            &mut uf,
            &mut root_map,
            &mut component_faces,
        ));
        merged
    };
    let face_right = {
        let mut merged = finalize_face_ports(
            &bottom.face_right,
            0,
            Face::Right,
            &mut uf,
            &mut root_map,
            &mut component_faces,
        );
        merged.extend(finalize_face_ports(
            &top.face_right,
            top_offset,
            Face::Right,
            &mut uf,
            &mut root_map,
            &mut component_faces,
        ));
        merged
    };

    let origin_component = finalize_origin_component(
        bottom.origin_component
            .or_else(|| top.origin_component.map(|component| top_offset + component)),
        &mut uf,
        &mut root_map,
        &mut component_faces,
    );

    TileOperator {
        a_min: bottom.a_min.min(top.a_min),
        a_max: bottom.a_max.max(top.a_max),
        b_min: bottom.b_min.min(top.b_min),
        b_max: bottom.b_max.max(top.b_max),
        face_inner,
        face_outer,
        face_left,
        face_right,
        num_components: component_faces.len(),
        component_faces,
        origin_component,
        num_primes: bottom.num_primes + top.num_primes,
        detail: None,
    }
}

pub fn compose_grid(mut grid: Vec<Vec<TileOperator>>, k_sq: u64) -> TileOperator {
    assert!(!grid.is_empty(), "grid must not be empty");
    assert!(!grid[0].is_empty(), "grid rows must not be empty");

    // All rows must have equal length. Without this check, the vertical
    // composition step uses zip() which silently drops tiles on ragged rows,
    // producing silently wrong results.
    let expected_cols = grid[0].len();
    for (row_idx, row) in grid.iter().enumerate() {
        assert_eq!(
            row.len(),
            expected_cols,
            "compose_grid: row {} has {} columns, expected {} (ragged grid not supported)",
            row_idx,
            row.len(),
            expected_cols,
        );
    }

    while grid.len() > 1 || grid[0].len() > 1 {
        if grid[0].len() > 1 {
            grid = grid
                .into_par_iter()
                .map(|row| {
                    let mut next_row = Vec::with_capacity(row.len().div_ceil(2));
                    let mut iter = row.into_iter();
                    while let Some(left) = iter.next() {
                        if let Some(right) = iter.next() {
                            next_row.push(compose_horizontal(&left, &right, k_sq));
                        } else {
                            next_row.push(left);
                        }
                    }
                    next_row
                })
                .collect();
        }

        if grid.len() > 1 {
            let mut next_grid = Vec::with_capacity(grid.len().div_ceil(2));
            let mut rows = grid.into_iter();
            while let Some(bottom_row) = rows.next() {
                if let Some(top_row) = rows.next() {
                    let merged_row: Vec<_> = bottom_row
                        .into_par_iter()
                        .zip(top_row.into_par_iter())
                        .map(|(bottom, top)| compose_vertical(&bottom, &top, k_sq))
                        .collect();
                    next_grid.push(merged_row);
                } else {
                    next_grid.push(bottom_row);
                }
            }
            grid = next_grid;
        }
    }

    grid.pop().unwrap().pop().unwrap()
}

/// Like `compose_grid` but collects SeamEvent records for L<->R composition.
/// Only does single-row grids (the common case in probe: one row of tiles per shell).
pub fn compose_grid_with_seams(
    tiles: Vec<TileOperator>,
    k_sq: u64,
    seams: &mut Vec<SeamEvent>,
) -> TileOperator {
    assert!(!tiles.is_empty(), "tiles must not be empty");

    if tiles.len() == 1 {
        return tiles.into_iter().next().unwrap();
    }

    // Sequential pairwise reduction with seam logging (cannot parallelize seam collection)
    let mut iter = tiles.into_iter().enumerate();
    let (_, mut acc) = iter.next().unwrap();
    let mut acc_left_strip = 0_usize;

    for (right_strip, right) in iter {
        acc = compose_horizontal_with_seams(&acc, &right, k_sq, seams, acc_left_strip, right_strip);
        acc_left_strip = right_strip;
    }

    acc
}

#[cfg(test)]
mod tests {
    use crate::tile::{build_tile, FACE_OUTER_BIT, FACE_RIGHT_BIT};

    use super::{compose_grid, compose_horizontal, compose_vertical};

    #[test]
    fn horizontal_composition_merges_across_strip_seam() {
        let left = build_tile(0, 2, 0, 1, 2);
        let right = build_tile(0, 2, 2, 3, 2);
        let merged = compose_horizontal(&left, &right, 2);

        let origin_component = merged.origin_component.expect("origin component should exist");
        assert_ne!(merged.num_components, 0);
        assert!(merged.component_faces[origin_component] & FACE_RIGHT_BIT != 0);
        assert_eq!(merged.b_min, 0);
        assert_eq!(merged.b_max, 3);
    }

    #[test]
    fn vertical_composition_merges_across_shell_seam() {
        let bottom = compose_horizontal(&build_tile(0, 2, 0, 1, 2), &build_tile(0, 2, 2, 3, 2), 2);
        let top = compose_horizontal(&build_tile(3, 5, 0, 1, 2), &build_tile(3, 5, 2, 3, 2), 2);
        let merged = compose_vertical(&bottom, &top, 2);

        let origin_component = merged.origin_component.expect("origin component should exist");
        assert!(merged.component_faces[origin_component] & FACE_OUTER_BIT != 0);
        assert_eq!(merged.a_min, 0);
        assert_eq!(merged.a_max, 5);
    }

    #[test]
    fn grid_composition_reduces_two_by_two_tiles() {
        let grid = vec![
            vec![build_tile(0, 2, 0, 1, 2), build_tile(0, 2, 2, 3, 2)],
            vec![build_tile(3, 5, 0, 1, 2), build_tile(3, 5, 2, 3, 2)],
        ];
        let merged = compose_grid(grid, 2);

        let origin_component = merged.origin_component.expect("origin component should exist");
        assert!(merged.component_faces[origin_component] & FACE_OUTER_BIT != 0);
        assert_eq!(merged.b_max, 3);
    }

    /// Ragged grid input must panic (previously zip would silently truncate).
    #[test]
    #[should_panic(expected = "ragged grid not supported")]
    fn compose_grid_rejects_ragged_input() {
        let grid = vec![
            vec![build_tile(0, 2, 0, 1, 2), build_tile(0, 2, 2, 3, 2)], // 2 columns
            vec![build_tile(3, 5, 0, 1, 2)],                              // 1 column
        ];
        compose_grid(grid, 2);
    }
}
