#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma pack(push, 1)
struct GPRFHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;
    uint64_t prime_count;
    uint64_t norm_min;
    uint64_t norm_max;
    uint64_t k_squared;
    uint64_t sieve_bound;
    uint8_t  reserved1[16];
};
#pragma pack(pop)

static_assert(sizeof(GPRFHeader) == 64, "GPRFHeader must be 64 bytes");

// Each record is: int32_t a (4) + int32_t b (4) + uint64_t norm (8) = 16 bytes
static constexpr size_t GPRF_RECORD_SIZE = sizeof(int32_t) + sizeof(int32_t) + sizeof(uint64_t);

// Default write buffer: 64KB (holds 4096 records exactly)
static constexpr size_t GPRF_BUFFER_SIZE = 65536u;

class PrimeFileWriter {
    FILE* fp_;
    uint64_t written_;
    uint64_t norm_min_;
    uint64_t norm_max_;
    uint64_t sieve_bound_;
    uint64_t k_squared_;

    // Write buffer — accumulate records, flush in bulk
    uint8_t buf_[GPRF_BUFFER_SIZE];
    size_t buf_pos_;

    void flush_buffer() {
        if (buf_pos_ > 0 && fp_) {
            fwrite(buf_, 1, buf_pos_, fp_);
            buf_pos_ = 0;
        }
    }

public:
    PrimeFileWriter(const char* path, uint64_t sieve_bound, uint64_t k_squared = 0)
        : fp_(nullptr), written_(0), norm_min_(UINT64_MAX), norm_max_(0),
          sieve_bound_(sieve_bound), k_squared_(k_squared), buf_pos_(0)
    {
        fp_ = fopen(path, "wb");
        if (!fp_) {
            fprintf(stderr, "FATAL: Cannot open GPRF output: %s\n", path);
            exit(1);
        }
        GPRFHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = 0x47505246;
        hdr.version = 1;
        hdr.sieve_bound = sieve_bound_;
        fwrite(&hdr, sizeof(hdr), 1, fp_);
    }

    void write(int32_t a, int32_t b, uint64_t norm) {
        // Flush if adding this record would exceed buffer
        if (buf_pos_ + GPRF_RECORD_SIZE > GPRF_BUFFER_SIZE) {
            flush_buffer();
        }

        // Append record to buffer (memcpy preserves byte layout)
        memcpy(buf_ + buf_pos_, &a, sizeof(a));
        buf_pos_ += sizeof(a);
        memcpy(buf_ + buf_pos_, &b, sizeof(b));
        buf_pos_ += sizeof(b);
        memcpy(buf_ + buf_pos_, &norm, sizeof(norm));
        buf_pos_ += sizeof(norm);

        written_++;
        if (norm < norm_min_) norm_min_ = norm;
        if (norm > norm_max_) norm_max_ = norm;
    }

    void finalize() {
        if (!fp_) return;

        // Flush remaining buffered records
        flush_buffer();

        // Rewrite header with final counts
        fseek(fp_, 0, SEEK_SET);
        GPRFHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = 0x47505246;
        hdr.version = 1;
        hdr.prime_count = written_;
        hdr.norm_min = (written_ > 0) ? norm_min_ : 0;
        hdr.norm_max = (written_ > 0) ? norm_max_ : 0;
        hdr.k_squared = k_squared_;
        hdr.sieve_bound = sieve_bound_;
        fwrite(&hdr, sizeof(hdr), 1, fp_);
        fclose(fp_);
        fp_ = nullptr;
    }

    ~PrimeFileWriter() {
        finalize();
    }

    uint64_t count() const { return written_; }
};
