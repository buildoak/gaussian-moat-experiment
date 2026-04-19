---
title: Bad Zone Pre-Build Soundness Check
date: 2026-04-20
engine: codex
type: design-note
status: complete
refs: [methodology/lemmas_v2/campaign-blueprint.md, methodology/lemmas_v2/tile-operator-definition-v-claude.md, build/bz_check.py]
---

## Scope

`build/bz_check.py` implements the pre-build reconciliation check required by
the blueprint. It fails the build if a Gaussian-prime norm lies in either bad
zone for the selected `(R_inner, R_outer, K)`.

## Mathematical Definition

The canonical norm-form tests are:

```text
geo_I iff (n - R_inner^2 - K)^2 <= 4 R_inner^2 K
geo_O iff (R_outer^2 - n + K)^2 <= 4 R_outer^2 K
```

where `n = ||p||^2`. The witness-geometric definition can disagree with the
norm form only in the boundary intervals defined in the math SSoT:

```text
BZ_I = ((sqrt(R_inner^2 - 1) + sqrt(K))^2, (R_inner + sqrt(K))^2]
BZ_O = [(R_outer - sqrt(K))^2, (sqrt(R_outer^2 + 1) - sqrt(K))^2)
```

The script computes both endpoints with `mpmath` at 50 decimal digits,
enumerates integer norms in `BZ_I union BZ_O`, and classifies each integer
using the Gaussian-prime norm criterion:

```text
n = 2
or n is a rational prime with n = 1 mod 4
or n = p^2 for a rational prime p with p = 3 mod 4
```

Any such norm in the bad zone is a soundness blocker and returns non-zero.

## Precision Argument

At project scale, the endpoints are near `6.4e15`. A 50-digit `mpmath` value
has roughly 34 decimal digits of precision beyond the integer part, while the
observed endpoint distance to the nearest relevant integer is much larger:

- `K=40`: fractional parts are about `0.253881...` and `0.231750...`.
- `K=36`: the tightest case is about `7.5e-8` from an integer.

The script also enumerates a small guard window around the rounded endpoint
range and then re-filters candidates against the high-precision interval
predicate, so endpoint rounding would have to exceed multiple integer units to
miss a candidate. The 50-digit precision margin is therefore far beyond the
needed resolution for current parameters.

## Project Results

Command:

```bash
uv run build/bz_check.py --r-inner 80000000 --r-outer 80008192 --k-sq 40
```

Result:

```text
BZ_I candidates: [6400001011928891]
BZ_O candidates: [6401309775076432]
Gaussian-prime norms: none
PASS
```

Command:

```bash
uv run build/bz_check.py --r-inner 80000000 --r-outer 80008192 --k-sq 36
```

Result:

```text
BZ_I candidates: [6400000960000035, 6400000960000036]
BZ_O candidates: [6401309827010596]
Gaussian-prime norms: none
PASS
```

Both project parameter sets are BZ-prime-free.

## CMake Integration Pattern

Run the check as an always-built pre-build soundness gate. The concrete
`R_INNER`, `R_OUTER`, and `K_SQ` variable names should match the campaign
CMake configuration:

```cmake
find_program(UV_EXECUTABLE uv REQUIRED)

add_custom_target(
  bz_check ALL
  COMMAND ${UV_EXECUTABLE} run
          ${CMAKE_SOURCE_DIR}/build/bz_check.py
          --r-inner ${R_INNER}
          --r-outer ${R_OUTER}
          --k-sq ${K_SQ}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Checking BZ reconciliation for Gaussian-prime norms"
  VERBATIM
)

add_dependencies(tile_kernel bz_check)
```

Because `bz_check` is in `ALL`, a non-zero exit from the script fails the build
before campaign artifacts are produced.
