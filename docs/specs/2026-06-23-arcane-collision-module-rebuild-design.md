# Arcane Physics — Collision Module Rebuild (Design)

- **Date:** 2026-06-23
- **Status:** Design approved; implementation plan pending.
- **Scope:** The 2D physics broadphase + contact pipeline (`Arcane/Core/src/Arcane/Physics/`).
- **Relates to:** the physics-perf investigation (Sandbox stress scene, 7 FPS root cause);
  `feedback_engine_evolves_not_frozen` (the Lua North Star is retired).
- **Supersedes (on landing):** the body-level `IBroadphase` + `SweepAndPrune`, the separate
  `ContactManager::Step` narrowphase pass, the `GenerateContacts` per-step rebuild, the interim
  `Collide()`/`GenerateContacts` AABB cull (commit `5ae0f97`), the `Baumgarte` solver, and all
  Lua-match golden tests.

---

## 1. Context & Motivation

Profiling the Sandbox stress scene (scene 8: ~1000 churned dynamic bodies + a 37-fixture kinematic
"whisk") at ~7 FPS showed the cost is the **single-threaded physics `Step` (~99% of the frame)**,
not the debug render (~1%). At dense churn the step ran **~45,000 `Collide()` narrowphase calls to
find ~2,000 real contacts — ~89% on provably-disjoint fixture pairs.**

Root causes, all structural and all inherited from the original Lua port:

