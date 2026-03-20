# sqrt(40) Gaussian Moat Location Prediction — Multi-Model Prompt

Date: 2026-03-19
Purpose: Independent mathematical prediction for R_moat(40) search range
Usage: Send to strong reasoning models via OpenRouter for independent opinions

---

## PROMPT

You are a mathematician specializing in analytic number theory and computational number theory. I need an independent prediction for a specific open problem.

### Problem

The Gaussian Moat Problem: in the Gaussian integers Z[i], define a graph where Gaussian primes are vertices and two primes are connected if their Euclidean distance <= sqrt(k^2). For a given k^2, is the connected component containing the origin finite? If so, what is R_moat(k^2) -- the farthest distance from the origin reachable?

All cases k^2 <= 36 have been resolved computationally (Tsuchimura 2004). I need your prediction for k^2 = 40 (step distance sqrt(40) ~ 6.325).

### Known Data Points

| k^2 | Step size | Farthest point xi(k) | R_moat (distance) | Component size (first octant) |
|-----|-----------|---------------------|--------------------|-------------------------------|
| 1   | 1.00      | 2 + i               | 2.236              | 2                             |
| 2   | 1.41      | 11 + 4i             | 11.705             | 14                            |
| 4   | 2.00      | 42 + 17i            | 45.310             | 92                            |
| 8   | 2.83      | 84 + 41i            | 93.472             | 380                           |
| 10  | 3.16      | 976 + 311i          | 1024.352           | 31221                         |
| 16  | 4.00      | 3297 + 2780i        | 4312.610           | 347638                        |
| 18  | 4.24      | 8174 + 6981i        | 10749.355          | 2386129                       |
| 20  | 4.47      | 120510 + 57857i     | 133679.065         | 273791623                     |
| 26  | 5.10      | 943460 + 376039i    | 1015638.765        | 14542615005                   |
| 32  | 5.66      | 2106442 + 1879505i  | 2823054.542        | 103711268594                  |
| 34  | 5.83      | --                  | UB < 24,289,452    | Finite                        |
| 36  | 6.00      | --                  | UB < 80,015,782    | Finite                        |
| 40  | 6.32      | ???                 | ???                | ???                           |

### Key Parameters

