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

## Task 4 — EPA fallback for deep core overlap

**Files:**
- Create: `Arcane/Core/src/Arcane/Physics/Narrowphase/Epa.hpp` / `Epa.cpp`
- Modify: `Collide.cpp` (route deep-overlap cases that SAT can't resolve to EPA)
- Test: `Arcane/Tests/src/PhysicsEpaTest.cpp` (new; `[physics]`)

**What:** EPA (Expanding Polytope Algorithm) over the Minkowski difference of two convex cores,
returning the minimum-translation normal + depth for DEEP overlap where the SAT polygon-axis set is
insufficient (general convex-convex). Bounded iteration count with explicit termination + a max-
iteration guard (determinism: fixed iteration order, no wall-clock). For polygon-polygon, SAT
already gives the answer cheaply; EPA is the general fallback. Reference: standard EPA (Box2D v3
does not ship EPA, so use the canonical EPA over the GJK simplex — see Gino van den Bergen / the
common 2D EPA: expand the simplex polygon toward the origin along the closest edge until
convergence). Circle/capsule never reach EPA (segment-distance fast path).

- [ ] **Step 1: Write the failing analytic test.** Two `MakeAabb(10,10)` cores overlapping deeply
  (centers (0,0) and (5,0)) → MTV normal (1,0) or (−1,0) (axis of min penetration = x), depth =
  20 − 5 = 15 within 1e-3; a rotated-polygon deep overlap with a hand-computed MTV; assert EPA
  terminates within the iteration cap.
- [ ] **Step 2: Run, verify it fails.**
- [ ] **Step 3: Implement** EPA + the `Collide` routing for deep overlap.
- [ ] **Step 4: Build + run** → PASS; full `[physics]` green.
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 T4 EPA deep-overlap fallback`).

---

## Task 5 — Wire the world to Collide + feed real body angle (the integration)

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (`GenerateContacts`, `SlotAabb`,
  `ComputeAABB` call sites, `BulletSweep`/query transforms) — replace every `Transform{pos, Real(0)}`
  with `Transform{pos, m_angle[i]}`; route contact generation through `Collide`.
- Test: `Arcane/Tests/src/PhysicsRotationTest.cpp` (new; `[physics]`)

**What:** The headline integration. `GenerateContacts` builds each transform with the body's real
`m_angle` and calls `Collide`; `SlotAabb`/`ComputeAABB` use real angle (rotation-aware bounds drive
the broadphase). `fixedRotation` bodies keep `invInertia=0` (solver won't spin them) and their
constant angle flows through naturally. Queries/`BulletSweep` transforms also pass real angle.

- [ ] **Step 1: Write the failing integration tests.** (a) **Box-dropped-at-45° settles flat:** a
  dynamic `MakeAabb(10,10)` released at angle 0.6 rad above a static floor, gravity on, ~120 steps
  → final |angle mod (π/2)| ≈ 0 within 0.05 (it rotates to rest on a flat face — IMPOSSIBLE with
  fixedRotation collision). (b) **Spinning-body manifold normal correct:** a rotated box resting in
  a corner has the contact normal aligned to the rotated face within 1e-2. (c) **fixedRotation
  lock:** the same box with `fixedRotation=true` + an off-center impulse → angle stays 0 (within
  1e-6) while it translates. (d) **warm-start persistence:** a box resting (small rotation drift)
  keeps the solver warm-start cache matching by feature id (cache size bounded; penetration < slop
  steady). (e) **determinism:** run (a) twice → identical final pose (exact ==).
- [ ] **Step 2: Run, verify (a)/(b)/(c) fail** (angle still ignored in contact gen).
- [ ] **Step 3: Implement** the real-angle wiring + `Collide` routing in the world.
- [ ] **Step 4: Build + run** the rotation suite → PASS. Run full `[physics]`: the BEHAVIORAL
  tests (solver stacking, determinism, CCD no-tunnel, island sleep) must stay green; the OLD
  oracle-bit-match tests (manifold/shapes/gjk/specialized/tilegrid/queries pinned to
  `physics_oracle/*.json`) will now FAIL where rotation/manifold changed — that is EXPECTED and
  Task 6 retires them. Note which fail; do not chase oracle parity.
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 T5 rotation-aware contact gen + AABB (box settles flat)`).

---

## Task 6 — Strangle the old narrowphase + retire the oracle gate; consolidate invariants

**Files:**
- Delete: `Narrowphase/Specialized.{hpp,cpp}`, `Narrowphase/Sat.{hpp,cpp}` and
  `Narrowphase/Dispatch.{hpp,cpp}` ONLY IF fully subsumed by `Collide`/`Epa` (otherwise fold their
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
`Collide`/`Epa` as the sole narrowphase and the invariant suite + analytic V2 tests as the gate.

- [ ] **Step 1:** Grep for callers of the old `Dispatch`/`Specialized`/`Manifold::CollidePolygons`
  and `physics_oracle/` fixtures; confirm `Collide` is the only live collision path
  (`GenerateContacts`, queries, `BulletSweep`). List what's dead.
- [ ] **Step 2:** Delete the dead narrowphase files + the oracle-bit-match test files; repurpose the
  capture program to write feel-reference traces; consolidate `PhysicsInvariantsTest.cpp`.
- [ ] **Step 3: Regenerate + build + run** full `[physics]` → green (only invariant + V2 analytic +
  behavioral tests remain; no oracle-pinned failures). Run `[gpu][physics]` (debug overlay) → green.
- [ ] **Step 4:** Verify the dead files are gone and nothing includes them (`grep -r Specialized\\.hpp`).
- [ ] **Step 5: Commit** (`refactor(arcane): physics-v2 T6 strangle old narrowphase + retire oracle gate`).

---

## Task 7 — fp_contract(off) + dual-flavor gate + full-suite green + new goldens

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
- [ ] **Step 5: Commit** (`build(arcane): physics-v2 T7 fp_contract(off) + dual-flavor + full-suite green`),
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
  feature ids (T3), EPA (T4), real-angle integration + rotation lock + box-settles-flat (T5),
  strangle old narrowphase + retire oracle + archive Lua feel traces (T6), fp_contract + dual-flavor
  + full green + new goldens (T7). All spec architecture items + the re-baseline + the determinism
  addition covered. TileGrid untouched (only its oracle-pinned TEST assertions re-baselined). ✓
- **Contracts** (presentation-free, SoA+handles, determinism, dual-flavor) restated + carried per
  task. ✓
- **No oracle bit-match introduced** — every test is analytic/invariant/golden, not
  `physics_oracle/*.json`. ✓
- **Type consistency:** `Collide(a,xfA,b,xfB,specMargin)->Manifold`, `ManifoldPoint.id` (stable
  feature key), EPA returns MTV normal+depth — named consistently across T2–T7. ✓
