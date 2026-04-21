// cross_compare: Compare TileOps from C++ and CUDA dump files.
//
// Handles group label permutation (CUDA union-find is nondeterministic):
// Two TileOps are "equivalent" if there exists a bijective group relabeling
// that makes them byte-identical.
//
// Reports exact matches, group-permutation matches, and real mismatches.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

constexpr int TILEOP_SIZE = 128;
constexpr int TILEOP_HEADER_BYTES = 3;
constexpr int NUM_FACES = 4;
constexpr int FACE_I = 0;
constexpr int FACE_O = 1;
constexpr int FACE_L = 2;
constexpr int FACE_R = 3;
constexpr uint8_t OVERFLOW_SENTINEL = 0xFF;
constexpr uint8_t EMPTY_OFFSET = 3;

struct TileRecord {
    int64_t a_lo;
    int64_t b_lo;
    uint32_t prime_count;
    uint8_t tileop[TILEOP_SIZE];
};

struct TileOpParsed {
    bool is_valid;
    bool is_empty;
    bool is_overflow;
    uint8_t off_I;
    uint8_t off_L;
    uint8_t off_R;
    uint8_t o_cnt;
    uint8_t i_cnt;
    uint8_t l_cnt;
    uint8_t r_cnt;
    uint8_t h_start;
};

struct PortInfo {
    int face;
    uint8_t group;
    uint16_t h1;
};

TileOpParsed parse_tileop(const uint8_t* bytes) {
    TileOpParsed p{};

    if (bytes[0] == OVERFLOW_SENTINEL) {
        p.is_valid = true;
        p.is_overflow = true;
        return p;
    }

    p.off_I = bytes[0];
    p.off_L = bytes[1];
    p.off_R = bytes[2];

    if (!(p.off_I >= TILEOP_HEADER_BYTES &&
          p.off_I <= p.off_L &&
          p.off_L <= p.off_R &&
          p.off_R <= TILEOP_SIZE)) {
        return p;
    }

    if (p.off_I == EMPTY_OFFSET &&
        p.off_L == EMPTY_OFFSET &&
        p.off_R == EMPTY_OFFSET &&
        bytes[3] == 0) {
        p.is_valid = true;
        p.is_empty = true;
        return p;
    }

    p.o_cnt = static_cast<uint8_t>(p.off_I - TILEOP_HEADER_BYTES);
    p.i_cnt = static_cast<uint8_t>(p.off_L - p.off_I);
    p.l_cnt = static_cast<uint8_t>(p.off_R - p.off_L);

    int residual = TILEOP_SIZE - static_cast<int>(p.off_R) - static_cast<int>(p.l_cnt);
    if (residual < 0) return p;

    p.r_cnt = static_cast<uint8_t>(residual / 2);
    p.h_start = static_cast<uint8_t>(p.off_R + p.r_cnt);
    p.is_valid = true;

    return p;
}

// Extract ports from a parsed TileOp
std::vector<PortInfo> extract_ports(const uint8_t* bytes, const TileOpParsed& p) {
    std::vector<PortInfo> ports;
    if (!p.is_valid || p.is_empty || p.is_overflow) return ports;

    // O face ports: bytes[3..off_I), group = byte, h1 not stored (only group)
    for (int j = TILEOP_HEADER_BYTES; j < p.off_I; ++j) {
        PortInfo pi;
        pi.face = FACE_O;
        pi.group = bytes[j];
        pi.h1 = 0; // O/I faces don't have h1 in the encoding
        ports.push_back(pi);
    }

    // I face ports: bytes[off_I..off_L)
    for (int j = p.off_I; j < p.off_L; ++j) {
        PortInfo pi;
        pi.face = FACE_I;
        pi.group = bytes[j];
        pi.h1 = 0;
        ports.push_back(pi);
    }

    // L face ports: group bytes at [off_L..off_R), h1 bytes at [h_start..h_start+l_cnt)
    for (int j = 0; j < p.l_cnt; ++j) {
        PortInfo pi;
        pi.face = FACE_L;
        uint8_t gb = bytes[p.off_L + j];
        pi.group = static_cast<uint8_t>(gb & 0x7F);
        uint8_t h1_byte = bytes[p.h_start + j];
        pi.h1 = static_cast<uint16_t>((static_cast<uint16_t>(gb >> 7) << 8) | h1_byte);
        ports.push_back(pi);
    }

    // R face ports: group bytes at [off_R..off_R+r_cnt), h1 bytes at [h_start+l_cnt..h_start+l_cnt+r_cnt)
    for (int j = 0; j < p.r_cnt; ++j) {
        uint8_t gb = bytes[p.off_R + j];
        if (gb == 0 && bytes[p.h_start + p.l_cnt + j] == 0) {
            // Zero padding slot (unused R port derived from budget formula)
            continue;
        }
        PortInfo pi;
        pi.face = FACE_R;
        pi.group = static_cast<uint8_t>(gb & 0x7F);
        uint8_t h1_byte = bytes[p.h_start + p.l_cnt + j];
        pi.h1 = static_cast<uint16_t>((static_cast<uint16_t>(gb >> 7) << 8) | h1_byte);
        ports.push_back(pi);
    }

    return ports;
}

