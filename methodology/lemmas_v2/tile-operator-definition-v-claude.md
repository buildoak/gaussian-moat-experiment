# Gaussian Primes COnnectivity Transfer Operator

*Tile Operator* (TileOp) for short.

---

## Motivation

The Gaussian primes graph at radius R contains O(R²/ln R) vertices. Even for an annular ring of width W at radius R — the actual computational target — the scale is formidable: at R = 10⁹, W = 8192, the ring contains ~10¹² Gaussian primes. No single UF pass can hold this graph.

The Tile Operator decomposes this problem. It partitions the region into bounded tiles placed on a _snapepd grid_ (aligned), runs independent local UF on each, compresses each result to its boundary connectivity; Then Tile Operator Connectivity Pipeline composes the compressed results into verdict about connectivity. 

The composition yields the same connectivity verdict as global UF - which we will prove in this document

The specific target: probe an annular ring for **connectivity transfer** from its inner boundary to its outer boundary — determine whether at least one path exists from the inner boundary to the outer boundary where each step connects Gaussian primes within distance √K. If connectivity transfers across the annulus through a chain of tiles — no moat of width √K exists. If no chain connects them — a moat is found.

The operator enables this probe to run in parallel across thousands of GPU threads, each processing one tile independently. Only the composition step is sequential, and it operates on compressed boundary data — not the full graph.


## Definitions

**G_full = (V, E)** is a graph over a bounded region of Z[i].

- **V** = Gaussian primes in the region. A point a + bi ∈ Z[i] is a Gaussian prime iff its norm a² + b² is a rational prime (for non-zero real and imaginary parts), or it is an associate of a rational prime p ≡ 3 (mod 4).
- **E** = {{p, q} : p, q ∈ V, ‖p − q‖² ≤ K} for fixed squared step bound K.

**{I}** **{O}** are two sets of gaussian primes between which we want to prove the absense of connectivity, further noted as **I** and **O**

each element of **I** is gaussian prime in **G_full**; same for **O**

**~** is a notation for _connectivity_ defined over graph **G_full**. **u~v** means that exists at least one path connecting u and v; **u!~v** means that no paths exists between u and v.

We additionally expand the **~** notation for **I** and **O** 

**I~O** means that exist at least one u from I and v from O such as u~v

**I!~O** means that for all u from I and v from O u!~v

we additionally define the UF partition over G_full as {G_1, G_2, ...., G_k} - set subgraphs where each subgraph is a single connected component. For every u and v that are connected exists G_i where they both belong. For each u and v taht are disconencted - single G_i containing them does not exist.

we additionaly define the ufs_global operation (Union Find Search over G_full) that is defined for vertices and for subgraphs S_single of G_full that are single connected element; ufs_global(v) returns connected component index as per the UF partition over G_full. ufs_global(S_single) returns connected component index as well - single index because S_single is defined as the single connectivity component of G_full (not necessarily equal to any of G_i though --> rather exists such G_i that sontains P at a whole)

ufs_global definition can be expanded to set of gaussian primes - ufs_global(I) returns set of scalars - where each scalar coresponds to the connected component index as per the UF partition over G_full

we additionally define ufs_local (Union Find Search over G_local - arbitrary graph that belongs to G_full) operation that takes vertices v belonging to G_local and returns connected component index as per the UF partition over G_local.

For an arbitrary graph G_random that is a subgraph of G_full — i.e., V(G_random) ⊆ V(G_full) and E(G_random) ⊆ E(G_full) — we call the set S := V(G_random) a **single local connectivity group** (with respect to G_random) iff every u ∈ S shares the same ufs_local(·; G_random) value. Equivalently, V(G_random) is a single connected component of G_random.

**Fact — `ufs_global` is single-valued on single local connectivity groups.** For any subgraph G_local ⊆ G_full and any S = V(G_local) that is a single local connectivity group with respect to G_local, all elements of S share one `ufs_global` value.

Proof: if u, v ∈ S, then u and v lie in the same connected component of G_local, so a path from u to v exists in G_local. Since E(G_local) ⊆ E(G_full), the same sequence of edges is a path in G_full, so u and v lie in the same G_full-component — hence ufs_global(u) = ufs_global(v). ∎

Consequently we extend `ufs_global` to single local connectivity groups: ufs_global(S) := the common ufs_global value of any element of S. For a collection {P_1, ..., P_n} of single local connectivity groups, ufs_global({P_1, ..., P_n}) := { ufs_global(P_1), ..., ufs_global(P_n) } — a set of scalars.

We then define **Process P** - P(I) and P(O) for sets I and O as P(I) = {P^I_1, P^I_2, ..., P^I_a} and P(O) = {P^O_1, P^O_2, ...., P^O_b} 
0) each element of P(I) is a non empty set of gaussian primes
1) no gaussian primes are lost - each of the element from I belongs to at least one element of the P(I).
2) set of all verticies of all elements of P(I) is equal to I - there are no additional gaussian primes
3) Each element of the P(I) is single local connectivity group - for each u belonging to P^I_J ufs_local (for G_local such as V(G_local) = P^I_j) returns the same scalar value - which allows ufs_global to be defined over the elements of P(I)
4*) Note: there is no struct requirement for a single gaussian prime to be in a single element of P(I) - there could be several elements of P(I) containing one gaussian prime
5*) Note: there is no strict rule for preserving all edges of the G_I - graph over the elevemts of set I - some of the edges could be lost.
_thesises 1 to 5 apply to both P(I) and P(O)_


**Lemma 1** 
I ~ O is equivalent (both ways) to existance of same scalar in ufs_global(I) and ufs_global(O)
1) I ~ O means exist element n that belongs to both ufs_global(I) and ufs_global(O) 
2) I !~ O means intersection of set ufs_global(I) with ufs_global(O) is empty (no same scalars exist)
3) existance of element n belonning to both ufs_global(I) and ufs_global(O) - non empty intersections - guarantees I~O
4) empty ufs_global(I), ufs_global(O) intersection guarantees I!~O

**Lemma 2.** I ~ O over G_full is equivalent (both ways) to existence of a common scalar in ufs_global(P(I)) and ufs_global(P(O)).

Proof: we show ufs_global(P(I)) = ufs_global(I) as sets of scalars; by symmetry the same holds for O. Combined with Lemma 1, the result follows.

(⊆) Take s ∈ ufs_global(P(I)). Then s = ufs_global(P^I_j) for some j — the common ufs_global value of all elements of P^I_j, well-defined by the Fact (each P^I_j is a single local connectivity group by Process P condition 3). Pick any u ∈ P^I_j; by Process P condition 2, u ∈ I. Hence s = ufs_global(u) ∈ ufs_global(I).

(⊇) Take s ∈ ufs_global(I). Then s = ufs_global(u) for some u ∈ I. By Process P condition 1, u ∈ P^I_j for some j. Since all elements of P^I_j share one ufs_global value (Fact), s = ufs_global(u) = ufs_global(P^I_j) ∈ ufs_global(P(I)).

Hence ufs_global(P(I)) = ufs_global(I), and by symmetry ufs_global(P(O)) = ufs_global(O). Applying Lemma 1:
1) I ~ O iff ufs_global(I) ∩ ufs_global(O) ≠ ∅ iff ufs_global(P(I)) ∩ ufs_global(P(O)) ≠ ∅.
2) I !~ O iff that intersection is empty.
3) A common element of ufs_global(P(I)) and ufs_global(P(O)) guarantees I ~ O.
4) Empty intersection guarantees I !~ O. ∎


Lemma 1 and Lemma 2 in combination mean that by showing that ufs_global(P(I)) has empty intersection with ufs_global(P(O)) we can guarantee that sets I and O are disconnected - in other words that no path exists between any gaussian prime of set I to set O.

We will use this logic in order to show the soundness and completeness of the Tile Operator Connectivity Pipeline.

---
## Tile Operator

### Tile

A tile T is defined by origin (tₓ, tᵧ) ∈ Z² and three parameters:

