---
date: 2026-03-19
engine: coordinator
type: probe-results-recovery
status: partial
source: session-c03970d5-transcript-recovery
note: Results observed during SSH session but not exfiltrated. Recovered from conversation transcript.
---

# sqrt(40) Gaussian Moat Probe Results — Transcript Recovery

## Recovery Context

Results were produced on a vast.ai RTX 3090 instance (ID 33123347, $0.128/hr, 257 GB RAM, Quebec).
The JSONL log (`research/results/sqrt40-bracket-log.jsonl`) and `.txt` log were written on the remote instance
but the instance was destroyed before results were exfiltrated. This document recovers all probe data
from the conversation transcript of session `c03970d5-3ded-49d7-9cd5-edd7d966a5d0`.

**Solver configuration:**
- k² = 40 (step distance sqrt(40) ~ 6.324)
- K = 7 (band padding only; actual neighbor check uses dist_sq <= 40)
- 8 angular wedges
- Band: [(D-7)², (D+7)²]
- CUDA sieve (RTX 3090) + Rust union-find solver with path compression
- Deterministic, reproducible (ran twice with identical output per transcript)

**Session metadata (exact, from remote):**
```json
{
  "gpu": "NVIDIA GeForce RTX 3090",
  "driver_version": "550.90.07",
  "cuda_version": "Build cuda_12.4.r12.4/compiler.34097967_0",
  "sieve_sha256": "0e3da90d...",
  "solver_sha256": "8e517b8c...",
  "git_commit": "65dc2f70...",
  "k_squared": 40,
  "instance_id": 33123347
}
```
Paper-quality logging commit: `0dd7e32`

---

## Phase 0 — Solver Validation (k²=36)

**Purpose:** Validate solver correctness before k²=40 probes. k²=36 Tsuchimura UB = 80,015,782.

| Field | Value | Precision |
|-------|-------|-----------|
| k² | 36 | exact |
| Probe D | 85M | exact |
| Result | SURVIVED | exact |
| Primes in band | 32.6M | approximate (from summary) |
| Component size | 30.2M | approximate (from summary) |
| Component ratio | 92.6% | approximate |
| Sieve time | 6.4s | exact (from log) |
| Solver time | 15.3s | exact (from log) |
| Total time | ~22s | derived |

**Interpretation:** Component survived at D=85M > Tsuchimura UB of 80M, as expected. Solver is producing correct results.

---

## Phase 1 — Initial Bracket Probes (k²=40)

Five probes at geometric distances. All probes completed successfully.
Probe order as executed: 200M, 400M, 100M, 50M, 800M.

### Probe 1: D = 50,000,007

| Field | Value | Precision |
|-------|-------|-----------|
| Probe D | 50,000,007 | exact |
| Result | **SURVIVED** | exact |
| Primes in band | 19,742,631 | exact (from GPT-5.4 Pro prompt table) |
| Component size | 18,932,531 | exact (from GPT-5.4 Pro prompt table) |
| Component ratio | 95.9% | exact |
| Total time | 13s | approximate (from summary table) |
| Farthest point | — | not recorded in transcript |

### Probe 2: D = 100,000,007

| Field | Value | Precision |
|-------|-------|-----------|
| Probe D | 100,000,007 | exact |
| Result | **SURVIVED** | exact |
| Primes in band | 38,008,141 | exact (from GPT-5.4 Pro prompt table) |
| Component size | 36,203,933 | exact (from GPT-5.4 Pro prompt table) |
| Component ratio | 95.3% | exact |
| Total time | 28s | approximate (from summary table) |
| Farthest point | — | not recorded in transcript |

### Probe 3: D = 200,000,007

| Field | Value | Precision |
|-------|-------|-----------|
| Probe D | 200,000,007 | exact |
| Result | **SURVIVED** | exact |
| Primes in band | 73,246,724 | exact (from GPT-5.4 Pro prompt table) |
| Component size | 69,270,558 | exact (from GPT-5.4 Pro prompt table) |
| Component ratio | 94.6% | exact |
| Farthest point | (195285395, 43169634) at distance 200,000,007 | exact (from first-result report) |
| Sieve time | 17.8s | exact (from first-result report) |
| Solver time | 39.1s | exact (from first-result report) |
| Total time | 57s | exact (55-57s across reports) |
| RAM | 4.4 GB | exact (from first-result report) |

