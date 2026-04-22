// tests/test_tileop.cpp

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "campaign/constants.h"
#include "campaign/tileop.h"
#include "../src/sha256.h"
#include "../src/tileop_internal.h"

namespace {

campaign::CampaignConstants test_constants() {
  return campaign::CampaignConstants::from_radii(
      10000, 11000, static_cast<std::uint32_t>(campaign::k_sq_value));
}

campaign::TileCoord origin_coord() {
  return campaign::TileCoord{0, 0, campaign::OFFSET_X, campaign::OFFSET_Y};
}

campaign::Prime make_prime(std::int64_t a, std::int64_t b) {
  return campaign::Prime{
      a,
      b,
      static_cast<std::uint64_t>(static_cast<__int128>(a) * a +
                                 static_cast<__int128>(b) * b),
      0,
  };
}

std::vector<std::uint8_t> bytes_of(const campaign::TileOp& op) {
  const auto* ptr = reinterpret_cast<const std::uint8_t*>(&op);
  return std::vector<std::uint8_t>(ptr, ptr + sizeof(op));
}

campaign::TileOp encode(
    std::vector<campaign::Prime> primes,
    std::vector<campaign::internal::PrimeGeoFlags> flags = {}) {
  if (flags.empty()) {
    flags.resize(primes.size());
  }
  return campaign::internal::build_tileop_for_primes(
      std::move(primes), std::move(flags), origin_coord(), test_constants());
}

}  // namespace

TEST(TileOp, SizeIs256Bytes) {
  static_assert(sizeof(campaign::TileOp) == 256, "TileOp must be 256 B");
  EXPECT_EQ(sizeof(campaign::TileOp), 256u);
}

TEST(TileOp, OffsetsAreLocked) {
  EXPECT_EQ(offsetof(campaign::TileOp, n), 0u);
  EXPECT_EQ(offsetof(campaign::TileOp, face_groups), 4u);
  EXPECT_EQ(offsetof(campaign::TileOp, inner_flags), 196u);
  EXPECT_EQ(offsetof(campaign::TileOp, outer_flags), 212u);
  EXPECT_EQ(offsetof(campaign::TileOp, tile_flags), 228u);
  EXPECT_EQ(offsetof(campaign::TileOp, reserved), 229u);
}

TEST(TileOp, DenseRemapCanonicalFourRootFixture) {
  const std::vector<std::int32_t> raw_roots = {3, 17, 42, 88};
  const auto remap =
      campaign::internal::dense_remap_raw_roots_for_test(raw_roots, 89);

  ASSERT_FALSE(remap.overflow);
  EXPECT_EQ(remap.max_label, 4);
  EXPECT_EQ(remap.zero_based_by_raw_root[3], 0);
  EXPECT_EQ(remap.zero_based_by_raw_root[17], 1);
  EXPECT_EQ(remap.zero_based_by_raw_root[42], 2);
  EXPECT_EQ(remap.zero_based_by_raw_root[88], 3);
}

TEST(TileOp, DenseRemapLabelsOnlyVisibleRoots) {
  const campaign::TileCoord coord = origin_coord();
  std::vector<campaign::Prime> primes;
  std::vector<campaign::internal::PrimeGeoFlags> flags;
  primes.reserve(campaign::MAX_GROUPS_PER_TILE + 1);
  flags.reserve(campaign::MAX_GROUPS_PER_TILE + 1);

  for (int y = 0; y < 9; ++y) {
    for (int x = 0; x < 15; ++x) {
      if (static_cast<int>(primes.size()) > campaign::MAX_GROUPS_PER_TILE) {
        break;
      }
      primes.push_back(make_prime(coord.a_lo + 20 + 12 * x,
                                  coord.b_lo + 20 + 12 * y));
      flags.push_back(campaign::internal::PrimeGeoFlags{});
    }
  }

  const campaign::TileOp op = campaign::internal::build_tileop_for_primes(
      std::move(primes), std::move(flags), coord, test_constants());

  EXPECT_EQ(op.tile_flags, 0);
  EXPECT_EQ(op.n[0], 0);
  EXPECT_EQ(op.n[1], 0);
  EXPECT_EQ(op.n[2], 0);
  EXPECT_EQ(op.n[3], 0);
  for (std::uint8_t byte : op.inner_flags) {
    EXPECT_EQ(byte, 0);
  }
  for (std::uint8_t byte : op.outer_flags) {
    EXPECT_EQ(byte, 0);
  }
}

