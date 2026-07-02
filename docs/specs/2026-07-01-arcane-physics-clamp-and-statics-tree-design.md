# Arcane Physics: Velocity Clamp + Statics-onto-Tree — Design

Date: 2026-07-01
Status: Draft (brainstorm) — for review
Branch: `feature/arcane-physics-clamp-statics-tree` (off main `0702189f`)
Motivates from: `2026-07-01-arcane-spatial-grid-benchmark-findings.md` (§8 robustness flag,
§4 statics verdict — that doc lives on the `feature/arcane-spatial-grid-benchmark` branch,
pending its own merge).

## 1. Why

The SpatialGrid benchmark spike surfaced two data-backed engine follow-ups, actioned here
together as one contained workstream (both Core/Physics, behavior-preserving for normal sims):

- **A — escape-crash robustness (correctness).** The full-window 10k benchmark run **crashed**
  in Release: unwalled high-density scenes let bodies accumulate unbounded velocity (no walls,
  no damping) and escape to extreme coordinates over hundreds of steps; a coordinate eventually
  goes out of range / non-finite and the downstream `SpatialGrid` cell-range explodes (or the
  motion blows up). The root cause is a **Box2D v3 parity gap**: `IntegrateVelocitiesRange`
  (`SoftStep.cpp:320`) applies gravity + linear damping but has **no max-velocity/translation
  clamp** — Box2D v3's `b2IntegrateVelocitiesTask` clamps every body's linear and angular speed
  as a safety net; Arcane does not.

- **B — statics onto a tree (perf).** The bench measured `m_staticGrid` (a `SpatialGrid`) at
  ~3× slower than a `DynamicTree` for static candidate lookup (62ns vs 17ns @10k). Statics
  belong in a tree.

Cycle-2 threads C (tile-occupancy query index) and D (grid mover broadphase / parallel grid
pair-find) are **out of scope** here — deferred, to be actioned separately.

## 2. Units — Arcane == Box2D units (no unit adaptation)

`Arcane::Physics::kLinearSlop = Real(0.005)` (`PhysicsTypes.hpp:145`) is exactly Box2D's
meter-based `b2_linearSlop` default, so **Arcane's physics operate in Box2D units**
(`lengthUnitsPerMeter = 1`); "pixels" are the game's render scale, not the physics unit. Box2D
v3's clamp constants and default therefore carry over **1:1** — no unit scaling. (Sanity: the
bench's `velScale` 20–60 is far below Box2D's per-step translation cap, so normal sims never
trip the clamp.)

## 3. Part A — velocity/translation clamp (exact Box2D v3 parity) + defensive grid guard

### A1. The clamp (matches Box2D v3 EXACTLY)

**Reference the actual Box2D v3 source** (`github.com/erincatto/box2d`): `src/solver.c`
`b2IntegrateVelocitiesTask`, `src/world.c` `b2DefaultWorldDef`, and `src/constants.h`
(`B2_MAX_ROTATION`, length-unit constants). Replicate the clamp formula, constants, and default
value verbatim — do not invent numbers. The mechanism (to be confirmed against source):

