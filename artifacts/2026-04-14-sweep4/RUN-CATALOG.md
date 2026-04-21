# K_SQ=40 Run Catalog — RTX 4090 Campaign

**Dates:** 2026-04-13 17:13 UTC to 2026-04-14 ~10:00 UTC
**Total unique R values tested:** 68 (including 3 OOM failures)

## Provenance

| Item | Value |
|------|-------|
| Code version | `c73085faa7f5dbcfc512a92ab25cd5fd5ccd1622` |
| Binary (campaign) | MD5 `d701460a08f2690743c584f3e721e770` |
| GPU | NVIDIA GeForce RTX 4090 (sm_89, 24GB GDDR6X) |
| Driver | 590.48.01 |
| CUDA runtime | 13.1 |
| nvcc | 12.4 (Build cuda_12.4.r12.4/compiler.34097967_0) |
| Platform | Vast.ai, instance 34629150, `ssh7.vast.ai:16704` |

**Source hash verification** (remote deployed = local HEAD):

| File | MD5 |
|------|-----|
| tiles-compositor/src/campaign.cpp | `14d5832013f2c2e0bee1f1bc0bf6ea12` |
| tiles-compositor/src/compositor.cpp | `748aea9a2ae2c8b0d9e72c1aacc41c3b` |
| tile_cuda_multi_kernel/src/kernel_face_encode.cu | `b63cacdfc4f288f844acd7957b1413a6` |
| tile_cuda_multi_kernel/src/kernel_uf.cu | `9df181fe74fe5b987ca8a6f03dcbba10` |
| tile_cuda_multi_kernel/src/kernel_sieve.cu | `17f3b5cd88c5ba9b30e99c0468264fbc` |

## Build Parameters

| Parameter | Value | Source |
|-----------|-------|--------|
| K_SQ | 40 | CMake `-DK_SQ=40` |
| burst_size | 28,000 | CLI `--burst-size 28000` |
| STREAM_CHUNK_SIZE | 200,000 | Compiled into CUDA binary |
| Mode | `--cuda-stream` | Fixed-chunk streaming pipeline |
| progress_interval | 1,000 (sweep1/2) or 5,000 (sweep4) | CLI flag |

## Sweep Chronology

### Sweep 1 — Coarse R-Sweep
- **When:** 2026-04-13, 17:13–19:10 UTC (~2h)
- **Runs:** 10 (R = 875M to 1125M at 25M steps)
- **Finding:** First MOAT at R=900M. R=950M anomaly (instant SPANNING).
- **Data:** `sweep-intermediate.jsonl` lines 1–150, `logs/sweep-full.log`

### Sweep 2 — Transition Refinement
- **When:** 2026-04-13, 20:19–22:36 UTC (~2.3h)
- **Runs:** 14 (R = 650–960M, variable steps)
- **Finding:** Sub-transition all SPANNING (650–850M). First MOAT island at R=860M inferred. Transition detail: 880M MOAT, 885M SPANNING, 890M MOAT, 895M SPANNING. 950M reproduced as SPANNING. 940/945/955/960 all MOAT.
- **Data:** `sweep-intermediate.jsonl` lines 151–360, `logs/campaign-R*.log`, `logs/sweep-summary.txt`

### Sweep 4 — Overnight Campaign
- **When:** 2026-04-13 23:40 – 2026-04-14 ~10:00 UTC (~10h)
- **Runs:** 47 (R = 860M–2500M)
- **Blocks:**
  - A: Transition fine-grain 876–899M at 1M steps (20 runs)
  - B: 950M anomaly bracket 946–954M at 1M steps (8 runs)
  - C: High-R push 1.2B–2.5B (8 runs, 3 OOM)
  - D: Gap fill 905–935M at 5M steps (6 runs)
  - E: Sub-transition 860–870M at 5M steps (3 runs)
  - F: Remaining gaps 965M, 970M (2 runs)
- **Data:** `sweep4-results.jsonl`, `sweep4-summary.txt`, `sweep4-console.log`

## Complete Run Table

All 68 R values, sorted ascending. Zero overflows across all runs.