TEST(TileOp, DenseRemapStillOverflowsVisibleRoots) {
  const campaign::TileCoord coord = origin_coord();
  std::vector<campaign::Prime> primes;
  primes.reserve(campaign::MAX_GROUPS_PER_TILE + 1);

  for (int i = 0; i < campaign::MAX_GROUPS_PER_TILE + 1; ++i) {
    primes.push_back(make_prime(coord.a_lo + 10 * i, coord.b_lo));
  }

  const campaign::TileOp op = encode(std::move(primes));
  EXPECT_EQ(op.tile_flags, campaign::OVERFLOW_BIT);
}

TEST(TileOp, FaceStripUfHandCaseOnFaceI) {
  std::vector<campaign::Prime> primes = {
      make_prime(1, 1),    make_prime(4, 1),    make_prime(20, 1),
      make_prime(23, 1),   make_prime(40, 1),   make_prime(43, 1),
      make_prime(46, 1),   make_prime(100, 50), make_prime(150, 80),
      make_prime(200, 90),
  };

  const campaign::TileOp op = encode(primes);

  ASSERT_EQ(op.n[static_cast<int>(campaign::Face::I)], 3);
  EXPECT_EQ(op.face_groups[0], 1);
  EXPECT_EQ(op.face_groups[1], 2);
  EXPECT_EQ(op.face_groups[2], 3);
}

TEST(TileOp, PortSortTiedInputDeterminism) {
  std::vector<campaign::Prime> primes = {
      make_prime(11, 1), make_prime(11, 4), make_prime(11, 7),
      make_prime(20, 1), make_prime(20, 5), make_prime(30, 1),
  };
  const std::vector<campaign::internal::PrimeGeoFlags> flags(primes.size());

  const auto expected = bytes_of(encode(primes, flags));
  std::mt19937 rng(0x715eed);
  for (int trial = 0; trial < 3; ++trial) {
    std::shuffle(primes.begin(), primes.end(), rng);
    EXPECT_EQ(bytes_of(encode(primes, flags)), expected);
  }
}

TEST(TileOp, ByteLevelEncodingAndSha256) {
  std::vector<campaign::Prime> primes = {
      make_prime(1, 1), make_prime(4, 1), make_prime(20, 1),
      make_prime(1, 257), make_prime(257, 257),
  };
  std::vector<campaign::internal::PrimeGeoFlags> flags = {
      {true, false}, {false, false}, {false, true}, {true, false},
      {false, true},
  };

  const campaign::TileOp op = encode(primes, flags);
  const auto bytes = bytes_of(op);

  EXPECT_EQ(bytes[0], 2);
  EXPECT_EQ(bytes[1], 2);
  EXPECT_EQ(bytes[2], 2);
  EXPECT_EQ(bytes[3], 1);
  EXPECT_EQ(bytes[4], 1);
  EXPECT_EQ(bytes[5], 3);
  EXPECT_EQ(bytes[6], 2);
  EXPECT_EQ(bytes[7], 4);
  EXPECT_EQ(bytes[8], 1);
  EXPECT_EQ(bytes[9], 2);
  EXPECT_EQ(bytes[10], 4);
  EXPECT_EQ(bytes[196], 0x03);
  EXPECT_EQ(bytes[212], 0x0c);
  EXPECT_EQ(bytes[228], 0x00);
  for (std::size_t i = 229; i < bytes.size(); ++i) {
    EXPECT_EQ(bytes[i], 0x00) << "reserved byte " << i;
  }

  constexpr const char* kExpectedSha256 =
      "70407d78b29749a92d9d64751791ffcd8f1d0fa262fe12dc6eba06d4b93ef5ee";
  EXPECT_EQ(campaign::detail::sha256_hex(bytes.data(), bytes.size()),
            kExpectedSha256);
}

TEST(TileOp, OverflowPortCountSetsOnlyOverflowBitAndZerosPayload) {
  std::vector<campaign::Prime> primes;
  for (std::int64_t i = 0; i < 193; ++i) {
    const std::int64_t h = 1 + 10 * i;
    primes.push_back(make_prime(h, 1));
    primes.push_back(make_prime(h, 7));
    if (i + 1 < 193) {
      primes.push_back(make_prime(h + 5, 10));
    }
  }

  const campaign::TileOp op = encode(primes);
  const auto bytes = bytes_of(op);

  EXPECT_EQ(op.tile_flags, campaign::OVERFLOW_BIT);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == offsetof(campaign::TileOp, tile_flags)) continue;
    EXPECT_EQ(bytes[i], 0x00) << "byte " << i;
  }
}
