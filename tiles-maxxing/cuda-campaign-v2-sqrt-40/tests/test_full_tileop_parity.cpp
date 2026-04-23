#include <array>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>

#include "campaign/constants.h"
#include "support/k5_parity_support.h"

namespace {

std::string run_cli_verbose_self_test(int* exit_code) {
  FILE* pipe = popen("./cuda_vs_cpu_diff --self-test-verbose-mismatch "
                     "--verbose 2>&1",
                     "r");
  if (pipe == nullptr) {
    throw std::runtime_error("failed to run cuda_vs_cpu_diff verbose self-test");
  }

  std::string output;
  std::array<char, 256> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

  const int status = pclose(pipe);
  if (status == -1) {
    throw std::runtime_error("failed to collect cuda_vs_cpu_diff status");
  }
  if (WIFEXITED(status)) {
    *exit_code = WEXITSTATUS(status);
  } else {
    *exit_code = 128;
  }
  return output;
}

bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
  try {
    static_assert(sizeof(campaign::TileOp) == 256,
                  "TileOp parity test assumes the canonical 256 B layout");

    const campaign::TileOp empty = k5_parity::empty_cpu_fixture();
    if (empty.tile_flags != campaign::EMPTY_BIT ||
        !k5_parity::face_group_padding_zero(empty)) {
      std::cerr << "empty TileOp fixture did not encode EMPTY_BIT cleanly\n";
      return 1;
    }

    const campaign::TileOp overflow = k5_parity::overflow_cpu_fixture();
    if (overflow.tile_flags != campaign::OVERFLOW_BIT ||
        !k5_parity::face_group_padding_zero(overflow)) {
      std::cerr << "overflow TileOp fixture did not encode OVERFLOW_BIT cleanly\n";
      return 1;
    }

    campaign::TileOp mutated = empty;
    reinterpret_cast<std::uint8_t*>(&mutated)[sizeof(campaign::TileOp) - 1] =
        0x7fU;
    if (k5_parity::same_tileop_bytes(empty, mutated)) {
      std::cerr << "full TileOp byte comparison missed final byte mutation\n";
      return 1;
    }
    const std::string summary =
        k5_parity::tileop_mismatch_summary(empty, mutated);
    if (!contains(summary, "offset 255")) {
      std::cerr << "TileOp mismatch summary did not report final byte: "
                << summary << "\n";
      return 1;
    }

    int cli_exit_code = 0;
    const std::string cli_output = run_cli_verbose_self_test(&cli_exit_code);
    if (cli_exit_code == 0 || !contains(cli_output, "tile 17") ||
        !contains(cli_output, "(i=3, j=5)") ||
        !contains(cli_output, "TileOp byte offset 255") ||
        !contains(cli_output, "cpu=0") || !contains(cli_output, "gpu=90") ||
        !contains(cli_output, "cpu=0x00") ||
        !contains(cli_output, "gpu=0x5a")) {
      std::cerr << "cuda_vs_cpu_diff --verbose self-test output was not "
                << "diagnostic enough:\n"
                << cli_output;
      return 1;
    }

    const auto constants = k5_parity::make_constants(10000, 17000);
    const auto coords = k5_parity::first_active_tiles(10000, 17000, 1024);
    if (coords.size() != 1024) {
      std::cerr << "expected 1024 active tiles, got " << coords.size() << "\n";
      return 1;
    }

    bool saw_pending_gpu = false;
    for (const auto& coord : coords) {
      const campaign::TileOp cpu =
          k5_parity::row_major_cpu_tileop(coord, constants);
      const k5_parity::GpuTileOpResult gpu =
          k5_parity::gpu_tileop_or_pending(coord, constants);
      if (!gpu.available) {
        saw_pending_gpu = true;
        continue;
      }
      if (!k5_parity::same_tileop_bytes(cpu, gpu.tileop)) {
        std::cerr << k5_parity::tileop_mismatch_summary(cpu, gpu.tileop)
                  << "\n";
        return 1;
      }
    }

    if (saw_pending_gpu) {
      std::cout << "pending GPU: full 256 B parity compared CPU oracle only\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_full_tileop_parity: " << e.what() << "\n";
    return 1;
  }
}
