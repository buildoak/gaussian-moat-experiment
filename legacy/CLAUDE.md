# CLAUDE.md — gaussian-moat-cuda

Operational reference for AI agents working in this repo. Read before acting.

---

## Long-running compute → tmux, not agent monitors

Heavy jobs (campaigns, probes, chunked sweeps) always launch in tmux on target servers (Jetson, vast.ai). Do not hold an agent alive as a status watcher — it wastes budget and blocks nothing. User pings for status; check on demand. Agents dispatch and exit.

---

## Vast.ai SSH port pinning

Pin the SSH endpoint once at loop start and cache it in runner state. Never re-resolve mid-run. The vast.ai API can return a changed binding mid-run — `ports["22/tcp"]` can disappear, and the fallback chain silently degrades to the wrong port. The validation guard in `vast.py` always prefers the container port. Cache it. Never query again until the next campaign init.

---

## UB probe semantics: "survived" ≠ origin connectivity

All UB probes use **annular sieving**: norm-lo is set near D², not from 0. The solver fictitiously auto-connects all primes at norm ≤ D². This means:

- **"Survived"** only proves: *if* the component reaches D, *then* it passes through D's annulus. It says nothing about organic connectivity from the origin.
- **"Terminated"** IS definitive: R_moat ≤ D.
- **Lower bounds** require `lower_bound_probe` mode — full sieve from norm=0, organic growth, no auto-connect.

Do not interpret a "survived" result as a lower bound. It is not.

---

## ISE = targeted probing, not origin sweep

ISE strips do **not** start from a=0. ISE probes specific radial bands around known moat distances — independent strips at a target radius checking f(r). This is compute-efficient by design: it skips millions of trivial shells.

Validation targets:
- k²=26: probe annulus ~900K–1.1M
- k²=32: probe annulus ~2.8M
- k²=36: probe annulus <80M

Also probe no-moat territories to confirm the probe discriminates correctly before trusting positive results.

---

## k²=2 correctness gate: farthest point, not component size

Component size is mode-dependent (14 / 10 / 8 depending on `--start-distance`). The true invariant is fixed:

```
farthest_point == (11, 4)   at distance sqrt(137)
```

This never changes regardless of mode. Use `farthest_point` as the correctness gate, not component size. If `farthest_point` is wrong, the solver is broken.

---

## Autoresearch mutation loop lessons

- **Include structural gates in mutation prompts.** Codex needs to know the invariants (see above) to preserve them across mutations. State them explicitly — don't assume the agent infers them.
- **Cheapest test first.** Run `verify-k2-upper-bound` (~5s) immediately after build, before any expensive scout runs. If this fails, the build is already broken.
- **Maintain COMBAT_LOG.md in branch loops.** Agents need to see what strategies have been tried and their outcomes. Without a log, agents mode-collapse into the same failed mutations. Write outcomes inline, keep it short.

---

## Jetson: always `--no-overlap`

At large k² scale, overlapping sieve+solver stages exceed 7.4GB and OOM the Jetson (7.4GB VRAM/RAM shared). Always pass `--no-overlap` for Jetson runs. No exceptions.

---

## Sieve is the pipeline bottleneck

Sieve accounts for ~78% of wall time at UB probe scale. GPU launch overhead dominates at narrow bands (<12M norms). At 400M+ norms, sieve throughput reaches ~2.65M primes/sec. Design campaigns and sweep widths accordingly — narrow bands are disproportionately expensive relative to yield.
