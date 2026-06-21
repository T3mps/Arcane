# Arcane Physics v2 — EPA + MPR Narrowphase Specialization — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to
> implement this plan task-by-task (fresh implementer + spec + quality review per task). Steps use
> checkbox (`- [ ]`) syntax. This is a BUILD-AGAINST-REFERENCE plan (like Phase A): the implementer
> reads the named standard algorithm (2D EPA / MPR, Box2D-v3 / dyn4j lineage) + the existing Phase A
> `Narrowphase/` code, applies the documented contract, and validates against the **concrete analytic
> test** in each task (hand-derived expected values, NOT captured from the old engine). The tests are
> the gate.

**Goal:** Make the narrowphase general and exact for deep convex overlap by restructuring `Collide()`
into an explicit static type-pair dispatcher and adding EPA (exact deep penetration axis/depth) +
MPR (fast single-point + EPA fallback), closing the deep round-in-polygon centroid-approximation hole.

**Architecture:** `Collide()` becomes a thin dispatcher over named solvers. The deep-overlap of a
round core (circle/capsule) against a polygon is escalated from today's centroid approximation
(`CollideRound`, `Collide.cpp:271-296`) to EPA, which returns the exact nearest-face axis + depth +
witness → a correct 1-point manifold. MPR is a cheap single-point solver (Phase-D particles / future
curved shapes) and EPA's convergence fallback. Poly-poly deep overlap keeps its existing exact
SAT-clip path (it is not routed to EPA).

**Tech Stack:** C++23 (Arcane /MD) + C++20/23 static-CRT (server ArcaneCore); glm; Catch2; premake5
vs2026; msbuild. Reference: standard 2D EPA + MPR (Box2D v3 / dyn4j / Bullet btGjkEpa lineage) + the
existing Phase A `Narrowphase/{Gjk,Collide}`.

---

## Design notes (read before Task 1)

**Spec:** `docs/superpowers/specs/2026-06-20-arcane-physics-v2-epa-mpr-narrowphase-design.md`. Read it
first — it has the locked decisions (generality/routing-first; static type-pair dispatch with a
GJK→EPA general cell; EPA = exact deep manifold, MPR = single-point + EPA fallback) and the algorithm
boundary table (Sutherland-Hodgman for contact clipping; GJK conservative-advancement for CCD; EPA/MPR
add only the deep-penetration axis — no new clip code).

**Engine philosophy (IMPORTANT — read this):** Arcane is a growing best-in-class engine, NOT a port
frozen to a reference. **Correctness and quality come first.** We do NOT treat "matches the old
numbers" as a goal. Where this plan says a path is "unchanged" or asks you to confirm an existing
test still passes, that is a **free regression tripwire** — a check that a change meant to be confined
to a new path did not *accidentally* leak — NOT a constraint forbidding improvement. The poly-poly
SAT path is simply not routed to EPA, so its tests should still pass; if they don't, the dispatcher
restructure leaked and you fix the leak. Nothing here should stop you from improving a path on
purpose (and re-baselining its tests on purpose) if you find a better formulation while you are in there.

**Build-against-reference methodology (per task):** read the named standard algorithm + the existing
Phase A file, write the structure in C++ honoring the contracts below, and write the analytic test that
asserts the documented behavior (hand-derived expected values — NOT captured from the old engine). The
plan describes EPA/MPR at algorithm+contract altitude (data structures, invariants, determinism rules,
analytic gates) rather than transcribing every line — re-deriving a standard algorithm line-by-line
here would be error-prone; the analytic test is the real gate.

**Contracts every task preserves (the Phase A cross-cutting rules):**
1. **Presentation-free Core:** `Arcane/Core/src/Arcane/Physics/**` includes only glm + std + sibling
   Physics headers. No SDL3/NVRHI/Batcher2D/ImGui/Astra. Compiles BOTH /MD (Arcane.dll) and
   static-CRT (server ArcaneCore).
2. **SoA + handles; zero steady-state heap in Step:** EPA/MPR use fixed-size stack scratch (bounded
   polytope), no per-call heap allocation.
