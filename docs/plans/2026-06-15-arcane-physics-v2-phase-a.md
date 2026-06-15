# Arcane Physics v2 — Phase A (Rotation-Aware Rigid-Body Core) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to
> implement this plan task-by-task (fresh implementer + spec + quality review per task). Steps
> use checkbox (`- [ ]`) syntax. This is a BUILD-AGAINST-REFERENCE plan: unlike M6 (which ported a
> Lua oracle), Phase A goes *beyond* the oracle, so the implementer READS the named **Box2D v3**
> algorithm reference + the existing **M6 `Arcane/Core/src/Arcane/Physics/`** code as the starting
> point, applies the documented contract, and validates against the **concrete analytic test** in
> each task (NOT oracle-derived values). The tests are the gate.

**Goal:** Make the Arcane physics narrowphase rotation-aware on a unified rounded-polygon shape
model with stable feature-ID manifolds, retiring the Lua oracle as a gate and re-baselining the
suite on physics invariants + fresh goldens. The solver is already rotation-complete — Phase A is
narrowphase + shapes + manifolds + AABB only.

**Architecture:** One `Shape` = convex polygon core (CCW verts + outward normals + centroid) +
radius. One `Collide(a, xfA, b, xfB, specMargin) -> Manifold` (rotation-aware GJK on cores →
reference-face clip manifold with stable feature ids; SAT/EPA for deep overlap) replaces the
SAT-poly / specialized-round / GJK-dispatch split. `GenerateContacts`/`SlotAabb` feed the body's
real `m_angle`. Solver / ContactManager / Island unchanged.

**Tech Stack:** C++23 (Arcane /MD) + C++20/23 static-CRT (server ArcaneCore); glm; Catch2;
premake5 vs2026; msbuild. Reference: Box2D v3 collision (`distance.c`/`manifold.c`/`b2CollidePolygons`/
EPA) + the existing M6 Physics module.

---

## Design notes (read before Task 1)

**Spec:** `docs/superpowers/specs/2026-06-15-arcane-physics-v2-phase-a-design.md`. Read it first —
it has the three locked decisions (f32 per-platform; replace+re-baseline; this phase = rigid core
only) and the grounding finding (solver already rotation-complete; the `fixedRotation` limit is
purely in contact-gen feeding angle `0` + rank-based manifold ids).

**Build-against-reference methodology (per task):** read the named Box2D v3 function + the existing
M6 file, write the structure in C++ honoring the contracts below, and write the analytic test that
asserts the documented behavior (hand-derived expected values for rotated configs — NOT captured
from the old engine). Do not invent geometry where Box2D v3 already encodes the standard solution.

**Contracts every task preserves (the M6 cross-cutting rules, unchanged):**
1. **Presentation-free Core:** `Arcane/Core/src/Arcane/Physics/**` includes only glm + std +
   sibling Physics headers. No SDL3/NVRHI/Batcher2D/ImGui/Astra. Compiles BOTH /MD (Arcane.dll) and
   static-CRT (server ArcaneCore).
2. **SoA + handles:** body state stays SoA over `{index,generation}` handles; zero steady-state
   allocation in `Step` after warmup (pooled scratch).
3. **Determinism (f32, per-platform):** fixed 60 Hz, iterate by stable index, sorted broadphase
   pairs, no wall-clock, no fast-math. Phase A ADDS `fp_contract(off)` (Task 7).
4. **Re-baseline, not bit-match:** the Lua oracle is retired as a gate. New tests assert physics
   INVARIANTS + hand-derived analytic values + fresh golden hashes. Tolerance-based (f32 epsilon),
   so they survive the `fp_contract` change and the manifold rewrite.

**Unified shape model detail:** keep the `ShapeKind` tag as a fast-path HINT (circle/capsule get
the segment-distance fast path), but every shape ALSO carries the core (`verts`+`normals`+`count`)
+ `radius`, and ALL collision routes through the one unified `Collide`. Circle = 1 vert + r;
capsule = 2 verts (±halfLen,0) + r; aabb = 4 verts + r=0; polygon = N verts + r=0. The four
`Make*` constructors are retained.

