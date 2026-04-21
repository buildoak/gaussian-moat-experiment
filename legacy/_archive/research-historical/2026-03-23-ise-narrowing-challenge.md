# Adversarial Review: ISE Narrowing Sweep Plan

**Reviewer:** Mathematical Challenge Protocol
**Document under review:** `2026-03-23-ise-narrowing-plan.md`
**Cross-reference:** `2026-03-22-connectivity-transfer-operator.md` (the "CTO paper")
**Date:** 2026-03-23

---

## Issue 1: Collar Definition Contradicts the Theory Paper

- **Severity:** Critical
- **Location:** Section 1.4, Eq. (17); Section 2.5 table (Collar = 7)
- **Issue:**

The ISE narrowing plan states: *"Collar: 7, $\lceil\sqrt{40}\rceil = 7$ (code convention)".*

The CTO paper (Section 3.2) defines the collar as:
$$c = \lfloor\sqrt{k^2}\rfloor$$
and explicitly works the example: *"If $k^2 = 40$, then $c = 6$, because $\lfloor\sqrt{40}\rfloor = \lfloor 6.32\ldots\rfloor = 6$."*

The code (`tile.rs:125`, `orchestrator.rs:154`) uses `ceil`: `(k_sq as f64).sqrt().ceil() as i64`, giving $c = 7$ for $k^2 = 40$.

So there is a three-way disagreement:
1. The CTO paper says $c = \lfloor\sqrt{k^2}\rfloor = 6$ and proves correctness using floor.
2. The code uses $c = \lceil\sqrt{k^2}\rceil = 7$.
3. The ISE narrowing plan uses $c = 7$ and cites "code convention" rather than the CTO paper's theorem.

The CTO paper's proof is actually correct: the maximal single-coordinate excursion of a step vector $(\Delta a, \Delta b)$ with $\Delta a^2 + \Delta b^2 \le k^2$ is indeed $\lfloor\sqrt{k^2}\rfloor$. The code's `ceil` is strictly conservative (over-collaring by 1 unit for non-perfect-square $k^2$), which preserves correctness but wastes computation. The real problem is that the ISE narrowing plan does not acknowledge or explain this discrepancy. If someone reads the CTO paper and then this plan, they will be confused about which collar definition is canonical.

Moreover, the over-collaring has downstream consequences: the minimum stride is $W + 2c$, so with $c = 7$ the minimum stride for $W = 2000$ is $2014$ (vs. $2012$ for $c = 6$). The $b_{\max}$ calculation in Eq. (4) uses $c = 7$, giving $b_{\max} = 630{,}007$. With the correct floor collar $c = 6$, $b_{\max}$ would be $630{,}006$ for tight packing (or unchanged with stride $= 10{,}000$ since the stride dominates). The numerical difference is negligible, but the conceptual inconsistency is not.

- **Suggested fix:** Either (a) update the CTO paper to use $\lceil\sqrt{k^2}\rceil$ with a corrected proof, or (b) update the code and this plan to use $\lfloor\sqrt{k^2}\rfloor$, or (c) at minimum, state explicitly that the code over-collars by design and explain why $c = 7$ is a safe upper bound on the theoretical $c = 6$.

---

## Issue 2: The $b_{\max}$ Formula Uses an Unusual Stride Convention

- **Severity:** Minor
- **Location:** Section 1.1, Eq. (4)
- **Issue:**

The document states:
$$b_{\max} = c + (M - 1) \cdot \text{stride} = 7 + 63 \times 10{,}000 = 630{,}007.$$

But $b_{\max}$ as written is the $b_{\text{lo}}$ of the outermost stripe, not the maximum $b$-coordinate actually probed. The outermost stripe spans $[b_{\max}, b_{\max} + W]$, so the actual maximum lateral coordinate touched by the ensemble is $b_{\max} + W = 632{,}007$. The expanded region extends to $b_{\max} + W + c = 632{,}014$.

