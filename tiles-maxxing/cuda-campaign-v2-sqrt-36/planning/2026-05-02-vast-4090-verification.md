# 2026-05-02 Vast 4090 verification

Instance:
- Vast contract: `36017558`
- GPU: `NVIDIA GeForce RTX 4090`
- Driver: `580.126.09`
- CUDA compiler: `12.4.131`
- Realized cost: `$0.3197777778/hr`

Committed code verified:
- `bdb4b0a Fix dispatcher reuse test fixture size`
- Includes streaming CUDA verdict mode, optional snapshots, timing/profile output, streaming CPU compositor, dispatcher reuse safety, and verification scripts.

Build gates:
- CPU `cpp-campaign-v2`: GCC release build, `K_SQ=36`, `108/108` CTest passed.
- CUDA `cuda-campaign-v2-sqrt-36`: nvcc release build, `K_SQ=36`, `sm_89`, `13/13` CTest passed.

Verification gates:
- CPU/CUDA parity probe:
  - `R_outer=80015782`, `--m4 --k5 --verbose --limit 16`: passed.
  - `R_outer=80015790`, `--m4 --k5 --verbose --limit 16`: passed.
- Snapshot smoke:
  - `R_inner=1000`, `R_outer=1600`: CPU and CUDA snapshot SHA-256 matched.
- Tsuchimura external truth:
  - `R_inner=80000000`, `R_outer=80015782`, early exit: `SPANNING`.
  - `R_inner=80000000`, `R_outer=80015782`, `--no-early-exit`: `SPANNING`, all overflow counters zero.
  - `R_inner=80000000`, `R_outer=80015790`, `--no-early-exit`: `MOAT`, all overflow counters zero.

Timing highlights:
- Early SPANNING, chunk `200000`: `42.183s`, produced `1,799,791`, ingested `1,644,744`.
- Full SPANNING, chunk `200000`: `353.998s`, ingested `15,444,921`.
- Full MOAT, chunk `200000`: `353.263s`, ingested `15,452,604`.
- Early SPANNING chunk sweep:
  - `50000`: `39.797s`
  - `200000`: `42.183s`
  - `500000`: `43.330s`

Grid-only scratch benchmark:
- `R_inner=80000000`, `R_outer=80015790`: `221,015` columns, `15,452,604` tiles, `1.385s`, `3.54MB` tower arrays.
- `R_inner=1100000000`, `R_outer=1100015790`: `3,038,394` columns, `212,453,469` tiles, `19.947s`, `48.6MB` tower arrays.

Local raw artifacts:
- Ignored raw logs/profiles were copied to `tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/vast-36017558-2026-05-02/` before history cleanup. They remain in the pre-filter backup, not in pushable git history.