- Coordination number p(40) = 18 (Tsuchimura's convention: edges (x,y) with x>=0, y>0, x^2+y^2 <= k^2, x === y mod 2)
- Full lattice neighbors (all non-zero vectors with norm^2 <= 40) = 128
- Gaussian prime density in first octant: dPi_8/dN ~ 1/(2 ln N)
- Site occupancy: rho(N) ~ 4/(pi ln N)
- Vardi-Tsuchimura scaling: log|xi(k)| ~ 1.160 * p(k), where p(k) is the edge count

### Tsuchimura's Scaling Estimate

Tsuchimura refined Vardi's percolation-based estimate to:
  log|xi(k)| ~ 1.160 * p(k)

For k^2=40, p(40) = 18, this gives:
  log|xi(40)| ~ 1.160 * 18 = 20.88
  |xi(40)| ~ e^20.88 ~ 1.17 billion

However, this is a log-linear fit to 10 data points (k^2=1 through 32). The fit quality degrades for larger k where only upper bounds exist.

### What I Need

1. Your independent prediction for R_moat(40) with confidence interval
2. Multiple approaches: empirical extrapolation from known data, theoretical bounds (percolation theory, prime distribution), and any other frameworks you consider relevant
3. Specifically assess: what is the mechanism of moat formation? Is it local prime depletion (an annular gap), or something else (e.g., connected component fragmentation while primes remain locally dense)?
4. How reliable is extrapolation from 5-7 exact data points (plus 2 upper bounds) across 6 orders of magnitude?
5. Critical: the growth rate in log(R_moat) per unit k^2 appears to DECELERATE:
   - k^2=20 to 26: rate = 0.338 per k^2 unit
   - k^2=26 to 32: rate = 0.170 per k^2 unit
   What does this imply for k^2=40?
6. Recommended computational search strategy: where should we start probing?

Be rigorous. Show your reasoning. Flag where you're extrapolating vs where you have solid mathematical ground.

---

## FULL PAPER: Tsuchimura 2004 (METR 2004-13)

**Computational Results for Gaussian Moat Problem**
Nobuyuki TSUCHIMURA
Department of Mathematical Informatics, Graduate School of Information Science and Technology, The University of Tokyo
tutimura@mist.i.u-tokyo.ac.jp
March, 2004
METR 2004-13

### Abstract

"Can one walk to infinity on Gaussian primes taking steps of bounded length?" We adopted computational techniques to probe into this open problem. We propose an efficient method to search for the farthest point reachable from the origin, which can be parallelized easily, and have confirmed the existence of a moat of width k = sqrt(36), whereas the best previous result was k = sqrt(26) due to Gethner et al. A refinement of Vardi's estimate for the farthest distance reachable from the origin is proposed. The proposed estimate incorporates discreteness into Vardi's that is based on percolation theory.

### 1 Introduction

The question addressed in this paper is whether one can walk to infinity on Gaussian primes taking steps of bounded length. More precisely, this problem may be formulated as follows. A Gaussian integer means a complex number a + bi with integers a and b. A Gaussian prime means a Gaussian integer that cannot be decomposed into a product of two Gaussian integers in a nontrivial way, i.e., with factors distinct from +/-1, +/-i, where i = sqrt(-1). Consider a graph G drawn on the complex plane, of which the vertex set is the set of all Gaussian primes augmented by the origin. Two vertices are connected by an edge if the distance between them is less than or equal to a specified parameter k, which we call the step size. The question is whether the graph with a specified step size k contains a path from the origin that extends to infinity.

Nonexistence of such an infinite path implies the existence of a moat of width k surrounding the component of the origin. According to the literature, this problem was posed by Basil Gordon in 1962 at International Congress of Mathematicians in Stockholm, and still remains open. It seems, however, that the opinions in the literature are inclined to the negative answer.

### 2 Generating Methods of Gaussian Primes

A Gaussian integer is a number of the form a + bi, where a and b are integers and i is the square root of -1. A Gaussian prime is a Gaussian integer a + bi which cannot be divided by any Gaussian integer, excepting for +/-1, +/-i, +/-(a + bi), and +/-(b - ai).

Key fact: Let p be an ordinary prime integer.
- If p === 3 (mod 4), then p + 0i is a Gaussian prime.
- If p === 1 (mod 4), there uniquely exist a and b such that a^2 + b^2 = p and 0 <= b <= a, for which a + bi is a Gaussian prime.

The number of Gaussian primes of absolute value <= x lying in the first octant can be estimated as:
  pi_1(x^2) + 1 + pi_3(x) ~ x^2 / (4 log x)

### 3 Computational Method

**3.1 Gethner's Method:** Breadth-first search on the graph G. Gaussian primes classified by step level from origin. Requires primality testing (Miller-Rabin).

**3.2 Tsuchimura's Method (10x faster):** Sequential subgraph construction. Generate Gaussian primes in order of their norm. Maintain connected components via union-find (arborescences with path compression). Only retain primes in band region B_n = {z : |x_n| - k <= |z| <= |x_n|}. Band size ~ k|xi(k)|/(2 log|xi(k)|).

**Upper bound method:** Choose a starting distance |y|. Fictitiously assume ALL primes with |z| <= |y| are connected to the origin. Run the sequential construction forward. If the component terminates, |y| + (distance advanced) is an upper bound on |xi(k)|.

### 4 Computational Results

Main discoveries:
- k=sqrt(32): farthest point = 2106442 + 1879505i, distance 2823054.542, component size 103711268594 (75% of all generated primes in first octant). 80 hours on 38 CPUs.
- k=sqrt(36): cannot walk to infinity. Upper bound 80015782. 26 hours on 38 CPUs.
- k=sqrt(34): upper bound 24289452. 130 hours on 38 CPUs.

Parallelization: sieve (prime generation) is the bottleneck. Connectivity testing is sequential but fast. Sieve parallelized across CPUs by norm ranges.

### 5 Estimating the Farthest Distance

Vardi's percolation estimate: k ~ sqrt(2 * pi * lambda_c * log|xi(k)|), where lambda_c ~ 0.35 (continuum percolation constant).

Tsuchimura's refinement: log|xi(k)| ~ 1.160 * p(k), where p(k) = number of valid edge vectors for step size k. This incorporates lattice discreteness into Vardi's continuous model. Least-squares fit using 10 data points (k^2=1 through 32).

p(k) values: p(sqrt(1))=1, p(sqrt(2))=2, p(sqrt(4))=3, p(sqrt(8))=4, p(sqrt(10))=6, p(sqrt(16))=7, p(sqrt(18))=8, p(sqrt(20))=9, p(sqrt(26))=12, p(sqrt(32))=14, p(sqrt(34))=15, p(sqrt(36))=16, p(sqrt(40))=18, p(sqrt(50))=21.

---

## CONTEXT FROM OUR PRIOR ANALYSIS

A 10-iteration Codex research swarm produced a Bayesian posterior with median ~227M. An Opus 4.6 audit challenged this, arguing the true uncertainty spans [10M, 3B] (2-3 orders of magnitude). The key disputed claim is whether moats form via "origin-lineage extinction" (a branching process dying) at smaller distances than local density failure.

We want YOUR independent assessment. Do not anchor on 227M -- derive your own prediction from first principles and the data.