The Euclidean distance from the origin to the inner face of the outermost stripe is $\sqrt{a_{\min}^2 + b_j^2}$ where $b_j$ is the center or the edge of that stripe. The document uses $b_j = b_{\max} = 630{,}007$, which is the left edge of the outermost stripe. This is acceptable as a conservative estimate (the right edge would give a slightly larger displacement), but it should be stated which point on the stripe is being measured.

- **Suggested fix:** Clarify that $b_{\max}$ refers to the $b_{\text{lo}}$ of the outermost stripe, and note that the actual maximum $b$-coordinate is $b_{\max} + W$.

---

## Issue 3: The Logistic Model Is Fit to a Narrow Window and May Not Be the Correct Functional Form

- **Severity:** Major
- **Location:** Section 2.2, Eqs. (19)-(21)
- **Issue:**

The logistic fit is calibrated using only 3 data points (R = 700M, 800M, 900M) in the transition region. The claim that "the logit is approximately linear in R across this range, confirming the logistic model" is circular: any smooth monotone function will look approximately linear over 3 points spanning a small range.

The logistic function has a specific mathematical character: symmetric tails and a single inflection point. There is no a priori reason why the ISE crossing fraction should follow a logistic. Alternative functional forms include:

1. **Complementary error function (erfc):** $f(r) = \frac{1}{2}\operatorname{erfc}\left(\frac{r - R_{0.5}}{\sigma\sqrt{2}}\right)$, which is the natural model if connectivity loss is driven by a Gaussian random variable crossing a threshold. This is arguably more physically motivated for a percolation-like transition.

2. **Power law decay:** $f(r) = (r_0/r)^\alpha$ for large $r$, motivated by the inverse-log density decay of primes.

3. **Gompertz or asymmetric sigmoid:** The transition might be asymmetric (sharper onset, longer tail, or vice versa). The data table hints at this: $f$ drops from 1.0 to 0.90 in one 100M step (600M to 700M) but from 0.08 to 0.016 over a 300M range (1.2B to 1.5B). An asymmetric model would shift $R_{0.5}$.

To test this: fit both logistic and erfc to the full 12-point dataset. If $R_{0.5}$ differs by more than the claimed $\pm 16$M CI between the two fits, the model choice itself is a dominant source of uncertainty not captured by the statistical analysis.

- **Suggested fix:** (a) Fit multiple functional forms to the existing data and report the sensitivity of $R_{0.5}$ to model choice. (b) After collecting the 6 new data points, perform a model selection test (e.g., AIC/BIC comparison between logistic, erfc, and Gompertz).

---

## Issue 4: Tile-Size Dependence of $f(r) = 0.5$ Is Not Addressed

- **Severity:** Critical
- **Location:** Section 2.3 (Definition 2.1 and Justification)
- **Issue:**

The document defines the ISE moat estimate as the radius where $f(r) = 0.5$. Justification point 4 claims: *"Tile-size effects inflate $f(r)$ in blocked regions and deflate it in connected regions... These effects shift the entire sigmoid but approximately preserve its midpoint."*

This claim is asserted without proof. The CTO paper (Section 6.1) discusses finite-size effects and notes that narrow tiles increase false blockage (they miss lateral paths). This means:

- At a given $r$, a narrower tile will measure a **lower** $f(r)$ than a wider tile.
- The sigmoid for $W = 2000$ tiles sits to the **left** of the sigmoid for $W = 4000$ tiles.
- The $f(r) = 0.5$ crossing point therefore occurs at a **smaller** radius for narrower tiles.

But the claim that the midpoint is "approximately preserved" requires the vertical shift to be approximately uniform across the sigmoid. If the shift is larger near $f = 0.9$ than near $f = 0.1$ (as one might expect: finite-size effects are strongest where connectivity barely survives), then the sigmoid narrows asymmetrically, and the midpoint shifts.

This is testable: the calibration analysis should contain tile-size sweep data. If $R_{0.5}$ at $W = 2000$ and $R_{0.5}$ at $W = 4000$ differ by more than the $\pm 16$M CI, then the $f(r) = 0.5$ crossing is not a tile-size-robust quantity, and the entire estimation framework is compromised.