| Parameter | Symbol | Role |
|-----------|--------|------|
| Side | S | Tile proper spans [tₓ, tₓ + S] × [tᵧ, tᵧ + S] — (S+1)² lattice points |
| Collar | C = ⌊√K⌋ | Extension beyond tile proper on all four sides |
| Halo | — | Full processing domain: tile proper + collar = (S + 1 + 2C)² lattice points |

**Collar sufficiency.** C = ⌊√K⌋ is the tight collar width. For any edge {p, q} ∈ E, the per-axis offset satisfies |Δ| ≤ C: if |Δ| ≥ C + 1, then Δ² ≥ (⌊√K⌋ + 1)² > K, contradicting ‖p − q‖² ≤ K. 

**Edge completeness.** If p lies in any tile's proper region, every neighbor q (with ‖p − q‖² ≤ K) lies within that tile's halo. Both endpoints of every edge survive in at least one tile's local graph.

**Face-to-face connections** - under the snapped grid constraint - two tiles can be conected only via sharing full face with each other.


Each tile has four faces at the edges of its proper region — the interfaces through which tiles communicate.

**Coordinate convention.** Tile-relative coordinates: col = x − tₓ (horizontal), row = y − tᵧ (vertical). Row 0 is the bottom edge (face I), row S is the top edge (face O).

| Face | Boundary | Along-face coord h | Depth predicate (tile-relative) |
|------|----------|--------------------|---------------------------------|
| **face_I** (inner) | row = 0 | h = col | row ∈ [−C, C] |
| **face_O** (outer) | row = S | h = col | row ∈ [S−C, S+C] |
| **face_L** (left) | col = 0 | h = row | col ∈ [−C, C] |
| **face_R** (right) | col = S | h = row | col ∈ [S−C, S+C] |

A **face prime** is a Gaussian prime within perpendicular distance C of a face boundary line. Face primes have no along-face restriction — a prime in a corner of the halo (outside the proper region in both axes) is a face prime of both adjacent faces. This unrestricted extent is necessary: for offset tiles (adjacent towers with different base_y), restricting face primes to the proper region's row or column range would split port sets at the overlap boundary between towers.

The **set of face primes** of a face f is the set of face primes of f — the full strip within perpendicular distance C on both sides of f's boundary line. We will note set of primes as face_I_primes, face_O_primes, etc. 


**Ports.** For each face f ∈ {I, O, L, R} of tile T:

1) Take the set of face primes face_f_primes.

2) Let **G_facestrip_f** be the graph induced from G_full on face_f_primes — V(G_facestrip_f) = face_f_primes, E(G_facestrip_f) = { {p, q} ∈ E(G_full) : p, q ∈ face_f_primes }. Apply ufs_local with G_local = G_facestrip_f. Every prime in face_f_primes receives a ufs_local(·; G_facestrip_f) scalar.

3) Group face_f_primes by this scalar — primes sharing a value form one set.

4) Enumerate the sets by a canonical lexicographic rule:
   - along-face coord h: h = col for face_I, face_O; h = row for face_L, face_R.
   - perpendicular coord p⊥: p⊥ = row for face_I, face_O; p⊥ = col for face_L, face_R.
   Each port's representative is its member with minimum (h, p⊥) under lex order. Sort ports by representative and assign ordinals 1..N_f. Representatives are well-defined because distinct ports hold disjoint primes on a single face, so (h, p⊥) uniquely identifies one prime per port.

Ports(face_f_primes) = [port_1, port_2, ..., port_{N_f}] — enumerated set.

G_facestrip_f and the enumeration rule depend only on the physical face-prime strip and the edges of G_full among those primes — not on which tile T we are inside. This is the property Lemma 4 rests on.

**Lemma 3.** For each face f of tile T, Ports(face_f_primes) is a valid Process P decomposition of face_f_primes (taking face_f_primes in the role of I in the Process P definition).

Proof: verify conditions 0–5 directly against the Ports construction above.

0) Each port is non-empty — ports are ufs_local(·; G_facestrip_f) equivalence classes on face_f_primes, which are non-empty by definition.

1) Every prime in face_f_primes belongs to some port — step 2 assigns a ufs_local scalar to every prime of face_f_primes; step 3 groups all primes by this scalar, so every prime lies in exactly one port.

2) The union of port prime sets equals face_f_primes — by (1) no prime is dropped and no extra primes are introduced.

3) Each port is a single local connectivity group — ports are ufs_local(·; G_facestrip_f) equivalence classes by construction, so every prime in a port shares the port's common ufs_local value. Taking G_random as the induced subgraph of G_facestrip_f on the port's vertices yields a connected G_random with V(G_random) = port, witnessing the property.

4*) Within a single face, ports are in fact disjoint — stronger than Process P condition 4* requires, hence compatible with the condition.

5*) Not all edges of G_full over face_f_primes are preserved — G_facestrip_f drops edges of G_full with one endpoint outside face_f_primes. Process P condition 5* allows this. ∎

**Lemma 4.** Let Tile A and Tile B share a full face — Tile A's face_R coincides with Tile B's face_L. Then
    Ports(Tile_A_face_R_primes) = Ports(Tile_B_face_L_primes)
as ordered sets. (Horizontal face-sharing — e.g., face_O of A coincident with face_I of B — is symmetric.)

Proof: the two face-prime sets are identical — both consist of the Gaussian primes within perpendicular distance C of the shared face boundary line x = (i_A + 1)·S. Consequently:

- V(G_facestrip_{A,R}) = V(G_facestrip_{B,L}) — the shared strip's primes.
- E(G_facestrip_{A,R}) = E(G_facestrip_{B,L}) — edges of G_full with both endpoints in the shared strip, tile-independent.
- The canonical enumeration reads each prime's (h, p⊥) coordinates in the tile-local frame. For the shared vertical face: h (= row) coincides exactly between A and B (both tiles have tᵧ = j_A·S), and p⊥ (= col) differs by a constant shift of S (A-col = x − i_A·S vs. B-col = x − (i_A + 1)·S). Lexicographic ordering under (h, p⊥) is invariant under constant translation of either coordinate, so both tiles produce the same permutation of primes — hence the same ordinal assignment.

Identical input graph plus identical prime ordering yield identical ordered ports. ∎


### Tile Operator Creation Process

Let T be an arbitrary tile. Let **G_tile** be the graph induced from G_full on T's halo — V(G_tile) = V(G_full) ∩ halo(T), E(G_tile) = { {p, q} ∈ E(G_full) : p, q ∈ V(G_tile) }. G_tile ⊆ G_full.

**Tile Operator Data structure** is the result of applying the following procedure:

1) Build the UF partition over G_tile, defining ufs_local(·; G_tile) on every vertex of G_tile.

2) For each face f, construct Ports(face_f_primes) via the Ports process above. Note: the Ports UF runs on G_facestrip_f — a different graph from G_tile. Enumeration is tile-independent.

3) For each face f and each port p ∈ Ports(face_f_primes), assign
       face_f_groups[p] := ufs_local(w; G_tile)   for any w ∈ port_p.
   Well-defined: every port is a single G_facestrip_f connected component; since G_facestrip_f ⊆ G_tile, it is also a single G_tile connected component, so all primes in port_p share a common ufs_local(·; G_tile) value.

4) Emit
       TileOp_T = { face_I_groups, face_O_groups, face_L_groups, face_R_groups },
   four ordered sets where position k in face_f_groups holds the G_tile-UF label of port k on face f.

**Lemma 5:**
u and v are 2 arbitrary gaussian primes from sets of face primes of Tile T -- u belongs to face_A_primes, v belongs to face_B_primes; face_A_primes and face_B_primes could be the same sets. port_u - is a port containing prime u, port_v is a port containing prime v; 

u~v over the G_tile is equivalent (both ways) to ufs_local(port_u) = ufs_local(port_v), where G_local = G_tile

(port_u could be equal to port_v - then it's true by the port definition)


### Stitching operator

**Port graph of a single tile.**

For Tile T with TileOp_T = {face_I_groups, face_O_groups, face_L_groups, face_R_groups}, define the **port graph** G_ports_T:

- V(G_ports_T) := set of ports of Tile T. Each port is denoted (f, p) — f ∈ {I, O, L, R} is the face the port belongs to and p is its ordinal position in the TileOp_T ordered set for that face.
- E(G_ports_T) := { {(f, p), (f', p')} : (f, p) ≠ (f', p'), face_f_groups[p] = face_f'_groups[p'] }.

