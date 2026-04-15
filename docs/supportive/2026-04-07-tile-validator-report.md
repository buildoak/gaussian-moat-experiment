---
title: Tile Validator Report — P1-P5 Property Validation at Small Radius
date: 2026-04-07
type: evidence
status: complete
supports: tile_spec.md
description: Full P1-P5 validation pass on small-radius tile pair at (100,100); confirms port determinism, group correctness, port completeness, fingerprint uniqueness, and composition correctness all pass with K=36.
---

# Tile Validator Report

Generated: 2026-04-07 22:02:16

## 1. Test Environment

- Python: 3.12.12
- Primality backend: gmpy2
- Platform: Darwin arm64
- Total runtime: 0.3s

## 2. Per-Tile Statistics

| Tile | Location | Halo Primes | Ports I/O/L/R | Groups |
|------|----------|-------------|---------------|--------|
| small-radius | (100, 100) | 8076 | 1/1/1/1 | 1 |

## 3. Pair Statistics

| Pair | A ports | B ports | Shared (A.R / B.L) |
|------|---------|---------|-------------------|
| small-radius pair | 4 | 4 | 1 / 1 |

## 4. Validation Results (P1-P5)

### Single-Tile Properties

**small-radius** at (100, 100):

- P2: Group correctness: PASS
  INFO: All 4 ports have correct group assignments
  INFO: Unique groups: 1, unique components: 1
- P4: Port completeness: PASS
  INFO: All face primes covered by ports (1702 prime-port memberships)
- P5: Fingerprint uniqueness: PASS
  INFO: All fingerprints unique within each face

### Pair Properties

**small-radius pair**:

- P1: Port determinism: PASS
  INFO: Shared face: 1 ports, fingerprints match exactly
- P3: Composition correctness: PASS
  INFO: Spanning results match: True
  INFO: Combined region: 15060 primes
  INFO: Top boundary primes: 859, Bottom: 827

## 5. Summary

- **Total tests: 5**
- **Passed: 5**
- **Failed: 0**

All properties validated.

## 6. Notes

- K=36 (step distance^2 threshold)
