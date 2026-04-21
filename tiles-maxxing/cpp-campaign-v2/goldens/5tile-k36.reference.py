#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["gmpy2>=2.1.5"]
# ///
"""
5-tile golden reference for cpp-campaign-v2 at project scale.

Independent derivation from the math SSoT
(`methodology/lemmas_v2/tile-operator-definition-v-claude.md`) and the
canonical engineering blueprint
(`methodology/lemmas_v2/campaign-blueprint.md`). No C++ source was read
while writing this. The committed snapshot.bin is whatever this script
produces.

Parameters (fixed):

    K_SQ    = 36
    R_inner = 80_000_000
    R_outer = 80_008_192
    offset  = (1, 1)
    S       = 256
    C       = floor_isqrt(36) = 6

Five active tiles chosen to exercise the pipeline:

    T1 = (117187, 289695)  straddles inner arc        (geo_I expected)
    T2 = (117187, 289696)  just above inner arc       (geo_I expected)  -- adj T1 via I/O
    T3 = (117188, 289696)  adjacent right of T2       (geo_I expected)  -- adj T2 via L/R
    T4 = (117187, 289710)  mid-band                   (neither expected)
    T5 = (117187, 289729)  straddles outer arc        (geo_O expected)

Run:

    cd goldens
    uv run 5tile-k36.reference.py

Produces:

    5tile-k36.snapshot.bin       -- the locked golden bytes
    5tile-k36.manifest.json      -- human-readable sidecar

Determinism: same parameters -> same bytes. Every ordering is total; no
Python-hash-order-dependent iteration.
"""

from __future__ import annotations

import dataclasses as dc
import hashlib
import json
import pathlib
import struct
from datetime import datetime, timezone
from typing import Sequence

import gmpy2  # type: ignore[import]


# ============================================================================
# Campaign parameters (project scale)
# ============================================================================

K_SQ = 36
R_INNER = 80_000_000
R_OUTER = 80_008_192
OFFSET_X = 1
OFFSET_Y = 1
S = 256

# Derived: C = floor_isqrt(K_SQ); for K=36 this is exactly 6.
# Use math.isqrt (stdlib, C-implemented, O(log n)). Original linear version
# is kept in comments for reference; it is correct but O(sqrt(n)) and infeasible
# for R >= 80M. math.isqrt produces the same integer result.
#
# Original (linear, kept for provenance):
#     def floor_isqrt(n):
#         r = 0
#         while (r + 1) * (r + 1) <= n: r += 1
#         return r
import math as _math_stdlib

def floor_isqrt(n: int) -> int:
    assert n >= 0
    return _math_stdlib.isqrt(n)

def ceil_isqrt(n: int) -> int:
    f = floor_isqrt(n)
    return f if f * f == n else f + 1

C = floor_isqrt(K_SQ)            # = 6
CEIL_SQRT_K = ceil_isqrt(K_SQ)   # = 6 (K=36 is a square)

# Wire-format budgets (blueprint §5.2)
TILEOP_SIZE = 256
MAX_GROUPS_PER_TILE = 128
MAX_PORTS_PER_TILE = 192

# tile_flags bits
OVERFLOW_BIT = 0x01
EMPTY_BIT = 0x02
TOWER_CLOSING_BIT = 0x04

# Face enum: I=0, O=1, L=2, R=3  (blueprint §5.2)
FACE_I = 0
FACE_O = 1
FACE_L = 2
FACE_R = 3

R_INNER_SQ = R_INNER * R_INNER
R_OUTER_SQ = R_OUTER * R_OUTER

# geo bands (Model A, per task spec):
#   is_inner iff norm_sq in [R_inner^2, (R_inner + ceil_sqrt_k)^2]
#   is_outer iff norm_sq in [(R_outer - ceil_sqrt_k)^2, R_outer^2]
GEO_INNER_UPPER_SQ = (R_INNER + CEIL_SQRT_K) ** 2
GEO_OUTER_LOWER_SQ = (R_OUTER - CEIL_SQRT_K) ** 2


