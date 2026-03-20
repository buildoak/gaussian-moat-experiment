#!/usr/bin/env python3
"""
tile-sweep.py — Parameter stability sweep for Gaussian moat tile-probe.

Sweeps tile_depth × strip_width × num_strips and records where band_io=0
first appears, to check stability against Tsuchimura's known moat at R~1,015,639.

Usage:
    python3 deploy/tile-sweep.py --dry-run
    python3 deploy/tile-sweep.py --tile-depths 2000 --strip-widths 240 --num-strips 64
    python3 deploy/tile-sweep.py --parallel 4
"""

import argparse
import csv
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime
from itertools import product
from pathlib import Path

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
DEFAULT_TILE_DEPTHS  = [250, 500, 1000, 2000, 4000]
DEFAULT_STRIP_WIDTHS = [120, 240, 480, 960]
DEFAULT_NUM_STRIPS   = [32, 64, 128, 256]
DEFAULT_K_SQUARED    = 26
DEFAULT_R_MIN        = 950_000
DEFAULT_R_MAX        = 1_050_000
DEFAULT_BINARY       = "./target/release/tile-probe"
DEFAULT_TSUCHIMURA   = 1_015_639
DEFAULT_TIMEOUT_SEC  = 300  # 5 minutes per run


# ---------------------------------------------------------------------------
# Signal / interrupt handling
# ---------------------------------------------------------------------------
_interrupted = False

def _handle_sigint(sig, frame):
    global _interrupted
    print("\n[INTERRUPTED] Ctrl+C caught — will save partial CSV and exit cleanly.", flush=True)
    _interrupted = True


# ---------------------------------------------------------------------------
# Core runner
# ---------------------------------------------------------------------------

def run_config(binary: str, k_sq: int, r_min: int, r_max: int,
               num_strips: int, strip_width: int, tile_depth: int,
               timeout: int) -> dict:
    """
    Run tile-probe for one config.  Returns a result dict.
    Uses --json-trace for structured shell data; stdout is discarded.
    """
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
        trace_path = tf.name

    cmd = [
        binary,
        "--k-squared",   str(k_sq),
        "--r-min",       str(r_min),
        "--r-max",       str(r_max),
        "--num-strips",  str(num_strips),
        "--strip-width", str(strip_width),
        "--tile-depth",  str(tile_depth),
        "--json-trace",  trace_path,
    ]

    t0 = time.monotonic()
    result = {
        "tile_depth":             tile_depth,
        "strip_width":            strip_width,
        "num_strips":             num_strips,
        "k_squared":              k_sq,
        "r_min":                  r_min,
        "r_max":                  r_max,
        "first_band_io_zero_R":   None,
        "first_acc_io_zero_R":    None,
        "band_io_zero_count":     0,
        "total_shells":           0,
        "wall_seconds":           0.0,
        "status":                 "ok",
        "_shells":                [],   # full profiles, stripped later
    }

    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        wall = time.monotonic() - t0
        result["wall_seconds"] = round(wall, 3)

        if proc.returncode != 0:
            result["status"] = f"exit_{proc.returncode}"
            stderr_snippet = proc.stderr.strip()[:200]
            result["_error"] = stderr_snippet
            return result

        # Parse JSON trace
        try:
            with open(trace_path, "r") as f:
                trace = json.load(f)
        except (json.JSONDecodeError, FileNotFoundError) as exc:
            result["status"] = f"json_error:{exc}"
            return result

        shells = trace.get("shells", [])
        result["total_shells"] = len(shells)
        result["_shells"] = shells

        band_zero_count = 0
        for sh in shells:
            r = sh.get("r_center")
            bio = sh.get("band_io", -1)
            aio = sh.get("acc_io", -1)

            if bio == 0 and result["first_band_io_zero_R"] is None:
                result["first_band_io_zero_R"] = r
            if bio == 0:
                band_zero_count += 1
            if aio == 0 and result["first_acc_io_zero_R"] is None:
                result["first_acc_io_zero_R"] = r

        result["band_io_zero_count"] = band_zero_count

    except subprocess.TimeoutExpired:
        result["wall_seconds"] = round(time.monotonic() - t0, 3)
        result["status"] = "timeout"
    except FileNotFoundError:
        result["wall_seconds"] = round(time.monotonic() - t0, 3)
        result["status"] = "binary_not_found"
    except Exception as exc:
        result["wall_seconds"] = round(time.monotonic() - t0, 3)
        result["status"] = f"error:{exc}"
    finally:
        try:
            os.unlink(trace_path)
        except OSError:
            pass

    return result


