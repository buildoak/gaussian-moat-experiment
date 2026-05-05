#!/usr/bin/env python3
"""Write a compact anatomy report from CUDA profile JSON files."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load(path: Path) -> dict:
    with path.open() as fh:
        return json.load(fh)


def row(profile: dict) -> dict:
    radii = profile.get("radii", {})
    tiles = profile.get("tiles", {})
    overflow = profile.get("overflow_counters", {})
    host = profile.get("host_tileop_counters", {})
    stats_v2 = profile.get("stats_v2", {})
    bz = profile.get("bz", {})
    return {
        "k_sq": radii.get("k_sq"),
        "r_inner": radii.get("r_inner"),
        "r_outer": radii.get("r_outer"),
        "width": (
            radii.get("r_outer") - radii.get("r_inner")
            if isinstance(radii.get("r_outer"), int)
            and isinstance(radii.get("r_inner"), int)
            else None
        ),
        "verdict": profile.get("verdict"),
        "active_tiles": tiles.get("active"),
        "produced_tiles": tiles.get("produced"),
        "ingested_tiles": tiles.get("ingested"),
        "early_exit_enabled": profile.get("early_exit_enabled"),
        "early_exit_taken": profile.get("early_exit_taken"),
        "bz_clean": bz.get("clean"),
        "bz_override_used": bz.get("override_used"),
        "geo_i_tiles": stats_v2.get("geo_i_tiles"),
        "geo_o_tiles": stats_v2.get("geo_o_tiles"),
        "emitted_overflow_bit_count": host.get("emitted_overflow_bit_count"),
        "overflow_counters": overflow,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profiles", nargs="+", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args()

    rows = [row(load(path)) | {"profile": str(path)} for path in args.profiles]
    lines = [
        "# K36 Lower Moat Anatomy Report",
        "",
        "This report summarizes profile-level boundary and run health signals. It is explanatory evidence, not an independent MOAT proof.",
        "",
        "| profile | K | R_inner | R_outer | verdict | produced/ingested | early-exit | BZ | overflow | geo_I tiles | geo_O tiles |",
        "|---|---:|---:|---:|---|---:|---|---|---:|---:|---:|",
    ]
    for r in rows:
        early = f"{r['early_exit_enabled']}/{r['early_exit_taken']}"
        bz = f"{r['bz_clean']}/{r['bz_override_used']}"
        lines.append(
            f"| `{Path(r['profile']).name}` | {r['k_sq']} | {r['r_inner']} | {r['r_outer']} | "
            f"{r['verdict']} | {r['produced_tiles']}/{r['ingested_tiles']} | {early} | {bz} | "
            f"{r['emitted_overflow_bit_count']} | {r['geo_i_tiles']} | {r['geo_o_tiles']} |"
        )
    lines.extend(
        [
            "",
            "Interpretation:",
            "- `geo_I`/`geo_O` tile counts require `--stats-level profile` output. `None` means the profile predates stats v2 or was emitted without stats collection.",
            "- A MOAT row remains whole-annulus evidence only if `produced == ingested == active`, zero overflows, and no BZ override was used.",
        ]
    )
    text = "\n".join(lines) + "\n"
    if args.out:
      args.out.write_text(text)
    else:
      print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