# Chosen tile coordinates (see header). Keep in lex (i,j) order; this is
# also the canonical snapshot payload order.
TILES: list[tuple[int, int]] = [
    (117187, 289695),
    (117187, 289696),
    (117187, 289710),
    (117187, 289729),
    (117188, 289696),
]

# MR witness table SHA-256 pinned by campaign_constants.h.
# (We do not use MR here -- gmpy2.is_prime is deterministic for u64 -- but
# the hash is embedded in the snapshot header as a parity anchor for the
# future CUDA port.)
MR_WITNESS_TABLE_SHA256 = (
    "92b8b0ea7ae8703a3fae4f7a1581dd0d04e041bde4eb1d23621a8f39846e909c"
)


# ============================================================================
# Gaussian primality
# ============================================================================

def is_rational_prime(n: int) -> bool:
    """Deterministic u64 primality via gmpy2."""
    if n < 2:
        return False
    return bool(gmpy2.is_prime(int(n)))


def is_gaussian_prime(a: int, b: int) -> bool:
    """
    Return True iff (a + b*i) is a Gaussian prime.

    Conventions (math SSoT Definitions):
      * (a, 0) with |a| rational prime, |a| ≡ 3 (mod 4)  -> prime
      * (0, b) with |b| rational prime, |b| ≡ 3 (mod 4)  -> prime
      * (a, b) both nonzero: prime iff a^2 + b^2 is a rational prime
        (covers the ‖p‖² = 2 case at (±1, ±1) as well)
    """
    if a == 0 and b == 0:
        return False
    if a == 0:
        q = abs(b)
        return is_rational_prime(q) and (q % 4 == 3)
    if b == 0:
        q = abs(a)
        return is_rational_prime(q) and (q % 4 == 3)
    return is_rational_prime(a * a + b * b)


# ============================================================================
# Octant / annulus / region predicates
# ============================================================================

def in_octant(a: int, b: int) -> bool:
    """R's octant constraint: x >= 0, y >= x."""
    return a >= 0 and b >= a

def in_annulus_sq(norm_sq: int) -> bool:
    """R_inner^2 <= norm_sq <= R_outer^2."""
    return R_INNER_SQ <= norm_sq <= R_OUTER_SQ

def in_region_R(a: int, b: int) -> bool:
    """Membership in R (octant ∩ annulus)."""
    if not in_octant(a, b):
        return False
    return in_annulus_sq(a * a + b * b)


# ============================================================================
# Per-prime geo flags (Model A interval form)
# ============================================================================

def geo_inner_flag(norm_sq: int) -> bool:
    """norm_sq in [R_inner^2, (R_inner+ceil_sqrt_k)^2]."""
    return R_INNER_SQ <= norm_sq <= GEO_INNER_UPPER_SQ

def geo_outer_flag(norm_sq: int) -> bool:
    """norm_sq in [(R_outer-ceil_sqrt_k)^2, R_outer^2]."""
    return GEO_OUTER_LOWER_SQ <= norm_sq <= R_OUTER_SQ


# ============================================================================
# Disjoint-set union (smaller-root-wins tiebreak, blueprint §5.4 / tileop-design §2)
# ============================================================================

class DSU:
    """
    Deterministic disjoint-set union.

    Union rule: after `find(a)` and `find(b)`, keep the smaller root as the
    surviving root. No rank heuristic -- we need exact determinism across
    any insertion order that respects the iteration rules below.
    """

    def __init__(self, n: int) -> None:
        self.parent = list(range(n))

    def find(self, x: int) -> int:
        # Path compression with loop (no recursion; Py recursion limits).
        root = x
        while self.parent[root] != root:
            root = self.parent[root]
        while self.parent[x] != root:
            nxt = self.parent[x]
            self.parent[x] = root
            x = nxt
        return root

    def unite(self, a: int, b: int) -> None:
        ra = self.find(a)
        rb = self.find(b)
        if ra == rb:
            return
        # Smaller-root-wins.
        if ra < rb:
            self.parent[rb] = ra
        else:
            self.parent[ra] = rb


