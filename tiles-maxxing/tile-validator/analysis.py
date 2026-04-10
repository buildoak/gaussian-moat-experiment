"""Shared analysis helpers for pruning, measurement, and diagnostics."""

from __future__ import annotations

from collections import Counter, defaultdict


FACES = ("I", "O", "L", "R")
PAYLOAD_BUDGET = 125
GROUP_LABEL_MAX = 127


def compute_group_face_incidence(ports_by_face: dict[str, list[dict]]) -> dict[int, dict[str, int]]:
    incidence: dict[int, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    for face in FACES:
        for port in ports_by_face.get(face, []):
            group = int(port["group"])
            incidence[group][face] += 1
    return {group: dict(face_counts) for group, face_counts in incidence.items()}


def identify_dead_ends(incidence: dict[int, dict[str, int]]) -> set[int]:
    dead_ends: set[int] = set()
    for group, face_counts in incidence.items():
        if len(face_counts) == 1 and sum(face_counts.values()) == 1:
            dead_ends.add(group)
    return dead_ends


def classify_surviving_groups(ports_by_face: dict[str, list[dict]]) -> list[dict]:
    incidence = compute_group_face_incidence(ports_by_face)
    classifications: list[dict] = []
    for group in sorted(incidence):
        counts = incidence[group]
        total_ports = sum(counts.values())
        if len(counts) >= 2:
            category = "multi_face"
        elif total_ports >= 2:
            category = "single_face_multi_port"
        else:
            category = "dead_end"
        classifications.append(
            {
                "group_id": group,
                "faces": sorted(counts),
                "ports_per_face": counts,
                "total_ports": total_ports,
                "category": category,
            }
        )
    return classifications


def packed_budget_counts(ports_by_face: dict[str, list[dict]]) -> dict[str, int]:
    counts = {face: len(ports_by_face.get(face, [])) for face in FACES}
    packed_bytes_used = counts["O"] + counts["I"] + 2 * counts["L"] + 2 * counts["R"]
    return {
        "o_cnt": counts["O"],
        "i_cnt": counts["I"],
        "l_cnt": counts["L"],
        "r_cnt": counts["R"],
        "packed_bytes_used": packed_bytes_used,
        "packed_bytes_slack": PAYLOAD_BUDGET - packed_bytes_used,
    }


def overflow_reason(ports_by_face: dict[str, list[dict]], group_count: int) -> str | None:
    budget = packed_budget_counts(ports_by_face)
    if group_count > GROUP_LABEL_MAX:
        return "group_count"
    if budget["packed_bytes_used"] > PAYLOAD_BUDGET:
        return "packed_budget"
    return None


def category_histogram(classifications: list[dict]) -> dict[str, int]:
    return dict(Counter(item["category"] for item in classifications))
