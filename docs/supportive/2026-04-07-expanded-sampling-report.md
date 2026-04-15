---
title: Expanded Tile Sampling Report — K=40 vs K=36 (96 tiles, 4 radius bands)
date: 2026-04-07
type: evidence
status: complete
supports: tile_spec.md
description: Validates that u4 group label (max 16) is safe at both K=40 and K=36 across 48 tile locations spanning small, medium, transition, and target radius bands; all P2/P4/P5 properties pass.
---

# Expanded Tile Sampling Report: K=40 vs K=36

Generated: 2026-04-07 22:01:58
Runtime: 3.1s | Backend: gmpy2
Python: 3.12.12 | Platform: Darwin arm64

## 1. Sample Grid

**Total tiles tested:** 48 locations x 2 K values = 96 tile computations

| # | Label | tx | ty | |z| (actual) | Angle |
|---|-------|----|----|-------------|-------|
| 1 | small-r120-a5 | 0 | 0 | 0 | 5deg |
| 2 | small-r120-a15 | 0 | 0 | 0 | 15deg |
| 3 | small-r120-a22 | 0 | 0 | 0 | 22deg |
| 4 | small-r120-a30 | 0 | 0 | 0 | 30deg |
| 5 | small-r120-a38 | 0 | 0 | 0 | 38deg |
| 6 | small-r120-a44 | 0 | 0 | 0 | 44deg |
| 7 | small-r180-a5 | 0 | 0 | 0 | 5deg |
| 8 | small-r180-a15 | 0 | 0 | 0 | 15deg |
| 9 | small-r180-a22 | 0 | 0 | 0 | 22deg |
| 10 | small-r180-a30 | 0 | 0 | 0 | 30deg |
| 11 | small-r180-a38 | 0 | 0 | 0 | 38deg |
| 12 | small-r180-a44 | 0 | 0 | 0 | 44deg |
| 13 | medium-r15000-a5 | 14,848 | 1,280 | 14,903 | 5deg |
| 14 | medium-r15000-a15 | 14,336 | 3,840 | 14,841 | 15deg |
| 15 | medium-r15000-a22 | 13,824 | 5,376 | 14,833 | 22deg |
| 16 | medium-r15000-a30 | 12,800 | 7,424 | 14,797 | 30deg |
| 17 | medium-r15000-a38 | 11,776 | 9,216 | 14,954 | 38deg |
| 18 | medium-r15000-a44 | 10,752 | 10,240 | 14,848 | 44deg |
| 19 | medium-r35000-a5 | 34,816 | 2,816 | 34,930 | 5deg |
| 20 | medium-r35000-a15 | 33,792 | 8,960 | 34,960 | 15deg |
| 21 | medium-r35000-a22 | 32,256 | 13,056 | 34,798 | 22deg |
| 22 | medium-r35000-a30 | 30,208 | 17,408 | 34,865 | 30deg |
| 23 | medium-r35000-a38 | 27,392 | 21,504 | 34,824 | 38deg |
| 24 | medium-r35000-a44 | 25,088 | 24,064 | 34,763 | 44deg |
| 25 | transition-r520000000-a5 | 518,021,120 | 45,320,960 | 519,999,875 | 5deg |
| 26 | transition-r520000000-a15 | 502,281,216 | 134,585,856 | 519,999,781 | 15deg |
| 27 | transition-r520000000-a22 | 482,135,552 | 194,795,264 | 519,999,890 | 22deg |
| 28 | transition-r520000000-a30 | 450,333,184 | 260,000,000 | 519,999,978 | 30deg |
| 29 | transition-r520000000-a38 | 409,765,376 | 320,143,872 | 519,999,771 | 38deg |
| 30 | transition-r520000000-a44 | 374,056,448 | 361,222,144 | 519,999,677 | 44deg |
| 31 | transition-r580000000-a5 | 577,792,768 | 50,550,272 | 579,999,839 | 5deg |
| 32 | transition-r580000000-a15 | 560,236,800 | 150,114,816 | 579,999,767 | 15deg |
| 33 | transition-r580000000-a22 | 537,766,400 | 217,271,808 | 579,999,775 | 22deg |
| 34 | transition-r580000000-a30 | 502,294,528 | 289,999,872 | 579,999,757 | 30deg |
| 35 | transition-r580000000-a38 | 457,046,016 | 357,083,648 | 579,999,821 | 38deg |
| 36 | transition-r580000000-a44 | 417,217,024 | 402,901,760 | 579,999,891 | 44deg |
| 37 | target-r820000000-a5 | 816,879,616 | 71,467,520 | 819,999,947 | 5deg |
| 38 | target-r820000000-a15 | 792,059,136 | 212,231,424 | 819,999,910 | 15deg |
| 39 | target-r820000000-a22 | 760,290,560 | 307,177,216 | 819,999,742 | 22deg |
| 40 | target-r820000000-a30 | 710,140,672 | 409,999,872 | 819,999,798 | 30deg |
| 41 | target-r820000000-a38 | 646,168,576 | 504,842,240 | 819,999,705 | 38deg |
| 42 | target-r820000000-a44 | 589,858,560 | 569,619,712 | 819,999,840 | 44deg |
| 43 | target-r870000000-a5 | 866,689,280 | 75,825,408 | 869,999,885 | 5deg |
| 44 | target-r870000000-a15 | 840,355,328 | 225,172,480 | 869,999,841 | 15deg |
| 45 | target-r870000000-a22 | 806,649,856 | 325,907,712 | 869,999,901 | 22deg |
| 46 | target-r870000000-a30 | 753,442,048 | 434,999,808 | 869,999,858 | 30deg |
| 47 | target-r870000000-a38 | 685,569,280 | 535,625,472 | 869,999,933 | 38deg |
| 48 | target-r870000000-a44 | 625,825,536 | 604,352,768 | 869,999,925 | 44deg |

