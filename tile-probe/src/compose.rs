use rayon::prelude::*;

use crate::tile::{face_bit, Face, FacePort, SimpleUF, TileOperator};

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
    let right_offset = left.num_components;
    let mut uf = SimpleUF::new(left.num_components + right.num_components);

    for left_port in &left.face_right {
        for right_port in &right.face_left {
            if port_distance_sq(left_port, right_port) <= k_sq {
                uf.union(left_port.component, right_offset + right_port.component);
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
            .map(|component| component)
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
    }
}

pub fn compose_vertical(bottom: &TileOperator, top: &TileOperator, k_sq: u64) -> TileOperator {
    let top_offset = bottom.num_components;
    let mut uf = SimpleUF::new(bottom.num_components + top.num_components);

    for bottom_port in &bottom.face_outer {
        for top_port in &top.face_inner {
            if port_distance_sq(bottom_port, top_port) <= k_sq {
                uf.union(bottom_port.component, top_offset + top_port.component);
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
            .map(|component| component)
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
    }
}

pub fn compose_grid(mut grid: Vec<Vec<TileOperator>>, k_sq: u64) -> TileOperator {
    assert!(!grid.is_empty(), "grid must not be empty");
    assert!(!grid[0].is_empty(), "grid rows must not be empty");

    while grid.len() > 1 || grid[0].len() > 1 {
        if grid[0].len() > 1 {
            grid = grid
                .into_par_iter()
                .map(|row| {
                    let mut next_row = Vec::with_capacity((row.len() + 1) / 2);
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
            let mut next_grid = Vec::with_capacity((grid.len() + 1) / 2);
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
}