3. **Determinism (f32 per-platform):** fixed 60 Hz, iterate by stable index, no wall-clock, no
   fast-math (`/fp:strict` already on Core from Phase A T9). EPA/MPR carry internal arithmetic in
   **f64**, narrowing to `Real` on return (the existing GJK simplex pattern). Fixed iteration +
   polytope-size caps + a fixed convergence epsilon + deterministic edge selection (priority by
   distance, fixed index tie-break).

**Current code facts (verified):**
- `Collide(a, xfA, b, xfB, specMargin)` (`Collide.cpp:336`) dispatches: `na<=2 || nb<=2` →
  `CollideRound` (the round fast path; its deep branch, lines 271-296, is the centroid approximation
  this workstream fixes). Both `>=3` → GJK then SAT reference-face clip (`Collide.cpp:404-590`, exact).
- `GjkDistanceCore(va,na,vb,nb) -> GjkCoreResult{distance,pointA,pointB,featureA,featureB}`
  (`Gjk.hpp:202`) returns early with `distance==0` and NO simplex when the origin is enclosed
  (`Gjk.cpp:329`). The internal simplex carries, per vertex: MD point (`smx/smy`), A witness
  (`sax/say`), B witness (`sbx/sby`), and input indices (`sia/sib`). The internal support helper is
  `MdSupport SupportMd(va,na,vb,nb, dx,dy)` (anonymous namespace, `Gjk.cpp:274`) returning
  `{md, pa, pb, ia, ib}`.
- `MakeFeatureId(refIsA, refEdge&0x7FFF, incFeature&0xFFFF)` packs the stable manifold-point id
  (`Collide.cpp:245,393`).

**Build/run (the Phase A toolchain):**
- Branch: `feature/arcane-physics-v2` (current; spec committed at `6891dd2`).
- Regenerate after adding files: `cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026` (GenerateProjects.bat hangs on a `pause`).
- Build: `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (+ `Release`).
- Tests (run FROM the exe dir): `cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]~[gpu]"`.
- **GOTCHA:** NEVER run the bare `[gpu]` tag during dev — it can hang on a wedged Vulkan validation
  layer; use `"[physics]~[gpu]"`. If ArcaneTests hangs: `taskkill //F //IM ArcaneTests.exe` + rebuild.
- Server-flavor gate: `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Server/Aphelyon.slnx" -t:ArcaneCore -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (+ Release; `VCPKG_ROOT` is set).
- clangd shows false positives in this repo — MSVC is the source of truth.
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

## Task 1 — Expose the GJK primitives EPA/MPR need (support + origin-enclosing simplex)

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/Narrowphase/Gjk.hpp` / `Gjk.cpp`
- Test: `Arcane/Tests/src/PhysicsGjkSeedTest.cpp` (new; `[physics]`)

**What:** EPA and MPR both need (a) the Minkowski-difference **support** function over two world cores,
and (b) the terminal **origin-enclosing simplex** GJK builds when the cores overlap (EPA's seed
polytope). Both already exist inside `Gjk.cpp` but are not exposed. Add two ADDITIVE public entries
that reuse the existing internals; do not change `GjkDistanceCore` or any existing GJK output.

Add to `Gjk.hpp` (namespace `Arcane::Physics`):
```cpp
// Minkowski-difference support over two world cores: the MD boundary point
// farthest along `dir`, plus its support points on A and B and their indices.
struct MdSupport
{
    Vec2 md{ Real(0), Real(0) };  // support on A minus support on B
    Vec2 pa{ Real(0), Real(0) };  // support point on A's core (+dir)
    Vec2 pb{ Real(0), Real(0) };  // support point on B's core (-dir)
    int  ia = 0;                  // A vertex index
    int  ib = 0;                  // B vertex index
};
[[nodiscard]] MdSupport SupportMinkowski(const Vec2* va, int na,
                                         const Vec2* vb, int nb, Vec2 dir);

// One vertex of the GJK simplex (MD point + the A/B support pair that produced it).
struct GjkSimplexVertex
{
    Vec2 md{ Real(0), Real(0) };
    Vec2 wa{ Real(0), Real(0) };
    Vec2 wb{ Real(0), Real(0) };
    int  ia = 0;
    int  ib = 0;
};

// The terminal GJK simplex. enclosesOrigin == true means the cores OVERLAP and
// v[0..2] form a triangle containing the origin (EPA's seed). enclosesOrigin ==
// false means the cores are separated (count/v describe the closest simplex).
struct GjkSimplex
{
    GjkSimplexVertex v[3];
    int  count = 0;             // 1..3
    bool enclosesOrigin = false;
};

// Run GJK on two world cores and return the terminal simplex. On overlap the
// returned triangle encloses the origin (EPA seed). f64 internal (same as
// GjkDistanceCore); deterministic; ADDITIVE (existing GJK entries unchanged).
[[nodiscard]] GjkSimplex GjkOriginSimplex(const Vec2* va, int na,
                                          const Vec2* vb, int nb);
```

