#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"
#include "campaign/primality.h"
#include "campaign/sieve.h"

namespace {

constexpr std::uint64_t kRinner = 10000;
constexpr std::uint64_t kRouter = 10032;

struct PrimeRef {
  std::int64_t a;
  std::int64_t b;
  std::uint64_t norm_sq;
  std::uint32_t packed_pos;

  bool operator==(const PrimeRef&) const = default;
};

campaign::CampaignConstants constants_for(std::uint64_t r_inner,
                                          std::uint64_t r_outer) {
  return campaign::CampaignConstants::from_radii(r_inner, r_outer,
                                                 campaign::k_sq_value);
}

std::vector<PrimeRef> refs_from_primes(const std::vector<campaign::Prime>& ps) {
  std::vector<PrimeRef> out;
  out.reserve(ps.size());
  for (const auto& p : ps) {
    out.push_back(PrimeRef{p.a, p.b, p.norm_sq, p.packed_pos});
  }
  return out;
}

bool has_point(const std::vector<campaign::Prime>& ps, std::int64_t a,
               std::int64_t b) {
  return std::any_of(ps.begin(), ps.end(), [a, b](const campaign::Prime& p) {
    return p.a == a && p.b == b;
  });
}

std::uint32_t expected_packed_pos(const campaign::TileCoord& coord,
                                  const campaign::CampaignConstants& constants,
                                  std::int64_t a,
                                  std::int64_t b) {
  const std::int64_t col = a - (coord.a_lo - constants.C_value);
  const std::int64_t row = b - (coord.b_lo - constants.C_value);
  const std::int64_t side = constants.S_value + 1 + 2 * constants.C_value;
  return static_cast<std::uint32_t>(row * side + col);
}

TEST(Sieve, TinyRadiusMatchesCommittedReference) {
  const campaign::TileCoord coord{20, 32, 5120, 8192};
  const auto constants = constants_for(kRinner, kRouter);

  static constexpr std::array<PrimeRef, 33> kExpected = {{
      PrimeRef{5354, 8451, 100084717ULL, 71525U},
      PrimeRef{5355, 8452, 100112329ULL, 71795U},
      PrimeRef{5356, 8449, 100072337ULL, 70989U},
      PrimeRef{5356, 8451, 100106137ULL, 71527U},
      PrimeRef{5357, 8450, 100099949ULL, 71259U},
      PrimeRef{5361, 8450, 100142821ULL, 71263U},
      PrimeRef{5363, 8442, 100029133ULL, 69113U},
      PrimeRef{5363, 8450, 100164269ULL, 71265U},
      PrimeRef{5364, 8441, 100022977ULL, 68845U},
      PrimeRef{5367, 8438, 100004533ULL, 68041U},
      PrimeRef{5368, 8443, 100099673ULL, 69387U},
      PrimeRef{5369, 8440, 100059761ULL, 68581U},
      PrimeRef{5369, 8446, 100161077ULL, 70195U},
      PrimeRef{5371, 8444, 100148777ULL, 69659U},
      PrimeRef{5372, 8445, 100176409ULL, 69929U},
      PrimeRef{5372, 8447, 100210193ULL, 70467U},
      PrimeRef{5373, 8438, 100068973ULL, 68047U},
      PrimeRef{5373, 8450, 100271629ULL, 71275U},
      PrimeRef{5373, 8452, 100305433ULL, 71813U},
      PrimeRef{5374, 8435, 100029101ULL, 67241U},
      PrimeRef{5374, 8439, 100096597ULL, 68317U},
      PrimeRef{5377, 8442, 100179493ULL, 69127U},
      PrimeRef{5378, 8437, 100105853ULL, 67783U},
      PrimeRef{5378, 8445, 100240909ULL, 69935U},
      PrimeRef{5378, 8453, 100376093ULL, 72087U},
      PrimeRef{5380, 8431, 100026161ULL, 66171U},
      PrimeRef{5380, 8437, 100127369ULL, 67785U},
      PrimeRef{5380, 8441, 100194881ULL, 68861U},
      PrimeRef{5380, 8443, 100228649ULL, 69399U},
      PrimeRef{5380, 8449, 100330001ULL, 71013U},
      PrimeRef{5381, 8436, 100121257ULL, 67517U},
      PrimeRef{5381, 8444, 100256297ULL, 69669U},
      PrimeRef{5381, 8446, 100290077ULL, 70207U},
  }};

  const auto got = refs_from_primes(campaign::sieve_tile(coord, constants));
  ASSERT_EQ(got.size(), kExpected.size());
  for (std::size_t i = 0; i < kExpected.size(); ++i) {
    EXPECT_EQ(got[i].a, kExpected[i].a) << i;
    EXPECT_EQ(got[i].b, kExpected[i].b) << i;
    EXPECT_EQ(got[i].norm_sq, kExpected[i].norm_sq) << i;
    EXPECT_EQ(got[i].packed_pos,
              expected_packed_pos(coord, constants, kExpected[i].a,
                                  kExpected[i].b))
        << i;
  }
}

TEST(Sieve, AxisPrimesUseInertResidueClass) {
  static_assert(campaign::OFFSET_X == 0);
  static_assert(campaign::OFFSET_Y == 0);
  const campaign::TileCoord coord{0, 0, 0, 0};
  const auto constants = constants_for(10, 263);
  const auto got = campaign::sieve_tile(coord, constants);

  ASSERT_TRUE(has_point(got, 0, 11));
  ASSERT_TRUE(has_point(got, 0, 19));
  ASSERT_TRUE(has_point(got, 0, 251));
  EXPECT_FALSE(has_point(got, 0, 13));
  EXPECT_FALSE(has_point(got, 0, 17));

  for (const auto& p : got) {
    if (p.a == 0) {
      EXPECT_EQ(static_cast<std::uint64_t>(p.b) & 3ULL, 3ULL);
      EXPECT_TRUE(campaign::is_prime(static_cast<std::uint64_t>(p.b)));
    }
  }
}

TEST(Sieve, AxisPrimeIsZeroOffsetProperColumnMaterial) {
  static_assert(campaign::OFFSET_X == 0);
  const campaign::TileCoord coord{0, 0, 0, 0};
  const auto constants = constants_for(10, 263);
  const auto got = campaign::sieve_tile(coord, constants);
  const auto it = std::find_if(got.begin(), got.end(), [](const auto& p) {
    return p.a == 0 && p.b == 11;
  });
  ASSERT_NE(it, got.end());

  const std::uint32_t halo_side =
      static_cast<std::uint32_t>(campaign::S + 1 + 2 * campaign::C);
  EXPECT_EQ(it->packed_pos % halo_side, static_cast<std::uint32_t>(campaign::C));
}

TEST(Sieve, DeterministicAcrossRepetitions) {
  const campaign::TileCoord coord{20, 32, 5120, 8192};
  const auto constants = constants_for(kRinner, kRouter);
  const auto first = refs_from_primes(campaign::sieve_tile(coord, constants));
  EXPECT_EQ(refs_from_primes(campaign::sieve_tile(coord, constants)), first);
  EXPECT_EQ(refs_from_primes(campaign::sieve_tile(coord, constants)), first);
}

TEST(Sieve, ClipsToCanonicalOctantBoundary) {
  const campaign::TileCoord coord{20, 20, 5120, 5120};
  const auto constants = constants_for(7000, 7300);
  const auto got = campaign::sieve_tile(coord, constants);

  ASSERT_FALSE(got.empty());
  for (const auto& p : got) {
    EXPECT_GE(p.a, 0);
    EXPECT_GE(p.b, p.a);
  }
}

TEST(Sieve, EmptyHaloOutsideAnnulus) {
  const campaign::TileCoord coord{0, 0, 0, 0};
  const auto constants = constants_for(kRinner, kRouter);
  EXPECT_TRUE(campaign::sieve_tile(coord, constants).empty());
}

TEST(SieveHelpers, GaussianPrimeNormClassification) {
  EXPECT_TRUE(campaign::is_prime_u64(1000003ULL));
  EXPECT_TRUE(campaign::is_gaussian_prime_norm(2));
  EXPECT_TRUE(campaign::is_gaussian_prime_norm(13));
  EXPECT_TRUE(campaign::is_gaussian_prime_norm(9));
  EXPECT_TRUE(campaign::is_gaussian_prime_norm(49));
  EXPECT_FALSE(campaign::is_gaussian_prime_norm(21));
  EXPECT_FALSE(campaign::is_gaussian_prime_norm(25));
}

}  // namespace