The deeper question is whether there exists a tile-size-independent quantity that could serve as a moat estimator. Candidates:

- The radius at which $f(r) = 0$ (but this is hard to localize precisely).
- The radius at which a rescaled $f(r)$ crossing reaches a theoretically derived threshold (from percolation theory, e.g., the Cardy formula crossing probability for rectangles of a specific aspect ratio).
- Extrapolation from a finite-size scaling analysis: measure $R_{0.5}(W)$ for several widths and extrapolate to $W \to \infty$.

- **Suggested fix:** (a) State explicitly that $R_{0.5}$ is tile-size dependent. (b) Propose a finite-size scaling protocol: measure $R_{0.5}$ at $W \in \{1000, 2000, 4000\}$ and extrapolate. (c) Remove or weaken the claim that the midpoint is "approximately preserved" unless supported by data.

---

## Issue 5: Outer-Face Radial Spread Derivation Has an Algebra Error

- **Severity:** Minor
- **Location:** Section 1.2, Proposition 1.3, Eq. (11)
- **Issue:**

Equation (10) gives:
$$R_{\text{outer},j} = \sqrt{R_{\text{target}}^2 + 2\,a_{\min,j}\,H + H^2}.$$

Equation (11) claims:
$$\Delta R_{\text{outer}} = R_{\text{outer},0} - R_{\text{outer},M-1} \approx \frac{H \cdot (a_{\min,0} - a_{\min,M-1})}{R_{\text{target}} + H}.$$

Let me verify this. Write $R_j = \sqrt{C + 2 a_j H}$ where $C = R_{\text{target}}^2 + H^2$. Then:

$$R_0 - R_{M-1} = \sqrt{C + 2 a_0 H} - \sqrt{C + 2 a_{M-1} H}$$

Using $\sqrt{x+\epsilon} - \sqrt{x} \approx \epsilon/(2\sqrt{x})$ for small $\epsilon$:

$$R_0 - R_{M-1} \approx \frac{2H(a_0 - a_{M-1})}{2\sqrt{C + 2 a_0 H}} = \frac{H(a_0 - a_{M-1})}{\sqrt{R_{\text{target}}^2 + 2 a_0 H + H^2}}.$$

Now $\sqrt{R_{\text{target}}^2 + 2 a_0 H + H^2} = \sqrt{(a_0 + H)^2 + b_0^2}$. For the on-axis stripe ($b_0 = c \approx 7$), this is approximately $a_0 + H \approx R_{\text{target}} + H$. So Eq. (11) is approximately correct. However, the derivation skips the step of explaining which square root is being linearized and under what approximation. For a mathematical paper, this should be explicit.

The numerical result $\Delta R_{\text{outer}} \approx 0.0004$ units is correct and correctly identified as negligible. The issue is purely expositional.

- **Suggested fix:** Add one line explaining the linearization used to go from Eq. (10) to Eq. (11).

---

## Issue 6: The Delta Method for $R_{0.5}$ Requires Smoothness That the Estimator Does Not Have

- **Severity:** Major
- **Location:** Section 3.5, Proposition 3.3, Eq. (31)
- **Issue:**

The delta method is applied to derive the standard error of $\hat{R}_{0.5}$. This requires:

1. That $f(r)$ is a smooth function of $r$.
2. That $\hat{R}_{0.5}$ is obtained by inverting a smooth estimator.

But $\hat{f}(r)$ is a discrete estimator: it takes values in $\{0, 1/N, 2/N, \ldots, 1\}$ with $N = 1280$. The "inversion" to obtain $\hat{R}_{0.5}$ is done by interpolation between two discrete probe radii, not by inverting a smooth function. The delta method's regularity conditions (differentiability of the mapping from data to estimate) are not satisfied in the strict sense.

In practice, this is probably fine because $N = 1280$ is large enough that the discreteness of $\hat{f}$ is much finer than the CI width. The step size of $\hat{f}$ is $1/1280 \approx 0.00078$, which is much smaller than $\sigma_{\hat{f}} = 0.014$. So the discrete estimator is well-approximated by a continuous one.

