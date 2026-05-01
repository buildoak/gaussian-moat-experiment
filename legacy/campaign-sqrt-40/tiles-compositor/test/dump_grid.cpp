#include "grid.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: dump_grid <R>\n");
        return 1;
    }

    const int64_t R = std::atoll(argv[1]);
    if (R <= 0) {
        std::fprintf(stderr, "R must be positive\n");
        return 1;
    }

    Grid grid{};
    compute_grid(R, grid);

    // Print JSON
    std::printf("{\n");
    std::printf("  \"R\": %lld,\n", static_cast<long long>(R));
    std::printf("  \"S\": %d,\n", grid.S);
    std::printf("  \"num_towers\": %d,\n", grid.num_towers);
    std::printf("  \"total_tiles\": %llu,\n", static_cast<unsigned long long>(grid.total_tiles));

    // tiles_per_tower array
    std::printf("  \"tiles_per_tower\": [");
    for (int j = 0; j < grid.num_towers; ++j) {
        if (j > 0) std::printf(",");
        std::printf("%u", grid.tiles_per_tower[static_cast<std::size_t>(j)]);
    }
    std::printf("],\n");

    // base_y array
    std::printf("  \"base_y\": [");
    for (int j = 0; j < grid.num_towers; ++j) {
        if (j > 0) std::printf(",");
        std::printf("%lld", static_cast<long long>(grid.base_y[static_cast<std::size_t>(j)]));
    }
    std::printf("],\n");

    // tower_offset array
    std::printf("  \"tower_offset\": [");
    for (int j = 0; j < grid.num_towers; ++j) {
        if (j > 0) std::printf(",");
        std::printf("%llu", static_cast<unsigned long long>(grid.tower_offset[static_cast<std::size_t>(j)]));
    }
    std::printf("],\n");

    // delta array
    std::printf("  \"delta\": [");
    for (int j = 0; j < static_cast<int>(grid.delta.size()); ++j) {
        if (j > 0) std::printf(",");
        std::printf("%lld", static_cast<long long>(grid.delta[static_cast<std::size_t>(j)]));
    }
    std::printf("],\n");

    // Per-tower deviation from continuous arc for verification
    // deviation[j] = base_y[j] - y_cont[j]; |deviation| <= 0.5 always holds
    std::printf("  \"arc_deviation\": [");
    {
        const __int128 R_sq = static_cast<__int128>(R) * R;
        const int64_t S = static_cast<int64_t>(TILE_SIDE);
        bool first = true;
        for (int64_t j = 0; j < grid.num_towers; ++j) {
            const int64_t x_j = j * S;
            const __int128 x_sq = static_cast<__int128>(x_j) * x_j;
            const double y_cont = std::sqrt(static_cast<double>(R_sq - x_sq));
            const double deviation = static_cast<double>(grid.base_y[static_cast<std::size_t>(j)]) - y_cont;
            if (!first) std::printf(",");
            std::printf("%.6f", deviation);
            first = false;
        }
    }
    std::printf("]\n");

    std::printf("}\n");

    return 0;
}
