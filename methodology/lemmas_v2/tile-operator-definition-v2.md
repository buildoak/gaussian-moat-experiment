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
2) We apply the ufs_local over them treating set of face primes as G_local so that every prime has a ufs_local scalar corresponding to it 
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

**Stitched port graph.**

Tile A and Tile B are placed on the snapped grid sharing a full face — face_R of Tile A coincides with face_L of Tile B. By Lemma 4, Ports(Tile_A_face_R_primes) = Ports(Tile_B_face_L_primes) as ordered sets of N ports.

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









logic: tile ares -- groups of connected ports -- show that it's identical to the cross face conectivity transfer first (UF-wise) --> then for cross tile lofic (via sticjting) (using Process P)






---