**Build/run (the M6 toolchain):**
- Branch: `feature/arcane-physics-v2` (already created off the M6 HEAD; spec committed there).
- Regenerate after adding files: `cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026` (GenerateProjects.bat hangs on a `pause`).
- Build: `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (+ Release).
- Tests (run FROM the exe dir): `cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"`.
- Server-flavor gate (Task 7): `cd Server && ../ThirdParty/premake5/premake5.exe vs2026` then msbuild `Aphelyon.slnx -t:ArcaneCore` (VCPKG_ROOT is set).
- clangd shows false positives in this repo — MSVC is the source of truth.
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` (this branch's convention; the user may change it).

---

## Task 1 — Unified shape core + rotation-aware AABB/mass

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/Shapes.hpp` / `Shapes.cpp`
- Test: `Arcane/Tests/src/PhysicsShapesV2Test.cpp` (new; `[physics]`)

**What:** Every `Shape` carries a populated convex core (`verts`, `normals`, `count`, `centroid`)
+ `radius`, for ALL four kinds (circle → 1 vert at local origin, radius r; capsule → 2 verts
(±halfLen, 0), radius r; aabb → 4 corner verts, radius 0; polygon → N verts, radius 0). Keep the
`ShapeKind` tag as a fast-path hint. Make `ComputeAABB(xf)` **rotation-aware** (rotate the core
verts by `xf.rotation`, bound them, then expand by `radius`). `ComputeMass(density)` derives
mass + centroidal inertia from core + radius (circle/capsule closed forms; polygon via the
standard triangle-fan integral; add the radius shell contribution). Reference: Box2D v3
`b2ComputePolygonMass` / `b2MakePolygon` (rounded). The existing kind-dispatched narrowphase keeps
compiling (it still reads `kind`/`halfLen`/`radius`) — this task is additive.

- [ ] **Step 1: Write the failing analytic test.** In `PhysicsShapesV2Test.cpp`, assert hand-derived
  values: (a) a `MakeAabb(10, 5)` rotated 45° has AABB half-extent `(10+5)/√2 ≈ 10.6066` on both
  axes (corner (10,5) rotates to max |x|,|y| = (10·c−5·s, 10·s+5·c) with c=s=0.7071 → x=3.5355,
  but the bounding half-width = max over 4 corners = |10·c|+|5·s| = 7.071+3.536 = 10.6066) — assert
  `aabb.max.x − center.x ≈ 10.6066` within 1e-3; (b) `MakeCircle(4)` AABB is ±4 at any rotation;
  (c) `MakeCapsule(6, 2)` at 90° has AABB half-height 6+2=8, half-width 2; (d) `ComputeMass` for a
  unit-density circle r=2 → mass = π·4 ≈ 12.566, inertia = ½·m·r² = 25.13 within 1e-2; (e) core
  vertex counts are 1/2/4/N for circle/capsule/aabb/polygon.
- [ ] **Step 2: Run it, verify it fails** (rotation-aware AABB / core verts not yet present).
  Run: `... ArcaneTests.exe "[physics] PhysicsShapesV2*"` → FAIL.
- [ ] **Step 3: Implement** the core population in the `Make*` constructors + rotation-aware
  `ComputeAABB` + `ComputeMass` per core+radius (Box2D v3 forms).
- [ ] **Step 4: Regenerate + build + run** the new test → PASS, and full `[physics]` still green
  (the old narrowphase still reads the legacy fields, unchanged).
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 T1 unified shape core + rotation-aware AABB/mass`).

---

## Task 2 — Rotation-aware GJK distance + closest features on cores

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/Narrowphase/Gjk.hpp` / `Gjk.cpp`
- Test: `Arcane/Tests/src/PhysicsGjkV2Test.cpp` (new; `[physics]`)

**What:** A GJK that takes two world-space cores (built by rotating+translating each shape's core
by its `Transform`) and returns: separation distance, closest points, and the **closest features**
(witness simplex → vertex/edge indices on each shape) needed for stable manifold ids in Task 3.
Operate on the cores only (radius added by the caller). Reference: Box2D v3 `b2ShapeDistance`
(`distance.c`) — it returns a `b2DistanceOutput` with the witness simplex / feature indices. Keep
the existing double-internal-simplex precision the M6 GJK used. Rotation enters ONLY via the
caller rotating the core verts; GJK itself is rotation-agnostic.