Implementation: promote the existing anonymous-namespace `MdSupport`/`SupportMd` to back
`SupportMinkowski` (same math: argmax over A along +dir, over B along -dir, `md = pa - pb`).
`GjkOriginSimplex` runs the SAME simplex loop as `GjkDistanceCore` but, at the `n==3` origin-inside
branch (`Gjk.cpp:326`), instead of returning `distance 0`, it fills `GjkSimplex{ v=the 3 terminal
MD verts, count=3, enclosesOrigin=true }`; on convergence without enclosing the origin it returns
the closest simplex with `enclosesOrigin=false`. Carry f64 internally, narrow to `Real` on return.

- [ ] **Step 1: Write the failing analytic test.** In `PhysicsGjkSeedTest.cpp`. (To build world core
  vertex arrays, copy the small test-local `RotateCoreVerts(shape, xf) -> std::vector<Vec2>` helper
  pattern from `PhysicsGjkV2Test.cpp:46-54` — it rotates+translates `shape.verts`; `RotateInto` is
  private to `Collide.cpp` and not usable from tests. For axis-aligned boxes you may also hand-write
  the 4 world corners directly.)
  (a) `SupportMinkowski` of two `MakeAabb(5,5)` cores at centers
  (0,0) and (3,0) (overlapping) along `dir=(1,0)` → `md.x` = (max A vert x = +5) − (min B vert x,
  i.e. B support along −x = 3−5 = −2) = 5 − (−2) = 7; assert `md.x ≈ 7` within 1e-4.
  (b) `GjkOriginSimplex` for those overlapping boxes → `enclosesOrigin == true`, `count == 3`, and
  the triangle `v[0].md, v[1].md, v[2].md` contains the origin (assert via a sign-of-cross-product
  winding test you write inline).
  (c) `GjkOriginSimplex` for two `MakeAabb(5,5)` at (0,0) and (20,0) (separated) → `enclosesOrigin
  == false`.
- [ ] **Step 2: Run it, verify it fails** (entries not declared): `... ArcaneTests.exe "[physics] PhysicsGjkSeed*"` → FAIL (compile/link).
- [ ] **Step 3: Implement** `SupportMinkowski` + `GjkOriginSimplex` (reuse `SupportMd`; mirror the
  `GjkDistanceCore` loop; fill the simplex at the origin-inside branch). Do NOT touch `GjkDistanceCore`.