**Note:** First probe executed. Farthest point hit band edge exactly. "69.3M" rounded in summaries; 69,270,558 is the exact count from the GPT-5.4 Pro prompt table (which was constructed from the JSONL log).

### Probe 4: D = 400,000,007

| Field | Value | Precision |
|-------|-------|-----------|
| Probe D | 400,000,007 | exact |
| Result | **SURVIVED** | exact |
| Primes in band | 141,372,277 | exact (from GPT-5.4 Pro prompt table) |
| Component size | 132,720,729 | exact (from GPT-5.4 Pro prompt table) |
| Component ratio | 93.9% | exact |
| Farthest point | (349258268, 194983763) at distance 400,000,007 | exact (from second-result report) |
| Sieve time | 38s | exact (from second-result report) |
| Solver time | 83s | exact (from second-result report) |
| Total time | 121s (119-121s across reports) | exact |
| RAM | 8.5 GB | exact (from second-result report) |

### Probe 5: D = 800,000,007

| Field | Value | Precision |
|-------|-------|-----------|
| Probe D | 800,000,007 | exact |
| Result | **SURVIVED** | exact |
| Primes in band | 273,172,316 | exact (from GPT-5.4 Pro prompt table) |
| Component size | 127,272,055 | exact (from GPT-5.4 Pro prompt table) |
| Component ratio | 46.6% | exact |
| Farthest point | — | not individually reported, but "hit band edge" per summary |
| Total time | 251s | approximate (from summary table) |
| RAM | ~8.5 GB | approximate (mentioned in context of 800M probe) |

**Critical observation:** Component ratio dropped from 93.9% at D=400M to 46.6% at D=800M. This is the first sign of connectivity thinning. The organic connections (primes with norm > D²) are barely bridging the upper half of the band.

---

## Phase 1 Summary Table (exact values from GPT-5.4 Pro prompt, sourced from JSONL log)

| Probe D | Primes in band | Origin component size | Ratio | Status | Time |
|---------|---------------|----------------------|-------|--------|------|
| 50,000,007 | 19,742,631 | 18,932,531 | 95.9% | SURVIVED | 13s |
| 100,000,007 | 38,008,141 | 36,203,933 | 95.3% | SURVIVED | 28s |
| 200,000,007 | 73,246,724 | 69,270,558 | 94.6% | SURVIVED | 57s |
| 400,000,007 | 141,372,277 | 132,720,729 | 93.9% | SURVIVED | 121s |
| 800,000,007 | 273,172,316 | 127,272,055 | 46.6% | SURVIVED | 251s |

**Reproducibility note:** Transcript states "reproducible, ran twice with identical output."

---

## Phase 2 — Extended Bracket Probes (k²=40)

Launched after Phase 1 showed moat is beyond 800M. Four probes queued: 1.2B, 1.6B, 3.2B, 6.4B.

### Probe 6: D = 1,200,000,007 (estimated)

| Field | Value | Precision |
|-------|-------|-----------|
| Probe D | ~1.2B | approximate (mentioned as "1.2B" in launch list) |
| Result | **SURVIVED** (implied) | inferred — 1.6B survived so 1.2B must have too |
| Details | — | No individual report in transcript |

**Note:** The 1.2B probe was listed in the Phase 2 launch but no individual result was reported. Since 1.6B survived, 1.2B must have survived (the probes ran sequentially: 1.2B, 1.6B, 3.2B, 6.4B). No exact counts recovered.

### Probe 7: D = 1,600,000,007 (estimated)

| Field | Value | Precision |
|-------|-------|-----------|
| Probe D | ~1.6B | approximate (reported as "1.6B") |
| Result | **SURVIVED** | exact |
| Primes in band | 528.5M (528,500,000 approx) | approximate (from summary) |
| Component size | 488.5M (488,500,000 approx) | approximate (from summary) |
| Component ratio | 92.5% | exact |
| Sieve time | 193s | exact (from report) |
| Solver time | 353s | exact (from report) |
| Total time | 546s (~9 min) | exact |
| RSS (system RAM) | 31 GB | exact (from report) |

