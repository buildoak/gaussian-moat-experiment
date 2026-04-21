---
date: 2026-03-23
engine: coordinator
status: complete
type: mathematical-foundation
campaign: k40-ise-narrowing
target_k_squared: 40
---

# ISE Narrowing Sweep: Curvature Compensation and High-Precision Protocol for $k^2 = 40$

## Abstract

We develop the mathematical foundation for a high-precision Independent Strip Ensemble (ISE) sweep to resolve the $k^2 = 40$ Gaussian moat candidate region. Three contributions are presented. First, we derive a curvature-compensated stripe placement that corrects the radial displacement inherent in the current constant-$a_{\min}$ design, ensuring all tile inner faces sit at the same true Euclidean radius from the origin. Second, we design a dense probe protocol that fills the measured sigmoid $f(r)$ at 6 additional radii and defines the $f(r) = 0.5$ crossing point as the ISE moat estimate. Third, we establish the statistical confidence theory for the crossing-point estimate under the independence model.

---

## Part 1: Moat Curvature Compensation

### 1.1 The Radial Displacement Problem

The ISE orchestrator places all stripes at a common radial lower bound $a_{\min}$. Strip $j$ has lateral offset $b_j$, so its inner-face center sits at the lattice point $(a_{\min}, b_j)$. The Euclidean distance from the origin to this point is

$$
R_{\text{stripe}}(a_{\min}, b_j) = \sqrt{a_{\min}^2 + b_j^2}. \tag{1}
$$

This is not equal to $a_{\min}$ whenever $b_j \neq 0$. The radial displacement of stripe $j$ relative to the on-axis stripe is

$$
\Delta R_j = \sqrt{a_{\min}^2 + b_j^2} - a_{\min}. \tag{2}
$$

For $b_j \ll a_{\min}$, a first-order expansion gives

$$
\Delta R_j \approx \frac{b_j^2}{2\, a_{\min}}. \tag{3}
$$

**Numerical example.** In the current campaign configuration with stride $= 10{,}000$, $M = 64$ stripes, and collar $c = 7$, the maximum lateral offset is

$$
b_{\max} = c + (M - 1) \cdot \text{stride} = 7 + 63 \times 10{,}000 = 630{,}007. \tag{4}
$$

At $a_{\min} = 10^9$ (i.e., $R \approx 1\text{B}$):

$$
\Delta R_{\max} = \sqrt{(10^9)^2 + (630{,}007)^2} - 10^9 \approx \frac{(6.3 \times 10^5)^2}{2 \times 10^9} \approx 198.5 \text{ units}. \tag{5}
$$

So the outermost stripe probes a shell that is $\sim\!199$ lattice units farther from the origin than the on-axis stripe. For tile height $H = 2000$, this displacement is $\sim\!10\%$ of $H$. At smaller radii, the effect is larger: at $a_{\min} = 5 \times 10^8$, $\Delta R_{\max} \approx 397$ units ($\sim\!20\%$ of $H$).

This is not catastrophic, but it means that different stripes are not sampling the same radial shell. In the transition region where $f(r)$ changes rapidly with $r$, a $\sim\!200$-unit radial smear could shift observed $f(r)$ values by several percent. For precision work, this should be corrected.

### 1.2 Curvature-Compensated Stripe Placement

**Definition 1.1** (Compensated placement). Fix a target radius $R_{\text{target}}$. For stripe $j$ at lateral offset $b_j$, define the compensated radial lower bound

$$
a_{\min,j} = \sqrt{R_{\text{target}}^2 - b_j^2}, \tag{6}
$$

so that the inner face of every tile in the ensemble sits at Euclidean distance $R_{\text{target}}$ from the origin:

$$
\sqrt{a_{\min,j}^2 + b_j^2} = \sqrt{R_{\text{target}}^2 - b_j^2 + b_j^2} = R_{\text{target}}. \tag{7}
$$

The outer face of stripe $j$ is then at

$$
a_{\max,j} = a_{\min,j} + H, \tag{8}
$$

and its true radial distance from origin is

$$
R_{\text{outer},j} = \sqrt{(a_{\min,j} + H)^2 + b_j^2}. \tag{9}
$$