# ---------------------------------------------------------------------------
# CSV helpers
# ---------------------------------------------------------------------------

CSV_COLUMNS = [
    "tile_depth", "strip_width", "num_strips", "k_squared",
    "r_min", "r_max",
    "first_band_io_zero_R", "first_acc_io_zero_R",
    "band_io_zero_count", "total_shells",
    "wall_seconds", "status",
]


def _load_completed(csv_path: Path) -> set:
    """Return set of (tile_depth, strip_width, num_strips) already done."""
    done = set()
    if not csv_path.exists():
        return done
    try:
        with open(csv_path, newline="") as f:
            reader = csv.DictReader(f)
            for row in reader:
                key = (int(row["tile_depth"]), int(row["strip_width"]), int(row["num_strips"]))
                done.add(key)
    except Exception:
        pass
    return done


def _append_csv(csv_path: Path, rows: list[dict]):
    write_header = not csv_path.exists() or csv_path.stat().st_size == 0
    with open(csv_path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_COLUMNS, extrasaction="ignore")
        if write_header:
            writer.writeheader()
        for row in rows:
            writer.writerow(row)


# ---------------------------------------------------------------------------
# Parallel runner
# ---------------------------------------------------------------------------

def run_parallel(configs: list[dict], binary: str, timeout: int,
                 tsuchimura: int, total: int, offset: int,
                 parallel: int) -> list[dict]:
    """
    Run configs with up to `parallel` workers using concurrent.futures
    (stdlib — available since Python 3.2).
    """
    from concurrent.futures import ThreadPoolExecutor, as_completed

    results = []
    with ThreadPoolExecutor(max_workers=parallel) as executor:
        future_to_cfg = {
            executor.submit(
                run_config,
                binary,
                cfg["k_squared"], cfg["r_min"], cfg["r_max"],
                cfg["num_strips"], cfg["strip_width"], cfg["tile_depth"],
                timeout,
            ): (i + offset + 1, cfg)
            for i, cfg in enumerate(configs)
        }

        for future in as_completed(future_to_cfg):
            idx, cfg = future_to_cfg[future]
            if _interrupted:
                break
            try:
                res = future.result()
            except Exception as exc:
                res = {**cfg, "status": f"future_error:{exc}",
                       "wall_seconds": 0.0, "first_band_io_zero_R": None,
                       "first_acc_io_zero_R": None, "band_io_zero_count": 0,
                       "total_shells": 0, "_shells": []}

            _print_progress(idx, total, cfg, res, tsuchimura)
            results.append(res)

    return results


def _print_progress(idx: int, total: int, cfg: dict, res: dict, tsuchimura: int):
    bio_r  = res.get("first_band_io_zero_R")
    gap    = int(bio_r - tsuchimura) if bio_r is not None else None
    gap_s  = f"gap={gap:+d}" if gap is not None else "gap=N/A"
    bio_s  = f"band_io_zero={int(bio_r)}" if bio_r is not None else "band_io_zero=none"
    status = res.get("status", "?")
    wall   = res.get("wall_seconds", 0)
    print(
        f"[{idx}/{total}] depth={cfg['tile_depth']} width={cfg['strip_width']} "
        f"strips={cfg['num_strips']} ... {bio_s} {gap_s} ({wall:.1f}s) [{status}]",
        flush=True,
    )


# ---------------------------------------------------------------------------
# Summary table
# ---------------------------------------------------------------------------