Two ports are connected in G_ports_T iff they have the same group number in TileOp_T. The connected components of G_ports_T correspond exactly to the distinct group numbers in TileOp_T.

By Lemma 5, {(f, p), (f', p')} ∈ E(G_ports_T) iff port_{f,p} ~ port_{f',p'} over G_tile — port-graph edges encode within-tile connectivity at the port level.

**Corollary of Lemma 5 — a prime's component in G_ports_T is well-defined.**

If a Gaussian prime w is a face prime of Tile T on multiple faces (e.g., a corner prime sitting on both face_R and face_O simultaneously), all ports of Tile T containing w lie in the same connected component of G_ports_T.

Proof: apply Lemma 5 with u = v = w. Since w ~ w over G_tile trivially, any two ports containing w satisfy ufs_local(port_1) = ufs_local(port_2) — same group in TileOp_T — hence these ports share an edge in G_ports_T (by the definition of E(G_ports_T)), and therefore sit in the same connected component. ∎

**Stitched port graph.**

Tile A and Tile B are placed on the snapped grid sharing a full face — face_R of Tile A coincides with face_L of Tile B. Define:

- **G_A** := graph over Tile A's halo — the single-tile graph G_tile from the Tile Operator Creation Process instantiated with T = Tile A. Vertices V(G_A) = V(G_full) ∩ halo(Tile A); edges E(G_A) = edges of E(G_full) with both endpoints in V(G_A).
- **G_B** := graph over Tile B's halo — analogous, with T = Tile B.
- **G_AB** := combined halo graph. Vertices V(G_AB) = V(G_A) ∪ V(G_B); edges E(G_AB) = edges of E(G_full) with both endpoints in V(G_AB).

G_A and G_B both belong to G_full. G_AB belongs to G_full as well and contains both G_A and G_B as subgraphs.

By Lemma 4, Ports(Tile_A_face_R_primes) = Ports(Tile_B_face_L_primes) as ordered sets of N ports.

Port identifiers (f, p) are tile-local, so we tag vertices with their tile of origin: (A, f, p) for ports of Tile A, (B, f, p) for ports of Tile B. The tagged vertex sets are disjoint.

G_ports_AB is the graph (V_AB, E_AB) defined as follows.

V_AB — disjoint union of the two tagged port sets:
    V_AB := { (A, f, p) : (f, p) ∈ V(G_ports_A) } ∪ { (B, f, p) : (f, p) ∈ V(G_ports_B) }

E_AB — three kinds of edges:

1) Within-tile A-edges — the lift of E(G_ports_A) onto the tagged vertices:
    E_A_tagged := { { (A, f, p), (A, f', p') } : {(f, p), (f', p')} ∈ E(G_ports_A) }

2) Within-tile B-edges — symmetric:
    E_B_tagged := { { (B, f, p), (B, f', p') } : {(f, p), (f', p')} ∈ E(G_ports_B) }

3) Bridge edges at the shared face:
    E_bridge := { { (A, R, i), (B, L, i) } : i ∈ 1..N }

   connecting port i on face_R of Tile A to port i on face_L of Tile B — which correspond to the same set of Gaussian primes by Lemma 4.

Then:
    E_AB := E_A_tagged ∪ E_B_tagged ∪ E_bridge

In compact notation (tile tagging implicit):
    G_ports_AB := ( V(G_ports_A) ⊔ V(G_ports_B), E(G_ports_A) ∪ E(G_ports_B) ∪ E_bridge )

**Stitched group of a port** (X, f, p), X ∈ {A, B}, is its connected component in G_ports_AB.

**Stitching.**

    stitch(TileOp_A, TileOp_B) := (TileOp_AS, TileOp_BS)

where each face_f_groups_XS[p] is a canonical label — any fixed injection on connected components of G_ports_AB — of the stitched group of (X, f, p).

Two ports receive the same stitched label iff they are connected in G_ports_AB — linked by a chain alternating within-tile same-group steps and shared-face port identifications.

TileOp_AS and TileOp_BS are the result of applying stitching to initial TileOp_A and TileOp_B.


**Lemma 6 — 2-tile port/prime equivalence**

Tile A and Tile B share a full face — face_R of Tile A coincides with face_L of Tile B. Let u and v be two arbitrary Gaussian primes, each sitting in some port of Tile A or Tile B; let port_u and port_v be their respective ports (tile-tagged: (A, f, p) or (B, f, p) depending on which tile the port belongs to).

u ~ v over G_AB is equivalent (both ways) to port_u and port_v being in the same connected component of G_ports_AB.

Proof:

1) (same-component => u ~ v over G_AB) Given a walk in G_ports_AB from port_u to port_v, every edge of the walk is one of three kinds:
  1a) within-tile A-edge — two A-ports joined by same group number in TileOp_A. By Lemma 5, any prime in one port is connected over G_A to any prime in the other (both ports are single local connectivity groups of the same G_A-group).
  1b) within-tile B-edge — analogous, connectivity over G_B.
  1c) bridge edge — (A, R, k) paired with (B, L, k); by Lemma 4 these two ports refer to the same set of Gaussian primes, so we pick a single representative prime w that belongs to both simultaneously.

Chain representative primes along the walk. Within-port connectivity from u to its chosen representative in port_u (and from a chosen representative in port_v to v) follows from Lemma 5 applied within each port. Every intermediate step is connectivity over G_A or over G_B, both of which are subgraphs of G_AB. Concatenation gives u ~ v over G_AB.

2) (u ~ v over G_AB => same-component) Sub-claim: every edge of G_AB lies in E(G_A) or in E(G_B).

Proof of sub-claim: for edge {p, q} with ‖p − q‖² ≤ K, per-axis offset satisfies |Δ| ≤ C (collar sufficiency). Suppose for contradiction that p ∈ V(G_A) \ V(G_B) and q ∈ V(G_B) \ V(G_A). Under snapped grid with face_R of A = face_L of B at column x = tAx + S, halo(A) has x-range [tAx − C, tAx + S + C] and halo(B) has x-range [tAx + S − C, tAx + 2S + C]. Then p.x ≤ tAx + S − C − 1 and q.x ≥ tAx + S + C + 1, so |p.x − q.x| ≥ 2C + 2 > C — contradiction. Hence both endpoints share at least one halo, and the edge is in E(G_A) or E(G_B). (Symmetric argument for vertical face-sharing.)

Given a path u = x_0, x_1, ..., x_n = v in G_AB, decompose it into maximal segments whose edges all lie in one tile's graph. Boundary vertices between consecutive segments belong to both V(G_A) and V(G_B) — i.e., they sit in the shared strip of width 2C+1 around the shared face, hence they are face primes of both tiles on the shared face (face_R of A and face_L of B simultaneously). By Lemma 4, any such vertex belongs to a port (A, R, k) and to the identical port (B, L, k), which are linked by a bridge edge in G_ports_AB.

Build a walk in G_ports_AB mirroring the path:
- Within an A-segment from x_a to x_b: both x_a and x_b are face primes of A (path endpoints are port primes by assumption on u, v; transition vertices are face primes of both tiles by the argument above). By Lemma 5, the ports of A containing x_a and x_b lie in the same connected component of G_ports_A, which embeds as a subgraph into G_ports_AB.
- Within a B-segment: analogous.
- At each segment transition: traverse the bridge edge between (A, R, k) and (B, L, k) for the boundary vertex.

The walk starts at some port containing u and ends at some port containing v. By the Corollary of Lemma 5, any port containing u lies in the same connected component of G_ports_AB as port_u (and analogously for v), so port_u and port_v are in the same connected component of G_ports_AB.

∎


---

## Tiling and Moat Verdict

### Tower tiling on the annular octant

**Region R (octant-annulus).** Let R_inner² and R_outer² be integers with 0 < R_inner² < R_outer². Define:

    R := { (x, y) ∈ Z² : x ≥ 0, y ≥ x, R_inner² ≤ x² + y² ≤ R_outer² }