- [ ] **Step 1: Write the failing analytic test.** Hand-derived: (a) two `MakeAabb(5,5)` cores at
  centers (0,0) and (20,0), the right one rotated 45°, separation along +x = 20 − 5 − (5·√2)
  ≈ 20 − 5 − 7.071 = 7.929 within 1e-3; (b) a circle core (1 vert) vs a box core → distance =
  center-to-box-surface; (c) closest-feature indices identify the correct vertices/edges for a
  known corner-to-edge config; (d) overlapping cores report distance 0 (GJK degenerate → Task 4
  takes over).
- [ ] **Step 2: Run, verify it fails.**
- [ ] **Step 3: Implement** the core-based GJK + feature-index output (port the witness/feature
  bookkeeping from Box2D v3 `distance.c`).
- [ ] **Step 4: Build + run** → PASS; full `[physics]` green (old GJK callers unaffected — add the
  new entry alongside, do not break the M6 `ShapeCast` GJK path yet).
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 T2 rotation-aware GJK + closest features`).

---

## Task 3 — Unified Collide + reference-face clip manifold with stable feature IDs

**Files:**
- Create: `Arcane/Core/src/Arcane/Physics/Narrowphase/Collide.hpp` / `Collide.cpp`
- Modify: `Arcane/Core/src/Arcane/Physics/Narrowphase/Manifold.hpp` (extend `ManifoldPoint.id` to
  the stable-feature encoding documented below; keep the struct otherwise)
- Test: `Arcane/Tests/src/PhysicsManifoldV2Test.cpp` (new; `[physics]`)

**What:** The one narrowphase entry:
`Manifold Collide(const Shape& a, const Transform& xfA, const Shape& b, const Transform& xfB, Real speculativeMargin = 0)`.
Algorithm: rotate both cores to world space; run Task-2 GJK; if core distance ≥ `rA+rB+margin`,
no contact; if within the margin but cores separated, emit a **single speculative point** (the
closest-feature pair, separation = coreDist − rA − rB, negative=gap); if cores overlap (GJK
degenerate), SAT over the polygon axes finds the reference face; then **reference-face clipping**
(clip the incident edge against the reference side planes, keep ≤2 points inside the reference
face, offset each point outward by the radii, separation = signed depth). Emit **stable feature
ids**: pack `(referenceShape:1 bit, referenceEdgeIndex:­~10 bits, incidentVertexIndex:~10 bits)`
into `ManifoldPoint.id` so the SAME physical feature pair yields the SAME id across frames (this
is the warm-start fix). Reference: Box2D v3 `b2CollidePolygons` (`manifold.c`) + its `b2MakeId`
feature-id scheme. Circle/capsule cores (1–2 verts) take the segment-distance fast path (no
clipping needed; the single/double closest points become the manifold, ids from the segment
feature).

- [ ] **Step 1: Write the failing analytic test.** Hand-derived manifolds: (a) `MakeAabb(10,10)`
  at (0,0) vs `MakeAabb(10,10)` at (18,0) overlapping 2 along x → normal ≈ (1,0) (B→A), 2 points
  on the shared face at y=±10 (clipped), separation ≈ 2 each, within 1e-3; (b) the SAME pair with
  B rotated 30° → reference face is A's right face, 2 clipped points, normal = (1,0); (c) circle
  r=4 at (0,0) vs box at (9,0) → 1 point, normal (1,0)... wait sign: normal B→A so box is B →
  normal points from box toward circle = (−1,0); assert the documented convention; (d)
  **feature-id stability:** sweep B's center from x=18 to x=17 in 10 steps (staying face-to-face);
  the two manifold-point ids MUST be identical across all 10 steps (same features) — this is the
  warm-start gate that the old rank-based id FAILED.
- [ ] **Step 2: Run, verify it fails.**
- [ ] **Step 3: Implement** `Collide` + the clip manifold + stable-id packing (Box2D v3 `manifold.c`).
- [ ] **Step 4: Regenerate + build + run** → PASS; full `[physics]` green (Collide is new code, not
  yet wired into the world — old path still active).
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 T3 unified Collide + stable feature-id clip manifold`).

