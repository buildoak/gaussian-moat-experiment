#include "cuda_campaign/host_driver.h"
#include "cuda_campaign/kernels.cuh"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "cuda_campaign/compact_buffers.cuh"
#include "cuda_campaign/constants.cuh"
#include "cuda_campaign/uf_buffers.cuh"

namespace cuda_campaign {

void launch_kernel_overflow_summary(
    const std::uint32_t* d_k1_overflow,
    const std::uint32_t* d_prime_count,
    const std::uint8_t* d_remap_overflow,
    const std::uint16_t* d_face_rep_counts,
    unsigned long long* d_summary_counts,
    int num_tiles,
    cudaStream_t stream = nullptr);

namespace {

inline constexpr int OVERFLOW_SUMMARY_COUNTERS = 4;

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

template <typename T>
class PinnedBuffer {
 public:
  PinnedBuffer() = default;

  explicit PinnedBuffer(std::size_t count) {
    allocate(count);
  }

  PinnedBuffer(const PinnedBuffer&) = delete;
  PinnedBuffer& operator=(const PinnedBuffer&) = delete;

  PinnedBuffer(PinnedBuffer&& other) noexcept
      : ptr_(other.ptr_), count_(other.count_) {
    other.ptr_ = nullptr;
    other.count_ = 0;
  }

  PinnedBuffer& operator=(PinnedBuffer&& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = other.ptr_;
      count_ = other.count_;
      other.ptr_ = nullptr;
      other.count_ = 0;
    }
    return *this;
  }

  ~PinnedBuffer() {
    reset();
  }

  void allocate(std::size_t count) {
    reset();
    count_ = count;
    if (count_ == 0) return;
    check_cuda(cudaHostAlloc(reinterpret_cast<void**>(&ptr_),
                             count_ * sizeof(T), cudaHostAllocDefault),
               "cudaHostAlloc");
  }

  void reset() noexcept {
    if (ptr_ != nullptr) {
      cudaFreeHost(ptr_);
      ptr_ = nullptr;
    }
    count_ = 0;
  }

  T* get() const noexcept {
    return ptr_;
  }

 private:
  T* ptr_ = nullptr;
  std::size_t count_ = 0;
};

class Stream {
 public:
  Stream() {
    check_cuda(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking),
               "cudaStreamCreateWithFlags");
  }

  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  ~Stream() {
    if (stream_ != nullptr) {
      cudaStreamDestroy(stream_);
    }
  }

  cudaStream_t get() const noexcept {
    return stream_;
  }

 private:
  cudaStream_t stream_ = nullptr;
};

class Event {
 public:
  Event() {
    check_cuda(cudaEventCreateWithFlags(&event_, cudaEventDisableTiming),
               "cudaEventCreateWithFlags");
  }

  Event(const Event&) = delete;
  Event& operator=(const Event&) = delete;

  ~Event() {
    if (event_ != nullptr) {
      cudaEventDestroy(event_);
    }
  }

  cudaEvent_t get() const noexcept {
    return event_;
  }

 private:
  cudaEvent_t event_ = nullptr;
};

void check_last_launch(const char* what) {
  check_cuda(cudaGetLastError(), what);
}

std::size_t checked_bytes(std::size_t count,
                          std::size_t bytes_per_item,
                          const char* what) {
  if (count != 0 &&
      bytes_per_item > std::numeric_limits<std::size_t>::max() / count) {
    throw std::overflow_error(std::string(what) + " byte count overflow");
  }
  return count * bytes_per_item;
}

std::size_t checked_add(std::size_t lhs,
                        std::size_t rhs,
                        const char* what) {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
    throw std::overflow_error(std::string(what) + " byte count overflow");
  }
  return lhs + rhs;
}

std::size_t pooled_device_bytes_for_tiles(std::size_t tiles, int ring_slots) {
  std::size_t total = 0;
  const auto add = [&](std::size_t count, std::size_t item_size,
                       const char* what) {
    total = checked_add(total, checked_bytes(count, item_size, what), what);
  };

  const std::size_t prime_slots = tiles * MAX_PRIMES_GPU;
  const std::size_t face_slots = tiles * NUM_FACES * MAX_PRIMES_GPU;
  const std::size_t face_count_slots = tiles * NUM_FACES;

  add(tiles, sizeof(campaign::TileCoord), "pooled d_coords");
  add(tiles * MAX_CANDIDATES_GPU, sizeof(std::uint32_t),
      "pooled d_cand_list");
  add(tiles, sizeof(std::uint32_t), "pooled d_total_cands");
  add(tiles, sizeof(std::uint32_t), "pooled d_raw_total_cands");
  add(tiles, sizeof(std::uint32_t), "pooled d_k1_overflow");
  add(tiles * BITMAP_WORDS, sizeof(std::uint32_t), "pooled d_bitmap");
  add(tiles * ROW_PREFIX_ENTRIES, sizeof(std::uint16_t),
      "pooled d_row_prefix");
  add(prime_slots, sizeof(std::uint32_t), "pooled d_prime_pos");
  add(tiles, sizeof(std::uint32_t), "pooled d_prime_count");
  add(prime_slots, sizeof(std::uint16_t), "pooled d_parent");
  add(prime_slots, sizeof(std::uint8_t), "pooled d_prime_geo_bits");
  add(prime_slots, sizeof(std::uint16_t),
      "pooled d_wire_label_by_raw_root");
  add(tiles, sizeof(std::uint16_t), "pooled d_max_label");
  add(tiles, sizeof(std::uint8_t), "pooled d_overflow");
  add(tiles * 256U, sizeof(std::uint8_t), "pooled d_group_flags");
  add(face_slots, sizeof(std::uint16_t), "pooled d_face_indices");
  add(face_count_slots, sizeof(std::uint16_t), "pooled d_face_counts");
  add(face_slots, sizeof(std::uint16_t), "pooled d_face_roots");
  add(face_slots, sizeof(FaceRepresentative), "pooled d_face_reps");
  add(face_count_slots, sizeof(std::uint16_t), "pooled d_face_rep_counts");
  add(OVERFLOW_SUMMARY_COUNTERS, sizeof(unsigned long long),
      "pooled d_overflow_summary");
  add(tiles * static_cast<std::size_t>(ring_slots), sizeof(TileOp),
      "pooled d_tileops");
  return total;
}

