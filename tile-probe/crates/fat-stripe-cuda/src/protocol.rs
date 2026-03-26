use std::fmt;
use std::io::{self, Read, Write};
use std::mem::size_of;

use bytemuck::{bytes_of, cast_slice, cast_slice_mut, pod_read_unaligned, Pod, Zeroable};

pub const JOB_MAGIC: [u8; 4] = *b"GMTJ";
pub const STREAM_MAGIC: [u8; 4] = *b"GMFP";
pub const PROTOCOL_VERSION: u16 = 1;
pub const STREAM_FLAG_CAMPAIGN_SUMMARY: u16 = 1u16 << 0;

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Pod, Zeroable)]
pub struct FatStripeJobHeader {
    pub magic: [u8; 4],
    pub version: u16,
    pub flags: u16,
    pub k_sq: u64,
    pub tile_side: u32,
    pub num_jobs: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Pod, Zeroable)]
pub struct TileJob {
    pub tile_id: u32,
    pub a_lo: i32,
    pub b_lo: i32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Pod, Zeroable)]
pub struct FacePortStreamHeader {
    pub magic: [u8; 4],
    pub version: u16,
    pub flags: u16,
    pub k_sq: u64,
    pub tile_side: u32,
    pub num_tiles: u32,
    pub reserved: u64,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Pod, Zeroable)]
pub struct TileResultHeader {
    pub tile_id: u32,
    pub a_lo: i32,
    pub b_lo: i32,
    pub side: u32,
    pub num_components: u32,
    pub num_face_inner: u32,
    pub num_face_outer: u32,
    pub num_face_left: u32,
    pub num_face_right: u32,
    pub num_primes: u32,
    pub origin_component: i32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Pod, Zeroable)]
pub struct FacePortRecord {
    pub a: i32,
    pub b: i32,
    pub component_id: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Pod, Zeroable)]
pub struct CampaignSummary {
    pub total_primes: u64,
    pub num_tiles: u32,
    pub num_components: u32,
    pub spanning_component: i32,
    pub reserved: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct TilePorts {
    pub inner: Vec<FacePortRecord>,
    pub outer: Vec<FacePortRecord>,
    pub left: Vec<FacePortRecord>,
    pub right: Vec<FacePortRecord>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawTileResult {
    pub header: TileResultHeader,
    pub ports: TilePorts,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FacePortStream {
    pub header: FacePortStreamHeader,
    pub tiles: Vec<RawTileResult>,
}

#[derive(Debug)]
pub enum ProtocolError {
    Io(io::Error),
    InvalidMagic {
        expected: [u8; 4],
        actual: [u8; 4],
        context: &'static str,
    },
    UnsupportedVersion {
        expected: u16,
        actual: u16,
        context: &'static str,
    },
    CountTooLarge {
        field: &'static str,
        value: u64,
    },
    InvalidData(String),
}

impl fmt::Display for ProtocolError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(err) => write!(f, "{err}"),
            Self::InvalidMagic {
                expected,
                actual,
                context,
            } => write!(
                f,
                "{context} magic mismatch: expected {:?}, got {:?}",
                std::str::from_utf8(expected).unwrap_or("????"),
                std::str::from_utf8(actual).unwrap_or("????")
            ),
            Self::UnsupportedVersion {
                expected,
                actual,
                context,
            } => write!(
                f,
                "{context} version mismatch: expected {expected}, got {actual}"
            ),
            Self::CountTooLarge { field, value } => {
                write!(f, "count too large for {field}: {value}")
            }
            Self::InvalidData(message) => write!(f, "{message}"),
        }
    }
}

impl std::error::Error for ProtocolError {}

impl From<io::Error> for ProtocolError {
    fn from(value: io::Error) -> Self {
        Self::Io(value)
    }
}

pub fn write_job_manifest(
    writer: &mut impl Write,
    k_sq: u64,
    tile_side: u32,
    jobs: &[TileJob],
) -> Result<(), ProtocolError> {
    let num_jobs = u32::try_from(jobs.len()).map_err(|_| ProtocolError::CountTooLarge {
        field: "num_jobs",
        value: jobs.len() as u64,
    })?;

    let header = FatStripeJobHeader {
        magic: JOB_MAGIC,
        version: PROTOCOL_VERSION,
        flags: 0,
        k_sq,
        tile_side,
        num_jobs,
    };

    writer.write_all(bytes_of(&header))?;
    writer.write_all(cast_slice(jobs))?;
    Ok(())
}

