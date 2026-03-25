#!/usr/bin/env python3
"""k²=40 fat-stripe verification campaign for Jetson Orin Nano.

24 sequential probes across 4 phases:
  Phase 1 — Sanity (3):       known-result probes to confirm tool correctness
  Phase 2 — Lower boundary (8): binary search for blockage onset
  Phase 3 — Off-axis (6):      isotropy verification at R=1.05B, θ=5°..45°
  Phase 4 — Upper boundary + degree profiling (7)

Designed for 7.4 GB RAM — strictly sequential, one fat-stripe at a time.
Resumable via checkpoint file. Progress written for remote monitoring.
"""

from __future__ import annotations

import datetime
import json
import math
import os
import pathlib
import re
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from typing import Optional

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

FAT_STRIPE_BIN = os.path.expanduser(
    "~/gaussian-moat-cuda/tile-probe/target/release/fat-stripe"
)
LOG_FILE = os.path.expanduser("~/k40-verify-campaign.log")
SUMMARY_FILE = os.path.expanduser("~/k40-verify-summary.txt")
CHECKPOINT_FILE = os.path.expanduser("~/k40-verify-checkpoint.json")
PROGRESS_FILE = os.path.expanduser("~/k40-verify-progress.txt")

K_SQUARED = 40
B_HALF_WIDTH = 64_000  # half of 128K strip
RADIAL_HALF_WIDTH = 1000  # ±1000 around center


# ---------------------------------------------------------------------------
# Probe definition
# ---------------------------------------------------------------------------


@dataclass
class Probe:
    """A single fat-stripe probe specification."""

    phase: int
    index: int  # 1-based within phase
    label: str
    R: float  # distance from origin (norm)
    theta_deg: float = 0.0
    b_max_override: Optional[int] = None  # for on-axis probes
    degree_stats: bool = False
    expect: Optional[str] = None  # "CONNECTED" | "BLOCKED" | None

    @property
    def uid(self) -> str:
        """Unique probe identifier for checkpoint tracking."""
        return f"P{self.phase}-{self.index:02d}"

    def build_cmd(self) -> list[str]:
        """Build the fat-stripe CLI invocation."""
        theta_rad = math.radians(self.theta_deg)

        if self.theta_deg == 0.0:
            # On-axis: r_min/r_max bracket R directly
            r_min = self.R - RADIAL_HALF_WIDTH
            r_max = self.R + RADIAL_HALF_WIDTH
            b_max = self.b_max_override or 128_000
            cmd = [
                FAT_STRIPE_BIN,
                "--k-squared", str(K_SQUARED),
                "--r-min", f"{r_min:.0f}",
                "--r-max", f"{r_max:.0f}",
                "--b-max", str(b_max),
                "--verbose",
            ]
        else:
            # Off-axis: center strip at the target angle
            a_center = self.R * math.cos(theta_rad)
            b_center = self.R * math.sin(theta_rad)
            r_min = a_center - RADIAL_HALF_WIDTH
            r_max = a_center + RADIAL_HALF_WIDTH
            b_min = int(b_center) - B_HALF_WIDTH
            b_max = int(b_center) + B_HALF_WIDTH
            cmd = [
                FAT_STRIPE_BIN,
                "--k-squared", str(K_SQUARED),
                "--r-min", f"{r_min:.0f}",
                "--r-max", f"{r_max:.0f}",
                "--b-min", str(b_min),
                "--b-max", str(b_max),
                "--verbose",
            ]

        if self.degree_stats:
            cmd.append("--degree-stats")

        return cmd


# ---------------------------------------------------------------------------
# Probe table
# ---------------------------------------------------------------------------


def build_probe_table() -> list[Probe]:
    """Build the full 24-probe campaign table."""
    probes: list[Probe] = []

    # Phase 1 — Sanity (3 probes)
    probes.append(Probe(1, 1, "sanity-connected-600M", 600_000_000, expect="CONNECTED"))
    probes.append(Probe(1, 2, "sanity-connected-800M", 800_000_000, expect="CONNECTED"))
    probes.append(Probe(1, 3, "sanity-blocked-1.05B", 1_050_000_000, expect="BLOCKED"))

    # Phase 2 — Lower boundary hunt (8 probes)
    for i, r in enumerate(
        [1_000_000_000, 980_000_000, 960_000_000, 940_000_000,
         920_000_000, 900_000_000, 850_000_000, 800_000_000],
        start=1,
    ):
        label = f"lb-R{r/1e9:.2f}B"
        probes.append(Probe(2, i, label, r))

    # Phase 3 — Off-axis verification (6 probes), all at R=1.05B
    R_offaxis = 1_050_000_000
    for i, theta in enumerate([5, 11, 22, 33, 40, 45], start=1):
        label = f"offaxis-{theta}deg"
        probes.append(Probe(3, i, label, R_offaxis, theta_deg=theta))

    # Phase 4 — Upper boundary + degree profiling (7 probes)
    for i, r in enumerate([1_120_000_000, 1_150_000_000, 1_200_000_000, 1_300_000_000], start=1):
        label = f"ub-R{r/1e9:.2f}B"
        probes.append(Probe(4, i, label, r))

    # Degree-stats probes
    for i, r in enumerate([950_000_000, 1_000_000_000, 1_050_000_000], start=5):
        label = f"degree-R{r/1e9:.2f}B"
        probes.append(Probe(4, i, label, r, degree_stats=True))

    return probes


# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------


def ts() -> str:
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def log(msg: str, *, log_fh=None) -> None:
    line = f"[{ts()}] {msg}"
    print(line, flush=True)
    if log_fh:
        log_fh.write(line + "\n")
        log_fh.flush()


# ---------------------------------------------------------------------------
# Checkpoint management
# ---------------------------------------------------------------------------


def load_checkpoint() -> set[str]:
    """Return set of completed probe UIDs."""
    if not os.path.exists(CHECKPOINT_FILE):
        return set()
    try:
        with open(CHECKPOINT_FILE) as f:
            data = json.load(f)
        return set(data.get("completed", []))
    except (json.JSONDecodeError, KeyError):
        return set()


def save_checkpoint(completed: set[str]) -> None:
    with open(CHECKPOINT_FILE, "w") as f:
        json.dump({"completed": sorted(completed), "updated": ts()}, f, indent=2)


# ---------------------------------------------------------------------------
# Progress file
# ---------------------------------------------------------------------------


def write_progress(
    probe: Probe,
    done: int,
    total: int,
    start_time: float,
    phase_times: dict[int, float],
) -> None:
    elapsed = time.monotonic() - start_time
    if done > 0:
        avg_per_probe = elapsed / done
        remaining = (total - done) * avg_per_probe
        eta_str = str(datetime.timedelta(seconds=int(remaining)))
    else:
        eta_str = "estimating..."

    lines = [
        f"k40-verify campaign progress — {ts()}",
        f"Running:   {probe.uid} ({probe.label})",
        f"Completed: {done}/{total}",
        f"Elapsed:   {str(datetime.timedelta(seconds=int(elapsed)))}",
        f"ETA:       {eta_str}",
        "",
    ]
    for phase in sorted(phase_times):
        lines.append(f"  Phase {phase} avg: {phase_times[phase]:.0f}s")

    with open(PROGRESS_FILE, "w") as f:
        f.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------
# Output parsing
# ---------------------------------------------------------------------------


def parse_output(stdout: str, stderr: str) -> dict:
    """Parse fat-stripe output into a summary dict."""
    result: dict = {
        "blocked": None,
        "spanning": None,
        "tiles": None,
        "elapsed_ms": None,
        "degree_stats": None,
    }

    # stdout: "campaign: blocked=true stripes=1 chunks=64 tiles=4096 elapsed=1234ms"
    m = re.search(r"campaign:\s+blocked=(\w+)\s+.*tiles=(\d+)\s+elapsed=(\d+)ms", stdout)
    if m:
        result["blocked"] = m.group(1) == "true"
        result["tiles"] = int(m.group(2))
        result["elapsed_ms"] = int(m.group(3))

    # stderr: "verdict: N spanning component(s)"
    m2 = re.search(r"verdict:\s+(\d+)\s+spanning", stderr)
    if m2:
        result["spanning"] = int(m2.group(1))

    # DEGREE_STATS line on stdout
    m3 = re.search(r"DEGREE_STATS:\s+(.*)", stdout)
    if m3:
        result["degree_stats"] = m3.group(1).strip()

    return result


# ---------------------------------------------------------------------------
# Memory safety guard
# ---------------------------------------------------------------------------


