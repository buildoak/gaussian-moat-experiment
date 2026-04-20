// tests/test_campaign_constants.cpp
//
// Phase 1 coverage for CampaignConstants: basic invariants, hashing, and
// the annulus-thickness gate. Full i128 boundary tests live in M3's
// test_geo_tests.cpp.

#include <cstdint>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"

namespace {

TEST(CampaignConstants, FromRadiiHappyPath) {
  auto c = campaign::CampaignConstants::from_radii(
      10000, 10032, campaign::k_sq_value);
  EXPECT_EQ(c.R_inner, 10000u);
  EXPECT_EQ(c.R_outer, 10032u);
  EXPECT_EQ(c.R_inner_sq, 10000ULL * 10000ULL);
  EXPECT_EQ(c.R_outer_sq, 10032ULL * 10032ULL);
  EXPECT_EQ(c.K_SQ_value, static_cast<std::uint32_t>(campaign::k_sq_value));
}

TEST(CampaignConstants, PrefilterUsesCeilIsqrt) {
  auto c = campaign::CampaignConstants::from_radii(
      10000, 10032, campaign::k_sq_value);
  const std::uint64_t expected_ceil =
      static_cast<std::uint64_t>(campaign::ceil_isqrt(campaign::k_sq_value));
  EXPECT_EQ(c.prefilter_inner, 2ULL * 10000 * expected_ceil + 1ULL);
  EXPECT_EQ(c.prefilter_outer, 2ULL * 10032 * expected_ceil + 1ULL);
}

TEST(CampaignConstants, CanonicalHashIsDeterministic) {
  auto c1 = campaign::CampaignConstants::from_radii(
      10000, 10032, campaign::k_sq_value);
  auto c2 = campaign::CampaignConstants::from_radii(
      10000, 10032, campaign::k_sq_value);
  EXPECT_EQ(c1.canonical_hash(), c2.canonical_hash());
  EXPECT_EQ(c1.canonical_hash().size(), 64u);  // SHA-256 hex length
}

TEST(CampaignConstants, CanonicalHashDifferentiatesParams) {
  auto a = campaign::CampaignConstants::from_radii(
      10000, 10032, campaign::k_sq_value);
  auto b = campaign::CampaignConstants::from_radii(
      10001, 10033, campaign::k_sq_value);
  EXPECT_NE(a.canonical_hash(), b.canonical_hash());
}

TEST(CampaignConstants, MrWitnessSetHashIsStable) {
  const auto h1 = campaign::CampaignConstants::mr_witness_set_sha256();
  const auto h2 = campaign::CampaignConstants::mr_witness_set_sha256();
  EXPECT_EQ(h1, h2);
  EXPECT_EQ(h1, campaign::kFj64WitnessTableSha256);
  EXPECT_EQ(h1.size(), 64u);
}

TEST(CampaignConstants, RejectsInvalidRadii) {
  EXPECT_THROW(campaign::CampaignConstants::from_radii(
                   0, 100, campaign::k_sq_value),
               std::invalid_argument);
  EXPECT_THROW(campaign::CampaignConstants::from_radii(
                   100, 50, campaign::k_sq_value),
               std::invalid_argument);
  EXPECT_THROW(campaign::CampaignConstants::from_radii(100, 200, 0),
               std::invalid_argument);
}

TEST(CampaignConstants, AnnulusThicknessGate) {
  // Tiny radii: thickness = 32, bound ≈ 256*2 + 2*6 = 524 at K=36.
  auto tiny = campaign::CampaignConstants::from_radii(
      10000, 10032, campaign::k_sq_value);
  EXPECT_FALSE(tiny.verify_annulus_thickness());

  // Wide annulus: thickness 8192, easily passes.
  auto wide = campaign::CampaignConstants::from_radii(
      80'000'000ULL, 80'008'192ULL, campaign::k_sq_value);
  EXPECT_TRUE(wide.verify_annulus_thickness());
}

TEST(CampaignConstants, FloorIsqrtIsIntegerOnly) {
  EXPECT_EQ(campaign::floor_isqrt(0), 0);
  EXPECT_EQ(campaign::floor_isqrt(1), 1);
  EXPECT_EQ(campaign::floor_isqrt(35), 5);
  EXPECT_EQ(campaign::floor_isqrt(36), 6);
  EXPECT_EQ(campaign::floor_isqrt(40), 6);
}

TEST(CampaignConstants, CeilIsqrtIsIntegerOnly) {
  EXPECT_EQ(campaign::ceil_isqrt(0), 0);
  EXPECT_EQ(campaign::ceil_isqrt(1), 1);
  EXPECT_EQ(campaign::ceil_isqrt(35), 6);
  EXPECT_EQ(campaign::ceil_isqrt(36), 6);
  EXPECT_EQ(campaign::ceil_isqrt(40), 7);
}

}  // namespace
