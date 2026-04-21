#pragma once

#include <cstdint>

#include "cuda_campaign/face_encode_buffers.cuh"
#include "cuda_campaign/tileop.cuh"

struct CUstream_st;
using cudaStream_t = CUstream_st*;

namespace cuda_campaign {

struct FaceSortPackInputBuffers {
  const FaceRepresentative* d_face_reps = nullptr;   // [N * 4 * FACE_REP_STRIDE]
  const std::uint16_t* d_face_rep_counts = nullptr;  // [N * 4]
};

struct FaceSortPackBuffers {
  FaceSortPackInputBuffers in;
  TileOp* d_tileops = nullptr;  // [N]
};

void launch_kernel_face_sort_pack(const FaceSortPackBuffers& buffers,
                                  int num_tiles,
                                  cudaStream_t stream = nullptr);

}  // namespace cuda_campaign
