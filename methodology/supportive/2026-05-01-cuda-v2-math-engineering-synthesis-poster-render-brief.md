---
title: "Poster 3 Render Brief: CUDA v2 Math + Engineering Synthesis"
date: 2026-05-01
type: poster-render-brief
status: ready-for-native-image-generation
scope: tiles-maxxing/cuda-campaign-v2-sqrt-36
---

# Poster 3 Render Brief: CUDA v2 Math + Engineering Synthesis

## Evidence Boundary

Use the poster as an educational synthesis of existing source material. Do not
present new proof, new benchmark, new profiling result, new correctness result,
or a CUDA performance claim. The safe visual thesis is:

> the math defines the invariant; the data structures preserve it; CUDA
> manufactures local TileOp witnesses; the host stitches them into a verdict;
> gates check that the invariant did not move.

## Title

Exact poster title:

```text
CUDA v2 Math + Engineering Synthesis
```

Optional small subtitle, only if rendered cleanly:

```text
Math object -> TileOp witness -> global verdict
```

## Aspect Ratio And Composition

- Aspect ratio: `3:4` portrait raster poster, 4K preferred.
- Composition: clean technical educational poster, one-page infographic.
- Main flow: left to right, with six labeled stages:
  `MATH OBJECT -> DATA STRUCTURE -> CUDA KERNEL -> TILEOP CONTRACT -> HOST ORCHESTRATION -> VALIDATION GATE`.
- Centerpiece: large crisp `TileOp{256B}` byte-contract block in the center.
- Bottom rail: small `IMPROVEMENT AREAS` strip with cautious engineering icons.
- Use icons, diagrams, arrows, and compact labels rather than paragraphs.

## Visual Language

Use a blueprint-to-foundry metaphor:

- Left third: blueprint math layer, thin canon-blue linework over an annular octant.
- Middle third: graphite data structures and amber CUDA kernel chevrons.
- Center: precise byte-layout object labeled `TileOp{256B}`.
- Right third: host compositor, stitched face ports, green validation gates.
- Footer: improvement surfaces as red/amber caution tags, not achievement badges.

Palette:

- Canon blue: `#1D5BFF`
- Implementation graphite: `#30343B`
- Kernel amber: `#F2A900`
- Gate green: `#22A06B`
- Caution red: `#D64545`
- Background: warm off-white `#F7F3E8`

Rendering style:

- Flat technical illustration with subtle paper texture.
- Crisp line art, diagrammatic geometry, high contrast.
- Use vector-poster clarity even though output is raster.
- No photorealism, no abstract glowing sci-fi scene, no decorative code rain.

## Exact Short Labels To Appear

Render only these labels, exactly as written. Avoid adding extra words.

```text
CUDA v2 Math + Engineering Synthesis
Math object -> TileOp witness -> global verdict
MATH OBJECT
DATA STRUCTURE
CUDA KERNEL
TILEOP CONTRACT
HOST ORCHESTRATION
VALIDATION GATE
IMPROVEMENT AREAS
G_full
G_tile
geo_I / geo_O
TileCoord
Grid
CampaignConstants
K1 sieve
K2 MR
K3 compact
K4 UF
K5 ports
TileOp{256B}
CPU compositor
snapshot SHA
known answer
memory overlay
MR tuning
face encode
compositor overlap
```

If the generator cannot fit all labels cleanly, prioritize the title, six stage
labels, `TileOp{256B}`, `K1 sieve` through `K5 ports`, `CPU compositor`,
`snapshot SHA`, `known answer`, and `IMPROVEMENT AREAS`.

## Diagram Objects

- Annular octant with blue inner and outer bands for `geo_I / geo_O`.
- Closed square tile grid over the annulus, with one enlarged halo tile.
- Data-structure boxes for `Grid`, `TileCoord`, and `CampaignConstants`.
- Amber kernel conveyor with five chevrons: `K1 sieve`, `K2 MR`, `K3 compact`,
  `K4 UF`, `K5 ports`.
- Central byte ruler labeled `TileOp{256B}` with small segmented blocks, but no
  tiny byte-offset text.
