#pragma once

#include "campaign/tileop.h"
#include "cuda_campaign/tileop.cuh"

namespace cuda_campaign {

campaign::TileOp run_stub_passthrough(const campaign::TileOp& cpu_result);

}  // namespace cuda_campaign
