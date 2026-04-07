---
date: 2026-03-26
engine: codex
status: complete
---

# k²=40 Fat-Stripe Pipeline Audit

## Executive Summary

The Gaussian-primality and tile-composition core is substantially sound: the active `fat-stripe` path uses deterministic Miller-Rabin on `u64` norms, collar handling is consistent with `k²=40`, and the `moat-kernel` seam-composition tests all passed under `cargo test -p moat-kernel -p fat-stripe`. The main weaknesses are geometric and operational: the off-axis campaign probes are not rotated 128K annular strips, and the campaign script can silently checkpoint partial or failed probes as complete.

I also found a narrower but important modeling concern in the spanning verdict: the final `fat-stripe` decision only inspects component ports that survive on the outer rectangle faces, not true radial-face intersections. That is close to correct for the current thin strips, but it is not an exact annular-face test.

## 1. Code Correctness

### 1a. Sieve Correctness

**Verdict: PASS**

- `sieve_ext.rs` does not trust the row sieve as a final primality oracle; it uses `sieve_row` only as a composite prefilter and then calls `is_gaussian_prime(a, b)` on every surviving off-axis point and on every axis point, so the final classification is exact up to the correctness of `is_gaussian_prime` itself (`tile-probe/crates/fat-stripe/src/sieve_ext.rs:19-37`).
- `sieve_row` marks the correct Gaussian-composite residue classes:
  - parity handles the even-norm obstruction (`tile-probe/crates/moat-kernel/src/primality.rs:333-336`);
  - split primes `p ≡ 1 (mod 4)` use the two roots of `sqrt(-1) mod p` (`tile-probe/crates/moat-kernel/src/primality.rs:338-347`);
  - inert primes `p ≡ 3 (mod 4)` only mark `a ≡ b ≡ 0 (mod p)` (`tile-probe/crates/moat-kernel/src/primality.rs:349-354`);
  - small norms are unmarked when the norm itself is prime, preventing false composite marks on small Gaussian primes such as `1+i` (`tile-probe/crates/moat-kernel/src/primality.rs:356-375`).
- `is_gaussian_prime` implements the standard Gaussian-prime classification:
  - on axes, `|a|` or `|b|` must be a rational prime congruent to `3 mod 4` (`tile-probe/crates/moat-kernel/src/primality.rs:446-455`);
  - off axes, `a²+b²` must be a rational prime (`tile-probe/crates/moat-kernel/src/primality.rs:457-475`).
- For the active pipeline, the primality backend is deterministic on the full numeric range actually used. The code uses trial division by the first 168 primes plus Miller-Rabin with bases `2,3,5,7,11,13,17,19,23,29,31,37` (`tile-probe/crates/moat-kernel/src/primality.rs:3-16`, `tile-probe/crates/moat-kernel/src/primality.rs:382-443`). Sorenson-Webster’s `ψ12 = 318665857834031151167461` exceeds `u64::MAX`, so every `u64` norm in this pipeline is covered deterministically. The comment at `primality.rs:15-16` overstates the bound by quoting the larger `ψ13` range, but the implementation is still safe for all `u64`.
- Collar handling is consistent for `k²=40`: `collar = ceil(sqrt(40)) = 7` (`tile-probe/crates/fat-stripe/src/config.rs:31-47`), while the backward-offset set has size 64 and never needs coordinate differences larger than 6 (`tile-probe/crates/moat-kernel/src/scanline.rs:10-29`).

### 1b. Tile Composition

**Verdict: PASS**

- Tile construction is internally consistent:
  - each tile is built on an inclusive in-bounds box and is fed all primes from the collar-padded box (`tile-probe/crates/moat-kernel/src/tile.rs:295-370`);
  - face membership uses `<= collar`, which is the correct inclusive condition for perfect-square step bounds and is explicitly tested for `k²=4` and `k²=9` (`tile-probe/crates/moat-kernel/src/tile.rs:420-446`, `tile-probe/crates/moat-kernel/src/tile.rs:545-608`).
- Horizontal composition is structurally sound: it unions `left.face_right` against `right.face_left` using exact squared distances, then re-canonicalizes component IDs by union-find root (`tile-probe/crates/moat-kernel/src/compose.rs:71-205`).
- Vertical composition does the analogous operation on `bottom.face_outer` and `top.face_inner` (`tile-probe/crates/moat-kernel/src/compose.rs:208-340`).
- For the actual `k²=40` campaign geometry, this is enough to preserve connectivity across the whole `64 x 64` layout:
  - each 2000-wide stripe has one chunk because `chunk_size * tile_width = 2,000,000 > 128,000`, so runtime composition is exactly “64 horizontal tile merges per stripe, then 64 vertical stripe merges” (`tile-probe/crates/fat-stripe/src/orchestrator.rs:61-85`);
  - any seam-crossing edge has coordinate deltas at most 6, so both endpoints necessarily lie in the 7-cell face collars retained by the composed operator.