## 2a. Per-Tile Results (K=40)

### Per-Tile Detail

| Tile | (tx, ty) | Primes | Ports | Raw Groups | Pruned Groups | Pruned Ports | Max/Face | P2 | P4 | P5 |
|------|----------|--------|-------|------------|---------------|--------------|----------|----|----|-----|
| small-r120-a5 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r120-a15 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r120-a22 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r120-a30 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r120-a38 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r120-a44 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a5 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a15 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a22 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a30 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a38 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a44 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| medium-r15000-a5 | (14,848, 1,280) | 4897 | 6 | 2 | 1 | 5 | 2 | ok | ok | ok |
| medium-r15000-a15 | (14,336, 3,840) | 4881 | 7 | 1 | 1 | 7 | 4 | ok | ok | ok |
| medium-r15000-a22 | (13,824, 5,376) | 4881 | 8 | 1 | 1 | 8 | 3 | ok | ok | ok |
| medium-r15000-a30 | (12,800, 7,424) | 4835 | 8 | 2 | 1 | 7 | 3 | ok | ok | ok |
| medium-r15000-a38 | (11,776, 9,216) | 4903 | 12 | 2 | 1 | 11 | 4 | ok | ok | ok |
| medium-r15000-a44 | (10,752, 10,240) | 4938 | 8 | 1 | 1 | 8 | 3 | ok | ok | ok |
| medium-r35000-a5 | (34,816, 2,816) | 4471 | 13 | 4 | 1 | 10 | 4 | ok | ok | ok |
| medium-r35000-a15 | (33,792, 8,960) | 4430 | 10 | 2 | 1 | 9 | 4 | ok | ok | ok |
| medium-r35000-a22 | (32,256, 13,056) | 4442 | 17 | 4 | 1 | 14 | 5 | ok | ok | ok |
| medium-r35000-a30 | (30,208, 17,408) | 4447 | 9 | 1 | 1 | 9 | 4 | ok | ok | ok |
| medium-r35000-a38 | (27,392, 21,504) | 4496 | 12 | 3 | 1 | 10 | 3 | ok | ok | ok |
| medium-r35000-a44 | (25,088, 24,064) | 4478 | 15 | 4 | 1 | 12 | 3 | ok | ok | ok |
| transition-r520000000-a5 | (518,021,120, 45,320,960) | 2344 | 66 | 39 | 7 | 34 | 10 | ok | ok | ok |
| transition-r520000000-a15 | (502,281,216, 134,585,856) | 2301 | 51 | 26 | 5 | 30 | 9 | ok | ok | ok |
| transition-r520000000-a22 | (482,135,552, 194,795,264) | 2415 | 59 | 27 | 3 | 35 | 12 | ok | ok | ok |
| transition-r520000000-a30 | (450,333,184, 260,000,000) | 2373 | 68 | 40 | 9 | 37 | 10 | ok | ok | ok |
| transition-r520000000-a38 | (409,765,376, 320,143,872) | 2331 | 70 | 36 | 2 | 36 | 11 | ok | ok | ok |
| transition-r520000000-a44 | (374,056,448, 361,222,144) | 2341 | 59 | 38 | 8 | 29 | 11 | ok | ok | ok |
| transition-r580000000-a5 | (577,792,768, 50,550,272) | 2302 | 66 | 43 | 8 | 31 | 10 | ok | ok | ok |
| transition-r580000000-a15 | (560,236,800, 150,114,816) | 2389 | 68 | 36 | 7 | 39 | 12 | ok | ok | ok |
| transition-r580000000-a22 | (537,766,400, 217,271,808) | 2294 | 67 | 37 | 5 | 35 | 12 | ok | ok | ok |
| transition-r580000000-a30 | (502,294,528, 289,999,872) | 2332 | 61 | 39 | 10 | 32 | 11 | ok | ok | ok |
| transition-r580000000-a38 | (457,046,016, 357,083,648) | 2375 | 63 | 33 | 5 | 35 | 12 | ok | ok | ok |
| transition-r580000000-a44 | (417,217,024, 402,901,760) | 2306 | 57 | 26 | 7 | 38 | 11 | ok | ok | ok |
| target-r820000000-a5 | (816,879,616, 71,467,520) | 2285 | 66 | 45 | 9 | 30 | 9 | ok | ok | ok |
| target-r820000000-a15 | (792,059,136, 212,231,424) | 2275 | 74 | 43 | 7 | 38 | 12 | ok | ok | ok |
| target-r820000000-a22 | (760,290,560, 307,177,216) | 2315 | 64 | 37 | 6 | 33 | 12 | ok | ok | ok |
| target-r820000000-a30 | (710,140,672, 409,999,872) | 2268 | 64 | 44 | 9 | 29 | 10 | ok | ok | ok |
| target-r820000000-a38 | (646,168,576, 504,842,240) | 2283 | 60 | 37 | 11 | 34 | 10 | ok | ok | ok |
| target-r820000000-a44 | (589,858,560, 569,619,712) | 2278 | 56 | 35 | 5 | 26 | 8 | ok | ok | ok |
| target-r870000000-a5 | (866,689,280, 75,825,408) | 2273 | 63 | 42 | 10 | 31 | 9 | ok | ok | ok |
| target-r870000000-a15 | (840,355,328, 225,172,480) | 2312 | 69 | 41 | 7 | 35 | 10 | ok | ok | ok |
| target-r870000000-a22 | (806,649,856, 325,907,712) | 2283 | 66 | 32 | 6 | 40 | 13 | ok | ok | ok |
| target-r870000000-a30 | (753,442,048, 434,999,808) | 2281 | 70 | 42 | 6 | 34 | 11 | ok | ok | ok |
| target-r870000000-a38 | (685,569,280, 535,625,472) | 2260 | 69 | 39 | 6 | 36 | 13 | ok | ok | ok |
| target-r870000000-a44 | (625,825,536, 604,352,768) | 2279 | 52 | 27 | 4 | 29 | 10 | ok | ok | ok |