1. **Body-level broadphase.** One proxy per body, AABB = union of all its fixtures. A multi-fixture
   body (the whisk's 37 wires; compounds) collapses to one giant fat box, so the narrowphase
   brute-forces every fixture pair. (`PhysicsWorld::SlotAabb` union; `IBroadphase` keyed by body id.)
2. **No contact persistence.** `GenerateContacts` clears and rebuilds the entire contact set from a
   full broadphase re-query every step.
3. **A second, redundant narrowphase pass.** `ContactManager::Step` re-runs the broadphase `Pairs()`
   *and* a full per-fixture `SlotsOverlap` narrowphase purely to derive begin/end events — duplicating
   work the contact generation already did. (~37% of the step.)
4. **Static linear scan.** `StaticCandidates` loops the entire `m_staticList` per dynamic body —
   O(dynamics × statics) — and recomputes each static's AABB on every query.

Box2D v3 (which our `SoftStep` solver is already modeled on) avoids all of this with **per-shape
broadphase proxies + enlarged AABBs + a move buffer**, **persistent contacts** with the **manifold
computed once per step**, and **begin/end events as a byproduct** of that one pass. Its "tumbler"
benchmark (2000 boxes in a rotating box — our scene at 2×) runs a few ms/step single-threaded. The
~50× gap is a single-threaded structural deficit, not a threading ceiling — so this rebuild lands
**before** any SIMD or multithreading work.

The interim AABB cull (commit `5ae0f97`) recovered ~27% (7.0 → 9.6 FPS) byte-identically as a
stopgap. It is removed by this rebuild.

## 2. Goals / Non-Goals

**Goals**
- Eliminate the per-fixture narrowphase brute-force (per-shape broadphase proxies).
- Compute each contact manifold at most once per step (persistent contacts).
- Eliminate the redundant second narrowphase pass (events as a byproduct).
- Make the fixed-tile grid a **first-class** spatial structure: statics/tile geometry and dynamic
  residency live in it natively, queryable by gameplay (region/resident queries → combat sphere).
- Preserve **determinism** (run-twice-identical) and behavioral correctness.

**Non-Goals (own specs / future consumers)**
- `SoftStep` solver internals — unchanged; just fed the new contacts.
- SIMD contact solver (future Spec 3).
- Multithreading (future Spec 4) — additive, after this lands.
- Combat-sphere instantiation — a future *consumer* of the grid's region-query API, not built here.

## 3. Decisions Log (from brainstorm)

1. **One coherent spec** covering broadphase + contact pipeline together (they are co-dependent).
2. **Adopt Box2D v3's collision module directly**, fitted to our `Shape`/`Fixture`/`Real` types and
   Core style — not a clean-room reinvention, not copied code.
3. **Consolidate the mover broadphase to one dynamic tree.** Retire the interchangeable mover
   broadphase strategies (`SpatialHash`-as-mover-strategy, `SweepAndPrune`) and the cross-strategy
   equivalence invariant. (The spatial-hash *algorithm* is not discarded — it is repurposed as the
   grid's backing per decision 5; it just no longer competes as a swappable mover broadphase.)
4. **The grid owns static + tile geometry collision *and* residency** (chosen over an all-tree design
   with a parallel gameplay grid). Statics are first-class on the grid; this also replaces the static
   linear scan.
5. **Grid backing = spatial hash (Teschner-optimized) at the fixed tile size, with per-shape
   registration.** SAP ruled out: it degrades to O(n²) under clustering (our pile is the worst case)
   and cannot serve region/resident queries.
6. **Retire `Baumgarte` and all Lua-match goldens completely** — no legacy support, no migration.
   Behavioral/analytic tests + a brute-force reference cross-check become the correctness gate.

## 4. Architecture Overview

Two co-equal, **per-fixture** spatial structures behind one `BroadPhase` facade:

- **`DynamicTree` (movers)** — one proxy per live fixture of a dynamic/kinematic body; enlarged
  (fat) AABBs; a move buffer so only proxies that left their fat box re-test → incremental
  mover↔mover candidate pairs.
- **`SpatialGrid` (statics + tiles + residency)** — spatial hash at the fixed tile size; static/tile
  geometry registers per-shape into cells; dynamic + kinematic bodies register cell residency. Serves
  mover↔static collision candidates, tile passability, and region/resident queries.

The body/fixture lifecycle (`AddBody` / `AddFixture` / `RemoveBody` / position moves) updates **both**
structures in one path — residency is maintained from day one, not bolted on.

The **contact pipeline** turns broadphase output into solver input + events in a single pass: one
persistent `Contact` per overlapping fixture-pair, manifold recomputed once per step, warm-start
on the contact, events from touch-state transitions. It feeds the existing `SoftStep` solver
unchanged.

```
AddFixture/move/remove ─┬─► DynamicTree (mover fixtures)  ─┐
                        └─► SpatialGrid (static fixtures +  ├─► move buffer ─► new candidate pairs
                                          mover residency) ─┘
                                                                     │
   persistent Contact array ◄───────────────────────────────────────┘
        │  (one update pass/step)
        ├─ fat-AABB separated?        → destroy contact (+End event)
        ├─ else recompute manifold ONCE → Begin/Stay/End byproduct + warm-start match
        └─ touching & ≥1 dynamic & non-sensor → ContactConstraint ─► SoftStep solver
   ForEachContact (debug overlay) reads the touching set directly.
```

## 5. Component — `DynamicTree` (movers), per-shape + incremental

- **Proxies are per-fixture, movers only.** A `fixtureSlot → proxyId` map; proxies created in
  `AddFixture`, destroyed in `DropFixture`/`RemoveBody`. The whisk's 37 wires become 37 small proxies
  that each pair only with nearby bodies.
- **Enlarged (fat) AABBs.** Stored box = `tightAABB + kAabbMargin` (constant skin), optionally
  extended along the body's velocity (predictive). A proxy is re-inserted only when its *tight* AABB
  escapes its stored *fat* box — slow/resting fixtures never touch the tree between steps.
- **Move buffer → incremental pairs.** Each step, fixtures whose tight AABB left the fat box are
  re-fattened and pushed to a move buffer; we query the tree **only for moved proxies**
  (O(moved·log n), vs today's full O(n·log n) `Pairs()` run *twice*). Each new fat-box overlap becomes
  a candidate proxy-pair handed to the contact pipeline. **Pair destruction is owned by the contact
  pipeline** — the broadphase only ever *adds*.
- **Reuse.** Keep the existing index-based, zero-alloc `DynamicTree` node pool; re-key it from bodies
  to fixtures rather than rewriting it.
- **Determinism.** Move buffer processed in proxy-id order; new candidate pairs stored canonical
  `(lo,hi)`; the per-step new-pair batch sorted before handoff. No tree iteration order leaks out.

## 6. Component — `SpatialGrid` (spatial hash at the fixed tile size)

- **Cells + hash.** Cell coord = `floor(worldPos / tileSize)`; `hash(cx,cy) → bucket`, table sized
  ~10× resident count. Bounded memory, sparse/streaming-friendly, no dense map array.
- **Registration (per-shape).**
  - Static + tile geometry registers into every cell its fixture AABB overlaps → mover↔static
    collision candidates.
  - Dynamic + kinematic bodies register **cell residency** as they move (same lifecycle path as the
    tree) → the gameplay index.
- **Passability layer.** The static `TileGrid` folds in: a per-tile passability attribute (from the
  map's `IPassabilitySource`) lives alongside the residency buckets — one grid, a dense passability
  layer + sparse residency buckets. `Raycast`/`LineOfSight` keep their Cartesian DDA over this layer.
- **Query API (first-class surface).**
  - `StaticCandidates(aabb)` → static fixtures in the overlapped cells (replaces the linear scan).
  - `Residents(tile)` / `Residents(tileRegion)` → bodies in a tile/region (gameplay, combat sphere).
  - `Occupancy(body)` → tiles a body covers; `Passable(tile)` / LOS.
- **Determinism & robustness.** Cells iterated in sorted `(cx,cy)` order; outputs sorted by id and
  de-duped (a fixture spanning several cells appears once); arithmetic-free integer interval test for
  the cell overlap; hash collisions rejected by the tight test, never affecting the output set.
- **Large-shape valve.** A static fixture spanning more than a small cell-count threshold goes into a
  tiny "oversized statics" list (linear-scanned — few in practice, e.g. a full-arena ground plane)
  instead of flooding thousands of buckets.

## 7. Contact Pipeline — persistent contacts + events-as-byproduct

- **Persistent contacts.** One pooled `Contact` per overlapping fixture-pair, with a
  `(loFixture, hiFixture)` key map for dedup. Created when a *moved* fixture's broadphase query
  reports a new fat-AABB overlap — **both sources feed the same array** (tree → mover↔mover; grid →
  mover↔static, a moved dynamic fixture querying its grid cells). A resting fixture keeps its contacts
  without re-querying.
- **One update pass per step.** For each contact: if the two fat AABBs separated → **destroy** (emit
  `End` if it was touching); otherwise recompute the manifold **once** (the sole narrowphase call).
  pointCount `0→>0` = `Begin`, `>0→0` = `End`, staying `>0` = `Stay`. **Warm-start impulses live on
  the contact**, matched across steps by manifold feature id (today's `MixContactId`).
- **Solver feed.** Walk the touching contacts; where ≥1 body is dynamic and neither fixture is a
  sensor → emit a `ContactConstraint` (A = dynamic). Sensors and kinematic-only pairs → events only.
  This replaces `GenerateContacts`' from-scratch rebuild; the constraint array is a walk over
  persistent contacts, fed to the unchanged `SoftStep` solver.
- **Determinism.** New pairs created from the sorted move-buffer/grid batches; the constraint array
  and event queue sorted by `(bodyA, bodyB, fixtureA, fixtureB)` before solve / before deferred
  delivery.
- **Consumers.** `ForEachContact` reads the touching set directly (Sandbox debug overlay). The old
  `ContactManager::Step` (its second broadphase `Pairs()` + `SlotsOverlap` narrowphase) is deleted —
  that work is now the byproduct above.

## 8. Testing, Determinism & Lifecycle Robustness

**Testing gate (Lua goldens retired → behavior + reference cross-check):**
- **Analytic/behavioral suites are the correctness gate** — stacks settle, pyramid stable, joints
  hold, CCD doesn't tunnel, compound tips correctly, restitution within bounds, resting penetration
  bounded, energy doesn't explode. These assert *behavior*, not exact numbers.
- **Reference cross-check (replaces the cross-strategy equivalence invariant):** a brute-force O(n²)
  AABB-pair oracle; assert the incremental tree+grid candidate set **equals** the brute-force set each
  step on randomized scenes. Guards the move-buffer / pair-lifecycle logic (the riskiest new code).
- **Contact-pipeline unit tests:** persistence across steps; `Begin/Stay/End` sequences; warm-start
  continuity; sensor = events-only; destruction on separation *and* on body removal.
- **Grid query tests:** `StaticCandidates == brute-force`; `Residents(tile/region)` correctness;
  passability/LOS unchanged.
- **Determinism:** run-twice bit-identical, plus one *re-baselineable* golden snapshot as an
  unintended-change tripwire (not a freeze).
- **Perf smoke:** the stress scene through the perf harness with a tracked step-time budget.

**Lifecycle & robustness (error handling):**
- **Removal:** destroying a body/fixture removes its proxies (tree + grid cells) and destroys its
  contacts (emitting `End`); the per-step update tolerates stale handles.
- **Slot recycling:** proxies + contacts keyed by fixture slot **+ generation**, so a recycled slot
  cannot alias a stale contact or its warm-start impulse.
- **Deterministic id assignment:** proxy/contact creation order is a determinism-sensitive seam —
  free-list / sorted-batch order is fixed and documented.

## 9. Tunables (documented at the definition site)

| Tunable | Default intent | Trade-off |
|---|---|---|
| `kAabbMargin` (tree fat-box skin) | a few world units | larger → fewer re-inserts, more false candidate pairs |
| predictive velocity extension | on for fast movers | catches fast approach; too large → false pairs |
| grid hash load factor | ~10× resident count | smaller → more collisions (cheap false positives); larger → memory |
| oversized-static cell threshold | small N cells | below → grid; above → linear "oversized" list |

## 10. Implementation Sequencing

Built in gate-green increments — never mid-rewrite with a red suite; each phase delivers a measurable
win.

0. **Perf harness** — stand up the permanent (gated) step/phase-timing + stress-scene smoke (the
   measurement backbone). *Independent quick win alongside: make "pause" skip `Step()`.*
1. **`SpatialGrid`** — spatial hash + per-shape static registration + residency + query API +
   passability fold-in; route `StaticCandidates` through it. Validate `== brute-force`. **Lands the
   static-scan fix; statics first-class on the grid.**
2. **`DynamicTree` → per-fixture proxies + fat AABBs + move buffer** + incremental candidate pairs;
   adapt `GenerateContacts` to consume fixture-pairs. Validate `incremental == brute-force`. **Lands
   the whisk fix (the 89% waste).**
3. **Persistent contacts + single manifold pass** + warm-start-on-contact; replace `GenerateContacts`'
   rebuild with a contact walk feeding the solver. Validate behavior + determinism. **Collapses the
   ~40% gen-rebuild.**
4. **Events-as-byproduct** — delete `ContactManager::Step`'s separate pass; `Begin/Stay/End` from
   contact touch-state. Validate event sequences. **Collapses the ~37% events pass.**
5. **Cleanup + re-baseline** — remove the interim AABB cull, `SweepAndPrune`, `Baumgarte` + Lua
   goldens + the equivalence invariant; re-baseline the determinism golden; document tunables.

Dependencies: 1 and 2 are independent (either order) and both precede 3; 4 follows 3; 5 last.

## 11. References

- Box2D 3.0 release notes — https://box2d.org/posts/2024/08/releasing-box2d-3.0/
- Box2D features (contact/broadphase lifecycle) — https://deepwiki.com/erincatto/box2d/1.1-features
- Box2D simulation islands — https://box2d.org/posts/2023/10/simulation-islands/
- Teschner et al., Optimized Spatial Hashing — https://matthias-research.github.io/pages/publications/tetraederCollision.pdf
- 0fps, Collision detection part 2 (box intersection) — https://0fps.net/2015/01/18/collision-detection-part-2/
- Build New Games, Broad-Phase via Spatial Partitioning — http://buildnewgames.com/broad-phase-collision-detection/