- Existing seam/UF tests cover the binary composition primitives and all passed locally under `cargo test -p moat-kernel -p fat-stripe`.

### 1c. Spanning Verdict

**Verdict: CONCERN**

- The off-axis geometric thresholds themselves are mathematically correct for the rectangle the code actually tiles. When `b_min > 0`, the minimum radius over the rectangle is at the lower-left corner and the maximum is at the upper-right corner, matching `sqrt(a_start²+b_min²)` and `sqrt(a_end²+b_max²)` in `orchestrator.rs` (`tile-probe/crates/fat-stripe/src/orchestrator.rs:123-136`).
- Backward compatibility for `b_min == 0` is preserved exactly, but by explicit branching rather than by geometric reduction: the code falls back to `(r_min, r_max)` in the `else` branch (`tile-probe/crates/fat-stripe/src/orchestrator.rs:130-133`). The geometric rectangle formula does **not** reduce to the on-axis thresholds when `b_max > 0`; the old behavior is being preserved intentionally.
- The material concern is that the final verdict only looks at component IDs attached to the four **external rectangle faces** of the fully composed operator (`tile-probe/crates/moat-kernel/src/tile.rs:168-183`, `tile-probe/crates/moat-kernel/src/compose.rs:124-177`, `tile-probe/crates/moat-kernel/src/compose.rs:261-314`, `tile-probe/crates/fat-stripe/src/orchestrator.rs:163-194`). It does **not** retain per-component radial extrema.
- That means the code is not checking “does this component intersect the inner radial face and the outer radial face?” exactly. It is checking “does this component have some surviving outer-rectangle face port whose radius is below/above the chosen thresholds?”
- On-axis, this approximation is very tight but not exact. With the campaign geometry `r_max = 1,050,064,000`, `b_max = 128,000`, `collar = 7`, the point `(a, b) = (1,050,063,991, 127,991)` lies 8 lattice units from both the right face and the top face, so it is not retained as any final face port, yet its radius still exceeds `r_max - 7`. Therefore `has_outer` in `orchestrator.rs:175-182` can miss some legitimate outer-threshold contacts. The gap is narrow, but it exists.
- Practical implication: `blocked=true` is still strong evidence, but it is not a mathematically exact annular-face test as currently implemented.

### 1d. Edge Cases

**Verdict: CONCERN**

- Integer-overflow risk at the audited radii is low:
  - norms are computed in `i128` and asserted to fit in `u64` before Miller-Rabin (`tile-probe/crates/moat-kernel/src/primality.rs:457-460`);
  - seam-distance checks also use `i128` temporaries (`tile-probe/crates/moat-kernel/src/compose.rs:17-21`);
  - at `R <= 1.3e9`, norms stay well below `2^64`.
- Floating-point precision is adequate for the current `f64` geometric thresholds. Around `1e18`, an `f64` ulp is 128 in squared-radius units, while changing radius by 1 at `R ~ 1e9` changes `r²` by about `2e9`. The threshold comparisons in `orchestrator.rs:137-138` and `orchestrator.rs:175-181` therefore have ample numeric headroom.
- Negative-`b` campaigns are not fully supported at the orchestration layer. Even if the lower layers handle negative coordinates correctly, `run_campaign` clamps the starting `b` to `max(b_min, 0)` (`tile-probe/crates/fat-stripe/src/orchestrator.rs:63`), so rectangles crossing the real axis are silently truncated.
- `FatStripeConfig::num_chunks()` and `total_tiles()` are logging helpers only, and both assume `b_min = 0`; they misreport off-axis runs and even on-axis runs they report per-stripe counts, not full-campaign counts (`tile-probe/crates/fat-stripe/src/config.rs:55-65`, `tile-probe/crates/fat-stripe/src/main.rs:98-109`). This does not change verdicts, but it does degrade operator visibility.

## 2. Campaign Script Audit

### 2a. Probe Geometry

**Verdict: FAIL**