---

## Task 4 — Fixture data model (Core SoA + AddFixture + mass aggregation)

> **EPA is DEFERRED** (user decision): SAT is provably complete + exact for convex polygon cores, so
> EPA is unexercised by the current shape set. A future "narrowphase specialization" workstream
> implements **EPA + MPR** for curved / high-vertex / per-circumstance routing. Not Phase A.

**Design ref:** `docs/superpowers/specs/2026-06-15-arcane-physics-v2-fixture-model-design.md`.

**Files:**
- Create: `Arcane/Core/src/Arcane/Physics/Fixture.hpp` (`FixtureHandle{index,generation}`, `FixtureDef`).
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` / `.cpp` (Fixture SoA + `AddFixture`/`DropFixture` + mass aggregation + `AddBody` back-compat); `Shapes.cpp` (rounded-polygon `ComputeMass`).
- Test: `Arcane/Tests/src/PhysicsFixtureTest.cpp` (new; `[physics]`)

**What:** Introduce the Box2D-style fixture layer ADDITIVELY (the old single-shape contact-gen path
keeps working; T5 switches it over). `FixtureDef = { Shape shape; Vec2 localPos; Real localAngle;
Real density; Real friction; Real restitution; uint32 categoryBits; uint32 maskBits; bool isSensor; }`.
A Fixture SoA on the world (`m_fxShape`, `m_fxLocalPos`, `m_fxLocalAngle`, `m_fxDensity`,
`m_fxFriction`, `m_fxRestitution`, `m_fxFilterCat`, `m_fxFilterMask`, `m_fxSensor`, `m_fxBody`,
`m_fxGen`) over a free-list; each body slot stores its fixture index list. `AddFixture(BodyHandle,
FixtureDef) -> FixtureHandle`; `DropFixture(FixtureHandle)`; `IsValid(FixtureHandle)`. The fixture's
WORLD shape transform = compose(bodyXf, fixtureLocalXf): `worldAngle = bodyAngle + localAngle`,
`worldPos = bodyPos + R(bodyAngle)*localPos`. **Mass aggregation:** body mass/COM/invInertia summed
over fixtures (each `Shape::ComputeMass(density)`, density-weighted centroid → body COM, parallel-axis
inertia about that COM); implement rounded-polygon `ComputeMass` here (the T1 carry-forward).
`fixedRotation` forces body `invInertia=0`. **Back-compat:** `AddBody(BodyDef{shape,material...})`
ALSO creates one fixture from that shape+material, while still populating the legacy single-shape
body fields so the unchanged old `GenerateContacts` keeps working (T5 removes the legacy path). NOT
yet wired into broadphase/contact-gen.

- [ ] **Step 1: Write the failing analytic test.** (a) **Compound mass:** body = `MakeAabb(10,2)` at
  local (0,0) + `MakeCircle(3)` at local (12,0), density 1 → total mass = box (4·10·2·1·... = area
  40) + circle (π·9 ≈ 28.274) = 68.274; body COM = mass-weighted centroid; body inertia = each
  fixture's centroidal inertia parallel-axis'd to the COM, summed — assert mass/COM/inertia within
  1e-2 (hand-derive). (b) **Fixture world transform:** fixture at localPos (5,0), localAngle 0, on a
  body at pos (0,0) angle π/2 → world shape center ≈ (0,5) within 1e-4 (R(π/2)·(5,0)=(0,5)). (c)
  **Back-compat:** `AddBody(BodyDef{ shape=MakeCircle(2), density=1 })` → a body with exactly 1
  fixture, mass ≈ 12.566; full `[physics]` stays green (old path unchanged). (d) **Stale handle:**
  `DropFixture` then `IsValid(staleHandle)==false`; the slot recycles with a bumped generation.
- [ ] **Step 2: Run, verify it fails.**
- [ ] **Step 3: Implement** the Fixture SoA + AddFixture/DropFixture + mass aggregation + rounded-poly ComputeMass + AddBody back-compat.
- [ ] **Step 4: Regenerate + build + run** → PASS; full `[physics]~[gpu]` green (old contact-gen unchanged; fixtures not yet wired). (Debug runner may need `~[gpu]`; see gotchas.)
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 T4 fixture data model + mass aggregation`).

