#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# ///

"""Validate CUDA campaign run artifacts as citeable local-annulus evidence."""

from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
BZ_CHECK = ROOT / "tiles-maxxing" / "cpp-campaign-v2" / "scripts" / "bz_check.py"
REQUIRED_COLUMNS = {
    "label",
    "R_inner",
    "width",
    "R_outer",
    "rc",
    "verdict",
    "k1_over",
    "k4_prime_over",
    "k4_group_over",
    "k5_port_over",
    "emitted_over",
    "produced",
    "ingested",
    "profile",
    "log",
    "bz_log",
}
ZERO_INDEX_COUNTERS = (
    "k1_over",
    "k4_prime_over",
    "k4_group_over",
    "k5_port_over",
    "emitted_over",
)
ZERO_PROFILE_COUNTERS = (
    "k1_cand_overflow_count",
    "k4_prime_overflow_count",
    "k4_group_overflow_count",
    "k5_port_overflow_count",
)
STDOUT_INT_KEYS = (
    "K_SQ",
    "active tiles",
    "produced tiles",
    "ingested tiles",
    "ingested columns",
    "chunk-size",
    "app batches",
    "chunk_overshoot_tiles",
    "total_chunk_overshoot_tiles",
    "k1_cand_overflow_count",
    "k4_prime_overflow_count",
    "k4_group_overflow_count",
    "k5_port_overflow_count",
    "emitted_overflow_bit_count",
)


@dataclass
class Finding:
    label: str
    message: str


@dataclass(frozen=True)
class ArtifactPaths:
    profile: Path
    stdout: Path
    bz: Path


@dataclass(frozen=True)
class StdoutSummary:
    ints: dict[str, int]
    r_inner: int
    r_outer: int
    snapshot: str
    early_exit_enabled: bool
    early_exit_taken: bool
    verdict: str
    constants_hash: str | None
    mr_witness_sha256: str | None
    spanning_detected: bool | None


@dataclass(frozen=True)
class BzSummary:
    k_sq: int
    r_inner: int
    r_outer: int
    verdict: str
    failures: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate a campaign result directory containing run-index.tsv, "
            "profiles, stdout logs, and BZ logs."
        )
    )
    parser.add_argument("run_dir", type=Path)
    parser.add_argument(
        "--expected-k",
        type=int,
        default=None,
        help="reject rows whose K does not match this value",
    )
    parser.add_argument(
        "--allow-spanning-without-path",
        action="store_true",
        help="downgrade SPANNING rows without stitch paths to acceptable evidence",
    )
    parser.add_argument(
        "--allow-early-spanning",
        action="store_true",
        help="allow early-exit SPANNING rows; MOAT rows always require full ingest",
    )
    parser.add_argument(
        "--no-recompute-bz",
        action="store_true",
        help="only inspect stored BZ logs; do not rerun bz_check.py",
    )
    parser.add_argument(
        "--require-profile-bz",
        action="store_true",
        help="require profile-embedded exact BZ status to be clean",
    )
    return parser.parse_args()


def fail(findings: list[Finding], label: str, message: str) -> None:
    findings.append(Finding(label, message))


def is_int(value: Any) -> bool:
    return type(value) is int


def is_bool(value: Any) -> bool:
    return type(value) is bool


def strict_int(text: str, *, positive: bool = False) -> int:
    if not re.fullmatch(r"[0-9]+", text):
        raise ValueError(f"not an unsigned integer: {text!r}")
    value = int(text)
    if positive and value <= 0:
        raise ValueError(f"not positive: {text!r}")
    return value


def require_index_int(
    findings: list[Finding],
    label: str,
    row: dict[str, str],
    key: str,
    *,
    positive: bool = False,
) -> int | None:
    value = row.get(key)
    if value is None or value == "":
        fail(findings, label, f"run-index missing {key}")
        return None
    try:
        return strict_int(value, positive=positive)
    except ValueError as exc:
        fail(findings, label, f"run-index {key} invalid: {exc}")
        return None


def expect_equal(
    findings: list[Finding],
    label: str,
    what: str,
    actual: Any,
    expected: Any,
) -> None:
    if actual != expected:
        fail(findings, label, f"{what} mismatch: got {actual!r}, expected {expected!r}")


def expect_zero(
    findings: list[Finding], label: str, what: str, value: Any
) -> None:
    if value != 0:
        fail(findings, label, f"{what} must be zero, got {value!r}")


def require_json_int(
    findings: list[Finding],
    label: str,
    obj: dict[str, Any],
    key: str,
    *,
    positive: bool = False,
) -> int | None:
    value = obj.get(key)
    if not is_int(value):
        fail(findings, label, f"profile {key} is not an integer: {value!r}")
        return None
    if positive and value <= 0:
        fail(findings, label, f"profile {key} is not positive: {value!r}")
        return None
    return value