But the interpolation introduces a separate concern: $\hat{R}_{0.5}$ depends not just on the noise in $\hat{f}$ at one radius, but on the noise at two adjacent probe radii and on the interpolation method. The delta-method formula Eq. (31) accounts for uncertainty in $\hat{f}$ at one point only. A proper crossing-point uncertainty would need to account for the joint distribution of $(\hat{f}(r_i), \hat{f}(r_{i+1}))$ where $r_i < R_{0.5} < r_{i+1}$.

If the two adjacent probes are independent (different radii, different shells), then the interpolated crossing point has variance:
$$\sigma_{R_{0.5}}^2 \approx \frac{\sigma_{\hat{f}_i}^2 + \sigma_{\hat{f}_{i+1}}^2}{(\hat{f}_i - \hat{f}_{i+1})^2 / (r_{i+1} - r_i)^2}$$
which differs from Eq. (31) by a factor of roughly $\sqrt{2}$ when both points have similar variance.

At 50M probe spacing, with $\sigma_{\hat{f}} = 0.014$ and the logistic slope giving $\Delta f \approx 0.17$ per 50M step at the midpoint, the corrected CI would be approximately:
$$\sigma_{R_{0.5}} \approx \frac{50\text{M} \cdot \sqrt{0.014^2 + 0.014^2}}{0.17} \approx \frac{50\text{M} \cdot 0.020}{0.17} \approx 5.8\text{M}.$$

This is actually *tighter* than the document's $\pm 16$M, because the document uses the global sigmoid slope rather than the local finite-difference slope at the crossing. The point is that the two approaches give different answers, and the choice should be justified.

- **Suggested fix:** (a) State that the delta method is an approximation valid for large $N$. (b) For the actual CI, use the finite-difference interpolation formula with joint uncertainty from two adjacent probes rather than the single-point implicit function theorem. (c) Report both estimates and note which is conservative.

---

## Issue 7: "Common Success Probability" Assumption Across Shells

- **Severity:** Major
- **Location:** Section 3.1, Eq. (24) and the paragraph following
- **Issue:**

The observation model states: *"the $X_{j,s}$ are independent Bernoulli trials with common success probability $p = p(r)$."* Here $j$ ranges over stripes and $s$ ranges over shells within a probe.

But a single probe spans 20 shells covering a radial range of $H \times 20 = 40{,}000$ units. The success probability $p(r)$ varies with $r$. Across 40,000 units at the transition center (where $\beta \approx 6.7 \times 10^{-9}$), the change in $p$ is:
$$\Delta p \approx |f'(R_{0.5})| \times 40{,}000 = 1.68 \times 10^{-9} \times 40{,}000 = 6.7 \times 10^{-5}.$$

This is negligible ($\Delta p \ll \sigma_{\hat{f}}$), so the "common $p$" assumption is justified within a single probe. This should be stated explicitly as a check rather than assumed silently.

However, there is a subtler issue: the document pools all $N = M \times S = 1280$ observations to estimate a single $\hat{f}(r)$ per probe. But the 20 shells sample $p(r)$ at 20 slightly different radii. The pooled estimator $\hat{f}$ is actually estimating the average $\bar{p} = \frac{1}{S}\sum_{s=1}^S p(r_s)$, not $p$ at any single radius. When the probe is then used to localize $R_{0.5}$, the probe radius is attributed as the midpoint of the probe range. The error from this radial averaging is at most $H \times S / 2 = 20{,}000$ units $= 0.02$M, which is negligible compared to the $\pm 16$M CI. But again, this should be stated.

- **Suggested fix:** Add a remark after Eq. (24) verifying that the within-probe variation of $p(r)$ is negligible and that the pooled estimator estimates $\bar{p}$ over the probe's radial range.

---

## Issue 8: The Independence Model Is Stronger Than Acknowledged

- **Severity:** Minor
- **Location:** Section 1.4 (last paragraph), Section 3.1
- **Issue:**

