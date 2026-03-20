#!/usr/bin/env python3
"""Morning campaign status checker. Run from Mac to check overnight GPU campaigns."""

from __future__ import annotations

import json
import subprocess
import sys
import time

SSH_HOST = "root@ssh8.vast.ai"
SSH_PORT = "18840"


def ssh_cmd(cmd: str, timeout: int = 30) -> str:
    """Run command on remote via SSH."""
    result = subprocess.run(
        ["ssh", "-o", "StrictHostKeyChecking=no", "-o", "ConnectTimeout=10",
         "-p", SSH_PORT, SSH_HOST, cmd],
        capture_output=True, text=True, timeout=timeout
    )
    return (result.stdout or "") + (result.stderr or "")


def check_tmux_sessions() -> dict:
    """Check running tmux sessions."""
    out = ssh_cmd("tmux ls 2>&1")
    sessions = {}
    for line in out.strip().split("\n"):
        if ":" in line and "windows" in line:
            name = line.split(":")[0].strip()
            sessions[name] = "running"
        elif "no server" in line.lower() or "error" in line.lower():
            pass
    return sessions


def check_campaign(name: str, work_dir: str) -> dict:
    """Read campaign status from JSONL results file."""
    info = {"name": name, "status": "unknown", "probes": 0, "last_phase": "",
            "last_distance": 0, "last_status": "", "moat_found": False,
            "farthest_distance": 0.0, "elapsed": ""}

    # Check if log exists and get last lines
    log_tail = ssh_cmd(f"tail -15 {work_dir}/campaign.log 2>/dev/null")
    if "No such file" in log_tail:
        info["status"] = "not_started"
        return info

    info["log_tail"] = log_tail.strip()

    # Check for SUMMARY (completion indicator)
    if "=== SUMMARY ===" in log_tail:
        info["status"] = "completed"
        for line in log_tail.split("\n"):
            if "result:" in line:
                info["result"] = line.split("result:")[1].strip()
            if "boundary:" in line:
                info["boundary"] = line.split("boundary:")[1].strip()
            if "total probes:" in line:
                info["probes"] = int(line.split("total probes:")[1].strip())
    else:
        info["status"] = "running"

    # Read JSONL for detailed progress
    jsonl_out = ssh_cmd(f"ls {work_dir}/*.jsonl 2>/dev/null")
    jsonl_files = [f.strip() for f in jsonl_out.strip().split("\n") if f.strip().endswith(".jsonl")]

    if jsonl_files:
        # Read last 5 records from first JSONL file
        last_records = ssh_cmd(f"tail -5 {jsonl_files[0]} 2>/dev/null")
        for line in reversed(last_records.strip().split("\n")):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
                info["last_phase"] = rec.get("phase", "")
                info["last_distance"] = rec.get("start_distance", 0)
                info["last_status"] = rec.get("status", "")
                info["farthest_distance"] = rec.get("farthest_distance", 0.0)
                if rec.get("status") == "moat_confirmed":
                    info["moat_found"] = True
                break
            except json.JSONDecodeError:
                continue

        # Count total probes
        count_out = ssh_cmd(f"wc -l < {jsonl_files[0]} 2>/dev/null")
        try:
            info["probes"] = int(count_out.strip())
        except ValueError:
            pass

    # Check GPU state
    gpu_out = ssh_cmd("nvidia-smi --query-gpu=temperature.gpu,utilization.gpu,memory.used --format=csv,noheader,nounits 2>/dev/null")
    info["gpu"] = gpu_out.strip()

    return info


def main():
    print("=" * 60)
    print("  Gaussian Moat Campaign Status Check")
    print(f"  {time.strftime('%Y-%m-%d %H:%M:%S %Z')}")
    print("=" * 60)
    print()

    # Check SSH connectivity
    try:
        ping = ssh_cmd("echo OK", timeout=15)
        if "OK" not in ping:
            print("ERROR: Cannot reach 3090 instance")
            print(f"  SSH: {SSH_HOST} port {SSH_PORT}")
            sys.exit(1)
        print(f"SSH: Connected to {SSH_HOST}:{SSH_PORT}")
    except Exception as e:
        print(f"ERROR: SSH connection failed: {e}")
        sys.exit(1)

    # Check tmux sessions
    sessions = check_tmux_sessions()
    print(f"Tmux sessions: {list(sessions.keys()) if sessions else 'none'}")
    print()

    # Check campaigns
    campaigns = [
        ("sqrt(36) UB validation", "/tmp/gm-sqrt36-ub"),
        ("sqrt(40) UB campaign", "/tmp/gm-sqrt40-ub"),
    ]

    for name, work_dir in campaigns:
        print(f"--- {name} ---")
        info = check_campaign(name, work_dir)
        print(f"  Status:    {info['status']}")
        print(f"  Probes:    {info['probes']}")

        if info["status"] == "completed":
            print(f"  Result:    {info.get('result', 'unknown')}")
            print(f"  Boundary:  {info.get('boundary', 'unknown')}")
        elif info["status"] == "running":
            print(f"  Phase:     {info['last_phase']}")
            print(f"  Distance:  {info['last_distance']:,}")
            print(f"  Farthest:  {info['farthest_distance']:,.1f}")
            print(f"  Last:      {info['last_status']}")

        if info["moat_found"]:
            print(f"  ** MOAT FOUND at distance {info['farthest_distance']:,.1f} **")

        if info.get("gpu"):
            print(f"  GPU:       {info['gpu']} (temp C, util %, VRAM MB)")

        if info.get("log_tail"):
            print(f"  Log tail:")
            for line in info["log_tail"].split("\n")[-5:]:
                print(f"    {line}")
        print()

    # Quick commands reference
    print("--- Quick Commands ---")
    print(f"  Full log:   ssh -p {SSH_PORT} {SSH_HOST} 'tail -50 /tmp/gm-sqrt40-ub/campaign.log'")
    print(f"  Attach:     ssh -t -p {SSH_PORT} {SSH_HOST} 'tmux attach -t sqrt40-ub'")
    print(f"  GPU:        ssh -p {SSH_PORT} {SSH_HOST} 'nvidia-smi'")
    print(f"  Results:    ssh -p {SSH_PORT} {SSH_HOST} 'tail -5 /tmp/gm-sqrt40-ub/*.jsonl'")
    print(f"  Kill:       ssh -p {SSH_PORT} {SSH_HOST} 'tmux kill-session -t sqrt40-ub'")


if __name__ == "__main__":
    main()
