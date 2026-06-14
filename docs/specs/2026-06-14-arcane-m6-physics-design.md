# Arcane M6 — 2D Physics Engine — Design

**Date:** 2026-06-14
**Status:** APPROVED — design contract for Arcane's first-party 2D physics engine: a
presentation-free, per-platform-deterministic, fixed-step rigid-body engine that PORTS the
proven Lua physics design and MODERNIZES its solver/broadphase to current best-in-class.
**Upstream:**
- `2026-06-10-client-physics-engine-design.md` — the IMPLEMENTED Lua engine this ports
  (Collider/Fixture/Rigidbody model, GJK+SAT narrowphase, SequentialImpulse solver,
  joints, islands, TileGrid single-source passability, CharacterController, the
  `physics_harness` determinism oracle). Its feature surface + game-specific decisions are
  the behavioral spec.
- `2026-06-11-engine-architecture-design.md` — M6 = physics port into a **presentation-free
  sim module** so the server can run authoritative validation with the same code (the port
  audit's T3 trigger, by design); determinism gate (`/fp:precise`, no contraction,
  per-platform self-consistency first); the host loop's reserved "Arcane pre-step (physics
  world)" slot.
- `2026-06-13-arcane-m4-sim-substrate-scene.md` + `project_arcane_m5_complete` — the M4/M5
  substrate this plugs into: engine-owned `Astra::Registry`, per-phase `SystemSchedulers`,
  `RunLoop` (fixed 60 Hz accumulator), `Arcane::Runtime`, the scene transform components
  (`LocalTransform`/`WorldTransform`), `JobSystem`/`IWorkScheduler` for parallelism.
- Research (2026-06-14): Box2D v3 (Aug 2024) **Soft Step solver** (soft constraints + TGS
  sub-stepping, from Catto's Solver2D study; far more stable than the v2-era Baumgarte
  sequential-impulse the Lua engine uses, at similar cost); v3 is 2x+ faster via
  data-oriented SoA + SIMD; **cross-platform determinism is achievable in plain IEEE-754
  float** (Box2D v3.1, Rapier `enhanced_determinism`) — no fixed-point required; the
  **dynamic AABB tree** (k·log n) is the gold-standard mover broadphase.

## Milestone framing and the three resolved forks

M6 is the engine bring-up table's "physics port". Three design forks were resolved with the
user (2026-06-14):

1. **First-party, port-AND-modernize** (not a literal 1:1 port, not vendored Box2D). Port
   the Lua engine's feature surface + the game-specific parts that are the whole reason it
   dropped Box2D (TileGrid single-source passability, the bespoke `CharacterController`
   slide modes, the Combat-Sphere event-gating model) + the geometric/kinematic core
   faithfully — AND upgrade the dynamics to the **Soft Step solver** (TGS soft +
   sub-stepping) and the **dynamic AABB tree** broadphase, with a SoA / SIMD-friendly data
   layout. Best-in-class + feature-complete + keeps single-source-of-truth integration that
   vendored Box2D can't do (Box2D insists on its own static-fixture world).
2. **Per-platform self-consistent determinism** for M6 (same binary + same input sequence →
   identical state hash). The engine-arch's "self-consistency first" gate; enough for
   record/replay, the harness, and single-authority server validation. Cross-platform
   bit-determinism (now achievable in float, no fixed-point) is a **later opt-in** if
   lockstep/rollback netcode becomes a goal — the algorithm choices below keep that door
   open (deterministic reduction order, no fast-math) rather than baking it in now.
3. **Full engine, phased within M6.** Three internal phases (below). "Feature complete to a
   T": every Lua-engine capability ports, the solver/broadphase modernize, and the new
   Astra/server integration is built.

**The tension-dissolving insight:** the overworld today uses **only the kinematic/geometric
layer** (the slide `CharacterController` + queries + `TileGrid`) — all deterministic and
bit-matchable to the Lua `physics_harness` oracle. **No current gameplay uses the dynamics
solver.** So the geometric/kinematic core is a *faithful* port (bit-parity where the game
depends on it), while the dynamics solver is *modernized* to Soft Step (behaviorally +
self-consistency validated, since nothing requires it to bit-match the old Baumgarte path).
The "port faithfully vs modernize" conflict simply doesn't bind.

## Goals

1. A presentation-free `Arcane::Physics` module: a deterministic fixed-step 2D rigid-body
   engine (`PhysicsWorld`) with the Lua engine's full feature surface, modernized solver and
   broadphase, zero rendering dependency, linkable by both `Arcane.dll` and the server.