**Proposition 1.2** (Uniform inner radius). Under the compensated placement of Definition 1.1, all $M$ stripes have their inner faces at exactly distance $R_{\text{target}}$ from the origin.

*Proof.* Immediate from Eq. (7). $\square$

**Proposition 1.3** (Outer-face radial spread). The outer-face radii satisfy

$$
R_{\text{outer},j} = \sqrt{(a_{\min,j} + H)^2 + b_j^2} = \sqrt{R_{\text{target}}^2 + 2\,a_{\min,j}\,H + H^2}. \tag{10}
$$

Since $a_{\min,j}$ varies across stripes (it decreases as $b_j$ increases), so does $R_{\text{outer},j}$. The spread is

$$
\Delta R_{\text{outer}} = R_{\text{outer},0} - R_{\text{outer},M-1} \approx \frac{H \cdot (a_{\min,0} - a_{\min,M-1})}{R_{\text{target}} + H}. \tag{11}
$$

For $H = 2000$, $R_{\text{target}} = 10^9$, $b_{\max} = 630{,}007$:

$$
a_{\min,0} - a_{\min,M-1} = R_{\text{target}} - \sqrt{R_{\text{target}}^2 - b_{\max}^2} \approx \frac{b_{\max}^2}{2\,R_{\text{target}}} \approx 199 \text{ units}. \tag{12}
$$

So $\Delta R_{\text{outer}} \approx 2000 \times 199 / 10^9 \approx 0.0004$ units. The outer-face radial spread is negligible. $\square$

### 1.3 Tile Axis Alignment: Rotations Are Not Needed

A natural concern is whether the compensated placement requires rotating tiles so that their faces are tangent to circles centered at the origin. We show this is unnecessary.

**Proposition 1.4** (Negligible curvature within a single tile). For a tile of width $W$ and height $H$ centered at lattice point $(a, b)$ with $a \gg W, H$, the deviation of the circle $|z| = R$ from a straight vertical line across the tile width satisfies

$$
\delta = a - \sqrt{R^2 - (b + W/2)^2} + \sqrt{R^2 - b^2} - a \approx \frac{b \cdot W + W^2/4}{2a}. \tag{13}
$$

More precisely, the maximum arc deviation across the tile is the difference in $a$-coordinate between the circle $|z| = R$ at $b$-coordinates $b$ and $b + W$:

$$
\delta_{\text{arc}} = \sqrt{R^2 - b^2} - \sqrt{R^2 - (b+W)^2}. \tag{14}
$$

For $b = b_{\max} = 630{,}007$, $W = 2000$, $R = 10^9$:

$$
\delta_{\text{arc}} = \sqrt{(10^9)^2 - (630{,}007)^2} - \sqrt{(10^9)^2 - (632{,}007)^2}. \tag{15}
$$

Using the approximation $\sqrt{R^2 - x^2} \approx R - x^2/(2R)$ for $x \ll R$:

$$
\delta_{\text{arc}} \approx \frac{(632{,}007)^2 - (630{,}007)^2}{2 \times 10^9} = \frac{2{,}524{,}028{,}000}{2 \times 10^9} \approx 0.63 \text{ units}. \tag{16}
$$

Since $\delta_{\text{arc}} < 1$ lattice unit, the curvature of the annulus within a single tile is invisible to the lattice. Tile edges remain axis-aligned. No rotation is needed. $\square$

**Remark 1.5.** The curvature deviation grows linearly with $b$ (the lateral offset) and with $W$ (the tile width), and inversely with $R$ (the radius). For the most extreme case in our configuration ($b_{\max} \approx 6.3 \times 10^5$, $W = 2000$, $R = 10^9$), the deviation is sub-lattice. At smaller radii, e.g., $R = 5 \times 10^8$, the deviation doubles to $\sim\!1.3$ units, which is still negligible compared to the collar $c = 7$. For radii $R > 10^8$ with $b_{\max} < 10^6$, rotations are never needed.

### 1.4 Implementation

The compensated placement is a localized modification to the ISE orchestrator. Two functions change:

**`stripe_offsets()`**: Unchanged. The lateral offsets $b_j$ are computed exactly as before.

