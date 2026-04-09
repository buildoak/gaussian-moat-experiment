"""Spec-facing validation helpers for canonical 128-byte TileOps."""

from __future__ import annotations

from dataclasses import dataclass

from tile import DEFAULT_K, process_tile
from tileop import decode_face_h1, validate_tileop_structure


@dataclass
class ValidationResult:
    name: str
    passed: bool = True
    details: list[str] | None = None

    def __post_init__(self):
        if self.details is None:
            self.details = []

    def fail(self, message: str):
        self.passed = False
        self.details.append(f"FAIL: {message}")

    def info(self, message: str):
        self.details.append(f"INFO: {message}")

    def __str__(self) -> str:
        status = "PASS" if self.passed else "FAIL"
        lines = [f"{self.name}: {status}"]
        lines.extend(f"  {detail}" for detail in self.details)
        return "\n".join(lines)


def validate_tileop_bytes(result: dict) -> ValidationResult:
    check = ValidationResult("TileOp Layout")
    tileop = result["tileop"]
    for problem in validate_tileop_structure(tileop):
        check.fail(problem)

    for face in ("I", "O", "L", "R"):
        expected_groups = [port["group"] for port in result["ports"][face]]
        actual_groups = result["tileop_decoded"][face]["groups"]
        if actual_groups != expected_groups:
            check.fail(f"{face} groups mismatch: expected {expected_groups}, got {actual_groups}")

    for face in ("L", "R"):
        expected_h1 = [port["h1"] for port in result["ports"][face]]
        actual_h1 = result["tileop_decoded"][face]["h1"]
        if actual_h1 != expected_h1:
            check.fail(f"{face} h1 mismatch: expected {expected_h1}, got {actual_h1}")

    if check.passed:
        check.info(
            f"groups={result['group_count']} ports_after={result['ports_after_pruning']} overflow={result['overflow']}"
        )
    return check


def validate_io_alignment(result_a: dict, result_b: dict) -> ValidationResult:
    check = ValidationResult("I/O Face Alignment")
    if result_a["overflow"] or result_b["overflow"]:
        check.info("skipped due to overflow sentinel")
        return check

    a_primes = {
        tuple(prime)
        for port in result_a["ports"]["O"]
        for prime in port["primes"]
    }
    b_primes = {
        tuple(prime)
        for port in result_b["ports"]["I"]
        for prime in port["primes"]
    }
    shared = a_primes & b_primes
    if not shared and (a_primes or b_primes):
        check.fail(f"shared-prime identity match failed: O={len(a_primes)} I={len(b_primes)}")
    else:
        check.info(f"shared boundary primes={len(shared)}")

    a_cnt = len(result_a["tileop_decoded"]["O"]["groups"])
    b_cnt = len(result_b["tileop_decoded"]["I"]["groups"])
    if a_cnt != b_cnt:
        check.fail(f"header-derived O/I count mismatch: A.O={a_cnt} B.I={b_cnt}")
    return check


def validate_lr_matching(result_a: dict, result_b: dict, delta_h: int = 0) -> ValidationResult:
    check = ValidationResult("L/R h1 Matching")
    if result_a["overflow"] or result_b["overflow"]:
        check.info("skipped due to overflow sentinel")
        return check

    if delta_h == 0:
        a_primes = {
            tuple(prime)
            for port in result_a["ports"]["R"]
            for prime in port["primes"]
        }
        b_primes = {
            tuple(prime)
            for port in result_b["ports"]["L"]
            for prime in port["primes"]
        }
        shared = a_primes & b_primes
        check.info(f"delta_h=0 shared primes={len(shared)} A.R={len(a_primes)} B.L={len(b_primes)}")
        return check

    a_h1 = result_a["tileop_decoded"]["R"]["h1"]
    b_h1 = result_b["tileop_decoded"]["L"]["h1"]
    matches = sum(1 for ah in a_h1 for bh in b_h1 if ah == bh + delta_h)
    check.info(f"delta_h={delta_h} decoded raw-h1 matches={matches} A.R={len(a_h1)} B.L={len(b_h1)}")
    return check


def validate_tile(a_lo: int, b_lo: int, k_sq: int = DEFAULT_K) -> list[ValidationResult]:
    result = process_tile(a_lo, b_lo, k_sq)
    return [validate_tileop_bytes(result)]


def validate_pair(
    a_lo_a: int,
    b_lo_a: int,
    a_lo_b: int,
    b_lo_b: int,
    k_sq: int = DEFAULT_K,
    delta_h: int = 0,
) -> list[ValidationResult]:
    result_a = process_tile(a_lo_a, b_lo_a, k_sq)
    result_b = process_tile(a_lo_b, b_lo_b, k_sq)
    checks = [validate_tileop_bytes(result_a), validate_tileop_bytes(result_b)]
    if a_lo_a == a_lo_b:
        checks.append(validate_io_alignment(result_a, result_b))
    if b_lo_a == b_lo_b:
        checks.append(validate_lr_matching(result_a, result_b, delta_h))
    return checks