From this point on, G_full is the Gaussian-prime graph of §Definitions restricted to V(G_full) = { Gaussian primes in R }. Edges of G_full are edges of the underlying ‖p − q‖² ≤ K connectivity graph with both endpoints in R.

**Inner and outer boundary prime sets (geometric).** The moat-verdict pipeline identifies primes "on the inner side" and "on the outer side" of the annular region — primes whose √K-neighborhood reaches past the corresponding annulus boundary. Define:

    geo_I := { p ∈ V(G_full) : ∃ q ∈ Z² with q² < R_inner² and ‖p − q‖² ≤ K },
    geo_O := { p ∈ V(G_full) : ∃ q ∈ Z² with q² > R_outer² and ‖p − q‖² ≤ K }.

In words: p ∈ geo_I iff p's closed √K-disk extends into the open inner disk; p ∈ geo_O iff p's √K-disk extends past the outer arc. The witness q is a **lattice point**, not required to be a Gaussian prime — it serves purely as a geometric probe confirming that the outside-of-R region is within edge-reach of p. The moat problem is geometric: "near the inner boundary" means "positioned near R_inner," independent of the prime distribution outside R.

Equivalent characterizations:

1) geo_I = { p ∈ V(G_full) : ‖p‖ ≤ R_inner + √K }.
2) geo_O = { p ∈ V(G_full) : ‖p‖ ≥ R_outer − √K }.
3) geo_I primes are exactly the primes that could form the first step of a √K-path coming from outside R on the inner side; symmetric for geo_O.

The continuous-norm characterization (1–2) and the lattice-witness characterization (top) coincide at project-scale parameters, where lattice density relative to √K is high. The lattice-witness form is computationally cleaner — integer arithmetic only, bounded scan over the √K-disk offsets, exact.

**Annulus thickness assumption.** R_outer − R_inner > S√2 + 2√K. This bound serves two purposes:

- geo_I ∩ geo_O = ∅ (requires R_outer − R_inner > 2√K alone).
- No single tile's proper region can host both a geo_I prime and a geo_O prime simultaneously — used by Theorem 12's degenerate-case argument (requires R_outer − R_inner > S√2 + 2√K, tighter).

For project parameters (R_outer − R_inner ~ 8192, S = 256, √K ≤ 7), the threshold is ≈ S√2 + 2√K ≈ 362 + 14 = 376, abundantly satisfied.

**Grid parameters.** Fix:
- S — tile side length, positive integer.
- C = ⌊√K⌋ — collar width (from §Tile).
- offset o = (o_x, o_y) — uniform grid offset applied to every tile origin, with o_x, o_y ∈ Z ∩ [0, S) (both integers in the half-open range [0, S)).

**Tile T_{i,j}.** For (i, j) ∈ Z², tile T_{i,j} has origin (o_x + iS, o_y + jS); its proper region is
    [o_x + iS, o_x + (i+1)S] × [o_y + jS, o_y + (j+1)S],
and its halo is
    [o_x + iS − C, o_x + (i+1)S + C] × [o_y + jS − C, o_y + (j+1)S + C].

**Snapped grid.** Tile origins live on the sub-lattice
    { (o_x + iS, o_y + jS) : (i, j) ∈ Z² } ⊂ Z²
— uniformly spaced with period S in both axes. All lemmas from §Tile depend only on this relative structure; the offset o translates the whole tiling without affecting any within-tile or cross-tile argument.

**Grid face-sharing structure (exhaustive classification).** Under the snapped grid, for distinct tiles T_{i,j} and T_{i',j'}:

1) **Face-adjacent**: (|i − i'|, |j − j'|) ∈ {(1, 0), (0, 1)}. The two tiles share a full face — a common length-S boundary edge of their proper regions. Their halos overlap in a strip of width 2C+1 around the shared face boundary line.

2) **Diagonally-adjacent**: |i − i'| = |j − j'| = 1. The two tiles share exactly one corner lattice point of their proper regions. Their halos overlap in a (2C+1)² corner square around that point. They do **not** share a full face.

3) **Distant**: max(|i − i'|, |j − j'|) ≥ 2. The two tiles share no proper-region points. Their halos are disjoint provided C < S/2 (project params: C = 6, S = 256).

**Only face-adjacent pairs (case 1) share full faces**, and full-face sharing is the sole mechanism by which tiles "connect" in the pipeline — bridge edges in G_ports_AB and G_ports_grid run exclusively between face-adjacent tiles. Diagonal adjacency is handled separately via corner-closure (Lemma 9); distant tile pairs play no role in the compositional structure. **No other tile-pair geometries are admissible under the snapped grid.**

**Active tile.** T_{i,j} is **active** iff its proper region contains at least one lattice point of R.

**Tower at column i.** Tower_i := { j ∈ Z : T_{i,j} is active }.

For each column i with Tower_i non-empty, define:
    j_low(i)  := min Tower_i,
    j_high(i) := max Tower_i,
so j_low(i) ≤ j_high(i).

**Tower-based tiling.** The collection of all active tiles, indexed by (i, j) with i ∈ [i_min, i_max], where
    i_min := min { i : Tower_i ≠ ∅ },   i_max := max { i : Tower_i ≠ ∅ }.
Both may be negative depending on the offset; for canonical o_x = 0 and R ⊂ { x ≥ 0 }, i_min = 0.


### Geometrical notes — three views of the grid

Three complementary views of the same object. Pick whichever makes the current reasoning most natural; they are mutually consistent.

**View 1 — Bricks in a wall.** Each tile is an S×S "brick" with 4 labeled faces (I bottom, O top, L left, R right). Tiles in a single column i stack vertically into a **tower**; towers sit side-by-side across the octant. Two adjacent bricks share a full face when they touch edge-to-edge in the same row (left-right) or the same column (up-down). Natural view for face-sharing and port-bridge arguments.

**View 2 — Column-strip slices of R.** The octant-annulus R is partitioned into vertical column-strips of width S:
    strip_i := { (x, y) ∈ R : o_x + iS ≤ x ≤ o_x + (i+1)S }.
Each strip is a curved 2D region bounded by the outer arc (above), and either the inner arc or the line y = x (below), depending on x's location relative to R_inner/√2. Tower_i is the set of row-indices j whose horizontal row-strip [o_y + jS, o_y + (j+1)S] captures at least one lattice point of strip_i. Natural view for curvature and boundary-slope arguments.

**View 3 — Z² index grid.** Active tiles form a subset of Z² (the (i, j) index plane). Drawing this subset reveals a **"staircase"-shaped region**: the top boundary rises/falls one step per column as j_high(i) shifts; the bottom boundary does the same for j_low(i). The staircase boundary IS the moat-verdict interface — the collection of exposed faces separating active-tile space from its complement. Horizontal boundary segments correspond to outer-exposed face_O (top) or inner-exposed face_I (bottom); vertical boundary segments correspond to outer-exposed face_R or inner-exposed face_L at tower overhangs. Natural view for combinatorial/adjacency arguments.

The same tower is: a stack of bricks in View 1; a curved vertical slice of R in View 2; a column of integer points in Z² in View 3. The same moat verdict is: "no face-to-face path from inner bricks to outer bricks"; "no lattice-path through the curved trapezoid from inner arc to outer arc"; "the staircase's inner and outer boundaries are in disjoint connected components of the port graph."


### Geometric invariants of the tiling

**Cross-section Lemma.** For each x ≥ 0, the slice { y : (x, y) ∈ R } is either empty or a single closed y-interval [y_lower(x), y_upper(x)], where:
    y_lower(x) = max(x, √max(0, R_inner² − x²)),
    y_upper(x) = √max(0, R_outer² − x²).

Proof: at fixed x ≥ 0, R's constraints on y are (a) y ≥ x from the octant cut, (b) y ≥ √(R_inner² − x²) from R_inner² ≤ x² + y² (upper branch, since y ≥ 0 in the octant), (c) y ≤ √(R_outer² − x²) from x² + y² ≤ R_outer². The maximum of lower bounds (a) and (b) against the single upper bound (c) gives a single interval (or empty). ∎

---

**Project-parameter axioms.** The grid parameters (S, C, o, R_inner, R_outer) are required to satisfy:

**(I1) Tower contiguity.** For every non-empty Tower_i:
    Tower_i = { j ∈ Z : j_low(i) ≤ j ≤ j_high(i) }.
(No gaps inside a tower.)

**(I2) Bounded boundary shift.** For every pair of columns i, i+1 both with non-empty towers:
    |j_low(i+1) − j_low(i)| ≤ 1    and    |j_high(i+1) − j_high(i)| ≤ 1.
(Tower endpoints move by at most one tile per column.)

**(I3) Tower-overlap.** For every pair of adjacent non-empty towers:
    [j_low(i), j_high(i)] ∩ [j_low(i+1), j_high(i+1)] ≠ ∅.
(Adjacent towers always share at least one j-position.)

**Proof sketch (valid for project parameters R ~ 10⁹, S = 256, annulus width ~ 8192):**

The octant boundary arcs all have |slope| ≤ 1:
- Outer arc y = √(R_outer² − x²): dy/dx = −x/y. In the octant (y ≥ x ≥ 0), y ≥ x, so |dy/dx| ≤ 1.
- Inner arc y = √(R_inner² − x²): same bound; the octant cut y ≥ x restricts this arc to x ≤ R_inner/√2 where |dy/dx| ≤ 1.
- y = x boundary: slope = +1.

Per column of width S, any boundary y-coordinate changes by at most S — hence (I2).

For (I1): the Cross-section Lemma shows R's slice at fixed x is a single y-interval; as x varies continuously, the interval shifts continuously, making strip_i (= R ∩ column-strip) a connected 2D region. Each row-strip [o_y + jS, o_y + (j+1)S] with j_low(i) ≤ j ≤ j_high(i) intersects strip_i in a 2D region of positive area — containing at least one lattice point of R for S moderately large (S = 256 abundantly sufficient).

For (I3): given (I1) + (I2), the intervals [j_low(i), j_high(i)] and [j_low(i+1), j_high(i+1)] overlap whenever at least one has height ≥ 2. Project-parameter tower heights are ~32 across the bulk of columns, so (I3) holds everywhere except the degenerate **tower-closing regime** near x = R_outer/√2, where tower heights drop to 1 or 0. The closing regime is excluded from the moat verdict and handled by the reflection-closure argument (see §Cross-octant symmetry closure below).

---

**Corollary — Two-tower interface structure.** For every pair of adjacent non-empty towers Tower_i, Tower_{i+1}, exactly one of the following holds:

**(A) Aligned towers.** Tower_i = Tower_{i+1}. Every tile of Tower_i shares its face_R with the face-adjacent tile of Tower_{i+1}, and symmetrically. **Zero** tiles at this interface have face_R (in Tower_i) or face_L (in Tower_{i+1}) exposed.

**(B) Shifted towers.** Tower_i ≠ Tower_{i+1}, and **at most 2** tiles across the pair have face_R (in Tower_i) or face_L (in Tower_{i+1}) exposed at the i→i+1 interface:
- **Top exposure** (at most 1 tile): either T_{i, j_high(i)}'s face_R is exposed (if j_high(i) > j_high(i+1)) or T_{i+1, j_high(i+1)}'s face_L is exposed (if j_high(i+1) > j_high(i)) — not both, since |Δj_high| ≤ 1 by (I2).
- **Bottom exposure** (at most 1 tile): symmetric, at the j_low endpoints.

Case (A) corresponds to Δj_low = Δj_high = 0; case (B) covers everything else. The total exposed count at the interface is |Δj_low| + |Δj_high| ∈ {0, 1, 2}.

This is the operational form of (I1)+(I2)+(I3): walking the grid column-by-column, each transition either keeps the tower identical or introduces at most 2 exposed face_R/face_L tiles, always concentrated at the tower endpoints (top or bottom), never in the interior.


### Exposed faces

**Exposed face of an active tile T_{i,j}.** A face f of T_{i,j} is exposed iff the face-neighbor tile across f is not active. Face-neighbors by face:
- face_I (bottom): T_{i, j-1}
- face_O (top): T_{i, j+1}
- face_L (left): T_{i-1, j}
- face_R (right): T_{i+1, j}

**Classification of exposed faces:**
- **outer-exposed** — exposed face whose outside direction points toward higher R² (the outer arc side). Includes face_O of tower-top tiles, and face_R of tiles in column i at rows j > j_high(i+1) (left-tower top-overhangs where column i's top extends above column i+1's).
- **inner-exposed** — exposed face whose outside points toward lower R² (the inner arc side). Includes face_I of tower-bottom tiles, and face_L of tiles in column i+1 at rows j < j_low(i) (right-tower bottom-overhangs where column i+1's bottom extends below column i's, applicable in the y=x regime).
- **side-exposed** — face_L of column-i_min tiles (outside crosses x = 0 into the second octant) and face_R of column-i_max tiles whose outside crosses y = x into σ_diag(R).

Side-exposed faces are excluded from the moat verdict — handled by the reflection-closure argument in §Cross-octant symmetry closure below.


**Lemma 7 — Tile coverage.**

Every Gaussian prime p ∈ V(G_full) lies in the proper region of at least one active tile.

Proof: let p = (a, b). p ∈ V(G_full) ⇒ p ∈ R. Let T = T_{⌊a/S⌋, ⌊b/S⌋}. Its proper region [⌊a/S⌋·S, (⌊a/S⌋+1)·S] × [⌊b/S⌋·S, (⌊b/S⌋+1)·S] contains (a, b). Hence (a, b) is a lattice point of R in T's proper, so T is active. ∎


**Lemma 8 — Edge coverage.**

Every edge {p, q} ∈ E(G_full) lies in the halo of at least one active tile — i.e., there exists an active tile T with both endpoints in V(G_T), hence {p, q} ∈ E(G_T).

Proof: by Lemma 7, p lies in the proper of some active tile T. By edge completeness (stated in the Tile section): for any p in T's proper and any q with ‖p − q‖² ≤ K, q lies in T's halo. Hence both endpoints of {p, q} lie in V(G_T), so {p, q} ∈ E(G_T). ∎


**Lemma 9 — Corner-closure of the tower tiling.**

Two active tiles P and Q are **diagonally adjacent** if their proper regions share exactly one corner lattice point — i.e., P = T_{i,j} and Q = T_{i+1, j+1} (upward-right) or Q = T_{i+1, j-1} (downward-right).

For any diagonally-adjacent pair of active tiles P, Q, at least one face-neighbor tile R of both P and Q is also active. Moreover halo(R) ⊇ halo(P) ∩ halo(Q) — the (2C+1) × (2C+1) corner square around the shared corner point.

Proof:

Case (a): P = T_{i,j}, Q = T_{i+1, j+1}. Routing candidates:

    R_1 = T_{i+1, j}    face-adjacent to P via P's face_R, to Q via Q's face_I
    R_2 = T_{i, j+1}    face-adjacent to P via P's face_O, to Q via Q's face_L

Suppose for contradiction that both R_1 and R_2 are inactive:
- R_1 inactive with j+1 ∈ Tower_{i+1} (since Q active) forces j ∉ Tower_{i+1} with j+1 being the boundary: j = j_low(i+1) − 1, i.e., j_low(i+1) = j + 1.
- R_2 inactive with j ∈ Tower_i (since P active) forces j+1 ∉ Tower_i with j being the upper boundary: j = j_high(i), i.e., j_high(i) = j.

Combining: j_high(i) = j < j + 1 = j_low(i+1), so [j_low(i), j_high(i)] ∩ [j_low(i+1), j_high(i+1)] = ∅ — contradicting the tower-overlap property.

Case (b): P = T_{i,j}, Q = T_{i+1, j-1}. Symmetric argument with routing candidates:

    R_1 = T_{i+1, j}    face-adjacent to P via P's face_R, to Q via Q's face_O
    R_2 = T_{i, j-1}    face-adjacent to P via P's face_I, to Q via Q's face_L

If both inactive: j_high(i+1) = j − 1 < j = j_low(i), so j_high(i+1) < j_low(i) — again no overlap, contradiction.

Hence at least one routing candidate is active in each case.

