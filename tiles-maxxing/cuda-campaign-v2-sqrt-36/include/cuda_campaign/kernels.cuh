#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/grid.h"
#include "campaign/tileop.h"
#include "cuda_campaign/compact_buffers.cuh"
#include "cuda_campaign/constants.cuh"
#include "cuda_campaign/face_encode_buffers.cuh"
#include "cuda_campaign/face_sort_pack.cuh"
#include "cuda_campaign/tileop.cuh"
#include "cuda_campaign/uf_buffers.cuh"

struct CUstream_st;
using cudaStream_t = CUstream_st*;

namespace cuda_campaign {

campaign::TileOp run_stub_passthrough(const campaign::TileOp& cpu_result);

struct K1K4Buffers {
  const campaign::TileCoord* d_coords = nullptr;
  std::uint32_t* d_cand_list = nullptr;
  std::uint32_t* d_total_cands = nullptr;
  std::uint32_t* d_raw_total_cands = nullptr;
  std::uint32_t* d_k1_overflow = nullptr;
  std::uint32_t* d_bitmap = nullptr;
  const std::uint16_t* d_fj64_table = nullptr;
  int k1_candidate_capacity = MAX_CANDIDATES_GPU;
  CompactBuffers compact;
  UfBuffers uf;
};

struct K1K4DebugDownload {
  std::vector<std::uint32_t> candidate_count;
  std::vector<std::uint32_t> bitmap;
  std::vector<std::uint32_t> prime_count;
  std::vector<std::uint32_t> prime_pos;
  std::vector<std::uint16_t> parent;
  std::vector<std::uint8_t> prime_geo_bits;
  std::vector<std::uint16_t> wire_label_by_raw_root;
  std::vector<std::uint16_t> max_label;
  std::vector<std::uint8_t> overflow;
  std::vector<std::uint8_t> group_flags;
};

struct K1K5Buffers {
  K1K4Buffers k1k4;
  TileOp* d_tileops = nullptr;
  FaceEncodeDebugBuffers face_debug;
};

struct K1K5DebugDownload {
  K1K4DebugDownload k1k4;
  std::vector<campaign::TileOp> tileops;
  std::vector<std::uint16_t> face_indices;
  std::vector<std::uint16_t> face_counts;
  std::vector<std::uint16_t> face_roots;
  std::vector<FaceRepresentative> face_reps;
  std::vector<std::uint16_t> face_rep_counts;
};

void upload_cuda_constants(const campaign::CampaignConstants& constants);
void upload_sieve_tables();
void upload_backward_offsets();
void upload_fj64_table(std::uint16_t** d_fj64_table);
void free_fj64_table(std::uint16_t* d_fj64_table);

void launch_k1_to_k4(const K1K4Buffers& buffers,
                     int num_tiles,
                     cudaStream_t stream = nullptr);

void launch_k1_to_k5(const K1K5Buffers& buffers,
                     int num_tiles,
                     cudaStream_t stream = nullptr);

K1K4DebugDownload run_k1_to_k4_debug(
    const std::vector<campaign::TileCoord>& coords,
    const campaign::CampaignConstants& constants,
    cudaStream_t stream = nullptr);

K1K5DebugDownload run_k1_to_k5_debug(
    const std::vector<campaign::TileCoord>& coords,
    const campaign::CampaignConstants& constants,
    int k1_candidate_capacity = MAX_CANDIDATES_GPU,
    cudaStream_t stream = nullptr);

void launch_kernel_sieve(const campaign::TileCoord* d_coords,
                         std::uint32_t* d_cand_list,
                         std::uint32_t* d_total_cands,
                         std::uint32_t* d_raw_total_cands,
                         std::uint32_t* d_k1_overflow,
                         int num_tiles,
                         int candidate_capacity = MAX_CANDIDATES_GPU,
                         cudaStream_t stream = nullptr);

void launch_kernel_mr(const campaign::TileCoord* d_coords,
                      const std::uint32_t* d_cand_list,
                      const std::uint32_t* d_total_cands,
                      std::uint32_t* d_bitmap,
                      const std::uint16_t* d_fj64_table,
                      int num_tiles,
                      cudaStream_t stream = nullptr);

std::size_t kernel_compact_shared_bytes();

void launch_kernel_compact(const std::uint32_t* d_bitmap,
                           std::uint16_t* d_row_prefix,
                           std::uint32_t* d_prime_pos,
                           std::uint32_t* d_prime_count,
                           int num_tiles,
                           cudaStream_t stream = nullptr);

void launch_kernel_compact(const std::uint32_t* d_bitmap,
                           const CompactBuffers& buffers,
                           int num_tiles,
                           cudaStream_t stream = nullptr);

void launch_kernel_uf_v2(const UfBuffers& buffers,
                         int num_tiles,
                         cudaStream_t stream = nullptr);

void launch_kernel_uf_v2(const std::uint32_t* d_bitmap,
                         const std::uint16_t* d_row_prefix,
                         const std::uint32_t* d_prime_pos,
                         const std::uint32_t* d_prime_count,
                         std::uint16_t* d_parent,
                         int num_tiles,
                         cudaStream_t stream = nullptr);

void launch_kernel_geo_flags(const campaign::TileCoord* d_coords,
                             const std::uint32_t* d_prime_pos,
                             const std::uint32_t* d_prime_count,
                             std::uint8_t* d_prime_geo_bits,
                             int num_tiles,
                             cudaStream_t stream = nullptr);

void launch_kernel_geo_norm_sweep(const std::uint64_t* d_norm_sq,
                                  std::uint8_t* d_geo_bits,
                                  std::size_t count,
                                  cudaStream_t stream = nullptr);

std::vector<std::uint8_t> debug_run_k4_geo_i128_sweep(
    const campaign::CampaignConstants& constants,
    const std::vector<std::uint64_t>& norm_sq_values);

void launch_kernel_face_encode_v2(const FaceEncodeBuffers& buffers,
                                  int num_tiles,
                                  cudaStream_t stream = nullptr);

}  // namespace cuda_campaign
