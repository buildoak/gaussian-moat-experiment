# Proposal: 9-Bit h1 via Group ID MSB Borrowing

## 1. Empirical Analysis

The coordinate `h1` represents the along-face position of a port's anchor prime. For a tile side of $S = 256$ and a collar depth of $7$, the tile proper spans coordinates $0$ to $256$, and the expanded boundary can reach up to $271$.
Therefore, `h1` natively requires 9 bits to accurately represent values $\ge 256$. While `h1` values $\ge 256$ are restricted to the far edge of the tile boundary, they are structurally guaranteed to occur because the shared boundary sits exactly at coordinate 256.

The 100K-tile census at the extreme $R=860M$ density reveals a crucial structural property: the maximum number of groups observed in a single tile is **21**, and the mean is **9.8**. Because group labels are assigned sequentially starting from 1, the group ID space (currently an 8-bit `u8` allowing 1-255) is vastly underutilized.

## 2. The Proposed Encoding

We fit the exact 9-bit `h1` into the existing 2-byte L/R port footprint by borrowing the unused Most Significant Bit (MSB) of the group ID byte. 

We formally cap the maximum number of groups per tile to **127**. This frees bit 7 (`0x80`) of the group ID byte to act as the 9th bit of `h1`.

**Encode Logic (CUDA/C++):**
If a tile has $> 127$ groups, we explicitly poison the entire TileOp (`0xFF` filled), just as we do for port capacity overflow.

For I and O faces (which do not store `h1`):
- Write the group ID directly: `group_byte = port.group_id` (guaranteed $\le 127$)

For L and R faces (which store `h1`):
- **Group section byte:** `group_byte = port.group_id | ((port.h1 >> 8) << 7)`
- **h1 section byte:** `h1_byte = port.h1 & 0xFF`

**Decode Logic (Rust/Compositor):**
```rust
// Reading an L/R port at index `i`
let g_stored = tileop[off_L_groups + i];
let h1_stored = tileop[off_L_h1 + i];

// Extract 7-bit group ID
let group_id = g_stored & 0x7F;

// Reconstruct 9-bit h1
// If the 0x80 bit is set in g_stored, (g_stored & 0x80) << 1 becomes 0x0100 (256)
let h1 = (h1_stored as u16) | (((g_stored & 0x80) as u16) << 1);
```
For I/O faces, the compositor can safely continue reading the group byte directly, or apply the `& 0x7F` mask for absolute code uniformity.

## 3. Why It Works

*   **Exact Mathematics:** By storing the exact 9-bit `h1` coordinate, we completely abandon the flawed constant-parity assumption (`h1 >> 1`) that broke down across the 0-6 depth collar. 
*   **Topological Guarantee:** Because group labels are allocated sequentially and max out at 21, capping groups at 127 is completely safe. The `0x80` bit is essentially free real estate provided by the tile's internal prime graph topology.
*   **Zero Size Increase:** The L/R ports still consume exactly 2 bytes. The 128-byte TileOp layout is perfectly preserved without eviction.
*   **Local Decoding:** The compositor reconstructs the true `h1` value instantly from the two bytes. It requires absolutely no external state—not even the tile origin or face constant—improving upon the parity-based approach.

## 4. Edge Cases and Failure Modes

*   **Tiles with > 127 groups:** These will overflow the 7-bit group ID space and trigger the `0xFF` poison sentinel. The compositor will correctly fall back to conservative bridging.
*   **h1 > 511:** The 9-bit space supports values up to 511. Since the grid geometry hard-caps $h1$ at $\approx 271$ ($256 + 7$), overflow here is geometrically impossible.
*   **I/O Face Compatibility:** I/O faces only store group IDs, so their bytes will never have the MSB set. The format remains perfectly compatible with the current packed layout.

## 5. Poison/Overflow Rate Estimate

*   **Group Limit Poisoning:** **0.00%**. In the 100K-tile census, the absolute maximum group count was 21. Reaching 128 groups would require a fundamentally different prime density not found in the Gaussian integers at these operating radii.
*   **Payload Budget Poisoning:** Unchanged. This proposal strictly repackages bits within the exact same payload footprint. 

## 6. Comparison to Alternatives Considered

*   **Owner's Wildcard Idea (Top-of-range subtraction):** The owner elegantly suggested mapping high h1 values to the top of the group range (e.g., `255 - g` representing group `g`). Our MSB proposal is the exact mathematical realization of this idea, just formalized using standard bitwise operations (`g | 0x80`) instead of subtraction. Bitwise packing maps perfectly to hardware instructions (1 cycle latency), is standard practice in CUDA/Rust, and enforces the 127 limit as an explicit bit-boundary rather than a collision check.
*   **h1 >> 1 (Current V5 Spec):** Failed for 42.4% of L/R ports. Face primes span multiple perpendicular depths (0-6), meaning parity is simply not constant within a port cluster, breaking reconstruction.
*   **10-bit / 16-bit h1 widening:** Would expand the L/R port footprint from 2 bytes to 3 bytes, severely reducing face capacity and compromising the 128-byte (2 cache line) alignment.
*   **Per-port parity bit:** Storing a parity bit alongside `h1 >> 1` would require finding a free bit anyway. If we must borrow a bit from the group ID, using it to directly store the 9th bit of `h1` is simpler, exact, and bypasses parity math entirely.