pub fn read_job_manifest(
    reader: &mut impl Read,
) -> Result<(FatStripeJobHeader, Vec<TileJob>), ProtocolError> {
    let header = read_pod::<FatStripeJobHeader>(reader)?;
    validate_magic(header.magic, JOB_MAGIC, "job manifest")?;
    validate_version(header.version, PROTOCOL_VERSION, "job manifest")?;
    let jobs = read_pod_vec::<TileJob>(reader, usize_from_u32("num_jobs", header.num_jobs)?)?;
    Ok((header, jobs))
}

pub fn read_stream_header(reader: &mut impl Read) -> Result<FacePortStreamHeader, ProtocolError> {
    let header = read_pod::<FacePortStreamHeader>(reader)?;
    validate_magic(header.magic, STREAM_MAGIC, "face-port stream")?;
    validate_version(header.version, PROTOCOL_VERSION, "face-port stream")?;
    Ok(header)
}

pub fn read_tile_result(reader: &mut impl Read) -> Result<RawTileResult, ProtocolError> {
    let header = read_pod::<TileResultHeader>(reader)?;
    let ports = TilePorts {
        inner: read_ports(reader, header.num_face_inner)?,
        outer: read_ports(reader, header.num_face_outer)?,
        left: read_ports(reader, header.num_face_left)?,
        right: read_ports(reader, header.num_face_right)?,
    };

    validate_component_ids(&header, &ports)?;

    Ok(RawTileResult { header, ports })
}

pub fn read_all_tiles(reader: &mut impl Read) -> Result<FacePortStream, ProtocolError> {
    let header = read_stream_header(reader)?;
    if header.flags & STREAM_FLAG_CAMPAIGN_SUMMARY != 0 {
        return Err(ProtocolError::InvalidData(
            "stream is a campaign summary, not tile results".to_string(),
        ));
    }
    let mut tiles = Vec::with_capacity(usize_from_u32("num_tiles", header.num_tiles)?);
    for _ in 0..header.num_tiles {
        tiles.push(read_tile_result(reader)?);
    }
    Ok(FacePortStream { header, tiles })
}

pub fn read_campaign_summary(
    reader: &mut impl Read,
) -> Result<(FacePortStreamHeader, CampaignSummary), ProtocolError> {
    let header = read_stream_header(reader)?;
    if header.flags & STREAM_FLAG_CAMPAIGN_SUMMARY == 0 {
        return Err(ProtocolError::InvalidData(
            "stream does not contain campaign summary flag".to_string(),
        ));
    }
    let summary = read_pod::<CampaignSummary>(reader)?;
    Ok((header, summary))
}

fn read_ports(reader: &mut impl Read, count: u32) -> Result<Vec<FacePortRecord>, ProtocolError> {
    read_pod_vec::<FacePortRecord>(reader, usize_from_u32("face_port_count", count)?)
}

fn read_pod<T: Pod>(reader: &mut impl Read) -> Result<T, ProtocolError> {
    let mut buf = vec![0u8; size_of::<T>()];
    reader.read_exact(&mut buf)?;
    Ok(pod_read_unaligned(&buf))
}

fn read_pod_vec<T: Pod + Zeroable>(
    reader: &mut impl Read,
    count: usize,
) -> Result<Vec<T>, ProtocolError> {
    let mut items = vec![T::zeroed(); count];
    reader.read_exact(cast_slice_mut(&mut items))?;
    Ok(items)
}

fn validate_magic(
    actual: [u8; 4],
    expected: [u8; 4],
    context: &'static str,
) -> Result<(), ProtocolError> {
    if actual == expected {
        Ok(())
    } else {
        Err(ProtocolError::InvalidMagic {
            expected,
            actual,
            context,
        })
    }
}

fn validate_version(
    actual: u16,
    expected: u16,
    context: &'static str,
) -> Result<(), ProtocolError> {
    if actual == expected {
        Ok(())
    } else {
        Err(ProtocolError::UnsupportedVersion {
            expected,
            actual,
            context,
        })
    }
}

fn usize_from_u32(field: &'static str, value: u32) -> Result<usize, ProtocolError> {
    usize::try_from(value).map_err(|_| ProtocolError::CountTooLarge {
        field,
        value: value as u64,
    })
}

