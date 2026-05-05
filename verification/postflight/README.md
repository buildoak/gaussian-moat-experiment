# Post-Flight Checker Skeleton

`src/postflight_check.cpp` is the first independent post-flight spine surface.
It reads a compact JSON bundle and emits a JSON report with one status:

- `REJECT`
- `RUN_CONTRACT_PASS`
- `TILE_SAMPLE_AUDIT_PASS`
- `SPAN_PROOF_PASS`
- `MOAT_PROOF_PASS` (reserved by the plan; not emitted by this slice)
- `CLAIM_PROOF_MISSING`

The checker intentionally does not import campaign or CUDA implementation code.
It uses `verification/include/independent_moat.hpp` only for independent
Gaussian-prime, annulus, K-step, and geo-band checks on inline SPANNING
coordinate certificates.

Current run-contract checks cover row/profile/stdout/run-index agreement when
present, width and full-octant contract, BZ-clean flags, zero overflow counters,
MOAT full-ingest evidence, optional independently enumerated active tile-count
agreement, build/hash identity presence, artifact table shape, telemetry level,
sample-audit metadata, and SPANNING coordinate certificate validity.

CMake wiring needed by the coordinator:

```cmake
add_executable(postflight_check postflight/src/postflight_check.cpp)
target_link_libraries(postflight_check PRIVATE verification_common nlohmann_json::nlohmann_json)
```

The existing `no_campaign_includes` check should also include
`verification/postflight/src/*.cpp` when this target is wired.
