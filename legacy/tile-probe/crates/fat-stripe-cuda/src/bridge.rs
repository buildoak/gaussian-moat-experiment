use moat_kernel::tile::{
    FacePort, TileOperator, FACE_INNER_BIT, FACE_LEFT_BIT, FACE_OUTER_BIT, FACE_RIGHT_BIT,
};

use crate::protocol::{TilePorts, TileResultHeader};

pub fn tile_operator_from_raw(
    header: TileResultHeader,
    ports: TilePorts,
    k_sq: u64,
) -> TileOperator {
    let _ = k_sq;

    let mut component_faces = vec![0u8; header.num_components as usize];

    for port in &ports.inner {
        component_faces[port.component_id as usize] |= FACE_INNER_BIT;
    }
    for port in &ports.outer {
        component_faces[port.component_id as usize] |= FACE_OUTER_BIT;
    }
    for port in &ports.left {
        component_faces[port.component_id as usize] |= FACE_LEFT_BIT;
    }
    for port in &ports.right {
        component_faces[port.component_id as usize] |= FACE_RIGHT_BIT;
    }

    TileOperator {
        a_min: header.a_lo as i64,
        a_max: (header.a_lo + header.side as i32) as i64,
        b_min: header.b_lo as i64,
        b_max: (header.b_lo + header.side as i32) as i64,
        face_inner: convert_ports(ports.inner),
        face_outer: convert_ports(ports.outer),
        face_left: convert_ports(ports.left),
        face_right: convert_ports(ports.right),
        num_components: header.num_components as usize,
        component_faces,
        component_sizes: Vec::new(),
        origin_component: (header.origin_component >= 0)
            .then_some(header.origin_component as usize),
        num_primes: header.num_primes as usize,
        detail: None,
    }
}

fn convert_ports(records: Vec<crate::protocol::FacePortRecord>) -> Vec<FacePort> {
    records
        .into_iter()
        .map(|record| FacePort {
            a: record.a as i64,
            b: record.b as i64,
            component: record.component_id as usize,
        })
        .collect()
}
