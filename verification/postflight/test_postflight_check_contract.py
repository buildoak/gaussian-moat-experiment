#!/usr/bin/env python3
"""Negative contract tests for the compact postflight gate."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any


class PostflightContractTests(unittest.TestCase):
    postflight_check: Path
    fixtures_dir: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.postflight_check = Path(CONTRACT_ARGS.postflight_check)
        cls.fixtures_dir = Path(CONTRACT_ARGS.fixtures_dir)

    def load_fixture(self, name: str) -> dict[str, Any]:
        with (self.fixtures_dir / name).open() as fh:
            data = json.load(fh)
        self.assertIsInstance(data, dict)
        return data

    def run_bundle(self, bundle: dict[str, Any]) -> dict[str, Any]:
        with tempfile.TemporaryDirectory() as tmp:
            bundle_path = Path(tmp) / "bundle.json"
            report_path = Path(tmp) / "report.json"
            bundle_path.write_text(json.dumps(bundle, indent=2) + "\n")
            completed = subprocess.run(
                [
                    str(self.postflight_check),
                    str(bundle_path),
                    "--report",
                    str(report_path),
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(
                completed.returncode,
                0,
                msg=f"bundle unexpectedly accepted:\n{completed.stdout}\n{completed.stderr}",
            )
            with report_path.open() as fh:
                report = json.load(fh)
        self.assertEqual(report["status"], "REJECT")
        return report

    def assert_rejects_with(self, bundle: dict[str, Any], needle: str) -> None:
        report = self.run_bundle(bundle)
        errors = "\n".join(report.get("errors", []))
        self.assertIn(needle, errors)

    def test_stats_v2_required_for_claimed_telemetry(self) -> None:
        bundle = self.load_fixture("tile-sample-audit-pass.json")
        bundle["profile"].pop("stats_v2")
        self.assert_rejects_with(bundle, "profile.stats_v2 missing")

    def test_sample_manifest_required(self) -> None:
        bundle = self.load_fixture("tile-sample-audit-pass.json")
        bundle["sample_audit"].pop("manifest")
        self.assert_rejects_with(bundle, "sample_audit.manifest missing")

    def test_sample_quotas_must_not_be_empty(self) -> None:
        bundle = self.load_fixture("tile-sample-audit-pass.json")
        bundle["sample_audit"]["quotas"] = {}
        bundle["sample_audit"]["class_counts"] = {}
        self.assert_rejects_with(bundle, "sample_audit quotas/class_counts missing or empty")

    def test_accepted_spanning_requires_cert(self) -> None:
        bundle = self.load_fixture("span-proof-pass.json")
        bundle.pop("span_certificate")
        bundle["artifacts"] = [
            item for item in bundle["artifacts"] if item.get("name") != "span_certificate"
        ]
        self.assert_rejects_with(bundle, "accepted SPANNING requires span certificate proof")

    def test_malformed_overflow_counter_rejects(self) -> None:
        bundle = self.load_fixture("run-contract-pass.json")
        bundle["overflow_counters"]["tileop"] = "0"
        self.assert_rejects_with(bundle, "overflow counter has invalid shape")

    def test_missing_host_tileop_counter_rejects(self) -> None:
        bundle = self.load_fixture("run-contract-pass.json")
        bundle["profile"].pop("host_tileop_counters")
        self.assert_rejects_with(bundle, "profile.host_tileop_counters missing")

    def test_malformed_host_tileop_counter_rejects(self) -> None:
        bundle = self.load_fixture("run-contract-pass.json")
        bundle["profile"]["host_tileop_counters"]["emitted_overflow_bit_count"] = "0"
        self.assert_rejects_with(
            bundle,
            "profile.host_tileop_counters.emitted_overflow_bit_count must be a nonnegative integer",
        )

    def test_nonzero_host_tileop_counter_rejects(self) -> None:
        bundle = self.load_fixture("run-contract-pass.json")
        bundle["profile"]["host_tileop_counters"]["emitted_overflow_bit_count"] = 1
        self.assert_rejects_with(
            bundle,
            "profile.host_tileop_counters.emitted_overflow_bit_count must be zero",
        )

    def test_bz_mismatch_rejects(self) -> None:
        bundle = self.load_fixture("tile-sample-audit-pass.json")
        bundle["bz"]["r_outer"] = 9
        self.assert_rejects_with(bundle, "bz.r_outer mismatch")

    def test_bz_override_rejects(self) -> None:
        bundle = self.load_fixture("tile-sample-audit-pass.json")
        bundle["bz"]["override_used"] = True
        self.assert_rejects_with(bundle, "bz.override_used must be false")

    def test_moat_requires_explicit_full_ingest(self) -> None:
        bundle = self.load_fixture("tile-sample-audit-pass.json")
        bundle["profile"]["early_exit"].pop("full_ingest")
        self.assert_rejects_with(bundle, "MOAT verdict requires explicit full-ingest evidence")

    def test_moat_requires_active_produced_ingested_equality(self) -> None:
        bundle = self.load_fixture("tile-sample-audit-pass.json")
        bundle["profile"]["tiles"]["ingested"] = 11
        self.assert_rejects_with(bundle, "MOAT verdict requires active == produced == ingested")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--postflight-check", required=True)
    parser.add_argument("--fixtures-dir", required=True)
    return parser.parse_args()


CONTRACT_ARGS = parse_args()


if __name__ == "__main__":
    unittest.main(argv=["test_postflight_check_contract.py"])
