use std::collections::BTreeSet;
use std::path::PathBuf;

use fat_stripe_cuda::{CudaDriver, TileJob};
use moat_kernel::tile::{build_tile, FacePort, TileOperator};
use tempfile::tempdir;

#[derive(Debug, Clone, Copy)]
struct TileCase {
    name: &'static str,
    a_lo: i64,
    b_lo: i64,
    side: u32,
    k_sq: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum FaceLabel {
    Inner,
    Outer,
    Left,
    Right,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
struct PortKey {
    face: FaceLabel,
    a: i64,
    b: i64,
}

#[test]
fn cuda_face_ports_match_cpu_tiles() {
    let Some(binary_path) = cuda_binary_from_env() else {
        eprintln!("skipping differential test: FAT_STRIPE_CUDA_BIN is not set");
        return;
    };

    let driver = CudaDriver {
        binary_path,
        device: 0,
        batch_size: 1,
    };

    let cases = [
        TileCase {
            name: "small-origin",
            a_lo: 0,
            b_lo: 0,
            side: 10,
            k_sq: 2,
        },
        TileCase {
            name: "medium-offset",
            a_lo: 1_000,
            b_lo: 1_000,
            side: 200,
            k_sq: 40,
        },
    ];

    for case in cases {
        assert_case_matches(&driver, case);
    }
}

fn assert_case_matches(driver: &CudaDriver, case: TileCase) {
    let work_dir = tempdir().expect("create temp dir for CUDA differential test");
    let jobs = [TileJob {
        tile_id: 0,
        a_lo: i32::try_from(case.a_lo).expect("a_lo must fit in i32"),
        b_lo: i32::try_from(case.b_lo).expect("b_lo must fit in i32"),
        reserved: 0,
    }];

    let mut gpu_tiles = driver
        .process_batch(work_dir.path(), case.k_sq, case.side, &jobs)
        .unwrap_or_else(|err| panic!("CUDA batch failed for {}: {err}", case.name));
    assert_eq!(gpu_tiles.len(), 1, "expected one CUDA tile for {}", case.name);
    let gpu = gpu_tiles.pop().unwrap();

    let cpu = build_tile(
        case.a_lo,
        case.a_lo + i64::from(case.side),
        case.b_lo,
        case.b_lo + i64::from(case.side),
        case.k_sq,
    );

    assert_eq!(
        gpu.num_primes, cpu.num_primes,
        "num_primes mismatch for {}",
        case.name
    );
    assert_eq!(
        gpu.num_components, cpu.num_components,
        "num_components mismatch for {}",
        case.name
    );
    assert_eq!(
        face_keys(&gpu),
        face_keys(&cpu),
        "face-port coordinates mismatch for {}",
        case.name
    );
    assert_eq!(
        component_partitions(&gpu),
        component_partitions(&cpu),
        "face-port partition mismatch for {}",
        case.name
    );
    assert_eq!(
        origin_face_partition(&gpu),
        origin_face_partition(&cpu),
        "origin face-port partition mismatch for {}",
        case.name
    );
}

fn cuda_binary_from_env() -> Option<PathBuf> {
    let path = PathBuf::from(std::env::var_os("FAT_STRIPE_CUDA_BIN")?);
    assert!(
        path.is_file(),
        "FAT_STRIPE_CUDA_BIN does not point to a file: {}",
        path.display()
    );
    Some(path)
}

fn face_keys(op: &TileOperator) -> BTreeSet<PortKey> {
    let mut keys = BTreeSet::new();
    extend_face_keys(&mut keys, FaceLabel::Inner, &op.face_inner);
    extend_face_keys(&mut keys, FaceLabel::Outer, &op.face_outer);
    extend_face_keys(&mut keys, FaceLabel::Left, &op.face_left);
    extend_face_keys(&mut keys, FaceLabel::Right, &op.face_right);
    keys
}

fn extend_face_keys(keys: &mut BTreeSet<PortKey>, face: FaceLabel, ports: &[FacePort]) {
    for port in ports {
        keys.insert(PortKey {
            face,
            a: port.a,
            b: port.b,
        });
    }
}

fn component_partitions(op: &TileOperator) -> Vec<Vec<PortKey>> {
    let mut groups = vec![Vec::new(); op.num_components];
    push_component_ports(&mut groups, FaceLabel::Inner, &op.face_inner);
    push_component_ports(&mut groups, FaceLabel::Outer, &op.face_outer);
    push_component_ports(&mut groups, FaceLabel::Left, &op.face_left);
    push_component_ports(&mut groups, FaceLabel::Right, &op.face_right);

    let mut non_empty: Vec<Vec<PortKey>> = groups
        .into_iter()
        .filter(|group| !group.is_empty())
        .map(|mut group| {
            group.sort();
            group
        })
        .collect();
    non_empty.sort();
    non_empty
}

fn push_component_ports(groups: &mut [Vec<PortKey>], face: FaceLabel, ports: &[FacePort]) {
    for port in ports {
        groups[port.component].push(PortKey {
            face,
            a: port.a,
            b: port.b,
        });
    }
}

fn origin_face_partition(op: &TileOperator) -> Option<Vec<PortKey>> {
    let component = op.origin_component?;
    let mut group = Vec::new();
    collect_origin_ports(&mut group, component, FaceLabel::Inner, &op.face_inner);
    collect_origin_ports(&mut group, component, FaceLabel::Outer, &op.face_outer);
    collect_origin_ports(&mut group, component, FaceLabel::Left, &op.face_left);
    collect_origin_ports(&mut group, component, FaceLabel::Right, &op.face_right);
    group.sort();
    Some(group)
}

fn collect_origin_ports(
    group: &mut Vec<PortKey>,
    component: usize,
    face: FaceLabel,
    ports: &[FacePort],
) {
    for port in ports {
        if port.component == component {
            group.push(PortKey {
                face,
                a: port.a,
                b: port.b,
            });
        }
    }
}
