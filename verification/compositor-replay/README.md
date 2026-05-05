# Compositor Replay

Reserved for a future independent full TileOp-surface replay verifier.

This is intentionally not an acceptance gate for the lower-K36 first hardening
wave. The current wave uses:

- exact BZ enforcement,
- boundary semantics tests,
- bounded independent global-UF,
- SPANNING coordinate certificate checking once coordinate certificates exist,
- deterministic production tile sampling,
- stats/anatomy reporting.

A full MOAT replay gate should be added here only after the campaign can emit a
stable TileOp surface stream with bridge/port metadata that a separate
compositor can replay without sharing the production compositor code path.
