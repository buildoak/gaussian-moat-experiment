# Poster 2 Render Brief: Tile Operator Mathematical Spine

## Render Target

Create one generated raster poster for an educational math visualization of the Tile Operator pipeline.

The poster should show the exact conceptual spine:

```text
G_full -> geo_I / geo_O -> snapped tiles -> ports -> TileOp -> stitched port graph -> theorem gate
```

Do not present this as a CUDA implementation poster. Do not invent proof claims beyond the listed theorem-gate language.

## Aspect Ratio And Composition

- Portrait poster, 2:3 aspect ratio.
- Single clean mathematical plate, suitable for high-resolution print.
- Top title band, then one vertical left-to-right flow spine through seven visual stations.
- Use a central arrow path that moves downward in a shallow S-curve, so each station has room for a diagram and one or two short labels.
- The first station is a global annular prime graph.
- The middle stations zoom into an octant staircase tile grid, then into one halo tile, then into face-strip ports.
- The lower stations show a compact `TileOp_T` object, then the stitched port graph, then a final theorem gate with two verdict outcomes.
- Leave generous whitespace around text. Text must be large, sparse, and sharply rendered.

## Visual Language

- Clean scientific illustration, vector-like raster finish.
- Warm off-white background with faint square grid paper texture.
- Thin black geometry lines, precise axes, crisp arrows.
- Prime vertices as small black dots.
- Inner boundary band in blue.
- Outer boundary band in red.
- Snapped active tiles in pale gold, with closed shared edges visibly overlapping.
- Tile halo as translucent teal collar around one enlarged tile.
- Face strips as translucent cyan/magenta/green/yellow bands on the enlarged tile.
- Ports as small colored connected blobs on face strips.
- `TileOp_T` as a compact operator card, not as code.
- Stitched port graph as black nodes and edges, with blue `I_ports` markers and red `O_ports` markers.
- Theorem gate at bottom: a formal gate/checkpoint graphic, not a decorative fantasy gate.

## Exact Visible Text

Render only these text strings. Do not add body copy, captions, extra equations, signatures, explanations, or labels.

```text
Tile Operator Mathematical Spine
G_full
E: ||p-q||^2 <= K
geo_I
geo_O
snapped closed tiles
C = floor(sqrt(K))
face strips
ports
TileOp_T
G_ports_grid
I_ports
O_ports
Theorem 11
geo_I !~ geo_O
no mixed component -> MOAT
mixed component -> SPANNING
```

## Required Visual Stations

1. `G_full`
   - Draw a sparse annular/octant prime graph with dots and short edges.
   - Show blue inner band `geo_I` and red outer band `geo_O`.
   - Include `E: ||p-q||^2 <= K`.

2. `geo_I` / `geo_O`
   - Show the two norm-boundary bands as geometric bands, not as exposed tile faces.
   - Keep the labels `geo_I` and `geo_O` close to the blue/red bands.

3. `snapped closed tiles`
   - Show the annular octant converted into a staircase of closed square tiles.
   - Make shared tile boundaries visibly aligned.
   - Include `C = floor(sqrt(K))`.

4. `face strips` and `ports`
   - Enlarge one tile with a translucent halo/collar.
   - Mark four face strips.
   - Show several colored connected components on the strips as `ports`.

5. `TileOp_T`
   - Show a compact operator object beside the tile.
   - It should visually map colored ports to local connectivity groups without adding text.

6. `G_ports_grid`
   - Show several tile port graphs stitched by exact ordinal bridge edges across shared faces.
   - Use blue markers for `I_ports` and red markers for `O_ports`.

7. `Theorem 11`
   - Bottom theorem gate checks whether one stitched component contains both blue and red markers.
   - Show two clean outcomes:
     - `no mixed component -> MOAT`
     - `mixed component -> SPANNING`
   - Include `geo_I !~ geo_O` near the moat outcome.

## No-Garbled-Text Instruction

All visible text must be exact, horizontal, legible, and copied only from the whitelist above. Use clean sans-serif mathematical typography. Do not approximate characters, mutate symbols, invent pseudo-text, add tiny unreadable paragraphs, or fill empty space with decorative text. If a label cannot fit cleanly, make the diagram simpler and keep the exact label.

## Negative Constraints

- No paragraphs on the poster.
- No long proof text.
- No unsupported theorem statements.
- No CUDA code, kernels, GPUs, terminal windows, benchmarks, or implementation tables.
- No random mathematical formulas outside the whitelist.
- No decorative fantasy imagery.
- No garbled microtext.
- No fake author names, dates, logos, QR codes, or watermarks.

## FINAL IMAGE PROMPT

```text
Create a single high-resolution portrait 2:3 educational math poster titled "Tile Operator Mathematical Spine".

Style: clean scientific illustration, vector-like raster poster, warm off-white faint grid-paper background, thin black geometry lines, crisp arrows, sparse exact typography, print-quality.

Composition: top title band, then a vertical left-to-right flow spine through seven stations:
G_full -> geo_I / geo_O -> snapped closed tiles -> face strips / ports -> TileOp_T -> G_ports_grid -> Theorem 11.
Use a central downward S-curve arrow connecting the stations. Keep text sparse and large.

Station 1: draw an annular/octant Gaussian-prime graph with small black prime dots and short edges. Label it "G_full". Add the exact equation label "E: ||p-q||^2 <= K". Show a blue inner norm band labeled "geo_I" and a red outer norm band labeled "geo_O".

Station 2: emphasize the two geometric boundary bands, blue "geo_I" and red "geo_O", as norm bands rather than exposed tile faces.

Station 3: transform the octant annulus into a staircase of pale-gold snapped closed square tiles with visibly aligned shared boundaries. Add a translucent teal collar/halo on one enlarged tile. Label this region "snapped closed tiles" and add "C = floor(sqrt(K))".

Station 4: enlarge one tile with four translucent face strips in cyan, magenta, green, and yellow. Show colored connected blobs on the strips as boundary components. Label the strips "face strips" and the blobs "ports".

Station 5: show a compact mathematical operator card labeled "TileOp_T" next to the enlarged tile. Use colored lines from ports into the card to imply local UF group compression, but do not add extra text.

Station 6: show several stitched port graphs across neighboring tiles, black nodes and edges, exact ordinal bridge edges across shared faces. Label the graph "G_ports_grid". Mark some vertices with blue "I_ports" badges and red "O_ports" badges.

Station 7: draw a formal theorem checkpoint at the bottom labeled "Theorem 11". It checks whether a stitched component contains both blue and red badges. Show two outcome lanes with exact labels: "no mixed component -> MOAT" and "mixed component -> SPANNING". Place "geo_I !~ geo_O" near the MOAT lane.

Only render these visible text strings, exactly:
"Tile Operator Mathematical Spine"
"G_full"
"E: ||p-q||^2 <= K"
"geo_I"
"geo_O"
"snapped closed tiles"
"C = floor(sqrt(K))"
"face strips"
"ports"
"TileOp_T"
"G_ports_grid"
"I_ports"
"O_ports"
"Theorem 11"
"geo_I !~ geo_O"
"no mixed component -> MOAT"
"mixed component -> SPANNING"

No other visible text. All text must be exact, horizontal, legible, and cleanly typeset. Do not mutate symbols, invent pseudo-text, add tiny unreadable paragraphs, or include random formulas. If a label feels crowded, simplify the drawing and keep the exact label.

Do not show CUDA code, GPUs, terminals, implementation tables, fake logos, QR codes, signatures, watermarks, fantasy decoration, or unsupported theorem claims.
```
