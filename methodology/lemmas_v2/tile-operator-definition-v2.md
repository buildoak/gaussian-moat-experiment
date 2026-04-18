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

for an abitrary graph G_random that belongs to G_full - we call set of gaussian primes that are verticies of it: set S = V(G_random) **single local connectivity group** if and only if for each u belonging to S - ufs_local returns the same value.

ufs_global definition can be expanded to set of **single local connectivity groups** - result will be set of scalars where each scalar coresponds to the connected component index of ALL elements of each graph as per the UF partition over G_full.

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

**Lemma 2**
I ~ O is equivalent (both ways) to existance of same scalar in ufs_global(P(I)) and ufs_global(P(O)) because ufs_global(P(I)) = ufs_global(I) (since they are both sets - by the definition)
--same 4 points--


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
| **face_I** (inner) | row = 0 | h = col | row ≤ C |
| **face_O** (outer) | row = S | h = col | row ≥ S − C |
| **face_L** (left) | col = 0 | h = row | col ≤ C |
| **face_R** (right) | col = S | h = row | col ≥ S − C |

A **face prime** is a Gaussian prime within perpendicular distance C of a face boundary line. Face primes have no along-face restriction — a prime in a corner of the halo (outside the proper region in both axes) is a face prime of both adjacent faces. This unrestricted extent is necessary: for offset tiles (adjacent towers with different base_y), restricting face primes to the proper region's row or column range would split port sets at the overlap boundary between towers.

The **set of face primes** of a face f is the set of face primes of f — the full strip within perpendicular distance C on both sides of f's boundary line. We will note set of primes as face_I_primes, face_O_primes, etc. 


**Ports** are the result of using process over the set of face primes defined as:
1) We take the set of face primes (face_I_primes, face_O_primes, etc)
2) We apply the ufs_local over them treating set of face primes as G_local so that every prime has a ufs_local scalar corresponding to it. NOTE: G_local is graph built only based on the set of face primes (as defined above)
3) We then group face_I_primes elemnts by the ufs_local - primes with same ufs_local are assembleed into one set
4) We then enumerate each of the sets by the geometrical walk from left to right in case of the face_I_primes and face_O_primes and bottom to top in case of face_R_primes and face_L_primes - assigning ordinal numbers from 1 to N that could be different for each of the faces

Ports(face_I_primes) = [port_1, port_2, ..., port_a] - enumerated set

**Lemma 3:**
as it can be derived from the definition of algorithm - the process is satisfying conditions of **Process P**

**Lemma 4:**
Let's consider two tiles - Tile A and Tile B sharing full faces with each other - Tile A's face_R is Tile B's face_L
Ports(Tile_A_face_R_primes) = Ports(Tile_B_face_L_primes) - by the Ports creation process defenition


### Tile Operator Creation Process
Let T be the arbitrary tile. Let G_tile be the graph contained in a space of the _Tile Halo_ - including both tile interior and collars on all 4 sides. G_tile belongs to the G_full. 

**Tile Operator Data structure** is the result of applying the following procedure:
1) we build UF partition over the G_tile, refulting in ufs_local being defined over G_tile (G_local = G_tile)
2) we create ports enumerated sets for each of the Tile T faces
3) for each 4 faces: for each port in ordered set we asign the ufs_local scalar meaning the within tile connectivity group
4) we emit data struture as 4 ordered sets of ufs_local scalars - where position of the scalar in ordered set corresponds to the port this scalar has been calculated for; TileOp = {face_I_groups, face_O_groups, face_L_groups, face_R_groups}

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

**Tile T_{i,j}.** For integers i, j ∈ Z, tile T_{i,j} has origin (iS, jS); its proper region is [iS, (i+1)S] × [jS, (j+1)S]; its halo is [iS − C, (i+1)S + C] × [jS − C, (j+1)S + C].

**Active tile.** T_{i,j} is active iff its proper region contains at least one lattice point of R.

**Tower at column i.** Tower_i := { j ∈ Z : T_{i,j} is active }. By convexity of R in the y-direction at fixed column-strip (iS ≤ x ≤ (i+1)S), Tower_i is either empty or a contiguous integer interval [j_low(i), j_high(i)] with j_low(i) ≤ j_high(i).

**Tower-based tiling.** The collection of all active tiles, organized by column i ∈ [0, i_max] where i_max is the largest column index with Tower_i non-empty.

**Curvature correction.** As i grows, tower endpoints shift with R's boundary arcs:
- Inner-arc regime (columns where the inner arc is the lower R-boundary within the column-strip): j_low(i) and j_high(i) both decrease as i grows.
- y=x regime (columns where y=x is the lower R-boundary within the column-strip): j_low(i) increases, j_high(i) decreases.
Towers close when the upper arc drops below the lower bound — near x = R_outer / √2.

**Tower-overlap property (geometric assumption).** For every pair of columns i and i+1 both with Tower non-empty:

    [j_low(i), j_high(i)] ∩ [j_low(i+1), j_high(i+1)] ≠ ∅

This is a property of the octant-annulus geometry for reasonable parameter scales — curvature per column bounded by one tile side S. For the project parameters (R ~ 10⁹, S = 256, annulus width ~ 8192), this holds by direct geometric check; we assume it throughout and flag it as a geometric axiom to verify for each concrete (R_inner, R_outer, S) triple.

