#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Face {
    Inner,
    Outer,
    Left,
    Right,
}

#[derive(Debug, Clone)]
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

pub fn build_tile(a_min: i64, a_max: i64, b_min: i64, b_max: i64, _k_sq: u64) -> TileOperator {
    TileOperator {
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
        num_primes: 0,
    }
}