Halo containment: halo(P) ∩ halo(Q) is a (2C+1) × (2C+1) square centered on the shared corner point. Any active R from Case (a) or (b) is face-adjacent to both P and Q; halo(R) extends C beyond its proper in every direction, so it covers the corner square shared between halo(P) and halo(Q). ∎


### Grid port graph and N-tile equivalence

**G_ports_grid.** The port graph over the whole tower-based tiling — extension of the pairwise G_ports_AB construction to arbitrarily many active tiles:

    V(G_ports_grid) := ⊔_{T active} V(G_ports_T)

with vertices tagged by host tile: (T, f, p) for each port (f, p) of active tile T.

    E(G_ports_grid) := ( ⋃_{T active} E(G_ports_T) lifted to tagged vertices )
                     ∪ ( ⋃_{T, T' active face-adjacent, shared face f} E_bridge^{T, T', f} )

where each E_bridge^{T, T', f} is the set of N_f bridge edges { { (T, f, k), (T', f', k) } : k ∈ 1..N_f } identifying corresponding ports on the shared face of T and T' (T's face f coincides with T''s face f'). This is exactly the pairwise bridge construction from the Stitching operator section, repeated over all shared-face pairs in the tiling.

**G_grid.** By Lemmas 7 and 8, ⋃_{T active} V(G_T) = V(G_full) and ⋃_{T active} E(G_T) = E(G_full). So the union of tile graphs equals G_full; we write G_grid := G_full interchangeably.


**Theorem 10 — N-tile port/prime equivalence.**

Let u, v be two Gaussian primes in V(G_full), each a port prime of some active tile — i.e., each sits in a port of some active tile. Let port_u, port_v be their respective ports (tile-tagged: (T, f, p)).

u ~ v over G_full is equivalent (both ways) to port_u and port_v being in the same connected component of G_ports_grid.

Proof:

(⇐) Given a walk in G_ports_grid from port_u to port_v. Every edge of the walk is one of two kinds:
  - within-tile edge of G_ports_T for some active T — by Lemma 5, any prime in one endpoint-port is connected over G_T ⊆ G_full to any prime in the other.
  - bridge edge at a shared face — by Lemma 4, the two endpoint-ports refer to the same set of primes; any common representative w witnesses w ~ w trivially.

Chain representative primes across the walk. By Corollary of Lemma 5, any port containing u lies in the same G_ports_T-component as port_u (analogously for v), so extending the walk to start at the chosen representative-port for u and end at the representative-port for v is free. Concatenation yields u ~ v over G_full.

(⇒) Suppose u ~ v over G_full. Then there is a path u = x_0, x_1, ..., x_n = v with every edge {x_k, x_{k+1}} ∈ E(G_full). By Lemma 8, every such edge lies in E(G_T) for at least one active T; assign each edge a home tile T_k.

Decompose the path into maximal runs of consecutive edges sharing a home tile. At each run boundary, the boundary vertex x_m lies in V(G_T) ∩ V(G_{T'}) for adjacent home tiles T and T'. Under the snapped grid, halo(T) ∩ halo(T') is non-empty only when T and T' are equal, face-adjacent, or diagonally-adjacent.

We construct a walk in G_ports_grid mirroring the path. For each run with home tile T and endpoints x_a, x_b: both are face primes of T (run endpoints are either path endpoints u, v — given as port primes — or boundary vertices, which are shown below to be face primes of their home tiles). By Lemma 5, the ports of T containing x_a and x_b lie in the same connected component of G_ports_T ⊆ G_ports_grid, providing a within-G_ports_T walk for the run.

At each run boundary, three sub-cases for the boundary vertex x_m:

  (i) T = T' — not actually a boundary (merge runs).

  (ii) T, T' face-adjacent — x_m lies in the shared strip of width 2C+1 around the shared face of T and T', hence is a face prime of both T and T' on that shared face. By Lemma 4, x_m belongs to corresponding ports (T, f_T, k) and (T', f_{T'}, k) joined by a bridge edge in G_ports_grid. Traverse that bridge edge.

  (iii) T, T' diagonally-adjacent — x_m lies in the (2C+1) × (2C+1) corner square around their shared corner. By Lemma 9 (corner-closure), an active face-neighbor R of both T and T' exists, with halo(R) ⊇ corner square. Hence x_m ∈ V(G_R); moreover, x_m is within perpendicular distance C of two of R's faces — the face R shares with T and the face R shares with T' — so x_m is a face prime of R on both of those faces. x_m therefore belongs to ports of R on two distinct R-faces; by Corollary of Lemma 5, these two R-ports lie in the same connected component of G_ports_R.

  The original path edges adjacent to x_m remain assigned to T and T' respectively; we do not reassign them. Instead, route the transition through R's port structure in three hops:

    - Bridge edge from (T, f_T, k) to (R, f_R, k), where f_T is T's face shared with R and (T, f_T, k), (R, f_R, k) are the corresponding ports containing x_m — exists by Lemma 4 applied to the T–R shared face.
    - Within-G_ports_R walk from (R, f_R, k) to (R, f'_R, k'), where f'_R is R's face shared with T' and (R, f'_R, k') contains x_m — exists by the Corollary above, since both R-ports contain x_m.
    - Bridge edge from (R, f'_R, k') to (T', f_{T'}, k'), the corresponding port of T' on its face shared with R — exists by Lemma 4 applied to the R–T' shared face.

  This handles the diagonal transition without reassigning any original edges.

Concatenating all runs (within-tile walks) and boundary transitions (case (ii) single bridges or case (iii) three-hop routes) yields a walk in G_ports_grid starting at some port containing u and ending at some port containing v. By Corollary of Lemma 5, port_u and port_v are in the same connected component of G_ports_grid. ∎


### Boundary port sets

**Inner boundary prime set.** I := geo_I (defined in §Tower tiling above). **Outer boundary prime set.** O := geo_O.

Under the annulus thickness assumption, I ∩ O = ∅.

The moat verdict asks whether I !~ O in G_full. The tile-based pipeline answers this via a port-graph disjointness check; this section formalizes the port-graph labeling and its soundness/completeness.

---

**Extended TileOp with group flags.** For each active tile T, augment the TileOp produced by the Tile Operator Creation Process with two boolean lookup tables indexed by G_tile UF-label:

    inner_flag_T(g) := ∃ w ∈ V(G_tile) with ufs_local(w; G_tile) = g and w ∈ geo_I,
    outer_flag_T(g) := ∃ w ∈ V(G_tile) with ufs_local(w; G_tile) = g and w ∈ geo_O.

The extended TileOp:

    TileOp_T = { face_I_groups, face_O_groups, face_L_groups, face_R_groups, inner_flags, outer_flags }.

**Computation.** Per active tile T: iterate w ∈ V(G_tile); for each w run the ∃-q lattice scan (bounded by the ~πK lattice offsets in the √K-disk) to test geo_I and geo_O membership; mark the G_tile UF group ufs_local(w; G_tile) with the corresponding flag if positive. Per-tile cost: O(K · |V(G_tile)|). Fully data-parallel across tiles; no cross-tile communication.

**Inner port set.** I_ports := { (T, f, p) : T active, inner_flag_T(face_f_groups[p]) = true }.

**Outer port set.** O_ports := { (T, f, p) : T active, outer_flag_T(face_f_groups[p]) = true }.

A port sits in I_ports iff the G_tile UF component labeled by its group label contains at least one prime of geo_I. This definition does **not** depend on whether the port's face is inner-exposed — geo_I primes deep in a tile's interior are represented through whichever port belongs to their UF component. Symmetric for O_ports.

This is the structural fix for the geometric-vs-tile mismatch: inclusion in I_ports is governed by UF-component membership, not by face-exposure heuristics. Every geo_I prime whose G_full component can leave its host tile contributes a port to I_ports via the Exit lemma below; geo_I primes in isolated-interior components cannot reach geo_O regardless and legitimately do not appear in I_ports.

---

**Lemma 11 — Exit lemma.**

Let u ∈ V(G_T) for some active tile T. If the connected component of G_full containing u contains any vertex outside V(G_T), then this component contains a face prime w of T with w in the same G_T UF group as u.

Proof: let v be a vertex of the component with v ∉ V(G_T), and fix a path u = x_0, x_1, ..., x_n = v in G_full. Let m be the largest index with x_m ∈ V(G_T). Then x_m ∈ V(G_T), x_{m+1} ∉ V(G_T), and {x_m, x_{m+1}} ∈ E(G_full) with ‖x_m − x_{m+1}‖² ≤ K, so per-axis offset |Δ| ≤ C (collar sufficiency). Since x_{m+1} lies outside halo(T) and |Δ| ≤ C, x_m must lie within perpendicular distance C of whichever face boundary x_{m+1} crossed — so x_m is a face prime of T.

Moreover, the sub-path x_0, x_1, ..., x_m stays entirely in V(G_T) by choice of m. Every edge {x_k, x_{k+1}} for 0 ≤ k ≤ m−1 has both endpoints in V(G_T), hence lies in E(G_T). So u = x_0 ~ x_m in G_T — they share a G_T UF group. Take w := x_m. ∎


### Moat verdict

**Theorem 12 — Moat verdict.**

The following are equivalent:

1) geo_I !~ geo_O over G_full — no prime of geo_I is connected in G_full to any prime of geo_O.

2) No connected component of G_ports_grid contains both an I_ports vertex and an O_ports vertex.

When (1) and (2) hold, we say a **Gaussian moat of width √K exists within the annular octant R**.

Proof:

(⇒, contrapositive: (not 2) ⇒ (not 1).) Suppose a connected component D of G_ports_grid contains (T_I, f_I, p_I) ∈ I_ports and (T_O, f_O, p_O) ∈ O_ports.

By Theorem 10, any face prime w_I in port (T_I, f_I, p_I) and any face prime w_O in port (T_O, f_O, p_O) lie in the same connected component of G_full. Pick one w_I and one w_O.

Since (T_I, f_I, p_I) ∈ I_ports, inner_flag_{T_I}(face_f_I_groups_{T_I}[p_I]) = true — so some u ∈ geo_I is in the same G_{T_I} UF component as w_I; hence u ~ w_I in G_{T_I} ⊆ G_full. Symmetrically, some u' ∈ geo_O satisfies u' ~ w_O in G_{T_O} ⊆ G_full.

Chaining: u ~ w_I ~ w_O ~ u' in G_full. So u ~ u' with u ∈ geo_I and u' ∈ geo_O. Hence geo_I ~ geo_O — contradicting (1).

(⇐, contrapositive: (not 1) ⇒ (not 2).) Suppose geo_I ~ geo_O. Fix u ∈ geo_I and u' ∈ geo_O with u ~ u' in G_full. By Lemma 7, u lies in the proper region of a unique active tile T_u; likewise u' in T_{u'}. Let D denote the connected component of G_full containing u and u'. Write g_u := ufs_local(u; G_{T_u}) and g_{u'} := ufs_local(u'; G_{T_{u'}}); note inner_flag_{T_u}(g_u) = true (via u) and outer_flag_{T_{u'}}(g_{u'}) = true (via u').

**Case A: D extends outside V(G_{T_u}) and outside V(G_{T_{u'}}).** By Lemma 11 applied at u in T_u, D contains a face prime w of T_u in u's G_{T_u} UF group g_u. The port (T_u, f_w, p_w) satisfies face_f_w_groups_{T_u}[p_w] = g_u, hence (T_u, f_w, p_w) ∈ I_ports.

Symmetrically, Lemma 11 applied at u' in T_{u'} gives a face prime w' of T_{u'} in g_{u'} with (T_{u'}, f_{w'}, p_{w'}) ∈ O_ports.

Both w and w' lie in D (the same G_full component). By Theorem 10, (T_u, f_w, p_w) and (T_{u'}, f_{w'}, p_{w'}) lie in the same connected component of G_ports_grid — one in I_ports, one in O_ports. Contradicts (2).