- [ ] **Step 4: Regenerate + build + run** → `PhysicsGjkSeed*` PASS; full `[physics]~[gpu]` green
  (existing GJK entries untouched, so the existing GJK/manifold tests still pass — the tripwire that
  the addition didn't leak).
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 EPA seam -- expose GJK support + origin-enclosing simplex`).

---

## Task 2 — EPA solver (exact deep convex penetration)

**Files:**
- Create: `Arcane/Core/src/Arcane/Physics/Narrowphase/Epa.hpp` / `Epa.cpp`
- Test: `Arcane/Tests/src/PhysicsEpaTest.cpp` (new; `[physics]`)

**What:** Standard 2D EPA over two world cores. Seeded by `GjkOriginSimplex` (Task 1); expands a
convex polytope of MD boundary points until the closest edge to the origin stops moving, then returns
the penetration axis + depth + the witness pair. Reference: standard 2D EPA (Box2D v3 / dyn4j
`Epa.java` / Bullet `btGjkEpa` 2D analog).

`Epa.hpp` (namespace `Arcane::Physics`):
```cpp
// EPA result: the exact minimum-penetration data for two OVERLAPPING convex cores.
//   ok      : false if the inputs were not actually overlapping (caller error) or
//             EPA failed to converge within caps -> caller falls back to MPR.
//   normal  : unit penetration axis, B -> A (the direction to push A out of B).
//   depth   : penetration depth along `normal` (>= 0).
//   witnessA: witness point on A's core surface (radius NOT applied; caller adds rA).
//   witnessB: witness point on B's core surface (radius NOT applied; caller adds rB).
struct EpaResult
{
    bool ok = false;
    Vec2 normal{ Real(0), Real(0) };
    Real depth = Real(0);
    Vec2 witnessA{ Real(0), Real(0) };
    Vec2 witnessB{ Real(0), Real(0) };
};

// Exact deep-overlap penetration via EPA on two world cores (radii NOT applied).
// Deterministic: fixed max iterations + max polytope size + fixed epsilon.
[[nodiscard]] EpaResult Epa(const Vec2* va, int na, const Vec2* vb, int nb);
```

Algorithm (honor exactly): seed the polytope with the 3 `GjkOriginSimplex` MD verts (ensure CCW
winding); each iteration find the polytope **edge closest to the origin** (perpendicular distance;
deterministic tie-break = lowest edge index), compute `SupportMinkowski` along that edge's outward
normal, and if the support's distance along the normal exceeds the edge distance by more than
`kEpaEps` insert the new vertex (splitting that edge), else CONVERGED → that edge's outward normal is
`normal`, its origin-distance is `depth`. Witnesses: for the converged edge `(i, i+1)` with the
origin's closest point at barycentric `t`, `witnessA = lerp(v[i].wa, v[i+1].wa, t)` and `witnessB =
lerp(v[i].wb, v[i+1].wb, t)`. Caps: `kEpaMaxIters` (e.g. 32) and `kEpaMaxVerts` (e.g. 32) → on
overflow/non-convergence return `ok=false`. f64 internal polytope; narrow on return. Fixed-size stack
arrays (no heap). Define `kEpaEps`/`kEpaMaxIters`/`kEpaMaxVerts` as named `constexpr` in `Epa.cpp`.

- [ ] **Step 1: Write the failing analytic test.** In `PhysicsEpaTest.cpp` (build world core vertex
  arrays with the test-local `RotateCoreVerts` helper pattern from `PhysicsGjkV2Test.cpp:46-54`, or
  hand-written corners for axis-aligned boxes):
  (a) **Circle-core deep in box (THE gap case):** A = circle core (1 vert at center) at (5,1),
  B = `MakeAabb(6,6)` at origin. Nearest face is B's +x face (center-to-face dist 6−5 = 1, vs +y
  dist 6−1 = 5). Expect `normal ≈ (1,0)` (B→A, push circle out +x) within 1e-3, `depth ≈ 1` within
  1e-3 (core depth; radius added by the caller later), `witnessB ≈ (6,1)` (on the +x face). NOTE the
  discriminator: the OLD centroid approximation would give `normal = normalize(5,1) ≈ (0.98,0.196)`
  and a wrong depth — assert the exact face normal to prove EPA fixes it.
  (b) **Box deep in box (cross-check vs SAT):** A = `MakeAabb(5,5)` at (8,0), B = `MakeAabb(5,5)` at
  (0,0) → overlap 2 along x; expect `normal ≈ (1,0)`, `depth ≈ 2` within 1e-3 (matches the MTV the
  SAT path already computes — EPA agrees with SAT on the polygon case).
  (c) **Rotated box deep:** B rotated 30° still gives the exact min-penetration axis (hand-derive or
  assert depth == the SAT result for the same config to 1e-3).
  (d) **Determinism:** `Epa` on the same inputs twice → bit-identical `EpaResult` (exact ==).
- [ ] **Step 2: Run it, verify it fails** (`Epa` undeclared): `... "[physics] PhysicsEpa*"` → FAIL.
- [ ] **Step 3: Implement** `Epa.{hpp,cpp}` per the algorithm + contract above (seed from
  `GjkOriginSimplex`, expand, converge, interpolate witnesses, caps).
- [ ] **Step 4: Regenerate + build + run** → `PhysicsEpa*` PASS; full `[physics]~[gpu]` green (EPA is
  new code, not yet wired into `Collide`).
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 EPA solver (exact deep convex penetration)`).