def require_json_bool(
    findings: list[Finding], label: str, obj: dict[str, Any], key: str
) -> bool | None:
    value = obj.get(key)
    if not is_bool(value):
        fail(findings, label, f"profile {key} is not a JSON boolean: {value!r}")
        return None
    return value


def require_under_run_dir(run_dir: Path, path: Path, label: str, kind: str) -> Path:
    resolved = path.resolve(strict=True)
    try:
        resolved.relative_to(run_dir)
    except ValueError as exc:
        raise ValueError(f"{label}: {kind} artifact escapes run_dir: {resolved}") from exc
    return resolved


def resolve_artifact_path(run_dir: Path, raw: str, kind: str, label: str) -> Path:
    if raw == "":
        raise ValueError(f"{label}: empty {kind} path")
    raw_path = Path(raw)
    if any(part == ".." for part in raw_path.parts):
        raise ValueError(f"{label}: {kind} path contains '..': {raw}")

    candidates: list[Path] = []
    if raw_path.is_absolute():
        parts = raw_path.parts
        for anchor in ("profiles", "logs", "bz"):
            if anchor in parts:
                idx = parts.index(anchor)
                candidates.append(run_dir.joinpath(*parts[idx:]))
                break
        if run_dir.name in parts:
            idx = parts.index(run_dir.name)
            candidates.append(run_dir.joinpath(*parts[idx + 1 :]))
    else:
        candidates.append(run_dir / raw_path)
        candidates.append(run_dir / raw_path.name)

    for candidate in candidates:
        try:
            resolved = require_under_run_dir(run_dir, candidate, label, kind)
        except (FileNotFoundError, ValueError):
            continue
        if resolved.is_file():
            expected_parent = {
                "profile": "profiles",
                "stdout": "logs",
                "BZ": "bz",
            }[kind]
            if expected_parent not in resolved.parts:
                raise ValueError(
                    f"{label}: {kind} artifact is not under {expected_parent}/: {resolved}"
                )
            return resolved

    raise FileNotFoundError(f"{label}: {kind} artifact missing for {raw!r}")


