#include <cstdint>
#include <exception>
#include <iostream>
#include <vector>

#include "support/k5_parity_support.h"

int main() {
  try {
    std::vector<k5_parity::PortRecord> ports = {
        {7, -2, 11},
        {4, 0, 9},
        {4, 0, 3},
        {4, -1, 12},
    };

    ports = k5_parity::sort_ports_for_test(std::move(ports));

    const std::vector<std::uint8_t> expected_labels = {12, 3, 9, 11};
    for (std::size_t i = 0; i < expected_labels.size(); ++i) {
      if (ports[i].global_wire_label != expected_labels[i]) {
        std::cerr << "port sort collision failed at index " << i
                  << ": got label "
                  << static_cast<int>(ports[i].global_wire_label)
                  << ", expected " << static_cast<int>(expected_labels[i])
                  << "\n";
        return 1;
      }
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_port_sort_collision: " << e.what() << "\n";
    return 1;
  }
}