The document claims that curvature compensation preserves stripe independence because "the collared tiles under compensated placement remain disjoint in the $b$-direction." This is correct for the lateral separation.

However, curvature compensation introduces a new feature: different stripes now have different $a_{\min,j}$ values. This means they are sampling different (though overlapping) radial intervals. Stripe 0 might span $a \in [a_0, a_0 + H]$ while stripe 63 spans $a \in [a_0 - 199, a_0 + H - 199]$ (approximately). The inner faces are at the same Euclidean radius, but the tiles occupy different lattice rectangles.

The independence claim in Section 4.4 of the CTO paper is about non-overlapping collared tiles in the lattice. Under compensated placement, the tiles are still non-overlapping in $b$ (since only the $a$-range changes per stripe), so the formal independence argument is preserved. However, the tiles are now at different $a$-ranges, which means they sample slightly different prime populations in the $a$-direction. This is the intended behavior (to correct for curvature), but it subtly changes what "independent samples of the same quantity" means.

In the uncompensated case, all stripes measure connectivity at the same $a$-range but different Euclidean radii. In the compensated case, all stripes measure connectivity at the same Euclidean radius but different $a$-ranges. The second is arguably more principled, but the document should be explicit that the measurement target has changed.

- **Suggested fix:** Add a remark noting that compensated stripes sample the same Euclidean radius at different lattice $a$-ranges, and that this is the desired behavior.

---

## Issue 9: $\pm 16$M Precision vs. LB/UB Campaign Targeting

- **Severity:** Major
- **Location:** Section 3.5, Eq. (33); Section 4.5
- **Issue:**

The final deliverable is $R_{0.5} \approx 826\text{M} \pm 16\text{M}$ (95% CI). The question is whether this precision is useful.

The LB/UB campaign needs to identify a specific radial region where a moat exists and then exhaustively verify it. A $\pm 16$M window means the moat transition center could be anywhere in a 32M-wide band. To run seed-connected tracing over a 32M-wide band at the radii involved ($\sim 800$M) would be computationally prohibitive.

However, the ISE narrowing plan's purpose is not to locate the moat to a single lattice unit -- it is to identify the transition region so that future campaigns can target it. In that context, $\pm 16$M is probably adequate for a first pass, since the sigmoid transition spans $\sim 200$M. The ISE estimate narrows the interesting region by an order of magnitude.

But the document does not discuss how the ISE moat estimate relates to the actual moat location. The $f(r) = 0.5$ crossing is not the moat radius. The moat radius is where connectivity truly dies ($f(r) = 0$). The document should clarify the operational use of $R_{0.5}$: is it a proxy for the moat location? A guide for where to run the LB probe? A descriptive statistic for the transition?

- **Suggested fix:** Add a paragraph to Section 4.5 explaining how $R_{0.5}$ will be used operationally. Specifically: the LB/UB campaign should target the region $R > R_{0.5}$, where $f(r) < 0.5$, since that is where blockage becomes likely. The ISE narrowing plan's contribution is ruling out the region $R < R_{0.5} - 16\text{M}$ as "too connected to contain the moat."

---

## Issue 10: Probe Commands Do Not Include the Curvature-Compensation Flag

- **Severity:** Minor
- **Location:** Section 4.1
- **Issue:**

The probe commands in Section 4.1 do not include the `--curvature-compensated` flag. The document states this flag "requires implementation per Section 1.4," so the 6 new probes would run without compensation. Only the optional full rerun in Section 4.2 mentions compensation.

This is arguably intentional (run the gap-filling probes first with the existing code, then optionally rerun with compensation), but it creates a mixed dataset: the existing 12 probes are uncompensated, the 6 new probes would also be uncompensated, and only the optional rerun produces a compensated dataset.

Given that the curvature displacement is $\sim 10\%$ of tile height at the campaign radius, and the sigmoid has a slope of $\sim 0.17$ per 50M, the radial smear of $\sim 200$ units shifts $f(r)$ by approximately $|f'| \times 200 \approx 3.4 \times 10^{-7}$, which is negligible. The curvature compensation is a theoretical nicety that makes no practical difference at these parameters. The document acknowledges this in Section 4.4 (expected compensation effect $|\Delta f| < 0.03$), but the theoretical derivation in Part 1 occupies 2 pages and sets up the reader to expect it matters.

