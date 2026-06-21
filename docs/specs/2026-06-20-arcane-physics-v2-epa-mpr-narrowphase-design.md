# Arcane Physics v2 — EPA + MPR Narrowphase Specialization — Design

> **Program context.** This is the **narrowphase-specialization** workstream of the **Physics v2**
> program — a first-party, state-of-the-art 2D physics engine. It is NOT one of the sequenced
> gameplay phases (A rigid core / B character controller / C materials / D soft+particles /
> E destruction); it is a parallel **engine-generality** workstream that hardens the collision
> narrowphase. It builds directly on the Phase A rotation-aware, fixture-based rigid-body core
> (`docs/superpowers/specs/2026-06-15-arcane-physics-v2-phase-a-design.md`) and is independent of
> Phases B/C (no live-game / feel-judgment validation required — the gate is analytic). It is
> sequenced BEFORE (and partly to enable) Phase E destruction, which wants richer convex-overlap
> inputs.

## Goal

Make the Arcane narrowphase **general and exact for deep convex overlap** by restructuring
`Collide()` into an explicit **static type-pair dispatcher** over named, independently-testable
solvers, and adding two of them: **EPA** (Expanding Polytope Algorithm — exact minimum-penetration
axis/depth for deep convex overlap) and **MPR** (Minkowski Portal Refinement — a fast single-point
deep-overlap solver). The concrete correctness hole this closes is the **deep round-in-polygon
centroid approximation** in today's `CollideRound` (`Collide.cpp:271-296`): when a circle/capsule
core is deeply inside a polygon, GJK degenerates at distance 0 and the code falls back to a
**centroid-to-centroid** normal with **centroid** witness points — wrong penetration direction and
depth. EPA replaces that branch with the exact nearest-face axis; MPR provides a cheap single-point
alternative for particle-scale and future curved shapes, and serves as EPA's convergence fallback.

## The locked foundational decisions (from brainstorming)

1. **Driver = generality / routing-first.** Build a proper narrowphase **dispatcher** (named
   solvers behind a static type-pair table), not merely a point-patch of the centroid branch. This
   anticipates curved / high-vertex / future shapes via routing rather than rewrites.
2. **Routing = static type-pair dispatch table whose general cell is a GJK→EPA pipeline (with MPR
   available in that cell).** This is the mainstream Bullet / Chipmunk2D design and a clean
   evolution of Arcane's current classify-then-branch. The runtime escalation (GJK distance ≈ 0 →
   EPA) lives *inside one table cell*, not at the top level. (Box2D itself uses NO EPA — it relies
   on speculative margins + CCD and accepts the deep approximation; Arcane chooses to close the
   gap exactly.)
3. **EPA and MPR have distinct, non-overlapping roles + EPA gets a safety net.**
   - **EPA** = exact 2-point manifolds for deep convex overlap (closes the deep-round gap now; its
     penetration axis drives the EXISTING Phase A reference-face clip).
   - **MPR** = fast single-point contacts where one point suffices (Phase D particles at scale;
     future curved shapes with no face normals), AND EPA's fallback when EPA fails to converge.
4. **Determinism unchanged:** f32 per-platform, fixed iteration caps, deterministic ordering, no
   wall-clock, `/fp:strict` already in effect on Core (Phase A T9). EPA/MPR carry internal
   arithmetic in **f64**, narrowing to `Real` on return — the same precision discipline the existing
   GJK simplex uses.

## Key grounding finding (scopes the work accurately)

Phase A's `Collide()` already routes correctly for the common case: **poly × poly deep overlap uses
SAT reference-face clipping, whose minimum-translation axis is ALWAYS a face normal for convex
polygons — provably exact.** So **poly × poly never needs EPA** and must stay on its existing fast
path (zero perf regression, byte-identical behavior). The ONLY exact-correctness hole is the
**round-core deep-overlap** branch (`CollideRound`, the centroid approximation). EPA's real domain
is therefore: round-core deep overlap (now) + arbitrary non-polygon convex (future). This keeps the
workstream tightly scoped — it is a fallback-cell upgrade, not a narrowphase rewrite.

## Architecture

### 1. Static type-pair dispatcher

`Collide(a, xfA, b, xfB, speculativeMargin)` stays the single public entry, but its body becomes a
thin **dispatcher**: classify each shape by **core category** (each shape already carries its
unified core — circle = 1 vert, capsule = 2 verts, polygon = N verts, all + radius) and call one
named solver. Two categories:

- **Round** = circle (1 vert) / capsule (2 verts).
- **Poly** = 3+ verts.

The existing solver bodies (SAT ref-face clip, GJK-witness round path) are retained and become
named units the dispatcher calls. EPA and MPR are added as new sibling units.

### 2. Routing table