---

## Task 3 — Restructure Collide as an explicit dispatcher + wire EPA into the deep-round cell

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/Narrowphase/Collide.cpp`
- Test: `Arcane/Tests/src/PhysicsCollideDeepRoundTest.cpp` (new; `[physics]`)

**What:** Two changes. (1) Make `Collide()`'s top an EXPLICIT static type-pair dispatcher over named
cells (replacing the inline `if (na<=2||nb<=2)` branch with a small, readable category-routing block
that names each cell: round-deep → EPA, round-shallow/separated → existing GJK-witness, poly-deep →
existing SAT-clip, poly-shallow/separated → existing GJK-speculative). The existing solver bodies are
retained and called as named helpers — this is a readability/structure refactor, behavior of the
existing cells unchanged. (2) In the round path (`CollideRound`), REPLACE the deep-overlap centroid
approximation (`Collide.cpp:271-296`, the `g.distance <= 1e-6` branch) with a call to `Epa(...)`:
build the 1-point manifold from EPA's exact `normal` + `depth` + witnesses (surface witnesses =
`witnessA − normal*rA` and `witnessB + normal*rB`; contact point = their midpoint; `separation =
depth + rA + rB` core→surface; stable id from `MakeFeatureId` as the existing round path does). If
`Epa` returns `ok=false`, fall through to a safe behavior (Task 4 wires MPR here; for now keep the
existing centroid result as the temporary fallback so nothing regresses mid-series — leave a `// T4:
MPR fallback` marker).

This is the correctness payload: deep round-in-poly now reports the exact nearest-face normal/depth
instead of the centroid guess.

- [ ] **Step 1: Write the failing analytic test.** In `PhysicsCollideDeepRoundTest.cpp`:
  (a) **The gap fix:** `Collide(MakeCircle(2) at (5,1), MakeAabb(6,6) at (0,0), specMargin=0)` →
  1-point manifold, `normal ≈ (1,0)` (B→A) within 1e-3, `separation ≈ depth+rA+rB`; with the circle
  core depth 1 and r=2 the surface separation ≈ 1+2 = 3 (hand-derive precisely from your convention
  and assert within 1e-3). Contrast in a comment with the old centroid normal ≈ (0.98,0.196).
  (b) **Capsule deep in box** at an angle → normal aligned to the nearest face within 1e-2.
  (c) **Shallow round unchanged:** a circle just touching the box (separated within margin) still
  goes through the GJK-witness path (1 speculative point), proving only the DEEP branch changed.
- [ ] **Step 2: Run it, verify (a)/(b) fail** (centroid normal ≠ face normal).
- [ ] **Step 3: Implement** the explicit dispatcher restructure + the EPA wire in `CollideRound`'s
  deep branch.
- [ ] **Step 4: Regenerate + build + run** → the new deep-round suite PASS. Run full
  `[physics]~[gpu]`: the poly-poly and shallow-round paths are NOT routed to EPA, so their existing
  tests should still pass — if any fails, the dispatcher restructure leaked (fix the leak; do not
  "adjust" expectations). Note: tests that asserted the OLD centroid deep-round behavior (if any)
  SHOULD now change — those are the gap being fixed; re-baseline them to the exact EPA values on
  purpose and say so in the commit.
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 Collide dispatcher + EPA-exact deep round contacts`).

---

## Task 4 — MPR solver (single-point) + EPA convergence fallback