- On-axis probe construction is internally consistent: the script sets `r_min = R - 64,000`, `r_max = R + 64,000`, and `b_max = 128,000`, which matches the 128K-by-128K Mac runs (`deploy/k40-verify-campaign.py:72-84`).
- Off-axis probe construction is **not** a rotated 128K annular strip. It is an axis-aligned square in global `(a,b)` coordinates:
  - `a_min/a_max = a_center ± 64,000`;
  - `b_min/b_max = b_center ± 64,000`
  (`deploy/k40-verify-campaign.py:86-100`).
- Because the square is not rotated into local radial/tangential coordinates, the actual radial thickness grows with angle. At `45°`, the Jetson log shows
  - `r_inner_geom = 1049909489.98`
  - `r_outer_geom = 1050090509.32`
  so the radial span is `181,019.34`, not `128,000` (`/tmp/k40-jetson-campaign.log:1512-1514`).
- The same inflation is already visible at smaller angles:
  - `5°`: span `138,668.86` (`/tmp/k40-jetson-campaign.log:1067-1069`);
  - `11°`: span `150,071.83` (`/tmp/k40-jetson-campaign.log:1156-1158`);
  - `22°`: span `166,629.18` (`/tmp/k40-jetson-campaign.log:1245-1247`).
- Practical implication: the phase-3 “isotropy” probes are not like-for-like comparisons with the on-axis 128K radial strip. Larger-angle probes are materially thicker in the radial direction, which can only make spanning harder.

### 2b. Off-Axis Angle Computation

**Verdict: PASS**

- The angle math itself is correct:
  - the script converts degrees to radians with `math.radians(self.theta_deg)` (`deploy/k40-verify-campaign.py:68-71`);
  - it then uses `a_center = R*cos(theta)` and `b_center = R*sin(theta)` (`deploy/k40-verify-campaign.py:87-88`).
- The logged 45° probe confirms the expected symmetry: `a_min = b_min = 742,398,120` and `a_max = b_max = 742,526,120`, centered on roughly `(742,462,120, 742,462,120)` as expected for `1.05e9 / sqrt(2)` (`/tmp/k40-jetson-campaign.log:1434-1514`).

### 2c. Resumability

**Verdict: FAIL**

- The checkpoint logic marks probes complete unconditionally, even after timeouts, exceptions, parse failures, or nonzero subprocess exits. The `completed.add(probe.uid)` and `save_checkpoint(completed)` calls sit outside the success path (`deploy/k40-verify-campaign.py:395-410`).
- Because `subprocess.run(...)` return codes are never checked (`deploy/k40-verify-campaign.py:335-356`), a probe can:
  - print a partial `campaign:` line,
  - fail later,
  - still be checkpointed as complete,
  - and then be skipped on resume.
- This is not hypothetical: the three degree probes logged the start of degree-stat sieving but never emitted the `DEGREE_STATS:` line that `fat-stripe` should print on success (`tile-probe/crates/fat-stripe/src/main.rs:136-183`), yet the campaign still recorded them as completed results (`/tmp/k40-jetson-campaign.log:1961-1963`, `/tmp/k40-jetson-campaign.log:2051-2053`, `/tmp/k40-jetson-campaign.log:2141-2143`).

### 2d. Output Parsing

**Verdict: FAIL**

- `parse_output()` only extracts three best-effort regexes and never validates that the subprocess actually succeeded (`deploy/k40-verify-campaign.py:230-257`, `deploy/k40-verify-campaign.py:335-356`).
- Missing or malformed output yields `blocked=None`, `tiles=None`, `elapsed_ms=None`, `spanning=None`, but that is only rendered as `PARSE_ERROR`; it does not abort or prevent checkpointing (`deploy/k40-verify-campaign.py:361-410`).
- The degree-probe handling is especially weak:
  - `main.rs` prints `DEGREE_STATS:` only after the expensive post-campaign computation finishes (`tile-probe/crates/fat-stripe/src/main.rs:136-183`);
  - the Jetson log contains the `degree-stats: sieving expanded region ...` stderr line, but never the matching `DEGREE_STATS:` stdout line (`/tmp/k40-jetson-campaign.log:1961-1963`, `/tmp/k40-jetson-campaign.log:2051-2053`, `/tmp/k40-jetson-campaign.log:2141-2143`);
  - the script accepts that silently.

## 3. Data Integrity

### 3a. Cross-Platform Consistency

**Verdict: PASS**

