#include "cuda_campaign/kernels.cuh"

namespace cuda_campaign {

campaign::TileOp run_stub_passthrough(const campaign::TileOp& cpu_result) {
  return cpu_result;
}

}  // namespace cuda_campaign