**Case B: D ⊆ V(G_T) for some single tile T.** Then u, u' ∈ V(G_T). Since u ~ u' in G_full and both are in V(G_T), they share a G_T UF group — call it g. Then inner_flag_T(g) = true (via u ∈ g) and outer_flag_T(g) = true (via u' ∈ g).

Sub-case B.1: g has at least one port in the port graph — i.e., g labels some (T, f, p). Then this single port is simultaneously in I_ports and O_ports. It is trivially in the same G_ports_grid component as itself, and (2) fails (one component contains both an I_ports vertex and an O_ports vertex).

Sub-case B.2: g has no ports — i.e., g's G_T UF component contains no face primes. Then the component lies entirely in T's proper region interior (no prime within C of any face boundary). Every prime in the component therefore sits in the S×S proper region, so any two primes in it are at most S√2 apart. In particular, ‖u − u'‖ ≤ S√2, giving |‖u‖ − ‖u'‖| ≤ S√2. But u ∈ geo_I and u' ∈ geo_O force ‖u'‖ − ‖u‖ ≥ R_outer − R_inner − 2√K. The annulus thickness assumption R_outer − R_inner > S√2 + 2√K yields ‖u'‖ − ‖u‖ > S√2, contradicting ‖u − u'‖ ≤ S√2.

∎

**Soundness and completeness.** Theorem 12 pins the pipeline's port-graph verdict exactly to the geometric moat verdict on the octant R. No false positives: if the pipeline reports "moat exists," then no geo_I prime connects to any geo_O prime in G_full|_R. No false negatives: if the pipeline reports "no moat" via some shared G_ports_grid component, that component's underlying UF groups carry witnesses u ∈ geo_I and u' ∈ geo_O with a concrete G_full path between them. The tile staircase approximates R's boundaries at cost S, but the UF-flag machinery bridges the gap — geo_I primes in tile interiors are represented through the ports of their UF components (Exit lemma); geo_I primes in isolated-interior components have no path to geo_O and are correctly invisible to the verdict.


### Cross-octant symmetry closure (full-annulus extension)

Theorem 12's verdict is stated on the octant R, using the boundary sets geo_I and geo_O defined in §Tower tiling. The pipeline's actual target is the **full annulus**

    A := { (x, y) ∈ Z² : R_inner² ≤ x² + y² ≤ R_outer² }.

Extend the boundary definitions to A:

    I_A := { p ∈ V(G_full|_A) : ∃ q ∈ Z² with q² < R_inner² and ‖p − q‖² ≤ K },   (full-annulus geo_I)
    O_A := { p ∈ V(G_full|_A) : ∃ q ∈ Z² with q² > R_outer² and ‖p − q‖² ≤ K },   (full-annulus geo_O)
    I_R := I_A ∩ R = geo_I,    O_R := O_A ∩ R = geo_O.

(The R-restricted versions I_R, O_R coincide with the geo_I, geo_O of §Tower tiling by construction — same lattice-witness condition, same underlying prime set restricted to the octant.)

We show that the octant verdict (I_R !~ O_R over G_full|_R) is equivalent to the full-annulus verdict (I_A !~ O_A over G_full|_A) via the D_4 symmetry of the Gaussian-prime structure. This closes the side-exposed faces of R legitimately and extends Theorem 12 to A.

---

**D_4 symmetry of Gaussian primes.** The dihedral group D_4 of order 8 — generated by 90° rotation ρ and diagonal reflection σ_diag — acts on Z² by isometries. For g ∈ D_4:

1) g preserves the norm: ‖g(p)‖ = ‖p‖.
2) g preserves Gaussian-prime status: p is a Gaussian prime iff g(p) is. The 4 rotations correspond to unit multiplication (×1, ×i, ×−1, ×−i) in Z[i]; the 4 reflections correspond to complex conjugation composed with rotations. Both preserve the norm-prime / associate-of-p classification of §Definitions.
3) g preserves edges of G_full|_A: ‖g(p) − g(q)‖ = ‖p − q‖, so {p, q} ∈ E(G_full|_A) iff {g(p), g(q)} ∈ E(G_full|_A).

Hence G_full|_A is D_4-equivariant: for any u, v ∈ A and g ∈ D_4, u ~ v over G_full|_A iff g(u) ~ g(v) over G_full|_A.

**Octant covering.** A = ⋃_{g ∈ D_4} g(R). The 8 octants meet pairwise only on reflection axes. In particular:
- R and σ_diag(R) share the line y = x.
- R and σ_y(R) share the line x = 0 (where σ_y(x, y) := (−x, y)).