std::size_t device_slab_tiles_for(const DispatchConfig& config,
                                  std::size_t remaining_tiles) {
  if (remaining_tiles == 0) return 0;
  if (config.device_slab_tiles != 0) {
    return std::min(config.device_slab_tiles, remaining_tiles);
  }

  std::size_t free_bytes = 0;
  std::size_t total_bytes = 0;
  check_cuda(cudaMemGetInfo(&free_bytes, &total_bytes), "cudaMemGetInfo");
  (void)total_bytes;

  std::size_t budget = config.device_budget_bytes;
  if (free_bytes > config.device_safety_bytes) {
    budget = std::min(budget, free_bytes - config.device_safety_bytes);
  }
  const std::size_t per_tile =
      pooled_device_bytes_for_tiles(1, config.ring_slots);
  if (per_tile == 0 || budget < per_tile) {
    throw std::runtime_error("CUDA dispatch budget cannot fit one device slab");
  }
  return std::max<std::size_t>(1, std::min(remaining_tiles, budget / per_tile));
}

void dump_group_overflow_tile_csv(
    const char* path,
    const campaign::TileCoord& coord,
    std::uint32_t candidate_count,
    std::uint32_t prime_count,
    std::uint16_t max_label,
    const std::uint32_t* d_tile_prime_pos,
    const std::uint16_t* d_tile_parent,
    const std::uint16_t* d_tile_wire_label_by_raw_root) {
  std::vector<std::uint32_t> h_prime_pos(MAX_PRIMES_GPU);
  std::vector<std::uint16_t> h_parent(MAX_PRIMES_GPU);
  std::vector<std::uint16_t> h_wire_label_by_raw_root(MAX_PRIMES_GPU);

  check_cuda(cudaMemcpy(h_prime_pos.data(), d_tile_prime_pos,
                        h_prime_pos.size() * sizeof(std::uint32_t),
                        cudaMemcpyDeviceToHost),
             "cudaMemcpy(group_dump prime_pos)");
  check_cuda(cudaMemcpy(h_parent.data(), d_tile_parent,
                        h_parent.size() * sizeof(std::uint16_t),
                        cudaMemcpyDeviceToHost),
             "cudaMemcpy(group_dump parent)");
  check_cuda(cudaMemcpy(h_wire_label_by_raw_root.data(),
                        d_tile_wire_label_by_raw_root,
                        h_wire_label_by_raw_root.size() *
                            sizeof(std::uint16_t),
                        cudaMemcpyDeviceToHost),
             "cudaMemcpy(group_dump labels)");

  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error(std::string("failed to open group dump path: ") +
                             path);
  }

  out << "# tile_i," << coord.i << "\n";
  out << "# tile_j," << coord.j << "\n";
  out << "# a_lo," << coord.a_lo << "\n";
  out << "# b_lo," << coord.b_lo << "\n";
  out << "# candidate_count," << candidate_count << "\n";
  out << "# prime_count," << prime_count << "\n";
  out << "# max_label," << max_label << "\n";
  out << "prime_index,a,b,norm_sq,packed_pos,row,col,parent,final_group_label\n";

  const std::uint32_t bounded =
      std::min<std::uint32_t>(prime_count, MAX_PRIMES_GPU);
  for (std::uint32_t idx = 0; idx < bounded; ++idx) {
    const std::uint32_t packed = h_prime_pos[idx];
    const std::int64_t row = static_cast<std::int64_t>(packed / SIDE_EXP);
    const std::int64_t col = static_cast<std::int64_t>(packed % SIDE_EXP);
    const std::int64_t a = coord.a_lo + col - static_cast<std::int64_t>(C);
    const std::int64_t b = coord.b_lo + row - static_cast<std::int64_t>(C);
    const std::uint64_t norm_sq =
        static_cast<std::uint64_t>(a * a) +
        static_cast<std::uint64_t>(b * b);
    const std::uint16_t parent = h_parent[idx];
    const std::uint16_t label =
        parent < MAX_PRIMES_GPU ? h_wire_label_by_raw_root[parent] : 0;
    out << idx << "," << a << "," << b << "," << norm_sq << ","
        << packed << "," << row << "," << col << "," << parent << ","
        << label << "\n";
  }
}

struct HostRingSlot {
  PinnedBuffer<campaign::TileCoord> coords;
  PinnedBuffer<campaign::TileOp> tileops;
  DeviceBuffer<TileOp> d_tileops;
  Event h2d_done;
  Event compute_done;
  Event d2h_done;

  HostRingSlot(std::size_t host_chunk_tiles, std::size_t device_slab_tiles)
      : coords(host_chunk_tiles),
        tileops(host_chunk_tiles),
        d_tileops(device_slab_tiles) {}
};

struct PendingD2H {
  bool active = false;
  std::size_t global_offset = 0;
  std::size_t tiles = 0;
};

struct Streams {
  Stream h2d;
  Stream compute;
  Stream d2h;
};

void drain_pending_d2h(HostRingSlot& slot,
                       PendingD2H& pending,
                       campaign::TileOp* output_tileops) {
  if (!pending.active) return;
  check_cuda(cudaEventSynchronize(slot.d2h_done.get()),
             "cudaEventSynchronize(d2h_done)");
  std::memcpy(output_tileops + pending.global_offset, slot.tileops.get(),
              pending.tiles * sizeof(campaign::TileOp));
  pending = PendingD2H{};
}

struct DeviceWorkspace {
  explicit DeviceWorkspace(std::size_t capacity)
      : capacity_tiles(capacity),
        d_coords(capacity),
        d_cand_list(capacity * MAX_CANDIDATES_GPU),
        d_total_cands(capacity),
        d_raw_total_cands(capacity),
        d_k1_overflow(capacity),
        d_bitmap(capacity * BITMAP_WORDS),
        d_row_prefix(capacity * ROW_PREFIX_ENTRIES),
        d_prime_pos(capacity * MAX_PRIMES_GPU),
        d_prime_count(capacity),
        d_parent(capacity * MAX_PRIMES_GPU),
        d_prime_geo_bits(capacity * MAX_PRIMES_GPU),
        d_wire_label_by_raw_root(capacity * MAX_PRIMES_GPU),
        d_max_label(capacity),
        d_overflow(capacity),
        d_group_flags(capacity * 256U),
        d_face_indices(capacity * NUM_FACES * MAX_PRIMES_GPU),
        d_face_counts(capacity * NUM_FACES),
        d_face_roots(capacity * NUM_FACES * MAX_PRIMES_GPU),
        d_face_reps(capacity * NUM_FACES * MAX_PRIMES_GPU),
        d_face_rep_counts(capacity * NUM_FACES),
        d_overflow_summary(OVERFLOW_SUMMARY_COUNTERS) {}

