#!/usr/bin/env python3
"""Summarize profile stats_v2 blocks for quick shell inspection."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("profiles", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.profiles:
        data = json.loads(path.read_text())
        stats = data.get("stats_v2", {})
        print(
            path.name,
            "verdict=", data.get("verdict"),
            "geo_i_tiles=", stats.get("geo_i_tiles"),
            "geo_o_tiles=", stats.get("geo_o_tiles"),
            "sample_manifest=", stats.get("sample_manifest_path"),
            "tile_sample=", stats.get("tile_sample_path"),
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