# ============================================================================
# Tile activity test and enumeration
# ============================================================================

def tile_origin(i: int, j: int) -> tuple[int, int]:
    return (OFFSET_X + i * S, OFFSET_Y + j * S)

def tile_proper_lattice_points_in_R(i: int, j: int) -> list[tuple[int, int]]:
    """Return the lattice points of T_{i,j}'s proper region that lie in R.

    Used only for the active-tile sanity check.
    """
    a_lo, b_lo = tile_origin(i, j)
    out: list[tuple[int, int]] = []
    for a in range(a_lo, a_lo + S + 1):
        for b in range(b_lo, b_lo + S + 1):
            if in_region_R(a, b):
                out.append((a, b))
    return out

def is_tile_active(i: int, j: int) -> bool:
    """Active iff proper region contains >= 1 lattice point of R (blueprint §4.2)."""
    a_lo, b_lo = tile_origin(i, j)
    # Short-circuit scan.
    for a in range(a_lo, a_lo + S + 1):
        # Fast filter along x: find the y range that lies in R at this a.
        # R's slice at fixed a (octant): y in [max(a, ceil(sqrt(R_in^2 - a^2))), floor(sqrt(R_out^2 - a^2))]
        a2 = a * a
        if a2 > R_OUTER_SQ:
            continue
        if a < 0:
            continue
        # y_lower
        rem_inner = R_INNER_SQ - a2
        if rem_inner <= 0:
            y_lower = a  # R_inner arc passed; only y >= a constraint active
        else:
            y_lower_from_arc = ceil_isqrt(rem_inner)
            y_lower = max(a, y_lower_from_arc)
        rem_outer = R_OUTER_SQ - a2
        if rem_outer < 0:
            continue
        y_upper = floor_isqrt(rem_outer)
        # Clip to tile's proper y range [b_lo, b_lo + S]
        lo = max(y_lower, b_lo)
        hi = min(y_upper, b_lo + S)
        if lo <= hi:
            return True
    return False


# ============================================================================
# Sieve: enumerate Gaussian primes in a tile's halo, clipped to R
# ============================================================================

@dc.dataclass(frozen=True)
class Prime:
    a: int
    b: int
    norm_sq: int

    def __lt__(self, other: "Prime") -> bool:
        # Lex (a, b) — wire-stable canonical order used throughout.
        if self.a != other.a:
            return self.a < other.a
        return self.b < other.b


def sieve_tile(i: int, j: int) -> list[Prime]:
    """
    Enumerate Gaussian primes in the halo-expanded region of T_{i,j},
    clipped to the canonical octant R (intersection of octant and annulus).

    Halo: [a_lo - C, a_lo + S + C] × [b_lo - C, b_lo + S + C].
    """
    a_lo, b_lo = tile_origin(i, j)
    primes: list[Prime] = []
    for a in range(a_lo - C, a_lo + S + C + 1):
        if a < 0:
            continue
        a2 = a * a
        if a2 > R_OUTER_SQ:
            continue
        for b in range(b_lo - C, b_lo + S + C + 1):
            if b < a:
                continue  # octant: y >= x
            b2 = b * b
            norm_sq = a2 + b2
            if norm_sq < R_INNER_SQ or norm_sq > R_OUTER_SQ:
                continue
            if not is_gaussian_prime(a, b):
                continue
            primes.append(Prime(a=a, b=b, norm_sq=norm_sq))
    # Canonical lex (a, b) sort -- prime_idx == position in this vector
    # is the deterministic key every downstream step depends on.
    primes.sort()
    return primes


# ============================================================================
# TileOp pipeline (per blueprint §5, tileop-design.md §2-§5)
# ============================================================================