| R (M) | Verdict | Wall (s) | Total Tiles | Tiles Proc. | Towers % | Peak RSS (MB) | Source |
|------:|---------|----------|-------------|-------------|----------|---------------|--------|
| 650 | SPANNING | 7.9 | 64,844,433 | 165,032 | 0.3% | 451 | sweep2 |
| 700 | SPANNING | 8.0 | 69,832,388 | 257,267 | 0.4% | 479 | sweep2 |
| 750 | SPANNING | 8.2 | 74,820,299 | 357,818 | 0.5% | 505 | sweep2 |
| 800 | SPANNING | 15.5 | 79,808,256 | 1,094,477 | 1.5% | 561 | sweep2 |
| 850 | SPANNING | 31.0 | 84,796,209 | 2,855,390 | 3.7% | 679 | sweep2 |
| 860 | MOAT | 720.5 | 85,793,783 | 85,793,783 | 100% | 6,723 | sweep4 |
| 865 | MOAT | 723.5 | 86,292,593 | 86,292,593 | 100% | 6,763 | sweep4 |
| 870 | MOAT | 710.0 | 86,791,403 | 86,791,403 | 100% | 6,794 | sweep4 |
| 875 | SPANNING | 188.8 | 87,290,165 | 22,852,103 | 28.6% | 1,862 | sweep1 |
| 876 | SPANNING | 551.0 | 87,389,962 | 66,022,578 | 79.0% | 4,728 | sweep4 |
| 877 | MOAT | 722.9 | 87,489,716 | 87,489,716 | 100% | 6,846 | sweep4 |
| 878 | MOAT | 726.9 | 87,589,469 | 87,589,469 | 100% | 6,863 | sweep4 |
| 879 | MOAT | 724.8 | 87,689,222 | 87,689,222 | 100% | 6,864 | sweep4 |
| 880 | MOAT | 752.2 | 87,788,973 | 87,788,973 | 100% | 6,873 | sweep2 |
| 881 | MOAT | 721.6 | 87,888,728 | 87,888,728 | 100% | 6,884 | sweep4 |
| 882 | MOAT | 733.9 | 87,988,479 | 87,988,479 | 100% | 6,891 | sweep4 |
| 883 | MOAT | 719.7 | 88,088,233 | 88,088,233 | 100% | 6,903 | sweep4 |
| 884 | SPANNING | 8.1 | 88,188,031 | 527,801 | 0.7% | 570 | sweep4 |
| 885 | SPANNING | 133.8 | 88,287,783 | 14,898,410 | 18.4% | 1,356 | sweep2 |
| 886 | MOAT | 726.0 | 88,387,537 | 88,387,537 | 100% | 6,921 | sweep4 |
| 887 | MOAT | 734.4 | 88,487,290 | 88,487,290 | 100% | 6,934 | sweep4 |
| 888 | SPANNING | 552.4 | 88,587,042 | 67,240,944 | 79.3% | 4,813 | sweep4 |
| 889 | MOAT | 737.1 | 88,686,796 | 88,686,796 | 100% | 6,941 | sweep4 |
| 890 | MOAT | 754.4 | 88,786,549 | 88,786,549 | 100% | 6,947 | sweep2 |
| 891 | MOAT | 727.9 | 88,886,346 | 88,886,346 | 100% | 6,960 | sweep4 |
| 892 | MOAT | 725.6 | 88,986,099 | 88,986,099 | 100% | 6,971 | sweep4 |
| 893 | SPANNING | 363.4 | 89,085,853 | 44,928,214 | 54.4% | 3,330 | sweep4 |
| 894 | MOAT | 733.9 | 89,185,606 | 89,185,606 | 100% | 6,972 | sweep4 |
| 895 | SPANNING | 306.2 | 89,285,357 | 35,341,211 | 43.1% | 2,695 | sweep2 |
| 896 | SPANNING | 422.9 | 89,385,110 | 51,726,341 | 62.0% | 3,775 | sweep4 |
| 897 | MOAT | 737.9 | 89,484,863 | 89,484,863 | 100% | 7,009 | sweep4 |
| 898 | MOAT | 733.9 | 89,584,661 | 89,584,661 | 100% | 7,008 | sweep4 |
| 899 | SPANNING | 16.1 | 89,684,414 | 1,763,321 | 2.1% | 645 | sweep4 |
| 900 | MOAT | 756.5 | 89,784,166 | 89,784,166 | 100% | 7,008 | sweep1 |
| 905 | MOAT | 761.6 | 90,282,931 | 90,282,931 | 100% | 7,064 | sweep4 |
| 910 | MOAT | 766.6 | 90,781,741 | 90,781,741 | 100% | 7,099 | sweep4 |
| 915 | MOAT | 779.0 | 91,280,550 | 91,280,550 | 100% | 7,133 | sweep4 |
| 920 | MOAT | 782.3 | 91,779,313 | 91,779,313 | 100% | 7,169 | sweep4 |
| 925 | MOAT | 774.4 | 92,278,122 | 92,278,122 | 100% | 7,203 | sweep1 |
| 930 | MOAT | 769.8 | 92,776,931 | 92,776,931 | 100% | 7,240 | sweep4 |
| 935 | MOAT | 770.4 | 93,275,740 | 93,275,740 | 100% | 7,282 | sweep4 |
| 940 | MOAT | 788.6 | 93,774,503 | 93,774,503 | 100% | 7,326 | sweep2 |
| 945 | MOAT | 787.7 | 94,273,313 | 94,273,313 | 100% | 7,367 | sweep2 |
| 946 | MOAT | 779.9 | 94,373,066 | 94,373,066 | 100% | 7,367 | sweep4 |
| 947 | MOAT | 770.1 | 94,472,817 | 94,472,817 | 100% | 7,373 | sweep4 |
| 948 | MOAT | 773.4 | 94,572,570 | 94,572,570 | 100% | 7,386 | sweep4 |
| 949 | MOAT | 781.4 | 94,672,325 | 94,672,325 | 100% | 7,390 | sweep4 |
| 950 | SPANNING | 7.6 | 94,772,122 | 62,303 | 0.07% | 578 | sweep1+2 |
| 951 | MOAT | 770.1 | 94,871,874 | 94,871,874 | 100% | 7,406 | sweep4 |
| 952 | MOAT | 781.0 | 94,971,628 | 94,971,628 | 100% | 7,414 | sweep4 |
| 953 | MOAT | 780.0 | 95,071,381 | 95,071,381 | 100% | 7,429 | sweep4 |
| 954 | MOAT | 772.1 | 95,171,134 | 95,171,134 | 100% | 7,420 | sweep4 |
| 955 | MOAT | 785.1 | 95,270,887 | 95,270,887 | 100% | 7,435 | sweep2 |
| 960 | MOAT | 792.2 | 95,769,695 | 95,769,695 | 100% | 7,476 | sweep2 |
| 965 | MOAT | 791.4 | 96,268,506 | 96,268,506 | 100% | — | sweep4 |
| 970 | MOAT | 792.1 | 96,767,267 | 96,767,267 | 100% | — | sweep4 |
| 975 | MOAT | 814.2 | 97,266,079 | 97,266,079 | 100% | 7,577 | sweep1 |
| 1025 | MOAT | 849.9 | 102,254,036 | 102,254,036 | 100% | 7,955 | sweep1 |
| 1050 | MOAT | 879.1 | 104,747,989 | 104,747,989 | 100% | 8,147 | sweep1 |
| 1075 | MOAT | 880.1 | 107,241,989 | 107,241,989 | 100% | 8,336 | sweep1 |
| 1100 | MOAT | 916.9 | 109,735,945 | 109,735,945 | 100% | 8,524 | sweep1 |
| 1125 | MOAT | 933.0 | 112,229,900 | 112,229,900 | 100% | 8,705 | sweep1 |
| 1200 | MOAT | 990.1 | 119,711,857 | 119,711,857 | 100% | 9,276 | sweep4 |
| 1300 | MOAT | 1,055.2 | 129,687,723 | 129,687,723 | 100% | 10,018 | sweep4 |
| 1400 | MOAT | 1,150.3 | 139,663,634 | 139,663,634 | 100% | 10,760 | sweep4 |
| 1500 | MOAT | 1,255.3 | 149,639,500 | 149,639,500 | 100% | 11,526 | sweep4 |
| 1750 | MOAT | 1,445.5 | 174,579,234 | 174,579,234 | 100% | 13,624 | sweep4 |
| 2000 | OOM | 1,441.1 | — | 173,510,878 | 89.0% | — | sweep4 |
| 2250 | OOM | 1,421.0 | — | 171,427,915 | 79.4% | — | sweep4 |
| 2500 | OOM | 1,447.9 | — | 173,566,192 | 73.0% | — | sweep4 |

