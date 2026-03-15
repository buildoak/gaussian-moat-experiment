use std::fs::File;
use std::io;

use memmap2::Mmap;

use crate::sieve::GaussianPrime;

const HEADER_SIZE: usize = 64;
const RECORD_SIZE: usize = 16;
const MAGIC: u32 = 0x47505246;

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
