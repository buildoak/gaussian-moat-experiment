#!/usr/bin/env python3
"""Upper-bound Gaussian moat campaign runner."""

from __future__ import annotations

import argparse
import datetime
import json
import math
import os
import pathlib
import re
import subprocess
import sys
import time
from typing import Dict, Tuple

PLATFORM_PROFILES: Dict[str, Dict[str, object]] = {
    "jetson": {
        "sieve_bin": "./build/gm_cuda_primes",
        "solver_bin": "./solver/target/release/gaussian-moat-solver",
        "wedges": 6,
        "batch_size": 500000,
    },
    "4090": {
        "sieve_bin": "./build/gm_cuda_primes",
        "solver_bin": "./solver/target/release/gaussian-moat-solver",
        "wedges": 8,
        "batch_size": 500000,
    },
    "custom": {
        "sieve_bin": "./build/gm_cuda_primes",
        "solver_bin": "./solver/target/release/gaussian-moat-solver",
        "wedges": 6,
        "batch_size": 500000,
    },
}


def parse_distance(value: str, name: str) -> int:
    try:
        return int(float(value))
    except (TypeError, ValueError) as exc:
        raise argparse.ArgumentTypeError(f"invalid {name}: {value!r}") from exc


def iso_utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def safe_tag(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", value)


class UBCampaign:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.k_squared = int(args.k_squared)
        self.k_float = math.sqrt(self.k_squared)
        self.k_int = math.ceil(self.k_float)

        self.work_dir = pathlib.Path(args.work_dir)
        self.work_dir.mkdir(parents=True, exist_ok=True)

        self.results_jsonl = self.work_dir / f"ub_{safe_tag(args.tag)}.jsonl"

        self.total_probes = 0
        self.total_sieve_ms = 0
        self.total_solve_ms = 0
        self.global_iteration = 0

    def log(self, message: str, *, always: bool = False) -> None:
        if always or self.args.verbose:
            ts = datetime.datetime.now(datetime.timezone.utc).strftime("%H:%M:%S")
            print(f"[{ts}] {message}", file=sys.stderr)

    def _next_iteration(self) -> int:
        self.global_iteration += 1
        return self.global_iteration

    def _append_record(self, record: Dict[str, object]) -> None:
        with self.results_jsonl.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(record, separators=(",", ":")) + "\n")

        self.total_probes += 1
        self.total_sieve_ms += int(record.get("sieve_ms", 0))
        self.total_solve_ms += int(record.get("solve_ms", 0))

    @staticmethod
    def _count_primes(file_path: pathlib.Path) -> int:
        if not file_path.exists():
            return 0
        size = file_path.stat().st_size
        if size < 64:
            return 0
        return max(0, (size - 64) // 16)

    @staticmethod
    def _parse_solver_output(text: str) -> Dict[str, object]:
        parsed: Dict[str, object] = {
            "moat_found": "MOAT_FOUND" in text,
            "farthest_point": "(0,0)",
            "farthest_distance": 0.0,
            "farthest_norm": 0,
            "component_size": 0,
            "primes_processed": 0,
        }

        m = re.search(r"farthest point:\s*\((-?\d+)\s*,\s*(-?\d+)\)", text)
        if m:
            parsed["farthest_point"] = f"({m.group(1)},{m.group(2)})"

        m = re.search(r"farthest distance:\s*([0-9]+(?:\.[0-9]+)?)", text)
        if m:
            parsed["farthest_distance"] = float(m.group(1))

        m = re.search(r"farthest_norm=(\d+)", text)
        if m:
            parsed["farthest_norm"] = int(m.group(1))

        m = re.search(r"origin component size:\s*(\d+)", text)
        if m:
            parsed["component_size"] = int(m.group(1))

        m = re.search(r"primes processed:\s*(\d+)", text)
        if m:
            parsed["primes_processed"] = int(m.group(1))

        m = re.search(
            r"RESULT\s+farthest_norm=(\d+)\s+farthest_point=\((-?\d+),(-?\d+)\)\s+"
            r"component_size=(\d+)\s+primes_processed=(\d+)",
            text,
        )
        if m:
            parsed["farthest_norm"] = int(m.group(1))
            parsed["farthest_point"] = f"({m.group(2)},{m.group(3)})"
            parsed["component_size"] = int(m.group(4))
            parsed["primes_processed"] = int(m.group(5))

        return parsed

    @staticmethod
    def _classify_moat(parsed: Dict[str, object], norm_hi: int) -> str:
        if bool(parsed.get("moat_found", False)):
            return "moat_confirmed"

        component_size = int(parsed.get("component_size", 0))
        primes_processed = int(parsed.get("primes_processed", 0))

        if component_size == 0:
            return "moat_confirmed"

        if component_size > 0 and component_size < primes_processed:
            farthest_dist_int = int(float(parsed.get("farthest_distance", 0.0)))
            file_edge_dist = int(math.sqrt(norm_hi))
            if farthest_dist_int < 0.9 * file_edge_dist:
                return "moat_confirmed"

        return "inconclusive_edge"

    def _compute_norms(self, distance: int, shell_width: int) -> Tuple[int, int, int]:
        norm_lo = max(0, (distance - self.k_int) ** 2)
        band_width = int(4 * distance * self.k_float)
        norm_hi = norm_lo + band_width * shell_width
        return norm_lo, norm_hi, band_width

    def _safe_shell_width(self, distance: int, requested_shell: int) -> int:
        """Reduce shell_width if the estimated prime count would exceed a safe
        limit for this platform. Calibrated: Jetson safely handles ~131M primes
        (~2GB GPRF). We cap at 150M estimated primes as the safety ceiling.
        If requested shell produces more, we reduce to fit."""
        max_primes = 150_000_000  # safe ceiling for Jetson
        band_width = int(4 * distance * self.k_float)
        if band_width <= 0:
            return requested_shell

        for sw in range(requested_shell, 0, -1):
            norm_lo = max(0, (distance - self.k_int) ** 2)
            norm_hi = norm_lo + band_width * sw
            est = self._estimate_prime_count(norm_lo, norm_hi)
            if est <= max_primes:
                if sw < requested_shell:
                    self.log(
                        f"  shell_width reduced {requested_shell}->{sw} at D={distance} "
                        f"(est {est/1e6:.0f}M primes, cap {max_primes/1e6:.0f}M)",
                        always=True,
                    )
                return sw

        # Even shell_width=1 exceeds limit; use it anyway with a warning
        self.log(
            f"  WARNING: even shell_width=1 exceeds prime cap at D={distance}",
            always=True,
        )
        return 1

    @staticmethod
    def _estimate_prime_count(norm_lo: int, norm_hi: int) -> float:
        span = max(0, norm_hi - norm_lo)
        denom = 2.0 * math.log(max(norm_hi, 10))
        if denom <= 0:
            return 0.0
        return span / denom

    def _estimate_probe_timeout_s(self, norm_lo: int, norm_hi: int) -> int:
        """Estimate per-command timeout. Calibrated from Jetson runs:
        131M primes ~90s sieve, ~100s solve. Use 500K primes/sec as
        conservative floor. Timeout applies per command (sieve or solver),
        so estimate for the slower of the two. Cap at 3600s (1 hour)."""
        num_primes_estimate = self._estimate_prime_count(norm_lo, norm_hi)
        # 500K primes/sec conservative estimate, 2x safety margin
        estimate_s = (num_primes_estimate / 500_000.0) * 2.0
        return int(math.ceil(max(300.0, min(3600.0, estimate_s))))

    def _run_cmd(self, cmd: list[str], timeout_s: int) -> str:
        cmd_text = " ".join(cmd)
        if self.args.verbose or self.args.dry_run:
            self.log(f"CMD (timeout={timeout_s}s): {cmd_text}")

        if self.args.dry_run:
            return ""

        try:
            completed = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=max(1, int(timeout_s)),
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"timeout after {timeout_s}s: {cmd_text}") from exc

        if completed.returncode != 0:
            tail = ((completed.stdout or "") + "\n" + (completed.stderr or "")).strip()
            if len(tail) > 4000:
                tail = tail[-4000:]
            raise RuntimeError(f"command failed (exit {completed.returncode}): {cmd_text}\n{tail}")

        return (completed.stdout or "") + (completed.stderr or "")

    def _probe(self, phase: str, start_distance: int, shell_width: int) -> Dict[str, object]:
        iteration = self._next_iteration()
        shell_width = self._safe_shell_width(start_distance, shell_width)
        norm_lo, norm_hi, band_width = self._compute_norms(start_distance, shell_width)

        timeout_s = self._estimate_probe_timeout_s(norm_lo, norm_hi)

        gprf_file = self.work_dir / f"ub_{safe_tag(self.args.tag)}_{phase}_it{iteration}.gprf"

        sieve_ms = 0
        solve_ms = 0
        file_prime_count = 0
        parsed: Dict[str, object] = {
            "moat_found": False,
            "farthest_point": "(0,0)",
            "farthest_distance": float(start_distance),
            "farthest_norm": 0,
            "component_size": 0,
            "primes_processed": 0,
        }
        status = "inconclusive_edge"
        error_text = ""

        sieve_cmd = [
            self.args.sieve_bin,
            "--norm-lo",
            str(norm_lo),
            "--norm-hi",
            str(norm_hi),
            "--output",
            str(gprf_file),
            "--mode",
            "sieve",
            "--k-squared",
            str(self.k_squared),
        ]

        solver_cmd = [
            self.args.solver_bin,
            "--k-squared",
            str(self.k_squared),
            "--start-distance",
            str(start_distance),
            "--angular",
            str(self.args.wedges),
            "--prime-file",
            str(gprf_file),
            "--batch-size",
            str(self.args.batch_size),
            "--profile",
        ]

        self.log(
            f"[{phase}] it={iteration} start={start_distance} shell={shell_width} "
            f"norm=[{norm_lo},{norm_hi}) timeout={timeout_s}s",
            always=True,
        )

        if self.args.dry_run:
            self._run_cmd(sieve_cmd, timeout_s)
            self._run_cmd(solver_cmd, timeout_s)
            parsed["component_size"] = 1
            status = "inconclusive_edge"
        else:
            try:
                t0 = time.monotonic()
                self._run_cmd(sieve_cmd, timeout_s)
                sieve_ms = int((time.monotonic() - t0) * 1000)

                file_prime_count = self._count_primes(gprf_file)

                t1 = time.monotonic()
                solver_output = self._run_cmd(solver_cmd, timeout_s)
                solve_ms = int((time.monotonic() - t1) * 1000)

                parsed = self._parse_solver_output(solver_output)
                status = self._classify_moat(parsed, norm_hi)

                self.log(
                    f"  sieve={sieve_ms}ms solve={solve_ms}ms primes={file_prime_count} "
                    f"farthest={parsed.get('farthest_distance', 0):.1f} "
                    f"comp={parsed.get('component_size', 0)}/{parsed.get('primes_processed', 0)} "
                    f"status={status}",
                    always=True,
                )

            except Exception as exc:
                status = "error"
                error_text = str(exc)
                self.log(f"[{phase}] it={iteration} ERROR: {error_text}", always=True)
            finally:
                if gprf_file.exists():
                    try:
                        gprf_file.unlink()
                    except OSError:
                        pass

        num_primes = int(parsed.get("primes_processed", 0))
        if num_primes <= 0:
            num_primes = int(file_prime_count)

        record: Dict[str, object] = {
            "timestamp": iso_utc_now(),
            "phase": phase,
            "iteration": iteration,
            "start_distance": int(start_distance),
            "shell_width": int(shell_width),
            "norm_lo": int(norm_lo),
            "norm_hi": int(norm_hi),
            "num_primes": int(num_primes),
            "farthest_point": str(parsed.get("farthest_point", "(0,0)")),
            "farthest_norm": int(parsed.get("farthest_norm", 0)),
            "farthest_distance": float(parsed.get("farthest_distance", 0.0)),
            "component_size": int(parsed.get("component_size", 0)),
            "sieve_ms": int(sieve_ms),
            "solve_ms": int(solve_ms),
            "status": status,
            "tag": self.args.tag,
            "k_squared": self.k_squared,
            "wedges": int(self.args.wedges),
            "band_width": int(band_width),
            "action": "",
            "next_start_distance": int(start_distance),
            "next_shell_width": int(shell_width),
        }

        if error_text:
            record["error"] = error_text

        return record

    def _advance_distance(self, current_distance: int, farthest_distance: float, band_width: int, shell_width: int) -> int:
        farthest_int = int(farthest_distance)
        if farthest_int > current_distance:
            new_dist = farthest_int - (self.k_int * 2)
            return max(1, new_dist)

        denom = max(current_distance, 1)
        step = max(10, int((band_width * shell_width) / (2 * denom)))
        return current_distance + step

    def _forward_walk(self, phase: str, start_distance: int, max_iters: int) -> Dict[str, object]:
        """Walk forward from start_distance, advancing the sieve window on each
        iteration. Returns when moat is confirmed or max_iters exhausted.
        This is the core workhorse used by both progressive and bisect modes."""
        current_distance = int(start_distance)
        shell = int(self.args.shell_width)
        last_farthest = 0.0

        for step in range(max_iters):
            if current_distance > self.args.ceiling:
                break

            record = self._probe(phase, current_distance, shell)
            status = str(record["status"])
            farthest = float(record.get("farthest_distance", 0.0))
            last_farthest = max(last_farthest, farthest)

            if status == "moat_confirmed":
                self.log(
                    f"MOAT CONFIRMED at D={current_distance} "
                    f"farthest={record.get('farthest_distance', 0)}",
                    always=True,
                )
                record["action"] = "stop_moat"
                record["next_start_distance"] = int(current_distance)
                record["next_shell_width"] = int(shell)
                self._append_record(record)
                return {
                    "moat_found": True,
                    "boundary": int(current_distance),
                    "farthest": last_farthest,
                    "iterations": step + 1,
                }

            if status == "error":
                next_distance = current_distance + 10
                record["action"] = "advance_after_error"
            else:
                next_distance = self._advance_distance(
                    current_distance=current_distance,
                    farthest_distance=farthest,
                    band_width=int(record["band_width"]),
                    shell_width=shell,
                )
                if int(farthest) > current_distance:
                    record["action"] = "advance_from_farthest"
                else:
                    record["action"] = "advance_by_step"

            record["next_start_distance"] = int(next_distance)
            record["next_shell_width"] = int(shell)
            self._append_record(record)

            current_distance = int(next_distance)

        return {
            "moat_found": False,
            "boundary": int(min(current_distance, self.args.ceiling)),
            "farthest": last_farthest,
            "iterations": max_iters,
        }

    def run_progressive(self) -> Dict[str, object]:
        result = self._forward_walk(
            "progressive",
            int(self.args.start_distance),
            int(self.args.max_iterations),
        )
        if result["moat_found"]:
            return {"result": "moat_found", "boundary": result["boundary"]}
        return {"result": "no_moat_in_range", "boundary": result["boundary"]}

    def run_bisect(self) -> Dict[str, object]:
        shell = int(self.args.shell_width)
        walk_iters = int(self.args.max_iterations)

        lo = None
        hi = None
        prev = int(self.args.start_distance)
        dist = prev

        # Phase 1: LOCATE -- geometric doubling
        self.log("--- Phase 1: LOCATE (geometric doubling) ---", always=True)
        while dist <= self.args.ceiling:
            self.log(f"LOCATE: trying D={dist}", always=True)
            result = self._forward_walk("locate", dist, walk_iters)

            if result["moat_found"]:
                hi = dist
                lo = prev if prev < dist else max(1, dist // 2)
                self.log(f"LOCATE: bracket found [{lo}, {hi}]", always=True)
                break

            prev = dist
            lo = dist
            dist *= 2

        if hi is None or lo is None:
            return {
                "result": "no_moat_in_range",
                "boundary": int(min(dist, self.args.ceiling)),
            }

        # Phase 2: REFINE -- binary search
        self.log(f"--- Phase 2: REFINE [{lo}, {hi}] ---", always=True)
        refine_steps = 0
        while (hi - lo) > int(self.args.bisect_tolerance) and refine_steps < walk_iters:
            mid = (lo + hi) // 2
            self.log(f"REFINE: trying D={mid} range=[{lo},{hi}]", always=True)
            result = self._forward_walk("refine", mid, walk_iters)

            if result["moat_found"]:
                hi = mid
            else:
                lo = mid
            refine_steps += 1

        boundary = int(hi)
        self.log(f"REFINE: converged to boundary={boundary}", always=True)

        # Phase 3: VERIFY -- wide shell probe
        verify_shell_saved = self.args.shell_width
        self.args.shell_width = shell * 3
        self.log(f"--- Phase 3: VERIFY at D={boundary} shell={self.args.shell_width} ---", always=True)
        verify = self._forward_walk("verify", boundary, walk_iters)
        self.args.shell_width = verify_shell_saved

        if verify["moat_found"]:
            return {"result": "boundary_found", "boundary": boundary}

        return {"result": "boundary_inconclusive", "boundary": boundary}

    def run(self) -> Dict[str, object]:
        if not self.args.dry_run:
            for exe in (self.args.sieve_bin, self.args.solver_bin):
                path = pathlib.Path(exe)
                if not (path.exists() and os.access(path, os.X_OK)):
                    raise RuntimeError(f"binary not executable: {exe}")

        self.log(f"=== UB Campaign: k^2={self.k_squared} tag={self.args.tag} mode={self.args.mode} ===", always=True)
        self.log(f"  platform={self.args.platform} sieve={self.args.sieve_bin} solver={self.args.solver_bin}", always=True)
        self.log(f"  start={self.args.start_distance} ceiling={self.args.ceiling} shell={self.args.shell_width} wedges={self.args.wedges}", always=True)
        self.log(f"  results={self.results_jsonl}", always=True)

        if self.args.mode == "progressive":
            outcome = self.run_progressive()
        else:
            outcome = self.run_bisect()

        print("=== SUMMARY ===")
        print(f"total probes: {self.total_probes}")
        print(f"total sieve time (s): {self.total_sieve_ms / 1000.0:.3f}")
        print(f"total solve time (s): {self.total_solve_ms / 1000.0:.3f}")
        print(f"result: {outcome['result']}")
        print(f"boundary: {outcome['boundary']}")

        return outcome


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Upper-bound Gaussian moat campaign")
    parser.add_argument("--k-squared", required=True, type=int)
    parser.add_argument("--start-distance", required=True, type=lambda v: parse_distance(v, "start-distance"))
    parser.add_argument(
        "--ceiling",
        type=lambda v: parse_distance(v, "ceiling"),
        default=None,
        help="Max distance ceiling (default: 10x start-distance)",
    )
    parser.add_argument("--shell-width", type=int, default=5)
    parser.add_argument("--work-dir", default="/tmp/gm-ub")
    parser.add_argument("--mode", choices=["progressive", "bisect"], default="progressive")
    parser.add_argument("--wedges", type=int, default=None)
    parser.add_argument("--max-iterations", type=int, default=200)
    parser.add_argument("--bisect-tolerance", type=int, default=1_000_000)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--verbose", action="store_true")

    parser.add_argument("--tag", default=None)
    parser.add_argument("--platform", choices=["jetson", "4090", "custom"], default="custom")
    parser.add_argument("--sieve-bin", default=None)
    parser.add_argument("--solver-bin", default=None)
    parser.add_argument("--batch-size", type=int, default=500000)

    return parser


def resolve_platform_defaults(args: argparse.Namespace) -> None:
    profile = PLATFORM_PROFILES[args.platform]

    if args.sieve_bin is None:
        args.sieve_bin = str(profile["sieve_bin"])
    if args.solver_bin is None:
        args.solver_bin = str(profile["solver_bin"])
    if args.wedges is None:
        args.wedges = int(profile["wedges"])
    if args.batch_size is None:
        args.batch_size = int(profile["batch_size"])


def validate_args(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if args.ceiling is None:
        args.ceiling = max(args.start_distance * 10, args.start_distance)

    if args.tag is None:
        args.tag = f"k{args.k_squared}-campaign"

    if args.k_squared <= 0:
        parser.error("--k-squared must be > 0")
    if args.start_distance < 0:
        parser.error("--start-distance must be >= 0")
    if args.ceiling < args.start_distance:
        parser.error("--ceiling must be >= --start-distance")
    if args.shell_width <= 0:
        parser.error("--shell-width must be > 0")
    if args.wedges <= 0:
        parser.error("--wedges must be > 0")
    if args.max_iterations <= 0:
        parser.error("--max-iterations must be > 0")
    if args.batch_size <= 0:
        parser.error("--batch-size must be > 0")


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    resolve_platform_defaults(args)
    validate_args(parser, args)

    campaign = UBCampaign(args)
    campaign.run()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        sys.exit(130)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
