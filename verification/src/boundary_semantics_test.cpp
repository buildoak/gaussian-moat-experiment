#include "independent_moat.hpp"

#include <cassert>
#include <iostream>

using namespace moat_verify;

int main() {
  RowConfig row{36, 100, 500};

  assert(collar(34) == 5);
  assert(collar(36) == 6);
  assert(collar(38) == 6);
  assert(collar(40) == 6);

  assert(gaussian_prime_point(Point{0, 3}));
  assert(gaussian_prime_point(Point{0, 7}));
  assert(!gaussian_prime_point(Point{0, 5}));
  assert(gaussian_prime_point(Point{1, 2}));
  assert(gaussian_prime_point(Point{2, 3}));
  assert(!gaussian_prime_point(Point{3, 3}));

  assert(norm_sq(Point{6, 8}) == 100);
  assert(geo_inner(10000, row));
  assert(!geo_inner(9999, row));
  assert(geo_inner(11236, row));
  assert(!geo_inner(11237, row));
  assert(geo_outer(244036, row));
  assert(!geo_outer(244035, row));
  assert(geo_outer(250000, row));
  assert(!geo_outer(250001, row));

  TileCoord tile{0, 1, 0, 256};
  Prime axis{0, 263, 263 * 263};
  Prime inner_face{17, 260, 17ULL * 17ULL + 260ULL * 260ULL};
  Prime diag{260, 260, 260ULL * 260ULL * 2ULL};
  assert(on_face_strip(axis, tile, Face::L, 36));
  assert(!on_face_strip(axis, tile, Face::R, 36));
  assert(on_face_strip(inner_face, tile, Face::I, 36));
  assert(on_face_strip(diag, tile, Face::R, 36));

  RowConfig partial{36, 100, 110};
  TileCoord partial_tile{0, 0, 0, 0};
  const auto primes = enumerate_primes_in_box(partial_tile, partial);
  for (const Prime& p : primes) {
    assert(p.a >= 0);
    assert(p.b >= p.a);
    assert(p.norm_sq >= 10000);
    assert(p.norm_sq <= 12100);
    assert(gaussian_prime_point(Point{p.a, p.b}));
  }

  std::cout << "boundary semantics PASS\n";
  return 0;
}