@dc.dataclass
class TileOpBytes:
    bytes_: bytes
    # Debug/diagnostic fields
    n: list[int]
    face_groups: list[int]
    inner_labels: list[int]
    outer_labels: list[int]
    tile_flags: int
    n_primes: int
    n_components: int
    n_ports_per_face: list[int]
    has_inner: bool
    has_outer: bool
    is_empty: bool


def _encode_tileop(
    n: list[int],
    face_groups: list[int],
    inner_mask_bits: list[int],
    outer_mask_bits: list[int],
    tile_flags: int,
) -> bytes:
    """Pack the 256 B wire format per blueprint §5.2 / tileop.h static_asserts.

    Offsets (locked):
        0   : n[4]                        (4 B)
        4   : face_groups[192]            (192 B, zero-padded after sum(n))
        196 : inner_flags[16]             (16 B, bit (g-1) for label g=1..128)
        212 : outer_flags[16]             (16 B, same pattern)
        228 : tile_flags                  (1 B)
        229 : reserved[27]                (27 B, zero)
    """
    assert len(n) == 4
    assert all(0 <= x < 256 for x in n)
    total = sum(n)
    assert len(face_groups) == total

    buf = bytearray(256)
    # n[4]
    for k in range(4):
        buf[k] = n[k]
    # face_groups: written contiguously at the prefix-sum offset per face.
    # Callers must pre-linearize face_groups in face order (I, O, L, R).
    for idx, g in enumerate(face_groups):
        assert 0 <= g <= 255, f"group label must fit in byte, got {g}"
        buf[4 + idx] = g
    # Remaining 192 - total bytes in face_groups are already zero (bytearray init).

    # inner_flags: 16 bytes, bit (g-1) is inner for label g in 1..128
    inner_flags = bytearray(16)
    for g in inner_mask_bits:
        assert 1 <= g <= 128
        g0 = g - 1
        inner_flags[g0 >> 3] |= 1 << (g0 & 7)
    buf[196:196 + 16] = inner_flags

    outer_flags = bytearray(16)
    for g in outer_mask_bits:
        assert 1 <= g <= 128
        g0 = g - 1
        outer_flags[g0 >> 3] |= 1 << (g0 & 7)
    buf[212:212 + 16] = outer_flags

    buf[228] = tile_flags & 0xFF
    # buf[229:256] stays zero (reserved[27]).
    return bytes(buf)


