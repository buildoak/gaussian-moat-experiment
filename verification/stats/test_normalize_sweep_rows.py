#!/usr/bin/env python3
"""Focused checks for normalize_sweep_rows timing extraction."""

from __future__ import annotations

from pathlib import Path
import unittest

from normalize_sweep_rows import normalize_row


class NormalizeSweepRowsTimingTest(unittest.TestCase):
    def test_nested_timings_seconds_populate_row_seconds(self) -> None:
        profile = {
            "claim_id": "nested-timing-profile",
            "radii": {"k_sq": 36, "r_inner": 80000000, "r_outer": 80015790},
            "region": "full-octant",
            "verdict": "MOAT",
            "timings_seconds": {
                "total": 12.5,
                "cuda_k1_k5": 9.25,
                "compositor": 2.75,
            },
            "cuda_stage_timings_seconds": {
                "h2d": 0.1,
                "k1_sieve": 1.2,
                "d2h": 0.3,
            },
        }

        row = normalize_row(profile, Path("nested-timing-profile.json"))

        self.assertEqual(row["total_seconds"], 12.5)
        self.assertEqual(row["cuda_k1_k5_seconds"], 9.25)
        self.assertEqual(row["compositor_seconds"], 2.75)

    def test_legacy_flat_timing_seconds_still_populate_row_seconds(self) -> None:
        profile = {
            "claim_id": "legacy-timing-profile",
            "verdict": "SPANNING",
            "total_seconds": "4.5",
            "cuda_k1_k5_seconds": "3",
            "compositor_seconds": 1.25,
        }

        row = normalize_row(profile, Path("legacy-timing-profile.json"))

        self.assertEqual(row["total_seconds"], 4.5)
        self.assertEqual(row["cuda_k1_k5_seconds"], 3)
        self.assertEqual(row["compositor_seconds"], 1.25)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
