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
from typing import Dict, List, Tuple

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
            "farthest_a": 0,
            "farthest_b": 0,
            "farthest_distance": 0.0,
            "farthest_norm": 0,
            "component_size": 0,
            "primes_processed": 0,
        }

        m = re.search(r"farthest point:\s*\((-?\d+)\s*,\s*(-?\d+)\)", text)
        if m:
            parsed["farthest_point"] = f"({m.group(1)},{m.group(2)})"
            parsed["farthest_a"] = int(m.group(1))
            parsed["farthest_b"] = int(m.group(2))

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
            parsed["farthest_a"] = int(m.group(2))
            parsed["farthest_b"] = int(m.group(3))
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
            farthest_norm = int(parsed.get("farthest_norm", 0))
            farthest_dist_int = int(float(parsed.get("farthest_distance", 0.0)))
            file_edge_dist = int(math.sqrt(norm_hi))

            # Primary check: farthest_norm well below file edge.
            # The component stopped growing while primes continued.
            # The multi-wedge solver does not emit MOAT_FOUND, so we infer
            # moat from the gap between farthest_norm and norm_hi.
            if farthest_norm > 0 and farthest_norm < norm_hi:
                # Gap = norms between farthest and file edge.
                # If the gap is at least 1% of the file span, the component
                # genuinely stopped — not just barely at the edge.
                gap_fraction = (norm_hi - farthest_norm) / max(1, norm_hi)
                if gap_fraction > 0.01:
                    return "moat_confirmed"

            # Fallback: distance-based check (original heuristic for narrow bands)
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

    @staticmethod
    def _print_sweep_summary(
        sieve_ms: int,
        solve_ms: int,
        primes_generated: int,
        primes_processed: int,
        moat_found: bool,
        farthest_distance: float,
    ) -> None:
        total_ms = int(sieve_ms) + int(solve_ms)
        total_s = total_ms / 1000.0
        throughput = 0.0 if total_s <= 0 else (float(primes_processed) / total_s)
        moat_label = "MOAT_FOUND" if moat_found else "NO_MOAT_IN_RANGE"

        print("=== SWEEP SUMMARY ===")
        print(f"sieve time (s): {sieve_ms / 1000.0:.3f}")
        print(f"solve time (s): {solve_ms / 1000.0:.3f}")
        print(f"total time (s): {total_s:.3f}")
        print(f"primes generated: {int(primes_generated)}")
        print(f"primes processed: {int(primes_processed)}")
        print(f"effective throughput (primes/sec): {throughput:.1f}")
        print(f"moat result: {moat_label} distance={float(farthest_distance):.3f}")

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

    def _compute_chunks(self, start: int, end: int, max_primes: int) -> List[Tuple[int, int]]:
        chunks: List[Tuple[int, int]] = []
        current = int(start)

        while current <= end:
            chunk_norm_lo = max(0, (current - self.k_int) ** 2)
            lo = current
            hi = int(end)
            best = current
            found_fit = False

            while lo <= hi:
                mid = (lo + hi) // 2
                extra_margin = self.k_int if mid >= end else 0
                chunk_norm_hi = (mid + self.k_int + extra_margin) ** 2
                est = self._estimate_prime_count(chunk_norm_lo, chunk_norm_hi)

                if est <= max_primes:
                    best = mid
                    found_fit = True
                    lo = mid + 1
                else:
                    hi = mid - 1

            if not found_fit:
                best = current

            chunks.append((current, best))
            if best >= end:
                break

            next_start = max(start, best - self.k_int)
            if next_start <= current:
                next_start = current + 1
            current = next_start

        return chunks

    def _chunked_sweep(self, start: int, end: int, full_norm_lo: int, full_norm_hi: int) -> Dict[str, object]:
        """Sweep in chunks when a full-range sweep exceeds the prime cap."""
        max_primes = 150_000_000
        chunks = self._compute_chunks(start, end, max_primes)
        if not chunks:
            return {"result": "no_moat_in_range", "boundary": int(end)}

        self.log(
            f"sweep exceeds cap, using {len(chunks)} chunks for norm=[{full_norm_lo},{full_norm_hi}]",
            always=True,
        )

        farthest_norm = 0
        farthest_a = 0
        farthest_b = 0
        total_sieve_ms = 0
        total_solve_ms = 0
        total_generated = 0
        total_processed = 0
        best_farthest_distance = float(start)

        for i, (chunk_start_dist, chunk_end_dist) in enumerate(chunks, start=1):
            iteration = self._next_iteration()
            chunk_norm_lo = max(0, (chunk_start_dist - self.k_int) ** 2)
            extra_margin = self.k_int if chunk_end_dist >= end else 0
            chunk_norm_hi = (chunk_end_dist + self.k_int + extra_margin) ** 2

            timeout_s = self._estimate_probe_timeout_s(chunk_norm_lo, chunk_norm_hi)
            gprf_file = self.work_dir / f"ub_{safe_tag(self.args.tag)}_chunk{i}.gprf"

            sieve_ms = 0
            solve_ms = 0
            file_prime_count = 0
            parsed: Dict[str, object] = {
                "moat_found": False,
                "farthest_point": "(0,0)",
                "farthest_a": 0,
                "farthest_b": 0,
                "farthest_distance": float(chunk_start_dist),
                "farthest_norm": 0,
                "component_size": 0,
                "primes_processed": 0,
            }
            status = "inconclusive_edge"
            error_text = ""

            sieve_cmd = [
                self.args.sieve_bin,
                "--norm-lo",
                str(chunk_norm_lo),
                "--norm-hi",
                str(chunk_norm_hi),
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
                str(chunk_start_dist),
                "--angular",
                str(self.args.wedges),
                "--prime-file",
                str(gprf_file),
                "--batch-size",
                str(self.args.batch_size),
                "--norm-bound",
                str(chunk_norm_hi),
                "--profile",
            ]
            # Note: resume args are only effective in LB mode. In UB mode
            # (always active here since --start-distance is provided), the
            # solver ignores them. Each UB chunk independently auto-connects
            # primes below its start_distance and sweeps from there. A moat
            # within any chunk's range will be detected without cross-chunk
            # state transfer.
            # Resume args left here for potential future LB chunked sweep support.
            if i > 1 and farthest_norm > 0:
                solver_cmd += [
                    "--resume-farthest-norm",
                    str(farthest_norm),
                    "--resume-farthest-a",
                    str(farthest_a),
                    "--resume-farthest-b",
                    str(farthest_b),
                ]

            self.log(
                f"[chunked_sweep_{i}] it={iteration} dist=[{chunk_start_dist},{chunk_end_dist}] "
                f"norm=[{chunk_norm_lo},{chunk_norm_hi}] timeout={timeout_s}s",
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
                    status = self._classify_moat(parsed, chunk_norm_hi)
                except Exception as exc:
                    status = "error"
                    error_text = str(exc)
                    self.log(f"[chunked_sweep_{i}] it={iteration} ERROR: {error_text}", always=True)
                finally:
                    if gprf_file.exists():
                        try:
                            gprf_file.unlink()
                        except OSError:
                            pass

            num_primes = int(parsed.get("primes_processed", 0))
            if num_primes <= 0:
                num_primes = int(file_prime_count)

            parsed_norm = int(parsed.get("farthest_norm", 0))
            if parsed_norm > farthest_norm:
                farthest_norm = parsed_norm
                farthest_a = int(parsed.get("farthest_a", 0))
                farthest_b = int(parsed.get("farthest_b", 0))

            farthest_distance = float(parsed.get("farthest_distance", 0.0))
            best_farthest_distance = max(best_farthest_distance, farthest_distance)

            total_sieve_ms += int(sieve_ms)
            total_solve_ms += int(solve_ms)
            total_generated += int(file_prime_count)
            total_processed += int(num_primes)

            record: Dict[str, object] = {
                "timestamp": iso_utc_now(),
                "phase": f"chunked_sweep_{i}",
                "iteration": iteration,
                "start_distance": int(chunk_start_dist),
                "shell_width": int(self.args.shell_width),
                "norm_lo": int(chunk_norm_lo),
                "norm_hi": int(chunk_norm_hi),
                "num_primes": int(num_primes),
                "farthest_point": str(parsed.get("farthest_point", "(0,0)")),
                "farthest_a": int(parsed.get("farthest_a", 0)),
                "farthest_b": int(parsed.get("farthest_b", 0)),
                "farthest_norm": int(parsed.get("farthest_norm", 0)),
                "farthest_distance": farthest_distance,
                "component_size": int(parsed.get("component_size", 0)),
                "sieve_ms": int(sieve_ms),
                "solve_ms": int(solve_ms),
                "status": status,
                "tag": self.args.tag,
                "k_squared": self.k_squared,
                "wedges": int(self.args.wedges),
                "band_width": int(max(0, chunk_norm_hi - chunk_norm_lo)),
                "primes_generated": int(file_prime_count),
                "action": "continue_chunked_sweep",
                "next_start_distance": int(chunk_end_dist),
                "next_shell_width": int(self.args.shell_width),
            }
            if status == "moat_confirmed":
                record["action"] = "stop_moat"
            elif status == "error":
                record["action"] = "stop_error"
            if error_text:
                record["error"] = error_text
            self._append_record(record)

            if status == "error":
                self._print_sweep_summary(
                    total_sieve_ms,
                    total_solve_ms,
                    total_generated,
                    total_processed,
                    False,
                    best_farthest_distance,
                )
                return {"result": "error", "boundary": int(chunk_start_dist)}

            if status == "moat_confirmed":
                self._print_sweep_summary(
                    total_sieve_ms,
                    total_solve_ms,
                    total_generated,
                    total_processed,
                    True,
                    farthest_distance,
                )
                return {"result": "moat_found", "boundary": int(farthest_distance)}

        self._print_sweep_summary(
            total_sieve_ms,
            total_solve_ms,
            total_generated,
            total_processed,
            False,
            best_farthest_distance,
        )
        return {"result": "no_moat_in_range", "boundary": int(end)}

    def run_progressive_sweep(self) -> Dict[str, object]:
        """Wide-band progressive: one sieve + one solver for the full range."""
        start = int(self.args.start_distance)
        end = int(self.args.ceiling)

        norm_lo = max(0, (start - self.k_int) ** 2)
        norm_hi = (end + self.k_int + self.k_int) ** 2
        est_primes = self._estimate_prime_count(norm_lo, norm_hi)
        if est_primes > 150_000_000:
            self.log(
                f"[sweep] estimated primes {est_primes/1e6:.1f}M exceed 150.0M cap; switching to chunked sweep",
                always=True,
            )
            return self._chunked_sweep(start, end, norm_lo, norm_hi)

        iteration = self._next_iteration()
        gprf_file = self.work_dir / f"ub_{safe_tag(self.args.tag)}_sweep.gprf"
        timeout_s = self._estimate_probe_timeout_s(norm_lo, norm_hi)

        sieve_ms = 0
        solve_ms = 0
        file_prime_count = 0
        parsed: Dict[str, object] = {
            "moat_found": False,
            "farthest_point": "(0,0)",
            "farthest_a": 0,
            "farthest_b": 0,
            "farthest_distance": float(start),
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
            str(start),
            "--angular",
            str(self.args.wedges),
            "--prime-file",
            str(gprf_file),
            "--batch-size",
            str(self.args.batch_size),
            "--norm-bound",
            str(norm_hi),
            "--profile",
        ]

        self.log(
            f"[sweep] it={iteration} start={start} end={end} norm=[{norm_lo},{norm_hi}] timeout={timeout_s}s",
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
                    f"  sweep sieve={sieve_ms}ms solve={solve_ms}ms primes={file_prime_count} "
                    f"farthest={parsed.get('farthest_distance', 0):.1f} "
                    f"comp={parsed.get('component_size', 0)}/{parsed.get('primes_processed', 0)} "
                    f"status={status}",
                    always=True,
                )
            except Exception as exc:
                status = "error"
                error_text = str(exc)
                self.log(f"[sweep] it={iteration} ERROR: {error_text}", always=True)
            finally:
                if gprf_file.exists():
                    try:
                        gprf_file.unlink()
                    except OSError:
                        pass

        num_primes = int(parsed.get("primes_processed", 0))
        if num_primes <= 0:
            num_primes = int(file_prime_count)

        farthest_distance = float(parsed.get("farthest_distance", 0.0))
        record: Dict[str, object] = {
            "timestamp": iso_utc_now(),
            "phase": "sweep",
            "iteration": iteration,
            "start_distance": int(start),
            "shell_width": int(self.args.shell_width),
            "norm_lo": int(norm_lo),
            "norm_hi": int(norm_hi),
            "num_primes": int(num_primes),
            "farthest_point": str(parsed.get("farthest_point", "(0,0)")),
            "farthest_a": int(parsed.get("farthest_a", 0)),
            "farthest_b": int(parsed.get("farthest_b", 0)),
            "farthest_norm": int(parsed.get("farthest_norm", 0)),
            "farthest_distance": farthest_distance,
            "component_size": int(parsed.get("component_size", 0)),
            "sieve_ms": int(sieve_ms),
            "solve_ms": int(solve_ms),
            "status": status,
            "tag": self.args.tag,
            "k_squared": self.k_squared,
            "wedges": int(self.args.wedges),
            "band_width": int(max(0, norm_hi - norm_lo)),
            "primes_generated": int(file_prime_count),
            "action": "sweep_complete",
            "next_start_distance": int(start),
            "next_shell_width": int(self.args.shell_width),
        }
        if status == "moat_confirmed":
            record["action"] = "stop_moat"
        elif status == "error":
            record["action"] = "stop_error"
        if error_text:
            record["error"] = error_text
        self._append_record(record)

        self._print_sweep_summary(
            sieve_ms,
            solve_ms,
            int(file_prime_count),
            int(num_primes),
            status == "moat_confirmed",
            farthest_distance,
        )

        if status == "error":
            return {"result": "error", "boundary": int(start)}
        if status == "moat_confirmed":
            return {"result": "moat_found", "boundary": int(farthest_distance)}
        return {"result": "no_moat_in_range", "boundary": int(end)}

    def run_progressive(self) -> Dict[str, object]:
        if self.args.sweep_mode == "sweep":
            return self.run_progressive_sweep()

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
        saved_shell = int(self.args.shell_width)

        if self.args.sweep_mode == "sweep":
            # Default bisect_sweep_shell=50 gives 10x (locate/verify) and 5x (refine).
            locate_mult = max(1, int(self.args.bisect_sweep_shell) // 5)
            refine_mult = max(1, locate_mult // 2)
            verify_mult = locate_mult
            locate_shell = shell * locate_mult
            refine_shell = shell * refine_mult
            verify_shell = shell * verify_mult
            self.log(
                f"bisect sweep shells: locate={locate_shell} refine={refine_shell} verify={verify_shell}",
                always=True,
            )
        else:
            locate_shell = shell
            refine_shell = shell
            verify_shell = shell * 3

        lo = None
        hi = None
        prev = int(self.args.start_distance)
        dist = prev

        try:
            # Phase 1: LOCATE -- geometric doubling
            self.args.shell_width = locate_shell
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
            self.args.shell_width = refine_shell
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
            self.args.shell_width = verify_shell
            self.log(f"--- Phase 3: VERIFY at D={boundary} shell={self.args.shell_width} ---", always=True)
            verify = self._forward_walk("verify", boundary, walk_iters)

            if verify["moat_found"]:
                return {"result": "boundary_found", "boundary": boundary}

            return {"result": "boundary_inconclusive", "boundary": boundary}
        finally:
            self.args.shell_width = saved_shell

    def run(self) -> Dict[str, object]:
        if not self.args.dry_run:
            for exe in (self.args.sieve_bin, self.args.solver_bin):
                path = pathlib.Path(exe)
                if not (path.exists() and os.access(path, os.X_OK)):
                    raise RuntimeError(f"binary not executable: {exe}")

        self.log(
            f"=== UB Campaign: k^2={self.k_squared} tag={self.args.tag} "
            f"mode={self.args.mode} sweep_mode={self.args.sweep_mode} ===",
            always=True,
        )
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
    parser.add_argument("--sweep-mode", choices=["per-probe", "sweep"], default="per-probe")
    parser.add_argument("--wedges", type=int, default=None)
    parser.add_argument("--max-iterations", type=int, default=200)
    parser.add_argument("--bisect-tolerance", type=int, default=1_000_000)
    parser.add_argument(
        "--bisect-sweep-shell",
        type=int,
        default=50,
        help="Sweep bisect shell control (50 => locate/verify 10x, refine 5x)",
    )
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
    if args.bisect_sweep_shell <= 0:
        parser.error("--bisect-sweep-shell must be > 0")
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