**`shell_bounds()`** (or a new function called per-stripe): Currently returns a single $(a_{\text{lo}}, a_{\text{hi}}, r_{\text{center}})$ triple applied to all stripes. Under compensation, each stripe $j$ gets its own radial bounds:

$$
a_{\text{lo},j} = \left\lfloor \sqrt{R_{\text{target}}^2 - b_j^2} \right\rfloor, \qquad a_{\text{hi},j} = a_{\text{lo},j} + H. \tag{17}
$$

The per-stripe $a_{\text{lo},j}$ is passed to the tile kernel instead of the global $a_{\text{lo}}$.

**Quantification of improvement.** At stride $= 10{,}000$, $M = 64$, $R = 10^9$:

| Metric | Uncompensated | Compensated |
|--------|---------------|-------------|
| Inner-face radial spread | $\Delta R = 199$ units | $\Delta R = 0$ |
| Outer-face radial spread | $\sim\!199$ units | $< 1$ unit |
| Tile axis alignment | Axis-aligned | Axis-aligned (no rotation) |
| Stripe independence | Preserved | Preserved |

The collared tiles under compensated placement remain disjoint in the $b$-direction (the lateral spacing is unchanged), so the independence model (Section 4.4 of the transfer operator paper) is unaffected.

### 1.5 Compensated Placement at Different Radii

For a multi-shell sweep (e.g., 20 shells across a range), each shell $n$ has its own target radius $R_n$. The compensation is applied per shell:

$$
a_{\text{lo},j}^{(n)} = \left\lfloor \sqrt{R_n^2 - b_j^2} \right\rfloor, \qquad a_{\text{hi},j}^{(n)} = a_{\text{lo},j}^{(n)} + H. \tag{18}
$$

This means that within a single shell, all stripes probe the same true radius, but across shells the radius advances by approximately $H$ (exactly $H$ for the on-axis stripe; $H$ minus the compensation adjustment for off-axis stripes, which is negligible as shown in Proposition 1.3).

---

## Part 2: High-Precision Sweep Protocol

### 2.1 Current Data

The ISE connectivity profile for $k^2 = 40$ with $2000^2$ tiles, 64 stripes at stride $10{,}000$, and 20 shells per probe:

| $R$ | $f(r)$ | Regime |
|-----|--------|--------|
| $100\text{M}$--$500\text{M}$ | $\approx 1.0$ | Fully connected |
| $600\text{M}$ | $0.90$ | Onset of decline |
| $700\text{M}$ | $0.71$ | Transition |
| $800\text{M}$ | $0.54$ | Near midpoint |
| $900\text{M}$ | $0.39$ | Below midpoint |
| $1\text{B}$ | $0.22$ | Steep decline |
| $1.2\text{B}$ | $0.08$ | Near extinction |
| $1.3\text{B}$ | $0.06$ | Near extinction |
| $1.5\text{B}$ | $0.016$ | Sparse survival |
| $2.0\text{B}$ | $0.004$ | Near-total extinction |
| $2.5\text{B}$ | $0.000$ | Total extinction |
| $3.5\text{B}$ | $0.000$ | Confirmed extinction |

### 2.2 The Sigmoid Model

The data describe a decreasing sigmoid. We model $f(r)$ as a logistic function:

$$
f(r) = \frac{1}{1 + \exp\bigl(\beta \cdot (r - R_{0.5})\bigr)}, \tag{19}
$$

where $R_{0.5}$ is the radius at which $f(r) = 0.5$ (the midpoint of the transition) and $\beta > 0$ controls the steepness.

From the existing data, the midpoint lies between $R = 700\text{M}$ ($f = 0.71$) and $R = 900\text{M}$ ($f = 0.39$). Linear interpolation on the logit scale:

$$
\text{logit}(f) = \ln\!\left(\frac{f}{1 - f}\right). \tag{20}
$$

| $R$ | $f(r)$ | $\text{logit}(f)$ |
|-----|--------|---------------------|
| $700\text{M}$ | $0.71$ | $0.896$ |
| $800\text{M}$ | $0.54$ | $0.161$ |
| $900\text{M}$ | $0.39$ | $-0.446$ |

The logit is approximately linear in $R$ across this range, confirming the logistic model. By linear interpolation:

$$
R_{0.5} \approx 800\text{M} + 100\text{M} \times \frac{0.161}{0.161 + 0.446} \approx 826\text{M}. \tag{21}
$$

The slope gives $\beta \approx (0.896 + 0.446) / (200 \times 10^6) \approx 6.7 \times 10^{-9}$ per unit radius.

### 2.3 The $f(r) = 0.5$ Crossing Point as the ISE Moat Estimate

**Definition 2.1.** The *ISE moat estimate* is the radius $R_{0.5}$ at which $f(r) = 0.5$.

**Justification.** The crossing fraction $f(r)$ measures the probability that a randomly placed independent strip carries inner-to-outer connectivity at radius $r$. When $f(r) = 0.5$, exactly half the independent probes are blocked. This is the natural threshold for the following reasons:

1. **Percolation transition.** In classical percolation theory, the critical threshold $p_c$ marks the point where large-scale connectivity ceases. For the strip-crossing observable, the analog is $f(r) = 0.5$: above this level, the majority of independent probes cross; below it, the majority are blocked. The transition from "mostly connected" to "mostly blocked" is centered here.

2. **Symmetry.** At $f(r) = 0.5$, the probability of observing a crossing equals the probability of observing blockage. This is the point of maximum uncertainty about the local connectivity state, and therefore the point at which the transition is most sharply located.

3. **Estimator precision.** As shown in Part 3, the variance of the binomial estimator $\hat{f}$ is $p(1-p)/N$, which is maximized at $p = 0.5$. The midpoint is the hardest point to estimate precisely, but it is also the most informative: observing $f(r) \approx 0.5$ at a specific radius pins the transition center with maximum diagnostic power.

4. **Robustness to finite-size effects.** Tile-size effects inflate $f(r)$ in blocked regions and deflate it in connected regions (as documented in the rectangular tile calibration analysis). These effects shift the entire sigmoid but approximately preserve its midpoint. The $f(r) = 0.5$ crossing is therefore more robust to tile-size artifacts than any threshold closer to $0$ or $1$.

### 2.4 Gap-Filling Probe Design

The current data has its largest gaps in the transition region, precisely where resolution matters most. We propose 6 additional probes:

| Probe | $R$ | Fills gap between | Expected $f(r)$ (logistic model) |
|-------|-----|-------------------|-----------------------------------|
| A | $750\text{M}$ | $700\text{M}$--$800\text{M}$ | $\sim 0.63$ |
| B | $850\text{M}$ | $800\text{M}$--$900\text{M}$ | $\sim 0.46$ |
| C | $950\text{M}$ | $900\text{M}$--$1\text{B}$ | $\sim 0.30$ |
| D | $1.05\text{B}$ | $1\text{B}$--$1.2\text{B}$ | $\sim 0.17$ |
| E | $1.1\text{B}$ | $1\text{B}$--$1.2\text{B}$ | $\sim 0.13$ |
| F | $1.15\text{B}$ | $1\text{B}$--$1.2\text{B}$ | $\sim 0.10$ |

With these additions, the transition region ($f(r) \in [0.1, 0.7]$) will be sampled at 50M intervals, giving a smooth sigmoid fit with 14 data points in the critical range.

### 2.5 Sweep Configuration

Each probe uses:

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Tile size | $2000 \times 2000$ | Validated geometry (calibration analysis) |
| Stripes | $M = 64$ | Doubles the Phase 1 count for better precision |
| Stride | $10{,}000$ | Wide separation for strong independence |
| Shells | $20$ | Sufficient for averaging within each probe |
| Curvature compensation | Enabled | Per Eq. (6), eliminates radial smear |
| $k^2$ | $40$ | Target |
| Collar | $7$ | $\lceil\sqrt{40}\rceil = 7$ (code convention) |

### 2.6 Runtime Estimate

From existing campaign data, each probe at these settings takes approximately 380 seconds on the Jetson Orin Nano (6 cores, rayon parallelism). The 6 new probes total:

$$
6 \times 380\text{s} = 2{,}280\text{s} \approx 38\text{ minutes}. \tag{22}
$$

The full set of 18 probes (existing 12 + new 6), if rerun with curvature compensation, takes:

$$
18 \times 380\text{s} = 6{,}840\text{s} \approx 114\text{ minutes} \approx 1.9\text{ hours}. \tag{23}
$$

This is a single-evening run on the Jetson.

---

## Part 3: Statistical Confidence

### 3.1 The Observation Model

Fix a shell at radius $r$. The ISE probes this shell with $M$ independent stripes across $S$ shells. Each stripe-shell pair produces a binary outcome:

$$
X_{j,s} = \begin{cases} 1 & \text{if stripe } j \text{ at shell } s \text{ has } \operatorname{io\_count} > 0, \\ 0 & \text{otherwise.} \end{cases} \tag{24}
$$

Under the independence model (justified by collar-disjoint stripe placement), the $X_{j,s}$ are independent Bernoulli trials with common success probability $p = p(r)$.

The observed crossing fraction is

$$
\hat{f}(r) = \frac{1}{N} \sum_{j=1}^{M} \sum_{s=1}^{S} X_{j,s}, \qquad N = M \cdot S. \tag{25}
$$

### 3.2 Unbiasedness and Variance

**Proposition 3.1.** $\hat{f}(r)$ is an unbiased estimator of $p(r)$ with variance

$$
\operatorname{Var}[\hat{f}(r)] = \frac{p(r)\,(1 - p(r))}{N}. \tag{26}
$$

*Proof.* Since $\hat{f}(r)$ is the sample mean of $N$ i.i.d. Bernoulli($p$) random variables,

$$
\mathbb{E}[\hat{f}(r)] = p, \qquad \operatorname{Var}[\hat{f}(r)] = \frac{p(1-p)}{N}. \tag{27}
$$

$\square$

### 3.3 Sample Size

With $M = 64$ stripes and $S = 20$ shells per probe, the total number of independent tile observations per probe is

$$
N = M \times S = 64 \times 20 = 1{,}280. \tag{28}
$$

### 3.4 Confidence Interval for $p$ Given $\hat{f}$

For large $N$, the Wald confidence interval at level $1 - \alpha$ is

$$
\hat{f} \pm z_{\alpha/2} \sqrt{\frac{\hat{f}(1 - \hat{f})}{N}}, \tag{29}
$$

where $z_{\alpha/2}$ is the standard normal quantile (for 95% confidence, $z_{0.025} = 1.96$).

**Proposition 3.2** (Width at the midpoint). At $\hat{f} = 0.5$ with $N = 1{,}280$:

$$
\text{CI width} = 2 \times 1.96 \times \sqrt{\frac{0.5 \times 0.5}{1280}} = 2 \times 1.96 \times 0.01398 = 0.0548. \tag{30}
$$

So the 95% confidence interval for $p$ when $\hat{f} = 0.5$ is $(0.473, 0.527)$. The estimate is precise to $\pm\, 2.7\%$ at the transition midpoint.

**Table of CI half-widths for $N = 1{,}280$:**

| $\hat{f}$ | $\sqrt{\hat{f}(1-\hat{f})/N}$ | 95% CI half-width |
|-----------|-------------------------------|---------------------|
| $0.90$ | $0.00839$ | $\pm\, 0.016$ |
| $0.70$ | $0.01281$ | $\pm\, 0.025$ |
| $0.50$ | $0.01398$ | $\pm\, 0.027$ |
| $0.30$ | $0.01281$ | $\pm\, 0.025$ |
| $0.10$ | $0.00839$ | $\pm\, 0.016$ |
| $0.05$ | $0.00609$ | $\pm\, 0.012$ |

### 3.5 Precision of the $f(r) = 0.5$ Crossing-Point Estimate

The crossing point $R_{0.5}$ is defined implicitly by $f(R_{0.5}) = 0.5$. Its precision depends on both the precision of individual $\hat{f}$ measurements and the slope of the sigmoid.

**Proposition 3.3** (Crossing-point uncertainty via the delta method). Let $f(r)$ be a smooth monotone decreasing function of $r$ with $f(R_{0.5}) = 0.5$, and let $\hat{R}_{0.5}$ be the estimate obtained by interpolating the measured $\hat{f}(r_i)$ values. The standard error of $\hat{R}_{0.5}$ is approximately