**Files:**
- Create: `Arcane/Core/src/Arcane/Physics/Narrowphase/Mpr.hpp` / `Mpr.cpp`
- Modify: `Arcane/Core/src/Arcane/Physics/Narrowphase/Collide.cpp` (use MPR as EPA's `ok=false` fallback)
- Test: `Arcane/Tests/src/PhysicsMprTest.cpp` (new; `[physics]`)

**What:** Standard 2D MPR (Minkowski Portal Refinement / XenoCollide) over two world cores: build a
portal from an interior point (the MD of the two core centroids) toward the origin, refine until the
portal straddles the origin ray, return the final portal face's normal + depth + single contact point.
Reference: Snethen XenoCollide / dyn4j `MinkowskiPenetrationSolver` (2D). Then wire it as EPA's
fallback in the deep-round cell from Task 3.

`Mpr.hpp` (namespace `Arcane::Physics`):
```cpp
// MPR result: a single-point deep-overlap contact for two convex cores.
//   ok     : false if MPR failed to converge within caps (rare).
//   normal : unit contact normal, B -> A.
//   depth  : penetration depth along `normal` (>= 0).
//   point  : single contact point on the MD boundary, in world space.
struct MprResult
{
    bool ok = false;
    Vec2 normal{ Real(0), Real(0) };
    Real depth = Real(0);
    Vec2 point{ Real(0), Real(0) };
};

// Single-point deep overlap via MPR on two world cores (radii NOT applied).
// Deterministic: fixed max refinement iterations + fixed epsilon. f64 internal.
[[nodiscard]] MprResult Mpr(const Vec2* va, int na, const Vec2* vb, int nb);
```

Algorithm (honor exactly): v0 = MD of the two centroids (interior ray origin); v1,v2 = supports
building the initial portal facing the origin; iterate — find the portal edge facing the origin,
support outward; if no progress within `kMprEps`, converged → `normal` = portal outward normal,
`depth` = origin distance along it, `point` = the portal-face point nearest the origin. Caps:
`kMprMaxIters` (e.g. 32). f64 internal; fixed stack scratch; named `constexpr` epsilons. In
`Collide.cpp`, replace the Task-3 `// T4: MPR fallback` marker: when `Epa` returns `ok=false`, call
`Mpr` and build the same 1-point manifold from its `normal`/`depth`/`point`.

- [ ] **Step 1: Write the failing analytic test.** In `PhysicsMprTest.cpp`:
  (a) **Single-point correctness:** `Mpr` for a circle core at (5,1) vs `MakeAabb(6,6)` at origin →
  `normal ≈ (1,0)` within 1e-2, `depth ≈ 1` within 1e-2 (same deep case as EPA Task 2a).
  (b) **EPA ↔ MPR cross-check:** for several deep pairs (box-in-box, circle-in-box, rotated), the MPR
  and EPA `normal`s agree within 1e-2 AND `mpr.depth <= epa.depth + 1e-3` (EPA is the true minimum;
  MPR must not report a deeper penetration than the exact minimum).
  (c) **Determinism:** `Mpr` twice → bit-identical result.
  (d) **Fallback path:** a deep round contact where `Epa` is forced to `ok=false` (you can add a
  test-only path or construct a near-degenerate case) routes through `Mpr` and still yields a sane
  1-point manifold (normal within 1e-2 of the face). If a forced-fail hook is needed, gate it behind
  a test-only parameter — do NOT add production branches that can't be hit.
- [ ] **Step 2: Run it, verify it fails** (`Mpr` undeclared).
- [ ] **Step 3: Implement** `Mpr.{hpp,cpp}` + wire it as EPA's fallback in `Collide.cpp`.
- [ ] **Step 4: Regenerate + build + run** → `PhysicsMpr*` PASS; full `[physics]~[gpu]` green.
- [ ] **Step 5: Commit** (`feat(arcane): physics-v2 MPR single-point solver + EPA convergence fallback`).

---

## Task 5 — Deep-overlap invariants + determinism + full-suite / dual-flavor gate + final review

**Files:**
- Modify: `Arcane/Tests/src/PhysicsInvariantsTest.cpp` (add a deep-penetration-recovery invariant)
- Modify: `Arcane/Tests/src/PhysicsDeterminismTest.cpp` (add a deep-overlap scene to the run-twice gate)
- Test: full `ArcaneTests` + ArcaneCore build.

**What:** Prove the new deep-overlap path is robust + deterministic in the full pipeline, then gate
the whole workstream green both flavors.

- [ ] **Step 1: Add the deep-penetration-recovery invariant** to `PhysicsInvariantsTest.cpp`
  (tag `[physics][invariant]`): construct a dynamic round body (`MakeCircle`) initialized DEEPLY
  overlapping a static box (spawn it well inside), step ~120 frames, and assert it **recovers
  cleanly** — pushes out to rest with penetration within the solver's settled budget (use the same
  `~0.21`/`< half-extent` style bound the existing invariant cases use; read them) and kinetic energy
  stays bounded (no explosion: assert a sane velocity/energy cap every step). With the old centroid
  approximation a deeply-spawned circle could eject in the wrong direction; the EPA axis makes
  recovery correct.
