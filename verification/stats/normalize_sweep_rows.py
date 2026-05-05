#!/usr/bin/env python3
"""Normalize CUDA profile/audit JSON into sweep_rows.jsonl records."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


PROOF_STATUSES = {
    "CLAIM_PROOF_MISSING",
    "MOAT_PROOF_PASS",
    "SPAN_PROOF_PASS",
}

POSTFLIGHT_STATUSES = {
    "CLAIM_PROOF_MISSING",
    "MOAT_PROOF_PASS",
    "REJECT",
    "RUN_CONTRACT_PASS",
    "SPAN_PROOF_PASS",
    "TILE_SAMPLE_AUDIT_PASS",
}


def load_json(path: Path) -> dict[str, Any]:
    with path.open() as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected top-level JSON object")
    return data


def get_path(data: dict[str, Any], *path: str) -> Any:
    cur: Any = data
    for key in path:
        if not isinstance(cur, dict) or key not in cur:
            return None
        cur = cur[key]
    return cur


def first_value(*values: Any) -> Any:
    for value in values:
        if value is not None:
            return value
    return None


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


def number_or_none(value: Any) -> int | float | None:
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, (int, float)):
        return value
    if isinstance(value, str):
        try:
            return int(value)
        except ValueError:
            try:
                return float(value)
            except ValueError:
                return None
    return None


def profile_timing_seconds(
    profile: dict[str, Any], current_key: str, *legacy_values: Any
) -> int | float | None:
    return number_or_none(
        first_value(
            get_path(profile, "timings_seconds", current_key),
            *legacy_values,
        )
    )


def sum_ints(value: Any) -> int | None:
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, dict):
        total = 0
        found = False
        for child in value.values():
            child_total = sum_ints(child)
            if child_total is not None:
                total += child_total
                found = True
        return total if found else None
    if isinstance(value, list):
        total = 0
        found = False
        for child in value:
            child_total = sum_ints(child)
            if child_total is not None:
                total += child_total
                found = True
        return total if found else None
    return None


def detector_status(verdict: Any) -> str:
    if verdict == "SPANNING":
        return "ANY_SPAN_DETECTED"
    if verdict == "MOAT":
        return "ANY_SHELL_MOAT_DETECTED"
    return "UNKNOWN_DETECTOR_STATUS"


def verdict_mode(verdict: Any) -> str | None:
    if verdict == "SPANNING":
        return "ANY-SPAN"
    if verdict == "MOAT":
        return "ANY-SHELL-MOAT"
    return None


def audit_status(audit: dict[str, Any] | None) -> str | None:
    if audit is None:
        return None
    value = first_value(
        audit.get("postflight_status"),
        audit.get("status"),
        get_path(audit, "summary", "status"),
        get_path(audit, "report", "status"),
    )
    if isinstance(value, str) and value in POSTFLIGHT_STATUSES:
        return value
    return None


def proof_status(profile: dict[str, Any], audit: dict[str, Any] | None) -> str:
    explicit = first_value(
        get_path(audit or {}, "proof_status"),
        get_path(audit or {}, "claim", "proof_status"),
        get_path(audit or {}, "proof", "status"),
        profile.get("proof_status"),
    )
    if isinstance(explicit, str) and explicit in PROOF_STATUSES:
        return explicit

    status = audit_status(audit)
    if status in {"SPAN_PROOF_PASS", "MOAT_PROOF_PASS", "CLAIM_PROOF_MISSING"}:
        return status

    return "CLAIM_PROOF_MISSING"


def run_contract_status(audit: dict[str, Any] | None) -> str | None:
    if audit is None:
        return None
    explicit = first_value(
        audit.get("run_contract_status"),
        get_path(audit, "run_contract", "status"),
    )
    if isinstance(explicit, str) and explicit in POSTFLIGHT_STATUSES:
        return explicit
    status = audit_status(audit)
    if status in POSTFLIGHT_STATUSES:
        return "REJECT" if status == "REJECT" else "RUN_CONTRACT_PASS"
    return None


def tile_sample_audit_status(audit: dict[str, Any] | None) -> str | None:
    if audit is None:
        return None
    explicit = first_value(
        audit.get("tile_sample_audit_status"),
        get_path(audit, "tile_sample_audit", "status"),
        get_path(audit, "audit", "status"),
    )
    if isinstance(explicit, str) and explicit in POSTFLIGHT_STATUSES:
        return explicit
    return "TILE_SAMPLE_AUDIT_PASS" if audit_status(audit) == "TILE_SAMPLE_AUDIT_PASS" else None


def normalize_row(
    profile: dict[str, Any],
    profile_path: Path,
    audit: dict[str, Any] | None = None,
    audit_path: Path | None = None,
) -> dict[str, Any]:
    radii = profile.get("radii", {}) if isinstance(profile.get("radii"), dict) else {}
    tiles = profile.get("tiles", {}) if isinstance(profile.get("tiles"), dict) else {}
    stats_v2 = profile.get("stats_v2", {}) if isinstance(profile.get("stats_v2"), dict) else {}
    bz = profile.get("bz", {}) if isinstance(profile.get("bz"), dict) else {}
    host = (
        profile.get("host_tileop_counters", {})
        if isinstance(profile.get("host_tileop_counters"), dict)
        else {}
    )

    k_sq = int_or_none(first_value(radii.get("k_sq"), profile.get("k_sq")))
    r_inner = int_or_none(first_value(radii.get("r_inner"), profile.get("r_inner")))
    r_outer = int_or_none(first_value(radii.get("r_outer"), profile.get("r_outer")))
    width = int_or_none(first_value(radii.get("width"), profile.get("width")))
    if width is None and r_inner is not None and r_outer is not None:
        width = r_outer - r_inner

    verdict = profile.get("verdict")
    row: dict[str, Any] = {
        "sweep_row_schema_version": 1,
        "source_profile": str(profile_path),
        "source_audit": str(audit_path) if audit_path else None,
        "claim_id": first_value(profile.get("claim_id"), profile_path.stem),
        "k_sq": k_sq,
        "r_inner": r_inner,
        "r_outer": r_outer,
        "width": width,
        "region": first_value(profile.get("region"), radii.get("region")),
        "verdict": verdict,
        "verdict_mode": verdict_mode(verdict),
        "detector_status": detector_status(verdict),
        "proof_status": proof_status(profile, audit),
        "postflight_status": audit_status(audit),
        "run_contract_status": run_contract_status(audit),
        "tile_sample_audit_status": tile_sample_audit_status(audit),
        "telemetry_level": first_value(
            profile.get("telemetry_level"),
            profile.get("stats_level"),
            stats_v2.get("telemetry_level"),
        ),
        "schema_version": profile.get("schema_version"),
        "commit": first_value(
            profile.get("commit"),
            profile.get("git_commit"),
            get_path(profile, "build", "commit"),
        ),
        "build_id": first_value(
            profile.get("build_id"),
            profile.get("build_identity"),
            get_path(profile, "build", "id"),
        ),
        "cuda_arch": first_value(
            profile.get("cuda_arch"),
            get_path(profile, "build", "cuda_arch"),
            get_path(profile, "device", "cuda_arch"),
        ),
        "device": first_value(profile.get("device"), profile.get("gpu")),
        "driver": first_value(profile.get("driver"), profile.get("cuda_driver")),
        "command": profile.get("command"),
        "active_tiles": int_or_none(first_value(tiles.get("active"), profile.get("active_tiles"))),
        "produced_tiles": int_or_none(
            first_value(tiles.get("produced"), profile.get("produced_tiles"))
        ),
        "ingested_tiles": int_or_none(
            first_value(tiles.get("ingested"), profile.get("ingested_tiles"))
        ),
        "early_exit_enabled": profile.get("early_exit_enabled"),
        "early_exit_taken": profile.get("early_exit_taken"),
        "bz_checked": first_value(bz.get("checked"), profile.get("bz_checked")),
        "bz_clean": first_value(bz.get("clean"), bz.get("bz_clean"), profile.get("bz_clean")),
        "bz_override_used": first_value(
            bz.get("override_used"),
            bz.get("override"),
            profile.get("bz_override_used"),
        ),
        "bz_bad_norm_count": int_or_none(
            first_value(bz.get("bad_norm_count"), profile.get("bz_bad_norm_count"))
        ),
        "overflow_total": sum_ints(profile.get("overflow_counters")),
        "emitted_overflow_bit_count": int_or_none(host.get("emitted_overflow_bit_count")),
        "geo_i_tiles": int_or_none(stats_v2.get("geo_i_tiles")),
        "geo_o_tiles": int_or_none(stats_v2.get("geo_o_tiles")),
        "geo_i_ports": int_or_none(stats_v2.get("geo_i_ports")),
        "geo_o_ports": int_or_none(stats_v2.get("geo_o_ports")),
        "candidate_count_distribution": first_value(
            stats_v2.get("candidate_count_distribution"),
            stats_v2.get("candidate_counts"),
        ),
        "gaussian_prime_count_distribution": first_value(
            stats_v2.get("gaussian_prime_count_distribution"),
            stats_v2.get("prime_count_distribution"),
            stats_v2.get("prime_counts"),
        ),
        "group_count_distribution": first_value(
            stats_v2.get("group_count_distribution"),
            stats_v2.get("group_counts"),
        ),
        "total_port_count_distribution": first_value(
            stats_v2.get("total_port_count_distribution"),
            stats_v2.get("port_count_distribution"),
            stats_v2.get("port_counts"),
        ),
        "max_face_port_count_distribution": first_value(
            stats_v2.get("max_face_port_count_distribution"),
            stats_v2.get("max_face_port_counts"),
        ),
        "high_pressure_tiles": first_value(
            stats_v2.get("high_pressure_tiles"),
            stats_v2.get("high_pressure_top_n"),
        ),
        "component_census": stats_v2.get("component_census"),
        "sample_manifest_path": stats_v2.get("sample_manifest_path"),
        "tile_sample_path": stats_v2.get("tile_sample_path"),
        "sample_count": int_or_none(
            first_value(
                stats_v2.get("sample_count"),
                stats_v2.get("tile_sample_count"),
                stats_v2.get("tile_samples_written"),
            )
        ),
        "snapshot_path": stats_v2.get("snapshot_path"),
        "snapshot_sha256": stats_v2.get("snapshot_sha256"),
        "total_seconds": profile_timing_seconds(
            profile,
            "total",
            profile.get("total_seconds"),
            profile.get("total_time_seconds"),
            get_path(profile, "timing", "total_seconds"),
        ),
        "cuda_k1_k5_seconds": profile_timing_seconds(
            profile,
            "cuda_k1_k5",
            profile.get("cuda_k1_k5_seconds"),
            get_path(profile, "timing", "cuda_k1_k5_seconds"),
        ),
        "compositor_seconds": profile_timing_seconds(
            profile,
            "compositor",
            profile.get("compositor_seconds"),
            get_path(profile, "timing", "compositor_seconds"),
        ),
    }
    return row


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Emit sweep_rows.jsonl-compatible rows from profile/audit JSON."
    )
    parser.add_argument("--profiles", nargs="+", type=Path, required=True)
    parser.add_argument(
        "--audits",
        nargs="*",
        type=Path,
        default=[],
        help="Optional audit/postflight JSON files, one per profile.",
    )
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args()

    if args.audits and len(args.audits) != len(args.profiles):
        parser.error("--audits must be omitted or contain one file per --profiles entry")

    rows = []
    for index, profile_path in enumerate(args.profiles):
        audit_path = args.audits[index] if args.audits else None
        audit = load_json(audit_path) if audit_path else None
        rows.append(normalize_row(load_json(profile_path), profile_path, audit, audit_path))

    text = "".join(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n" for row in rows)
    if args.out:
        args.out.write_text(text)
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