**Critical observation:** Ratio RECOVERED from 46.6% at 800M back to 92.5% at 1.6B. This was flagged as unexpected. The connectivity got stronger, not weaker. Possible explanations discussed:
1. The 800M probe hit a local thin spot that the component routes around at larger scales
2. The 800M result itself may be anomalous
3. Non-monotonic ratio behavior as a function of D

### Probe 8: D = 3,200,000,007 (estimated)

| Field | Value | Precision |
|-------|-------|-----------|
| Probe D | ~3.2B | approximate |
| Result | **CRASHED — DISK FULL** | exact |
| Cause | 20 GB disk total, 1.6B sieve file was 8.4 GB, 3.2B needed ~16 GB but only 8 GB free | exact |
| Sieve status | Started but write failed | exact |
| Solver status | Never ran | exact |

**Root cause:** vast.ai instance had only 20 GB disk. After 1.6B probe, the prime file (`/tmp/ub-probe.gprf`, 8.4 GB) consumed available space. The 3.2B sieve write failed, crashing the script and killing the tmux session. RAM (158 GB free of 251 GB) and GPU were fine.

**Recovery attempt:** Old prime file was deleted, freeing 15.8 GB. Estimate for 3.2B prime file was ~16 GB — razor thin margin. Disk resize not possible on running vast.ai instance.

### Probe 9: D = 6,400,000,007 (estimated)

| Field | Value | Precision |
|-------|-------|-----------|
| Probe D | ~6.4B | — |
| Result | **NEVER EXECUTED** | exact |
| Cause | Predecessor (3.2B) crashed; would need ~32 GB disk (impossible on 20 GB instance) | exact |

---

## Phase 2 Summary

| Probe D | Primes | Component | Ratio | Status | Time | Notes |
|---------|--------|-----------|-------|--------|------|-------|
| ~1.2B | — | — | — | SURVIVED (inferred) | — | No individual report |
| ~1.6B | 528.5M | 488.5M | 92.5% | SURVIVED | 546s | Ratio recovered from 800M dip |
| ~3.2B | — | — | — | CRASHED (disk full) | — | Sieve write failed |
| ~6.4B | — | — | — | NEVER EXECUTED | — | Blocked by disk |

---

## Combined Results Table

| # | Phase | Probe D | Primes | Component | Ratio | Status | Time |
|---|-------|---------|--------|-----------|-------|--------|------|
| 0 | Phase 0 | 85M (k²=36) | 32.6M | 30.2M | 92.6% | SURVIVED | 22s |
| 1 | Phase 1 | 50,000,007 | 19,742,631 | 18,932,531 | 95.9% | SURVIVED | 13s |
| 2 | Phase 1 | 100,000,007 | 38,008,141 | 36,203,933 | 95.3% | SURVIVED | 28s |
| 3 | Phase 1 | 200,000,007 | 73,246,724 | 69,270,558 | 94.6% | SURVIVED | 57s |
| 4 | Phase 1 | 400,000,007 | 141,372,277 | 132,720,729 | 93.9% | SURVIVED | 121s |
| 5 | Phase 1 | 800,000,007 | 273,172,316 | 127,272,055 | 46.6% | SURVIVED | 251s |
| 6 | Phase 2 | ~1.2B | — | — | — | SURVIVED (inferred) | — |
| 7 | Phase 2 | ~1.6B | ~528.5M | ~488.5M | 92.5% | SURVIVED | 546s |
| 8 | Phase 2 | ~3.2B | — | — | — | CRASHED (disk) | — |
| 9 | Phase 2 | ~6.4B | — | — | — | NOT EXECUTED | — |

---

## Anomalies and Open Questions

### 1. The 800M Ratio Dip (46.6%)

The component ratio follows a smooth decline from 95.9% (50M) to 93.9% (400M), then drops sharply to 46.6% at 800M, before recovering to 92.5% at 1.6B. Three hypotheses from the session:

