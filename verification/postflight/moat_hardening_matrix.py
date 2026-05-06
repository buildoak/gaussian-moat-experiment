#!/usr/bin/env python3
"""Build and optionally run the K36 MOAT hardening matrix commands."""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


DEFAULT_CAMPAIGN_BIN = Path(
    "tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k36/campaign_main_cuda"
)
DEFAULT_POSTFLIGHT = Path("verification/postflight/postflight_orchestrate.py")
DEFAULT_R_INNER = 80_000_000
DEFAULT_WIDTHS = (17_000, 18_000, 19_000, 20_000)
ACCEPTANCE_K_SQ = 36
OPTIONAL_TELEMETRY_K = (37, 38, 39)


@dataclass(frozen=True)
class MatrixCase:
    k_sq: int
    r_inner: int
    width: int
    r_outer: int
    acceptance: bool
    run_dir: Path
    profile: Path
    sample_manifest: Path
    tile_samples: Path
    stdout_log: Path
    stderr_log: Path
    command_log: Path
    postflight_dir: Path


def case_label(k_sq: int, r_inner: int, width: int) -> str:
    return f"k{k_sq}_r{r_inner}_w{width}"


def matrix_cases(args: argparse.Namespace) -> list[MatrixCase]:
    cases: list[MatrixCase] = []
    k_values = [ACCEPTANCE_K_SQ, *args.telemetry_k_sq]
    for k_sq in k_values:
        for width in args.widths:
            label = case_label(k_sq, args.r_inner, width)
            run_dir = args.out_root / f"k{k_sq}" / f"r{args.r_inner}_w{width}"
            cases.append(
                MatrixCase(
                    k_sq=k_sq,
                    r_inner=args.r_inner,
                    width=width,
                    r_outer=args.r_inner + width,
                    acceptance=(k_sq == ACCEPTANCE_K_SQ),
                    run_dir=run_dir,
                    profile=run_dir / "profiles" / f"{label}.profile.json",
                    sample_manifest=run_dir / "samples" / f"{label}.sample-manifest.json",
                    tile_samples=run_dir / "samples" / f"{label}.tiles.jsonl",
                    stdout_log=run_dir / "logs" / f"{label}.stdout.log",
                    stderr_log=run_dir / "logs" / f"{label}.stderr.log",
                    command_log=run_dir / "logs" / f"{label}.commands.json",
                    postflight_dir=run_dir / "postflight",
                )
            )
    return cases


def campaign_command(args: argparse.Namespace, case: MatrixCase) -> list[str]:
    return [
        str(args.campaign_bin),
        "--k-sq",
        str(case.k_sq),
        "--r-inner",
        str(case.r_inner),
        "--r-outer",
        str(case.r_outer),
        "--region",
        args.region,
        "--chunk-size",
        str(args.chunk_size),
        "--no-early-exit",
        "--telemetry",
        args.telemetry,
        "--tile-sample-count",
        str(args.sample_count),
        "--sample-manifest",
        str(case.sample_manifest),
        "--tile-sample-out",
        str(case.tile_samples),
        "--profile",
        str(case.profile),
    ]


def postflight_command(args: argparse.Namespace, case: MatrixCase) -> list[str]:
    return [
        args.python,
        str(args.postflight_orchestrate),
        "--profiles",
        str(case.profile),
        "--sample-manifest",
        str(case.sample_manifest),
        "--samples",
        str(case.tile_samples),
        "--log",
        str(case.stdout_log),
        "--out-dir",
        str(case.postflight_dir),
        "--row-class",
        "audit" if case.acceptance else "sweep",
        "--telemetry-level",
        args.telemetry,
        "--fail-on-reject",
    ]


