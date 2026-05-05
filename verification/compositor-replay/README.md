# Compositor Replay

Reserved for optional forensic/debug work over emitted TileOp surfaces.

This is not an official acceptance gate. The current post-flight spine uses:

- exact BZ enforcement,
- boundary semantics tests,
- bounded independent global-UF,
- SPANNING coordinate certificate checking once coordinate certificates exist,
- deterministic production tile sampling,
- first-class telemetry and sweep-row normalization.

MOAT replay is deliberately demoted from the verification spine. Running a
second compositor over emitted TileOps mostly checks the emitted surface, not
TileOp mathematical faithfulness. Future negative-proof work should attack
TileOp faithfulness directly before it is promoted as a claim-proof gate.
