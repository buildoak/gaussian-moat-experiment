// src/compositor.cpp
//
// STUB — Phase 2 M5 implements.

#include "campaign/compositor.h"

#include <cstdint>

namespace campaign {

struct Compositor::State {
  bool has_spanning = false;
};

Compositor::Compositor() : state_(new State{}) {}
Compositor::~Compositor() { delete state_; }

void Compositor::init(const Grid& /*grid*/) {
  // Phase 1 stub.
}

void Compositor::ingest_column(std::int32_t /*i*/,
                               const TileOp* /*column_tileops*/) {
  // Phase 1 stub.
}

bool Compositor::has_spanning() const noexcept {
  return state_ ? state_->has_spanning : false;
}

Verdict Compositor::finalize() {
  return Verdict::kUnknown;  // Phase 1 stub
}

}  // namespace campaign