  std::size_t capacity_tiles = 0;
  DeviceBuffer<campaign::TileCoord> d_coords;
  DeviceBuffer<std::uint32_t> d_cand_list;
  DeviceBuffer<std::uint32_t> d_total_cands;
  DeviceBuffer<std::uint32_t> d_raw_total_cands;
  DeviceBuffer<std::uint32_t> d_k1_overflow;
  DeviceBuffer<std::uint32_t> d_bitmap;
  DeviceBuffer<std::uint16_t> d_row_prefix;
  DeviceBuffer<std::uint32_t> d_prime_pos;
  DeviceBuffer<std::uint32_t> d_prime_count;
  DeviceBuffer<std::uint16_t> d_parent;
  DeviceBuffer<std::uint8_t> d_prime_geo_bits;
  DeviceBuffer<std::uint16_t> d_wire_label_by_raw_root;
  DeviceBuffer<std::uint16_t> d_max_label;
  DeviceBuffer<std::uint8_t> d_overflow;
  DeviceBuffer<std::uint8_t> d_group_flags;
  DeviceBuffer<std::uint16_t> d_face_indices;
  DeviceBuffer<std::uint16_t> d_face_counts;
  DeviceBuffer<std::uint16_t> d_face_roots;
  DeviceBuffer<FaceRepresentative> d_face_reps;
  DeviceBuffer<std::uint16_t> d_face_rep_counts;
  DeviceBuffer<unsigned long long> d_overflow_summary;
};

void fill_k1k4_buffers(K1K4Buffers* buffers,
                       campaign::TileCoord* d_coords,
                       std::uint32_t* d_cand_list,
                       std::uint32_t* d_total_cands,
                       std::uint32_t* d_raw_total_cands,
                       std::uint32_t* d_k1_overflow,
                       std::uint32_t* d_bitmap,
                       std::uint16_t* d_fj64_table,
                       std::uint16_t* d_row_prefix,
                       std::uint32_t* d_prime_pos,
                       std::uint32_t* d_prime_count,
                       std::uint16_t* d_parent,
                       std::uint8_t* d_prime_geo_bits,
                       std::uint16_t* d_wire_label_by_raw_root,
                       std::uint16_t* d_max_label,
                       std::uint8_t* d_overflow,
                       std::uint8_t* d_group_flags) {
  *buffers = K1K4Buffers{};
  buffers->d_coords = d_coords;
  buffers->d_cand_list = d_cand_list;
  buffers->d_total_cands = d_total_cands;
  buffers->d_raw_total_cands = d_raw_total_cands;
  buffers->d_k1_overflow = d_k1_overflow;
  buffers->d_bitmap = d_bitmap;
  buffers->d_fj64_table = d_fj64_table;
  buffers->compact = CompactBuffers{
      d_row_prefix,
      d_prime_pos,
      d_prime_count,
  };
  buffers->uf.in = UfInputBuffers{
      d_bitmap,
      d_row_prefix,
      d_prime_pos,
      d_prime_count,
      d_coords,
      d_k1_overflow,
  };
  buffers->uf.out.d_parent = d_parent;
  buffers->uf.out.d_prime_geo_bits = d_prime_geo_bits;
  buffers->uf.out.d_wire_label_by_raw_root = d_wire_label_by_raw_root;
  buffers->uf.out.d_max_label = d_max_label;
  buffers->uf.out.d_overflow = d_overflow;
  buffers->uf.out.d_group_flags = d_group_flags;
}

}  // namespace

std::size_t phase1_bytes_for_tiles(std::size_t tiles) {
  const std::size_t per_tile =
      sizeof(campaign::TileCoord) +
      sizeof(std::uint32_t) * MAX_CANDIDATES_GPU +
      sizeof(std::uint32_t) +
      sizeof(std::uint32_t) +
      sizeof(std::uint32_t) +
      sizeof(std::uint32_t) * BITMAP_WORDS;
  return checked_bytes(tiles, per_tile, "phase1");
}

std::size_t phase2_bytes_for_tiles(std::size_t tiles) {
  const std::size_t prime_slots = MAX_PRIMES_GPU;
  const std::size_t face_slots = NUM_FACES * MAX_PRIMES_GPU;
  const std::size_t face_count_slots = NUM_FACES;
  const std::size_t per_tile =
      sizeof(campaign::TileCoord) +
      sizeof(std::uint32_t) * BITMAP_WORDS +
      sizeof(std::uint32_t) +
      sizeof(std::uint16_t) * ROW_PREFIX_ENTRIES +
      sizeof(std::uint32_t) * prime_slots +
      sizeof(std::uint32_t) +
      sizeof(std::uint16_t) * prime_slots +
      sizeof(std::uint8_t) * prime_slots +
      sizeof(std::uint16_t) * prime_slots +
      sizeof(std::uint16_t) +
      sizeof(std::uint8_t) +
      sizeof(std::uint8_t) * 256U +
      sizeof(TileOp) +
      sizeof(std::uint16_t) * face_slots +
      sizeof(std::uint16_t) * face_count_slots +
      sizeof(std::uint16_t) * face_slots +
      sizeof(FaceRepresentative) * face_slots +
      sizeof(std::uint16_t) * face_count_slots;
  return checked_bytes(tiles, per_tile, "phase2");
}

std::size_t pinned_bytes_for_tiles(std::size_t tiles, int ring_slots) {
  if (ring_slots <= 0) {
    throw std::invalid_argument("ring_slots must be positive");
  }
  const std::size_t per_slot =
      checked_bytes(tiles, sizeof(campaign::TileCoord), "pinned input") +
      checked_bytes(tiles, sizeof(campaign::TileOp), "pinned output");
  return checked_bytes(static_cast<std::size_t>(ring_slots), per_slot,
                       "pinned ring");
}

void launch_k1_to_k4(const K1K4Buffers& buffers,
                     int num_tiles,
                     cudaStream_t stream) {
  launch_kernel_sieve(buffers.d_coords, buffers.d_cand_list,
                      buffers.d_total_cands, buffers.d_raw_total_cands,
                      buffers.d_k1_overflow, num_tiles,
                      buffers.k1_candidate_capacity, stream);
  check_last_launch("launch_kernel_sieve");

  launch_kernel_mr(buffers.d_coords, buffers.d_cand_list, buffers.d_total_cands,
                   buffers.d_bitmap, buffers.d_fj64_table, num_tiles, stream);
  check_last_launch("launch_kernel_mr");

  launch_kernel_compact(buffers.d_bitmap, buffers.compact, num_tiles, stream);
  check_last_launch("launch_kernel_compact");

  launch_kernel_uf_v2(buffers.uf, num_tiles, stream);
  check_last_launch("launch_kernel_uf_v2");
}

