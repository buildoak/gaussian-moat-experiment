#pragma once

#include <cstddef>
#include <cstdint>

#include "campaign/campaign_constants.h"
#include "campaign/grid.h"
#include "campaign/tileop.h"
#include "cuda_campaign/compact_buffers.cuh"
#include "cuda_campaign/constants.cuh"
#include "cuda_campaign/tileop.cuh"
#include "cuda_campaign/uf_buffers.cuh"

struct CUstream_st;
using cudaStream_t = CUstream_st*;

namespace cuda_campaign {

campaign::TileOp run_stub_passthrough(const campaign::TileOp& cpu_result);

void upload_cuda_constants(const campaign::CampaignConstants& constants);
void upload_sieve_tables();
void upload_backward_offsets();
void upload_fj64_table(std::uint16_t** d_fj64_table);
void free_fj64_table(std::uint16_t* d_fj64_table);

void launch_kernel_sieve(const campaign::TileCoord* d_coords,
                         std::uint32_t* d_cand_list,
                         std::uint32_t* d_total_cands,
                         int num_tiles,
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

}  // namespace cuda_campaign
