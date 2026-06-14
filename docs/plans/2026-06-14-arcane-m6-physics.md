# Arcane M6 — 2D Physics Engine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) to implement this plan task-by-task (fresh implementer + spec + quality review
> per task). This is a PORT-AND-MODERNIZE: most tasks port a proven Lua source file to C++,
> so the implementer READS the named `Client/src/physics/*.lua` as the algorithm source of
> truth, applies the documented modernization delta, and validates against the Lua oracle.

**Goal:** Ship Arcane's first-party, presentation-free, per-platform-deterministic 2D physics
engine — porting the implemented Lua engine's full feature surface and game-specific parts
(TileGrid single-source passability, CharacterController, event gating), while modernizing the
solver to Box2D-v3-class **Soft Step** and the mover broadphase to a **dynamic AABB tree**.

**Architecture:** A presentation-free `Arcane/Core/src/Arcane/Physics/` module (no SDL3/NVRHI/
ImGui), SoA `PhysicsWorld` + stable `{index,generation}` body handles, the Lua engine's
`Step()` pipeline (geometric stages port faithfully + bit-match the oracle), with the dynamics
solver swapped to Soft Step (sub-stepped soft constraints) behind a `Solver` interface, plus a
new Astra component/`PhysicsSystem` layer driven by the M5 `RunLoop` fixedUpdate phase. Debug
draw is a pull API consumed by a separate Arcane.dll overlay.

**Tech Stack:** C++23 (Arcane flavor) / C++20 (server flavor of Core); glm; Astra ECS; Catch2;
premake5 vs2026; msbuild. Source-of-truth to port: `Client/src/physics/*.lua` (2,479 lines) +
the `physics_harness` (`Client/src/tests/physics_harness/main.lua`, 967 lines) as the oracle.

---

## Design notes (read before Task 1)

**Spec:** `docs/superpowers/specs/2026-06-14-arcane-m6-physics-design.md`. This plan implements
it. The three resolved forks: first-party port+modernize; per-platform self-consistent
determinism; full engine, phased.

**The port methodology (per task):** the implementer reads the named Lua file(s) +
the spec section, ports the structure/math to C++ with the contracts below, applies any
"MODERNIZE" delta, and writes the test that asserts parity with the Lua oracle (for geometric/
kinematic tasks) or the documented invariant (for modernized dynamics). The Lua engine is the
behavioral spec; do not invent algorithms where the Lua source already encodes the decision.

**Faithful vs modernized (the master split):**
- **Faithful (bit-match the oracle):** shapes, SAT, specialized circle/capsule tests, GJK
  distance/closest/shapeCast, broadphase interface + TileGrid + pair-set equivalence,
  ContactManager + event gating, CharacterController, queries. The overworld uses only this
  layer today.
- **Modernized (behavioral + self-consistency, NOT bit-matched to Lua):** the dynamics solver
  (Soft Step replaces `SequentialImpulse.lua`'s Baumgarte), the primary mover broadphase
  (dynamic AABB tree promoted to default over SpatialHash), CCD (speculative contacts replace
  conservative advancement). A retained Baumgarte PGS impl rides along as an A/B cross-check.

**Cross-cutting contracts (every task honors):**
1. **Presentation-free:** the `Physics/` module includes no SDL3/NVRHI/Batcher2D/ImGui. It
   compiles into Core (built /MD into Arcane.dll AND static-CRT into the server). Debug data is
   a pull API only.
2. **SoA + handles:** body/contact/joint state in SoA arrays on the world; a `Body` is a packed
   `{uint32 index, uint32 generation}` handle over a free-list, never a pointer. Zero
   steady-state allocation in `Step()` after warmup; scratch buffers pooled on the world.
3. **Determinism (per-platform self-consistent):** fixed 60 Hz `Step`; no wall-clock reads in
   `Step`; iterate bodies/contacts/joints/pairs by stable index in a fixed order; broadphase
   emits pairs in a sorted, strategy-independent order; `/fp:precise` + contraction OFF in the
   physics TU (no `-ffast-math`). Identical input ⇒ identical state hash on the same binary.
4. **Reflection:** physics components (`RigidBody2D`/`Collider2D`/`PhysicsBodyRef`) are
   `ASTRA_REFLECT`-annotated (editor/JSON/M5-hot-reload-snapshot path).
5. **glm** for vec2/mat ops (already vendored); `f32` for stored state unless a determinism
   test shows a need for `f64` (decide per the harness; default `f32` to match SIMD-friendliness).

**Module placement detail:** Core currently builds as the `Core` project (Arcane workspace) and
`ArcaneCore` (Server workspace). New files under `Arcane/Core/src/Arcane/Physics/` are picked up
by Core's `src/**` glob in BOTH flavors automatically. Verify the server-flavor (static-CRT,
C++20) compiles the physics TUs too — keep the physics code C++20-clean (no C++23-only features
that the server flavor rejects). If a feature needs C++23, gate it or keep it in an Arcane.dll-
only adapter, not in the presentation-free core.

