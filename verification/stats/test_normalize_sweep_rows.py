#!/usr/bin/env python3
"""Focused checks for normalize_sweep_rows telemetry extraction."""

from __future__ import annotations

import json
from pathlib import Path
import unittest

from normalize_sweep_rows import normalize_row


FIXTURE_DIR = Path(__file__).resolve().parents[1] / "fixtures" / "stats"

DISTRIBUTION_KEYS = (
    "candidate_count_distribution",
    "gaussian_prime_count_distribution",
    "group_count_distribution",
    "total_port_count_distribution",
    "max_face_port_count_distribution",
)

HIGH_PRESSURE_INTEGER_KEYS = (
    "tile_i",
    "tile_j",
    "a_lo",
    "b_lo",
    "candidate_count",
    "gaussian_prime_count",
    "group_count",
    "total_port_count",
    "max_face_port_count",
)

COMPONENT_CENSUS_INTEGER_KEYS = (
    "i_only_components",
    "o_only_components",
    "i_and_o_components",
)


def load_fixture(name: str) -> dict:
    path = FIXTURE_DIR / name
    with path.open() as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise AssertionError(f"{name}: expected top-level JSON object")
    return data


def is_non_negative_integer(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def analytical_stats_errors(profile: dict) -> list[str]:
    stats = profile.get("stats_v2")
    if not isinstance(stats, dict):
        return ["stats_v2 must be object"]

    errors: list[str] = []
    for key in DISTRIBUTION_KEYS:
        distribution = stats.get(key)
        if not isinstance(distribution, dict):
            errors.append(f"{key} must be object")
            continue
        for bucket, count in distribution.items():
            if not isinstance(bucket, str):
                errors.append(f"{key} bucket keys must be strings")
            if not is_non_negative_integer(count):
                errors.append(f"{key}.{bucket} must be non-negative integer")

    high_pressure = stats.get("high_pressure_tiles")
    if not isinstance(high_pressure, list):
        errors.append("high_pressure_tiles must be array")
    else:
        for index, entry in enumerate(high_pressure):
            if not isinstance(entry, dict):
                errors.append(f"high_pressure_tiles[{index}] must be object")
                continue
            for key in HIGH_PRESSURE_INTEGER_KEYS:
                if key not in entry:
                    errors.append(f"high_pressure_tiles[{index}].{key} missing")
                elif not is_non_negative_integer(entry[key]):
                    errors.append(
                        f"high_pressure_tiles[{index}].{key} must be non-negative integer"
                    )

    census = stats.get("component_census")
    if not isinstance(census, dict):
        errors.append("component_census must be object")
    else:
        for key in COMPONENT_CENSUS_INTEGER_KEYS:
            if not is_non_negative_integer(census.get(key)):
                errors.append(f"component_census.{key} must be non-negative integer")
        for key in ("largest_component_sizes", "largest_boundary_touching_components"):
            if not isinstance(census.get(key), list):
                errors.append(f"component_census.{key} must be array")

    return errors


class NormalizeSweepRowsTimingTest(unittest.TestCase):
    def test_stats_v2_nested_analytical_payloads_are_preserved(self) -> None:
        component_census = {
            "i_only_components": 7,
            "o_only_components": 5,
            "i_and_o_components": 0,
            "largest_component_sizes": [
                {"component": 11, "ports": 144, "tiles": 39},
                {"component": 12, "ports": 88, "tiles": 21},
            ],
            "largest_boundary_touching_components": [
                {"component": 11, "boundary": "I", "ports": 144}
            ],
        }
        high_pressure = [
            {
                "tile": {"i": 42, "j": 17},
                "candidate_count": 6144,
                "gaussian_prime_count": 913,
                "group_count": 16,
                "port_counts": [31, 29, 12, 8],
            }
        ]
        profile = {
            "claim_id": "analytical-profile",
            "verdict": "MOAT",
            "stats_v2": {
                "telemetry_level": "profile",
                "geo_i_tiles": 3,
                "geo_o_tiles": 4,
                "geo_i_ports": 12,
                "geo_o_ports": 15,
                "distributions": {
                    "candidate_count": {
                        "bucket_width": 128,
                        "histogram": {"0": 2, "128": 5, "256": 1},
                    },
                    "gaussian_prime_count": {"0": 1, "64": 8},
                    "group_count": [{"value": 1, "tiles": 9}],
                    "total_port_count": {"0": 4, "16": 2},
                    "max_face_port_count": {"0": 4, "8": 2},
                },
                "top_n": {"high_pressure": high_pressure},
                "components": {"census": component_census},
                "tile_samples_written": "6",
            },
        }

        row = normalize_row(profile, Path("analytical-profile.json"))

        self.assertEqual(row["geo_i_tiles"], 3)
        self.assertEqual(row["geo_o_ports"], 15)
        self.assertEqual(
            row["candidate_count_distribution"],
            {"bucket_width": 128, "histogram": {"0": 2, "128": 5, "256": 1}},
        )
        self.assertEqual(row["gaussian_prime_count_distribution"], {"0": 1, "64": 8})
        self.assertEqual(row["group_count_distribution"], [{"value": 1, "tiles": 9}])
        self.assertEqual(row["total_port_count_distribution"], {"0": 4, "16": 2})
        self.assertEqual(row["max_face_port_count_distribution"], {"0": 4, "8": 2})
        self.assertEqual(row["high_pressure_tiles"], high_pressure)
        self.assertEqual(row["component_census"], component_census)
        self.assertEqual(row["sample_count"], 6)

    def test_stats_v2_flat_emitted_payloads_still_populate_row(self) -> None:
        profile = {
            "claim_id": "flat-analytical-profile",
            "verdict": "SPANNING",
            "stats_v2": {
                "stats_level": "profile",
                "geo_i_tiles": 1,
                "geo_o_tiles": 2,
                "candidate_count_distribution": {"0": 1, "128": 2},
                "gaussian_prime_count_distribution": {"0": 3},
                "group_count_distribution": {"1": 4},
                "total_port_count_distribution": {"2": 5},
                "max_face_port_count_distribution": {"3": 6},
                "high_pressure_tiles": [{"tile": {"i": 1, "j": 2}}],
                "component_census": {"i_only_components": 1},
                "tile_samples_written": 2,
            },
        }

        row = normalize_row(profile, Path("flat-analytical-profile.json"))

        self.assertEqual(row["candidate_count_distribution"], {"0": 1, "128": 2})
        self.assertEqual(row["gaussian_prime_count_distribution"], {"0": 3})
        self.assertEqual(row["group_count_distribution"], {"1": 4})
        self.assertEqual(row["total_port_count_distribution"], {"2": 5})
        self.assertEqual(row["max_face_port_count_distribution"], {"3": 6})
        self.assertEqual(row["high_pressure_tiles"], [{"tile": {"i": 1, "j": 2}}])
        self.assertEqual(row["component_census"], {"i_only_components": 1})
        self.assertEqual(row["sample_count"], 2)

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


class NormalizeSweepRowsAnalyticalTelemetryFixtureTest(unittest.TestCase):
    def test_stats_fixtures_parse_as_json_objects(self) -> None:
        for name in (
            "valid-analytical-profile.json",
            "bad-malformed-distribution.json",
            "bad-high-pressure-entry.json",
            "bad-component-census.json",
        ):
            with self.subTest(name=name):
                self.assertIsInstance(load_fixture(name), dict)

    def test_valid_analytical_profile_normalizes_stats_and_postflight_fields(self) -> None:
        profile_path = FIXTURE_DIR / "valid-analytical-profile.json"
        profile = load_fixture("valid-analytical-profile.json")
        audit = {
            "schema_version": 1,
            "status": "TILE_SAMPLE_AUDIT_PASS",
            "run_contract_status": "RUN_CONTRACT_PASS",
            "tile_sample_audit_status": "TILE_SAMPLE_AUDIT_PASS",
        }

        self.assertEqual(analytical_stats_errors(profile), [])
        row = normalize_row(profile, profile_path, audit, Path("postflight.report.json"))

        self.assertEqual(row["detector_status"], "ANY_SPAN_DETECTED")
        self.assertEqual(row["proof_status"], "CLAIM_PROOF_MISSING")
        self.assertEqual(row["postflight_status"], "TILE_SAMPLE_AUDIT_PASS")
        self.assertEqual(row["run_contract_status"], "RUN_CONTRACT_PASS")
        self.assertEqual(row["tile_sample_audit_status"], "TILE_SAMPLE_AUDIT_PASS")
        self.assertEqual(row["telemetry_level"], "profile")
        self.assertEqual(row["candidate_count_distribution"]["4"], 7200)
        self.assertEqual(row["gaussian_prime_count_distribution"]["3"], 6200)
        self.assertEqual(row["max_face_port_count_distribution"]["2"], 6700)
        self.assertEqual(row["high_pressure_tiles"][0]["tile_i"], 9152)
        self.assertEqual(row["component_census"]["i_and_o_components"], 1)
        self.assertEqual(row["sample_count"], 1024)
        self.assertEqual(row["total_seconds"], 14.25)
        self.assertEqual(row["cuda_k1_k5_seconds"], 11.5)
        self.assertEqual(row["compositor_seconds"], 2.25)

    def test_hostile_stats_fixtures_fail_focused_shape_checks(self) -> None:
        cases = {
            "bad-malformed-distribution.json": "candidate_count_distribution must be object",
            "bad-high-pressure-entry.json": "high_pressure_tiles[0].tile_j missing",
            "bad-component-census.json": (
                "component_census.i_only_components must be non-negative integer"
            ),
        }
        for name, expected_error in cases.items():
            with self.subTest(name=name):
                errors = analytical_stats_errors(load_fixture(name))
                self.assertIn(expected_error, errors)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
