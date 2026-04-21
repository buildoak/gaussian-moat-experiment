#!/usr/bin/env python3
"""
Assemble the Gaussian Moat visual explainer HTML.
Reads all PNGs from plots/, encodes as base64, embeds in self-contained HTML.
"""
import base64
import os

PLOTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "plots")
OUT_HTML = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "2026-03-20-moat-explainer.html")


def b64(filename):
    path = os.path.join(PLOTS_DIR, filename)
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode("ascii")


sections = [
    {
        "num": 1,
        "title": "Gaussian Primes: The Lattice",
        "img": "01_gaussian_primes.png",
        "text": (
            "Gaussian integers are complex numbers a+bi where a,b are integers. "
            "A Gaussian integer is <em>prime</em> if it can't be factored further "
            "(up to units). The open question: starting from any Gaussian prime, "
            "can you walk to infinity by stepping only to neighboring primes "
            "within distance &radic;k?"
        ),
        "bridge": (
            "Neighborhood graph at step &radic;k &harr; "
            "Connectivity of the Gaussian prime lattice"
        ),
    },
    {
        "num": 2,
        "title": "Annulus to Strip: Unrolling the Geometry",
        "img": "02_annulus_to_strip.png",
        "text": (
            "Far from the origin, an annular ring of primes is essentially flat &mdash; "
            "curvature scales as 1/R. We can &ldquo;unroll&rdquo; the ring into a "
            "rectangular strip where the x-axis is angle and y-axis is radius. "
            "This transforms a curved 2D problem into a flat one, which is far "
            "easier to analyze with matrix methods."
        ),
        "bridge": (
            "Translate to (0,0) &harr; Cylinder coordinates &mdash; "
            "local coords where curvature vanishes"
        ),
    },
    {
        "num": 3,
        "title": "Why Fixed-Width Strips, Not Fixed-Angle Wedges",
        "img": "03_strip_geometry.png",
        "text": (
            "A fixed-angle wedge gets physically wider as R grows, so the local "
            "statistics change with radius &mdash; useless for comparison. A fixed "
            "physical-width strip shrinks in angle as &Delta;&theta; = W/R, keeping "
            "local prime density comparable across all radii. This is the sampling "
            "geometry that makes transfer matrices work."
        ),
        "bridge": (
            "Strip not annulus &harr; Fixed-width sampling &mdash; "
            "comparable local statistics"
        ),
    },
    {
        "num": 4,
        "title": "Prime Angle Gaps Are Random",
        "img": "04_prime_angle_gaps.png",
        "text": (
            "Rudnick &amp; Waxman (2019) showed that the angular gaps between "
            "consecutive Gaussian primes at large radius follow an exponential "
            "distribution &mdash; exactly like gaps between uniform random points. "
            "The left panel shows actual prime gaps; the right shows random gaps. "
            "They're statistically indistinguishable. This means primes don't "
            "cluster or repel &mdash; locally, they behave like Poisson rain."
        ),
        "bridge": None,
    },
    {
        "num": 5,
        "title": "The Transfer Matrix",
        "img": "05_transfer_matrix.png",
        "text": (
            "The core computational tool. Given primes on an inner shell boundary "
            "and an outer shell boundary, the transfer matrix T encodes which inner "
            "ports can reach which outer ports within step distance &radic;k. "
            "Dead columns (no connections) are where the walk can't continue. "
            "Composing T across many shells reveals long-range connectivity."
        ),
        "bridge": (
            "Connectivity operator &harr; Transfer matrix &mdash; "
            "boundary-to-boundary reachability"
        ),
    },
    {
        "num": 6,
        "title": "The Four-Sided Tile",
        "img": "06_four_sided_tile.png",
        "text": (
            "Each tile has four faces: Inner (I), Outer (O), Left (L), Right (R). "
            "Interior primes are eliminated via Schur complement &mdash; only "
            "boundary ports survive. The overlap collar of width &radic;k ensures "
            "neighboring tiles share enough boundary to stitch together seamlessly. "
            "This is how we decompose the infinite strip into manageable pieces."
        ),
        "bridge": (
            "Flow / electricity &harr; Schur complement &mdash; "
            "eliminate interior, keep boundary"
        ),
    },
    {
        "num": 7,
        "title": "Three Observables: Boolean, Channels, Gap-Runs",
        "img": "07_three_observables.png",
        "text": (
            "We measure connectivity at three resolutions. <strong>Boolean crossing</strong>: "
            "does any path cross this shell? <strong>Channel count</strong>: how many "
            "independent paths cross? (When this hits zero &mdash; that's the moat.) "
            "<strong>Gap-run field</strong>: where do large angular gaps persist across "
            "multiple shells? These &ldquo;danger corridors&rdquo; are where moats nucleate."
        ),
        "bridge": (
            "Gap-run alarm &harr; Dangerous gap persistence &mdash; "
            "run-length across shells"
        ),
    },
    {
        "num": 8,
        "title": "Anderson Localization: When the Walk Dies",
        "img": "08_anderson_localization.png",
        "text": (
            "By analogy with wave propagation through random media: the walk's "
            "amplitude decays exponentially through shells of random prime barriers. "
            "The Lyapunov exponent &gamma; is the decay rate. If &gamma; &gt; 0, "
            "the walk is <em>localized</em> &mdash; it dies out, meaning a moat exists "
            "at predictable radius. If &gamma; &le; 0, the walk is robust or critical."
        ),
        "bridge": (
            "Measuring decay &harr; Lyapunov exponent &mdash; "
            "rate predicts WHERE the moat lives"
        ),
    },
    {
        "num": 9,
        "title": "Computational Cost: Why Strips Win",
        "img": "09_cost_comparison.png",
        "text": (
            "Brute-forcing a full annulus from radius 80M to 10B requires ~10<sup>18</sup> "
            "primes &mdash; impossible. A single fixed-width strip (W=128) covers the "
            "same radial range with ~3.7&times;10<sup>10</sup> primes, computable in hours. "
            "The transfer matrix reduces each shell to ~25 boundary ports. That's a "
            "~10<sup>7</sup>&times; compression."
        ),
        "bridge": (
            "Annulus throughput &harr; Conductance / Min-cut &mdash; "
            "how much transport flows through the bottleneck"
        ),
    },
    {
        "num": 10,
        "title": "Rosetta Stone: Engineering &harr; Mathematics",
        "img": "10_rosetta_stone.png",
        "text": (
            "Every engineering idea in this pipeline has a precise mathematical name. "
            "This table maps between the two vocabularies so you can read the "
            "number theory papers and the code side by side."
        ),
        "bridge": None,
    },
]