### Per-Band Summary

| K | Radius Band | Tiles | Avg Groups (raw) | Avg Groups (pruned) | Max Groups (pruned) | Avg Ports (pruned) | Max Ports/Face (pruned) | u4 holds? |
|---|-------------|-------|------------------|---------------------|---------------------|--------------------|-------------------------|-----------|
| 40 | small | 12 | 1.0 | 1.0 | 1 | 6.0 | 2 | YES |
| 40 | medium | 12 | 2.2 | 1.0 | 1 | 9.2 | 5 | YES |
| 40 | transition | 12 | 35.0 | 6.3 | 10 | 34.2 | 12 | YES |
| 40 | target | 12 | 38.7 | 7.2 | 11 | 32.9 | 13 | YES |

## 2b. Per-Tile Results (K=36)

### Per-Tile Detail

| Tile | (tx, ty) | Primes | Ports | Raw Groups | Pruned Groups | Pruned Ports | Max/Face | P2 | P4 | P5 |
|------|----------|--------|-------|------------|---------------|--------------|----------|----|----|-----|
| small-r120-a5 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r120-a15 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r120-a22 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r120-a30 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r120-a38 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r120-a44 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a5 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a15 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a22 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a30 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a38 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| small-r180-a44 | (0, 0) | 9174 | 6 | 1 | 1 | 6 | 2 | ok | ok | ok |
| medium-r15000-a5 | (14,848, 1,280) | 4897 | 14 | 4 | 1 | 11 | 4 | ok | ok | ok |
| medium-r15000-a15 | (14,336, 3,840) | 4881 | 12 | 1 | 1 | 12 | 5 | ok | ok | ok |
| medium-r15000-a22 | (13,824, 5,376) | 4881 | 14 | 2 | 1 | 13 | 4 | ok | ok | ok |
| medium-r15000-a30 | (12,800, 7,424) | 4835 | 15 | 3 | 1 | 13 | 5 | ok | ok | ok |
| medium-r15000-a38 | (11,776, 9,216) | 4903 | 15 | 2 | 1 | 14 | 5 | ok | ok | ok |
| medium-r15000-a44 | (10,752, 10,240) | 4938 | 10 | 1 | 1 | 10 | 4 | ok | ok | ok |
| medium-r35000-a5 | (34,816, 2,816) | 4471 | 19 | 5 | 1 | 15 | 6 | ok | ok | ok |
| medium-r35000-a15 | (33,792, 8,960) | 4430 | 15 | 4 | 1 | 12 | 4 | ok | ok | ok |
| medium-r35000-a22 | (32,256, 13,056) | 4442 | 25 | 8 | 1 | 18 | 6 | ok | ok | ok |
| medium-r35000-a30 | (30,208, 17,408) | 4447 | 18 | 3 | 1 | 16 | 5 | ok | ok | ok |
| medium-r35000-a38 | (27,392, 21,504) | 4496 | 17 | 4 | 1 | 14 | 4 | ok | ok | ok |
| medium-r35000-a44 | (25,088, 24,064) | 4478 | 22 | 5 | 1 | 18 | 5 | ok | ok | ok |
| transition-r520000000-a5 | (518,021,120, 45,320,960) | 2344 | 78 | 58 | 11 | 31 | 11 | ok | ok | ok |
| transition-r520000000-a15 | (502,281,216, 134,585,856) | 2301 | 78 | 57 | 11 | 32 | 11 | ok | ok | ok |
| transition-r520000000-a22 | (482,135,552, 194,795,264) | 2415 | 78 | 54 | 9 | 33 | 10 | ok | ok | ok |
| transition-r520000000-a30 | (450,333,184, 260,000,000) | 2373 | 93 | 59 | 13 | 47 | 15 | ok | ok | ok |
| transition-r520000000-a38 | (409,765,376, 320,143,872) | 2331 | 88 | 65 | 13 | 36 | 12 | ok | ok | ok |
| transition-r520000000-a44 | (374,056,448, 361,222,144) | 2341 | 91 | 71 | 13 | 33 | 12 | ok | ok | ok |
| transition-r580000000-a5 | (577,792,768, 50,550,272) | 2302 | 91 | 70 | 10 | 31 | 9 | ok | ok | ok |
| transition-r580000000-a15 | (560,236,800, 150,114,816) | 2389 | 88 | 63 | 10 | 35 | 11 | ok | ok | ok |
| transition-r580000000-a22 | (537,766,400, 217,271,808) | 2294 | 89 | 67 | 13 | 35 | 11 | ok | ok | ok |
| transition-r580000000-a30 | (502,294,528, 289,999,872) | 2332 | 83 | 69 | 11 | 25 | 12 | ok | ok | ok |
| transition-r580000000-a38 | (457,046,016, 357,083,648) | 2375 | 82 | 59 | 10 | 33 | 11 | ok | ok | ok |
| transition-r580000000-a44 | (417,217,024, 402,901,760) | 2306 | 81 | 54 | 11 | 38 | 10 | ok | ok | ok |
| target-r820000000-a5 | (816,879,616, 71,467,520) | 2285 | 84 | 68 | 11 | 27 | 8 | ok | ok | ok |
| target-r820000000-a15 | (792,059,136, 212,231,424) | 2275 | 92 | 71 | 13 | 34 | 11 | ok | ok | ok |
| target-r820000000-a22 | (760,290,560, 307,177,216) | 2315 | 89 | 68 | 9 | 30 | 12 | ok | ok | ok |
| target-r820000000-a30 | (710,140,672, 409,999,872) | 2268 | 77 | 68 | 8 | 17 | 7 | ok | ok | ok |
| target-r820000000-a38 | (646,168,576, 504,842,240) | 2283 | 83 | 67 | 12 | 28 | 8 | ok | ok | ok |
| target-r820000000-a44 | (589,858,560, 569,619,712) | 2278 | 82 | 61 | 9 | 30 | 9 | ok | ok | ok |
| target-r870000000-a5 | (866,689,280, 75,825,408) | 2273 | 85 | 70 | 12 | 27 | 8 | ok | ok | ok |
| target-r870000000-a15 | (840,355,328, 225,172,480) | 2312 | 83 | 66 | 10 | 27 | 8 | ok | ok | ok |
| target-r870000000-a22 | (806,649,856, 325,907,712) | 2283 | 92 | 60 | 12 | 44 | 13 | ok | ok | ok |
| target-r870000000-a30 | (753,442,048, 434,999,808) | 2281 | 87 | 69 | 8 | 26 | 7 | ok | ok | ok |
| target-r870000000-a38 | (685,569,280, 535,625,472) | 2260 | 86 | 63 | 13 | 36 | 13 | ok | ok | ok |
| target-r870000000-a44 | (625,825,536, 604,352,768) | 2279 | 70 | 50 | 8 | 28 | 10 | ok | ok | ok |

