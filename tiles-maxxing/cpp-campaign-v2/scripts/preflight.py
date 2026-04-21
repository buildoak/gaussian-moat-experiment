#!/usr/bin/env python3
"""Reusable cpp-campaign-v2 preflight harness."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
ORACLE = ROOT / "goldens" / "preflight-oracle.py"
STATS = ROOT / "scripts" / "collect_stats.py"


def oracle_runner() -> list[str]:
    uv = shutil.which("uv")
    if uv:
        return [uv, "run", "--script", str(ORACLE)]
    return [sys.executable, str(ORACLE)]


def run(cmd: list[str], cwd: pathlib.Path = ROOT, stdout_path: pathlib.Path | None = None) -> str:
    print("+ " + " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    if stdout_path is not None:
        stdout_path.write_text(proc.stdout + proc.stderr)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise RuntimeError(f"command failed with exit {proc.returncode}: {' '.join(cmd)}")
    return proc.stdout + proc.stderr


def manifest_path_for(snapshot: pathlib.Path) -> pathlib.Path:
    name = snapshot.name
    if name.endswith(".snapshot.bin"):
        stem = name[: -len(".snapshot.bin")]
    elif name.endswith(".bin"):
        stem = name[: -len(".bin")]
    else:
        stem = snapshot.stem
    return snapshot.with_name(stem + ".manifest.json")


def ensure_build(k_sq: int, r_inner: int, r_outer: int, build_dir: pathlib.Path) -> None:
    if not (build_dir / "campaign_main").exists() or not (build_dir / "compare_snapshots").exists():
        cmake_cmd = [
            "cmake",
            f"-DK_SQ={k_sq}",
            f"-DBZ_R_INNER={r_inner}",
            f"-DBZ_R_OUTER={r_outer}",
            "-DCAMPAIGN_BUILD_TESTS=OFF",
            "-DCMAKE_BUILD_TYPE=Release",
            "-S",
            str(ROOT),
            "-B",
            str(build_dir),
        ]
        libomp = shutil.which("brew")
        if libomp:
            prefix = subprocess.run(
                ["brew", "--prefix", "libomp"],
                text=True,
                capture_output=True,
                check=False,
            ).stdout.strip()
            if prefix:
                cmake_cmd.extend([
                    f"-DOpenMP_CXX_FLAGS=-Xpreprocessor -fopenmp -I{prefix}/include",
                    f"-DOpenMP_CXX_LIB_NAMES=omp",
                    f"-DOpenMP_omp_LIBRARY={prefix}/lib/libomp.dylib",
                ])
        run(cmake_cmd)
        run(["cmake", "--build", str(build_dir), "-j"])


def total_seconds(output: str) -> float:
    match = re.search(r"total:\s+([0-9]+)\s+ms", output)
    if not match:
        raise RuntimeError("could not parse campaign_main total time")
    return int(match.group(1)) / 1000.0


def campaign(
    binary: pathlib.Path,
    k_sq: int,
    r_inner: int,
    r_outer: int,
    region: pathlib.Path,
    output: pathlib.Path,
    threads: int,
    log: pathlib.Path,
) -> float:
    text = run([
        str(binary),
        f"--k-sq={k_sq}",
        f"--r-inner={r_inner}",
        f"--r-outer={r_outer}",
        "--region",
        str(region),
        "--out",
        str(output),
        f"--threads={threads}",
    ], stdout_path=log)
    return total_seconds(text)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("k_sq", type=int)
    parser.add_argument("r_inner", type=int)
    parser.add_argument("r_outer", type=int)
    parser.add_argument("n_tiles", type=int)
    parser.add_argument("thread_count", type=int)
    parser.add_argument("output_dir", type=pathlib.Path)
    parser.add_argument("--tiles-spec", type=pathlib.Path)
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--skip-oracle", action="store_true")
    args = parser.parse_args()

    out = args.output_dir
    out.mkdir(parents=True, exist_ok=True)
    build_dir = args.build_dir or (ROOT / f"build-k{args.k_sq}")
    ensure_build(args.k_sq, args.r_inner, args.r_outer, build_dir)

    region = out / "region.json"
    oracle = out / "oracle.bin"
    oracle_cmd = [
        *oracle_runner(),
        "--k-sq",
        str(args.k_sq),
        "--r-inner",
        str(args.r_inner),
        "--r-outer",
        str(args.r_outer),
        "--n-tiles",
        str(args.n_tiles),
        "--output",
        str(oracle),
        "--region-output",
        str(region),
    ]
    if args.tiles_spec:
        oracle_cmd.extend(["--tiles-spec", str(args.tiles_spec)])
    if not args.skip_oracle:
        run(oracle_cmd, stdout_path=out / "oracle.log")
    elif not region.exists():
        run(oracle_cmd + ["--output", str(out / "_region_seed_oracle.bin")], stdout_path=out / "region.log")

    one_snapshot = out / "snapshot-1thread.bin"
    main_snapshot = out / "snapshot.bin"
    t1 = campaign(
        build_dir / "campaign_main",
        args.k_sq,
        args.r_inner,
        args.r_outer,
        region,
        one_snapshot,
        1,
        out / "campaign-1thread.log",
    )
    tn = campaign(
        build_dir / "campaign_main",
        args.k_sq,
        args.r_inner,
        args.r_outer,
        region,
        main_snapshot,
        args.thread_count,
        out / f"campaign-{args.thread_count}thread.log",
    )

    compare_out = out / "compare_result.txt"
    if not args.skip_oracle:
        run([str(build_dir / "compare_snapshots"), str(oracle), str(main_snapshot)], stdout_path=compare_out)

    run([
        sys.executable,
        str(STATS),
        "--snapshot",
        str(main_snapshot),
        "--manifest",
        str(manifest_path_for(main_snapshot)),
        "--output",
        str(out / "stats.json"),
        "--wall-time-1",
        str(t1),
        "--wall-time-n",
        str(tn),
        "--thread-count",
        str(args.thread_count),
    ])

    summary = {
        "k_sq": args.k_sq,
        "r_inner": args.r_inner,
        "r_outer": args.r_outer,
        "n_tiles": args.n_tiles,
        "thread_count": args.thread_count,
        "wall_time_1": t1,
        "wall_time_n": tn,
    }
    (out / "preflight-summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