# ── Build HTML ────────────────────────────────────────────────────────────
parts = []
parts.append("""\
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gaussian Moat Problem — Visual Explainer</title>
<style>
  :root {
    --bg: #0a0a0f;
    --text: #d8d8e0;
    --muted: #888899;
    --accent-blue: #4a9eff;
    --accent-orange: #ff6b35;
    --accent-green: #2ecc71;
    --accent-purple: #a66bff;
    --card-bg: #0f0f1a;
    --border: #1a1a2e;
  }
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto,
                 Helvetica, Arial, sans-serif;
    line-height: 1.7;
    padding: 0;
  }
  .container {
    max-width: 960px;
    margin: 0 auto;
    padding: 2rem 1.5rem;
  }
  h1 {
    font-size: 2rem;
    font-weight: 700;
    margin-bottom: 0.5rem;
    color: #fff;
    letter-spacing: -0.02em;
  }
  .subtitle {
    color: var(--muted);
    font-size: 1.05rem;
    margin-bottom: 3rem;
    border-bottom: 1px solid var(--border);
    padding-bottom: 1.5rem;
  }
  section {
    margin-bottom: 3.5rem;
  }
  .section-header {
    display: flex;
    align-items: baseline;
    gap: 0.75rem;
    margin-bottom: 1rem;
  }
  .section-num {
    font-size: 1.1rem;
    font-weight: 700;
    color: var(--accent-blue);
    min-width: 2rem;
  }
  .section-title {
    font-size: 1.35rem;
    font-weight: 600;
    color: #fff;
  }
  .plot-wrap {
    background: var(--card-bg);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 0.75rem;
    margin-bottom: 1.25rem;
    text-align: center;
  }
  .plot-wrap img {
    max-width: 100%;
    height: auto;
    border-radius: 4px;
  }
  .explanation {
    font-size: 0.98rem;
    color: var(--text);
    margin-bottom: 0.75rem;
    padding: 0 0.25rem;
  }
  .bridge {
    background: #111122;
    border-left: 3px solid var(--accent-purple);
    padding: 0.6rem 1rem;
    border-radius: 0 6px 6px 0;
    font-size: 0.92rem;
    color: var(--accent-purple);
  }
  .bridge strong {
    color: var(--accent-orange);
  }
  footer {
    margin-top: 4rem;
    padding-top: 1.5rem;
    border-top: 1px solid var(--border);
    color: var(--muted);
    font-size: 0.85rem;
    text-align: center;
  }
</style>
</head>
<body>
<div class="container">

<h1>The Gaussian Moat Problem</h1>
<p class="subtitle">
  A visual walkthrough: from prime lattice to transfer matrices, strip sampling,
  and Lyapunov exponents. Ten plots, zero JavaScript.
</p>
""")

for sec in sections:
    img_b64 = b64(sec["img"])
    parts.append(f"""
<section>
  <div class="section-header">
    <span class="section-num">{sec['num']:02d}</span>
    <span class="section-title">{sec['title']}</span>
  </div>
  <div class="plot-wrap">
    <img src="data:image/png;base64,{img_b64}" alt="{sec['title']}">
  </div>
  <p class="explanation">{sec['text']}</p>
""")
    if sec.get("bridge"):
        parts.append(f"""  <div class="bridge">Your idea &harr; The math: {sec['bridge']}</div>
""")
    parts.append("</section>\n")

parts.append("""
<footer>
  Generated 2026-03-20 &middot; gaussian-moat-cuda/research &middot;
  Plots: matplotlib, dark theme, 150 DPI
</footer>

</div>
</body>
</html>
""")

html = "".join(parts)
with open(OUT_HTML, "w") as f:
    f.write(html)

size_kb = os.path.getsize(OUT_HTML) / 1024
print(f"Written {OUT_HTML}")
print(f"Size: {size_kb:.0f} KB ({len(sections)} sections, all images embedded)")
