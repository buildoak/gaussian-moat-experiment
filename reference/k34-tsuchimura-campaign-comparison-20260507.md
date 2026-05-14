# K34 Tsuchimura Campaign Comparison

Date: 2026-05-07

This note compares Tsuchimura's published `sqrt(34)` upper-bound campaign with
the local CUDA static-annulus campaign near the same radius.

The comparison is computational only. Tsuchimura's result is an
origin-component upper bound. The local CUDA rows are static-annulus detector
rows:

- `SPANNING`: some component connects the inner and outer boundary of the
  tested annulus.
- `MOAT`: no component connects the inner and outer boundary of the tested
  annulus.

These are different mathematical claim modes. The throughput comparison below
uses swept-prime / effective-area scale, not proof equivalence.

## Paper Facts

Primary source:

```text
Nobuyuki Tsuchimura, "Computational Results for Gaussian Moat Problem",
METR 2004-13, University of Tokyo, March 2004.
https://www.keisu.t.u-tokyo.ac.jp/data/2004/METR04-13.pdf
```

Reported K34 row:

```text
step size: sqrt(34)
farthest distance: < 24289452
origin component: finite
time: 130 hour on 38 CPUs
```

Reported hardware:

```text
19 machines
Intel Pentium III 1.4GHz x 2 per machine
38 CPUs total
1 GB RAM per machine
Gigabit Ethernet
Red Hat Linux 7.1
Java J2SE 1.4.2 / HotSpot Server VM
gcc 2.96
```

The paper reports for K32:

```text
generated Gaussian primes: 138,994,584,350
time: 80 hour on 38 CPUs
```

This implies cluster throughput:

```text
138,994,584,350 / (80 * 3600) = 482,620 Gaussian primes/sec
```

This is the source of the remembered "~400k primes/sec" scale. The paper also
lists `25,000-100,000/sec` in its method comparison and says parallel generation
was about `20x` faster, but it does not give a K34 generated-prime count.

## Effective Width Estimate

Tsuchimura's live origin-component band has literal radial thickness:

```text
k = sqrt(34) = 5.830951...
```

That is not the same object as a static annulus with `W=200000`. However, if we
ask how much first-octant radial width near `R=24289452` would contain the same
number of generated Gaussian primes, use the heuristic:

```text
N ~= R * W / (2 log R)
W ~= 2 * N * log R / R
```

At:

```text
R = 24289452
log R = 17.00555274
```

If K34 used the K32-implied cluster throughput:

```text
rate = 482,620 primes/sec
time = 130 * 3600 sec
N ~= 225,866,160,000 primes
W_eff ~= 316,267
```

Sensitivity:

| Assumed Tsuchimura rate | K34 generated primes over 130h | Effective width near `R0` |
|---:|---:|---:|
| `250,000/sec` | `117.0B` | `163,828` |
| `400,000/sec` | `187.2B` | `262,125` |
| `482,620/sec` | `225.9B` | `316,267` |

The old back-of-envelope `W_eff ~= 160k` corresponds to an assumed rate near
`250k primes/sec`. The K32-implied throughput supports a more central estimate
near `316k`.

## Local CUDA Measurements

Local K34 anchor:

```text
R0 = 24289452
remote instance: Vast 1x RTX 4090
price: $0.2889/hour
repo commit: fc52edce6f48d312220f1f4082fc17dee332be56
```

Key audited rows:

| Row | Time | Cost at `$0.2889/hour` |
|---|---:|---:|
| `W=200000` sampled audit | `622.1s = 0.1728h` | `$0.050` |
| `W=300000` sampled audit | `955.0s = 0.2653h` | `$0.077` |
| `W=400000` sampled audit | `1286.2s = 0.3573h` | `$0.103` |
| `W=1000000` exact-bound profile | `3257.5s = 0.9048h` | `$0.261` |

Interpolating to `W_eff ~= 316,000` between the measured W300k and W400k audit
rows:

```text
estimated CUDA time ~= 1008 sec
estimated CUDA time ~= 16.8 min
estimated CUDA cost ~= $0.081
```

## Speedup

Pure computational comparison, using `W_eff ~= 316k` as the local equivalent of
Tsuchimura's inferred K34 generated-prime budget:

```text
Tsuchimura K34 wall time: 130 hours
CUDA equivalent-probe time: ~0.280 hours
wall-clock speedup: 130 / 0.280 ~= 464x
```

If the conservative `400k primes/sec` memory is used instead:

```text
W_eff ~= 262k
estimated CUDA time ~= 14.5 min
wall-clock speedup ~= 540x
```

Robust range:

```text
pure wall-clock speedup: ~460x-540x
```

CPU-hour normalization:

```text
Tsuchimura CPU-hours = 130 * 38 = 4,940 CPU-hours
CUDA equivalent-probe time ~= 0.280 GPU-hours
CPU-hour / GPU-hour ratio ~= 17,600x
```

This is not a literal hardware-efficiency benchmark because the algorithms and
claim modes differ. It is a useful swept-prime / effective-area comparison.

## Per-Dollar Scale

There was no commodity cloud CPU-hour market in 2004. EC2 launched in 2006 at
about `$0.10` per instance-hour for a virtual CPU roughly comparable in era to a
1.7GHz Xeon:

```text
AWS EC2 beta announcement, 2006:
https://aws.amazon.com/blogs/aws/amazon_ec2_beta/
```

Possible Tsuchimura cost baselines:

| Baseline | CPU-hour price | K34 cost for `4,940 CPUh` |
|---|---:|---:|
| Electricity-only owned cluster | `$0.01-$0.02` | `$49-$99` |
| EC2-2006 proxy | `$0.10` | `$494` |
| Amortized owned cluster guess | `$0.05-$0.20` | `$247-$988` |

Local CUDA equivalent-probe cost:

```text
~0.280h * $0.2889/hour ~= $0.081
```

Per-dollar ratio:

| Baseline | Approximate dollar ratio |
|---|---:|
| electricity-only | `600x-1,200x` |
| EC2-2006 proxy | `~6,100x` |
| amortized owned cluster | `3,000x-12,200x` |

Best compact statement:

```text
Tsuchimura K34 UB campaign: 130h on 38 Pentium III CPUs.
Equivalent local CUDA swept-prime scale: W_eff ~= 316k near R0.
Our 4090 time for that scale: ~17 minutes, about $0.08.
Wall-clock speedup: ~460x.
Per-dollar speedup: thousands-fold; ~6,000x using the closest public CPU-hour proxy.
```

## Caveats

- Tsuchimura's K34 generated-prime count is not stated. `W_eff` is inferred from
  K32 reported throughput.
- Tsuchimura computes origin-component reachability with a streaming live band.
  Local CUDA computes static-annulus connectivity.
- Static-annulus rows can show `SPANNING` at `R_outer=R0` even when an
  origin-component upper bound holds, because disconnected shell components may
  span.
- The speedup statement should be presented as computational throughput near
  the same radius, not as a theorem-proof speedup.
