# Port Overflow Census & Group Structure Analysis

Date: 2026-04-09
Commits: fc81874, 9a4d5ce, 7ae2ecf, 581b6a6

## Summary

A 100K-tile census at R=860M revealed a 33.16% overflow rate at PORTS_PER_FACE=16. Deep structural analysis of 1000 tiles (24K groups) shows the overflow is structural — driven by large multi-face connected components fragmenting into many small port windows per face, not by insufficient pruning. Observed max face ports: 22. Recommendation: increase PORTS_PER_FACE to 24.

## 1. Census Setup

- C++ tile pipeline with Montgomery-optimized sieve (~8ms/tile single-threaded, ~1ms amortized with 12-core GCD multithreading)
- Sampled 100,000 tiles in a narrow stripe at R=860,000,000, starting from Y axis going right (3,125 towers × 32 tiles/tower)
- Census tool: `tests/census_tiles.cpp`, output: `census_output/census_R860000000_T3125.csv`
- Completed in 100.7 seconds at 993 tiles/sec (multithreaded)

## 2. Census Results

Key stats from 100K tiles:

- Prime count: mean=2044.1, range=[1887, 2214]
- Group count: mean=9.8, median=10, max=21
- Ports before pruning: mean=65.1, max=86
- Ports after pruning: mean=51.1, max=73
- Per-face ports (non-overflow): mean=12.4, p99=16, max=16
- **Overflow tiles: 33,160 / 100,000 (33.16%)**
- Overflow is entirely port-count driven (groups max=21, well under 255)
- Pruning removes only 21.5% of ports (spec predicted 70-80%)

## 3. Hypothesis Testing

Three hypotheses investigated:

**H1: Pruning correct but weak at this density** — Most groups span 2+ faces, so few qualify as dead ends.

**H2: Pruning has a bug** — C++ doesn't correctly identify all dead-end groups.

**H3: Port clustering too fragmented** — Consecutive-pair rule creates too many small ports.

### Method

- Selected 4 diagnostic tiles (heavy overflow, marginal overflow, edge case, comfortable)
- Ran full Python structural analysis: primes, components, face primes, port clustering, group-face incidence, dead-end identification
- Cross-verified all 4 tiles against C++ `run_tile` — exact match on every metric

### Result: H1 confirmed, H2 eliminated

C++ and Python agree perfectly. Pruning is correct. The issue is structural: at R=860M density, most groups span multiple faces and carry many ports. Dead-end pruning can only remove groups on 1 face with 1 port.

Important: single-face multi-port groups ("bridge groups") CANNOT be pruned — they relay connectivity between separate groups in adjacent tiles. This was verified by reasoning about the compositor's group-level union-find.

## 4. Uncapped Port Distribution

500 overflow tiles analyzed via Python without the 16-port cap:

- Max face ports: min=12, mean=17.29, median=17, p95=20, p99=21, **max=22**
- Distribution peaks sharply at 17-18 (61% of overflow tiles)
- Tail drops off steeply: only 5 tiles out of 500 exceeded 20

Cumulative coverage:

- PORTS_PER_FACE=16: 66.8% of all tiles (current)
- PORTS_PER_FACE=20: captures 99.0% of overflow tiles
- PORTS_PER_FACE=22: captures 100% of observed sample
- PORTS_PER_FACE=24: safe with headroom

## 5. Deep Group Structure (1000 tiles, 24K groups)

### Group taxonomy

| Category | % of groups | Avg primes | Avg ports | Avg max-face-ports |
|---|---|---|---|---|
| Dead end (1 face, 1 port) | 58.2% | 6.8 | 1.0 | 1.0 |
| Bridge (1 face, 2+ ports) | 22.5% | 56.3 | 2.8 | 2.8 |
| Multi-face (2+ faces) | 19.3% | 297.6 | 8.0 | 4.4 |

### Port budget after pruning (per face average: 13.0 ports)

- Multi-face groups: 9.2 ports/face (70.7%) — the dominant consumer
- Bridge groups: 3.8 ports/face (29.3%)

### Multi-face group structure

- 2-face groups: 84.4% (adjacent face pairs I+L, I+R, O+L, O+R dominate at ~21% each)
- 3-face groups: 8.5% (avg 18.6 ports, 966 primes)
- 4-face groups: 7.1% (avg 27.7 ports, 1403 primes)
- Opposite-face pairs (I+O, L+R) extremely rare (<0.2%)

### The mega-group

- Largest group per tile: mean=1024 primes (half the tile), 2.9 faces, 19.1 ports
- 32.6% of tiles have a group spanning all 4 faces
- 22.1% of tiles: one group accounts for >50% of surviving ports

### Bridge group profile

- Modest: median 2 ports, 57% have exactly 2 ports
- Thin tail: only 1 group in 1000 tiles reached 14 ports
- Evenly distributed across faces (~25% each)

### Overflow vs non-overflow

- Same structure: 4.6 multi-face groups/tile in both
- Same bridge count: ~5.3/tile
- Difference: per-face ports 13.5-13.7 (overflow) vs 12.3-12.5 (non-overflow)
- The margin is razor-thin: +1.2 ports/face average pushes a third of tiles past the cap

## 6. Implications for TileOp Design

The 128-byte TileOp with 16 ports/face is insufficient at R=860M. Options:

| PORTS_PER_FACE | Coverage | TileOp size | Memory at 73.4M tiles |
|---|---|---|---|
| 16 (current) | 66.8% | 128 bytes | 9.4 GB |
| 20 | ~99.7% | ~160 bytes | 11.7 GB |
| 24 | ~100% | 192 bytes | 14.1 GB |
| 32 | ~100% | 256 bytes | 18.8 GB |

The TileOp has 32 reserved bytes (96-127) designed for this scenario. Recommendation: PORTS_PER_FACE=24 at 192 bytes (3 cache lines), using the reserved space plus 32 additional bytes.

## 7. Measurement Tools

| Tool | Location | Purpose |
|---|---|---|
| census_tiles.cpp | tile-cpp/tests/ | 100K-tile C++ census with GCD multithreading |
| diagnose_tile.py | tile-validator/ | Single-tile deep diagnostic |
| measure_uncapped.py | tile-validator/ | Batch uncapped port measurement |
| group_stats.py | tile-validator/ | 1000-tile group structure analysis |

## 8. Raw Data

| File | Rows | Description |
|---|---|---|
| census_R860000000_T3125.csv | 100,001 | Full 100K census (header + data) |
| uncapped_R860000000.csv | 601 | 500 overflow + 100 calibration uncapped measurements |
| group_stats_R860000000.csv | ~24K | Per-group structural data from 1000 tiles |