def process_tile(i: int, j: int) -> TileOpBytes:
    """
    Full pipeline for one tile:

      1) Sieve Gaussian primes in halo ∩ R, sorted by lex (a, b).
      2) Build G_tile local UF: union primes at squared dist <= K_SQ,
         smaller-root-wins. Iteration: prime_idx ascending, inner j > i.
      3) Per-prime geo flags via Model A interval test.
      4) Dense remap raw DSU root -> 1-based dense label (blueprint §5.3
         and §5.5: labels 1..128).
      5) Per face F in {I, O, L, R}:
          a) Extract face-strip primes via perpendicular-distance-≤C test.
          b) Sub-DSU over face strip using G_full edges; components are ports.
          c) Each port's representative = smallest (h, p⊥) lex member.
          d) Sort ports by (h, p⊥) primary, (p⊥, h) secondary.
          e) face_groups byte = dense label of any member prime.
      6) Pack 256 B.
    """
    a_lo, b_lo = tile_origin(i, j)
    primes = sieve_tile(i, j)
    n_primes = len(primes)

    # ------------------------------------------------------------------
    # Empty tile guard (blueprint §5.2): no primes => EMPTY_BIT.
    # ------------------------------------------------------------------
    if n_primes == 0:
        raw = _encode_tileop(
            n=[0, 0, 0, 0],
            face_groups=[],
            inner_mask_bits=[],
            outer_mask_bits=[],
            tile_flags=EMPTY_BIT,
        )
        return TileOpBytes(
            bytes_=raw,
            n=[0, 0, 0, 0],
            face_groups=[],
            inner_labels=[],
            outer_labels=[],
            tile_flags=EMPTY_BIT,
            n_primes=0,
            n_components=0,
            n_ports_per_face=[0, 0, 0, 0],
            has_inner=False,
            has_outer=False,
            is_empty=True,
        )

    # ------------------------------------------------------------------
    # (2) G_tile local UF, deterministic iteration.
    # ------------------------------------------------------------------
    dsu = DSU(n_primes)
    # Pairwise O(n^2) is acceptable at tile scale (n~ few hundred).
    for ia in range(n_primes):
        pa = primes[ia]
        for ib in range(ia + 1, n_primes):
            pb = primes[ib]
            da = pa.a - pb.a
            # Early axis prune: since primes are lex-sorted by (a,b) and da<=0.
            # |da| <= C is a required condition for edge; past that,
            # later ib's will have even larger a so we can break.
            if -da > C:
                break
            db = pa.b - pb.b
            if abs(db) > C:
                continue
            dist_sq = da * da + db * db
            if dist_sq <= K_SQ:
                dsu.unite(ia, ib)

    # ------------------------------------------------------------------
    # (3) Per-prime geo flags.
    # ------------------------------------------------------------------
    prime_inner = [geo_inner_flag(p.norm_sq) for p in primes]
    prime_outer = [geo_outer_flag(p.norm_sq) for p in primes]

    # ------------------------------------------------------------------
    # (4) Dense remap: raw root -> 1-based dense label.
    #     Iteration order: prime_idx ascending (canonical sieve order).
    # ------------------------------------------------------------------
    raw_root_per_prime = [dsu.find(idx) for idx in range(n_primes)]
    dense_label_of_root: dict[int, int] = {}
    next_label = 0
    overflow_groups = False
    for idx in range(n_primes):
        r = raw_root_per_prime[idx]
        if r not in dense_label_of_root:
            if next_label >= MAX_GROUPS_PER_TILE:
                overflow_groups = True
                break
            dense_label_of_root[r] = next_label + 1  # 1-indexed
            next_label += 1
    max_label = next_label
    n_components = max_label

    dense_label_per_prime: list[int] = []
    if not overflow_groups:
        dense_label_per_prime = [dense_label_of_root[r] for r in raw_root_per_prime]

    # ------------------------------------------------------------------
    # (5) Per-component inner/outer flags (OR across primes in the component).
    # ------------------------------------------------------------------
    inner_by_label: dict[int, bool] = {g: False for g in range(1, max_label + 1)}
    outer_by_label: dict[int, bool] = {g: False for g in range(1, max_label + 1)}
    if not overflow_groups:
        for idx in range(n_primes):
            g = dense_label_per_prime[idx]
            if prime_inner[idx]:
                inner_by_label[g] = True
            if prime_outer[idx]:
                outer_by_label[g] = True

    # ------------------------------------------------------------------
    # (6) Per-face port enumeration.
    # ------------------------------------------------------------------
    # Face-strip depth: perpendicular distance <= C of the face's boundary line.
    # Face coordinate convention:
    #   face_I (row=0):   depth row in [-C, C]   h = col       p_perp = row
    #   face_O (row=S):   depth row in [S-C, S+C] h = col      p_perp = row
    #   face_L (col=0):   depth col in [-C, C]    h = row      p_perp = col
    #   face_R (col=S):   depth col in [S-C, S+C] h = row      p_perp = col
    # Tile-relative: col = a - a_lo, row = b - b_lo.

    def face_strip_indices(face: int) -> list[int]:
        out: list[int] = []
        for idx, p in enumerate(primes):
            col = p.a - a_lo
            row = p.b - b_lo
            if face == FACE_I:
                if -C <= row <= C:
                    out.append(idx)
            elif face == FACE_O:
                if S - C <= row <= S + C:
                    out.append(idx)
            elif face == FACE_L:
                if -C <= col <= C:
                    out.append(idx)
            elif face == FACE_R:
                if S - C <= col <= S + C:
                    out.append(idx)
        return out

    def port_h_pperp(face: int, idx: int) -> tuple[int, int]:
        p = primes[idx]
        col = p.a - a_lo
        row = p.b - b_lo
        if face in (FACE_I, FACE_O):
            return (col, row)  # h=col, p_perp=row
        return (row, col)  # h=row, p_perp=col for L/R

    per_face_ports: list[list[list[int]]] = [[], [], [], []]
    # Each per_face_ports[face] is a list of ports, each port is a list of prime indices.
    for face in (FACE_I, FACE_O, FACE_L, FACE_R):
        strip = face_strip_indices(face)
        if not strip:
            continue
        # Sub-DSU over face-strip primes only (using G_full edges).
        face_dsu = DSU(len(strip))
        for u_k, u_idx in enumerate(strip):
            pu = primes[u_idx]
            for v_k in range(u_k + 1, len(strip)):
                v_idx = strip[v_k]
                pv = primes[v_idx]
                da = pu.a - pv.a
                db = pu.b - pv.b
                if da * da + db * db <= K_SQ:
                    face_dsu.unite(u_k, v_k)
        # Group strip indices by face-DSU root
        groups: dict[int, list[int]] = {}
        for k, p_idx in enumerate(strip):
            r = face_dsu.find(k)
            groups.setdefault(r, []).append(p_idx)
        per_face_ports[face] = list(groups.values())

    # For each port, compute representative (h, p_perp) = lex-min member.
    # Then sort ports by (h, p_perp) primary, (p_perp, h) secondary.
    sorted_ports_per_face: list[list[list[int]]] = [[], [], [], []]
    port_dense_label_per_face: list[list[int]] = [[], [], [], []]
    for face in (FACE_I, FACE_O, FACE_L, FACE_R):
        ports = per_face_ports[face]
        if not ports:
            continue
        rep_list: list[tuple[tuple[int, int], tuple[int, int], list[int]]] = []
        for port in ports:
            # Representative: lex-min (h, p_perp) over the port's primes.
            rep = min(port_h_pperp(face, idx) for idx in port)
            h, pperp = rep
            # Secondary key (p_perp, h) per blueprint §5.4 / BACKLOG B9.
            rep_list.append(((h, pperp), (pperp, h), port))
        rep_list.sort(key=lambda t: (t[0], t[1]))
        for _rep, _sec, port in rep_list:
            sorted_ports_per_face[face].append(port)
        # Each port's face_groups byte = dense label of any member prime
        if not overflow_groups:
            for port in sorted_ports_per_face[face]:
                # All primes in the port share the same G_tile UF component
                # (G_facestrip_f ⊆ G_tile), so they share a dense label.
                lbl = dense_label_per_prime[port[0]]
                port_dense_label_per_face[face].append(lbl)

    # ------------------------------------------------------------------
    # (7) Overflow / pack / emit.
    # ------------------------------------------------------------------
    n_per_face = [len(sorted_ports_per_face[f]) for f in range(4)]
    total_ports = sum(n_per_face)

    overflow = overflow_groups or (total_ports > MAX_PORTS_PER_TILE)

    if overflow:
        raw = _encode_tileop(
            n=[0, 0, 0, 0],
            face_groups=[],
            inner_mask_bits=[],
            outer_mask_bits=[],
            tile_flags=OVERFLOW_BIT,
        )
        return TileOpBytes(
            bytes_=raw,
            n=[0, 0, 0, 0],
            face_groups=[],
            inner_labels=[],
            outer_labels=[],
            tile_flags=OVERFLOW_BIT,
            n_primes=n_primes,
            n_components=max_label,
            n_ports_per_face=n_per_face,
            has_inner=False,
            has_outer=False,
            is_empty=False,
        )

    # Linearize face_groups in face order I, O, L, R (blueprint §5.2).
    linear_face_groups: list[int] = []
    for face in (FACE_I, FACE_O, FACE_L, FACE_R):
        linear_face_groups.extend(port_dense_label_per_face[face])

    inner_labels = [g for g in range(1, max_label + 1) if inner_by_label.get(g, False)]
    outer_labels = [g for g in range(1, max_label + 1) if outer_by_label.get(g, False)]

    tile_flags = 0
    # TOWER_CLOSING_BIT: informational; set iff tower height at column i is <= 1.
    # For our picked tiles, tower height is very much > 1 in the bulk regime.
    # We do NOT set TOWER_CLOSING in the reference for these tiles.

    raw = _encode_tileop(
        n=n_per_face,
        face_groups=linear_face_groups,
        inner_mask_bits=inner_labels,
        outer_mask_bits=outer_labels,
        tile_flags=tile_flags,
    )
    return TileOpBytes(
        bytes_=raw,
        n=n_per_face,
        face_groups=linear_face_groups,
        inner_labels=inner_labels,
        outer_labels=outer_labels,
        tile_flags=tile_flags,
        n_primes=n_primes,
        n_components=max_label,
        n_ports_per_face=n_per_face,
        has_inner=len(inner_labels) > 0,
        has_outer=len(outer_labels) > 0,
        is_empty=False,
    )