- **Suggested fix:** Either (a) state upfront that curvature compensation is negligible at these parameters and Part 1 is included for completeness, or (b) implement the flag and include it in all probe commands.

---

## Issue 11: The Logistic Slope Formula $f'(R_{0.5}) = -\beta/4$ Is Correct but the Usage Is Inconsistent

- **Severity:** Minor
- **Location:** Section 3.5
- **Issue:**

The document states: *"The maximum slope of a logistic function $1/(1+e^{\beta(r-R_0)})$ is $\beta/4$, attained at $r = R_0$."*

This is correct. The derivative of $f(r) = (1+e^{\beta(r-R_0)})^{-1}$ is $f'(r) = -\beta e^{\beta(r-R_0)} / (1+e^{\beta(r-R_0)})^2$. At $r = R_0$: $f'(R_0) = -\beta/4$. The magnitude is $\beta/4$.

With $\beta \approx 6.7 \times 10^{-9}$: $|f'(R_{0.5})| = 6.7 \times 10^{-9}/4 = 1.675 \times 10^{-9}$. The document rounds to $1.68 \times 10^{-9}$. Fine.

But note that $\beta$ was estimated from data between R=700M and R=900M using the logit slope: $\beta = (0.896 + 0.446)/(200 \times 10^6) = 1.342/(2 \times 10^8) = 6.71 \times 10^{-9}$. This $\beta$ is a finite-difference estimate of the logistic slope parameter, not a fit to the full dataset. The true $\beta$ from a weighted nonlinear least-squares fit to all 12 data points might differ.

Additionally, the $\beta$ estimate from Section 2.2 uses the fact that $\text{logit}(f) = -\beta(r - R_{0.5})$, so $\beta = -\Delta\text{logit}/\Delta r$. The sign convention: $\Delta\text{logit} = \text{logit}(0.39) - \text{logit}(0.71) = -0.446 - 0.896 = -1.342$. $\Delta r = 900\text{M} - 700\text{M} = 200\text{M}$. So $\beta = -(-1.342)/(200\text{M}) = 6.71 \times 10^{-9}$. This is consistent. No error here, just noting that the sign convention should be tracked carefully.

- **Suggested fix:** No fix needed. This is a nit for verification purposes only.

---

## Issue 12: The False-Extinction Bound Conflates Two Different Questions

- **Severity:** Minor
- **Location:** Section 3.7, Eq. (34)
- **Issue:**

Equation (34) computes $\Pr(\hat{f} = 0 \mid p)$, the probability of observing zero crossings when the true crossing probability is $p$. This is used to bound the true $p$ given the observed $\hat{f} = 0$.

The interpretation "if $\hat{f} = 0$, we can be confident that $p < 0.005$" inverts the probability. Strictly, $(1-0.005)^{1280} = 1.6 \times 10^{-3}$, so $\Pr(\hat{f} = 0 \mid p = 0.005) = 0.0016$. This means: if $p = 0.005$, the chance of seeing $\hat{f} = 0$ is $0.16\%$. So observing $\hat{f} = 0$ makes $p \ge 0.005$ implausible at the $0.16\%$ level.

The document says "at the $0.2\%$ level," which is correct (rounding $0.16\%$ up). But it then says "implies $p(2.5\text{B}) < 0.005$ with very high confidence." The phrase "very high confidence" is somewhat generous for a $99.8\%$ credibility, but this is a stylistic point, not a mathematical error.

The real concern: this bound is for a single probe. Across 12+ probes (multiple radii), the probability of at least one false extinction somewhere is higher (by a Bonferroni-like factor). The document does not discuss multiple testing.

- **Suggested fix:** Add a sentence noting that the false-extinction bound applies per probe and that multiple testing across all radii is not a concern here because extinction at $R = 2.5$B is confirmed by multiple consecutive shells all showing $\hat{f} = 0$.