**Build/run (the M5 toolchain):**
- Branch: `feature/arcane-m6-physics` (already created off main, spec committed).
- Regenerate: `cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026` (directly; GenerateProjects.bat hangs on a pause).
- Build: `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (+ Release).
- Tests (physics are headless; run FROM the output dir): `cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"`.
- **Server-flavor gate (Phase 3):** also build the Server workspace so `ArcaneCore` compiles the physics TUs static-CRT/C++20: `cd Server && GenerateProjects.bat` then msbuild `Aphelyon.slnx` (Core/ArcaneCore only is enough). This proves presentation-free + dual-flavor.
- Commit trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. Never `db-reset`/`clean --deep`/`docker down -v`/`tauri dev`.

**Oracle capture (do this in Task P1.0):** the Lua `physics_harness` produces reference values
(manifold normals/depths, GJK distances, raycast fractions, broadphase pair sets, slide paths,
determinism hashes). Capture them ONCE into a checked-in fixture (`Arcane/Tests/data/physics_oracle/*.json`)
by running the Lua harness in a capture mode, so the C++ tests assert against fixed reference
numbers without needing Lua at test time. (If the harness has no capture mode, add a tiny Lua
script that runs the same scenarios and dumps JSON — read `physics_harness/main.lua` for the
scenarios.)

---

# PHASE 1 — Geometric / kinematic core (the live overworld need; faithful oracle parity)

Each task: read the named Lua file(s) + the spec, port to `Arcane/Core/src/Arcane/Physics/<area>/`,
honor the cross-cutting contracts, write the `[physics]` test asserting Lua-oracle parity, build,
commit. New `.cpp`/`.hpp` under Core's `src/**` glob need a premake regen (no file-list edit).

## Task P1.0 — Module scaffold + math + oracle fixtures
- **Create:** `Arcane/Core/src/Arcane/Physics/PhysicsTypes.hpp` (vec2 alias to glm, `BodyHandle{uint32 index, generation}`, `BodyType` enum, fixed constants `kMaxPolyVerts=128`, `kLinearSlop`, `kSkin`), `Physics/Math.hpp` (cross2/perp/clamp/closest-point-on-segment — port from `Geometry.lua`).
- **Oracle:** capture the Lua harness reference values into `Arcane/Tests/data/physics_oracle/` (see Oracle capture above). Add a Catch2 `[physics]` smoke test that loads one fixture (proves the fixture path + JSON loader work).
- **Determinism note:** pick `f32` vs `f64` for stored state now; document the choice. Default `f32`.
- Build + run `[physics]`; commit.

## Task P1.1 — Shapes (Collider set)
- **Port:** `shapes.lua` → `Physics/Shapes.hpp/.cpp` — `Circle`, `Capsule` (segment+radius), `AABB`, convex `Polygon` (CCW, ≤128, with computed normals/centroid). Immutable, shareable. Each shape: `ComputeAABB(xf)`, `ComputeMass(density)`.
- **Test:** `[physics]` — AABB/mass/centroid for known shapes match `Geometry.lua`-derived oracle values. Commit.

## Task P1.2 — SAT narrowphase + Manifold
- **Port:** `SAT.lua` + `Manifold.lua` → `Physics/Narrowphase/Sat.hpp/.cpp` + `Manifold.hpp` — polygon↔polygon / polygon↔AABB overlap + manifold (clip incident face; up to 2 contact points; normal+depth+points). Add the speculative skin margin (`kSkin`) to manifold generation (MODERNIZE: contacts within skin distance are reported so the solver sees them pre-penetration; faithful otherwise).
- **Test:** manifold normals/depths/points for known configs **bit-match** the oracle. Commit.