### Per-Band Summary

| K | Radius Band | Tiles | Avg Groups (raw) | Avg Groups (pruned) | Max Groups (pruned) | Avg Ports (pruned) | Max Ports/Face (pruned) | u4 holds? |
|---|-------------|-------|------------------|---------------------|---------------------|--------------------|-------------------------|-----------|
| 36 | small | 12 | 1.0 | 1.0 | 1 | 6.0 | 2 | YES |
| 36 | medium | 12 | 3.5 | 1.0 | 1 | 13.8 | 6 | YES |
| 36 | transition | 12 | 62.2 | 11.2 | 13 | 34.1 | 15 | YES |
| 36 | target | 12 | 65.1 | 10.4 | 13 | 29.5 | 13 | YES |

## 3. K=40 vs K=36 Comparison (Side-by-Side)

| Radius Band | K | Tiles | Avg Raw Groups | Avg Pruned Groups | Max Pruned Groups | Avg Pruned Ports | Max Ports/Face |
|-------------|---|-------|----------------|-------------------|-------------------|------------------|----------------|
| small | 40 | 12 | 1.0 | 1.0 | 1 | 6.0 | 2 |
| medium | 40 | 12 | 2.2 | 1.0 | 1 | 9.2 | 5 |
| transition | 40 | 12 | 35.0 | 6.3 | 10 | 34.2 | 12 |
| target | 40 | 12 | 38.7 | 7.2 | 11 | 32.9 | 13 |
| small | 36 | 12 | 1.0 | 1.0 | 1 | 6.0 | 2 |
| medium | 36 | 12 | 3.5 | 1.0 | 1 | 13.8 | 6 |
| transition | 36 | 12 | 62.2 | 11.2 | 13 | 34.1 | 15 |
| target | 36 | 12 | 65.1 | 10.4 | 13 | 29.5 | 13 |

