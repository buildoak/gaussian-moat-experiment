#include "cuda_campaign/kernels.cuh"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "cuda_campaign/compact_buffers.cuh"
#include "cuda_campaign/constants.cuh"
#include "cuda_campaign/uf_buffers.cuh"

namespace cuda_campaign {
namespace {

void check_cuda(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(status));
  }
}

template <typename T>
class DeviceBuffer {
 public:
  DeviceBuffer() = default;

  explicit DeviceBuffer(std::size_t count) {
    allocate(count);
  }

  DeviceBuffer(const DeviceBuffer&) = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;

  DeviceBuffer(DeviceBuffer&& other) noexcept
      : ptr_(other.ptr_), count_(other.count_) {
    other.ptr_ = nullptr;
    other.count_ = 0;
  }

  DeviceBuffer& operator=(DeviceBuffer&& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = other.ptr_;
      count_ = other.count_;
      other.ptr_ = nullptr;
      other.count_ = 0;
    }
    return *this;
  }

  ~DeviceBuffer() {
    reset();
  }

  void allocate(std::size_t count) {
    reset();
    count_ = count;
    if (count_ == 0) return;
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&ptr_), count_ * sizeof(T)),
               "cudaMalloc");
  }

  void reset() noexcept {
    if (ptr_ != nullptr) {
      cudaFree(ptr_);
      ptr_ = nullptr;
    }
    count_ = 0;
  }

  T* get() const noexcept {
    return ptr_;
  }

  std::size_t count() const noexcept {
    return count_;
  }

 private:
  T* ptr_ = nullptr;
  std::size_t count_ = 0;
};

void check_last_launch(const char* what) {
  check_cuda(cudaGetLastError(), what);
}

}  // namespace

void launch_k1_to_k4(const K1K4Buffers& buffers,
                     int num_tiles,
                     cudaStream_t stream) {
  launch_kernel_sieve(buffers.d_coords, buffers.d_cand_list,
                      buffers.d_total_cands, num_tiles, stream);
  check_last_launch("launch_kernel_sieve");

  launch_kernel_mr(buffers.d_coords, buffers.d_cand_list, buffers.d_total_cands,
                   buffers.d_bitmap, buffers.d_fj64_table, num_tiles, stream);
  check_last_launch("launch_kernel_mr");

  launch_kernel_compact(buffers.d_bitmap, buffers.compact, num_tiles, stream);
  check_last_launch("launch_kernel_compact");

  launch_kernel_uf_v2(buffers.uf, num_tiles, stream);
  check_last_launch("launch_kernel_uf_v2");
}