---

## Issue 13: The Monotonicity Check Is Too Weak

- **Severity:** Minor
- **Location:** Section 4.4 (Success Criteria table)
- **Issue:**

The monotonicity criterion is: $f(r_i) - f(r_{i+1}) > -0.05$ for all $i$. This allows a non-monotonicity of up to $0.05$. Given that the 95% CI half-width for $\hat{f}$ at the transition is $\sim 0.027$, a non-monotonicity of $0.05$ is roughly $2\sigma$. This is reasonable as a "flag for investigation" threshold.

However, the criterion is stated as a one-sided check. It should also flag cases where $f(r_i) = f(r_{i+1})$ exactly (suggesting a flat region, which would invalidate the sigmoid model) and cases where the decline is much steeper than the logistic model predicts (suggesting a non-logistic transition).

- **Suggested fix:** This is fine as a first-pass check. Consider adding a chi-squared goodness-of-fit test for the logistic model as a more principled monotonicity/shape check.

---

## Issue 14: The Table in Section 3.6 Claims Doubling M Halves Runtime

- **Severity:** Nit
- **Location:** Section 3.6, paragraph after the table
- **Issue:**

The text states: *"Doubling $M$ from 64 to 128 halves the runtime per probe."* This is backwards. Doubling $M$ means twice as many stripes, which should approximately double the runtime per probe (or at least increase it, modulo parallelism effects). The precision improvement is $\sqrt{2}$, not $2$.

The sentence likely intended to say something like "doubling $M$ provides $\sqrt{2}$ improvement in precision at $2\times$ the computational cost."

- **Suggested fix:** Correct the sentence. Doubling $M$ roughly doubles runtime and gives $\sqrt{2}$ improvement in CI width.

---

## Issue 15: The Rectangular Tile Calibration Is Referenced but Not Cited

- **Severity:** Minor
- **Location:** Section 2.3, Justification point 4
- **Issue:**

The document references *"the rectangular tile calibration analysis"* as having documented tile-size effects. This analysis is not cited with a filename or date. The calibration manifest (`2026-03-22-calibration-manifest.md`) might contain this, but the reader cannot verify the claim without a specific reference.

- **Suggested fix:** Add a citation: "as documented in [calibration manifest filename]."

---

## Summary of Issues by Severity

| Severity | Count | Issues |
|----------|-------|--------|
| Critical | 2 | #1 (collar), #4 (tile-size dependence) |
| Major | 4 | #3 (logistic model), #6 (delta method), #7 (common-p assumption), #9 (precision utility) |
| Minor | 6 | #2, #5, #8, #10, #12, #13, #15 |
| Nit | 2 | #11, #14 |

---

## Overall Verdict: **Accept with Revisions**

The mathematical framework is sound. The curvature compensation derivation is correct and the statistical analysis is fundamentally right, though several intermediate claims need tightening. The two critical issues are:

1. **The collar inconsistency (Issue 1)** must be resolved before this document can serve as a standalone reference. The reader needs to know which collar definition is canonical.

2. **The tile-size dependence of $R_{0.5}$ (Issue 4)** is the deepest concern. The ISE moat estimate is only meaningful if it is understood as a property of a specific tile geometry, not as a property of the prime graph itself. The document should either propose a finite-size scaling extrapolation or clearly label $R_{0.5}$ as the "$2000 \times 2000$-tile crossing estimate."

Among the major issues, the most actionable are:

- **Logistic model validation (Issue 3):** fit erfc as an alternative and compare $R_{0.5}$.
- **Delta method correction (Issue 6):** use the two-point interpolation formula instead of the single-point implicit function theorem.
- **Sentence correction (Issue 14):** "halves the runtime" is simply wrong.

None of the issues invalidate the plan. The sweep protocol is well-designed, the runtime estimates are realistic, and the gap-filling probes are placed at the right radii. After addressing the critical and major issues, this document would be a strong foundation for the k^2=40 narrowing campaign.