def load_run_index(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        if reader.fieldnames is None:
            raise ValueError("missing header")
        missing = sorted(REQUIRED_COLUMNS - set(reader.fieldnames))
        if missing:
            raise ValueError(f"run-index missing required columns: {', '.join(missing)}")
        return list(reader)


def load_json(path: Path) -> dict[str, Any]:
    with path.open() as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise ValueError("top-level JSON is not an object")
    return data


def parse_unique_int(lines: list[str], key: str) -> int:
    pattern = re.compile(rf"^\s*{re.escape(key)}:\s*([0-9]+)\s*$")
    values = [strict_int(match.group(1)) for line in lines if (match := pattern.match(line))]
    if len(values) != 1:
        raise ValueError(f"expected exactly one stdout {key!r}, found {len(values)}")
    return values[0]


def parse_stdout_summary(text: str) -> StdoutSummary:
    lines = text.splitlines()
    ints = {key: parse_unique_int(lines, key) for key in STDOUT_INT_KEYS}

    radii_matches = [
        match
        for line in lines
        if (
            match := re.match(
                r"^\s*R_inner:\s*([0-9]+),\s*R_outer:\s*([0-9]+)\s*$", line
            )
        )
    ]
    if len(radii_matches) != 1:
        raise ValueError(f"expected exactly one stdout radii line, found {len(radii_matches)}")
    r_inner = strict_int(radii_matches[0].group(1), positive=True)
    r_outer = strict_int(radii_matches[0].group(2), positive=True)

    snapshot_matches = [
        match
        for line in lines
        if (match := re.match(r"^\s*snapshot: ([^\s].*?)\s*$", line))
    ]
    if len(snapshot_matches) != 1:
        raise ValueError(
            f"expected exactly one stdout snapshot line, found {len(snapshot_matches)}"
        )
    snapshot = snapshot_matches[0].group(1)

    early_matches = [
        match
        for line in lines
        if (match := re.match(r"^\s*early-exit:\s*(enabled|disabled)( \(taken\))?\s*$", line))
    ]
    if len(early_matches) != 1:
        raise ValueError(
            f"expected exactly one stdout early-exit line, found {len(early_matches)}"
        )
    early_exit_enabled = early_matches[0].group(1) == "enabled"
    early_exit_taken = early_matches[0].group(2) is not None

    verdict_matches = [
        match for line in lines if (match := re.match(r"^VERDICT:\s*(MOAT|SPANNING)\s*$", line))
    ]
    if len(verdict_matches) != 1:
        raise ValueError(
            f"expected exactly one stdout VERDICT line, found {len(verdict_matches)}"
        )
    verdict = verdict_matches[0].group(1)

    def optional_hash(key: str) -> str | None:
        matches = [
            match
            for line in lines
            if (match := re.match(rf"^\s*{re.escape(key)}:\s*([0-9a-f]{{64}})\s*$", line))
        ]
        if len(matches) > 1:
            raise ValueError(f"duplicate stdout {key}")
        return matches[0].group(1) if matches else None

    trace_matches = [
        match
        for line in lines
        if (match := re.match(r"^SPANNING_TRACE:\s+detected=([01])(?:\s|$)", line))
    ]
    if len(trace_matches) > 1:
        raise ValueError("duplicate SPANNING_TRACE lines")
    spanning_detected = bool(int(trace_matches[0].group(1))) if trace_matches else None

    return StdoutSummary(
        ints=ints,
        r_inner=r_inner,
        r_outer=r_outer,
        snapshot=snapshot,
        early_exit_enabled=early_exit_enabled,
        early_exit_taken=early_exit_taken,
        verdict=verdict,
        constants_hash=optional_hash("constants_hash"),
        mr_witness_sha256=optional_hash("mr_witness_sha256"),
        spanning_detected=spanning_detected,
    )


def parse_bz_summary(text: str) -> BzSummary:
    lines = text.splitlines()
    header_matches = [
        match
        for line in lines
        if (
            match := re.match(
                r"^BZ check:\s+R_inner=([0-9]+)\s+R_outer=([0-9]+)\s+K=([0-9]+)\s+",
                line,
            )
        )
    ]
    if len(header_matches) != 1:
        raise ValueError(f"expected exactly one BZ header, found {len(header_matches)}")
    verdict_lines = [
        line
        for line in lines
        if line.startswith("PASS: no Gaussian-prime norms found")
        or line.startswith("FAIL: Gaussian-prime norm")
    ]
    if len(verdict_lines) != 1:
        raise ValueError(f"expected exactly one BZ PASS/FAIL verdict, found {len(verdict_lines)}")
    gaussian_count_lines = [
        line for line in lines if re.match(r"^BZ_[IO]: gaussian_prime_norm_count=", line)
    ]
    failures = 0
    for line in gaussian_count_lines:
        failures += strict_int(line.rsplit("=", 1)[1])
    match = header_matches[0]
    return BzSummary(
        k_sq=strict_int(match.group(3), positive=True),
        r_inner=strict_int(match.group(1), positive=True),
        r_outer=strict_int(match.group(2), positive=True),
        verdict="PASS" if verdict_lines[0].startswith("PASS:") else "FAIL",
        failures=failures,
    )


def parse_command(command: Any) -> dict[str, str | bool]:
    if not isinstance(command, list) or not all(isinstance(item, str) for item in command):
        raise ValueError("profile command is not a string list")
    parsed: dict[str, str | bool] = {}
    i = 0
    flags_with_values = {
        "--k-sq",
        "--r-inner",
        "--r-outer",
        "--region",
        "--chunk-size",
        "--profile",
        "--snapshot-out",
        "--out",
        "--stats-level",
        "--emit-span-cert",
        "--sample-manifest",
        "--tile-sample-out",
    }
    bool_flags = {
        "--no-early-exit",
        "--overflow-diagnostics",
        "--trace-spanning",
        "--trace-spanning-path",
        "--overlap-compositor",
        "--timing",
        "--allow-uncertified-boundary-band",
    }
    while i < len(command):
        part = command[i]
        matched = False
        for flag in flags_with_values:
            if part == flag:
                if i + 1 >= len(command):
                    raise ValueError(f"command flag {flag} missing value")
                parsed[flag] = command[i + 1]
                i += 2
                matched = True
                break
            if part.startswith(flag + "="):
                parsed[flag] = part.split("=", 1)[1]
                i += 1
                matched = True
                break
        if matched:
            continue
        if part in bool_flags:
            parsed[part] = True
        i += 1
    return parsed


def validate_bz_log(
    findings: list[Finding],
    label: str,
    text: str,
    *,
    k_sq: int,
    r_inner: int,
    r_outer: int,
) -> None:
    try:
        summary = parse_bz_summary(text)
    except ValueError as exc:
        fail(findings, label, f"BZ log parse failed: {exc}")
        return
    expect_equal(findings, label, "BZ k_sq", summary.k_sq, k_sq)
    expect_equal(findings, label, "BZ r_inner", summary.r_inner, r_inner)
    expect_equal(findings, label, "BZ r_outer", summary.r_outer, r_outer)
    expect_equal(findings, label, "BZ verdict", summary.verdict, "PASS")
    expect_equal(findings, label, "BZ gaussian prime norm count", summary.failures, 0)


def recompute_bz(
    findings: list[Finding], label: str, *, k_sq: int, r_inner: int, r_outer: int
) -> None:
    cmd = [
        "uv",
        "run",
        str(BZ_CHECK),
        "--k-sq",
        str(k_sq),
        "--r-inner",
        str(r_inner),
        "--r-outer",
        str(r_outer),
    ]
    proc = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True, check=False)
    if proc.returncode != 0:
        fail(
            findings,
            label,
            "BZ recomputation failed with rc="
            f"{proc.returncode}: {(proc.stdout + proc.stderr).strip()}",
        )
        return
    validate_bz_log(findings, label, proc.stdout, k_sq=k_sq, r_inner=r_inner, r_outer=r_outer)