## 4. u4 Headroom Analysis

### K=40

- **Total tiles tested:** 48
- **Max surviving groups (pruned):** 11
- **95th percentile surviving groups:** 10
- **Tiles with pruned groups > 12 (approaching u4=16 limit):** 0
  - None
- **u4 (max 16) holds:** YES
- **Headroom to u4 limit:** 5 groups

### K=36

- **Total tiles tested:** 48
- **Max surviving groups (pruned):** 13
- **95th percentile surviving groups:** 13
- **Tiles with pruned groups > 12 (approaching u4=16 limit):** 6
  - transition-r520000000-a30 at (450,333,184, 260,000,000): 13 groups
  - transition-r520000000-a38 at (409,765,376, 320,143,872): 13 groups
  - transition-r520000000-a44 at (374,056,448, 361,222,144): 13 groups
  - transition-r580000000-a22 at (537,766,400, 217,271,808): 13 groups
  - target-r820000000-a15 at (792,059,136, 212,231,424): 13 groups
  - target-r870000000-a38 at (685,569,280, 535,625,472): 13 groups
- **u4 (max 16) holds:** YES
- **Headroom to u4 limit:** 3 groups

## 5. Validation Summary

- **Total tile validations:** 96 (48 at K=40, 48 at K=36)
- **All properties passed:** 96/96
- **P2 failures (group correctness):** 0
- **P4 failures (port completeness):** 0
- **P5 failures (fingerprint uniqueness):** 0

## 6. Conclusions

### u4 Safety

- **K=40:** max pruned groups = 11, u4 (16) SAFE with 5 headroom
- **K=36:** max pruned groups = 13, u4 (16) SAFE with 3 headroom

### Coverage

- 48 tile locations across 4 radius bands
- Angles: [5, 15, 22, 30, 38, 44] degrees (first octant, avoiding axis and diagonal)
- Both K=40 (step sqrt(40)) and K=36 (step 6) tested
- All P2/P4/P5 properties validated at every tile

**Conclusion:** u4 is safe at both K=40 and K=36 across comprehensive coverage. The 4-bit group label has sufficient headroom.