$$
\sigma_{R_{0.5}} \approx \frac{\sigma_{\hat{f}}}{|f'(R_{0.5})|}, \tag{31}
$$

where $\sigma_{\hat{f}} = \sqrt{0.25/N}$ is the standard error of $\hat{f}$ at $p = 0.5$, and $f'(R_{0.5})$ is the slope of the sigmoid at the midpoint.

*Proof.* By the implicit function theorem, if $f(R_{0.5}) = 0.5$ and $f$ is differentiable with $f' \neq 0$, then small perturbations $\delta f$ in the measured crossing fraction induce perturbations

$$
\delta R_{0.5} \approx -\frac{\delta f}{f'(R_{0.5})}
$$

in the estimated crossing point. Taking standard deviations gives Eq. (31). $\square$

**Numerical estimate.** From Section 2.2, $\beta \approx 6.7 \times 10^{-9}$ per unit radius. For the logistic model, $f'(R_{0.5}) = -\beta/4 = -1.68 \times 10^{-9}$. (The maximum slope of a logistic function $1/(1+e^{\beta(r-R_0)})$ is $\beta/4$, attained at $r = R_0$.) Therefore:

$$
\sigma_{R_{0.5}} \approx \frac{0.01398}{1.68 \times 10^{-9}} \approx 8.3 \times 10^6 \text{ units} = 8.3\text{M}. \tag{32}
$$

The 95% confidence interval for $R_{0.5}$ is

$$
R_{0.5} \pm 1.96 \times 8.3\text{M} \approx R_{0.5} \pm 16.3\text{M}. \tag{33}
$$

With the gap-filling probes at 50M intervals and the existing data, the crossing point will be localized to approximately $R_{0.5} = 826\text{M} \pm 16\text{M}$ (95% CI).

### 3.6 Effect of Increasing $M$

If more precision is needed, increasing the number of stripes $M$ reduces $\sigma_{\hat{f}} \propto 1/\sqrt{N}$ and therefore $\sigma_{R_{0.5}} \propto 1/\sqrt{N}$. The table below shows the tradeoffs:

| $M$ | $S$ | $N = M \cdot S$ | $\sigma_{\hat{f}}$ at $p = 0.5$ | 95% CI for $R_{0.5}$ |
|-----|-----|-----------------|----------------------------------|-----------------------|
| 32 | 20 | 640 | 0.0198 | $\pm\, 23\text{M}$ |
| 64 | 20 | 1,280 | 0.0140 | $\pm\, 16\text{M}$ |
| 128 | 20 | 2,560 | 0.0099 | $\pm\, 12\text{M}$ |
| 64 | 50 | 3,200 | 0.0088 | $\pm\, 10\text{M}$ |

Doubling $M$ from 64 to 128 halves the runtime per probe (twice as many tiles per shell, but $\sqrt{2}$ improvement in precision). Increasing $S$ from 20 to 50 provides $\sqrt{2.5}$ improvement at $2.5\times$ runtime. The current configuration ($M = 64$, $S = 20$) is a good balance: the 95% CI of $\pm\, 16\text{M}$ is comfortably smaller than the 50M probe spacing.

### 3.7 False-Extinction Bound

The probability that all $N$ tile observations in a probe show blockage, even though the true crossing probability is $p > 0$, is

$$
\Pr(\hat{f} = 0 \mid p) = (1 - p)^N. \tag{34}
$$

For $N = 1{,}280$:

| True $p$ | $(1-p)^{1280}$ | Interpretation |
|----------|-----------------|----------------|
| $0.01$ | $2.4 \times 10^{-6}$ | Very unlikely to see $\hat{f} = 0$ |
| $0.005$ | $1.6 \times 10^{-3}$ | Unlikely |
| $0.001$ | $0.278$ | Plausible |
| $0.0005$ | $0.527$ | Coin flip |

If the observed $\hat{f} = 0$, we can be confident that $p < 0.005$ (at the $0.2\%$ level). The $R = 2.5\text{B}$ extinction ($\hat{f} = 0$ with $N = 1{,}280$) therefore implies $p(2.5\text{B}) < 0.005$ with very high confidence.

---

## Part 4: Protocol

### 4.1 Probe Commands

All commands assume the ISE binary at `./tile-probe/target/release/ise` and execution on the Jetson Orin Nano. The `--curvature-compensated` flag activates per-stripe $a_{\min}$ adjustment (requires implementation per Section 1.4).

**Gap-filling probes (6 new radii):**

```bash
# Probe A: R = 750M
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 64 \
    --stripe-stride 10000 \
    --r-min 750000000 \
    --r-max 750040000 \
    --trace

# Probe B: R = 850M
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 64 \
    --stripe-stride 10000 \
    --r-min 850000000 \
    --r-max 850040000 \
    --trace

# Probe C: R = 950M
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 64 \
    --stripe-stride 10000 \
    --r-min 950000000 \
    --r-max 950040000 \
    --trace

# Probe D: R = 1.05B
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 64 \
    --stripe-stride 10000 \
    --r-min 1050000000 \
    --r-max 1050040000 \
    --trace

# Probe E: R = 1.1B
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 64 \
    --stripe-stride 10000 \
    --r-min 1100000000 \
    --r-max 1100040000 \
    --trace

# Probe F: R = 1.15B
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 64 \
    --stripe-stride 10000 \
    --r-min 1150000000 \
    --r-max 1150040000 \
    --trace
```

Each probe spans $r_{\max} - r_{\min} = 40{,}000$, yielding $40{,}000 / 2{,}000 = 20$ shells.

### 4.2 Full Rerun with Compensation (Optional)

If the curvature-compensated flag is implemented, rerun all 18 radii with compensation to produce a clean, consistent dataset:

```bash
for R in 100000000 200000000 300000000 400000000 500000000 \
         600000000 700000000 750000000 800000000 850000000 \
         900000000 950000000 1000000000 1050000000 1100000000 \
         1150000000 1200000000 1300000000; do
    ./tile-probe/target/release/ise \
        --k-squared 40 \
        --tile-size 2000 \
        --stripes 64 \
        --stripe-stride 10000 \
        --r-min $R \
        --r-max $((R + 40000)) \
        --trace \
        > research/results/k40-narrowing/R${R}.json 2>&1
done
```

Total runtime: $\sim\!2$ hours.

### 4.3 Post-Processing

After all probes complete:

1. **Extract $f(r)$ vs $R$ table.** For each probe, compute the mean $f(r)$ across its 20 shells as the per-probe estimate, and the standard deviation across shells as a consistency check.

2. **Fit the logistic model.** Minimize weighted least squares on

$$
\text{logit}(\hat{f}_i) = \beta \cdot (R_i - R_{0.5})
$$

across all probes with $0.05 < \hat{f}_i < 0.95$ (avoid boundary compression).

3. **Extract $R_{0.5}$ and 95% CI.** From the fitted model, report $R_{0.5}$ with standard error from Eq. (31).

4. **Verify sigmoid monotonicity.** Check that $f(r)$ is non-increasing across the full range. Any non-monotonicity exceeding the CI width flags a systematic issue (tile-size artifact, primality error, or curvature contamination).

### 4.4 Success Criteria

| Criterion | Threshold | Status |
|-----------|-----------|--------|
| All 18 probes complete | No OOM, no panic | Required |
| $f(r)$ monotone decreasing (within CI) | $f(r_i) - f(r_{i+1}) > -0.05$ for all $i$ | Required |
| Logistic fit $R^2$ | $> 0.95$ | Expected |
| $R_{0.5}$ 95% CI width | $< 50\text{M}$ | Expected |
| Extinction confirmed at $R = 2.5\text{B}$ | $\hat{f} = 0$ | Already confirmed |
| Curvature compensation effect | $|\Delta f| < 0.03$ at all probes | Expected |

### 4.5 Expected Deliverables

1. **ISE moat estimate for $k^2 = 40$**: $R_{0.5} \approx 826\text{M} \pm 16\text{M}$ (95% CI), to be refined by the actual data.
2. **Full sigmoid profile**: 18-point $f(r)$ vs $R$ table from $100\text{M}$ to $1.3\text{B}$.
3. **Validation of curvature compensation**: quantified difference between compensated and uncompensated $f(r)$ values.
4. **Statistical confidence**: per-probe confidence intervals and crossing-point uncertainty.