## Task P1.3 — Specialized circle/capsule tests
- **Port:** `CircleTests` portion of the Lua narrowphase (`Manifold.lua`/`Geometry.lua`) → `Physics/Narrowphase/Specialized.hpp/.cpp` — circle↔circle/AABB/polygon, capsule↔* via closest-point-on-segment.
- **Test:** manifolds match the oracle for circle/capsule configs. Commit.

## Task P1.4 — GJK (distance / closest points / shapeCast)
- **Port:** `GJK.lua` → `Physics/Narrowphase/Gjk.hpp/.cpp` — distance(shapeA,xfA,shapeB,xfB), closest points, `ShapeCast` (conservative-advancement sweep). No EPA (SAT/specials own penetration, per the Lua decision). This is also the CCD time-of-impact primitive.
- **Test:** GJK distances/closest-points/shapeCast fractions match the oracle. Commit.

## Task P1.5 — Narrowphase dispatch
- **Port:** `Dispatch.lua` → `Physics/Narrowphase/Dispatch.hpp/.cpp` — the shape-pair function table routing to SAT/specialized/GJK. Commit (no standalone test; exercised by P1.x manifold tests + later world tests).

## Task P1.6 — Broadphase interface + dynamic AABB tree (primary movers) + SpatialHash/SAP
- **Port + MODERNIZE:** `Broadphase.lua` interface (`Insert/Remove/Update/QueryAABB/Pairs`) → `Physics/Broadphase/Broadphase.hpp`. Port `AABBTree.lua` → `Physics/Broadphase/DynamicTree.hpp/.cpp` and make it the **default mover broadphase** (MODERNIZE: Lua defaulted to SpatialHash). Port `SpatialHash.lua` + `SAP.lua` as alternates behind the same interface.
- **Test:** for several scenes, all strategies (tree/hash/SAP) emit **identical pair sets** (the Lua equivalence invariant) and match the oracle pair set. Commit.

## Task P1.7 — TileGrid static broadphase (the keystone — single-source passability)
- **Port:** `TileGrid.lua` → `Physics/Broadphase/TileGrid.hpp/.cpp` — answers queries straight from cell flags via an `IPassabilitySource` seam (so client + server share it), materializing transient AABB virtual fixtures for solid cells near a query/mover with **greedy per-row rectangle merging** (kills the tile-seam internal-edge bug). Define `IPassabilitySource` (a `bool IsSolid(int cx, int cy)` + bounds) and a simple in-memory impl for tests.
- **Test:** tile-seam slide — a swept capsule glides across a merged solid span with no internal-edge catch; merged-rect set matches the oracle for a known cell-flag grid. Commit.

## Task P1.8 — PhysicsWorld core + Body SoA + ContactManager + event gating
- **Port:** `PhysicsWorld.lua` (the 621-line orchestration — the big one; may split across 1-2 subtasks) → `Physics/PhysicsWorld.hpp/.cpp` + `Physics/Body.hpp` (SoA accessors over the world arrays) + `Physics/ContactManager.hpp/.cpp`. Implement `Create/AddBody/RemoveBody/Step` and the `Step` pipeline stages 1-3 + 7 (integrate velocities, broadphase update, narrowphase→manifolds, ContactManager persist + warm-start cache + begin/end/stay + sensor events, integrate positions, publish prev/curr). **Port the two-granularity event gating verbatim** (`ContactManager.lua`): per-body `eventsEnabled` + world gate; drop-don't-queue; level-triggered re-arm. NO dynamics solving yet (stages 4-6 are Phase 2).
- **Test:** events (begin/end/stay/sensor) fire deterministically for a scripted kinematic scene; gating drops + re-arms correctly; positions match the oracle for kinematic integration. Commit.

## Task P1.9 — Queries (raycast / queryAABB / overlapShape)
- **Port:** `Cast.lua` + the world query methods → `Physics/Queries` (on `PhysicsWorld`) — `QueryAABB`, `Raycast` (vs movers via tree + statics via TileGrid; returns fraction/point/normal/body), `OverlapShape`, `ShapeCast` (GJK-based, P1.4).
- **Test:** raycast/queryAABB/overlap hit fractions/normals/sets match the oracle. Commit.

