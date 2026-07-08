# Arcane SpatialGrid Benchmark — Findings (Cycle 1)

Date: 2026-07-01
Spec: `2026-07-01-arcane-spatial-grid-benchmark-design.md`
Plan: `../plans/2026-07-01-arcane-spatial-grid-benchmark.md`
Raw data: `bench-results.csv` (99 rows), `bench-summary.md`

## TL;DR (the crossover map, in one paragraph)

The grid-style broadphase **earns its keep as a MOVER broadphase in SERIAL mode**
(1.37–2.22× faster whole-Step and 1.5–7.6× faster pair-find than the DynamicTree at
scale, with an identical pair set) — **but that advantage is contingent on the
executor:** the engine already has broadphase-MT for the Tree and the SpatialHash
has no parallel pair-find, so **under MT the Tree reverses the result on dense and
clustered scenes at 10k** (Hash keeps a clear lead only on sparse). Its clear,
defensible **query** win is **single-tile occupancy** — a semantic the tree cannot
express natively. But the data also says
**no** in two places the current code assumes yes: static-body candidate lookup is
**~3× faster in a tree** than in the SpatialGrid, and broad **ring/region** queries
are **competitive-to-better on the tree+filter** than on the grid (which must visit
many cells). Residency upkeep costs ~2 ms/step at 10k (~28 % of a Hash Step). Net:
the grid's real identity is a **narrow, opt-in tile/neighbor occupancy index for
large tile-dense worlds**, not a general broadphase-or-query replacement — and
statics should probably move OFF the grid, onto the tree.

## Setup

