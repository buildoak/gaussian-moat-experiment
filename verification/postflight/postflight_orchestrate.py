#!/usr/bin/env python3
"""Turnkey post-flight orchestration for campaign profile JSON files."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
VERIFICATION_ROOT = REPO_ROOT / "verification"
DEFAULT_BUILD_DIR = VERIFICATION_ROOT / "build"
POSTFLIGHT_STATUSES = {
    "CLAIM_PROOF_MISSING",
    "MOAT_PROOF_PASS",
    "REJECT",
    "RUN_CONTRACT_PASS",
    "SPAN_PROOF_PASS",
    "TILE_SAMPLE_AUDIT_PASS",
}
SAMPLE_CLASSES = [
    "geo_I",
    "geo_O",
    "axis",
    "diagonal",
    "high_pressure",
    "deterministic_random",
]


class PostflightError(RuntimeError):
    pass


def load_json(path: Path) -> dict[str, Any]:
    with path.open() as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise PostflightError(f"{path}: expected a top-level JSON object")
    return data


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def get_path(data: dict[str, Any], *keys: str) -> Any:
    current: Any = data
    for key in keys:
        if not isinstance(current, dict) or key not in current:
            return None
        current = current[key]
    return current


def first_value(*values: Any) -> Any:
    for value in values:
        if value is not None:
            return value
    return None


def as_int(value: Any, field: str) -> int:
    if isinstance(value, bool) or value is None:
        raise PostflightError(f"missing integer field: {field}")
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value)
        except ValueError as exc:
            raise PostflightError(f"{field}: not an integer: {value}") from exc
    raise PostflightError(f"{field}: not an integer")


def as_optional_int(value: Any) -> int | None:
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


def as_str(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise PostflightError(f"missing string field: {field}")
    return value


def verdict_mode(verdict: str) -> str:
    if verdict == "SPANNING":
        return "ANY-SPAN"
    if verdict == "MOAT":
        return "ANY-SHELL-MOAT"
    raise PostflightError(f"unsupported verdict: {verdict}")


def profile_run_root(profile_path: Path) -> Path:
    if profile_path.parent.name == "profiles":
        return profile_path.parent.parent
    return profile_path.parent


def default_out_dir(profile_paths: list[Path]) -> Path:
    roots = {profile_run_root(path.resolve()) for path in profile_paths}
    if len(roots) == 1:
        return next(iter(roots)) / "postflight"
    return Path.cwd() / "postflight"


def choose_by_index(paths: list[Path], index: int, label: str) -> Path | None:
    if not paths:
        return None
    if len(paths) == 1:
        return paths[0]
    if index >= len(paths):
        raise PostflightError(f"{label}: expected one path or one per profile")
    return paths[index]


def path_candidates(raw: Any, profile_path: Path) -> list[Path]:
    if raw is None:
        return []
    raw_path = Path(str(raw)).expanduser()
    if raw_path.is_absolute():
        return [raw_path]
    run_root = profile_run_root(profile_path)
    return [
        profile_path.parent / raw_path,
        run_root / raw_path,
        Path.cwd() / raw_path,
        REPO_ROOT / raw_path,
    ]


def first_existing(paths: list[Path]) -> Path | None:
    for path in paths:
        if path.exists():
            return path.resolve()
    return None


def infer_sample_manifest(profile: dict[str, Any], profile_path: Path) -> Path | None:
    explicit = first_value(
        profile.get("sample_manifest_path"),
        get_path(profile, "stats_v2", "sample_manifest_path"),
        get_path(profile, "sample_audit", "manifest_path"),
    )
    found = first_existing(path_candidates(explicit, profile_path))
    if found:
        return found

    stem = profile_path.name.removesuffix(".profile.json").removesuffix(".json")
    run_root = profile_run_root(profile_path)
    patterns = [
        f"{stem}.sample-manifest.json",
        f"{stem}.samples.manifest.json",
        f"{stem}.manifest.json",
        f"{profile_path.stem}.sample-manifest.json",
        f"{profile_path.stem}.manifest.json",
    ]
    for base in (profile_path.parent, run_root, run_root / "samples"):
        for pattern in patterns:
            candidate = base / pattern
            if candidate.exists():
                return candidate.resolve()
    return None


def infer_samples(profile: dict[str, Any], profile_path: Path) -> Path | None:
    explicit = first_value(
        profile.get("tile_sample_path"),
        profile.get("samples_path"),
        get_path(profile, "stats_v2", "tile_sample_path"),
        get_path(profile, "sample_audit", "samples_path"),
    )
    found = first_existing(path_candidates(explicit, profile_path))
    if found:
        return found

    stem = profile_path.name.removesuffix(".profile.json").removesuffix(".json")
    run_root = profile_run_root(profile_path)
    patterns = [
        f"{stem}.tiles.jsonl",
        f"{stem}.samples.jsonl",
        f"{profile_path.stem}.tiles.jsonl",
        f"{profile_path.stem}.samples.jsonl",
    ]
    for base in (profile_path.parent, run_root, run_root / "samples"):
        for pattern in patterns:
            candidate = base / pattern
            if candidate.exists():
                return candidate.resolve()
    return None


def resolve_tool(path: Path | None, name: str) -> Path | str:
    if path is not None:
        return path
    built = DEFAULT_BUILD_DIR / name
    if built.exists():
        return built
    return name


def run_command(command: list[str]) -> dict[str, Any]:
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    return {
        "command": command,
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def count_jsonl_samples(path: Path) -> tuple[int, dict[str, int]]:
    count = 0
    class_counts: dict[str, int] = {}
    with path.open() as fh:
        for line_number, line in enumerate(fh, start=1):
            if not line.strip():
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError as exc:
                raise PostflightError(f"{path}:{line_number}: invalid JSONL") from exc
            if not isinstance(item, dict):
                raise PostflightError(f"{path}:{line_number}: expected object record")
            sample_class = item.get("sample_class")
            if isinstance(sample_class, str):
                class_counts[sample_class] = class_counts.get(sample_class, 0) + 1
            count += 1
    return count, class_counts


def manifest_quotas(manifest: dict[str, Any]) -> dict[str, int]:
    quotas: dict[str, int] = {}
    raw = manifest.get("quotas")
    if isinstance(raw, dict):
        for key, value in raw.items():
            parsed = as_optional_int(value)
            if parsed is not None:
                quotas[str(key)] = parsed
    selection = manifest.get("selection")
    if isinstance(selection, list):
        for item in selection:
            if not isinstance(item, dict):
                continue
            klass = item.get("class") or item.get("sample_class")
            target = as_optional_int(item.get("target_count", item.get("quota")))
            if isinstance(klass, str) and target is not None:
                quotas[klass] = target
    return {klass: quotas.get(klass, 0) for klass in SAMPLE_CLASSES if klass in quotas}


def manifest_exhaustion(manifest: dict[str, Any]) -> dict[str, bool]:
    out: dict[str, bool] = {}
    for key in ("population_exhausted", "exhausted"):
        raw = manifest.get(key)
        if not isinstance(raw, dict):
            continue
        for klass, value in raw.items():
            if isinstance(value, bool):
                out[str(klass)] = value
            elif isinstance(value, dict):
                out[str(klass)] = bool(value.get("exhausted") or value.get("population_exhausted"))
    return out


def artifact_entry(path: Path, name: str, artifact_type: str | None = None) -> dict[str, Any]:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    item: dict[str, Any] = {
        "name": name,
        "path": str(path),
        "sha256": digest,
        "size_bytes": path.stat().st_size,
        "schema_version": 1,
    }
    if artifact_type:
        item["type"] = artifact_type
    return item


def build_row(
    profile: dict[str, Any],
    profile_path: Path,
    row_class: str,
    telemetry_level: str | None,
    claim_proof_required: bool,
) -> dict[str, Any]:
    k_sq = as_int(first_value(get_path(profile, "radii", "k_sq"), profile.get("k_sq")), "k_sq")
    r_inner = as_int(
        first_value(get_path(profile, "radii", "r_inner"), profile.get("r_inner")), "r_inner"
    )
    r_outer = as_int(
        first_value(get_path(profile, "radii", "r_outer"), profile.get("r_outer")), "r_outer"
    )
    width = as_optional_int(
        first_value(get_path(profile, "radii", "width"), profile.get("width"))
    )
    if width is None:
        width = r_outer - r_inner
    region = as_str(first_value(profile.get("region"), get_path(profile, "radii", "region")), "region")
    verdict = as_str(profile.get("verdict"), "verdict")
    level = telemetry_level or first_value(
        profile.get("telemetry_level"),
        profile.get("stats_level"),
        get_path(profile, "stats_v2", "telemetry_level"),
        "profile",
    )
    if level not in {"none", "profile", "audit", "full"}:
        level = "profile"
    return {
        "claim_id": str(first_value(profile.get("claim_id"), profile_path.stem)),
        "k_sq": k_sq,
        "r_inner": r_inner,
        "r_outer": r_outer,
        "width": width,
        "region": region,
        "verdict": verdict,
        "verdict_mode": verdict_mode(verdict),
        "row_class": row_class,
        "telemetry_level": level,
        "claim_proof_required": claim_proof_required,
    }


def build_bz(profile: dict[str, Any]) -> dict[str, Any]:
    bz = profile.get("bz") if isinstance(profile.get("bz"), dict) else {}
    k_sq = as_optional_int(first_value(get_path(profile, "radii", "k_sq"), profile.get("k_sq")))
    r_inner = as_optional_int(first_value(get_path(profile, "radii", "r_inner"), profile.get("r_inner")))
    r_outer = as_optional_int(first_value(get_path(profile, "radii", "r_outer"), profile.get("r_outer")))
    width = as_optional_int(first_value(get_path(profile, "radii", "width"), profile.get("width")))
    if width is None and r_inner is not None and r_outer is not None:
        width = r_outer - r_inner
    region = first_value(profile.get("region"), get_path(profile, "radii", "region"))
    record = {
        "checked": bool(first_value(bz.get("checked"), profile.get("bz_checked"), False)),
        "clean": bool(first_value(bz.get("clean"), bz.get("bz_clean"), profile.get("bz_clean"), False)),
        "override_used": bool(
            first_value(
                bz.get("override_used"),
                bz.get("override"),
                profile.get("bz_override_used"),
                False,
            )
        ),
        "bad_norm_count": as_optional_int(
            first_value(bz.get("bad_norm_count"), profile.get("bz_bad_norm_count"))
        )
        or 0,
    }
    for key, value in (
        ("k_sq", k_sq),
        ("r_inner", r_inner),
        ("r_outer", r_outer),
        ("width", width),
    ):
        if value is not None:
            record[key] = value
    if isinstance(region, str) and region:
        record["region"] = region
    return record


def parse_bz_log(path: Path, row: dict[str, Any]) -> dict[str, Any]:
    text = path.read_text(errors="replace")
    header = re.search(
        r"^BZ check:\s+R_inner=([0-9]+)\s+R_outer=([0-9]+)\s+K=([0-9]+)\s+",
        text,
        re.MULTILINE,
    )
    if header is None:
        raise PostflightError(f"{path}: missing BZ check header")

    verdict_lines = [
        line
        for line in text.splitlines()
        if line.startswith("PASS: no Gaussian-prime norms found")
        or line.startswith("FAIL: Gaussian-prime norm")
    ]
    if len(verdict_lines) != 1:
        raise PostflightError(f"{path}: expected exactly one BZ PASS/FAIL verdict")

    bad_norm_count = 0
    for match in re.finditer(r"^BZ_[IO]: gaussian_prime_norm_count=([0-9]+)$", text, re.MULTILINE):
        bad_norm_count += int(match.group(1))

    record: dict[str, Any] = {
        "checked": True,
        "clean": verdict_lines[0].startswith("PASS:") and bad_norm_count == 0,
        "override_used": False,
        "bad_norm_count": bad_norm_count,
        "k_sq": int(header.group(3)),
        "r_inner": int(header.group(1)),
        "r_outer": int(header.group(2)),
        "width": row.get("width"),
        "region": row.get("region"),
        "source": "external_bz_check",
        "path": str(path),
    }
    return record


def build_sample_audit(
    manifest_path: Path | None,
    samples_path: Path | None,
    tile_sample_result: dict[str, Any] | None,
) -> dict[str, Any] | None:
    if manifest_path is None and samples_path is None:
        return None
    if tile_sample_result is None or tile_sample_result["returncode"] != 0:
        return {
            "present": True,
            "status": "FAIL",
            "sample_count": 0,
            "manifest": {},
        }
    manifest = load_json(manifest_path) if manifest_path else {}
    sample_count = as_optional_int(manifest.get("sample_count"))
    class_counts: dict[str, int] = {}
    if isinstance(manifest.get("class_counts"), dict):
        for key, value in manifest["class_counts"].items():
            parsed = as_optional_int(value)
            if parsed is not None:
                class_counts[str(key)] = parsed
    if samples_path is not None:
        counted, counted_classes = count_jsonl_samples(samples_path)
        sample_count = sample_count if sample_count is not None else counted
        if not class_counts:
            class_counts = counted_classes
    return {
        "present": True,
        "status": "PASS",
        "sample_count": sample_count or 0,
        "manifest": manifest,
        "quotas": manifest_quotas(manifest),
        "class_counts": class_counts,
        "population_exhausted": manifest_exhaustion(manifest),
    }


def load_log_object(path: Path | None) -> dict[str, Any] | None:
    if path is None:
        return None
    try:
        return load_json(path)
    except (OSError, json.JSONDecodeError, PostflightError):
        return {"path": str(path)}


def build_bundle(
    profile_path: Path,
    profile: dict[str, Any],
    row: dict[str, Any],
    bz_log_path: Path | None,
    manifest_path: Path | None,
    samples_path: Path | None,
    span_cert_path: Path | None,
    log_path: Path | None,
    tile_sample_result: dict[str, Any] | None,
) -> dict[str, Any]:
    stem = profile_path.name.removesuffix(".profile.json").removesuffix(".json")
    bundle: dict[str, Any] = {
        "schema_version": 1,
        "bundle_id": f"{stem}-postflight",
        "row": row,
        "profile": profile,
        "bz": parse_bz_log(bz_log_path, row) if bz_log_path else build_bz(profile),
        "overflow_counters": profile.get("overflow_counters", {}),
    }

    sample_audit = build_sample_audit(manifest_path, samples_path, tile_sample_result)
    if sample_audit is not None:
        bundle["sample_audit"] = sample_audit

    if span_cert_path is not None:
        span_cert = load_json(span_cert_path)
        span_cert.setdefault("present", True)
        span_cert.setdefault("path", str(span_cert_path))
        bundle["span_certificate"] = span_cert

    stdout = load_log_object(log_path)
    if stdout is not None:
        bundle["stdout"] = stdout

    artifacts = [artifact_entry(profile_path, "profile", "profile")]
    if manifest_path is not None:
        artifacts.append(artifact_entry(manifest_path, "sample_manifest", "sample_manifest"))
    if samples_path is not None:
        artifacts.append(artifact_entry(samples_path, "tile_samples", "tile_samples"))
    if span_cert_path is not None:
        artifacts.append(artifact_entry(span_cert_path, "span_certificate", "span_certificate"))
    if bz_log_path is not None:
        artifacts.append(artifact_entry(bz_log_path, "bz_log", "bz_log"))
    if log_path is not None and log_path.exists():
        artifacts.append(artifact_entry(log_path, "log", "log"))
    if artifacts:
        bundle["artifacts"] = artifacts

    return bundle


def parse_checker_report(raw: str) -> dict[str, Any] | None:
    raw = raw.strip()
    if not raw:
        return None
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        return None
    return data if isinstance(data, dict) else None


def run_postflight_check(tool: Path | str, bundle_path: Path, report_path: Path) -> dict[str, Any]:
    result = run_command([str(tool), str(bundle_path), "--report", str(report_path)])
    report = None
    if report_path.exists():
        try:
            report = load_json(report_path)
        except (OSError, json.JSONDecodeError, PostflightError):
            report = None
    report = report or parse_checker_report(result["stdout"]) or parse_checker_report(result["stderr"])
    if report is None:
        report = {
            "schema_version": 1,
            "status": "REJECT",
            "errors": ["postflight_check did not emit parseable JSON"],
            "warnings": [],
        }
        write_json(report_path, report)
    return {"process": result, "report": report}


def run_normalizer(
    script: Path,
    profile_paths: list[Path],
    report_paths: list[Path],
    out_path: Path,
) -> dict[str, Any]:
    command = [
        sys.executable,
        str(script),
        "--profiles",
        *[str(path) for path in profile_paths],
        "--audits",
        *[str(path) for path in report_paths],
        "--out",
        str(out_path),
    ]
    return run_command(command)


def orchestrate(args: argparse.Namespace) -> int:
    profile_paths = [path.expanduser().resolve() for path in args.profiles]
    out_dir = (args.out_dir.expanduser().resolve() if args.out_dir else default_out_dir(profile_paths))
    out_dir.mkdir(parents=True, exist_ok=True)

    tile_sample_check = resolve_tool(args.tile_sample_check, "tile_sample_check")
    postflight_check = resolve_tool(args.postflight_check, "postflight_check")
    normalize_script = args.normalize_sweep_rows or (VERIFICATION_ROOT / "stats" / "normalize_sweep_rows.py")

    records: list[dict[str, Any]] = []
    report_paths: list[Path] = []

    for index, profile_path in enumerate(profile_paths):
        profile = load_json(profile_path)
        manifest_path = choose_by_index(args.sample_manifest, index, "--sample-manifest")
        samples_path = choose_by_index(args.samples, index, "--samples")
        span_cert_path = choose_by_index(args.span_cert, index, "--span-cert")
        bz_log_path = choose_by_index(args.bz_log, index, "--bz-log")
        log_path = choose_by_index(args.log, index, "--log")

        manifest_path = manifest_path.expanduser().resolve() if manifest_path else infer_sample_manifest(profile, profile_path)
        samples_path = samples_path.expanduser().resolve() if samples_path else infer_samples(profile, profile_path)
        span_cert_path = span_cert_path.expanduser().resolve() if span_cert_path else None
        bz_log_path = bz_log_path.expanduser().resolve() if bz_log_path else None
        log_path = log_path.expanduser().resolve() if log_path else None

        tile_sample_result = None
        if samples_path is not None:
            command = [str(tile_sample_check), "--samples", str(samples_path)]
            if manifest_path is not None:
                command.extend(["--manifest", str(manifest_path)])
            tile_sample_result = run_command(command)

        claim_proof_required = args.claim_proof_required or args.row_class == "accepted"
        row = build_row(
            profile,
            profile_path,
            args.row_class,
            args.telemetry_level,
            claim_proof_required,
        )
        bundle = build_bundle(
            profile_path,
            profile,
            row,
            bz_log_path,
            manifest_path,
            samples_path,
            span_cert_path,
            log_path,
            tile_sample_result,
        )
        stem = profile_path.name.removesuffix(".profile.json").removesuffix(".json")
        bundle_path = out_dir / f"{stem}.postflight.bundle.json"
        report_path = out_dir / f"{stem}.postflight.report.json"
        write_json(bundle_path, bundle)

        postflight = run_postflight_check(postflight_check, bundle_path, report_path)
        report_paths.append(report_path)
        status = postflight["report"].get("status")
        records.append(
            {
                "profile": str(profile_path),
                "bundle": str(bundle_path),
                "report": str(report_path),
                "status": status,
                "accepted_status": status in POSTFLIGHT_STATUSES and status != "REJECT",
                "sample_manifest": str(manifest_path) if manifest_path else None,
                "samples": str(samples_path) if samples_path else None,
                "bz_log": str(bz_log_path) if bz_log_path else None,
                "tile_sample_check": tile_sample_result,
                "postflight_check": {
                    "command": postflight["process"]["command"],
                    "returncode": postflight["process"]["returncode"],
                },
            }
        )

    normalized_path = out_dir / args.normalized_name
    normalizer_result = None
    if not args.no_normalize:
        normalizer_result = run_normalizer(normalize_script, profile_paths, report_paths, normalized_path)

    summary = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "out_dir": str(out_dir),
        "normalized_rows": str(normalized_path) if not args.no_normalize else None,
        "normalizer": normalizer_result,
        "records": records,
    }
    summary_path = args.report_out.expanduser().resolve() if args.report_out else out_dir / "postflight-summary.json"
    write_json(summary_path, summary)

    failed = False
    for record in records:
        if record["status"] == "REJECT":
            failed = True
        sample_result = record.get("tile_sample_check")
        if isinstance(sample_result, dict) and sample_result.get("returncode") != 0:
            failed = True
    if normalizer_result is not None and normalizer_result["returncode"] != 0:
        failed = True

    if args.fail_on_reject and failed:
        return 1
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build compact postflight bundles for campaign profiles, run the "
            "independent checkers, and emit report JSON plus normalized sweep rows."
        )
    )
    parser.add_argument("--profiles", nargs="+", type=Path, required=True)
    parser.add_argument("--sample-manifest", nargs="*", type=Path, default=[])
    parser.add_argument("--samples", nargs="*", type=Path, default=[])
    parser.add_argument("--span-cert", nargs="*", type=Path, default=[])
    parser.add_argument("--bz-log", nargs="*", type=Path, default=[])
    parser.add_argument("--log", nargs="*", type=Path, default=[])
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--report-out", type=Path)
    parser.add_argument("--row-class", choices=["accepted", "profile", "sweep", "audit"], default="sweep")
    parser.add_argument("--telemetry-level", choices=["none", "profile", "audit", "full"])
    parser.add_argument("--claim-proof-required", action="store_true")
    parser.add_argument("--tile-sample-check", type=Path)
    parser.add_argument("--postflight-check", type=Path)
    parser.add_argument("--normalize-sweep-rows", type=Path)
    parser.add_argument("--normalized-name", default="sweep_rows.normalized.jsonl")
    parser.add_argument("--no-normalize", action="store_true")
    parser.add_argument(
        "--fail-on-reject",
        action="store_true",
        help="Return non-zero if any checker rejects. Reports are still written.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return orchestrate(args)
    except PostflightError as exc:
        print(f"postflight_orchestrate: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