- After gravity + linear damping are applied, per body, clamp:
  - **linear:** if `dot(v,v) > maxLinearSpeed²`, scale `v *= maxLinearSpeed / length(v)`.
  - **angular:** if `w² > maxAngularSpeed²`, scale `w *= maxAngularSpeed / |w|`, where
    `maxAngularSpeed = B2_MAX_ROTATION * invH` (Box2D's quarter-turn-per-step cap; unit-free).
- `maxLinearSpeed` comes from a new `WorldDef` field (name/semantics mirroring Box2D v3's
  `maximumLinearSpeed`), defaulted to Box2D v3's default. Store it on the world; the solver reads
  it (mirror how `ContactPushMaxVelocity()` is threaded, `PhysicsWorld.hpp:1132`).

**Placement:** `SoftStep::IntegrateVelocitiesRange` (`SoftStep.cpp:320-356`), after the
gravity+damping block, before writing `m_bodyState[i].vx/vy/w`. This is the sole body-integrate
site (integrate is scalar; the SIMD path is contacts-only). The implementer MUST confirm there
is no second integrate path (e.g. a wide/SIMD body integrate) — if one exists, the clamp goes
there too, identically.

**Determinism:** the clamp uses `length`/`sqrt` under `/fp:strict` (no `/fp:fast`) — deterministic;
a per-body branch, no cross-lane dependence. ST==MT byte-identity is preserved (the clamp is a
pure per-body function of that body's own state).

### A2. Defensive `SpatialGrid` guard (Arcane-specific belt-and-braces)

Box2D has no SpatialGrid to mirror, so this is pure defense-in-depth: `SpatialGrid::CellCoord`
/ `CellRange` (`SpatialGrid.cpp:10-20`) must not turn a non-finite or absurdly-large AABB into a
runaway cell loop (the proximate crash). Guard: if any input coordinate is non-finite
(`!std::isfinite`) or the computed cell range would exceed a sane cell-count budget, skip the
registration/query (treat as empty) rather than looping billions of cells. No behavior change for
valid input (the guard only triggers on already-broken input the clamp now prevents).

### A3. Acceptance stance (per "match Box2D v3 exactly")

The **goal is exact Box2D v3 parity**, not preserving current behavior at all costs. Expected
outcome: `[physics]` suite **byte-identical** (normal scenes stay under the clamp). If exact-Box2D
behavior legitimately changes a case (a test body that Box2D itself would clamp), that case
**should** change — re-baseline it **with written justification** naming the exceeded clamp, rather
than tuning the constant to hide it. A change from a scene that should NOT exceed the clamp is a
bug to investigate.

## 4. Part B — statics onto a `DynamicTree`

Replace the `m_staticGrid` (`SpatialGrid`) static index with a `DynamicTree` (`BroadphaseKind::Tree`'s
structure; it already implements `IBroadphase`). Statics never move, so the tree's move-buffer is
simply unused — that is fine (it is exactly what the bench measured at ~3×).

**Touch-points (behavior-preserving — the tree returns the identical tight-narrowed candidate set):**
- `PhysicsWorld.hpp:1403` — `SpatialGrid m_staticGrid{...}` → `DynamicTree m_staticTree;`
- `PhysicsWorld.cpp:1015` — `m_staticGrid.Insert(idx, SlotAabb(idx))` → `m_staticTree.Update(idx, ...)`
  (Update is upsert: first Update inserts).
- `PhysicsWorld.cpp:506, 561, 1558` — `m_staticGrid.Move(...)` → `m_staticTree.Update(...)`.
- `PhysicsWorld.cpp:1140` — `m_staticGrid.Remove(idx)` → `m_staticTree.Remove(idx)`.
- `Queries.cpp:553` — `m_staticGrid.QueryAABB(box, gridScratch)` → `m_staticTree.QueryAABB(box, gridScratch)`
  (same `int QueryAABB(const Aabb2&, std::vector<uint32_t>&)` signature + sorted-candidate contract).
- **Accessor + debug-viz ripple:** the public `StaticGrid()` (`PhysicsWorld.hpp:709`, returns
  `const SpatialGrid&`) becomes `StaticTree()` (`const DynamicTree&`). Its consumers migrate from
  grid-cells to tree-leaves — `PhysicsDebugDraw.cpp` static overlay (`ForEachCell` → `ForEachLeaf`,
  mirroring the existing mover-tree overlay), the Sandbox HUD label, and `PhysicsDebugAccessorsTest.cpp`.
  These land in the SAME commit as the index swap (they will not compile otherwise). It spans
  Core + Arcane.dll + Sandbox + Tests — larger than the physics-only touch-points, but a necessary
  consequence of the index type change.
- `m_staticGridScratch` and `m_staticList` are unchanged (`m_staticList` is a separate flat list used
  elsewhere, e.g. `PhysicsWorld.cpp:2853`; leave it). **Whether the tree should subsume `m_staticList`**
  — removing the flat list, e.g. making that full-static iteration query the tree instead — is a
  **DEFERRED, benchmark-driven decision** (measure whether that path is hot before touching it; don't
  assume). This workstream keeps `m_staticList` in place, parallel to the tree, exactly as today.

`DynamicTree.hpp` is already included by `PhysicsWorld.hpp`. The `StaticCandidates` narrowing
(tight `AabbOverlap` re-test) is unchanged, so the emitted static-candidate set is identical to today.

## 5. Testing

Both parts target `[physics]` **byte-identical** (A3's re-baseline exception applies to A only).

- **A1 clamp:** a new behavioral test — a high-energy / would-runaway configuration (dense bodies,
  no walls, no damping) stepped enough that WITHOUT the clamp a body's `|v|·h` exceeds
  `maxTranslation`; WITH the clamp, every awake body satisfies `|v| ≤ maxLinearSpeed` and
  `|w| ≤ maxAngularSpeed` every step, and positions stay finite/bounded. Assert the clamp caps at
  exactly the Box2D formula. Also confirm a normal moderate-velocity scene is byte-identical
  (clamp never fires).
- **A2 guard:** a unit test feeding `SpatialGrid` a non-finite / absurd AABB — `Insert`/`QueryAABB`
  return without looping/allocating unboundedly (no crash, empty result).
- **B oracle:** `StaticCandidates` returns the identical set (sorted, tight-filtered) with the tree
  vs the prior grid, over a stream of query boxes on a static-heavy scene — plus a `RemoveBody` /
  `AddFixture` case (mirrors the existing `PhysicsSpatialGridTest` static cases, which must still pass
  against the tree-backed index).
- Full `[physics]` run on the final HEAD; any A-driven divergence root-caused + justified (A3).

## 6. Non-goals

- No Cycle-2 feature (tile-occupancy query index) or grid mover broadphase / parallel grid
  pair-find (threads C/D) — separate future workstreams.
- No new unit system (Arcane already == Box2D units; §2 only confirms it).
- No retirement of `m_residencyGrid` (the occupancy index) — untouched here.
- No change to `m_staticList` or the `StaticCandidates` narrowing logic. Whether the tree should
  eventually subsume `m_staticList` is a **deferred, benchmark-driven** question (§4) — not settled here.

## 7. Risks

- **Clamp threshold vs existing tests:** if a current `[physics]` scene has a body faster than
  Box2D's default cap, exact parity re-baselines it (A3). Expected to be zero cases; verify.
- **Second integrate site:** if a wide/SIMD body-integrate exists beyond
  `IntegrateVelocitiesRange`, the clamp must be replicated there identically or ST==MT parity
  breaks — the implementer confirms this explicitly.
- **Debug-viz consumers:** swapping the static index type breaks `StaticGrid()` and its consumers
  (debug overlay, HUD label, `PhysicsDebugAccessorsTest`) — they migrate to the tree in the same
  commit as the swap (they will not compile otherwise). Spans Core + Arcane.dll + Sandbox + Tests.
- **Tree-for-statics semantics:** `DynamicTree::Update` is upsert (no separate Insert); ensure the
  AddBody path uses `Update` for the first registration (it does by contract). Static bodies with
  `AddFixture` growth must `Update` the grown AABB (mirrors the existing `m_staticGrid.Move`).