I_A and O_A are D_4-invariant: g(I_A) = I_A and g(O_A) = O_A for all g ∈ D_4. Reason: D_4 preserves norm, so ‖g(p)‖ = ‖p‖ — and the norm-form characterizations I_A = { p : ‖p‖ ≤ R_inner + √K } and O_A = { p : ‖p‖ ≥ R_outer − √K } depend only on ‖p‖. Equivalently, the lattice-witness condition (∃ q with q² < R_inner² and ‖p − q‖² ≤ K) is preserved under any isometry of Z² sending q to g(q).

---

**Adjacent Octants Lemma.** Assume R_inner > √(2K). For any edge {u, v} ∈ E(G_full|_A) with u ∈ R: v ∈ R ∪ σ_diag(R) ∪ σ_y(R). Equivalently, every single edge from R reaches at most into R's two axis-adjacent octants.

Proof: u ∈ R has polar angle θ_u ∈ [π/4, π/2]. ‖u − v‖ ≤ √K, so the angular displacement to v satisfies

    |θ_v − θ_u| ≤ arcsin(√K / ‖u‖) ≤ arcsin(√K / R_inner) < π/4

since R_inner > √(2K) gives √K / R_inner < 1/√2 = sin(π/4). Hence θ_v ∈ (θ_u − π/4, θ_u + π/4) ⊂ (0, 3π/4), which is exactly covered by the three octants R (θ ∈ [π/4, π/2]), σ_diag(R) (θ ∈ [0, π/4]), and σ_y(R) (θ ∈ [π/2, 3π/4]). ∎

For project parameters, R_inner ~ 10⁹ vs √(2K) ~ 9.4 — comfortably satisfied.

---

**Monotone Reflection Lemma.** Let σ ∈ {σ_diag, σ_y}. For any u ∈ R and v ∈ σ(R):
    ‖u − σ(v)‖² ≤ ‖u − v‖².

(Reflecting v back into R does not increase its distance to u.)

Proof: direct coordinate computation.

Case σ = σ_diag, σ_diag(x, y) = (y, x). u = (a, b) with b ≥ a ≥ 0 (u ∈ R); v = (c, d) with c ≥ d ≥ 0 (v ∈ σ_diag(R)); σ_diag(v) = (d, c) ∈ R.

    ‖u − v‖² − ‖u − σ_diag(v)‖² = [(a − c)² + (b − d)²] − [(a − d)² + (b − c)²]
                                   = 2(b − a)(c − d)
                                   ≥ 0,
since b − a ≥ 0 and c − d ≥ 0.

Case σ = σ_y, σ_y(x, y) = (−x, y). u = (a, b) ∈ R (a ≥ 0); v = (c, d) ∈ σ_y(R) (c ≤ 0); σ_y(v) = (−c, d) ∈ R.

    ‖u − v‖² − ‖u − σ_y(v)‖² = (a − c)² − (a + c)² = −4ac ≥ 0,
since a ≥ 0 and c ≤ 0. ∎

---

**Theorem 13 — Reflection closure.** Assume R_inner > √(2K). Then
    I_R !~ O_R over G_full|_R    iff    I_A !~ O_A over G_full|_A.

Proof:

(⇐ contrapositive: I_R ~ O_R over G_full|_R implies I_A ~ O_A over G_full|_A.) R ⊂ A, so any path in G_full|_R is a path in G_full|_A. I_R ⊂ I_A and O_R ⊂ O_A, so a path from I_R to O_R within R witnesses I_A ~ O_A over G_full|_A.

(⇒ contrapositive: I_A ~ O_A over G_full|_A implies I_R ~ O_R over G_full|_R.) Let p = (y_0, y_1, ..., y_n) be a path in G_full|_A with y_0 ∈ I_A and y_n ∈ O_A. We construct a folded path x = (x_0, x_1, ..., x_n) in G_full|_R with x_0 ∈ I_R and x_n ∈ O_R.

*Base case.* I_A is D_4-invariant and A = ⋃_g g(R), so there exists g_0 ∈ D_4 with g_0⁻¹(y_0) ∈ I_A ∩ R = I_R. (If y_0 is axis-fixed, multiple g_0's work; fix any.) Set x_0 := g_0⁻¹(y_0) ∈ I_R.

*Inductive step.* Suppose x_{k−1} := g_{k−1}⁻¹(y_{k−1}) ∈ R has been constructed. Apply g_{k−1}⁻¹ to the edge {y_{k−1}, y_k}: both endpoints of the edge g_{k−1}⁻¹({y_{k−1}, y_k}) = {x_{k−1}, g_{k−1}⁻¹(y_k)} lie within distance √K in G_full|_A (D_4 preserves distance). By the Adjacent Octants Lemma applied to x_{k−1} ∈ R and g_{k−1}⁻¹(y_k):

    g_{k−1}⁻¹(y_k) ∈ R ∪ σ_diag(R) ∪ σ_y(R).

Choose g_k according to which region contains g_{k−1}⁻¹(y_k):
- If g_{k−1}⁻¹(y_k) ∈ R: set g_k := g_{k−1}, so x_k = g_{k−1}⁻¹(y_k) ∈ R.
- If g_{k−1}⁻¹(y_k) ∈ σ_diag(R): set g_k := g_{k−1} ∘ σ_diag, so x_k = σ_diag(g_{k−1}⁻¹(y_k)) ∈ R.
- If g_{k−1}⁻¹(y_k) ∈ σ_y(R): set g_k := g_{k−1} ∘ σ_y, so x_k = σ_y(g_{k−1}⁻¹(y_k)) ∈ R.

(Ambiguous cases on axes: pick any valid g_k deterministically.)

*Distance bound on the folded edge.* Let u := x_{k−1} ∈ R and v := g_{k−1}⁻¹(y_k); ‖u − v‖ = ‖y_{k−1} − y_k‖ ≤ √K.
- Case 1 (v ∈ R): x_k = v, so ‖x_k − x_{k−1}‖ = ‖v − u‖ ≤ √K.
- Case 2 (v ∈ σ_diag(R)): x_k = σ_diag(v). By the Monotone Reflection Lemma, ‖u − σ_diag(v)‖² ≤ ‖u − v‖² ≤ K.
- Case 3 (v ∈ σ_y(R)): analogous with σ_y.

In every case, {x_{k−1}, x_k} is an edge of G_full|_R (both endpoints are Gaussian primes in R, at squared-distance ≤ K).

*Endpoint.* x_n = g_n⁻¹(y_n) with y_n ∈ O_A, and O_A is D_4-invariant, so x_n ∈ O_A ∩ R = O_R.

Hence (x_0, ..., x_n) is a path in G_full|_R from I_R to O_R — witnessing I_R ~ O_R. ∎

---

**Corollary — Full-annulus moat verdict.** For project parameters (R_inner ≫ √K), Theorem 12's octant-verdict on R is equivalent to the full-annulus moat verdict on A:

    (moat in R: I_R !~ O_R over G_full|_R)    iff    (moat in A: I_A !~ O_A over G_full|_A).

Side-exposed faces of the tiling (face_L at column i_min across x = 0; face_R at column i_max across y = x) are legitimately excluded from the tile-based moat verdict: by Theorem 13, any A-path through adjacent octants folds back into an R-path, so paths exiting R via side-exposed faces cannot establish connectivity that isn't already expressible within G_full|_R.

The tower-closing regime near x = R_outer/√2 sits exactly where y = x exits the octant at x = y = R_outer/√2. Side-exposed face_R faces at column i_max are the interface the reflection closure handles — tile-closure and symmetry closure happen at the same axis, so no leftover edge cases remain.


**Pipeline interpretation.** The CUDA pipeline computes G_ports_grid components (per-tile TileOps produced in parallel; cross-tile bridge edges resolved via a union-find pass). The verdict is then a disjointness check between components labeled by I_ports and components labeled by O_ports:
- Disjoint → **moat exists** in R at connectivity resolution K.
- Any shared component → **no moat** — that component contains a prime-level connectivity chain from inner arc to outer arc.


---
