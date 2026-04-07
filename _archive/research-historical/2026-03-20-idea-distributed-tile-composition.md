# Distributed Tile Composition for Gaussian Moat Search

*2026-03-20 — Idea stage*

---

## Core Idea

Each tile in the Gaussian moat tile-probe is a **black box**. Its interface is just face ports (~8KB while its internals are millions of primes (~50MB). This asymmetry is the key insight.

- Participants compute individual tiles independently: find primes, build connectivity graph, extract face ports
- They share only the **TileOperator summary**: face ports (a, b, component_id) per face + component-to-face-set mapping
- A coordinator assembles tiles via composition — hierarchical horizontal L↔R then vertical I↔O merging
- The composition only needs face ports, not raw primes — **~6000x data compression**

The tile abstraction already exists in the tile-probe Rust crate. What's missing is everything around it: serialization, network, verification, coordinator logic.

---

## Protocol Sketch

1. **Assignment**: Coordinator divides the Gaussian plane into tiles and distributes work assignments to participants
2. **Computation**: Each participant computes its assigned tile — enumerate Gaussian primes in the rectangle, build the connectivity graph, extract face ports — and returns a TileOperator (face ports + component metadata)
3. **Composition**: Coordinator (or sub-coordinators) compose TileOperators hierarchically: tiles → super-tiles → mega-tiles, using the existing composition rules (horizontal L↔R merge, then vertical I↔O merge)
4. **Detection**: Moat detection runs on the final composed result

No participant ever needs to see another participant's primes. The face port interface is complete.

---

## Composition Properties

These properties are what make distributed composition tractable:

- **Associativity**: Composition order doesn't matter — any tree of merges produces the same result. Sub-coordinators can work independently.
- **Complete interface**: Face ports are a sufficient summary — no internal state leaks across the boundary. The composition proof only needs the ports.
- **Independence**: Tiles have no communication requirements during computation. Pure embarrassingly parallel.
- **Logarithmic bandwidth**: Hierarchical composition means bandwidth grows as O(n log n) of the number of tiles, not O(n * tile_size).

---

## Verification Challenge

Trusting participant-submitted TileOperators is the hard problem. Options in increasing sophistication:

**Redundant computation** — assign 2+ participants per tile, compare outputs. Simple, costs 2x work, catches most errors. The natural baseline.

**Spot-check sampling** — coordinator recomputes a random subset of tiles and compares. Probabilistic, cheaper than full redundancy. Tunable false-negative rate.

**Cryptographic commitment** — participant submits hash of sorted face ports alongside the TileOperator. Lightweight binding; doesn't prove correctness but makes post-hoc audits cheap.

**Zero-knowledge proofs** — speculative, but interesting: a participant proves their tile computation is correct without revealing the internal prime list. "I correctly enumerated all Gaussian primes in this rectangle and computed the connectivity graph" as a ZKP. The statement is well-defined — Gaussian primes are deterministic given the rectangle — so in principle this is achievable. Whether practical ZKP circuits exist for Gaussian prime enumeration + graph connectivity is an open question.

For a research-grade distributed search, redundant computation is probably the right first answer. ZKPs are worth keeping as a long-horizon note.

---

## Why This Is Novel

Domain decomposition and Schur complement techniques are well-established in numerical PDE — you decompose a domain, solve subproblems on subdomains, and couple via interface variables. The tile operator abstraction here is structurally identical to the transfer matrix / Schur complement formalism: face ports are the interface degrees of freedom, composition is the Schur complement assembly.

What appears to be new is applying this to **combinatorial graph connectivity on Gaussian primes**. The tile-probe approach already discovered this structure empirically. The distributed extension follows naturally once you recognize the face ports as a complete Schur complement summary.

The transfer matrix framing also suggests connections to 1D statistical mechanics models (transfer matrices for partition functions), which may be a useful analogy for understanding the moat problem geometrically.

---

## Current Status

**What exists:**
- TileOperator abstraction in tile-probe Rust crate
- Composition logic (horizontal + vertical merge)
- Single-machine tile-probe working

**What's missing:**
- TileOperator serialization format (bincode? flatbuffers? protobuf?)
- Network protocol for assignment distribution and result collection
- Verification layer (at minimum: redundant computation comparison)
- Coordinator logic (tile assignment, hierarchical merge scheduling)
- Bandwidth profiling (confirm the ~6000x compression claim empirically)

---

## Open Questions

- What's the right granularity for tiles in a distributed setting? Current tile sizing is tuned for single-machine L2 cache. Network tile sizing may want to be much larger.
- Can composition be pipelined? As soon as two adjacent tiles are complete, their merge can start — no need to wait for all tiles globally.
- Is there a natural checkpointing structure? If the coordinator stores intermediate super-tile operators, resuming from failure becomes cheap.
- How does the moat detection interact with partial results? Can we detect "definitely no moat at radius k" from partial tile coverage, before the full plane is assembled?