- The Mac note reports `R=1.05B, θ=0°` as `BLOCKED` in `5m12-13s` (`research/2026-03-25-k40-fat-stripe-experiment.md:43-47`).
- The Jetson raw log reports the same probe as `BLOCKED`, `spanning=0`, `elapsed=1334083ms` (`/tmp/k40-jetson-campaign.log:266-271`).
- The Mac hardware summary gives about `76 ms/tile`, while the Jetson standard probes average about `327 ms/tile`; that is a factor of about `4.3x`, which matches the document’s stated platform ratio (`research/2026-03-25-k40-fat-stripe-experiment.md:18-22`).

### 3b. Timing Sanity

**Verdict: PASS**

- I extracted all 21 standard 4096-tile probes from the Jetson log. Their wall times range from `1322s` to `1357s`, with mean `1340.3s` and coefficient of variation about `0.64%`.
- There are no 10x outliers or even 20% outliers; the full spread is only about `2.6%`. Representative endpoints are:
  - fastest standard probe: `P3-06` at `1322s` (`/tmp/k40-jetson-campaign.log:1515-1517`);
  - slowest standard probe: `P2-08` at `1357s` (`/tmp/k40-jetson-campaign.log:981-983`).
- The degree probes are much slower in wall time (`2363s` to `2390s`), but their embedded `elapsed_ms` values remain the same `~1334s` campaign times because `fat-stripe` prints the `campaign:` line **before** degree computation (`tile-probe/crates/fat-stripe/src/main.rs:121-130`, `tile-probe/crates/fat-stripe/src/main.rs:136-183`).

### 3c. Transition Sharpness

**Verdict: CONCERN**

- The available probes place the transition somewhere between `R=800M` and `R=850M`:
  - `800M` is `CONNECTED` (`/tmp/k40-jetson-campaign.log:978-983`);
  - `850M` is `BLOCKED` (`/tmp/k40-jetson-campaign.log:889-894`);
  - every sampled radius from `850M` through `1.30B` is blocked (`research/2026-03-25-k40-fat-stripe-experiment.md:76-116`, `research/2026-03-25-k40-fat-stripe-experiment.md:138-165`).
- That is a plausibly sharp percolation-style transition, but the resolution is still only 50M.
- Because the spanning predicate is not an exact radial-face test and the off-axis campaign geometry is not like-for-like, the current data support “there is a blocked regime by `850M`” more strongly than “the exact lower edge lies in `[800M, 850M)`.”

### 3d. Spanning Counts

**Verdict: PASS**

- Every `CONNECTED` probe in the Jetson log has `spanning=1` (`/tmp/k40-jetson-campaign.log:88-91`, `/tmp/k40-jetson-campaign.log:178-181`, `/tmp/k40-jetson-campaign.log:980-983`).
- Every `BLOCKED` probe has `spanning=0` in both the raw log and the experiment note (`research/2026-03-25-k40-fat-stripe-experiment.md:68-116`).
- Those counts are physically plausible: a single dominant spanning component below the transition is exactly what one expects from a strongly connected regime.

### 3e. Degree Statistics

**Verdict: CONCERN**

- The Mac document’s degree values `3.92`, `3.97`, `4.03` for `k² = 32,36,40` are physically reasonable for a sparse 2D geometric graph approaching a percolation-style transition (`research/2026-03-25-k40-fat-stripe-experiment.md:120-128`). Nothing about those values looks numerically pathological.
- However, the Jetson degree probes do **not** independently confirm those numbers. The raw log never contains a `DEGREE_STATS:` line for `P4-05`/`P4-06`/`P4-07`, despite `main.rs` printing one on success (`tile-probe/crates/fat-stripe/src/main.rs:174-183`; `/tmp/k40-jetson-campaign.log:1961-1963`, `/tmp/k40-jetson-campaign.log:2051-2053`, `/tmp/k40-jetson-campaign.log:2141-2143`).
- So the document’s statement that the Jetson issue was merely a “parsing gap” is too weak. The captured output is incomplete, and the script did not detect that.

## 4. Pipeline Risks

### 4a. False Positive Risk (Composite classified as prime)

**Verdict: PASS**

- For the active path, false positives from primality testing are low risk. `is_gaussian_prime` is deterministic on every `u64` norm used by the campaign (`tile-probe/crates/moat-kernel/src/primality.rs:396-475`).
- Residual note: the bound comment at `primality.rs:15-16` should be corrected to the actual `ψ12` bound from Sorenson-Webster, but that is a documentation issue, not a correctness defect for `u64`.

### 4b. False Negative Risk (Reported BLOCKED though a path exists)

**Verdict: CONCERN**