def wait_for_no_fat_stripe(timeout: int = 60) -> bool:
    """Ensure no fat-stripe process is running. Wait up to timeout seconds."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ret = subprocess.run(["pgrep", "-x", "fat-stripe"], capture_output=True)
        if ret.returncode != 0:
            return True  # no process found
        time.sleep(2)
    return False


# ---------------------------------------------------------------------------
# Main campaign loop
# ---------------------------------------------------------------------------


def main() -> None:
    probes = build_probe_table()
    total = len(probes)
    completed = load_checkpoint()

    # Open log file in append mode
    log_fh = open(LOG_FILE, "a")

    log(f"=== k40-verify campaign start ({total} probes, {len(completed)} already done) ===", log_fh=log_fh)

    # Verify binary exists
    if not os.path.isfile(FAT_STRIPE_BIN):
        log(f"FATAL: fat-stripe binary not found at {FAT_STRIPE_BIN}", log_fh=log_fh)
        sys.exit(1)

    if not os.access(FAT_STRIPE_BIN, os.X_OK):
        log(f"FATAL: fat-stripe binary not executable at {FAT_STRIPE_BIN}", log_fh=log_fh)
        sys.exit(1)

    campaign_start = time.monotonic()
    done_count = len(completed)
    phase_durations: dict[int, list[float]] = {}

    for probe in probes:
        if probe.uid in completed:
            log(f"SKIP {probe.uid} ({probe.label}) — already completed", log_fh=log_fh)
            continue

        # Memory safety: ensure no other fat-stripe is running
        if not wait_for_no_fat_stripe(timeout=120):
            log(f"ABORT: another fat-stripe still running after 120s wait", log_fh=log_fh)
            sys.exit(2)

        # Phase/probe header
        log(f"", log_fh=log_fh)
        log(f"{'='*70}", log_fh=log_fh)
        log(f"PROBE {probe.uid} — Phase {probe.phase} #{probe.index}: {probe.label}", log_fh=log_fh)
        log(f"  R={probe.R:.0f}  θ={probe.theta_deg}°  degree_stats={probe.degree_stats}", log_fh=log_fh)
        if probe.expect:
            log(f"  EXPECTED: {probe.expect}", log_fh=log_fh)
        log(f"{'='*70}", log_fh=log_fh)

        cmd = probe.build_cmd()
        log(f"CMD: {' '.join(cmd)}", log_fh=log_fh)

        # Update progress file
        phase_avgs = {
            p: sum(ds) / len(ds) for p, ds in phase_durations.items() if ds
        }
        write_progress(probe, done_count, total, campaign_start, phase_avgs)

        # Run the probe
        t0 = time.monotonic()
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=7200,  # 2h hard timeout per probe
            )
            wall_s = time.monotonic() - t0
            stdout = result.stdout
            stderr = result.stderr

            # Log all output
            if stderr:
                for line in stderr.splitlines():
                    log_fh.write(f"  [stderr] {line}\n")
            if stdout:
                for line in stdout.splitlines():
                    log_fh.write(f"  [stdout] {line}\n")
            log_fh.flush()

            # Parse result
            parsed = parse_output(stdout, stderr)
            blocked = parsed["blocked"]
            spanning = parsed["spanning"]
            elapsed_ms = parsed["elapsed_ms"]
            tiles = parsed["tiles"]

            if blocked is True:
                verdict = "BLOCKED"
            elif blocked is False:
                verdict = "CONNECTED"
            else:
                verdict = "PARSE_ERROR"

            # Check expectation
            expect_ok = ""
            if probe.expect:
                if verdict == probe.expect:
                    expect_ok = " [OK]"
                else:
                    expect_ok = f" [MISMATCH! expected {probe.expect}]"

            summary_line = (
                f"{probe.uid} {probe.label:30s} "
                f"R={probe.R:.0f} θ={probe.theta_deg:5.1f}° "
                f"→ {verdict}{expect_ok}  "
                f"wall={wall_s:.0f}s  tiles={tiles}  "
                f"elapsed_ms={elapsed_ms}  spanning={spanning}"
            )
            if parsed["degree_stats"]:
                summary_line += f"  {parsed['degree_stats']}"

            log(f"RESULT: {summary_line}", log_fh=log_fh)

            # Append to summary file
            with open(SUMMARY_FILE, "a") as sf:
                sf.write(f"[{ts()}] {summary_line}\n")

            # Track phase durations for ETA
            phase_durations.setdefault(probe.phase, []).append(wall_s)

        except subprocess.TimeoutExpired:
            wall_s = time.monotonic() - t0
            log(f"TIMEOUT after {wall_s:.0f}s on {probe.uid}", log_fh=log_fh)
            summary_line = f"{probe.uid} {probe.label:30s} → TIMEOUT after {wall_s:.0f}s"
            with open(SUMMARY_FILE, "a") as sf:
                sf.write(f"[{ts()}] {summary_line}\n")

        except Exception as exc:
            log(f"ERROR on {probe.uid}: {exc}", log_fh=log_fh)
            summary_line = f"{probe.uid} {probe.label:30s} → ERROR: {exc}"
            with open(SUMMARY_FILE, "a") as sf:
                sf.write(f"[{ts()}] {summary_line}\n")

        # Mark completed and checkpoint
        completed.add(probe.uid)
        save_checkpoint(completed)
        done_count += 1

    # Final summary
    total_elapsed = time.monotonic() - campaign_start
    log(f"", log_fh=log_fh)
    log(f"{'='*70}", log_fh=log_fh)
    log(f"CAMPAIGN COMPLETE — {done_count}/{total} probes in {datetime.timedelta(seconds=int(total_elapsed))}", log_fh=log_fh)
    log(f"{'='*70}", log_fh=log_fh)

    # Final progress file
    with open(PROGRESS_FILE, "w") as f:
        f.write(f"k40-verify campaign COMPLETE — {ts()}\n")
        f.write(f"Total: {done_count}/{total} probes\n")
        f.write(f"Wall time: {datetime.timedelta(seconds=int(total_elapsed))}\n")
        f.write(f"See: {SUMMARY_FILE}\n")

    log_fh.close()


if __name__ == "__main__":
    main()