| Pair | Separated / shallow | Deep overlap (GJK distance ≈ 0) |
|---|---|---|
| Poly × Poly | SAT ref-face clip *(existing, exact)* | SAT ref-face clip — **no EPA** (MTV is a face normal) |
| Round × Poly | GJK witness *(existing)* | **EPA** — closes the centroid-approx gap |
| Round × Round | GJK witness *(existing)* | **EPA** |
| General convex *(future high-vertex/curved)* | GJK witness | **EPA** primary, **MPR** fallback |
| One-point-acceptable *(Phase D particles)* | — | **MPR** (fast single point) |

Consequence: box stacking and all poly-poly contacts stay on the existing exact fast path — **zero
perf regression on the common case**. EPA runs only when a round core is deeply overlapping (or, in
future, a non-polygon convex pair).

### 3. EPA — exact deep convex penetration

`Narrowphase/Epa.{hpp,cpp}`. Standard 2D EPA:

- **Seed:** the origin-enclosing simplex from the GJK pass that detected the overlap (see seam below).
- **Expand:** iteratively find the polytope edge closest to the origin, compute a new support point
  in that edge's outward normal direction, insert it, until convergence (support distance gain <
  epsilon) or the iteration/size cap.
- **Returns:** penetration **axis** (normal, B→A convention) + **depth** + **witness** point on the
  Minkowski boundary.

**Manifold construction reuses existing machinery (no new clip code):** EPA's penetration axis is
fed into the **existing Phase A reference-face clip** (`ClipSegment` + the stable-feature-id scheme
from T3) to emit ≤2 contact points with stable ids. EPA only supplies the axis GJK could not
(degenerate at distance 0); it slots into exactly the position the centroid approximation occupies
today. Radii are added back by the caller (round cores: surface = core witness offset outward by r).

### 4. MPR — fast single-point deep overlap

`Narrowphase/Mpr.{hpp,cpp}`. Standard 2D Minkowski Portal Refinement:

- Build a portal from an interior point (Minkowski difference of the two cores' centroids) toward
  the origin; refine the portal until it straddles the origin ray; the final portal face gives the
  contact **normal + depth + single point**.
- **Returns** a **1-point manifold** (sufficient for particles / curved shapes). If an MPR-routed
  pair ever needs 2 points, its normal drives the same reference-face clip as EPA.
- **As EPA's fallback:** when EPA fails to converge within its caps, the dispatcher falls to MPR
  for a robust single-point result rather than emitting the bad centroid approximation.

### 5. The one new seam into existing code

EPA needs the terminal origin-enclosing simplex from the GJK overlap detection as its seed.
`GjkDistanceCore` is extended to **retain and return that terminal simplex when distance ≈ 0** (or,
equivalently, EPA runs a short internal GJK-to-overlap pass to obtain it). This is the only
modification to existing narrowphase code beyond the dispatcher restructure; the separated/shallow
GJK return value and all existing callers are unchanged (additive output field).

## What changes, module by module

| Module | Change |
|---|---|
| `Narrowphase/Collide.cpp` | `Collide()` becomes a static type-pair **dispatcher**; existing SAT-clip + GJK-witness paths retained as named callees; the `CollideRound` centroid-approx deep branch is **replaced** by an escalation to EPA. |
| `Narrowphase/Epa.{hpp,cpp}` | **NEW.** 2D EPA: axis + depth + witness for deep convex overlap; f64 internal, deterministic caps. |
| `Narrowphase/Mpr.{hpp,cpp}` | **NEW.** 2D MPR: single-point normal/depth/point; f64 internal; EPA convergence fallback. |
| `Narrowphase/Gjk.{hpp,cpp}` | Additive: `GjkDistanceCore` returns the terminal origin-enclosing simplex when distance ≈ 0 (EPA seed). Existing outputs/callers unchanged. |
| `Narrowphase/Manifold.*` / clip | **Unchanged** — EPA/MPR axes drive the EXISTING reference-face clip + stable-id scheme. |
| `Solver/*`, `ContactManager`, `Island`, queries, CCD | **No change** — they consume `Collide()`'s manifold exactly as today. |
| `Tests/` | New `PhysicsEpaTest.cpp`, `PhysicsMprTest.cpp` (analytic); a deep-penetration-recovery invariant + a deep-overlap scene in the determinism gate. |

## Algorithm choices & alternatives considered

The narrowphase deliberately uses different algorithm families per job; recording the boundaries so
the next reader does not reach for the wrong tool:

| Job | Chosen | Alternatives NOT used (and why) |
|---|---|---|
| Discrete manifold clipping | **Sutherland-Hodgman** (incident edge vs reference-face side planes; `ClipSegment`, Box2D v3 `b2ClipSegments` lineage) | **Greiner-Hormann** — general Boolean/concave polygon clipping; unnecessary for contact gen (we only clip ONE convex edge against ONE face for ≤2 points). Its home is the offline **geometry-utilities** workstream (decomposition, sprite-outline Booleans, Phase E fracture cuts), NOT the runtime narrowphase. S-H is exact here precisely because cores are convex (concave → convex fixtures). |
| Distance, closest points, CCD | **GJK** + **conservative advancement** (`ShapeCast`/`ShapeCastPoly`) | **Lin-Canny / V-Clip** — incremental closest-feature / Voronoi-region tracking for continuous collision; would add a parallel feature-adjacency subsystem for no benefit since GJK already owns distance, and conservative advancement is fixed-iteration (determinism-friendly). Modern Box2D/Bullet approach. |
| Deep-overlap penetration axis | **EPA** (exact) + **MPR** (fast single point) | (this workstream adds them) |

**EPA/MPR reuse, not replace, the clip:** EPA/MPR supply only the penetration AXIS that GJK cannot
(degenerate at distance 0). That axis is handed to the EXISTING Sutherland-Hodgman `ClipSegment` to
emit the 2-point manifold — this workstream adds **no new clip code**.

## Determinism

- **f32** per-platform, fixed 60 Hz, stable index iteration, no wall-clock — all retained.
  `/fp:strict` (Phase A T9) already covers Core, so FP contraction cannot drift EPA/MPR.
- EPA/MPR internal arithmetic in **f64**, narrowed to `Real` on return (the existing GJK pattern).
- **Deterministic termination:** EPA — max iterations + max polytope size + fixed convergence
  epsilon, deterministic closest-edge selection (priority by distance with a fixed index tie-break);
  MPR — max refinement iterations + fixed epsilon. No data-dependent unbounded loops.

## Re-baseline test strategy (all analytic — no oracle)

- **EPA exactness:** circle pushed deep into a box → exact nearest-face normal + depth =
  `r + insideDistance` (the case the centroid approx currently gets wrong); capsule deep in a box at
  an angle; round-vs-round deep. Hand-derived expected values.
- **EPA ↔ MPR cross-check:** same deep pair → normals agree within tolerance; MPR depth ≤ EPA depth
  (EPA is the true minimum). Validates both against each other.
- **MPR single-point:** a particle-vs-poly deep contact yields a correct 1-point manifold
  (normal + depth + point).
- **No regression:** existing manifold / `[invariant]` suites stay green; **poly × poly is
  byte-identical** (proves the common path is untouched). New **deep-penetration-recovery**
  invariant — a body that deeply overlaps in one frame settles cleanly with no energy explosion —
  and a **deep-overlap scene added to the run-twice determinism gate**.

## Non-goals (explicitly deferred)

- **Closed-form primitive specializations** (circle×poly nearest-face, capsule×poly, etc.) as a
  perf optimization — deep-round goes through the general EPA path for now (one path, simpler). A
  future optimization may add specialized cells; the table makes that a localized change.
- **Genuine curved shape types** (true arcs / ellipses / NURBS) — none exist in the engine today.
  The "general convex / future curved" table cell routes to EPA/MPR on the polygon-core
  representation; a real curved type later is a NEW TABLE ROW, not a rewrite.
- **Particle systems** — Phase D. This workstream only makes MPR *ready* (single-point output);
  it does not build particles.
- **3D EPA/MPR** — the engine is 2D.
- **Geometry utilities** (convex hull / decomposition) — a separate workstream that feeds Phase E.

## Risks & mitigations

- **EPA termination / robustness.** Mitigated by fixed iteration + polytope-size caps, the f64
  internal pipeline, and the MPR convergence fallback (a non-converging EPA degrades to a robust
  single point, never to the old centroid approximation). Bounded by construction.
- **Determinism drift from the new f64→f32 paths.** Mitigated by `/fp:strict`, deterministic
  ordering, and the deep-overlap scene added to the run-twice determinism gate.
- **Common-case regression.** Mitigated structurally: poly×poly never enters EPA; the poly-poly
  byte-identity test is the guard.
- **GJK seam.** The terminal-simplex return is additive (new output field populated only when
  distance ≈ 0); existing GJK callers (`ShapeDistance` / `ShapeCast` / queries) are unaffected.

## Success criteria

- A round core deeply overlapping a polygon produces the **exact** nearest-face normal + depth
  (EPA), replacing the centroid approximation; verified by hand-derived analytic tests.
- `Collide()` is an explicit static type-pair **dispatcher** over named solvers (SAT-clip,
  GJK-witness, EPA, MPR); each solver is independently testable.
- **MPR** produces correct single-point manifolds and serves as EPA's convergence fallback;
  EPA↔MPR cross-check passes.
- **Poly × poly is byte-identical** (no common-case regression); full `[physics]` + `[invariant]`
  suites green Debug + Release, both backends; deep-penetration-recovery + deep-overlap-determinism
  invariants green; ArcaneCore builds static-CRT.
- Presentation-free Core preserved (glm + std + sibling Physics headers only); no new dependency.

## Next step

On approval: `writing-plans` → a phased implementation plan (GJK terminal-simplex seam → EPA solver
+ analytic tests → dispatcher restructure wiring EPA into the deep-round cell → MPR solver +
single-point tests + EPA fallback → deep-penetration/determinism invariants + full-suite + dual-flavor
gate), executed via subagent-driven-development.