- [ ] **Step 2: Add a deep-overlap scene to the determinism gate** in `PhysicsDeterminismTest.cpp`:
  add one dynamic round body spawned overlapping a static body to `RunScene`, hashed (position+angle)
  in stable index order after the existing bodies. Confirm the run-twice-identical asserts still hold
  (the new EPA/MPR f64→f32 paths are deterministic). This deliberately changes the determinism hash
  value (new body) — that is an intentional re-baseline (the gate is run-twice identity + `!= 0`, not
  a pinned constant), note it in the commit.
- [ ] **Step 3: Run it, verify** the new invariant + determinism cases pass; `[physics]~[gpu]` green Debug.
- [ ] **Step 4: Full suite, both configs:** build + run the ENTIRE `ArcaneTests` (no filter) Debug AND
  Release, both backends (the run exercises `[gpu]` D3D12+Vulkan), exit 0. Run from the exe dir. (If
  the Debug `[gpu]` runner hangs on the Vulkan layer: `taskkill //F //IM ArcaneTests.exe`, rebuild,
  retry — a healthy driver is needed for this gate, same as Phase A T9.)
- [ ] **Step 5: Server-flavor gate:** msbuild `Aphelyon.slnx -t:ArcaneCore` Debug+Release → 0 errors
  (EPA/MPR compile static-CRT; confirm presentation-free — glm+std+sibling Physics headers only).
- [ ] **Step 6: Commit** (`test(arcane): physics-v2 deep-overlap invariant + determinism + full-suite/dual-flavor gate`),
  then dispatch the final holistic review (the subagent-driven-development final reviewer), then STOP —
  defer push/merge to the user (CI runs on push).

---

## Notes on plan altitude (honest)

Tasks are at algorithm+contract granularity (like the approved Phase A plan) because EPA and MPR are
standard algorithms built against a named reference + the existing Phase A `Narrowphase/` code;
transcribing every line here would be redundant and error-prone. The CONCRETE gate in each task is the
analytic test (hand-derived expected values for the deep cases), which is the real correctness
checkpoint. The TDD micro-steps (failing analytic test → impl against reference → pass → commit) are
executed by the subagent-driven-development implementers.

## Self-review checklist (run before executing)
- **Spec coverage:** static type-pair dispatcher (T3), EPA exact deep penetration (T2, wired T3),
  MPR single-point + EPA fallback (T4), the GJK seed seam (T1), deep-penetration-recovery +
  deep-overlap determinism invariants (T5), full-suite + dual-flavor gate (T5). The deep round-in-poly
  centroid gap is closed (T2 analytic + T3 wire). Sutherland-Hodgman clip reused (no new clip code);
  poly-poly SAT path not routed to EPA. EPA→2-point-clip for future general-convex/curved is
  documented in the spec but NOT built (YAGNI — no current shape exercises it; it becomes a new table
  cell when a curved type lands). ✓
- **Engine philosophy:** byte-identity is framed as a free regression tripwire on untouched paths,
  NOT a goal; deliberate re-baselines (deep-round values, determinism hash) are called out as
  intentional. ✓
- **Contracts** (presentation-free, SoA + fixed stack scratch, determinism f64-internal/f32-return +
  fixed caps) restated + carried per task. ✓
- **Type consistency:** `MdSupport`, `GjkSimplex`/`GjkSimplexVertex`, `GjkOriginSimplex`,
  `SupportMinkowski` (T1) → `EpaResult`/`Epa` (T2) → dispatcher wire (T3) → `MprResult`/`Mpr` (T4) —
  named consistently across tasks. Manifold built via the existing `MakeFeatureId` + surface-witness
  offset convention. ✓
