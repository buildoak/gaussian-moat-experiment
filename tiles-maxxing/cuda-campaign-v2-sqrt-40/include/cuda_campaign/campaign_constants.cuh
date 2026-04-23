#pragma once

#include <cstdint>

#include "campaign/campaign_constants.h"
#include "cuda_campaign/constants.cuh"

namespace cuda_campaign {

struct DeviceCampaignConstants {
  std::uint64_t R_inner;
  std::uint64_t R_outer;
  std::uint64_t R_inner_sq;
  std::uint64_t R_outer_sq;
  std::uint64_t prefilter_inner;
  std::uint64_t prefilter_outer;
  std::uint64_t four_rin_sq_k_hi;
  std::uint64_t four_rin_sq_k_lo;
  std::uint64_t four_rout_sq_k_hi;
  std::uint64_t four_rout_sq_k_lo;
  std::uint32_t K_SQ_value;
  std::uint32_t S_value;
  std::uint32_t C_value;
  std::uint32_t o_x;
  std::uint32_t o_y;
};

static_assert(sizeof(DeviceCampaignConstants) == sizeof(campaign::CampaignConstants),
              "DeviceCampaignConstants must mirror CampaignConstants");

DeviceCampaignConstants make_device_constants(const campaign::CampaignConstants& constants);

extern __constant__ DeviceCampaignConstants c_campaign_constants;

}  // namespace cuda_campaign
