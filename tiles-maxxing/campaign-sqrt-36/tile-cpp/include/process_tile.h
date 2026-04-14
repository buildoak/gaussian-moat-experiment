#pragma once
#include "types.h"
#include "sieve.h"

TileResult process_tile(const TileCoord& coord, const SieveTables& tables,
                        PhaseTimings* timings = nullptr);