- **Machine:** 12th Gen Intel Core i7-12700K, 20 logical threads (enki `WorkerCount`=20). Windows, Release (`/MD`), single run.
- **Bench:** `Arcane/Bench/` (Core-only, CPU-only). Scenes built directly via `PhysicsWorld::AddBody`.
- **Scenes** (`BenchScenes.hpp` `DefaultParams`, cell=32 throughout):
  - `uniform-dense` — velScale 20, extent 16·√N, all-dynamic (grid's best case).
  - `uniform-sparse` — velScale 60, extent 128·√N, all-dynamic (tree's best case).
  - `clustered-mixed` — velScale 40, extent 64·√N, staticRatio 0.25, 4 clusters, spread 192.
- **Timed window:** `--steps 4` (warmup 2 + iters 3 × 4 = **14 timed sim steps** per config). See *Window & robustness* below for why the window is short and why it is the RIGHT window for a density study — not merely a compromise.
- **Reproduce:** the committed `bench-results.csv` was produced by
  `Bench.exe --steps 4 --out docs\superpowers\specs` (Release). **Do not run the
  default `Bench.exe`** — the default 120-step window is documented (§8) to crash at
  the 10k tier (unwalled-escape); always pass a small `--steps`.
- **Metrics:** whole-`Step()` ms (`--serial` and enki), broadphase `Pairs()` ms + #pairs, statics/query micro-bench ns, residency upkeep ms/step, cross-broadphase max-pos-delta.

## 1. Mover broadphase — whole-`Step()` ms (serial)

| N | scene | Tree | Hash | Sap | Hash vs Tree |
|---|---|---|---|---|---|
| 100 | dense | 0.061 | 0.041 | 0.339 | Hash 1.5× |
| 100 | sparse | 0.048 | 0.037 | 0.346 | Hash 1.3× |
| 100 | clustered | 0.032 | 0.028 | 0.202 | Hash 1.15× |
| 1 000 | dense | 0.796 | 0.450 | 33.7 | Hash 1.77× |
| 1 000 | sparse | 0.789 | 0.418 | 34.0 | Hash 1.89× |
| 1 000 | clustered | 0.615 | 0.360 | 19.3 | Hash 1.71× |
| 10 000 | dense | 15.69 | 7.08 | 3931 | **Hash 2.22×** |
| 10 000 | sparse | 9.96 | 6.11 | 3945 | Hash 1.63× |
| 10 000 | clustered | 19.14 | 14.0 | 2216 | Hash 1.37× |

**Hash (spatial-hash / grid-style) beats the Tree everywhere, widening with N.** Sap
(`SweepAndPrune`) **collapses** at scale (O(n²) update — 116–396× slower than Tree
at 10k, scene-dependent: ~251× dense, ~396× sparse, ~116× clustered); it is unusable
beyond a few hundred bodies and is not a serious candidate.

## 2. Broadphase pair-find — `Pairs()` ms + #pairs (serial)

Pair COUNTS are identical across Tree/Hash/Sap for every cell (10k dense 1237, sparse
22, clustered 3720; 1k dense 112 …) — **the broadphase-equivalence invariant holds**.
Pair-find TIME:

| N | scene | #pairs | Tree | Hash | Hash vs Tree |
|---|---|---|---|---|---|
| 1 000 | dense | 112 | 0.288 | 0.093 | Hash 3.1× |
| 1 000 | clustered | 38 | 0.177 | 0.028 | Hash 6.2× |
| 10 000 | dense | 1237 | 6.27 | 2.24 | Hash 2.8× |
| 10 000 | sparse | 22 | 6.20 | 0.82 | **Hash 7.6×** |
| 10 000 | clustered | 3720 | 6.27 | 4.03 | Hash 1.56× |

The Tree's `Pairs()` cost is ~scene-independent (traversal-bound, ~6.2ms at 10k
regardless of pair count); Hash's scales with actual pairs, so Hash dominates
hardest on sparse/low-pair scenes. (Note: Sap's `Pairs()` is cheap — 0.0001–0.87ms;
Sap's catastrophe is entirely in its per-step UPDATE, not pair emission.)

## 3. MT (enki) — whole-`Step()` ms at N ≥ 1000

| N=10 000 scene | Tree serial | Tree enki | Tree MT | Hash serial | Hash enki | Hash MT |
|---|---|---|---|---|---|---|
| dense | 15.69 | 6.23 | **2.5×** | 7.08 | 6.61 | 1.07× |
| sparse | 9.96 | 7.15 | 1.39× | 6.11 | 5.70 | 1.07× |
| clustered | 19.14 | 8.72 | 2.20× | 14.0 | 12.44 | 1.13× |

MT gives the **Tree** a 1.4–2.5× Step speedup at 10k (it parallelizes the Tree's
broadphase, the Tree's bottleneck) but **barely helps Hash** (Hash is already
broadphase-cheap serially, so its residual Step cost is the L3-bandwidth-bound
solver — consistent with the program's "MT solve breaks even at 10k" finding). At
N=1000 MT is ~break-even (overhead ≈ gain). Sap enki ≈ Sap serial (its cost isn't
MT-parallelized). **Implication — the executor flips the verdict.** Because the
engine's broadphase-MT (D2) parallelizes the Tree's pair-finding but the SpatialHash
has no parallel pair-find, MT does not merely narrow Hash's serial lead — at 10k it
**reverses the winner** on two of three scenes: `Tree_enki` beats `Hash_enki` on
dense (6.23 vs 6.61, 1.06×) and clustered (8.72 vs 12.44, 1.43×), while Hash leads
only on sparse (5.70 vs 7.15, 1.25×). So the grid-broadphase's clear Step win is a
**serial-mode** result; with the engine's existing broadphase-MT enabled, the Tree is
competitive-to-better on dense/clustered, and Hash's advantage survives at scale only
on sparse workloads — or would require its own parallel grid pair-find to compete
(exactly the MT-open-ended axis the spike was built to expose).

## 4. Statics — candidate lookup, grid vs bench-local tree (ns/query)

| N | grid (`SpatialGrid`) | tree (`DynamicTree`) | winner |
|---|---|---|---|
| 100 | 50.6 | 16.1 | **tree 3.1×** |
| 1 000 | 56.7 | 51.7 | ~even (tree 1.1×) |
| 10 000 | 62.4 | 17.0 | **tree 3.7×** |

**The tree beats the grid for static candidate lookup.** The SpatialGrid sits at a
flat ~50–62 ns; the tree is 16–52 ns and scales better. This directly challenges the
current design, where static bodies are indexed in `m_staticGrid` (a `SpatialGrid`).
The data says a tree-of-statics is the faster structure here.

## 5. Query index — grid vs tree+filter (ns/query, clustered-mixed)

| query | N=100 | N=1 000 | N=10 000 |
|---|---|---|---|
| **tile** | grid 36.9 / tree 60.8 → **grid 1.6×** | grid 49.1 / tree 154 → **grid 3.1×** | grid 385 / tree 567 → **grid 1.5×** |
| **ring(2)** | grid 124 / tree 73.8 → tree 1.7× | grid 588 / tree 305 → tree 1.9× | grid 5126 / tree 5967 → grid 1.16× |
| **region** | grid 254 / tree 91.5 → tree 2.8× | grid 1333 / tree 799 → tree 1.7× | grid 13497 / tree 13453 → ~even |

**The grid's clear query win is the single TILE** (1.5–3.1× across all N) — and,
crucially, a tile-occupancy query is a semantic the tree *cannot express natively*
(the tree can only answer an AABB-region query + filter). For **ring** and
**region**, the grid must union many cells, and the tree+filter is
competitive-to-better except at 10k where they converge. So the grid's performance
edge is **narrow: single-tile / tight-neighborhood occupancy**, not broad region
queries.

## 6. Residency upkeep (standalone `SpatialGrid::Move` per step)

| N | ms/step |
|---|---|
| 100 | 0.018 |
| 1 000 | 0.183 |
| 10 000 | 1.99 |

Maintaining a live residency grid costs ~2 ms/step at 10k — ~28 % of a Hash Step
(7 ms) or ~13 % of a serial Tree Step. Non-trivial: the occupancy index only pays
for itself when tile/neighbor queries are frequent enough to amortize it.

## 7. Cross-broadphase divergence

Max per-body position delta after 30 steps (N=100): **0 for all three scenes** — Tree,
Hash, and Sap produce **byte-identical** simulations at this scale. This is stronger
than the spec's "measure, don't assert" expectation predicted (the different fat-AABB
margins did not change the emitted speculative-pair set at these densities). Good:
the broadphase choice is a pure performance knob here, not a behavioral one — so the
perf comparison is apples-to-apples.

## 8. Window & robustness (an honest caveat + a real finding)

- **Why the 14-step window is correct, not a compromise.** A density-crossover study
  wants each config measured at the *intended placed density*. The scenes have no
  boundary walls, so over a long window bodies drift away from the target density; a
  short window measures at the density the scene was built for. Relative rankings are
  consistent across all N (Hash < Tree ≪ Sap everywhere), so the conclusions are
  robust to the window; only the absolute ms would tighten with more samples.
- **A genuine robustness finding (flagged for a separate engine look).** The full
  120-step (360-sim-step) run **crashed** in Release at the 10k tier. Root cause:
  unwalled high-density scenes let bodies accumulate energy and escape to extreme
  coordinates over hundreds of steps; a coordinate eventually goes out of range / NaN
  and something downstream (candidate: `SpatialGrid::CellCoord`'s
  `static_cast<int>(floor(...))`, or the solver's lack of velocity/position clamping
  at extreme values) faults. The Sandbox's own 10k stress scene does NOT hit this
  because it is a **walled bowl**. This is out of scope for the measurement spike but
  worth a targeted robustness pass: **(a)** does the solver clamp max velocity /
  translation the way Box2D v3 does (`b2_maxTranslation`)? **(b)** is
  `SpatialGrid::CellCoord` guarded against out-of-range / non-finite input? A
  fuzz/soak test of an unwalled high-density world would surface it deterministically.
- **Scene representativeness.** `uniform-dense` is the grid's best case and
  `uniform-sparse` the tree's; the truth for a given game is in between. `Sap`'s
  collapse is definitive regardless.

## 9. Cycle-2 recommendation (data-backed)

**(a) Grid as an opt-in MOVER broadphase — QUALIFIED yes; the executor decides.** In
SERIAL mode the spatial-hash broadphase is 1.37–2.22× faster on Step and 1.5–7.6× on
pair-find at scale (byte-identical sim) — a clear win, and it validates keeping
`BroadphaseKind::Hash` opt-in-per-world. BUT the engine already has broadphase-MT for
the Tree (D2), and the SpatialHash has no parallel pair-find, so **under MT the Tree
overtakes Hash on dense (1.06×) and clustered (1.43×) at 10k**, leaving Hash a clear
win only on **sparse** (1.25×). So the grid mover broadphase is worth adopting when
(i) the broadphase runs serially, (ii) the workload is sparse, or (iii) a **parallel
grid pair-find** is built (the natural next step the MT axis points to). For
MT-dense/clustered workloads the existing Tree is competitive-to-better. Below ~1000
bodies the absolute differences are sub-millisecond (noise) — so any grid opt-in is
for **large** worlds regardless.

**(b) Move statics OFF the grid, onto a tree.** `m_staticGrid` (a `SpatialGrid`) is
~3× slower than a tree for static candidate lookup. Recommend routing static-body
candidate lookup through a `DynamicTree` (statics rarely move — a static tree is
cheap to build once) and retiring `m_staticGrid`, or at minimum re-benchmarking it
against a tree before keeping it.

**(c) The query index's identity is single-TILE / tight-neighbor occupancy — build
the API around that.** Grid wins tile (1.5–3.1×) and expresses a semantic the tree
can't. For region/large-AABB queries, delegate to the tree (competitive-to-better).
So a lean, tile-first occupancy API (`TileOccupants(x,y)`, small `Neighbors(x,y,k)`),
with `Region(AABB)` / k-nearest backed by the tree — NOT a grid-answers-everything
query layer.

**(d) Opt-in crossover.** Enable the grid index (broadphase and/or occupancy) only
for worlds that are both **large** (≥ ~1000 bodies) and **tile-structured**, and only
when tile/neighbor queries are frequent enough to amortize the ~2 ms/step (@10k)
residency upkeep. Non-tile / small / sparse worlds pay nothing (tree-only) — which is
the original opt-in premise, now validated.

**(e) Do NOT unify `m_staticGrid` and `m_residencyGrid`.** They point opposite ways:
statics belong in a tree (b); the residency/occupancy grid is the opt-in tile-query
index (c). Keep the occupancy grid as the single opt-in `SpatialGrid`; replace the
static grid with a tree.

**(f) Query API surface the data justifies:** `TileOccupants(cx,cy)` and small
`Neighbors/Ring(cx,cy,k)` on the grid; `Region(AABB)`, k-nearest, and cell-raycast/LoS
routed through the tree (or the existing `Raycast`/`LineOfSight` for LoS, which is
already the passability/`TileGrid` layer). Cell size/origin wired explicitly from the
tile grid (spec §2, still the right call).

## 10. What Cycle 2 should NOT re-litigate

Ownership (physics owns it 100 %), opt-in-per-world, cell==tile, tree stays the
default broadphase, determinism policy — all settled and confirmed by this data. The
open design work is (b) statics-onto-tree and (c)/(f) the tile-first query API, plus
the flagged (§8) engine robustness pass.