- Two adjacent tiles with matching face-port dots and bridge lines.
- Host-side compositor as a green/graphite DSU network labeled `CPU compositor`.
- Validation gates as green shields labeled `snapshot SHA` and `known answer`.
- Improvement footer with four compact caution tags: `memory overlay`,
  `MR tuning`, `face encode`, `compositor overlap`.

## No-Garbled-Text Instruction

The poster must contain no random glyphs, no lorem ipsum, no pseudo-code filler,
no invented labels, no malformed mathematical notation, and no extra text beyond
the approved label list. All labels must be crisp, correctly spelled, horizontal,
and readable. If any label would be too small or uncertain, omit it instead of
rendering distorted text.

## Negative Claim Guardrails

Do not depict or imply:

- the GPU proving the theorem;
- a performance speedup or benchmark result;
- a solved improvement area;
- global union-find running entirely on the GPU;
- overflow as a successful moat result;
- diagonal tile bridges.

Show CUDA as manufacturing local `TileOp{256B}` witnesses and the CPU host as
performing global stitching and verdict validation.

## FINAL IMAGE PROMPT

```text
Create a 3:4 portrait technical educational raster poster titled "CUDA v2 Math + Engineering Synthesis".

Make a clean one-page infographic with a warm off-white paper background (#F7F3E8), crisp vector-like line art, subtle blueprint texture, and high-contrast readable labels. The poster explains the pipeline:
MATH OBJECT -> DATA STRUCTURE -> CUDA KERNEL -> TILEOP CONTRACT -> HOST ORCHESTRATION -> VALIDATION GATE.

Composition: left-to-right flow. On the left, draw a canon-blue (#1D5BFF) annular octant with blue inner and outer boundary bands labeled "geo_I / geo_O", plus small labels "G_full" and "G_tile". Overlay a closed square tile grid and one enlarged halo tile. Next, show graphite (#30343B) data boxes labeled "Grid", "TileCoord", and "CampaignConstants". In the middle, show an amber (#F2A900) CUDA foundry conveyor with five chevrons labeled exactly "K1 sieve", "K2 MR", "K3 compact", "K4 UF", and "K5 ports". At the center, place the largest object: a precise segmented byte-contract block labeled "TileOp{256B}" under the stage label "TILEOP CONTRACT". On the right, show two adjacent tiles with matching face-port dots connected by bridge lines, flowing into a green-and-graphite DSU network labeled "CPU compositor". End with two green (#22A06B) shield gates labeled "snapshot SHA" and "known answer".

Add a compact footer strip labeled "IMPROVEMENT AREAS" with four small caution tags in amber/red: "memory overlay", "MR tuning", "face encode", and "compositor overlap". These should look like future work surfaces, not completed achievements.

Use only these exact text labels and no others:
"CUDA v2 Math + Engineering Synthesis"
"Math object -> TileOp witness -> global verdict"
"MATH OBJECT"
"DATA STRUCTURE"
"CUDA KERNEL"
"TILEOP CONTRACT"
"HOST ORCHESTRATION"
"VALIDATION GATE"
"IMPROVEMENT AREAS"
"G_full"
"G_tile"
"geo_I / geo_O"
"TileCoord"
"Grid"
"CampaignConstants"
"K1 sieve"
"K2 MR"
"K3 compact"
"K4 UF"
"K5 ports"
"TileOp{256B}"
"CPU compositor"
"snapshot SHA"
"known answer"
"memory overlay"
"MR tuning"
"face encode"
"compositor overlap"

All text must be crisp, correctly spelled, horizontal, and readable. Do not add lorem ipsum, fake code, pseudo-code, random symbols, extra labels, malformed math, or decorative text. If a label cannot be rendered cleanly, omit that label rather than garbling it.

Visual tone: blueprint-to-foundry synthesis, serious engineering artifact, flat technical illustration, crisp arrows, compact labels, no photorealism, no decorative code rain, no unsupported speedup claims. CUDA is shown as manufacturing local TileOp witnesses; the CPU host stitches witnesses into the global verdict; validation gates check the route.
```