---

## Task 5 — Wire fixture-pair, rotation-aware contact generation (the integration)

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (`GenerateContacts`, `SlotAabb`/AABB call
  sites, `BulletSweep`/query transforms), `ContactManager.{hpp,cpp}`, the broadphase glue.
- Test: `Arcane/Tests/src/PhysicsRotationTest.cpp` (new; `[physics]`)

**What:** The headline integration, fixture-aware. The broadphase now indexes **fixtures** (each
fixture's proxy AABB = the rotation-aware bound of its WORLD shape transform); `GenerateContacts`
iterates the broadphase **fixture pairs** (sorted; same-body + filter-rejected pairs skipped), calls
`Collide(shapeA, worldXfA, shapeB, worldXfB, specMargin)` with the body+fixture composed angle, and
sources friction/restitution from the two fixtures (friction = sqrt(fA·fB), restitution = max). The
`ContactConstraint` still references the two BODY slots (solver unchanged); anchors are from the body
COM. `ContactManager` keys pairs on `(minFixture<<32)|maxFixture`; `DropFixture`/`DropBody` drop the
right pairs. `SlotAabb`/`ComputeAABB` use real angle. **Remove the legacy single-shape contact-gen
path** (now superseded). `fixedRotation` bodies keep `invInertia=0`.

- [ ] **Step 1: Write the failing integration tests.** (a) **Box-dropped-at-45° settles flat:** a
  dynamic `MakeAabb(10,10)` released at angle 0.6 rad above a static floor, gravity on, ~120 steps →
  final |angle mod (π/2)| ≈ 0 within 0.05 (IMPOSSIBLE with fixedRotation collision). (b) **rotated
  manifold normal** aligned to the rotated face within 1e-2. (c) **fixedRotation lock:** off-center
  impulse → angle stays 0 within 1e-6 while it translates. (d) **compound collision:** a body with
  two fixtures (box + offset circle) resting on a floor reports contacts on BOTH fixtures (two
  fixture-pair manifolds). (e) **warm-start persistence** (cache bounded; penetration < slop steady).
  (f) **determinism:** run (a) twice → identical final pose (exact ==).
- [ ] **Step 2: Run, verify (a)/(b)/(c)/(d) fail.**
- [ ] **Step 3: Implement** the fixture broadphase proxies + fixture-pair `GenerateContacts` through
  `Collide` with real angle + ContactManager fixture keys; remove the legacy single-shape path.
- [ ] **Step 4: Build + run** the rotation+compound suite → PASS. Run full `[physics]~[gpu]`: the
  BEHAVIORAL tests (solver stacking, determinism, CCD, island sleep) stay green; the OLD
  oracle-bit-match tests (manifold/shapes/gjk/specialized/tilegrid/queries pinned to
  `physics_oracle/*.json`) now FAIL where rotation/manifold/fixtures changed — EXPECTED; Task 7
  retires them. Note which fail; do not chase oracle parity.
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 T5 fixture-pair rotation-aware contact gen (box settles flat)`).

---

## Task 6 — Astra scene layer: Collider2D fixture list + PhysicsSystem sync

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/PhysicsComponents.hpp` (`Collider2D` → a fixture list;
  material/filter/isSensor move per-fixture; `RigidBody2D` drops material),
  `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp`.
- Test: extend `Arcane/Tests/src/PhysicsComponentsTest.cpp` + `PhysicsSystemTest.cpp`.

**What:** `Collider2D` carries a small list of fixtures, each `{ ShapeKind kind + scalars; Vec2
localPos; Real localAngle; float density/friction/restitution; uint32 categoryBits/maskBits; bool
isSensor }` (still `ASTRA_REFLECT` — reflect the vector-of-struct, or a fixed small array + count if
Astra can't reflect a `std::vector` field; verify against the Astra reflection API). `RigidBody2D`
keeps body-type/velocity/`fixedRotation`/`bullet`/`linearDamping` (material removed). `PhysicsSystem`
create-pass builds the body + each fixture (Core `Shape` from the descriptor via `MakeCircle/Capsule/
Aabb`, placed by local transform) via `AddBody`+`AddFixture`; destroy drops the body (drops its
fixtures). `PhysicsBodyRef` keeps the `BodyHandle`.

- [ ] **Step 1: Write the failing test.** Extend the component round-trip: a `Collider2D` with TWO
  fixtures (box@local0 + circle@offset, different materials) survives an M5 Save/Load with all
  per-fixture fields. Extend the system test: an entity with a 2-fixture `Collider2D` steps via
  `PhysicsSystem`; both fixtures exist in the world; `LocalTransform` updates; determinism run-twice.
- [ ] **Step 2: Run, verify it fails.**
- [ ] **Step 3: Implement** the `Collider2D` fixture list + `PhysicsSystem` multi-fixture sync.
- [ ] **Step 4: Regenerate + build + run** `[physics]~[gpu]` → green (component + system tests pass).
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 T6 Astra Collider2D fixtures + PhysicsSystem sync`).

---

## Task 7 — Strangle the old narrowphase + retire the oracle gate; consolidate invariants

**Files:**
- Delete: `Narrowphase/Specialized.{hpp,cpp}`, `Narrowphase/Sat.{hpp,cpp}` and
  `Narrowphase/Dispatch.{hpp,cpp}` ONLY IF fully subsumed by `Collide` (otherwise fold their
  still-needed kernels into `Collide`/`GeometryKernel` and delete the rest). Remove the old
  `Manifold::CollidePolygons` once no caller remains.
- Delete/repurpose: the oracle-bit-match tests (`PhysicsManifoldTest.cpp`, `PhysicsShapesTest.cpp`,
  `PhysicsGjkTest.cpp`, `PhysicsSpecializedTest.cpp`, and the oracle-pinned portions of
  `PhysicsTileGridTest.cpp`/`PhysicsQueriesTest.cpp`). Keep TileGrid/Queries BEHAVIOR tests
  (re-baselined to analytic/invariant assertions, NOT `physics_oracle/*.json`).
- Modify: `Client/src/tests/physics_oracle_capture/main.lua` — repurpose to emit the **Lua
  feel-reference traces** (overworld slide paths + query results) into
  `Arcane/Tests/data/physics_feel_reference/` for Phase B; it is no longer a C++ gate.
- Create/consolidate: `Arcane/Tests/src/PhysicsInvariantsTest.cpp` — the canonical invariant gate
  (rest penetration < slop, no-tunnel incl. rotating bodies, energy bounded, broadphase-strategy
  invariance, slide-no-catch over merged spans, warm-start cache bounded). Pull existing invariant
  cases here (or tag them `[invariant]`) so the gate is named in one place.

**What:** Remove the now-dead kind-dispatched collision code and the oracle bit-match gate, leaving
`Collide` as the sole narrowphase and the invariant suite + analytic V2 tests as the gate.

- [ ] **Step 1:** Grep for callers of the old `Dispatch`/`Specialized`/`Manifold::CollidePolygons`
  and `physics_oracle/` fixtures; confirm `Collide` is the only live collision path
  (`GenerateContacts`, queries, `BulletSweep`). List what's dead.
- [ ] **Step 2:** Delete the dead narrowphase files + the oracle-bit-match test files; repurpose the
  capture program to write feel-reference traces; consolidate `PhysicsInvariantsTest.cpp`.
- [ ] **Step 3: Regenerate + build + run** full `[physics]` → green (only invariant + V2 analytic +
  behavioral tests remain; no oracle-pinned failures). Run `[gpu][physics]` (debug overlay) → green.
- [ ] **Step 4:** Verify the dead files are gone and nothing includes them (`grep -r Specialized\\.hpp`).
- [ ] **Step 5: Commit** (`refactor(arcane): physics-v2 T7 strangle old narrowphase + retire oracle gate`).

---

## Task 8 — fp_contract(off) + dual-flavor gate + full-suite green + new goldens

**Files:**
- Modify: `Arcane/premake5.lua` (the Core project) and `Server/premake5.lua` (ArcaneCore) — add
  `floatingpoint "Strict"` (premake → `/fp:precise /fp:contract` off; on MSVC also emit
  `buildoptions { "/fp:precise" }` + ensure contraction off via `#pragma float_control` in the hot
  TUs if premake's flag is insufficient — verify the generated vcxproj).
- Modify: `Arcane/Tests/src/PhysicsDeterminismTest.cpp` — re-capture the golden hashes from the v2
  engine; extend the scene with a rotating body so the golden covers rotation.
- Test: full `ArcaneTests` + ArcaneCore build.

**What:** Close the deferred M6 FP item (safe now — goldens are re-captured), prove dual-flavor, and
gate the whole phase green.

- [ ] **Step 1:** Add `floatingpoint "Strict"` to the Core build (both workspaces); regenerate;
  confirm the generated vcxproj carries the FP flag. Rebuild; run `[physics]` — invariant/analytic
  tests stay green (tolerance-based; contraction-off only shifts low bits).
- [ ] **Step 2:** Re-capture `PhysicsDeterminismTest` goldens from the v2 engine (run twice → derive
  the new expected hash; the run-twice-identical + cross-broadphase asserts are the real gate, the
  literal hash is just an anchor). Add a rotating body to the scene + the angle hash term.
- [ ] **Step 3: Full suite, both configs:** build + run the ENTIRE `ArcaneTests` (no filter) Debug
  AND Release, both backends (D3D12+Vulkan), exit 0. Run from the exe dir.
- [ ] **Step 4: Server-flavor gate:** `cd Server && ../ThirdParty/premake5/premake5.exe vs2026`
  then msbuild `Aphelyon.slnx -t:ArcaneCore` Debug+Release — 0 errors (the v2 narrowphase compiles
  static-CRT; confirm no presentation/dialect leak).
- [ ] **Step 5: Commit** (`build(arcane): physics-v2 T8 fp_contract(off) + dual-flavor + full-suite green`),
  then final holistic review (the subagent-driven-development final reviewer), then STOP — defer
  push/merge to the user (CI runs on push).

---

## Notes on plan altitude (honest)

Tasks are at algorithm/file granularity (like the approved M6 plan) because each builds a standard
collision algorithm against a named Box2D-v3 reference + the existing M6 code — re-deriving every
line here would be redundant and error-prone. The CONCRETE part of each task is the analytic test
(hand-derived expected values for rotated configs), which is the actual gate now that the oracle is
retired. The TDD micro-steps (failing analytic test → impl against reference → pass → commit) are
executed by the subagent-driven-development implementers.

## Self-review checklist (run before executing)
- **Spec coverage:** unified shape model (T1), rotation-aware GJK (T2), clip manifold + stable
  feature ids (T3), fixture data model + mass aggregation (T4), fixture-pair rotation-aware contact
  gen + box-settles-flat + compound collision (T5), Astra Collider2D fixtures + PhysicsSystem (T6),
  strangle old narrowphase + retire oracle + archive Lua feel traces (T7), fp_contract + dual-flavor
  + full green + new goldens (T8). All Phase-A spec + fixture-model spec items + the re-baseline +
  the determinism addition covered. EPA deferred (future EPA+MPR workstream). TileGrid untouched
  (only its oracle-pinned TEST assertions re-baselined). ✓
- **Contracts** (presentation-free, SoA+handles, determinism, dual-flavor) restated + carried per
  task. ✓
- **No oracle bit-match introduced** — every test is analytic/invariant/golden, not
  `physics_oracle/*.json`. ✓
- **Type consistency:** `Collide(a,xfA,b,xfB,specMargin)->Manifold`, `ManifoldPoint.id` (stable
  feature key), `FixtureHandle{index,generation}`, `FixtureDef`/`AddFixture` — named consistently
  across T2–T8. ✓