def vertex_key(vertex: Any) -> tuple[int, int] | None:
    if not isinstance(vertex, dict):
        return None
    tile_index = vertex.get("tile_index")
    group_label = vertex.get("group_label")
    if not is_int(tile_index) or not is_int(group_label):
        return None
    if tile_index < 0 or group_label <= 0:
        return None
    return (tile_index, group_label)


def edge_vertices(edge: Any) -> tuple[tuple[int, int], tuple[int, int]] | None:
    if not isinstance(edge, dict):
        return None
    lhs_tile = edge.get("lhs_tile_index")
    lhs_group = edge.get("lhs_group_label")
    rhs_tile = edge.get("rhs_tile_index")
    rhs_group = edge.get("rhs_group_label")
    if not all(is_int(v) for v in (lhs_tile, lhs_group, rhs_tile, rhs_group)):
        return None
    if lhs_tile < 0 or rhs_tile < 0 or lhs_group <= 0 or rhs_group <= 0:
        return None
    return (lhs_tile, lhs_group), (rhs_tile, rhs_group)


def validate_stitch_edge(
    findings: list[Finding],
    label: str,
    path_name: str,
    idx: int,
    edge: Any,
    *,
    active_tiles: int,
) -> tuple[tuple[int, int], tuple[int, int]] | None:
    endpoints = edge_vertices(edge)
    if endpoints is None:
        fail(findings, label, f"{path_name}[{idx}] has invalid vertices")
        return None

    event = edge.get("event") if isinstance(edge, dict) else None
    lhs_face = edge.get("lhs_face") if isinstance(edge, dict) else None
    rhs_face = edge.get("rhs_face") if isinstance(edge, dict) else None
    lhs_ordinal = edge.get("lhs_ordinal") if isinstance(edge, dict) else None
    rhs_ordinal = edge.get("rhs_ordinal") if isinstance(edge, dict) else None
    if not is_int(lhs_ordinal) or not is_int(rhs_ordinal):
        fail(findings, label, f"{path_name}[{idx}] ordinals are not integers")
    elif lhs_ordinal != rhs_ordinal:
        fail(
            findings,
            label,
            f"{path_name}[{idx}] ordinal mismatch: {lhs_ordinal!r} vs {rhs_ordinal!r}",
        )
    for side, vertex in (("lhs", endpoints[0]), ("rhs", endpoints[1])):
        if vertex[0] >= active_tiles:
            fail(
                findings,
                label,
                f"{path_name}[{idx}] {side} tile_index {vertex[0]} >= active {active_tiles}",
            )
    if event == "bridge_io":
        if {lhs_face, rhs_face} != {"I", "O"}:
            fail(findings, label, f"{path_name}[{idx}] bridge_io faces are {lhs_face}/{rhs_face}")
    elif event == "bridge_lr":
        if {lhs_face, rhs_face} != {"L", "R"}:
            fail(findings, label, f"{path_name}[{idx}] bridge_lr faces are {lhs_face}/{rhs_face}")
    else:
        fail(findings, label, f"{path_name}[{idx}] unexpected event {event!r}")
    return endpoints