**Exposed face of an active tile T_{i,j}.** A face f of T_{i,j} is exposed iff the face-neighbor tile across f is not active. Face-neighbors by face:
- face_I (bottom): T_{i, j-1}
- face_O (top): T_{i, j+1}
- face_L (left): T_{i-1, j}
- face_R (right): T_{i+1, j}

**Classification of exposed faces:**
- **outer-exposed** — exposed face whose outside direction points toward higher R² (the outer arc side). Includes face_O of tower-top tiles, and face_R of tiles in column i at rows j > j_high(i+1) (left-tower top-overhangs where column i's top extends above column i+1's).
- **inner-exposed** — exposed face whose outside points toward lower R² (the inner arc side). Includes face_I of tower-bottom tiles, and face_L of tiles in column i+1 at rows j < j_low(i) (right-tower bottom-overhangs where column i+1's bottom extends below column i's, applicable in the y=x regime).
- **side-exposed** — face_L of column-0 tiles (outside has x < 0, outside the octant) and face_R of column-i_max tiles whose outside is across y = x.

Side-exposed faces are excluded from the moat verdict — the octant symmetry-closure argument (outside this doc's scope) handles y-axis and y=x reflections.


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

**Inner boundary prime set I.** I := ⋃ (face-prime sets of inner-exposed faces of all active tiles).

**Outer boundary prime set O.** O := ⋃ (face-prime sets of outer-exposed faces of all active tiles).

We assume throughout that the annulus is thick enough for the inner and outer face-prime strips not to overlap — specifically, R_outer − R_inner > 2C. Under this assumption, I ∩ O = ∅. For project parameters (R_outer − R_inner ~ 8192, C = 6), the margin is enormous.

**Inner port set I_ports.** I_ports := { (T, f, p) : T active, f an inner-exposed face of T, p ∈ 1..N_f }.

**Outer port set O_ports.** O_ports := { (T, f, p) : T active, f an outer-exposed face of T, p ∈ 1..N_f }.


**Lemma 11 — Boundary port sets satisfy Process P.**

I_ports is a Process P of I; O_ports is a Process P of O.

Proof (I case; O case is symmetric). Verify conditions 0 through 5:

0) Each port in I_ports is non-empty — by the port definition (a port is a non-empty ufs_local group of face primes).

1) Every prime in I belongs to some port of I_ports — by the definition of I, any prime in I is a face prime of some inner-exposed face f of some active tile T; by the port construction, this prime is placed into at least one port of f, which is in I_ports.

2) The union of prime sets of all ports in I_ports equals I — ports of inner-exposed faces partition their face-prime sets (up to port-overlap from multi-face membership), whose total union is I by definition.

3) Each port in I_ports is a single local connectivity group — by Lemma 3 applied at the per-tile level.

4*) A single Gaussian prime may belong to multiple ports of I_ports — when it is a face prime of multiple inner-exposed faces of the same tile (corner primes) or of multiple tiles via shared halo strip. This non-disjointness is allowed by Process P.

5*) Not every edge of G_full over I-primes need be preserved at the port level — edges between I-primes handled by within-tile graph of a tile whose inner-exposed face doesn't own one of the endpoints are "lost" in the projection to I_ports. Process P does not require full edge preservation. ∎


### Moat verdict

**Theorem 12 — Moat verdict.**

The following are equivalent:

1) I !~ O over G_full — no Gaussian prime at the inner boundary of the annular octant R is connected to any Gaussian prime at the outer boundary.

2) The set of connected components of G_ports_grid containing any vertex of I_ports is disjoint from the set of connected components containing any vertex of O_ports.

When (1) and (2) hold, we say a **Gaussian moat of width √K exists within the annular octant R**.

Proof:

By Lemma 1: I ~ O over G_full iff ufs_global(I) ∩ ufs_global(O) ≠ ∅.

By Lemma 2 applied with P(I) := I_ports and P(O) := O_ports (valid Process P decompositions by Lemma 11): I ~ O iff ufs_global(I_ports) ∩ ufs_global(O_ports) ≠ ∅ — i.e., at least one port of I_ports and at least one port of O_ports belong to the same connected component of G_full at the prime level.

By Theorem 10, two ports belong to the same prime-level connected component of G_full iff their port identifiers belong to the same connected component of G_ports_grid.

Combining: I ~ O over G_full iff ∃ π_I ∈ I_ports, π_O ∈ O_ports with π_I and π_O in the same connected component of G_ports_grid.

Negating: I !~ O over G_full iff the components containing I_ports vertices and the components containing O_ports vertices are pairwise disjoint as sets of components. ∎


**Pipeline interpretation.** The CUDA pipeline computes G_ports_grid components (per-tile TileOps produced in parallel; cross-tile bridge edges resolved via a union-find pass). The verdict is then a disjointness check between components labeled by I_ports and components labeled by O_ports:
- Disjoint → **moat exists** in R at connectivity resolution K.
- Any shared component → **no moat** — that component contains a prime-level connectivity chain from inner arc to outer arc.


---
