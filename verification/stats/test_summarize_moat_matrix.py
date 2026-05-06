#!/usr/bin/env python3
"""Tests for MOAT hardening matrix summarization."""

from __future__ import annotations

import unittest

from summarize_moat_matrix import MONOTONICITY_VIOLATION, format_text, summarize


def row(width: int, verdict: str, k_sq: int = 36) -> dict:
    return {
        "claim_id": f"k{k_sq}-w{width}",
        "k_sq": k_sq,
        "r_inner": 80000000,
        "r_outer": 80000000 + width,
        "width": width,
        "verdict": verdict,
        "postflight_status": "TILE_SAMPLE_AUDIT_PASS",
        "run_contract_status": "RUN_CONTRACT_PASS",
        "tile_sample_audit_status": "TILE_SAMPLE_AUDIT_PASS",
        "bz_checked": True,
        "bz_clean": True,
        "bz_override_used": False,
        "bz_bad_norm_count": 0,
        "overflow_total": 0,
        "emitted_overflow_bit_count": 0,
        "stats_v2_present": True,
        "geo_i_tiles": 11,
        "geo_o_tiles": 13,
        "active_tiles": 101,
        "produced_tiles": 101,
        "ingested_tiles": 101,
    }


class MoatMatrixSummaryTest(unittest.TestCase):
    def test_all_moat_acceptance_matrix_passes_monotonicity(self) -> None:
        summary = summarize(
            [row(17000, "MOAT"), row(18000, "MOAT"), row(19000, "MOAT"), row(20000, "MOAT")]
        )

        self.assertEqual(summary["monotonicity_status"], "PASS")
        self.assertEqual(summary["findings"], [])
        self.assertEqual([item["width"] for item in summary["rows"]], [17000, 18000, 19000, 20000])
        self.assertTrue(all(item["acceptance"] for item in summary["rows"]))

    def test_moat_followed_by_wider_spanning_flags_violation(self) -> None:
        summary = summarize(
            [
                row(17000, "SPANNING"),
                row(18000, "MOAT"),
                row(19000, "SPANNING"),
                row(20000, "MOAT"),
            ]
        )

        self.assertEqual(summary["monotonicity_status"], "FAIL")
        self.assertEqual(summary["findings"][0]["code"], MONOTONICITY_VIOLATION)
        self.assertEqual(summary["findings"][0]["narrower_width"], 18000)
        self.assertEqual(summary["findings"][0]["wider_width"], 19000)
        self.assertIn(MONOTONICITY_VIOLATION, format_text(summary))

    def test_optional_k37_to_k39_rows_are_telemetry_not_acceptance(self) -> None:
        summary = summarize([row(17000, "MOAT"), row(18000, "SPANNING", k_sq=37)])

        self.assertEqual(summary["monotonicity_status"], "PASS")
        self.assertEqual(summary["findings"], [])
        by_k = {item["k_sq"]: item for item in summary["rows"]}
        self.assertTrue(by_k[36]["acceptance"])
        self.assertFalse(by_k[37]["acceptance"])


if __name__ == "__main__":
    unittest.main()
