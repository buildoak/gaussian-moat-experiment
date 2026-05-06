#!/usr/bin/env python3
"""Summarize MOAT hardening matrix normalized rows."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ACCEPTANCE_K_SQ = 36
MONOTONICITY_VIOLATION = "MONOTONICITY_VIOLATION"


@dataclass(frozen=True)
class MatrixFinding:
    code: str
    k_sq: int | None
    r_inner: int | None
    narrower_width: int | None
    narrower_verdict: str
    wider_width: int | None
    wider_verdict: str


def load_jsonl(paths: list[Path]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for path in paths:
        with path.open() as fh:
            for line_number, line in enumerate(fh, start=1):
                if not line.strip():
                    continue
                try:
                    row = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise ValueError(f"{path}:{line_number}: invalid JSONL") from exc
                if not isinstance(row, dict):
                    raise ValueError(f"{path}:{line_number}: expected object row")
                rows.append(row)
    return rows


def int_or_none(value: Any) -> int | None:
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value)
        except ValueError:
            return None
    return None


def status_or_missing(value: Any) -> str:
    if not isinstance(value, str) or not value:
        return "MISSING"
    if value == "MOAT_PROOF_PASS":
        return "RESERVED_MOAT_PROOF_STATUS"
    return value


def bz_status(row: dict[str, Any]) -> str:
    checked = row.get("bz_checked")
    clean = row.get("bz_clean")
    override = row.get("bz_override_used")
    bad_norm_count = int_or_none(row.get("bz_bad_norm_count"))
    if checked is None and clean is None and override is None and bad_norm_count is None:
        return "MISSING"
    if checked is True and clean is True and override is False and bad_norm_count == 0:
        return "PASS"
    return "FAIL"


def overflow_status(row: dict[str, Any]) -> str:
    overflow_total = int_or_none(row.get("overflow_total"))
    emitted_bits = int_or_none(row.get("emitted_overflow_bit_count"))
    if overflow_total is None and emitted_bits is None:
        return "MISSING"
    if (overflow_total in (None, 0)) and (emitted_bits in (None, 0)):
        return "PASS"
    return "FAIL"


def stats_v2_status(row: dict[str, Any]) -> str:
    explicit = row.get("stats_v2_present")
    if explicit is True:
        return "PRESENT"
    if explicit is False:
        return "MISSING"
    stats_fields = (
        "telemetry_level",
        "geo_i_tiles",
        "geo_o_tiles",
        "geo_i_ports",
        "geo_o_ports",
        "candidate_count_distribution",
        "gaussian_prime_count_distribution",
        "group_count_distribution",
        "total_port_count_distribution",
        "max_face_port_count_distribution",
        "high_pressure_tiles",
        "component_census",
        "sample_count",
    )
    return "PRESENT" if any(row.get(field) is not None for field in stats_fields) else "MISSING"


def row_summary(row: dict[str, Any], acceptance_k_sq: int) -> dict[str, Any]:
    k_sq = int_or_none(row.get("k_sq"))
    r_inner = int_or_none(row.get("r_inner"))
    r_outer = int_or_none(row.get("r_outer"))
    width = int_or_none(row.get("width"))
    if width is None and r_inner is not None and r_outer is not None:
        width = r_outer - r_inner
    return {
        "claim_id": row.get("claim_id"),
        "k_sq": k_sq,
        "r_inner": r_inner,
        "r_outer": r_outer,
        "width": width,
        "acceptance": k_sq == acceptance_k_sq,
        "verdict": status_or_missing(row.get("verdict")),
        "postflight_status": status_or_missing(row.get("postflight_status")),
        "run_contract_status": status_or_missing(row.get("run_contract_status")),
        "tile_sample_audit_status": status_or_missing(row.get("tile_sample_audit_status")),
        "bz_status": bz_status(row),
        "overflow_status": overflow_status(row),
        "stats_v2_status": stats_v2_status(row),
        "geo_i_tiles": int_or_none(row.get("geo_i_tiles")),
        "geo_o_tiles": int_or_none(row.get("geo_o_tiles")),
        "active_tiles": int_or_none(row.get("active_tiles")),
        "produced_tiles": int_or_none(row.get("produced_tiles")),
        "ingested_tiles": int_or_none(row.get("ingested_tiles")),
    }


def monotonicity_findings(rows: list[dict[str, Any]], acceptance_k_sq: int) -> list[MatrixFinding]:
    grouped: dict[tuple[int | None, int | None], list[dict[str, Any]]] = {}
    for row in rows:
        if row.get("k_sq") != acceptance_k_sq:
            continue
        grouped.setdefault((row.get("k_sq"), row.get("r_inner")), []).append(row)

    findings: list[MatrixFinding] = []
    for (k_sq, r_inner), group in grouped.items():
        ordered = sorted(
            group,
            key=lambda row: (
                row.get("width") is None,
                row.get("width") if row.get("width") is not None else -1,
            ),
        )
        moat_before: dict[str, Any] | None = None
        for row in ordered:
            verdict = row.get("verdict")
            if verdict == "MOAT" and moat_before is None:
                moat_before = row
            elif verdict == "SPANNING" and moat_before is not None:
                findings.append(
                    MatrixFinding(
                        code=MONOTONICITY_VIOLATION,
                        k_sq=k_sq,
                        r_inner=r_inner,
                        narrower_width=moat_before.get("width"),
                        narrower_verdict="MOAT",
                        wider_width=row.get("width"),
                        wider_verdict="SPANNING",
                    )
                )
    return findings


def summarize(rows: list[dict[str, Any]], acceptance_k_sq: int = ACCEPTANCE_K_SQ) -> dict[str, Any]:
    matrix_rows = [row_summary(row, acceptance_k_sq) for row in rows]
    findings = monotonicity_findings(matrix_rows, acceptance_k_sq)
    return {
        "schema_version": 1,
        "acceptance_k_sq": acceptance_k_sq,
        "rows": sorted(
            matrix_rows,
            key=lambda row: (
                row["k_sq"] is None,
                row["k_sq"] if row["k_sq"] is not None else -1,
                row["r_inner"] is None,
                row["r_inner"] if row["r_inner"] is not None else -1,
                row["width"] is None,
                row["width"] if row["width"] is not None else -1,
            ),
        ),
        "findings": [finding.__dict__ for finding in findings],
        "monotonicity_status": "FAIL" if findings else "PASS",
    }


def format_text(summary: dict[str, Any]) -> str:
    lines = [
        "MOAT hardening matrix summary",
        f"acceptance_k_sq: {summary['acceptance_k_sq']}",
        "width k_sq role verdict postflight sample bz overflow stats_v2 geo_I geo_O active produced ingested",
    ]
    for row in summary["rows"]:
        role = "acceptance" if row["acceptance"] else "telemetry"
        lines.append(
            " ".join(
                str(value)
                for value in (
                    row["width"],
                    row["k_sq"],
                    role,
                    row["verdict"],
                    row["postflight_status"],
                    row["tile_sample_audit_status"],
                    row["bz_status"],
                    row["overflow_status"],
                    row["stats_v2_status"],
                    row["geo_i_tiles"],
                    row["geo_o_tiles"],
                    row["active_tiles"],
                    row["produced_tiles"],
                    row["ingested_tiles"],
                )
            )
        )
    if summary["findings"]:
        for finding in summary["findings"]:
            lines.append(
                f"{finding['code']}: K{finding['k_sq']} R_inner={finding['r_inner']} "
                f"width {finding['narrower_width']} {finding['narrower_verdict']} before "
                f"width {finding['wider_width']} {finding['wider_verdict']}"
            )
    else:
        lines.append("monotonicity: PASS")
    return "\n".join(lines) + "\n"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize normalized rows for the K36 MOAT hardening matrix."
    )
    parser.add_argument("--rows", nargs="+", type=Path, required=True)
    parser.add_argument("--acceptance-k-sq", type=int, default=ACCEPTANCE_K_SQ)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--format", choices=["text", "json"], default="text")
    parser.add_argument(
        "--no-fail-on-violation",
        action="store_true",
        help="Return zero even when MONOTONICITY_VIOLATION is present.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        summary = summarize(load_jsonl(args.rows), args.acceptance_k_sq)
    except ValueError as exc:
        print(f"summarize_moat_matrix: error: {exc}", file=sys.stderr)
        return 2

    text = (
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
        if args.format == "json"
        else format_text(summary)
    )
    if args.out:
        args.out.write_text(text)
    else:
        print(text, end="")

    if summary["findings"] and not args.no_fail_on_violation:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
