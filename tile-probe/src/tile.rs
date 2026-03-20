use fxhash::FxHashMap;

use crate::primes::gaussian_primes_in_rect;

pub const FACE_INNER_BIT: FaceSet = 1 << 0;
pub const FACE_OUTER_BIT: FaceSet = 1 << 1;
pub const FACE_LEFT_BIT: FaceSet = 1 << 2;
pub const FACE_RIGHT_BIT: FaceSet = 1 << 3;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Face {
    Inner,
    Outer,
    Left,
    Right,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FacePort {
    pub a: i64,
    pub b: i64,
    pub component: usize,
}

pub type FaceSet = u8;

#[derive(Debug, Clone)]
pub struct TileOperator {
    pub a_min: i64,
    pub a_max: i64,
    pub b_min: i64,
    pub b_max: i64,
    pub face_inner: Vec<FacePort>,
    pub face_outer: Vec<FacePort>,
    pub face_left: Vec<FacePort>,
    pub face_right: Vec<FacePort>,
    pub num_components: usize,
    pub component_faces: Vec<FaceSet>,
    pub origin_component: Option<usize>,
    pub num_primes: usize,
}

#[derive(Debug, Clone)]
pub(crate) struct SimpleUF {
    parent: Vec<usize>,
    rank: Vec<u8>,
}

impl SimpleUF {
    pub(crate) fn new(n: usize) -> Self {
        let parent = (0..n).collect();
        Self {
            parent,
            rank: vec![0; n],
        }
    }

    pub(crate) fn find(&mut self, x: usize) -> usize {
        if self.parent[x] != x {
            let root = self.find(self.parent[x]);
            self.parent[x] = root;
        }
        self.parent[x]
    }

    pub(crate) fn union(&mut self, x: usize, y: usize) {
        let rx = self.find(x);
        let ry = self.find(y);
        if rx == ry {
            return;
        }

        if self.rank[rx] < self.rank[ry] {
            self.parent[rx] = ry;
        } else if self.rank[rx] > self.rank[ry] {
            self.parent[ry] = rx;
        } else {
            self.parent[ry] = rx;
            self.rank[rx] += 1;
        }
    }
}

pub(crate) fn face_bit(face: Face) -> FaceSet {
    match face {
        Face::Inner => FACE_INNER_BIT,
        Face::Outer => FACE_OUTER_BIT,
        Face::Left => FACE_LEFT_BIT,
        Face::Right => FACE_RIGHT_BIT,
    }
}

fn cell_key(a: i64, b: i64, cell_size: i64) -> (i64, i64) {
    (a.div_euclid(cell_size), b.div_euclid(cell_size))
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

pub fn build_tile(a_min: i64, a_max: i64, b_min: i64, b_max: i64, k_sq: u64) -> TileOperator {
    let collar = (k_sq as f64).sqrt().ceil() as i64;
    let expanded_a_min = a_min - collar;
    let expanded_a_max = a_max + collar;
    let expanded_b_min = b_min - collar;
    let expanded_b_max = b_max + collar;

    let primes = gaussian_primes_in_rect(expanded_a_min, expanded_a_max, expanded_b_min, expanded_b_max);
    let num_primes = primes.len();

    if primes.is_empty() {
        return TileOperator {
            a_min,
            a_max,
            b_min,
            b_max,
            face_inner: Vec::new(),
            face_outer: Vec::new(),
            face_left: Vec::new(),
            face_right: Vec::new(),
            num_components: 0,
            component_faces: Vec::new(),
            origin_component: None,
            num_primes,
        };
    }

    let cell_size = collar.max(1);
    let mut cells: FxHashMap<(i64, i64), Vec<usize>> = FxHashMap::default();
    for (idx, &(a, b)) in primes.iter().enumerate() {
        cells.entry(cell_key(a, b, cell_size)).or_default().push(idx);
    }

    let mut uf = SimpleUF::new(primes.len());
    for (idx, &(a, b)) in primes.iter().enumerate() {
        let (cx, cy) = cell_key(a, b, cell_size);
        for dcx in -2..=2 {
            for dcy in -2..=2 {
                if let Some(neighbors) = cells.get(&(cx + dcx, cy + dcy)) {
                    for &other in neighbors {
                        if other <= idx {
                            continue;
                        }
                        let (oa, ob) = primes[other];
                        let da = oa - a;
                        let db = ob - b;
                        let dist_sq = (da as i128 * da as i128 + db as i128 * db as i128) as u64;
                        if dist_sq <= k_sq {
                            uf.union(idx, other);
                        }
                    }
                }
            }
        }
    }

    let mut origin_component = None;
    if a_min <= 0 && 0 <= a_max && b_min <= 0 && 0 <= b_max {
        let origin_neighbors: Vec<_> = primes
            .iter()
            .enumerate()
            .filter_map(|(idx, &(a, b))| {
                let norm = (a as i128 * a as i128 + b as i128 * b as i128) as u64;
                (norm <= k_sq).then_some(idx)
            })
            .collect();

        if let Some((&first, rest)) = origin_neighbors.split_first() {
            for &idx in rest {
                uf.union(first, idx);
            }
            origin_component = Some(uf.find(first));
        }
    }

    let mut face_inner = Vec::new();
    let mut face_outer = Vec::new();
    let mut face_left = Vec::new();
    let mut face_right = Vec::new();
    let mut component_faces = Vec::new();
    let mut root_map: FxHashMap<usize, usize> = FxHashMap::default();

    for (idx, &(a, b)) in primes.iter().enumerate() {
        if a < a_min || a > a_max || b < b_min || b > b_max {
            continue;
        }

        let root = uf.find(idx);
        let component = component_id_for_root(root, &mut root_map, &mut component_faces);

        if a - a_min < collar {
            face_inner.push(FacePort { a, b, component });
            component_faces[component] |= FACE_INNER_BIT;
        }
        if a_max - a < collar {
            face_outer.push(FacePort { a, b, component });
            component_faces[component] |= FACE_OUTER_BIT;
        }
        if b - b_min < collar {
            face_left.push(FacePort { a, b, component });
            component_faces[component] |= FACE_LEFT_BIT;
        }
        if b_max - b < collar {
            face_right.push(FacePort { a, b, component });
            component_faces[component] |= FACE_RIGHT_BIT;
        }
    }

    let origin_component = origin_component.map(|root| {
        component_id_for_root(root, &mut root_map, &mut component_faces)
    });

    TileOperator {
        a_min,
        a_max,
        b_min,
        b_max,
        face_inner,
        face_outer,
        face_left,
        face_right,
        num_components: component_faces.len(),
        component_faces,
        origin_component,
        num_primes,
    }
}

#[cfg(test)]
mod tests {
    use super::{build_tile, FACE_INNER_BIT, FACE_LEFT_BIT, FACE_OUTER_BIT, FACE_RIGHT_BIT};

    #[test]
    fn tile_around_origin_records_origin_component() {
        let tile = build_tile(-1, 1, -1, 1, 2);

        assert_eq!(tile.num_primes, 24);
        assert!(tile.origin_component.is_some());
        assert_eq!(tile.face_inner.len(), 2);
        assert_eq!(tile.face_outer.len(), 2);
        assert_eq!(tile.face_left.len(), 2);
        assert_eq!(tile.face_right.len(), 2);

        let origin_component = tile.origin_component.unwrap();
        assert_eq!(
            tile.component_faces[origin_component],
            FACE_INNER_BIT | FACE_OUTER_BIT | FACE_LEFT_BIT | FACE_RIGHT_BIT
        );
    }

    #[test]
    fn tile_face_ports_follow_collar_rules() {
        let tile = build_tile(0, 2, 0, 2, 2);

        let all_inner: Vec<_> = tile.face_inner.iter().map(|p| (p.a, p.b)).collect();
        let all_outer: Vec<_> = tile.face_outer.iter().map(|p| (p.a, p.b)).collect();
        let all_left: Vec<_> = tile.face_left.iter().map(|p| (p.a, p.b)).collect();
        let all_right: Vec<_> = tile.face_right.iter().map(|p| (p.a, p.b)).collect();

        assert!(all_inner.contains(&(1, 1)));
        assert!(all_inner.contains(&(1, 2)));
        assert!(all_outer.contains(&(1, 1)));
        assert!(all_outer.contains(&(1, 2)));
        assert!(all_outer.contains(&(2, 1)));
        assert!(all_left.contains(&(1, 1)));
        assert!(all_left.contains(&(2, 1)));
        assert!(all_right.contains(&(1, 1)));
        assert!(all_right.contains(&(1, 2)));
        assert!(all_right.contains(&(2, 1)));
    }
}