def _print_summary(results: list[dict], tsuchimura: int):
    print("\n" + "=" * 78)
    print(f"{'tile_depth':>10} {'strip_width':>11} {'num_strips':>10} "
          f"{'band_io=0 R':>13} {'gap':>8} {'acc_io=0 R':>12} {'shells':>7} {'sec':>6} {'status':}")
    print("-" * 78)
    for r in sorted(results, key=lambda x: (x["tile_depth"], x["strip_width"], x["num_strips"])):
        bio_r = r.get("first_band_io_zero_R")
        aio_r = r.get("first_acc_io_zero_R")
        gap   = int(bio_r - tsuchimura) if bio_r is not None else None
        print(
            f"{r['tile_depth']:>10} {r['strip_width']:>11} {r['num_strips']:>10} "
            f"{int(bio_r) if bio_r else 'none':>13} "
            f"{gap if gap is not None else '':>8} "
            f"{int(aio_r) if aio_r else 'none':>12} "
            f"{r.get('total_shells', 0):>7} "
            f"{r.get('wall_seconds', 0):>6.1f} "
            f"{r.get('status', '?')}"
        )
    print("=" * 78)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_int_list(s: str) -> list[int]:
    return [int(x.strip()) for x in s.split(",") if x.strip()]


def main():
    global _interrupted
    signal.signal(signal.SIGINT, _handle_sigint)

    parser = argparse.ArgumentParser(
        description="Parameter stability sweep for tile-probe Gaussian moat detection."
    )
    parser.add_argument("--k-squared",    type=int, default=DEFAULT_K_SQUARED)
    parser.add_argument("--r-min",        type=int, default=DEFAULT_R_MIN)
    parser.add_argument("--r-max",        type=int, default=DEFAULT_R_MAX)
    parser.add_argument("--tile-depths",  type=str,
                        default=",".join(str(x) for x in DEFAULT_TILE_DEPTHS))
    parser.add_argument("--strip-widths", type=str,
                        default=",".join(str(x) for x in DEFAULT_STRIP_WIDTHS))
    parser.add_argument("--num-strips",   type=str,
                        default=",".join(str(x) for x in DEFAULT_NUM_STRIPS))
    parser.add_argument("--binary",       type=str, default=DEFAULT_BINARY)
    parser.add_argument("--dry-run",      action="store_true",
                        help="Print configs without running")
    parser.add_argument("--resume",       action="store_true",
                        help="Skip configs already in the CSV")
    parser.add_argument("--parallel",     type=int, default=1,
                        help="Run N configs in parallel (default: 1)")
    parser.add_argument("--tsuchimura",   type=int, default=DEFAULT_TSUCHIMURA,
                        help="Known moat R for gap calculation")
    parser.add_argument("--timeout",      type=int, default=DEFAULT_TIMEOUT_SEC,
                        help="Per-run timeout in seconds (default: 300)")

    args = parser.parse_args()

    tile_depths  = parse_int_list(args.tile_depths)
    strip_widths = parse_int_list(args.strip_widths)
    num_strips   = parse_int_list(args.num_strips)

    # Build full config matrix
    configs = [
        {
            "tile_depth":  td,
            "strip_width": sw,
            "num_strips":  ns,
            "k_squared":   args.k_squared,
            "r_min":       args.r_min,
            "r_max":       args.r_max,
        }
        for td, sw, ns in product(tile_depths, strip_widths, num_strips)
    ]

    total = len(configs)

    # Output paths
    repo_root = Path(__file__).resolve().parent.parent
    results_dir = repo_root / "research" / "results"
    results_dir.mkdir(parents=True, exist_ok=True)

    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    csv_path     = results_dir / f"tile-sweep-k{args.k_squared}-{ts}.csv"
    profile_path = results_dir / f"tile-sweep-k{args.k_squared}-{ts}-profiles.json"

    # Dry run
    if args.dry_run:
        print(f"DRY RUN — {total} configs")
        print(f"  k²={args.k_squared}  r=[{args.r_min:,}, {args.r_max:,}]")
        print(f"  tile_depths:  {tile_depths}")
        print(f"  strip_widths: {strip_widths}")
        print(f"  num_strips:   {num_strips}")
        print(f"  binary:       {args.binary}")
        print(f"  CSV output:   {csv_path}")
        print()
        print(f"{'#':>4} {'tile_depth':>10} {'strip_width':>11} {'num_strips':>10}")
        print("-" * 40)
        for i, cfg in enumerate(configs, 1):
            print(f"{i:>4} {cfg['tile_depth']:>10} {cfg['strip_width']:>11} {cfg['num_strips']:>10}")
        return

    # Resume: filter already-done configs
    completed = set()
    if args.resume:
        # Find most recent CSV with same k_squared if no explicit path
        existing = sorted(results_dir.glob(f"tile-sweep-k{args.k_squared}-*.csv"))
        if existing:
            resume_csv = existing[-1]
            completed = _load_completed(resume_csv)
            csv_path = resume_csv  # append to existing file
            print(f"[RESUME] Found {len(completed)} completed configs in {resume_csv.name}")

    pending = [
        cfg for cfg in configs
        if (cfg["tile_depth"], cfg["strip_width"], cfg["num_strips"]) not in completed
    ]
    skipped = total - len(pending)
    if skipped:
        print(f"[SKIP] {skipped}/{total} configs already done — running {len(pending)} remaining")

    if not pending:
        print("All configs already complete.")
        return

    print(f"Starting sweep: {len(pending)} configs  "
          f"(k²={args.k_squared}, r=[{args.r_min:,},{args.r_max:,}], "
          f"parallel={args.parallel}, timeout={args.timeout}s)")
    print(f"CSV  → {csv_path}")
    print(f"JSON → {profile_path}")
    print()

    all_results: list[dict] = []
    profiles: list[dict] = []

    def _flush(batch: list[dict]):
        """Save a batch to CSV and accumulate profiles."""
        _append_csv(csv_path, batch)
        for r in batch:
            shells = r.pop("_shells", [])
            profiles.append({
                "tile_depth":  r["tile_depth"],
                "strip_width": r["strip_width"],
                "num_strips":  r["num_strips"],
                "k_squared":   r["k_squared"],
                "r_min":       r["r_min"],
                "r_max":       r["r_max"],
                "shells":      shells,
            })

    try:
        if args.parallel <= 1:
            # Sequential
            for i, cfg in enumerate(pending, skipped + 1):
                if _interrupted:
                    break
                res = run_config(
                    args.binary,
                    cfg["k_squared"], cfg["r_min"], cfg["r_max"],
                    cfg["num_strips"], cfg["strip_width"], cfg["tile_depth"],
                    args.timeout,
                )
                _print_progress(i, total, cfg, res, args.tsuchimura)
                all_results.append(res)
                _flush([res])
        else:
            # Parallel — batch by parallel width so we flush periodically
            batch_size = args.parallel
            for batch_start in range(0, len(pending), batch_size):
                if _interrupted:
                    break
                batch_cfgs = pending[batch_start: batch_start + batch_size]
                offset = skipped + batch_start
                batch_results = run_parallel(
                    batch_cfgs, args.binary, args.timeout,
                    args.tsuchimura, total, offset, args.parallel,
                )
                all_results.extend(batch_results)
                _flush(batch_results)

    except KeyboardInterrupt:
        _interrupted = True

    finally:
        # Save profiles JSON (partial on interrupt)
        try:
            with open(profile_path, "w") as f:
                json.dump(profiles, f, indent=2)
        except Exception as exc:
            print(f"[WARN] Could not write profiles JSON: {exc}", file=sys.stderr)

    # Summary
    if all_results:
        _print_summary(all_results, args.tsuchimura)

    ran    = len(all_results)
    errors = sum(1 for r in all_results if r.get("status") not in ("ok",))
    print(f"\nDone: {ran} configs run, {errors} errors/timeouts.")
    print(f"CSV  → {csv_path}")
    print(f"JSON → {profile_path}")

    if _interrupted:
        print("[PARTIAL] Sweep interrupted — partial results saved.")
        sys.exit(1)


if __name__ == "__main__":
    main()