2. **Geometric/kinematic core (faithful):** Collider set (Circle/Capsule/AABB/convex
   Polygon), the narrowphase (SAT + specialized circle/capsule tests + GJK
   distance/closest-points/shapeCast), the broadphase interface with **TileGrid**
   (single-source passability) + **dynamic AABB tree** (primary movers) + SpatialHash/SAP
   alternates, the `CharacterController` (WASD slide + click-to-move kinematic), queries
   (queryAABB/raycast/shapeCast/overlapShape), and the `ContactManager` (persistent pairs,
   warm-start, begin/end/stay + sensor events, the two-granularity event gating). This layer
   bit-matches the Lua oracle where gameplay uses it.
3. **Dynamics (modernized):** dynamic bodies, the **Soft Step solver** (soft constraints +
   sub-stepping) behind a swappable `Solver` interface, constraint graph + island extraction
   + sleeping, the full joint set (distance, revolute, prismatic, weld, mouse/target, wheel,
   motor) in the soft-constraint formulation, and **speculative-contact CCD** (modern;
   replaces the Lua engine's conservative-advancement CCD and integrates with sub-stepping).
4. **Astra/engine integration:** physics components reflected into the engine `Registry`
   (`RigidBody2D`, `Collider2D`, …), a `PhysicsSystem` driven in the engine's `fixedUpdate`
   phase via the `RunLoop`'s reserved engine pre-step, body↔entity mapping, and transform
   sync (physics → `LocalTransform`/`WorldTransform`, with prev/curr for the render alpha).
5. **Determinism + the oracle:** fixed 60 Hz step, no wall-clock reads inside `step()`,
   deterministic iteration order, `/fp:precise` + contraction-off; the Lua `physics_harness`
   ports as the C++ oracle — geometric unit tests, broadphase pair-set equivalence, and a
   record→replay state-hash test.

## Non-goals (deferred; captured in Roadmap)

- Cross-platform bit-determinism / lockstep-rollback netcode (a later opt-in; M6 keeps the
  door open but targets per-platform self-consistency).
- 3D physics, soft bodies, cloth, fluids, destruction.
- A full multithreaded solver (the solver is structured for it — island-parallel via the
  `IWorkScheduler` — but M6 ships correct + single-threaded-solve first; broadphase/narrowphase
  parallelism via the scheduler is in scope where trivially safe).
- The Grimoire physics authoring/inspection UI (M7); M6 ships only the presentation-free
  engine + a debug-draw *data* feed (an Arcane render overlay consumes it, separately).
- Combat-Sphere gameplay built on the engine (later); M6 delivers the event-gating *mechanism*
  the Combat Sphere will use, not the gameplay.

---

## Architecture

### Module placement (presentation-free)

The engine lives in a presentation-free module so the server can link the same authoritative
sim (engine-arch decision 9 + the port audit's T3). Two candidate homes:

- **`Arcane.Core` `src/Physics/`** — Core is already the presentation-free static lib both
  the server and `Arcane.dll` link. Physics is math + data; it fits Core's contract
  (presentation-free, no SDL3/NVRHI/audio). **Chosen.** Keeps the "one sim, two flavors"
  property (Core builds /MD into Arcane.dll and static-CRT into the server) without a new
  workspace seam. (If link-time or dependency weight argues against it during planning, the
  fallback is a sibling presentation-free static lib `Arcane.Physics` that links Core; the
  *contract* — presentation-free, dual-flavor — is identical either way.)

Hard rule: the physics module includes **no** SDL3 / NVRHI / Batcher2D / ImGui. Debug
visualization is a *pull* API (`PhysicsWorld` exposes iterables over shapes/contacts/AABBs/
islands); a separate `Arcane/Render/PhysicsDebugDraw` overlay (Arcane.dll side) consumes it
and submits to the Batcher2D. The engine never draws.

### Core model (ported)

- **Collider** (shape): pure immutable geometry, shareable — `Circle`, `Capsule` (segment +
  radius), `AABB`, convex `Polygon` (CCW, ≤ `kMaxPolyVerts` = 128 per the Lua decision; common
  movers stay small, big polys are occasional statics). A `kSkin`/speculative margin is part of
  the shape-query contract (modern speculative-contact support).
- **Fixture**: a collider instance on a body — local transform, material (friction/restitution/
  density), collision filter (category/mask bits), `isSensor` (trigger volumes: overlap events,
  no response). (The Lua engine folded Fixture into Body until multi-fixture need; M6 keeps a
  thin Fixture record so multi-shape bodies and editor introspection are first-class.)
- **Rigidbody** (`Body`): `Static` (never integrated — tile statics + props), `Kinematic`
  (script-driven velocity; pushes dynamics, immune to forces — the path follower), `Dynamic`
  (integrated + solved). State lives in the world's **SoA arrays** (`posX/posY`,
  `prevX/prevY`, `rot/prevRot`, `velX/velY/angVel`, `invMass/invI`, flags…), `f32`/`f64`
  chosen for determinism+perf (see Determinism). A `Body` is a stable handle = packed
  `{index, generation}` (free-list recycle), never a pointer — the SoA-and-handle layout is
  the design center, not a retrofit.
- **`PhysicsWorld`**: `Create(WorldDesc)`, `AddBody/RemoveBody`, `AddJoint/RemoveJoint`,
  `Step(dt)` once per `fixedUpdate`. Query API: `QueryAABB`, `Raycast`, `ShapeCast` (GJK),
  `OverlapShape`. Determinism contract: identical input sequence ⇒ identical state; no
  wall-clock reads inside `Step`.

### `Step()` pipeline (modernized; preserves the Lua ordering, swaps the solver)

```
1. Integrate velocities          (gravity configurable; OFF for the top-down world)
2. Broadphase update -> candidate pairs   (movers via dynamic AABB tree; statics via TileGrid)
3. Narrowphase dispatch -> manifolds      (SAT + specialized tests + speculative margins);
   ContactManager persists pairs (warm-start cache), queues begin/end/stay + sensor events
   under the two-granularity gating (delivered AFTER step)
4. Build constraint graph (bodies=nodes, contacts+joints=edges); extract islands; sleep idle
5. SOLVE: Soft Step -- for each substep: integrate, solve velocity (soft contacts+joints,
   warm-started), relax; then a position/restitution pass. Iteration/substep counts are
   WorldDesc-configurable. (Solver is an interface; a Baumgarte PGS impl is retained for the
   oracle cross-check, see Testing.)
6. CCD: speculative contacts handle most fast movers inline (the skin margin lets the solver
   see contacts before interpenetration); a GJK time-of-impact clamp backs up flagged bullets
   against statics
7. Integrate positions; publish transforms (physics keeps prev/curr; the render boundary lerps
   by the RunLoop alpha)
```

The pipeline shape is the Lua engine's (so the geometric stages bit-match the oracle); steps
5–6 are the modernization (Soft Step replaces Baumgarte; speculative contacts replace
conservative-advancement as the primary CCD).

### Narrowphase

- **SAT** for polygon↔polygon / polygon↔AABB overlap + manifold (clip incident face) — the
  branch-predictable common path; ports faithfully (oracle-matchable).
- **Specialized tests** for circle↔circle/AABB/polygon and capsule↔* (closest-point-on-
  segment) — faithful port.
- **GJK** for distance/closest-points, `ShapeCast`, and the CCD time-of-impact primitive
  (no EPA — SAT/specials own penetration manifolds, matching the Lua decision). Faithful.
- **Speculative margins** (new): manifolds are generated within a skin distance so the Soft
  Step solver resolves contacts a hair before interpenetration — the modern CCD foundation.

### Broadphase (interface; the world composes statics + movers)

`Broadphase` interface: `Insert/Remove/Update(proxy)`, `QueryAABB`, `Pairs()`. Pairs =
(mover↔mover) ∪ (mover↔static). The world composes two instances:

- **`TileGrid` (statics, the keystone):** stores no wall fixtures — it answers straight from
  `Map` cell flags, materializing transient AABB "virtual fixtures" for solid cells near a
  query/mover, with **greedy per-row rectangle merging** to kill the tile-seam internal-edge
  bug. This is the single-source-of-truth mechanism: change a cell flag, physics follows
  instantly (editor live-edit inherits it free). Off-grid props are ordinary static bodies.
  **Vendored Box2D cannot do this** — the reason for first-party. (The engine reads the same
  map JSON families as the client per the data single-source rule; the cell-flag source is a
  `IPassabilitySource` seam so the server and client share it.)
- **Dynamic AABB tree (movers, primary — promoted from the Lua M4 alternate):** binary AABB
  BVH, k·log n insert/query/raycast; fat AABBs + tree rotations for balance. The gold-standard
  mover broadphase (Box2D v3); replaces the Lua engine's `SpatialHash` as the default.
- **`SpatialHash` / `SAP` (alternates):** behind the same interface for profiling; the harness
  runs every scenario across all strategies and asserts **identical pair sets** (the Lua
  engine's equivalence invariant — a strong determinism + correctness check).

### Solver — Soft Step (the modernization centerpiece)

Replaces the Lua engine's Baumgarte `SequentialImpulse`. Per `Step`, the dt is divided into
`substepCount` sub-steps; each sub-step integrates, then solves **soft** velocity constraints
(contacts + joints) Temporal-Gauss-Seidel with warm-starting, then a relax pass; a final pass
applies restitution. Soft constraints are tuned by `(hertz, dampingRatio)` per constraint
class (rigid contacts use a high hertz that, with small sub-steps, appears rigid — the v3
insight). Benefits over Baumgarte (research-backed): stable high mass ratios, long chains,
large stacks; tunable springiness for gameplay; cost ≈ Baumgarte, ≪ NGS. The `Solver`
interface stays (the Lua abstraction) so a Baumgarte PGS impl rides along as the oracle
cross-check and a future XPBD experiment can drop in without touching the world.

### Joints (soft-constraint formulation)

`Joint` base + `Distance`, `Revolute`, `Prismatic`, `Weld`, `Mouse`/target (editor
manipulation), `Wheel`, `Motor` — each a soft constraint solved in the sub-step loop with
warm-starting, optional motors/limits. The Lua engine shipped Distance/Revolute/Prismatic/
Weld/Mouse; M6 adds Wheel + Motor for completeness ("to a T").

### CharacterController (the live consumer — faithful)

- **WASD slide:** kinematic-style swept-capsule slide-move vs statics, two-pass slide along
  contact normals, step-free; deterministic, no solver involvement. Player shape = capsule
  from day one (best corner-slide feel). Bit-matchable to the Lua oracle.
- **Click-to-move:** plain kinematic body; the path follower sets velocity along a cell-safe
  path; physics only reports overlaps (sensors / future combat-contact initiation), never
  deflects it.

### ContactManager + the two-granularity event gating (the Combat-Sphere mechanism — faithful)

Persistent contact pairs with warm-start cache; queues `begin`/`end`/`stay` + sensor events,
delivered after `Step` (no user code mutates mid-solve). **Gating (ported verbatim — the
mechanism Combat Sphere needs):** per-body `eventsEnabled` (mute combat participants when a
Combat Sphere forms while the rest of the world keeps simulating — the Wakfu bystander model)
+ a world-level gate (cutscenes/map swap). Gated events are **dropped, not queued** (replaying
a stale `begin` could fire triggers for contacts that already ended); on re-enable, every
currently-overlapping persistent pair emits a fresh `begin` (level-triggered re-arm).

### Astra / engine integration (the new work vs the Lua port)

- **Components (reflected, in the scene/physics layer):** `RigidBody2D` (type, velocity,
  mass props, flags), `Collider2D` (shape ref + material + filter + isSensor),
  `PhysicsBodyRef` (the stable `Body` handle linking an entity to its world row). All
  `ASTRA_REFLECT` (editor/JSON/snapshot path, hot-reload-survivable — they round-trip through
  the M5 whole-registry snapshot).
- **`PhysicsSystem`** (engine `fixedUpdate` phase): the engine-arch host loop's reserved
  "Arcane pre-step (physics world)" slot. Each fixed step: sync authored component state into
  the `PhysicsWorld` SoA (create/destroy rows for added/removed bodies), `world.Step(fixedDt)`,
  then write back (`posX/posY/rot` → `LocalTransform`; the existing `TransformPropagationSystem`
  then derives `WorldTransform`). Runs BEFORE `TransformPropagationSystem` in the fixedUpdate
  order. The `Body↔Entity` map lives on a `PhysicsResource` (the world + the map).
- **Render alpha:** physics keeps prev/curr; the `RunLoop` already exposes the interpolation
  alpha — the render boundary lerps (no change to the M5 render path).
- **Server reuse:** because the module is presentation-free, the server links the same
  `PhysicsWorld` for authoritative validation (Combat-Sphere-era); the `IPassabilitySource`
  seam lets client + server share cell-flag passability.

---

## Determinism (per-platform self-consistent)

- Fixed 60 Hz `Step`; the `RunLoop` accumulator owns cadence; no wall-clock reads inside
  `Step`. Identical input sequence ⇒ identical state hash on the same binary.
- Deterministic iteration order everywhere: bodies/contacts/joints iterated by stable index;
  broadphase emits pairs in a sorted, strategy-independent order (the Lua "identical pair set
  across strategies" invariant); island extraction is order-stable; solver sub-step/iteration
  order is fixed.
- `/fp:precise`, contraction OFF in the physics TU (engine-arch determinism rule); no
  `-ffast-math`. SIMD is allowed only where it does not reorder reductions in a result-
  affecting way (SoA lane math that is bit-identical to scalar is fine; horizontal reductions
  that change rounding are gated behind the cross-platform-determinism opt-in).
- The door to **cross-platform** bit-determinism stays open (Box2D v3.1 proves float suffices):
  deterministic reductions + careful transcendentals are a later opt-in, not M6 work.

## Testing — the `physics_harness` ports as the C++ oracle

The Lua `physics_harness` (967 lines) is the behavioral spec. It ports into `ArcaneTests`
(Catch2), plus headless physics unit tests:

| Test | Asserts |
|---|---|
| Geometric units (manifolds) | normals/depths from known configs **bit-match** the Lua reference outputs (SAT/specials/circle/capsule are faithful ports) |
| GJK distance / closest-points / shapeCast | match the Lua reference values |
| Raycast / queryAABB / overlapShape | hit fractions/normals match the Lua reference |
| Tile-seam slide | swept capsule glides across merged tile spans (no internal-edge catch) |
| Broadphase pair-set equivalence | dynamic-AABB-tree, SpatialHash, SAP, TileGrid emit **identical pair sets** for the same scene |
| CharacterController (WASD slide) | kinematic slide paths match the Lua reference (the live gameplay layer) |
| Dynamics behavioral | stacks settle, chains hang, mass ratios stable (Soft Step) — NOT bit-matched to Baumgarte; validated by invariants (penetration < eps, energy bounded) |
| Solver A/B cross-check | the retained Baumgarte PGS and Soft Step both satisfy the dynamics invariants on the same scenes (guards against solver bugs) |
| **Determinism replay** | record an input sequence → run twice → identical state hash (per-platform self-consistency); covers the full pipeline incl. dynamics |
| Astra integration | a scene with RigidBody2D/Collider2D entities steps via `PhysicsSystem`; transforms sync to `LocalTransform`; survives an M5 hot-reload snapshot round-trip |

GPU is irrelevant (the engine is presentation-free) — all physics tests are headless. The
debug-draw overlay gets a small `[gpu]` smoke test (shapes/contacts render, `RenderErrorCount()
== 0`) on the Arcane.dll side only.

## Phasing within M6

- **Phase 1 — geometric/kinematic core (the overworld's live need, faithful oracle parity):**
  presentation-free module placement; SoA `PhysicsWorld` + handles; Collider set; SAT +
  specialized narrowphase + GJK distance/shapeCast; broadphase interface + **TileGrid** +
  **dynamic AABB tree** (+ SpatialHash/SAP alternates); `ContactManager` + events + the
  two-granularity gating; `CharacterController` (both modes); queries; the geometric +
  pair-set-equivalence + slide + CharacterController harness tests (bit-match the Lua oracle).
- **Phase 2 — dynamics (modernized):** dynamic bodies; the **Soft Step solver** behind the
  `Solver` interface (+ retained Baumgarte PGS for cross-check); constraint graph + islands +
  sleeping; the full joint set; behavioral + A/B harness tests.
- **Phase 3 — CCD, integration, determinism gate:** speculative-contact CCD + GJK-TOI bullets;
  the Astra components + `PhysicsSystem` + transform sync + `IPassabilitySource`; the
  record→replay determinism harness; the debug-draw overlay (Arcane.dll); presentation-free
  server-link verification (Core builds both flavors against the physics TU).

## Constraints (carried forward)

- Presentation-free physics module (no SDL3/NVRHI/audio/ImGui); dual-flavor (Arcane.dll /MD +
  server static-CRT) via Core. Debug draw is a pull API consumed by a separate Arcane overlay.
- `/fp:precise`, no `-ffast-math`, contraction off in the physics TU; deterministic iteration
  order; no wall-clock in `Step`; fixed 60 Hz via `RunLoop`.
- Every physics component `ASTRA_REFLECT`-annotated (editor/JSON/snapshot/hot-reload path).
- UTF-8 without BOM, ASCII comments, C++23 (Arcane flavor) / C++20 (server flavor of Core).
- Build/run via the M5 toolchain (premake5 vs2026 directly, full MSBuild path + DASH flags,
  run tests from output dir). Never `db-reset`/`clean --deep`/`docker down -v`/`tauri dev`.
  Commit trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## Out of scope (deferred to their own designs)

Cross-platform bit-determinism + lockstep/rollback netcode; multithreaded island-parallel
solve (structured for it, not shipped); 3D / soft bodies / cloth / fluids / destruction;
Grimoire physics authoring UI (M7); Combat-Sphere gameplay (later, on this engine); the
real Aphelyon `Game.dll` client port (the reserved `Game/` slot).
