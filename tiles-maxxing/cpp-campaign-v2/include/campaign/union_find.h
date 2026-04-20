// include/campaign/union_find.h
//
// Rank-path-compressed DSU for the cpp-campaign-v2 reference build.
//
// Deterministic tie-break: smaller-root-wins. After find(a) and find(b),
// swap so ra <= rb, then parent[rb] = ra. This rule is LOAD-BEARING for
// byte-stable snapshots — any tie-break drift reorders UF roots across
// threads and makes the golden regression fail (plan risk R2).
//
// API is generic over int-indexed elements; component labels are the
// root indices. Dense-remap happens downstream in tileop.cpp.
//
// Dependencies: <cstdint>, <vector>.

#pragma once

#include <cstdint>
#include <vector>

namespace campaign {

// Int-indexed DSU over N elements.
//
// Construction: `DSU dsu(N)` — N roots, each its own parent.
// find() does path compression; unite() uses smaller-root-wins.
//
// All operations O(α(N)) amortized.
//
// STUB in Phase 1 except for the constructor and trivial accessors; full
// impl in M3.
class DSU {
 public:
  // Create a DSU with `n` elements, each its own singleton root.
  explicit DSU(std::int32_t n);

  // Return the root of element x with path compression.
  //
  // Preconditions: 0 <= x < size().
  std::int32_t find(std::int32_t x);

  // Union elements a and b. Smaller root wins: after find() on both, the
  // larger-valued root is re-parented to the smaller-valued root.
  //
  // Returns the resulting common root.
  //
  // Preconditions: 0 <= a, b < size().
  std::int32_t unite(std::int32_t a, std::int32_t b);

  // Element count.
  std::int32_t size() const noexcept;

  // Return the set of distinct roots currently in the DSU, sorted
  // ascending. Useful for dense-remap and for tests.
  //
  // Complexity: O(N * α(N)).
  std::vector<std::int32_t> roots() const;

 private:
  mutable std::vector<std::uint16_t> parent_;
};

}  // namespace campaign
