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

class PrimeFileWriter {
    FILE* fp_;
    uint64_t written_;
    uint64_t norm_min_;
    uint64_t norm_max_;
    uint64_t sieve_bound_;

public:
    PrimeFileWriter(const char* path, uint64_t sieve_bound)
        : fp_(nullptr), written_(0), norm_min_(UINT64_MAX), norm_max_(0), sieve_bound_(sieve_bound)
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
        fwrite(&a, sizeof(a), 1, fp_);
        fwrite(&b, sizeof(b), 1, fp_);
        fwrite(&norm, sizeof(norm), 1, fp_);
        written_++;
        if (norm < norm_min_) norm_min_ = norm;
        if (norm > norm_max_) norm_max_ = norm;
    }

    void finalize() {
        if (!fp_) return;
        fseek(fp_, 0, SEEK_SET);
        GPRFHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = 0x47505246;
        hdr.version = 1;
        hdr.prime_count = written_;
        hdr.norm_min = (written_ > 0) ? norm_min_ : 0;
        hdr.norm_max = (written_ > 0) ? norm_max_ : 0;
        hdr.k_squared = 0;
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