def shell_lines(args: argparse.Namespace, cases: list[MatrixCase]) -> list[str]:
    lines: list[str] = [
        "# K36 widths are the acceptance MOAT hardening matrix.",
        "# K37-K39, when requested, are optional boundary/BZ telemetry only.",
    ]
    for case in cases:
        campaign = campaign_command(args, case)
        postflight = postflight_command(args, case)
        dirs = [
            case.profile.parent,
            case.sample_manifest.parent,
            case.stdout_log.parent,
            case.postflight_dir,
        ]
        lines.append("")
        role = "acceptance" if case.acceptance else "telemetry-only"
        lines.append(f"# {case_label(case.k_sq, case.r_inner, case.width)} ({role})")
        lines.append(shlex.join(["mkdir", "-p", *[str(path) for path in dirs]]))
        lines.append(
            shlex.join(campaign)
            + " > "
            + shlex.quote(str(case.stdout_log))
            + " 2> "
            + shlex.quote(str(case.stderr_log))
        )
        lines.append(shlex.join(postflight))
    return lines


def write_command_log(
    path: Path, case: MatrixCase, campaign: list[str], postflight: list[str]
) -> None:
    payload = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "case": {
            "k_sq": case.k_sq,
            "r_inner": case.r_inner,
            "r_outer": case.r_outer,
            "width": case.width,
            "acceptance": case.acceptance,
        },
        "campaign_command": campaign,
        "postflight_command": postflight,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def run_case(args: argparse.Namespace, case: MatrixCase) -> int:
    campaign = campaign_command(args, case)
    postflight = postflight_command(args, case)
    for path in (case.profile.parent, case.sample_manifest.parent, case.stdout_log.parent, case.postflight_dir):
        path.mkdir(parents=True, exist_ok=True)
    write_command_log(case.command_log, case, campaign, postflight)

    with case.stdout_log.open("w") as stdout_fh, case.stderr_log.open("w") as stderr_fh:
        campaign_result = subprocess.run(campaign, stdout=stdout_fh, stderr=stderr_fh, check=False)
    if campaign_result.returncode != 0:
        return campaign_result.returncode

    postflight_result = subprocess.run(postflight, check=False)
    return postflight_result.returncode


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Prepare the K36 R_inner=80,000,000 MOAT hardening matrix. "
            "Dry-run prints shell commands and never invokes CUDA."
        )
    )
    parser.add_argument("--out-root", type=Path, required=True)
    parser.add_argument("--campaign-bin", type=Path, default=DEFAULT_CAMPAIGN_BIN)
    parser.add_argument("--postflight-orchestrate", type=Path, default=DEFAULT_POSTFLIGHT)
    parser.add_argument("--python", default="python3")
    parser.add_argument("--r-inner", type=int, default=DEFAULT_R_INNER)
    parser.add_argument("--widths", nargs="+", type=int, default=list(DEFAULT_WIDTHS))
    parser.add_argument("--chunk-size", type=int, default=200_000)
    parser.add_argument("--sample-count", type=int, default=512)
    parser.add_argument("--region", default="full-octant")
    parser.add_argument("--telemetry", choices=["audit", "full"], default="audit")
    parser.add_argument(
        "--telemetry-k-sq",
        nargs="*",
        type=int,
        choices=OPTIONAL_TELEMETRY_K,
        default=[],
        help="Optional K37-K39 telemetry rows. These are not acceptance rows.",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true", help="Print commands without executing.")
    mode.add_argument("--execute", action="store_true", help="Run the commands sequentially.")
    args = parser.parse_args(argv)

    if not args.widths:
        parser.error("--widths must contain at least one width")
    if args.sample_count <= 0:
        parser.error("--sample-count must be positive")
    if args.chunk_size <= 0:
        parser.error("--chunk-size must be positive")
    if args.r_inner <= 0:
        parser.error("--r-inner must be positive")
    if args.execute and not args.campaign_bin.exists():
        parser.error(f"--campaign-bin does not exist: {args.campaign_bin}")
    if args.execute and not args.postflight_orchestrate.exists():
        parser.error(f"--postflight-orchestrate does not exist: {args.postflight_orchestrate}")
    if not args.execute:
        args.dry_run = True
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    cases = matrix_cases(args)
    if args.dry_run:
        print("\n".join(shell_lines(args, cases)))
        return 0

    for case in cases:
        result = run_case(args, case)
        if result != 0:
            return result
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
