#include "independent_moat.hpp"

#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

using namespace moat_verify;

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: span_cert_check CERT.json\n";
    return 2;
  }
  try {
    std::ifstream in(argv[1]);
    nlohmann::json cert;
    in >> cert;
    RowConfig row{
        cert.at("k_sq").get<std::uint32_t>(),
        cert.at("r_inner").get<std::uint64_t>(),
        cert.at("r_outer").get<std::uint64_t>()};
    const auto points_json = cert.at("path");
    if (!points_json.is_array() || points_json.size() < 2) {
      throw std::runtime_error("path must contain at least two points");
    }
    std::vector<Point> path;
    for (const auto& item : points_json) {
      path.push_back(Point{item.at("a").get<std::int64_t>(),
                           item.at("b").get<std::int64_t>()});
    }
    for (std::size_t i = 0; i < path.size(); ++i) {
      const Point p = path[i];
      if (p.a < 0 || p.b < p.a) {
        throw std::runtime_error("point outside full-octant ordering at index " +
                                 std::to_string(i));
      }
      const std::uint64_t n = norm_sq(p);
      if (n < row.r_inner * row.r_inner || n > row.r_outer * row.r_outer) {
        throw std::runtime_error("point outside annulus at index " +
                                 std::to_string(i));
      }
      if (!gaussian_prime_point(p)) {
        throw std::runtime_error("point is not a Gaussian prime at index " +
                                 std::to_string(i));
      }
      if (i != 0 && !within_k(path[i - 1], p, row.k_sq)) {
        throw std::runtime_error("step exceeds K at index " + std::to_string(i));
      }
    }
    if (!geo_inner(norm_sq(path.front()), row)) {
      throw std::runtime_error("first point is not in geo_I");
    }
    if (!geo_outer(norm_sq(path.back()), row)) {
      throw std::runtime_error("last point is not in geo_O");
    }
    std::cout << "span certificate PASS: points=" << path.size() << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
