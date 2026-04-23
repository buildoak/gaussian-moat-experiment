## Bug Fix: Lemma 4 Port-Count Mismatch

`campaign_main_cuda` fails at compositor stage with "Lemma 4 port-count mismatch on I/O".

### Problem
- Individual TileOps pass byte-for-byte parity with CPU
- But when compositor validates cross-tile face port counts at shared boundaries, they don't match
- Lemma 4: Adjacent tiles must agree on shared face port counts (tile A's right face count = tile B's left face count)

### Symptoms
- `cuda_vs_cpu_diff --k5` passes (individual tile parity)
- `campaign_main_cuda` fails when compositor checks inter-tile invariants
- Error: "Lemma 4 port-count mismatch on I/O"

### Your task
1. **Investigate** — find why adjacent tiles have mismatched port counts
2. **Root cause** — likely in K5 face encoding or DSU boundary handling
3. **Fix** — ensure CUDA tiles satisfy Lemma 4 like CPU tiles do
4. **Verify** — `campaign_main_cuda` runs without compositor errors

### Key files to investigate
- `src/kernel_face_encode_v2.cu` — K5 face encoding logic
- `src/kernel_face_sort_pack.cu` — face port sorting and packing
- `cpp-campaign-v2/src/compositor.cpp` — where Lemma 4 is checked
- `cpp-campaign-v2/src/tileop.cpp` — CPU face port encoding reference

### Debugging approach
1. Find two adjacent tiles that fail Lemma 4
2. Compare their shared face port counts (CPU vs CUDA)
3. Trace back to what causes the count divergence
4. Check face-strip boundary detection across tile edges

### Likely bug areas
1. Face-strip primes at tile boundary not being encoded consistently
2. DSU component counting differs for boundary primes
3. Port representative selection differs for shared faces
4. Geo-flag handling at boundaries

### Verification gate
- `campaign_main_cuda --r-inner 100 --r-outer 500` runs without Lemma 4 errors
- `cuda_vs_cpu_diff --k5` still passes
- ctest suite still passes

### Deliverable
Commit with the fix. Include explanation of root cause and what was changed.
