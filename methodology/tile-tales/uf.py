"""
uf.py — Union-Find (Disjoint Set Union)

Core data structure for connectivity queries. Used in two contexts:
1. Per-tile: grouping lattice points into connected components.
2. Global compositor: merging tile groups across shared boundaries.

Implementation: union by rank with path compression.
Amortized O(α(n)) per operation where α is the inverse Ackermann function —
effectively constant for any practical input size.
"""

from __future__ import annotations


class UnionFind:
    """Weighted union-find with path compression.

    Elements are integers 0..n-1. Rank-based union keeps trees shallow;
    path compression flattens them on every find. Together they guarantee
    near-constant amortized cost per operation.
    """

    __slots__ = ("parent", "rank", "_n")

    def __init__(self, n: int) -> None:
        """Create n singleton sets {0}, {1}, ..., {n-1}."""
        self.parent: list[int] = list(range(n))
        self.rank: list[int] = [0] * n
        self._n: int = n

    # -- Core operations --------------------------------------------------

    def find(self, x: int) -> int:
        """Return the canonical representative (root) of x's set.

        Path compression: every node visited on the way to the root is
        re-parented directly under the root, collapsing the tree for
        future queries.
        """
        root = x
        while self.parent[root] != root:
            root = self.parent[root]
        # Compress path: point every visited node straight at root.
        while self.parent[x] != root:
            self.parent[x], x = root, self.parent[x]
        return root

    def union(self, a: int, b: int) -> bool:
        """Merge the sets containing a and b. Returns True if they were
        previously disjoint (i.e., a real merge happened).

        Union by rank: the shorter tree is grafted under the taller one,
        keeping overall depth logarithmic.
        """
        ra, rb = self.find(a), self.find(b)
        if ra == rb:
            return False
        # Attach the shorter tree under the taller one.
        if self.rank[ra] < self.rank[rb]:
            ra, rb = rb, ra
        self.parent[rb] = ra
        if self.rank[ra] == self.rank[rb]:
            self.rank[ra] += 1
        return True

    def connected(self, a: int, b: int) -> bool:
        """Are a and b in the same set?"""
        return self.find(a) == self.find(b)

    # -- Diagnostics ------------------------------------------------------

    def component_count(self) -> int:
        """Number of distinct connected components."""
        return len({self.find(i) for i in range(self._n)})

    def component_map(self) -> dict[int, list[int]]:
        """Map from root representative → list of member elements."""
        groups: dict[int, list[int]] = {}
        for i in range(self._n):
            root = self.find(i)
            groups.setdefault(root, []).append(i)
        return groups

    def __len__(self) -> int:
        return self._n

    def __repr__(self) -> str:
        return f"UnionFind(n={self._n}, components={self.component_count()})"