void launch_k1_to_k5(const K1K5Buffers& buffers,
                     int num_tiles,
                     cudaStream_t stream) {
  launch_k1_to_k4(buffers.k1k4, num_tiles, stream);

  FaceEncodeBuffers face_encode{};
  face_encode.in.d_coords = buffers.k1k4.d_coords;
  face_encode.in.d_prime_pos = buffers.k1k4.compact.d_prime_pos;
  face_encode.in.d_prime_count = buffers.k1k4.compact.d_prime_count;
  face_encode.in.d_remap_overflow = buffers.k1k4.uf.out.d_overflow;
  face_encode.in.d_parent = buffers.k1k4.uf.out.d_parent;
  face_encode.in.d_wire_label_by_raw_root =
      buffers.k1k4.uf.out.d_wire_label_by_raw_root;
  face_encode.in.d_group_flags = buffers.k1k4.uf.out.d_group_flags;
  face_encode.d_tileops = buffers.d_tileops;
  face_encode.debug = buffers.face_debug;
  launch_kernel_face_encode_v2(face_encode, num_tiles, stream);
  check_last_launch("launch_kernel_face_encode_v2");

  FaceSortPackBuffers sort_pack{};
  sort_pack.in.d_face_reps = buffers.face_debug.d_face_reps;
  sort_pack.in.d_face_rep_counts = buffers.face_debug.d_face_rep_counts;
  sort_pack.d_tileops = buffers.d_tileops;
  launch_kernel_face_sort_pack(sort_pack, num_tiles, stream);
  check_last_launch("launch_kernel_face_sort_pack");
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
    DeviceBuffer<std::uint32_t> d_raw_total_cands(tile_count);
    DeviceBuffer<std::uint32_t> d_k1_overflow(tile_count);
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
    buffers.d_raw_total_cands = d_raw_total_cands.get();
    buffers.d_k1_overflow = d_k1_overflow.get();
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
        d_k1_overflow.get(),
    };
    buffers.uf.out.d_parent = d_parent.get();
    buffers.uf.out.d_prime_geo_bits = d_prime_geo_bits.get();
    buffers.uf.out.d_wire_label_by_raw_root = d_wire_label_by_raw_root.get();
    buffers.uf.out.d_max_label = d_max_label.get();
    buffers.uf.out.d_overflow = d_overflow.get();
    buffers.uf.out.d_group_flags = d_group_flags.get();

    launch_k1_to_k4(buffers, num_tiles, stream);

    out.candidate_count.resize(tile_count);
    out.bitmap.resize(tile_count * BITMAP_WORDS);
    out.prime_count.resize(tile_count);
    out.prime_pos.resize(tile_count * MAX_PRIMES_GPU);
    out.parent.resize(tile_count * MAX_PRIMES_GPU);
    out.prime_geo_bits.resize(tile_count * MAX_PRIMES_GPU);
    out.wire_label_by_raw_root.resize(tile_count * MAX_PRIMES_GPU);
    out.max_label.resize(tile_count);
    out.overflow.resize(tile_count);
    out.group_flags.resize(tile_count * 256U);

    check_cuda(cudaMemcpyAsync(out.candidate_count.data(),
                               d_raw_total_cands.get(),
                               out.candidate_count.size() *
                                   sizeof(std::uint32_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(candidate_count)");
    check_cuda(cudaMemcpyAsync(out.bitmap.data(), d_bitmap.get(),
                               out.bitmap.size() * sizeof(std::uint32_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(bitmap)");
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

K1K5DebugDownload run_k1_to_k5_debug(
    const std::vector<campaign::TileCoord>& coords,
    const campaign::CampaignConstants& constants,
    int k1_candidate_capacity,
    cudaStream_t stream) {
  K1K5DebugDownload out;
  if (coords.empty()) {
    return out;
  }

  static_assert(sizeof(campaign::TileOp) == sizeof(TileOp),
                "CPU and CUDA TileOp layouts must remain byte-identical");

  const int num_tiles = static_cast<int>(coords.size());
  const std::size_t tile_count = coords.size();
  const std::size_t prime_slots = tile_count * MAX_PRIMES_GPU;
  const std::size_t face_slots = tile_count * NUM_FACES * MAX_PRIMES_GPU;
  const std::size_t face_count_slots = tile_count * NUM_FACES;

  upload_cuda_constants(constants);

  std::uint16_t* d_fj64_table = nullptr;
  upload_fj64_table(&d_fj64_table);

  try {
    DeviceBuffer<campaign::TileCoord> d_coords(tile_count);
    DeviceBuffer<std::uint32_t> d_cand_list(tile_count * MAX_CANDIDATES_GPU);
    DeviceBuffer<std::uint32_t> d_total_cands(tile_count);
    DeviceBuffer<std::uint32_t> d_raw_total_cands(tile_count);
    DeviceBuffer<std::uint32_t> d_k1_overflow(tile_count);
    DeviceBuffer<std::uint32_t> d_bitmap(tile_count * BITMAP_WORDS);
    DeviceBuffer<std::uint16_t> d_row_prefix(tile_count * ROW_PREFIX_ENTRIES);
    DeviceBuffer<std::uint32_t> d_prime_pos(prime_slots);
    DeviceBuffer<std::uint32_t> d_prime_count(tile_count);
    DeviceBuffer<std::uint16_t> d_parent(prime_slots);
    DeviceBuffer<std::uint8_t> d_prime_geo_bits(prime_slots);
    DeviceBuffer<std::uint16_t> d_wire_label_by_raw_root(prime_slots);
    DeviceBuffer<std::uint16_t> d_max_label(tile_count);
    DeviceBuffer<std::uint8_t> d_overflow(tile_count);
    DeviceBuffer<std::uint8_t> d_group_flags(tile_count * 256U);
    DeviceBuffer<TileOp> d_tileops(tile_count);
    DeviceBuffer<std::uint16_t> d_face_indices(face_slots);
    DeviceBuffer<std::uint16_t> d_face_counts(face_count_slots);
    DeviceBuffer<std::uint16_t> d_face_roots(face_slots);
    DeviceBuffer<FaceRepresentative> d_face_reps(face_slots);
    DeviceBuffer<std::uint16_t> d_face_rep_counts(face_count_slots);

    check_cuda(cudaMemcpyAsync(d_coords.get(), coords.data(),
                               tile_count * sizeof(campaign::TileCoord),
                               cudaMemcpyHostToDevice, stream),
               "cudaMemcpyAsync(d_coords)");
    check_cuda(cudaMemsetAsync(d_face_indices.get(), 0,
                               face_slots * sizeof(std::uint16_t), stream),
               "cudaMemsetAsync(face_indices)");
    check_cuda(cudaMemsetAsync(d_face_counts.get(), 0,
                               face_count_slots * sizeof(std::uint16_t),
                               stream),
               "cudaMemsetAsync(face_counts)");
    check_cuda(cudaMemsetAsync(d_face_roots.get(), 0,
                               face_slots * sizeof(std::uint16_t), stream),
               "cudaMemsetAsync(face_roots)");
    check_cuda(cudaMemsetAsync(d_face_reps.get(), 0,
                               face_slots * sizeof(FaceRepresentative), stream),
               "cudaMemsetAsync(face_reps)");
    check_cuda(cudaMemsetAsync(d_face_rep_counts.get(), 0,
                               face_count_slots * sizeof(std::uint16_t),
                               stream),
               "cudaMemsetAsync(face_rep_counts)");

    K1K5Buffers buffers{};
    buffers.k1k4.d_coords = d_coords.get();
    buffers.k1k4.d_cand_list = d_cand_list.get();
    buffers.k1k4.d_total_cands = d_total_cands.get();
    buffers.k1k4.d_raw_total_cands = d_raw_total_cands.get();
    buffers.k1k4.d_k1_overflow = d_k1_overflow.get();
    buffers.k1k4.d_bitmap = d_bitmap.get();
    buffers.k1k4.d_fj64_table = d_fj64_table;
    buffers.k1k4.k1_candidate_capacity = k1_candidate_capacity;
    buffers.k1k4.compact = CompactBuffers{
        d_row_prefix.get(),
        d_prime_pos.get(),
        d_prime_count.get(),
    };
    buffers.k1k4.uf.in = UfInputBuffers{
        d_bitmap.get(),
        d_row_prefix.get(),
        d_prime_pos.get(),
        d_prime_count.get(),
        d_coords.get(),
        d_k1_overflow.get(),
    };
    buffers.k1k4.uf.out.d_parent = d_parent.get();
    buffers.k1k4.uf.out.d_prime_geo_bits = d_prime_geo_bits.get();
    buffers.k1k4.uf.out.d_wire_label_by_raw_root =
        d_wire_label_by_raw_root.get();
    buffers.k1k4.uf.out.d_max_label = d_max_label.get();
    buffers.k1k4.uf.out.d_overflow = d_overflow.get();
    buffers.k1k4.uf.out.d_group_flags = d_group_flags.get();
    buffers.d_tileops = d_tileops.get();
    buffers.face_debug.d_face_indices = d_face_indices.get();
    buffers.face_debug.d_face_counts = d_face_counts.get();
    buffers.face_debug.d_face_roots = d_face_roots.get();
    buffers.face_debug.d_face_reps = d_face_reps.get();
    buffers.face_debug.d_face_rep_counts = d_face_rep_counts.get();

    launch_k1_to_k5(buffers, num_tiles, stream);

    out.k1k4.candidate_count.resize(tile_count);
    out.k1k4.bitmap.resize(tile_count * BITMAP_WORDS);
    out.k1k4.prime_count.resize(tile_count);
    out.k1k4.prime_pos.resize(prime_slots);
    out.k1k4.parent.resize(prime_slots);
    out.k1k4.prime_geo_bits.resize(prime_slots);
    out.k1k4.wire_label_by_raw_root.resize(prime_slots);
    out.k1k4.max_label.resize(tile_count);
    out.k1k4.overflow.resize(tile_count);
    out.k1k4.group_flags.resize(tile_count * 256U);
    out.tileops.resize(tile_count);
    out.face_indices.resize(face_slots);
    out.face_counts.resize(face_count_slots);
    out.face_roots.resize(face_slots);
    out.face_reps.resize(face_slots);
    out.face_rep_counts.resize(face_count_slots);

    check_cuda(cudaMemcpyAsync(out.k1k4.candidate_count.data(),
                               d_raw_total_cands.get(),
                               out.k1k4.candidate_count.size() *
                                   sizeof(std::uint32_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(candidate_count)");
    check_cuda(cudaMemcpyAsync(out.k1k4.bitmap.data(), d_bitmap.get(),
                               out.k1k4.bitmap.size() *
                                   sizeof(std::uint32_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(bitmap)");
    check_cuda(cudaMemcpyAsync(out.k1k4.prime_count.data(),
                               d_prime_count.get(),
                               out.k1k4.prime_count.size() *
                                   sizeof(std::uint32_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(prime_count)");
    check_cuda(cudaMemcpyAsync(out.k1k4.prime_pos.data(), d_prime_pos.get(),
                               out.k1k4.prime_pos.size() *
                                   sizeof(std::uint32_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(prime_pos)");
    check_cuda(cudaMemcpyAsync(out.k1k4.parent.data(), d_parent.get(),
                               out.k1k4.parent.size() * sizeof(std::uint16_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(parent)");
    check_cuda(cudaMemcpyAsync(out.k1k4.prime_geo_bits.data(),
                               d_prime_geo_bits.get(),
                               out.k1k4.prime_geo_bits.size() *
                                   sizeof(std::uint8_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(prime_geo_bits)");
    check_cuda(cudaMemcpyAsync(out.k1k4.wire_label_by_raw_root.data(),
                               d_wire_label_by_raw_root.get(),
                               out.k1k4.wire_label_by_raw_root.size() *
                                   sizeof(std::uint16_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(wire_label_by_raw_root)");
    check_cuda(cudaMemcpyAsync(out.k1k4.max_label.data(), d_max_label.get(),
                               out.k1k4.max_label.size() *
                                   sizeof(std::uint16_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(max_label)");
    check_cuda(cudaMemcpyAsync(out.k1k4.overflow.data(), d_overflow.get(),
                               out.k1k4.overflow.size() *
                                   sizeof(std::uint8_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(overflow)");
    check_cuda(cudaMemcpyAsync(out.k1k4.group_flags.data(),
                               d_group_flags.get(),
                               out.k1k4.group_flags.size() *
                                   sizeof(std::uint8_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(group_flags)");
    check_cuda(cudaMemcpyAsync(out.tileops.data(), d_tileops.get(),
                               out.tileops.size() * sizeof(campaign::TileOp),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(tileops)");
    check_cuda(cudaMemcpyAsync(out.face_indices.data(), d_face_indices.get(),
                               out.face_indices.size() *
                                   sizeof(std::uint16_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(face_indices)");
    check_cuda(cudaMemcpyAsync(out.face_counts.data(), d_face_counts.get(),
                               out.face_counts.size() *
                                   sizeof(std::uint16_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(face_counts)");
    check_cuda(cudaMemcpyAsync(out.face_roots.data(), d_face_roots.get(),
                               out.face_roots.size() * sizeof(std::uint16_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(face_roots)");
    check_cuda(cudaMemcpyAsync(out.face_reps.data(), d_face_reps.get(),
                               out.face_reps.size() *
                                   sizeof(FaceRepresentative),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(face_reps)");
    check_cuda(cudaMemcpyAsync(out.face_rep_counts.data(),
                               d_face_rep_counts.get(),
                               out.face_rep_counts.size() *
                                   sizeof(std::uint16_t),
                               cudaMemcpyDeviceToHost, stream),
               "cudaMemcpyAsync(face_rep_counts)");
    check_cuda(cudaStreamSynchronize(stream), "cudaStreamSynchronize");
  } catch (...) {
    free_fj64_table(d_fj64_table);
    throw;
  }

  free_fj64_table(d_fj64_table);
  return out;
}

class TileBatchDispatcher::Impl {
 public:
  Impl(const campaign::CampaignConstants& constants,
       const DispatchConfig& config)
      : config_(config) {
    upload_cuda_constants(constants);
    upload_fj64_table(&d_fj64_table_);
  }

  ~Impl() {
    if (d_fj64_table_ != nullptr) {
      cudaFree(d_fj64_table_);
    }
  }

  void dispatch(const campaign::TileCoord* tiles,
                std::size_t count,
                campaign::TileOp* output_tileops,
                DispatchStats* stats);

 private:
  DispatchConfig config_;
  std::uint16_t* d_fj64_table_ = nullptr;
};

void TileBatchDispatcher::Impl::dispatch(const campaign::TileCoord* tiles,
                                         std::size_t count,
                                         campaign::TileOp* output_tileops,
                                         DispatchStats* stats) {
  if (count == 0) {
    if (stats != nullptr) {
      *stats = DispatchStats{};
    }
    return;
  }
  if (tiles == nullptr) {
    throw std::invalid_argument("dispatch_tile_batch tiles pointer is null");
  }
  if (output_tileops == nullptr) {
    throw std::invalid_argument("dispatch_tile_batch output pointer is null");
  }
  if (config_.host_chunk_tiles == 0) {
    throw std::invalid_argument("host_chunk_tiles must be positive");
  }
  if (config_.ring_slots <= 0) {
    throw std::invalid_argument("ring_slots must be positive");
  }
  if (count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("dispatch_tile_batch count exceeds int launch ABI");
  }

  static_assert(sizeof(campaign::TileOp) == sizeof(TileOp),
                "CPU and CUDA TileOp layouts must remain byte-identical");

    Streams streams;
    const std::size_t max_requested_slab =
        std::min(config_.host_chunk_tiles, count);
    const std::size_t device_slab_capacity =
        device_slab_tiles_for(config_, max_requested_slab);
    std::vector<std::unique_ptr<HostRingSlot>> ring;
    std::vector<PendingD2H> pending(
        static_cast<std::size_t>(config_.ring_slots));
    ring.reserve(static_cast<std::size_t>(config_.ring_slots));
    for (int slot = 0; slot < config_.ring_slots; ++slot) {
      ring.emplace_back(std::make_unique<HostRingSlot>(config_.host_chunk_tiles,
                                                       device_slab_capacity));
    }
    DeviceWorkspace workspace(device_slab_capacity);

    DispatchStats local_stats{};
    local_stats.tiles = count;
    local_stats.host_chunk_tiles = config_.host_chunk_tiles;
    local_stats.pinned_host_bytes =
        pinned_bytes_for_tiles(config_.host_chunk_tiles, config_.ring_slots);
    local_stats.stream_count = 3;
    const char* group_dump_path = std::getenv("CUDA_CAMPAIGN_GROUP_DUMP");
    const bool group_dump_abort =
        std::getenv("CUDA_CAMPAIGN_GROUP_DUMP_ABORT") != nullptr;
    bool group_dump_written = false;
    const bool collect_overflow_diagnostics =
        config_.overflow_diagnostics || group_dump_path != nullptr;
    const bool collect_overflow_summary =
        stats != nullptr && !collect_overflow_diagnostics;
    const std::size_t max_overflow_diagnostics =
        collect_overflow_diagnostics ? config_.max_overflow_diagnostics : 0;

    if (collect_overflow_summary) {
      check_cuda(cudaMemsetAsync(workspace.d_overflow_summary.get(), 0,
                                 OVERFLOW_SUMMARY_COUNTERS *
                                     sizeof(unsigned long long),
                                 streams.compute.get()),
                 "cudaMemsetAsync(overflow_summary)");
    }

    int device = 0;
    cudaDeviceProp prop{};
    if (cudaGetDevice(&device) == cudaSuccess &&
        cudaGetDeviceProperties(&prop, device) == cudaSuccess) {
      local_stats.device_name = prop.name;
    }

    std::size_t copied = 0;
    std::size_t slab_index = 0;
    while (copied < count) {
      const std::size_t host_chunk =
          std::min(config_.host_chunk_tiles, count - copied);
      local_stats.chunks += 1;

      std::size_t chunk_copied = 0;
      while (chunk_copied < host_chunk) {
        const std::size_t remaining = host_chunk - chunk_copied;
        const std::size_t slab_tiles =
            std::min(device_slab_capacity, remaining);
        if (slab_tiles > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
          throw std::invalid_argument("device slab exceeds int launch ABI");
        }

        local_stats.device_slab_tiles =
            std::max(local_stats.device_slab_tiles, slab_tiles);
        local_stats.phase1_peak_bytes =
            std::max(local_stats.phase1_peak_bytes,
                     phase1_bytes_for_tiles(slab_tiles));
        local_stats.phase2_peak_bytes =
            std::max(local_stats.phase2_peak_bytes,
                     phase2_bytes_for_tiles(slab_tiles));
        local_stats.slabs += 1;

        HostRingSlot& slot =
            *ring[slab_index % static_cast<std::size_t>(config_.ring_slots)];
        PendingD2H& slot_pending =
            pending[slab_index % static_cast<std::size_t>(config_.ring_slots)];
        drain_pending_d2h(slot, slot_pending, output_tileops);
        if (slab_index != 0) {
          check_cuda(cudaStreamSynchronize(streams.compute.get()),
                     "cudaStreamSynchronize(compute workspace reuse)");
        }

        const std::size_t global_offset = copied + chunk_copied;
        std::memcpy(slot.coords.get(), tiles + global_offset,
                    slab_tiles * sizeof(campaign::TileCoord));

        check_cuda(cudaMemcpyAsync(workspace.d_coords.get(), slot.coords.get(),
                                   slab_tiles * sizeof(campaign::TileCoord),
                                   cudaMemcpyHostToDevice, streams.h2d.get()),
                   "cudaMemcpyAsync(dispatch coords H2D)");
        check_cuda(cudaEventRecord(slot.h2d_done.get(), streams.h2d.get()),
                   "cudaEventRecord(h2d_done)");
        check_cuda(cudaStreamWaitEvent(streams.compute.get(),
                                       slot.h2d_done.get(), 0),
                   "cudaStreamWaitEvent(h2d_done)");

        const int num_tiles = static_cast<int>(slab_tiles);
        launch_kernel_sieve(workspace.d_coords.get(),
                            workspace.d_cand_list.get(),
                            workspace.d_total_cands.get(),
                            workspace.d_raw_total_cands.get(),
                            workspace.d_k1_overflow.get(), num_tiles,
                            MAX_CANDIDATES_GPU, streams.compute.get());
        check_last_launch("dispatch launch_kernel_sieve");
        launch_kernel_mr(workspace.d_coords.get(),
                         workspace.d_cand_list.get(),
                         workspace.d_total_cands.get(),
                         workspace.d_bitmap.get(), d_fj64_table_, num_tiles,
                         streams.compute.get());
        check_last_launch("dispatch launch_kernel_mr");

        const std::size_t prime_slots = slab_tiles * MAX_PRIMES_GPU;
        const std::size_t face_slots =
            slab_tiles * NUM_FACES * MAX_PRIMES_GPU;
        const std::size_t face_count_slots = slab_tiles * NUM_FACES;
        (void)prime_slots;
        (void)face_slots;
        (void)face_count_slots;

        K1K4Buffers k1k4{};
        fill_k1k4_buffers(
            &k1k4, workspace.d_coords.get(), nullptr, nullptr,
            workspace.d_raw_total_cands.get(), workspace.d_k1_overflow.get(),
            workspace.d_bitmap.get(), d_fj64_table_,
            workspace.d_row_prefix.get(), workspace.d_prime_pos.get(),
            workspace.d_prime_count.get(), workspace.d_parent.get(),
            workspace.d_prime_geo_bits.get(),
            workspace.d_wire_label_by_raw_root.get(),
            workspace.d_max_label.get(), workspace.d_overflow.get(),
            workspace.d_group_flags.get());

        launch_kernel_compact(workspace.d_bitmap.get(), k1k4.compact,
                              num_tiles, streams.compute.get());
        check_last_launch("dispatch launch_kernel_compact");
        launch_kernel_uf_v2(k1k4.uf, num_tiles, streams.compute.get());
        check_last_launch("dispatch launch_kernel_uf_v2");

        K1K5Buffers k1k5{};
        k1k5.k1k4 = k1k4;
        k1k5.d_tileops = slot.d_tileops.get();
        k1k5.face_debug.d_face_indices = workspace.d_face_indices.get();
        k1k5.face_debug.d_face_counts = workspace.d_face_counts.get();
        k1k5.face_debug.d_face_roots = workspace.d_face_roots.get();
        k1k5.face_debug.d_face_reps = workspace.d_face_reps.get();
        k1k5.face_debug.d_face_rep_counts =
            workspace.d_face_rep_counts.get();

        FaceEncodeBuffers face_encode{};
        face_encode.in.d_coords = k1k5.k1k4.d_coords;
        face_encode.in.d_prime_pos = k1k5.k1k4.compact.d_prime_pos;
        face_encode.in.d_prime_count = k1k5.k1k4.compact.d_prime_count;
        face_encode.in.d_remap_overflow = k1k5.k1k4.uf.out.d_overflow;
        face_encode.in.d_parent = k1k5.k1k4.uf.out.d_parent;
        face_encode.in.d_wire_label_by_raw_root =
            k1k5.k1k4.uf.out.d_wire_label_by_raw_root;
        face_encode.in.d_group_flags = k1k5.k1k4.uf.out.d_group_flags;
        face_encode.d_tileops = k1k5.d_tileops;
        face_encode.debug = k1k5.face_debug;
        launch_kernel_face_encode_v2(face_encode, num_tiles,
                                     streams.compute.get());
        check_last_launch("dispatch launch_kernel_face_encode_v2");

        FaceSortPackBuffers sort_pack{};
        sort_pack.in.d_face_reps = k1k5.face_debug.d_face_reps;
        sort_pack.in.d_face_rep_counts = k1k5.face_debug.d_face_rep_counts;
        sort_pack.d_tileops = k1k5.d_tileops;
        launch_kernel_face_sort_pack(sort_pack, num_tiles,
                                     streams.compute.get());
        check_last_launch("dispatch launch_kernel_face_sort_pack");

        if (collect_overflow_summary) {
          launch_kernel_overflow_summary(
              workspace.d_k1_overflow.get(), workspace.d_prime_count.get(),
              workspace.d_overflow.get(), workspace.d_face_rep_counts.get(),
              workspace.d_overflow_summary.get(), num_tiles,
              streams.compute.get());
          check_last_launch("dispatch launch_kernel_overflow_summary");
        }

        check_cuda(cudaEventRecord(slot.compute_done.get(),
                                   streams.compute.get()),
                   "cudaEventRecord(compute_done)");
        if (stats != nullptr && collect_overflow_diagnostics) {
          check_cuda(cudaEventSynchronize(slot.compute_done.get()),
                     "cudaEventSynchronize(compute_done)");

          std::vector<std::uint32_t> h_k1_overflow(slab_tiles);
          std::vector<std::uint32_t> h_prime_count(slab_tiles);
          std::vector<std::uint8_t> h_remap_overflow(slab_tiles);
          std::vector<std::uint16_t> h_face_rep_counts(face_count_slots);
          check_cuda(cudaMemcpy(h_k1_overflow.data(),
                                workspace.d_k1_overflow.get(),
                                h_k1_overflow.size() * sizeof(std::uint32_t),
                                cudaMemcpyDeviceToHost),
                     "cudaMemcpy(k1_overflow)");
          check_cuda(cudaMemcpy(h_prime_count.data(),
                                workspace.d_prime_count.get(),
                                h_prime_count.size() * sizeof(std::uint32_t),
                                cudaMemcpyDeviceToHost),
                     "cudaMemcpy(prime_count)");
          check_cuda(cudaMemcpy(h_remap_overflow.data(),
                                workspace.d_overflow.get(),
                                h_remap_overflow.size() * sizeof(std::uint8_t),
                                cudaMemcpyDeviceToHost),
                     "cudaMemcpy(remap_overflow)");
          check_cuda(cudaMemcpy(h_face_rep_counts.data(),
                                workspace.d_face_rep_counts.get(),
                                h_face_rep_counts.size() *
                                    sizeof(std::uint16_t),
                                cudaMemcpyDeviceToHost),
                     "cudaMemcpy(face_rep_counts)");

          std::vector<std::uint32_t> h_raw_total_cands;
          std::vector<std::uint16_t> h_max_label;
          if (collect_overflow_diagnostics) {
            h_raw_total_cands.resize(slab_tiles);
            h_max_label.resize(slab_tiles);
            check_cuda(cudaMemcpy(h_raw_total_cands.data(),
                                  workspace.d_raw_total_cands.get(),
                                  h_raw_total_cands.size() *
                                      sizeof(std::uint32_t),
                                  cudaMemcpyDeviceToHost),
                       "cudaMemcpy(raw_total_cands)");
            check_cuda(cudaMemcpy(h_max_label.data(),
                                  workspace.d_max_label.get(),
                                  h_max_label.size() *
                                      sizeof(std::uint16_t),
                                  cudaMemcpyDeviceToHost),
                       "cudaMemcpy(max_label)");
          }

          for (std::size_t t = 0; t < slab_tiles; ++t) {
            const bool k1_cand_overflow = h_k1_overflow[t] != 0;
            const bool k4_prime_overflow =
                h_prime_count[t] > static_cast<std::uint32_t>(MAX_PRIMES_GPU);
            const bool k4_group_overflow =
                h_remap_overflow[t] != 0 && !k1_cand_overflow &&
                !k4_prime_overflow;
            std::uint32_t port_sum = 0;
            std::uint16_t port_counts[4] = {0, 0, 0, 0};
            for (int face = 0; face < NUM_FACES; ++face) {
              const std::uint16_t ports =
                  h_face_rep_counts[t * NUM_FACES +
                                    static_cast<std::size_t>(face)];
              port_counts[face] = ports;
              port_sum += ports;
            }
            const bool k5_port_overflow = port_sum > MAX_PORTS_PER_TILE;

            local_stats.k1_cand_overflow_count += k1_cand_overflow ? 1U : 0U;
            local_stats.k4_prime_overflow_count += k4_prime_overflow ? 1U : 0U;
            local_stats.k4_group_overflow_count += k4_group_overflow ? 1U : 0U;
            local_stats.k5_port_overflow_count += k5_port_overflow ? 1U : 0U;

            if ((k1_cand_overflow || k4_prime_overflow ||
                 k4_group_overflow || k5_port_overflow) &&
                local_stats.first_overflow_tiles.size() <
                    max_overflow_diagnostics) {
              DispatchStats::OverflowDiagnostic diag{};
              diag.coord = tiles[global_offset + t];
              diag.candidate_count = h_raw_total_cands[t];
              diag.prime_count = h_prime_count[t];
              diag.group_count = h_max_label[t];
              for (int face = 0; face < NUM_FACES; ++face) {
                diag.port_counts[face] = port_counts[face];
              }
              diag.k1_cand_overflow = k1_cand_overflow;
              diag.k4_prime_overflow = k4_prime_overflow;
              diag.k4_group_overflow = k4_group_overflow;
              diag.k5_port_overflow = k5_port_overflow;
              local_stats.first_overflow_tiles.push_back(diag);
            }

            if (k4_group_overflow && group_dump_path != nullptr &&
                !group_dump_written) {
              dump_group_overflow_tile_csv(
                  group_dump_path, tiles[global_offset + t],
                  h_raw_total_cands[t], h_prime_count[t], h_max_label[t],
                  workspace.d_prime_pos.get() + t * MAX_PRIMES_GPU,
                  workspace.d_parent.get() + t * MAX_PRIMES_GPU,
                  workspace.d_wire_label_by_raw_root.get() +
                      t * MAX_PRIMES_GPU);
              group_dump_written = true;
              if (group_dump_abort) {
                throw std::runtime_error(
                    std::string("captured K4 group overflow dump at ") +
                    group_dump_path);
              }
            }
          }
        }

        check_cuda(cudaStreamWaitEvent(streams.d2h.get(),
                                       slot.compute_done.get(), 0),
                   "cudaStreamWaitEvent(compute_done)");
        check_cuda(cudaMemcpyAsync(slot.tileops.get(), slot.d_tileops.get(),
                                   slab_tiles * sizeof(campaign::TileOp),
                                   cudaMemcpyDeviceToHost,
                                   streams.d2h.get()),
                   "cudaMemcpyAsync(dispatch tileops D2H)");
        check_cuda(cudaEventRecord(slot.d2h_done.get(), streams.d2h.get()),
                   "cudaEventRecord(d2h_done)");
        slot_pending.active = true;
        slot_pending.global_offset = global_offset;
        slot_pending.tiles = slab_tiles;

        chunk_copied += slab_tiles;
        ++slab_index;
      }

      copied += host_chunk;
    }

    for (std::size_t slot_idx = 0; slot_idx < pending.size(); ++slot_idx) {
      drain_pending_d2h(*ring[slot_idx], pending[slot_idx], output_tileops);
    }

    check_cuda(cudaStreamSynchronize(streams.h2d.get()),
               "cudaStreamSynchronize(h2d)");
    check_cuda(cudaStreamSynchronize(streams.compute.get()),
               "cudaStreamSynchronize(compute)");
    check_cuda(cudaStreamSynchronize(streams.d2h.get()),
               "cudaStreamSynchronize(d2h)");

    if (collect_overflow_summary) {
      unsigned long long h_overflow_summary[OVERFLOW_SUMMARY_COUNTERS] = {};
      check_cuda(cudaMemcpy(h_overflow_summary,
                            workspace.d_overflow_summary.get(),
                            sizeof(h_overflow_summary),
                            cudaMemcpyDeviceToHost),
                 "cudaMemcpy(overflow_summary)");
      local_stats.k1_cand_overflow_count = h_overflow_summary[0];
      local_stats.k4_prime_overflow_count = h_overflow_summary[1];
      local_stats.k4_group_overflow_count = h_overflow_summary[2];
      local_stats.k5_port_overflow_count = h_overflow_summary[3];
    }

    if (stats != nullptr) {
      *stats = local_stats;
    }
}

TileBatchDispatcher::TileBatchDispatcher(
    const campaign::CampaignConstants& constants,
    const DispatchConfig& config)
    : impl_(std::make_unique<Impl>(constants, config)) {}

TileBatchDispatcher::~TileBatchDispatcher() = default;

void TileBatchDispatcher::dispatch(const campaign::TileCoord* tiles,
                                   std::size_t count,
                                   campaign::TileOp* output_tileops,
                                   DispatchStats* stats) {
  impl_->dispatch(tiles, count, output_tileops, stats);
}

void dispatch_tile_batch(const campaign::TileCoord* tiles,
                         std::size_t count,
                         const campaign::CampaignConstants& constants,
                         campaign::TileOp* output_tileops,
                         const DispatchConfig& config,
                         DispatchStats* stats) {
  TileBatchDispatcher dispatcher(constants, config);
  dispatcher.dispatch(tiles, count, output_tileops, stats);
}

std::vector<campaign::TileOp> dispatch_tile_batch(
    const std::vector<campaign::TileCoord>& tiles,
    const campaign::CampaignConstants& constants,
    const DispatchConfig& config,
    DispatchStats* stats) {
  std::vector<campaign::TileOp> output(tiles.size());
  dispatch_tile_batch(tiles.data(), tiles.size(), constants, output.data(),
                      config, stats);
  return output;
}

}  // namespace cuda_campaign