## Task P1.10 — CharacterController (WASD slide + click-to-move)
- **Port:** `CharacterController.lua` → `Physics/CharacterController.hpp/.cpp` — WASD swept-capsule two-pass slide vs statics (kinematic, deterministic, step-free); click-to-move plain kinematic follower (sets velocity along a path, never deflected). Player shape = capsule.
- **Test:** slide paths for scripted WASD inputs against a TileGrid **match the Lua oracle** (this is the live gameplay layer — strongest parity requirement). Commit.

## Task P1.11 — Phase 1 harness + full headless green
- Port the geometric/slide/pair-set scenarios from `physics_harness/main.lua` into a `[physics]` suite; confirm all assert against the captured oracle. Build Debug+Release; full `[physics]` green both configs. Commit.

---

# PHASE 2 — Dynamics (modernized: Soft Step solver)

Dynamics are NOT used by current gameplay, so these modernize freely (behavioral + self-
consistency validation, not bit-match-to-Lua). Reference the spec's Solver section + the Box2D
v3 Soft Step material ([Solver2D](https://box2d.org/posts/2024/02/solver2d/),
[Releasing Box2D 3.0](https://box2d.org/posts/2024/08/releasing-box2d-3.0/)) for the algorithm;
read `SequentialImpulse.lua` for the constraint-prep/warm-start structure to reuse.

## Task P2.1 — Dynamic bodies + velocity integration + the Solver interface
- **Files:** extend `PhysicsWorld` for `Dynamic` bodies (mass/invMass/invI, force/torque accumulators, gravity config); `Physics/Solver/Solver.hpp` interface (`PrepareContacts/Joints`, `SolveVelocity(substep)`, `Relax`, `ApplyRestitution`, `SolvePosition`).
- **Test:** a single dynamic body integrates under a constant force deterministically (no solver yet). Commit.

## Task P2.2 — Soft Step solver (the modernization centerpiece)
- **Create:** `Physics/Solver/SoftStep.hpp/.cpp` — sub-step loop: for each of `substepCount` sub-steps, integrate, solve **soft** contact+joint velocity constraints (TGS, warm-started), relax; final restitution pass. Soft constraints tuned by `(hertz, dampingRatio)` per class (rigid contacts use high hertz; small sub-steps make them appear rigid). Wire into `Step` pipeline stage 5.
- **Test:** behavioral invariants — a stack of N boxes settles with penetration < `kLinearSlop`; a long chain hangs stably; high mass-ratio pair is stable; energy is bounded (no blow-up). NOT bit-matched to Lua. Commit.

## Task P2.3 — Baumgarte PGS cross-check solver
- **Port:** `SequentialImpulse.lua` → `Physics/Solver/Baumgarte.hpp/.cpp` (the retained A/B impl behind the `Solver` interface).
- **Test:** both solvers satisfy the P2.2 invariants on the same scenes (guards against solver-specific bugs). Commit.

## Task P2.4 — Constraint graph + islands + sleeping
- **Port:** `Island.lua` → `Physics/Island.hpp/.cpp` — build the constraint graph (bodies=nodes, contacts+joints=edges), extract islands (order-stable), sleep idle islands (linear/angular velocity below threshold for N steps), wake on new contact/force.
- **Test:** islands extract deterministically; an idle island sleeps and stops consuming solve time; a new contact wakes it. Commit.

## Task P2.5 — Joints (soft-constraint formulation)
- **Port + extend:** `Joints.lua` → `Physics/Joints/` — `Joint` base + `Distance`, `Revolute`, `Prismatic`, `Weld`, `Mouse`/target. **Add Wheel + Motor** (MODERNIZE: completeness "to a T"). Each solved in the sub-step loop with warm-starting + optional motors/limits.
- **Test:** each joint satisfies its constraint invariant (distance held, revolute angle/limits, prismatic axis, weld rigid, mouse tracks target, wheel suspension, motor drives) under the Soft Step solver. Commit.

## Task P2.6 — Phase 2 harness green
- Port the dynamics scenarios from `physics_harness/main.lua`; full `[physics]` green Debug+Release. Commit.

---

# PHASE 3 — CCD, Astra integration, determinism gate, server-link

## Task P3.1 — Speculative-contact CCD + GJK-TOI bullets
- **MODERNIZE:** speculative contacts (the P1.2 skin margin) handle most fast movers inline in the Soft Step solver; add a GJK time-of-impact clamp (P1.4) for `isBullet` bodies vs statics (`CCD.lua` is the conservative-advancement reference for the TOI primitive). Wire into `Step` stage 6.
- **Test:** a fast body does not tunnel through a thin static wall (speculative + TOI); deterministic. Commit.

## Task P3.2 — Astra physics components
- **Create (in the scene/physics layer, ASTRA_REFLECT):** `Arcane/Arcane/src/Arcane/Scene/PhysicsComponents.hpp` — `RigidBody2D` (type/velocity/mass/flags), `Collider2D` (shape ref + material + filter + isSensor), `PhysicsBodyRef` (the `BodyHandle`). All reflected.
- **Test:** components register + reflect (visitFields non-null); round-trip through an M5 whole-registry snapshot (Save/Load). Commit.

## Task P3.3 — PhysicsSystem + transform sync + PhysicsResource
- **Create:** `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp/.cpp` — a `fixedUpdate` system (registered BEFORE `TransformPropagationSystem`): syncs authored `RigidBody2D`/`Collider2D` into the `PhysicsWorld` SoA (create/destroy rows on add/remove), `world.Step(fixedDt)`, writes body pose → `LocalTransform`. A `PhysicsResource` holds the `PhysicsWorld` + the `BodyHandle↔Entity` map. (This fills the engine-arch host loop's reserved "Arcane pre-step".)
- **Test:** a scene of dynamic + kinematic entities steps via `PhysicsSystem`; `LocalTransform` updates; `TransformPropagationSystem` then derives `WorldTransform`; deterministic. Commit.

## Task P3.4 — Determinism replay harness
- **Test:** record an input/force sequence, run the full pipeline (incl. dynamics) twice on the same binary, assert **identical state hash** (per-platform self-consistency). Cover kinematic + dynamic + joints. Commit.

## Task P3.5 — Server-flavor (presentation-free) gate
- Build the Server workspace so `ArcaneCore` compiles the physics TUs static-CRT / C++20 (the dual-flavor proof). Fix any C++23-only or presentation leak. Run an existing server build target to confirm. Commit any fixes.

## Task P3.6 — Debug-draw overlay (Arcane.dll side; pull API consumer)
- **Create:** `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp/.cpp` — consumes `PhysicsWorld`'s pull iterables (shapes/contacts/AABBs/islands) and submits to the Batcher2D (colors by island). Port `PhysicsDebug.lua`/`ColliderDebug.lua` visuals.
- **Test:** `[gpu]` smoke — overlay renders a small scene, `RenderErrorCount()==0`, both backends. Commit.

## Task P3.7 — Full-suite green + finish
- Full ArcaneTests Debug+Release (headless `[physics]` + `[gpu]`), exit 0 both configs; server-flavor Core builds; push branch; update memory (`project_arcane_m6_complete`, repoint next-milestone to M7). Final code review. Defer merge to the user after CI.

---

## Notes on plan altitude (honest)
Phase 1 tasks are at file/algorithm granularity because they PORT a proven Lua file — the
implementer reads the Lua source for the exact algorithm, so re-deriving every line here would
be redundant and error-prone. Phases 2-3 carry the modernization deltas + the engine-specific
integration. The per-task TDD micro-steps (failing test → impl → pass → commit) are executed by
the subagent-driven-development implementers. If Phase 2's Soft Step tuning or Phase 3's CCD
reveal sub-tasks worth their own detail once Phase 1's API is concrete, expand them via
writing-plans at that point — front-loading Phase 1 is deliberate (later-phase details depend on
the realized core API + profiling), not a shortcut.

## Self-review checklist (run before executing)
- **Spec coverage:** module placement (P1.0), shapes/SAT/specialized/GJK/dispatch (P1.1-5),
  broadphase incl. TileGrid keystone (P1.6-7), world+ContactManager+gating (P1.8), queries
  (P1.9), CharacterController (P1.10); Soft Step solver + Baumgarte A/B + islands + joints
  (P2.1-6); speculative CCD (P3.1), Astra components + PhysicsSystem + transform sync (P3.2-3),
  determinism harness (P3.4), server-flavor gate (P3.5), debug overlay (P3.6). All spec Goals + the
  faithful/modernized split + the oracle covered. ✓
- **Determinism + presentation-free contracts** restated in design notes + carried per task. ✓
- **No invented algorithms where the Lua source decides** — each port task names its Lua source. ✓