1. **Local thin spot:** The 800M band happens to contain a region of locally sparse primes that fragments the component. At 1.6B the component has more room to route around it.
2. **Anomalous probe:** Something specific to the D=800M band geometry causes the ratio metric to be misleading.
3. **Non-monotonic connectivity:** The ratio as a function of D may genuinely be non-monotonic, with dips and recoveries before final termination.

This anomaly is the single most important unresolved data point. It needs to be reproduced on a fresh instance to confirm it's not a fluke.

### 2. Missing Exact Counts for 50M, 100M, 800M

The Phase 1 summary (line 804 in transcript) reports 50M, 100M, and 800M without component size. However, the GPT-5.4 Pro prompt table (line 845) contains exact counts for all five probes including 800M (127,272,055). The 50M and 100M exact counts (18,932,531 and 36,203,933) also appear there. These were sourced from the JSONL log on the remote instance.

### 3. GPT-5.4 Pro Revised Prediction

After seeing Phase 1 results (800M survived), GPT-5.4 Pro revised its prediction toward 1.1-1.2B (per line 899 reference). The full reasoning trace for this revision was logged but the session ended before it could be exfiltrated as a structured document.

### 4. Campaign Instance Status at Session End

At session end (line 899-906): original instances were dead, instance 33123347 was alive ($0.128/hr) but clean (no campaign deployed). Probe results on the original compute instance were lost with instance destruction. This document IS the recovery.

---

## Predictions vs Reality

| Source | Central Prediction | Status After Probes |
|--------|-------------------|---------------------|
| MiniMax M2.5 | 15M | ELIMINATED (survived 50M) |
| Empirical scaling | 35M | ELIMINATED (survived 50M) |
| External ensemble (batch 1) | 40M | ELIMINATED (survived 50M) |
| Origin-lineage survival | 100M | ELIMINATED (survived 200M) |
| DeepSeek Terminus | 120M | ELIMINATED (survived 200M) |
| MiMo v2 Pro | 180M | ELIMINATED (survived 200M) |
| Grok 4.1 Fast | 190M | ELIMINATED (survived 200M) |
| Codex swarm (Bayesian blend) | 227M | ELIMINATED (survived 400M) |
| GLM-5 | 280M | ELIMINATED (survived 400M) |
| Calibrated percolation | 300M | ELIMINATED (survived 400M) |
| Ensemble consensus | 300M | ELIMINATED (survived 400M) |
| Pre-campaign final verdict | 300M | ELIMINATED (survived 400M) |
| Grok 4.20 Beta | 400M | ELIMINATED (survived 800M) |
| Gemini Flash | 420M | ELIMINATED (survived 800M) |
| GPT-5.4 Pro (z_eff, original) | 450M | ELIMINATED (survived 800M) |
| GPT-5.4 Pro (revised) | ~1.1-1.2B | STILL ALIVE |
| Tsuchimura naive formula | 1.17B | STILL ALIVE |
| Opus auditor 90% CI upper | 3B | STILL ALIVE |

**Bottom line:** The moat is confirmed beyond 1.6 billion. Every prediction model under-predicted, most dramatically. Only the upper tails of the widest confidence intervals remain viable. The 3.2B and 6.4B probes are needed to narrow the bracket.

---

## Infrastructure Notes

- **First campaign attempt:** Used `--sweep-mode sweep` with `ceiling=2000000000`, which triggered O(n/36) chunk planning (~50M chunks). CPU at 100%, zero GPU activity, zero probes executed. Root cause: sweep mode is for Phase 3 exact resolution, not Phase 1 bracketing.
- **Fix:** Killed sweep, relaunched with direct binary calls (one sieve + one solver per probe). Reduced from 47 hours to ~2 minutes per probe.
- **Disk bottleneck:** 20 GB disk allocation was sufficient through 1.6B but failed at 3.2B. Prime file sizes scale linearly with D (~5 bytes per prime). Future instances need 50+ GB disk for probes beyond 2B.
- **RAM vs VRAM:** Solver is CPU-bound (Rust union-find), not GPU-bound. At 1.6B, RSS was 31 GB system RAM, well within the 257 GB available. GPU VRAM (24 GB) used only during sieve phase.