## Summary Statistics

- **Total tiles processed:** ~5.8 billion across all runs
- **Total GPU-hours:** ~16.5h
- **Overflow count:** 0 (all runs)
- **Transition band:** R = 860M–899M (40M wide, ~4.5% of R)
- **First MOAT:** R = 860M
- **Last SPANNING in transition:** R = 899M
- **Clean MOAT wall:** R >= 900M (except R = 950M anomaly)
- **SPANNING islands in transition (R=876–899M):** 876, 884, 885, 888, 893, 895, 896, 899
- **Isolated anomaly:** R = 950M (0.07% towers, reproduced twice)
- **OOM ceiling:** R >= 2000M (parent_ array exceeds 2B)

## Data Files

| File | Description |
|------|-------------|
| `sweep4-results.jsonl` | Structured JSON, 47 runs (sweep4) |
| `sweep-intermediate.jsonl` | Structured JSON, 24 runs (sweep1 + sweep2) |
| `sweep4-summary.txt` | Human-readable sweep4 log |
| `sweep4-console.log` | Raw console output |
| `logs/sweep-summary.txt` | Sweep1+2 summary |
| `logs/sweep-full.log` | Sweep1 full console (114K) |
| `logs/campaign-R*.log` | Per-R campaign logs (24 files) |
| `logs/campaign-1b.log` | Initial R=1B exploratory run |

## Known Correctness Caveats

These results were produced by code with three known correctness gaps (per `docs/supportive/2026-04-14-mathematical-audit.md`):

1. **Overflow handling:** campaign replaces overflow tiles with empty tiles. **Not triggered** — zero overflows across all K_SQ=40 runs.
2. **Face-prime truncation:** K5 silently clamps at 256/face without poison flag. **Not triggered** at operating radii.
3. **Diagonal/octant proof gap:** Code skips sub-diagonal tiles; spec says they should be processed. **Affects all runs** — the graph composed near y=x is smaller than the proof assumes. Impact unknown.

All verdicts are contingent on resolving caveat 3. The R=950M anomaly warrants independent investigation.