# ============================================================================
# Snapshot encoding (per include/campaign/snapshot.h)
# ============================================================================

def sha256_hex(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()

def sha256_bytes(b: bytes) -> bytes:
    return hashlib.sha256(b).digest()


def constants_canonical_string() -> str:
    """
    Canonical logical-field serialization for CampaignConstants.
    Format pinned by plan §3 C10 + execution-plan Q4:

        "K=<K_SQ>;R_inner=<R_i>;R_outer=<R_o>;offset=<ox>,<oy>;collar=<C>"
    """
    return (
        f"K={K_SQ};"
        f"R_inner={R_INNER};"
        f"R_outer={R_OUTER};"
        f"offset={OFFSET_X},{OFFSET_Y};"
        f"collar={C}"
    )


def grid_canonical_string(tile_count: int, tile_coords: Sequence[tuple[int, int]]) -> str:
    """
    Canonical logical-field serialization for Grid.

    The execution plan defines CampaignConstants.canonical_hash() exactly
    (Q4 resolved) but does NOT publish the Grid canonical form. We adopt a
    structurally identical convention: list every load-bearing logical
    field (radii, K, S, C, offset) and then the (i, j) coords of the tiles
    this snapshot covers, in canonical lex order. This is the explicit
    ambiguity surfaced in the README; the C++ implementation is free to
    adopt any canonical form as long as it matches this one byte-for-byte.

    Format:
        "R_inner=<R_i>;R_outer=<R_o>;K=<K>;S=<S>;C=<C>;"
        "offset=<ox>,<oy>;tile_count=<N>;"
        "tiles=<i1>,<j1>|<i2>,<j2>|..."
    """
    tile_str = "|".join(f"{i},{j}" for (i, j) in tile_coords)
    return (
        f"R_inner={R_INNER};"
        f"R_outer={R_OUTER};"
        f"K={K_SQ};"
        f"S={S};"
        f"C={C};"
        f"offset={OFFSET_X},{OFFSET_Y};"
        f"tile_count={tile_count};"
        f"tiles={tile_str}"
    )


def build_snapshot(
    tileops: Sequence[TileOpBytes],
    tile_coords: Sequence[tuple[int, int]],
) -> tuple[bytes, dict]:
    """Assemble the 120 B header + payload snapshot and a manifest dict."""
    tile_count = len(tileops)
    assert tile_count == len(tile_coords)

    grid_hash = sha256_bytes(grid_canonical_string(tile_count, tile_coords).encode("utf-8"))
    consts_hash = sha256_bytes(constants_canonical_string().encode("utf-8"))
    witness_hash = bytes.fromhex(MR_WITNESS_TABLE_SHA256)
    assert len(grid_hash) == 32
    assert len(consts_hash) == 32
    assert len(witness_hash) == 32

    header = bytearray(120)
    # magic[4] = "CMV2"
    header[0:4] = b"CMV2"
    # version uint32 LE = 1
    header[4:8] = struct.pack("<I", 1)
    # grid_params_hash[32]
    header[8:40] = grid_hash
    # constants_hash[32]
    header[40:72] = consts_hash
    # mr_witness_set_sha256[32]
    header[72:104] = witness_hash
    # tile_count uint64 LE
    header[104:112] = struct.pack("<Q", tile_count)
    # bytes_per_tile uint32 LE = 256
    header[112:116] = struct.pack("<I", TILEOP_SIZE)
    # reserved[4] -- zero
    header[116:120] = b"\x00\x00\x00\x00"

    payload = bytearray()
    for op in tileops:
        assert len(op.bytes_) == TILEOP_SIZE
        payload.extend(op.bytes_)

    blob = bytes(header) + bytes(payload)

    manifest = {
        "schema_version": 1,
        "grid_params_hash": grid_hash.hex(),
        "constants_hash": consts_hash.hex(),
        "mr_witness_set_sha256": witness_hash.hex(),
        "tile_count": tile_count,
        "bytes_per_tile": TILEOP_SIZE,
        "k_sq": K_SQ,
        "r_inner": R_INNER,
        "r_outer": R_OUTER,
        "offset": [OFFSET_X, OFFSET_Y],
        "collar": C,
        "tiles": [{"i": i, "j": j} for (i, j) in tile_coords],
        "generated_at": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "snapshot_sha256": sha256_hex(blob),
    }
    return blob, manifest


# ============================================================================
# Sanity diagnostics printed to stderr
# ============================================================================

def log(msg: str) -> None:
    print(msg)


# ============================================================================
# Entry point
# ============================================================================

def main() -> int:
    here = pathlib.Path(__file__).resolve().parent

    # Canonical tile order: lex (i, j). TILES is already sorted above.
    canonical_tiles = sorted(TILES)

    # Sanity: all picks active and non-degenerate.
    log("== 5-tile golden build ==")
    log(f"K_SQ={K_SQ} R_inner={R_INNER} R_outer={R_OUTER} offset=({OFFSET_X},{OFFSET_Y})")
    log(f"C={C} ceil_sqrt_K={CEIL_SQRT_K}")
    log(f"constants_canonical_string = '{constants_canonical_string()}'")
    log(f"constants_hash = {sha256_hex(constants_canonical_string().encode('utf-8'))}")

    tileop_records: list[TileOpBytes] = []

    for (i, j) in canonical_tiles:
        active = is_tile_active(i, j)
        if not active:
            raise RuntimeError(
                f"Chosen tile ({i}, {j}) is NOT active; pick aborted."
            )
        op = process_tile(i, j)
        if op.n_primes < 5:
            log(
                f"  WARNING: tile ({i},{j}) has only {op.n_primes} halo primes "
                "(spec asks for N >= 5)"
            )
        tileop_records.append(op)
        log(
            f"  tile ({i},{j}): N_primes={op.n_primes} N_components={op.n_components} "
            f"ports={op.n_ports_per_face} (sum={sum(op.n_ports_per_face)}) "
            f"has_inner={op.has_inner} has_outer={op.has_outer} "
            f"empty={op.is_empty} tile_flags=0x{op.tile_flags:02x}"
        )

    blob, manifest = build_snapshot(tileop_records, canonical_tiles)

    snap_path = here / "5tile-k36.snapshot.bin"
    man_path = here / "5tile-k36.manifest.json"
    spec_path = here / "5tile-spec.json"

    snap_path.write_bytes(blob)
    man_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")

    spec = {
        "k_sq": K_SQ,
        "r_inner": R_INNER,
        "r_outer": R_OUTER,
        "offset": [OFFSET_X, OFFSET_Y],
        "tiles": [{"i": i, "j": j} for (i, j) in canonical_tiles],
    }
    spec_path.write_text(json.dumps(spec, indent=2, sort_keys=True) + "\n")

    log(f"snapshot bytes: {len(blob)}  sha256: {sha256_hex(blob)}")
    log(f"wrote {snap_path.name}, {man_path.name}, {spec_path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
