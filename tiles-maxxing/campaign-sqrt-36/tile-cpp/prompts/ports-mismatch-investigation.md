# Bug Investigation: ports_after mismatch between C++ and Python tile validators

## Your Task
Investigate the port clustering divergence between tile-cpp/ (C++) and tile-validator/ (Python) and determine:
1. Whether the hypothesis below is correct
2. Which implementation is correct per the spec
3. The exact impact on ports_after counts at operating-point tiles

## Hypothesis
The C++ `cluster_face_ports()` in `tile-cpp/src/face_extract.cpp` (around line 131) compares only CONSECUTIVE sorted face primes:
```cpp
// Only checks primes[i] vs primes[i-1]
const int dx = face_primes[i].tile_col - face_primes[i-1].tile_col;
const int dy = face_primes[i].tile_row - face_primes[i-1].tile_row;
if (dx*dx + dy*dy > K_SQ) { /* new port */ }
```

The Python clustering in `tile-validator/ports.py` checks the new prime against ALL primes in the current cluster (reversed scan with h-delta early break). This means Python can keep primes in the same port when they connect to an EARLIER cluster member, even if they are far from the LAST member.

Concrete counterexample on Face I:
- P1(row=0,col=0,h=0), P2(row=6,col=2,h=2), P3(row=0,col=5,h=5)
- P1-P2: dist_sq=40 <= 40 -> same cluster in both
- C++: P2-P3: dist_sq=45 > 40 -> NEW PORT (2 ports total)
- Python: P3 checks P2 (45>40 fail), then P1 (25<=40 pass) -> CONNECTED (1 port total)

C++ would produce MORE ports than Python, leading to different ports_after counts.

## What To Do

### Step 1: Read and compare the clustering algorithms
- Read `tile-cpp/src/face_extract.cpp` - focus on `cluster_face_ports()`
- Read `tile-validator/ports.py` - focus on the clustering function
- Confirm or deny the consecutive-vs-full-cluster difference

### Step 2: Read the spec
- Read `docs/tile_operations.md` - look for port clustering definition
- Read `docs/tile_spec.md` - look for port definition
- Determine: does the spec say "consecutive" or "connected component"?

### Step 3: Construct a proof
If the hypothesis is correct, determine:
- Can this scenario (prime close to earlier cluster member but far from last) actually occur at R~850M?
- How many extra ports does C++ produce vs Python on the 3 operating-point tiles?
  - 45 deg: (601040640, 601040640)
  - 30 deg: (736121088, 424999936)
  - 15 deg: (820888320, 220000000)

If you can run the Python validator, run it on these tiles and count ports per face before and after pruning. The Python environment should be available at the system level or via uv.

### Step 4: Recommend the fix
Which implementation should we adopt? Consider:
- Spec compliance (what the spec actually says)
- Mathematical correctness for the proof pipeline (what produces valid port groupings for composition)
- CUDA friendliness (which is easier to implement in a kernel)

## Key Files
- `tile-cpp/src/face_extract.cpp` - C++ face extraction and port clustering
- `tile-cpp/src/prune.cpp` - C++ pruning
- `tile-validator/ports.py` - Python port clustering
- `tile-validator/tile.py` - Python pipeline orchestrator
- `tile-validator/sample.py` - Python test runner with operating-point coordinates
- `docs/tile_spec.md` - tile geometry spec
- `docs/tile_operations.md` - pipeline phase spec

## Output Format
Write your findings to `docs/supportive/2026-04-09-ports-after-mismatch-investigation.md` with:
1. Confirmed/denied hypothesis
2. Spec analysis (which interpretation is correct)
3. Numerical evidence from operating-point tiles (if obtainable)
4. Recommended fix with rationale
5. CUDA implications

Also print a brief summary (under 40 lines) to stdout.