def validate_path_continuity(
    findings: list[Finding],
    label: str,
    path_name: str,
    edges: Any,
    source: tuple[int, int] | None,
    target: tuple[int, int] | None,
    *,
    active_tiles: int,
) -> None:
    if not isinstance(edges, list):
        fail(findings, label, f"{path_name} is not a list")
        return
    if source is None or target is None:
        fail(findings, label, f"{path_name} source/target vertex is invalid")
        return
    current = source
    for idx, edge in enumerate(edges):
        endpoints = validate_stitch_edge(
            findings, label, path_name, idx, edge, active_tiles=active_tiles
        )
        if endpoints is None:
            return
        lhs, rhs = endpoints
        if lhs == current:
            current = rhs
        elif rhs == current:
            current = lhs
        else:
            fail(
                findings,
                label,
                f"{path_name}[{idx}] is discontinuous at {current}; edge is {lhs}<->{rhs}",
            )
            return
    if current != target:
        fail(findings, label, f"{path_name} ends at {current}, expected {target}")


def validate_stitch_path(
    findings: list[Finding],
    label: str,
    path: dict[str, Any],
    trace: dict[str, Any],
    *,
    active_tiles: int,
) -> None:
    expect_equal(findings, label, "stitch_path.failure_reason", path.get("failure_reason"), "")
    recorded_edges = require_json_int(findings, label, path, "recorded_edges")
    if recorded_edges is not None and recorded_edges <= 0:
        fail(findings, label, f"stitch_path.recorded_edges is not positive: {recorded_edges}")

    inner_source = vertex_key(path.get("inner_source"))
    outer_source = vertex_key(path.get("outer_source"))
    inner_endpoint = vertex_key(path.get("inner_endpoint"))
    outer_endpoint = vertex_key(path.get("outer_endpoint"))
    inner_edges = path.get("inner_path_edges")
    outer_edges = path.get("outer_path_edges")

    expect_equal(
        findings,
        label,
        "trace inner source",
        (trace.get("inner_source_tile_index"), trace.get("inner_source_group_label")),
        inner_source,
    )
    expect_equal(
        findings,
        label,
        "trace outer source",
        (trace.get("outer_source_tile_index"), trace.get("outer_source_group_label")),
        outer_source,
    )
    if is_int(path.get("inner_path_edge_count")) and isinstance(inner_edges, list):
        expect_equal(
            findings,
            label,
            "stitch_path.inner_path_edge_count",
            path.get("inner_path_edge_count"),
            len(inner_edges),
        )
    else:
        fail(findings, label, "stitch_path inner path count/edges malformed")
    if is_int(path.get("outer_path_edge_count")) and isinstance(outer_edges, list):
        expect_equal(
            findings,
            label,
            "stitch_path.outer_path_edge_count",
            path.get("outer_path_edge_count"),
            len(outer_edges),
        )
    else:
        fail(findings, label, "stitch_path outer path count/edges malformed")

    validate_path_continuity(
        findings,
        label,
        "inner_path_edges",
        inner_edges,
        inner_source,
        inner_endpoint,
        active_tiles=active_tiles,
    )
    validate_path_continuity(
        findings,
        label,
        "outer_path_edges",
        outer_edges,
        outer_source,
        outer_endpoint,
        active_tiles=active_tiles,
    )

    final_bridge = path.get("final_bridge")
    final_endpoints = validate_stitch_edge(
        findings, label, "final_bridge", 0, final_bridge, active_tiles=active_tiles
    )
    if final_endpoints is None or inner_endpoint is None or outer_endpoint is None:
        return
    if set(final_endpoints) != {inner_endpoint, outer_endpoint}:
        fail(
            findings,
            label,
            "final_bridge endpoints do not match reconstructed endpoints: "
            f"{final_endpoints} vs {inner_endpoint}/{outer_endpoint}",
        )
    trace_pair = {
        (trace.get("lhs_tile_index"), trace.get("lhs_group_label")),
        (trace.get("rhs_tile_index"), trace.get("rhs_group_label")),
    }
    if set(final_endpoints) != trace_pair:
        fail(
            findings,
            label,
            f"final_bridge endpoints do not match profile trace endpoints: {trace_pair}",
        )


def validate_profile_command(
    findings: list[Finding],
    label: str,
    profile: dict[str, Any],
    *,
    k_sq: int,
    r_inner: int,
    r_outer: int,
    region: str,
    chunk_size: int,
    early_exit_enabled: bool,
    profile_path: Path,
) -> dict[str, str | bool]:
    try:
        command = parse_command(profile.get("command"))
    except ValueError as exc:
        fail(findings, label, f"profile command parse failed: {exc}")
        return {}

    expect_equal(findings, label, "command --k-sq", command.get("--k-sq"), str(k_sq))
    expect_equal(findings, label, "command --r-inner", command.get("--r-inner"), str(r_inner))
    expect_equal(findings, label, "command --r-outer", command.get("--r-outer"), str(r_outer))
    expect_equal(findings, label, "command --region", command.get("--region"), region)
    expect_equal(findings, label, "command --chunk-size", command.get("--chunk-size"), str(chunk_size))
    expect_equal(
        findings,
        label,
        "command --no-early-exit",
        command.get("--no-early-exit", False),
        not early_exit_enabled,
    )
    expect_equal(
        findings,
        label,
        "command --overflow-diagnostics",
        command.get("--overflow-diagnostics", False),
        True,
    )
    command_profile = command.get("--profile")
    if not isinstance(command_profile, str):
        fail(findings, label, "command missing --profile")
    elif Path(command_profile).name != profile_path.name:
        fail(
            findings,
            label,
            f"command --profile basename mismatch: {command_profile!r} vs {profile_path.name!r}",
        )
    return command


