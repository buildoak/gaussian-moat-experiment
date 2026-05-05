#!/usr/bin/env python3
"""Parse guards for stats profile v2 schema examples."""

from __future__ import annotations

import json
from pathlib import Path
import unittest


VERIFICATION_ROOT = Path(__file__).resolve().parents[1]
SCHEMA_PATH = VERIFICATION_ROOT / "schemas" / "stats-profile-v2.schema.json"
FIXTURE_ROOT = VERIFICATION_ROOT / "fixtures" / "stats"


class StatsProfileSchemaFixturesTest(unittest.TestCase):
    def test_schema_and_fixtures_parse_as_json(self) -> None:
        json.loads(SCHEMA_PATH.read_text())
        for path in sorted(FIXTURE_ROOT.glob("*.json")):
            with self.subTest(path=path.name):
                data = json.loads(path.read_text())
                self.assertIsInstance(data, dict)

    def test_analytic_fixture_documents_reserved_stats_v2_shape(self) -> None:
        data = json.loads((FIXTURE_ROOT / "stats-profile-v2-analytic.json").read_text())
        stats_v2 = data["stats_v2"]

        for key in (
            "candidate_count_distribution",
            "gaussian_prime_count_distribution",
            "group_count_distribution",
            "total_port_count_distribution",
            "max_face_port_count_distribution",
        ):
            distribution = stats_v2[key]
            self.assertEqual(sorted(distribution.keys()), [
                "buckets",
                "observed_max",
                "observed_min",
                "sample_count",
                "total_count",
            ])
            self.assertTrue(distribution["buckets"])
            self.assertEqual(sorted(distribution["buckets"][0].keys()), ["count", "value"])

        high_pressure_tile = stats_v2["high_pressure_tiles"][0]
        self.assertIn("tile_i", high_pressure_tile)
        self.assertIn("tile_j", high_pressure_tile)
        self.assertIn("classes", high_pressure_tile)

        census = stats_v2["component_census"]
        self.assertIn("i_only_components", census)
        self.assertIn("o_only_components", census)
        self.assertIn("i_and_o_components", census)
        self.assertIn("largest_component_sizes", census)
        self.assertIn("largest_boundary_touching_components", census)

    def test_nullable_normalized_row_fixture_keeps_legacy_rows_representable(self) -> None:
        row = json.loads((FIXTURE_ROOT / "normalized-sweep-row-nullables.json").read_text())
        nullable_keys = (
            "k_sq",
            "r_inner",
            "r_outer",
            "width",
            "candidate_count_distribution",
            "high_pressure_tiles",
            "component_census",
        )
        for key in nullable_keys:
            with self.subTest(key=key):
                self.assertIsNone(row[key])


if __name__ == "__main__":
    raise SystemExit(unittest.main())
