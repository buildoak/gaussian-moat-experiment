"""Spec-faithful union-find and component construction for tile validation."""

from __future__ import annotations

from typing import Iterable


class UnionFind:
    """Union-find with path halving and deterministic smaller-root union."""

    __slots__ = ("parent",)

    def __init__(self, n: int):
        self.parent = list(range(n))

    def find(self, x: int) -> int:
        while self.parent[x] != x:
            self.parent[x] = self.parent[self.parent[x]]
            x = self.parent[x]
        return x

    def union(self, a: int, b: int) -> bool:
        ra = self.find(a)
        rb = self.find(b)
        if ra == rb:
            return False
        if ra > rb:
            ra, rb = rb, ra
        self.parent[rb] = ra
        return True

    def flatten(self) -> list[int]:
        for i in range(len(self.parent)):
            self.parent[i] = self.find(i)
        return self.parent


def make_backward_offsets(k_sq: int) -> list[tuple[int, int]]:
    reach = int(k_sq**0.5)
    offsets: list[tuple[int, int]] = []
    for dr in range(-reach, 1):
        for dc in range(-reach, reach + 1):
            if dr == 0 and dc >= 0:
                continue
            if dr * dr + dc * dc <= k_sq:
                offsets.append((dr, dc))
    offsets.sort()
    return offsets


def bitmap_test(bitmap: list[int], pos: int) -> bool:
    return bool(bitmap[pos >> 5] & (1 << (pos & 31)))


def bitmap_pos_to_compact_index(bitmap: list[int], prefix: list[int], pos: int) -> int:
    word = pos >> 5
    bit = pos & 31
    mask = (1 << bit) - 1
    return prefix[word] + (bitmap[word] & mask).bit_count()


def build_components(
    bitmap: list[int],
    prefix: list[int],
    prime_pos: list[int],
    side_exp: int,
    k_sq: int,
    backward_offsets: Iterable[tuple[int, int]] | None = None,
) -> list[int]:
    """Return flattened root index per compacted prime index."""

    if not prime_pos:
        return []

    offsets = list(backward_offsets) if backward_offsets is not None else make_backward_offsets(k_sq)
    uf = UnionFind(len(prime_pos))

    for i, pos in enumerate(prime_pos):
        row = pos // side_exp
        col = pos % side_exp
        for dr, dc in offsets:
            nr = row + dr
            nc = col + dc
            if nr < 0 or nr >= side_exp or nc < 0 or nc >= side_exp:
                continue
            npos = nr * side_exp + nc
            if bitmap_test(bitmap, npos):
                j = bitmap_pos_to_compact_index(bitmap, prefix, npos)
                uf.union(i, j)

    return uf.flatten()