def validate_row(
    run_dir: Path,
    row: dict[str, str],
    findings: list[Finding],
    artifact_paths_seen: set[Path],
    *,
    expected_k: int | None,
    allow_spanning_without_path: bool,
    allow_early_spanning: bool,
    recompute: bool,
    require_profile_bz: bool,
) -> None:
    label = row.get("label") or "<missing-label>"
    if label == "<missing-label>":
        fail(findings, label, "run-index label is missing")
        return

    k_sq = require_index_int(findings, label, row, "K", positive=True) if row.get("K") else None
    r_inner = require_index_int(findings, label, row, "R_inner", positive=True)
    width = require_index_int(findings, label, row, "width", positive=True)
    r_outer = require_index_int(findings, label, row, "R_outer", positive=True)
    rc = require_index_int(findings, label, row, "rc")
    produced = require_index_int(findings, label, row, "produced")
    ingested = require_index_int(findings, label, row, "ingested")
    if None in (r_inner, width, r_outer, rc, produced, ingested):
        return
    assert r_inner is not None
    assert width is not None
    assert r_outer is not None
    assert rc is not None
    assert produced is not None
    assert ingested is not None

    expect_equal(findings, label, "width", r_outer - r_inner, width)
    expect_equal(findings, label, "rc", rc, 0)
    for key in ZERO_INDEX_COUNTERS:
        value = require_index_int(findings, label, row, key)
        if value is not None:
            expect_zero(findings, label, f"run-index {key}", value)

    verdict = row.get("verdict", "")
    if verdict not in {"MOAT", "SPANNING"}:
        fail(findings, label, f"unexpected verdict {verdict!r}")

    try:
        paths = ArtifactPaths(
            profile=resolve_artifact_path(run_dir, row.get("profile", ""), "profile", label),
            stdout=resolve_artifact_path(run_dir, row.get("log", ""), "stdout", label),
            bz=resolve_artifact_path(run_dir, row.get("bz_log", ""), "BZ", label),
        )
    except (FileNotFoundError, ValueError) as exc:
        fail(findings, label, str(exc))
        return
    for artifact in (paths.profile, paths.stdout, paths.bz):
        if artifact in artifact_paths_seen:
            fail(findings, label, f"artifact path reused by multiple rows: {artifact}")
        artifact_paths_seen.add(artifact)

    try:
        profile = load_json(paths.profile)
    except Exception as exc:  # noqa: BLE001 - CLI validator reports context.
        fail(findings, label, f"profile JSON unreadable: {exc}")
        return
    try:
        stdout = parse_stdout_summary(paths.stdout.read_text(errors="replace"))
    except ValueError as exc:
        fail(findings, label, f"stdout parse failed: {exc}")
        return

    expect_equal(findings, label, "profile schema_version", profile.get("schema_version"), 1)
    radii = profile.get("radii", {})
    if not isinstance(radii, dict):
        fail(findings, label, "profile radii is not an object")
        radii = {}
    profile_k_sq = require_json_int(findings, label, radii, "k_sq", positive=True)
    if k_sq is None:
        k_sq = profile_k_sq
    if k_sq is None:
        return
    if expected_k is not None:
        expect_equal(findings, label, "expected K", k_sq, expected_k)
    expect_equal(findings, label, "profile k_sq", profile_k_sq, k_sq)
    expect_equal(findings, label, "profile r_inner", radii.get("r_inner"), r_inner)
    expect_equal(findings, label, "profile r_outer", radii.get("r_outer"), r_outer)

    region = profile.get("region")
    expect_equal(findings, label, "profile region", region, "full-octant")
    expect_equal(findings, label, "profile verdict", profile.get("verdict"), verdict)

    tiles = profile.get("tiles", {})
    if not isinstance(tiles, dict):
        fail(findings, label, "profile tiles is not an object")
        tiles = {}
    active = require_json_int(findings, label, tiles, "active")
    columns_ingested = require_json_int(findings, label, tiles, "columns_ingested")
    profile_produced = require_json_int(findings, label, tiles, "produced")
    profile_ingested = require_json_int(findings, label, tiles, "ingested")
    expect_equal(findings, label, "profile produced", profile_produced, produced)
    expect_equal(findings, label, "profile ingested", profile_ingested, ingested)

    chunk = profile.get("chunk", {})
    if not isinstance(chunk, dict):
        fail(findings, label, "profile chunk is not an object")
        chunk = {}
    chunk_size = require_json_int(findings, label, chunk, "target_tiles", positive=True)
    expect_equal(findings, label, "profile chunk max_overshoot", chunk.get("max_overshoot_tiles"), stdout.ints["chunk_overshoot_tiles"])
    expect_equal(findings, label, "profile chunk total_overshoot", chunk.get("total_overshoot_tiles"), stdout.ints["total_chunk_overshoot_tiles"])
    expect_equal(findings, label, "profile app_batches", chunk.get("app_batches"), stdout.ints["app batches"])

    overflow = profile.get("overflow_counters", {})
    if not isinstance(overflow, dict):
        fail(findings, label, "profile overflow_counters is not an object")
        overflow = {}
    for key in ZERO_PROFILE_COUNTERS:
        expect_zero(findings, label, f"profile {key}", overflow.get(key))
    host_counts = profile.get("host_tileop_counters", {})
    if not isinstance(host_counts, dict):
        fail(findings, label, "profile host_tileop_counters is not an object")
        host_counts = {}
    expect_zero(
        findings,
        label,
        "profile emitted_overflow_bit_count",
        host_counts.get("emitted_overflow_bit_count"),
    )

    bz = profile.get("bz", {})
    if bz == {} and not require_profile_bz:
        pass
    elif not isinstance(bz, dict):
        fail(findings, label, "profile bz is not an object")
        bz = {}
    if bz != {} or require_profile_bz:
        checked = require_json_bool(findings, label, bz, "checked")
        clean = require_json_bool(findings, label, bz, "clean")
        override_used = require_json_bool(findings, label, bz, "override_used")
        if checked is not None:
            expect_equal(findings, label, "profile bz.checked", checked, True)
        if clean is not None:
            expect_equal(findings, label, "profile bz.clean", clean, True)
        if override_used is not None:
            expect_equal(findings, label, "profile bz.override_used", override_used, False)
        bad_norm_count = bz.get("bad_norm_count")
        if bad_norm_count is not None:
            expect_zero(findings, label, "profile bz.bad_norm_count", bad_norm_count)

    expect_equal(findings, label, "stdout K_SQ", stdout.ints["K_SQ"], k_sq)
    expect_equal(findings, label, "stdout r_inner", stdout.r_inner, r_inner)
    expect_equal(findings, label, "stdout r_outer", stdout.r_outer, r_outer)
    expect_equal(findings, label, "stdout verdict", stdout.verdict, verdict)
    expect_equal(findings, label, "stdout active tiles", stdout.ints["active tiles"], active)
    expect_equal(findings, label, "stdout produced tiles", stdout.ints["produced tiles"], produced)
    expect_equal(findings, label, "stdout ingested tiles", stdout.ints["ingested tiles"], ingested)
    expect_equal(findings, label, "stdout columns", stdout.ints["ingested columns"], columns_ingested)
    expect_equal(findings, label, "stdout chunk-size", stdout.ints["chunk-size"], chunk_size)
    expect_equal(findings, label, "snapshot mode", stdout.snapshot, "disabled")
    for key in (
        "k1_cand_overflow_count",
        "k4_prime_overflow_count",
        "k4_group_overflow_count",
        "k5_port_overflow_count",
        "emitted_overflow_bit_count",
    ):
        expect_zero(findings, label, f"stdout {key}", stdout.ints[key])

    if active is None or chunk_size is None:
        return
    early_exit_enabled = require_json_bool(findings, label, profile, "early_exit_enabled")
    early_exit_taken = require_json_bool(findings, label, profile, "early_exit_taken")
    if early_exit_enabled is None or early_exit_taken is None:
        return
    expect_equal(findings, label, "stdout early_exit_enabled", stdout.early_exit_enabled, early_exit_enabled)
    expect_equal(findings, label, "stdout early_exit_taken", stdout.early_exit_taken, early_exit_taken)

    if region != "full-octant":
        return
    command = validate_profile_command(
        findings,
        label,
        profile,
        k_sq=k_sq,
        r_inner=r_inner,
        r_outer=r_outer,
        region=region,
        chunk_size=chunk_size,
        early_exit_enabled=early_exit_enabled,
        profile_path=paths.profile,
    )

    validate_bz_log(
        findings,
        label,
        paths.bz.read_text(errors="replace"),
        k_sq=k_sq,
        r_inner=r_inner,
        r_outer=r_outer,
    )
    if recompute:
        recompute_bz(findings, label, k_sq=k_sq, r_inner=r_inner, r_outer=r_outer)

    trace_present = "spanning_trace" in profile
    trace = profile.get("spanning_trace", {})
    if not trace_present:
        trace = {}
    elif not isinstance(trace, dict):
        fail(findings, label, "profile spanning_trace is not an object")
        trace = {}
    trace_detected = (
        require_json_bool(findings, label, trace, "detected")
        if trace_present
        else None
    )
    if stdout.spanning_detected is not None:
        expect_equal(findings, label, "stdout/profile spanning trace", stdout.spanning_detected, trace_detected)

    if verdict == "MOAT":
        expect_equal(findings, label, "MOAT active/produced", active, produced)
        expect_equal(findings, label, "MOAT active/ingested", active, ingested)
        expect_equal(findings, label, "MOAT early_exit_enabled", early_exit_enabled, False)
        expect_equal(findings, label, "MOAT early_exit_taken", early_exit_taken, False)
        if trace_present:
            expect_equal(findings, label, "MOAT spanning_trace.detected", trace_detected, False)
    elif verdict == "SPANNING":
        if trace_present:
            expect_equal(findings, label, "SPANNING spanning_trace.detected", trace_detected, True)
        elif not allow_spanning_without_path:
            fail(findings, label, "SPANNING row lacks spanning_trace evidence")
        if not allow_early_spanning:
            expect_equal(findings, label, "SPANNING early_exit_enabled", early_exit_enabled, False)
            expect_equal(findings, label, "SPANNING early_exit_taken", early_exit_taken, False)
            expect_equal(findings, label, "no-early SPANNING active/produced", active, produced)
            expect_equal(findings, label, "no-early SPANNING active/ingested", active, ingested)

        has_trace_path_command = command.get("--trace-spanning-path", False) is True
        if not has_trace_path_command and not allow_spanning_without_path:
            fail(findings, label, "SPANNING row lacks --trace-spanning-path evidence")
        elif has_trace_path_command:
            path = trace.get("stitch_path")
            if not isinstance(path, dict):
                fail(findings, label, "trace-spanning-path command lacks stitch_path object")
            else:
                expect_equal(findings, label, "stitch_path.enabled", path.get("enabled"), True)
                expect_equal(
                    findings,
                    label,
                    "stitch_path.reconstructed",
                    path.get("reconstructed"),
                    True,
                )
                expect_equal(
                    findings,
                    label,
                    "stitch_path.final_bridge_present",
                    path.get("final_bridge_present"),
                    True,
                )
                validate_stitch_path(findings, label, path, trace, active_tiles=active)