K1K4DebugDownload run_k1_to_k4_debug(
    const std::vector<campaign::TileCoord>& coords,
    const campaign::CampaignConstants& constants,
    cudaStream_t stream) {
  K1K4DebugDownload out;
  if (coords.empty()) {
    return out;
  }

  const int num_tiles = static_cast<int>(coords.size());
  const std::size_t tile_count = coords.size();

  upload_cuda_constants(constants);

  std::uint16_t* d_fj64_table = nullptr;
  upload_fj64_table(&d_fj64_table);

  try {
    DeviceBuffer<campaign::TileCoord> d_coords(tile_count);
    DeviceBuffer<std::uint32_t> d_cand_list(tile_count * MAX_CANDIDATES_GPU);
    DeviceBuffer<std::uint32_t> d_total_cands(tile_count);
    DeviceBuffer<std::uint32_t> d_bitmap(tile_count * BITMAP_WORDS);
    DeviceBuffer<std::uint16_t> d_row_prefix(tile_count * ROW_PREFIX_ENTRIES);
    DeviceBuffer<std::uint32_t> d_prime_pos(tile_count * MAX_PRIMES_GPU);
    DeviceBuffer<std::uint32_t> d_prime_count(tile_count);
    DeviceBuffer<std::uint16_t> d_parent(tile_count * MAX_PRIMES_GPU);
    DeviceBuffer<std::uint8_t> d_prime_geo_bits(tile_count * MAX_PRIMES_GPU);
    DeviceBuffer<std::uint16_t> d_wire_label_by_raw_root(
        tile_count * MAX_PRIMES_GPU);
    DeviceBuffer<std::uint16_t> d_max_label(tile_count);
    DeviceBuffer<std::uint8_t> d_overflow(tile_count);
    DeviceBuffer<std::uint8_t> d_group_flags(tile_count * 256U);

    check_cuda(cudaMemcpyAsync(d_coords.get(), coords.data(),
                               tile_count * sizeof(campaign::TileCoord),
                               cudaMemcpyHostToDevice, stream),
               "cudaMemcpyAsync(d_coords)");

    K1K4Buffers buffers{};
    buffers.d_coords = d_coords.get();
    buffers.d_cand_list = d_cand_list.get();
    buffers.d_total_cands = d_total_cands.get();
    buffers.d_bitmap = d_bitmap.get();
    buffers.d_fj64_table = d_fj64_table;
    buffers.compact = CompactBuffers{
        d_row_prefix.get(),
        d_prime_pos.get(),
        d_prime_count.get(),
    };
    buffers.uf.in = UfInputBuffers{
        d_bitmap.get(),
        d_row_prefix.get(),
        d_prime_pos.get(),
        d_prime_count.get(),
        d_coords.get(),
    };
    buffers.uf.out.d_parent = d_parent.get();
    buffers.uf.out.d_prime_geo_bits = d_prime_geo_bits.get();
    buffers.uf.out.d_wire_label_by_raw_root = d_wire_label_by_raw_root.get();
    buffers.uf.out.d_max_label = d_max_label.get();
    buffers.uf.out.d_overflow = d_overflow.get();
    buffers.uf.out.d_group_flags = d_group_flags.get();

    launch_k1_to_k4(buffers, num_tiles, stream);

    out.prime_count.resize(tile_count);
    out.prime_pos.resize(tile_count * MAX_PRIMES_GPU);
    out.parent.resize(tile_count * MAX_PRIMES_GPU);
    out.prime_geo_bits.resize(tile_count * MAX_PRIMES_GPU);
    out.wire_label_by_raw_root.resize(tile_count * MAX_PRIMES_GPU);
    out.max_label.resize(tile_count);
    out.overflow.resize(tile_count);
    out.group_flags.resize(tile_count * 256U);

    check_cuda(cudaMemcpyAsync(out.prime_count.data(), d_prime_count.get(),
                               out.prime_count.size() * sizeof(std::uint32_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(prime_count)");
    check_cuda(cudaMemcpyAsync(out.prime_pos.data(), d_prime_pos.get(),
                               out.prime_pos.size() * sizeof(std::uint32_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(prime_pos)");
    check_cuda(cudaMemcpyAsync(out.parent.data(), d_parent.get(),
                               out.parent.size() * sizeof(std::uint16_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(parent)");
    check_cuda(cudaMemcpyAsync(out.prime_geo_bits.data(), d_prime_geo_bits.get(),
                               out.prime_geo_bits.size() *
                                   sizeof(std::uint8_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(prime_geo_bits)");
    check_cuda(cudaMemcpyAsync(out.wire_label_by_raw_root.data(),
                               d_wire_label_by_raw_root.get(),
                               out.wire_label_by_raw_root.size() *
                                   sizeof(std::uint16_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(wire_label_by_raw_root)");
    check_cuda(cudaMemcpyAsync(out.max_label.data(), d_max_label.get(),
                               out.max_label.size() * sizeof(std::uint16_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(max_label)");
    check_cuda(cudaMemcpyAsync(out.overflow.data(), d_overflow.get(),
                               out.overflow.size() * sizeof(std::uint8_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(overflow)");
    check_cuda(cudaMemcpyAsync(out.group_flags.data(), d_group_flags.get(),
                               out.group_flags.size() * sizeof(std::uint8_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(group_flags)");
    check_cuda(cudaStreamSynchronize(stream), "cudaStreamSynchronize");
  } catch (...) {
    free_fj64_table(d_fj64_table);
    throw;
  }

  free_fj64_table(d_fj64_table);
  return out;
}

}  // namespace cuda_campaign