// Check if two TileOps are equivalent under group relabeling.
// Returns 0 = exact match, 1 = match under group permutation, 2 = structural mismatch
int compare_tileops(const uint8_t* cpp_bytes, const uint8_t* cuda_bytes) {
    // Exact byte match?
    if (std::memcmp(cpp_bytes, cuda_bytes, TILEOP_SIZE) == 0) {
        return 0;
    }

    TileOpParsed cpp_p = parse_tileop(cpp_bytes);
    TileOpParsed cuda_p = parse_tileop(cuda_bytes);

    // Both overflow?
    if (cpp_p.is_overflow && cuda_p.is_overflow) return 0;

    // Both empty?
    if (cpp_p.is_empty && cuda_p.is_empty) {
        // Should be exact match if both empty
        return 2;
    }

    // One valid, other not?
    if (!cpp_p.is_valid || !cuda_p.is_valid) return 2;

    // Different structure (overflow vs non-overflow)?
    if (cpp_p.is_overflow != cuda_p.is_overflow) return 2;
    if (cpp_p.is_empty != cuda_p.is_empty) return 2;

    // Headers must match (port counts per face must be equal)
    if (cpp_p.off_I != cuda_p.off_I ||
        cpp_p.off_L != cuda_p.off_L ||
        cpp_p.off_R != cuda_p.off_R) {
        return 2;
    }

    // Extract ports from both
    auto cpp_ports = extract_ports(cpp_bytes, cpp_p);
    auto cuda_ports = extract_ports(cuda_bytes, cuda_p);

    if (cpp_ports.size() != cuda_ports.size()) return 2;

    // Check that faces and h1 values match in order (they should, since
    // both pipelines sort face primes identically and use the same clustering).
    // Only group labels may differ.
    for (size_t i = 0; i < cpp_ports.size(); ++i) {
        if (cpp_ports[i].face != cuda_ports[i].face) return 2;
        if (cpp_ports[i].h1 != cuda_ports[i].h1) return 2;
    }

    // Build group relabeling: cuda_group -> cpp_group
    // Must be a consistent bijection
    std::map<uint8_t, uint8_t> cuda_to_cpp;
    std::map<uint8_t, uint8_t> cpp_to_cuda;

    for (size_t i = 0; i < cpp_ports.size(); ++i) {
        uint8_t cg = cpp_ports[i].group;
        uint8_t ug = cuda_ports[i].group;

        auto it = cuda_to_cpp.find(ug);
        if (it != cuda_to_cpp.end()) {
            if (it->second != cg) return 2; // Inconsistent mapping
        } else {
            cuda_to_cpp[ug] = cg;
        }

        auto it2 = cpp_to_cuda.find(cg);
        if (it2 != cpp_to_cuda.end()) {
            if (it2->second != ug) return 2; // Not bijective
        } else {
            cpp_to_cuda[cg] = ug;
        }
    }

    // All faces and h1 match, groups differ only by consistent relabeling
    return 1;
}

void hex_dump(const uint8_t* data, int len) {
    for (int i = 0; i < len; ++i) {
        if (i > 0 && i % 16 == 0) std::printf("\n    ");
        std::printf("%02x ", data[i]);
    }
    std::printf("\n");
}

void print_ports(const uint8_t* bytes, const TileOpParsed& p) {
    auto ports = extract_ports(bytes, p);
    const char* face_names[] = {"I", "O", "L", "R"};
    for (size_t i = 0; i < ports.size(); ++i) {
        std::printf("    port[%zu]: face=%s group=%u h1=%u\n",
                    i, face_names[ports[i].face], ports[i].group, ports[i].h1);
    }
}