def main() -> int:
    args = parse_args()
    run_dir = args.run_dir.resolve(strict=True)
    index_path = run_dir / "run-index.tsv"
    if not index_path.is_file():
        print(f"REJECT: missing run-index.tsv in {run_dir}", file=sys.stderr)
        return 2
    try:
        rows = load_run_index(index_path)
    except Exception as exc:  # noqa: BLE001 - CLI validator reports context.
        print(f"REJECT: could not read run-index.tsv: {exc}", file=sys.stderr)
        return 2
    if not rows:
        print(f"REJECT: empty run-index.tsv in {run_dir}", file=sys.stderr)
        return 2

    findings: list[Finding] = []
    labels: set[str] = set()
    artifact_paths_seen: set[Path] = set()
    for row in rows:
        label = row.get("label", "")
        if label in labels:
            fail(findings, label or "<missing-label>", "duplicate run-index label")
            continue
        labels.add(label)
        validate_row(
            run_dir,
            row,
            findings,
            artifact_paths_seen,
            expected_k=args.expected_k,
            allow_spanning_without_path=args.allow_spanning_without_path,
            allow_early_spanning=args.allow_early_spanning,
            recompute=not args.no_recompute_bz,
            require_profile_bz=args.require_profile_bz,
        )

    if findings:
        print(f"REJECT {run_dir}")
        for finding in findings:
            print(f"  [{finding.label}] {finding.message}")
        return 1

    print(f"ACCEPT {run_dir}")
    print(f"  rows: {len(rows)}")
    for row in rows:
        print(
            "  "
            f"{row.get('label', '<missing-label>')}: "
            f"K={row.get('K', 'profile')} "
            f"R={row.get('R_inner')}..{row.get('R_outer')} "
            f"verdict={row.get('verdict')}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
