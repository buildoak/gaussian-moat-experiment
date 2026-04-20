// tests/test_union_find.cpp

#include "campaign/union_find.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using campaign::DSU;

}  // namespace

TEST(UnionFind, ChainRootIsMinimumAcrossRandomOrders) {
  const std::array<std::int32_t, 5> base = {5, 3, 17, 2, 9};
  std::mt19937 rng(0x5eed);

  for (int trial = 0; trial < 100; ++trial) {
    std::array<std::int32_t, 5> chain = base;
    std::shuffle(chain.begin(), chain.end(), rng);

    DSU dsu(20);
    for (std::size_t i = 1; i < chain.size(); ++i) {
      dsu.unite(chain[i - 1], chain[i]);
    }

    for (const std::int32_t x : base) {
      EXPECT_EQ(dsu.find(x), 2) << "trial=" << trial << " x=" << x;
    }
  }
}

TEST(UnionFind, StarTopologyKeepsSmallerElementsSeparate) {
  DSU dsu(14);

  for (std::int32_t x = 5; x < 14; ++x) {
    dsu.unite(4, x);
  }

  for (std::int32_t x = 4; x < 14; ++x) {
    EXPECT_EQ(dsu.find(x), 4);
  }
  for (std::int32_t x = 0; x < 4; ++x) {
    EXPECT_EQ(dsu.find(x), x);
  }
}

TEST(UnionFind, DisjointSetsHaveDistinctRoots) {
  DSU dsu(6);

  dsu.unite(0, 1);
  dsu.unite(1, 2);
  dsu.unite(3, 4);
  dsu.unite(4, 5);

  EXPECT_NE(dsu.find(0), dsu.find(3));
}

TEST(UnionFind, SmallerRootWinsRegardlessOfArgumentOrder) {
  {
    DSU dsu(10);
    EXPECT_EQ(dsu.unite(9, 2), 2);
    EXPECT_EQ(dsu.find(9), 2);
  }
  {
    DSU dsu(10);
    EXPECT_EQ(dsu.unite(2, 9), 2);
    EXPECT_EQ(dsu.find(9), 2);
  }
}

TEST(UnionFind, RepeatedBuildsProduceIdenticalRoots) {
  const std::array<std::pair<std::int32_t, std::int32_t>, 16> unions = {{
      {19, 3},
      {4, 8},
      {11, 7},
      {8, 1},
      {3, 12},
      {6, 2},
      {15, 14},
      {12, 0},
      {7, 5},
      {2, 18},
      {14, 10},
      {1, 16},
      {5, 13},
      {18, 17},
      {16, 9},
      {10, 6},
  }};

  std::vector<std::int32_t> expected;
  for (int run = 0; run < 4; ++run) {
    DSU dsu(20);
    for (const auto [a, b] : unions) {
      dsu.unite(a, b);
    }

    std::vector<std::int32_t> roots;
    roots.reserve(20);
    for (std::int32_t i = 0; i < 20; ++i) {
      roots.push_back(dsu.find(i));
    }

    if (run == 0) {
      expected = roots;
    } else {
      EXPECT_EQ(roots, expected);
    }
  }
}

TEST(UnionFind, RootsReturnsDistinctRootsSortedAscending) {
  DSU dsu(7);
  dsu.unite(4, 6);
  dsu.unite(3, 5);
  dsu.unite(5, 1);

  EXPECT_EQ(dsu.roots(), (std::vector<std::int32_t>{0, 1, 2, 4}));
}

TEST(UnionFind, FindOutOfRangeAbortsInDebugBuilds) {
#ifndef NDEBUG
  DSU dsu(4);
  EXPECT_DEATH(static_cast<void>(dsu.find(4)), "");
#else
  GTEST_SKIP() << "asserts are disabled under NDEBUG";
#endif
}

TEST(UnionFind, NamedMaxDsuSizeMatchesUint16Cap) {
  // Explicit cap — kMaxDsuSize is the named constant the error message
  // cites. Guards against accidental widening (audit rec 5).
  EXPECT_EQ(campaign::kMaxDsuSize, 65536);
}

TEST(UnionFind, RejectsOversizeWithDiagnosticCitingCap) {
  try {
    DSU dsu(campaign::kMaxDsuSize);
    FAIL() << "expected std::invalid_argument at cap boundary";
  } catch (const std::invalid_argument& e) {
    const std::string msg = e.what();
    // Error must name the cap so a future parameter drift bug is
    // diagnosable without reading the source.
    EXPECT_NE(msg.find("65536"), std::string::npos)
        << "diagnostic must cite the cap 65536, got: " << msg;
    EXPECT_NE(msg.find("uint16"), std::string::npos)
        << "diagnostic must name the uint16 storage, got: " << msg;
  } catch (const std::exception& e) {
    FAIL() << "wrong exception type: " << e.what();
  }
}

TEST(UnionFind, RejectsNegativeSize) {
  EXPECT_THROW(DSU(-1), std::invalid_argument);
}
