use std::fs::File;
use std::io::{self, BufReader, Read};

use memmap2::Mmap;

use crate::sieve::GaussianPrime;

const HEADER_SIZE: usize = 64;
const RECORD_SIZE: usize = 16;
const MAGIC: u32 = 0x47505246;
/// Number of records to bulk-read at once in the streaming iterator.
/// 4096 records = 64KB per chunk read, amortizes syscall overhead.
const CHUNK_RECORDS: usize = 4096;

/// On-disk record layout matching the C++ writer exactly.
/// {i32 a, i32 b, u64 norm} = 16 bytes, no padding.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct PrimeRecord {
    pub a: i32,
    pub b: i32,
    pub norm: u64,
}

pub struct PrimeFileReader {
    mmap: Mmap,
    count: u64,
    pub norm_min: u64,
    pub norm_max: u64,
}

impl PrimeFileReader {
    pub fn open(path: &str) -> io::Result<Self> {
        let file = File::open(path)?;
        let mmap = unsafe { Mmap::map(&file)? };

        if mmap.len() < HEADER_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "File too small for GPRF header",
            ));
        }

        // Read header fields (little-endian)
        let magic = u32::from_le_bytes(mmap[0..4].try_into().unwrap());
        if magic != MAGIC {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Bad GPRF magic: 0x{:08X}, expected 0x{:08X}", magic, MAGIC),
            ));
        }

        let version = u16::from_le_bytes(mmap[4..6].try_into().unwrap());
        if version != 1 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Unsupported GPRF version: {}", version),
            ));
        }

        let count = u64::from_le_bytes(mmap[8..16].try_into().unwrap());
        let norm_min = u64::from_le_bytes(mmap[16..24].try_into().unwrap());
        let norm_max = u64::from_le_bytes(mmap[24..32].try_into().unwrap());

        let expected_size = HEADER_SIZE + (count as usize) * RECORD_SIZE;
        if mmap.len() != expected_size {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "File size mismatch: {} bytes, expected {} (header + {} records)",
                    mmap.len(),
                    expected_size,
                    count
                ),
            ));
        }

        Ok(Self {
            mmap,
            count,
            norm_min,
            norm_max,
        })
    }

    pub fn len(&self) -> u64 {
        self.count
    }

    pub fn get(&self, i: u64) -> PrimeRecord {
        assert!(
            i < self.count,
            "Index {} out of bounds (count={})",
            i,
            self.count
        );
        let offset = HEADER_SIZE + (i as usize) * RECORD_SIZE;
        let a = i32::from_le_bytes(self.mmap[offset..offset + 4].try_into().unwrap());
        let b = i32::from_le_bytes(self.mmap[offset + 4..offset + 8].try_into().unwrap());
        let norm = u64::from_le_bytes(self.mmap[offset + 8..offset + 16].try_into().unwrap());
        PrimeRecord { a, b, norm }
    }

    /// Binary search: return index of first record with norm >= target_norm.
    /// Returns self.count if no such record.
    pub fn lower_bound(&self, target_norm: u64) -> u64 {
        let mut lo: u64 = 0;
        let mut hi: u64 = self.count;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.get(mid).norm < target_norm {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        lo
    }

    pub fn iter(&self) -> PrimeRangeIter<'_> {
        PrimeRangeIter {
            reader: self,
            current: 0,
            end: self.count,
        }
    }

    pub fn iter_norm_range(&self, lo: u64, hi: u64) -> PrimeRangeIter<'_> {
        let start = self.lower_bound(lo);
        let end = self.lower_bound(hi.saturating_add(1));
        PrimeRangeIter {
            reader: self,
            current: start,
            end,
        }
    }
}

pub struct PrimeRangeIter<'a> {
    reader: &'a PrimeFileReader,
    current: u64,
    end: u64,
}

impl<'a> Iterator for PrimeRangeIter<'a> {
    type Item = GaussianPrime;

    fn next(&mut self) -> Option<GaussianPrime> {
        if self.current >= self.end {
            return None;
        }
        let rec = self.reader.get(self.current);
        self.current += 1;
        Some(GaussianPrime {
            a: rec.a,
            b: rec.b,
            norm: rec.norm,
        })
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let remaining = (self.end - self.current) as usize;
        (remaining, Some(remaining))
    }
}

impl<'a> ExactSizeIterator for PrimeRangeIter<'a> {}

// ---------------------------------------------------------------------------
// Streaming GPRF reader — works with File, Stdin, or any Read source.
// Reads 16-byte records one at a time through a BufReader.
// For stdin (pipe mode): skips header validation since the CUDA sieve
// writes raw records without a GPRF header in --stdout mode.
// For files: reads and validates the 64-byte GPRF header first.
// ---------------------------------------------------------------------------