std::vector<TileRecord> read_dump(const char* path) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        std::fprintf(stderr, "error: cannot open %s\n", path);
        std::exit(1);
    }

    uint32_t n = 0;
    if (std::fread(&n, sizeof(uint32_t), 1, fp) != 1) {
        std::fprintf(stderr, "error: cannot read header from %s\n", path);
        std::exit(1);
    }

    std::vector<TileRecord> records(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (std::fread(&records[i].a_lo, sizeof(int64_t), 1, fp) != 1 ||
            std::fread(&records[i].b_lo, sizeof(int64_t), 1, fp) != 1 ||
            std::fread(&records[i].prime_count, sizeof(uint32_t), 1, fp) != 1 ||
            std::fread(records[i].tileop, 1, TILEOP_SIZE, fp) != TILEOP_SIZE) {
            std::fprintf(stderr, "error: truncated record %u in %s\n", i, path);
            std::exit(1);
        }
    }

    std::fclose(fp);
    return records;
}

int main(int argc, char** argv) {
    const char* cpp_path = "cpp_tileops.bin";
    const char* cuda_path = "cuda_tileops.bin";
    bool verbose = false;
    int max_detail = 20; // Max mismatches to print in detail

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cpp") == 0 && i + 1 < argc) cpp_path = argv[++i];
        else if (std::strcmp(argv[i], "--cuda") == 0 && i + 1 < argc) cuda_path = argv[++i];
        else if (std::strcmp(argv[i], "-v") == 0) verbose = true;
        else if (std::strcmp(argv[i], "--max-detail") == 0 && i + 1 < argc) max_detail = std::atoi(argv[++i]);
    }

    auto cpp_records = read_dump(cpp_path);
    auto cuda_records = read_dump(cuda_path);

    if (cpp_records.size() != cuda_records.size()) {
        std::fprintf(stderr, "error: tile count mismatch: cpp=%zu cuda=%zu\n",
                     cpp_records.size(), cuda_records.size());
        return 1;
    }

    int n = static_cast<int>(cpp_records.size());
    int exact_matches = 0;
    int group_perm_matches = 0;
    int real_mismatches = 0;
    int prime_count_mismatches = 0;
    int coord_mismatches = 0;
    int both_empty = 0;
    int both_overflow = 0;
    int detail_printed = 0;

    for (int i = 0; i < n; ++i) {
        const TileRecord& cr = cpp_records[i];
        const TileRecord& ur = cuda_records[i];

        // Verify coordinates match
        if (cr.a_lo != ur.a_lo || cr.b_lo != ur.b_lo) {
            ++coord_mismatches;
            if (detail_printed < max_detail) {
                std::printf("COORD MISMATCH tile[%d]: cpp=(%lld,%lld) cuda=(%lld,%lld)\n",
                            i, (long long)cr.a_lo, (long long)cr.b_lo,
                            (long long)ur.a_lo, (long long)ur.b_lo);
                ++detail_printed;
            }
            continue;
        }

        // Check prime counts (expected to differ: cpp=tile-proper, cuda=full-domain)
        if (cr.prime_count != ur.prime_count) {
            ++prime_count_mismatches;
            if (verbose && detail_printed < max_detail) {
                std::printf("PRIME COUNT DIFF tile[%d] (%lld,%lld): cpp=%u cuda=%u (expected)\n",
                            i, (long long)cr.a_lo, (long long)cr.b_lo,
                            cr.prime_count, ur.prime_count);
            }
        }

        // Check for both empty/overflow
        TileOpParsed cpp_p = parse_tileop(cr.tileop);
        TileOpParsed cuda_p = parse_tileop(ur.tileop);
        if (cpp_p.is_empty && cuda_p.is_empty) ++both_empty;
        if (cpp_p.is_overflow && cuda_p.is_overflow) ++both_overflow;

        // Compare TileOps
        int result = compare_tileops(cr.tileop, ur.tileop);
        switch (result) {
            case 0:
                ++exact_matches;
                break;
            case 1:
                ++group_perm_matches;
                if (verbose && detail_printed < max_detail) {
                    std::printf("GROUP PERM tile[%d] (%lld,%lld): primes=%u\n",
                                i, (long long)cr.a_lo, (long long)cr.b_lo, cr.prime_count);
                    std::printf("  cpp ports:\n");
                    print_ports(cr.tileop, cpp_p);
                    std::printf("  cuda ports:\n");
                    print_ports(ur.tileop, cuda_p);
                    ++detail_printed;
                }
                break;
            case 2:
                ++real_mismatches;
                if (detail_printed < max_detail) {
                    std::printf("MISMATCH tile[%d] (%lld,%lld): cpp_primes=%u cuda_primes=%u\n",
                                i, (long long)cr.a_lo, (long long)cr.b_lo,
                                cr.prime_count, ur.prime_count);
                    std::printf("  cpp tileop header: %02x %02x %02x",
                                cr.tileop[0], cr.tileop[1], cr.tileop[2]);
                    if (cpp_p.is_valid) {
                        std::printf(" (o=%u i=%u l=%u r=%u)", cpp_p.o_cnt, cpp_p.i_cnt, cpp_p.l_cnt, cpp_p.r_cnt);
                    }
                    std::printf("\n");
                    std::printf("  cuda tileop header: %02x %02x %02x",
                                ur.tileop[0], ur.tileop[1], ur.tileop[2]);
                    if (cuda_p.is_valid) {
                        std::printf(" (o=%u i=%u l=%u r=%u)", cuda_p.o_cnt, cuda_p.i_cnt, cuda_p.l_cnt, cuda_p.r_cnt);
                    }
                    std::printf("\n");

                    // Print differing bytes
                    std::printf("  differing bytes:");
                    int diff_count = 0;
                    for (int j = 0; j < TILEOP_SIZE; ++j) {
                        if (cr.tileop[j] != ur.tileop[j]) {
                            if (diff_count < 20) {
                                std::printf(" [%d]=%02x/%02x", j, cr.tileop[j], ur.tileop[j]);
                            }
                            ++diff_count;
                        }
                    }
                    if (diff_count > 20) {
                        std::printf(" ...+%d more", diff_count - 20);
                    }
                    std::printf(" (%d total)\n", diff_count);

                    // Print ports
                    if (cpp_p.is_valid && !cpp_p.is_empty && !cpp_p.is_overflow) {
                        std::printf("  cpp ports:\n");
                        print_ports(cr.tileop, cpp_p);
                    }
                    if (cuda_p.is_valid && !cuda_p.is_empty && !cuda_p.is_overflow) {
                        std::printf("  cuda ports:\n");
                        print_ports(ur.tileop, cuda_p);
                    }

                    // Full hex dump for first few
                    if (detail_printed < 5) {
                        std::printf("  cpp hex:\n    ");
                        hex_dump(cr.tileop, TILEOP_SIZE);
                        std::printf("  cuda hex:\n    ");
                        hex_dump(ur.tileop, TILEOP_SIZE);
                    }

                    ++detail_printed;
                }
                break;
        }
    }

    // Summary
    std::printf("\n========================================\n");
    std::printf("Cross-validation summary: %d tiles\n", n);
    std::printf("========================================\n");
    std::printf("  Exact byte matches:     %d\n", exact_matches);
    std::printf("  Group-perm matches:     %d\n", group_perm_matches);
    std::printf("  Total equivalent:       %d (%.1f%%)\n",
                exact_matches + group_perm_matches,
                100.0 * (exact_matches + group_perm_matches) / n);
    std::printf("  Real mismatches:        %d\n", real_mismatches);
    std::printf("  Prime count diffs:      %d (cpp=tile-proper, cuda=full-domain)\n",
                prime_count_mismatches);
    std::printf("  Coord mismatches:       %d\n", coord_mismatches);
    std::printf("  Both empty:             %d\n", both_empty);
    std::printf("  Both overflow:          %d\n", both_overflow);
    std::printf("========================================\n");

    // Prime count differences are expected: C++ counts tile-proper primes (257x257)
    // while CUDA reports total sieve-domain primes (271x271 including collar).
    // This is a counting scope difference, not a correctness bug.
    // The definitive correctness check is the TileOp comparison.
    if (real_mismatches == 0 && coord_mismatches == 0) {
        std::printf("PASS: All %d TileOps match (exact or under group relabeling)\n", n);
        if (prime_count_mismatches > 0) {
            std::printf("  NOTE: Prime count diffs are expected (tile-proper vs full-domain scope)\n");
        }
        return 0;
    } else {
        std::printf("FAIL: %d real TileOp mismatches, %d coord mismatches\n",
                    real_mismatches, coord_mismatches);
        return 1;
    }
}
