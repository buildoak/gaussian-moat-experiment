"""Dead-end pruning analysis against the canonical tile pipeline."""

from __future__ import annotations

from analysis import category_histogram, classify_surviving_groups, packed_budget_counts
from tile import DEFAULT_K, process_tile


def pruning_analysis(a_lo: int, b_lo: int, k_sq: int = DEFAULT_K) -> dict:
    result = process_tile(a_lo, b_lo, k_sq)
    classifications = classify_surviving_groups(result["ports"])
    budget = packed_budget_counts(result["ports"])
    return {
        "a_lo": a_lo,
        "b_lo": b_lo,
        "prime_count": result["prime_count"],
        "group_count": result["group_count"],
        "ports_before_pruning": result["ports_before_pruning"],
        "ports_after_pruning": result["ports_after_pruning"],
        "overflow": result["overflow"],
        "packed_bytes_used": budget["packed_bytes_used"],
        "packed_bytes_slack": budget["packed_bytes_slack"],
        "classifications": classifications,
    }


def main():
    tiles = [
        (601040640, 601040640, "45deg"),
        (736121088, 424999936, "30deg"),
        (820888320, 220000000, "15deg"),
    ]
    for a_lo, b_lo, label in tiles:
        result = pruning_analysis(a_lo, b_lo)
        print(f"{label} ({a_lo},{b_lo})")
        print(f"  prime_count={result['prime_count']}")
        print(f"  group_count={result['group_count']}")
        print(f"  ports_before={result['ports_before_pruning']}")
        print(f"  ports_after={result['ports_after_pruning']}")
        print(f"  packed_bytes={result['packed_bytes_used']} slack={result['packed_bytes_slack']}")
        print(f"  overflow={result['overflow']}")
        print(f"  categories={category_histogram(result['classifications'])}")


if __name__ == "__main__":
    main()