pub struct GprfStreamReader<R: Read> {
    reader: BufReader<R>,
    records_read: u64,
    pub norm_min: u64,
    pub norm_max: u64,
    pub header_count: u64,
    /// Bulk-read buffer: CHUNK_RECORDS * RECORD_SIZE bytes
    chunk_buf: Vec<u8>,
    chunk_pos: usize,  // next record index within chunk_buf to return
    chunk_len: usize,  // number of valid records in chunk_buf
}

impl GprfStreamReader<File> {
    /// Open a GPRF file for streaming. Reads the 64-byte header, then
    /// yields records one at a time via the Iterator impl.
    pub fn open_file(path: &str) -> io::Result<Self> {
        let file = File::open(path)?;
        let mut reader = BufReader::with_capacity(4 * 1024 * 1024, file);

        // Read the 64-byte header
        let mut hdr = [0u8; HEADER_SIZE];
        reader.read_exact(&mut hdr)?;

        let magic = u32::from_le_bytes(hdr[0..4].try_into().unwrap());
        if magic != MAGIC {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Bad GPRF magic: 0x{:08X}, expected 0x{:08X}", magic, MAGIC),
            ));
        }

        let version = u16::from_le_bytes(hdr[4..6].try_into().unwrap());
        if version != 1 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Unsupported GPRF version: {}", version),
            ));
        }

        let header_count = u64::from_le_bytes(hdr[8..16].try_into().unwrap());
        let norm_min = u64::from_le_bytes(hdr[16..24].try_into().unwrap());
        let norm_max = u64::from_le_bytes(hdr[24..32].try_into().unwrap());

        Ok(Self {
            reader,
            records_read: 0,
            norm_min,
            norm_max,
            header_count,
            chunk_buf: vec![0u8; CHUNK_RECORDS * RECORD_SIZE],
            chunk_pos: 0,
            chunk_len: 0,
        })
    }
}

impl<R: Read> GprfStreamReader<R> {
    /// Wrap any Read source for headerless streaming (raw 16-byte records).
    /// Used for stdin pipe mode where the CUDA sieve writes records directly.
    pub fn from_raw(source: R) -> Self {
        Self {
            reader: BufReader::with_capacity(4 * 1024 * 1024, source),
            records_read: 0,
            norm_min: 0,
            norm_max: 0,
            header_count: 0,
            chunk_buf: vec![0u8; CHUNK_RECORDS * RECORD_SIZE],
            chunk_pos: 0,
            chunk_len: 0,
        }
    }

    pub fn records_read(&self) -> u64 {
        self.records_read
    }
}

impl<R: Read> Iterator for GprfStreamReader<R> {
    type Item = GaussianPrime;

    fn next(&mut self) -> Option<GaussianPrime> {
        // Fast path: return from pre-read chunk buffer
        if self.chunk_pos < self.chunk_len {
            let offset = self.chunk_pos * RECORD_SIZE;
            let a = i32::from_le_bytes(
                self.chunk_buf[offset..offset + 4].try_into().unwrap(),
            );
            let b = i32::from_le_bytes(
                self.chunk_buf[offset + 4..offset + 8].try_into().unwrap(),
            );
            let norm = u64::from_le_bytes(
                self.chunk_buf[offset + 8..offset + 16].try_into().unwrap(),
            );
            self.chunk_pos += 1;
            self.records_read += 1;
            return Some(GaussianPrime { a, b, norm });
        }

        // Refill chunk buffer: read up to CHUNK_RECORDS records at once
        let buf_len = CHUNK_RECORDS * RECORD_SIZE;
        let mut total_read = 0usize;
        while total_read < buf_len {
            match self.reader.read(&mut self.chunk_buf[total_read..buf_len]) {
                Ok(0) => break,
                Ok(n) => total_read += n,
                Err(ref e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => break,
            }
        }
        // Align to record boundary (drop partial trailing record)
        let full_records = total_read / RECORD_SIZE;
        if full_records == 0 {
            return None;
        }
        self.chunk_len = full_records;
        self.chunk_pos = 0;

        // Return first record from freshly filled buffer
        let a = i32::from_le_bytes(self.chunk_buf[0..4].try_into().unwrap());
        let b = i32::from_le_bytes(self.chunk_buf[4..8].try_into().unwrap());
        let norm = u64::from_le_bytes(self.chunk_buf[8..16].try_into().unwrap());
        self.chunk_pos = 1;
        self.records_read += 1;
        Some(GaussianPrime { a, b, norm })
    }
}