- Tile width `2000` is **not** the limiting factor. Composition carries connectivity across arbitrarily many tiles, so a valid detour can span the whole `64 x 64` layout if it stays inside the sampled strip (`tile-probe/crates/moat-kernel/src/compose.rs:71-340`).
- The real risks are:
  - the face-port-only spanning predicate discussed in 1c;
  - the finite strip width (`128K`);
  - the off-axis geometry mismatch.
- The user’s heuristic `sqrt(R) * log(R)` at `R ~ 10^9` is about `6.5e5`, which is larger than `128K`. That heuristic is not a proof about detour lengths, but it is enough to say the strip width is not obviously “too large to matter.”

### 4c. Resolution Bias

**Verdict: CONCERN**

- A single `128K x 128K` sample is a tiny fraction of the full annulus, so some angular-resolution bias is unavoidable.
- Multi-angle probing helps, but the current phase-3 off-axis runs are not geometrically comparable to the on-axis run because they use thicker axis-aligned squares rather than rotated constant-width strips (`deploy/k40-verify-campaign.py:86-100`; `/tmp/k40-jetson-campaign.log:1067-1069`, `/tmp/k40-jetson-campaign.log:1512-1514`).
- Therefore the isotropy evidence is suggestive, not yet definitive.

### 4d. Off-Axis Fix Correctness at Scale

**Verdict: PASS**

- The `bda7580` fix correctly computes geometric corner radii in `f64` for the rectangle that is actually sampled (`tile-probe/crates/fat-stripe/src/orchestrator.rs:123-136`).
- At `R ~ 10^9`, `f64` precision is easily sufficient for these comparisons.
- The important caveat is geometric, not numeric: the fix is correct for the current axis-aligned square model, but that model is not the same as a rotated 128K annular strip.

## Risk Assessment Matrix

| Risk | Likelihood | Impact | Evidence |
|---|---|---|---|
| Deterministic primality failure on active `fat-stripe` path | Low | High | `primality.rs:396-475`; Sorenson-Webster `ψ12 > u64::MAX` |
| Off-axis isotropy claim biased by wrong probe geometry | High | High | `deploy/k40-verify-campaign.py:86-100`; `/tmp/k40-jetson-campaign.log:1067-1069`, `/tmp/k40-jetson-campaign.log:1512-1514` |
| Partial/failed probes silently checkpointed as complete | High | High | `deploy/k40-verify-campaign.py:335-410` |
| Degree probes accepted despite missing degree output | High | Medium | `main.rs:136-183`; `/tmp/k40-jetson-campaign.log:1961-1963`, `/tmp/k40-jetson-campaign.log:2051-2053`, `/tmp/k40-jetson-campaign.log:2141-2143` |
| Radial spanning undercounted because only external face ports are retained | Medium | High | `tile.rs:168-183`; `compose.rs:124-177`; `orchestrator.rs:168-184` |
| Operator-facing tile/chunk counts misleading in logs | High | Low | `config.rs:55-65`; `main.rs:98-109`; `/tmp/k40-jetson-campaign.log:998-1001`, `/tmp/k40-jetson-campaign.log:1070-1072` |

## Overall Confidence

**MEDIUM**

Confidence is high that the core Gaussian-prime identification is correct and that the observed on-axis `BLOCKED` runs are not artifacts of an obvious sieve or seam-composition bug. Confidence is not high enough for a discovery claim covering exact lower-bound location, angle-independent 128K annular blockage, or the Jetson degree-profile evidence, because the off-axis campaign geometry is materially wrong for that interpretation and the campaign script can silently accept incomplete runs.

## Recommended Follow-Up Actions

1. Make the campaign script fail hard unless `returncode == 0`, the `campaign:` line parses, and `DEGREE_STATS:` is present when `--degree-stats` was requested.
2. Replace the off-axis square construction with a rotated local radial/tangential window, or explicitly rename the phase-3 experiment as an axis-aligned square probe rather than an annular-strip isotropy test.
3. Extend `TileOperator` to carry per-component `min_r_sq` / `max_r_sq` (or equivalent radial-band bits) through composition, so the final spanning verdict is exact and does not depend on surviving outer-face ports.
4. Re-run the off-axis and degree probes after fixes 1-3 before making isotropy or degree-threshold claims.
5. Correct the logging/documentation mismatches:
   - `primality.rs:15-16` witness-bound comment;
   - `config.rs:55-65` / `main.rs:98-109` tile-count reporting;
   - `main.rs:49-51` `b_max` help text versus the actual default at `main.rs:75`.