fn validate_component_ids(
    header: &TileResultHeader,
    ports: &TilePorts,
) -> Result<(), ProtocolError> {
    let limit = header.num_components;
    for port in ports
        .inner
        .iter()
        .chain(ports.outer.iter())
        .chain(ports.left.iter())
        .chain(ports.right.iter())
    {
        if port.component_id >= limit {
            return Err(ProtocolError::InvalidData(format!(
                "component_id {} out of range for tile {} with {} components",
                port.component_id, header.tile_id, header.num_components
            )));
        }
    }

    if header.origin_component >= header.num_components as i32 {
        return Err(ProtocolError::InvalidData(format!(
            "origin_component {} out of range for tile {} with {} components",
            header.origin_component, header.tile_id, header.num_components
        )));
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use std::io::Cursor;
    use std::mem::size_of;

    use bytemuck::{bytes_of, cast_slice};
    use moat_kernel::tile::{FACE_INNER_BIT, FACE_LEFT_BIT, FACE_OUTER_BIT, FACE_RIGHT_BIT};

    use crate::bridge::tile_operator_from_raw;

    use super::{
        read_all_tiles, read_job_manifest, read_stream_header, read_tile_result,
        write_job_manifest, CampaignSummary, FacePortRecord, FacePortStreamHeader,
        FatStripeJobHeader, TileJob, TilePorts, TileResultHeader, JOB_MAGIC, PROTOCOL_VERSION,
        STREAM_MAGIC,
    };

    #[test]
    fn protocol_struct_sizes_match_spec() {
        assert_eq!(size_of::<FatStripeJobHeader>(), 24);
        assert_eq!(size_of::<TileJob>(), 16);
        assert_eq!(size_of::<FacePortStreamHeader>(), 32);
        assert_eq!(size_of::<TileResultHeader>(), 44);
        assert_eq!(size_of::<FacePortRecord>(), 12);
        assert_eq!(size_of::<CampaignSummary>(), 24);
    }

    #[test]
    fn round_trip_job_manifest() {
        let jobs = vec![
            TileJob {
                tile_id: 10,
                a_lo: 100,
                b_lo: 200,
                reserved: 0,
            },
            TileJob {
                tile_id: 11,
                a_lo: 300,
                b_lo: 400,
                reserved: 0,
            },
        ];

        let mut buf = Vec::new();
        write_job_manifest(&mut buf, 40, 2000, &jobs).unwrap();

        let (header, parsed_jobs) = read_job_manifest(&mut Cursor::new(buf)).unwrap();
        assert_eq!(
            header,
            FatStripeJobHeader {
                magic: JOB_MAGIC,
                version: PROTOCOL_VERSION,
                flags: 0,
                k_sq: 40,
                tile_side: 2000,
                num_jobs: 2,
            }
        );
        assert_eq!(parsed_jobs, jobs);
    }

    #[test]
    fn round_trip_face_port_stream() {
        let stream_header = FacePortStreamHeader {
            magic: STREAM_MAGIC,
            version: PROTOCOL_VERSION,
            flags: 0,
            k_sq: 40,
            tile_side: 2000,
            num_tiles: 2,
            reserved: 0,
        };

        let tile0 = TileResultHeader {
            tile_id: 0,
            a_lo: 100,
            b_lo: 200,
            side: 2000,
            num_components: 2,
            num_face_inner: 1,
            num_face_outer: 1,
            num_face_left: 0,
            num_face_right: 1,
            num_primes: 17,
            origin_component: -1,
        };
        let tile0_ports = TilePorts {
            inner: vec![FacePortRecord {
                a: 101,
                b: 202,
                component_id: 0,
            }],
            outer: vec![FacePortRecord {
                a: 110,
                b: 220,
                component_id: 1,
            }],
            left: vec![],
            right: vec![FacePortRecord {
                a: 130,
                b: 240,
                component_id: 1,
            }],
        };

        let tile1 = TileResultHeader {
            tile_id: 1,
            a_lo: 300,
            b_lo: 400,
            side: 2000,
            num_components: 1,
            num_face_inner: 0,
            num_face_outer: 1,
            num_face_left: 1,
            num_face_right: 1,
            num_primes: 23,
            origin_component: 0,
        };
        let tile1_ports = TilePorts {
            inner: vec![],
            outer: vec![FacePortRecord {
                a: 305,
                b: 405,
                component_id: 0,
            }],
            left: vec![FacePortRecord {
                a: 306,
                b: 406,
                component_id: 0,
            }],
            right: vec![FacePortRecord {
                a: 307,
                b: 407,
                component_id: 0,
            }],
        };

        let mut buf = Vec::new();
        buf.extend_from_slice(bytes_of(&stream_header));
        append_tile(&mut buf, tile0, &tile0_ports);
        append_tile(&mut buf, tile1, &tile1_ports);

        let parsed = read_all_tiles(&mut Cursor::new(buf)).unwrap();
        assert_eq!(parsed.header, stream_header);
        assert_eq!(parsed.tiles.len(), 2);
        assert_eq!(parsed.tiles[0].header, tile0);
        assert_eq!(parsed.tiles[0].ports, tile0_ports);
        assert_eq!(parsed.tiles[1].header, tile1);
        assert_eq!(parsed.tiles[1].ports, tile1_ports);
    }

    #[test]
    fn magic_validation_fails() {
        let bad_header = FacePortStreamHeader {
            magic: *b"NOPE",
            version: PROTOCOL_VERSION,
            flags: 0,
            k_sq: 40,
            tile_side: 2000,
            num_tiles: 0,
            reserved: 0,
        };

        let err = read_stream_header(&mut Cursor::new(bytes_of(&bad_header).to_vec())).unwrap_err();
        assert!(err.to_string().contains("magic mismatch"));
    }

    #[test]
    fn bridge_conversion() {
        let header = TileResultHeader {
            tile_id: 7,
            a_lo: 50,
            b_lo: 75,
            side: 200,
            num_components: 3,
            num_face_inner: 1,
            num_face_outer: 1,
            num_face_left: 1,
            num_face_right: 2,
            num_primes: 12,
            origin_component: 2,
        };
        let ports = TilePorts {
            inner: vec![FacePortRecord {
                a: 51,
                b: 76,
                component_id: 0,
            }],
            outer: vec![FacePortRecord {
                a: 52,
                b: 77,
                component_id: 1,
            }],
            left: vec![FacePortRecord {
                a: 53,
                b: 78,
                component_id: 1,
            }],
            right: vec![
                FacePortRecord {
                    a: 54,
                    b: 79,
                    component_id: 1,
                },
                FacePortRecord {
                    a: 55,
                    b: 80,
                    component_id: 2,
                },
            ],
        };

        let op = tile_operator_from_raw(header, ports, 40);
        assert_eq!(op.a_min, 50);
        assert_eq!(op.a_max, 250);
        assert_eq!(op.b_min, 75);
        assert_eq!(op.b_max, 275);
        assert_eq!(op.num_components, 3);
        assert_eq!(
            op.component_faces,
            vec![
                FACE_INNER_BIT,
                FACE_OUTER_BIT | FACE_LEFT_BIT | FACE_RIGHT_BIT,
                FACE_RIGHT_BIT,
            ]
        );
        assert_eq!(op.component_sizes, Vec::<u32>::new());
        assert_eq!(op.origin_component, Some(2));
        assert_eq!(op.num_primes, 12);
        assert_eq!(op.face_inner.len(), 1);
        assert_eq!(op.face_outer.len(), 1);
        assert_eq!(op.face_left.len(), 1);
        assert_eq!(op.face_right.len(), 2);
    }

    #[test]
    fn read_single_tile_result() {
        let header = TileResultHeader {
            tile_id: 3,
            a_lo: 1,
            b_lo: 2,
            side: 4,
            num_components: 1,
            num_face_inner: 1,
            num_face_outer: 0,
            num_face_left: 0,
            num_face_right: 0,
            num_primes: 5,
            origin_component: -1,
        };
        let ports = TilePorts {
            inner: vec![FacePortRecord {
                a: 1,
                b: 2,
                component_id: 0,
            }],
            outer: vec![],
            left: vec![],
            right: vec![],
        };

        let mut buf = Vec::new();
        append_tile(&mut buf, header, &ports);

        let parsed = read_tile_result(&mut Cursor::new(buf)).unwrap();
        assert_eq!(parsed.header, header);
        assert_eq!(parsed.ports, ports);
    }

    fn append_tile(buf: &mut Vec<u8>, header: TileResultHeader, ports: &TilePorts) {
        buf.extend_from_slice(bytes_of(&header));
        buf.extend_from_slice(cast_slice(&ports.inner));
        buf.extend_from_slice(cast_slice(&ports.outer));
        buf.extend_from_slice(cast_slice(&ports.left));
        buf.extend_from_slice(cast_slice(&ports.right));
    }
}
