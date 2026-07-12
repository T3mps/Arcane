# Manifold2D Phase 3a -- standalone cross-platform repo, Windows-first (design)

Date: 2026-07-11. Status: approved direction (user). Phase 3 of the Manifold2D extraction arc,
Part A of two. Prereq: Phase 2 merged to local `main` @16786a6f (Manifold2D lifted into
`ThirdParty/Manifold2D` as a `Manifold2D::` static library with zero Arcane deps).

## Context and goal

The user named the 2D physics engine `Manifold2D` and wants it shipped as an independently-usable
Starworks component (like the ECS lib **Astra**): a developer wanting only 2D physics can take
Manifold2D without Arcane. Phases 1-2 made it dependency-free and lifted it into the Aphelyon
monorepo at `ThirdParty/Manifold2D`. Phase 3 turns it into its **own repository**
(github.com/T3mps/Manifold2D, already created: public, MIT, default branch `main`), mirroring how
Astra is developed (a standalone working copy synced out to GitHub and vendored back into the
monorepo).

**Phase 3 is split into two clean, independently-landable parts:**
- **Part A (THIS spec):** the structural extraction -- stand up the standalone repo, migrate the
  pure physics/geometry tests, get it **Windows-green**, re-vendor into Aphelyon, first GitHub push.
  Scaffolded so Linux is a drop-in.
- **Part B (its own later spec):** the Linux/cross-platform burn-down -- `gmake2` + g++/clang until
  green, enable the Linux CI job. Part A ships the `gmake2` target, the `filter "system:linux"`
  flags, and the portability scaffolding so Part B is "run it and fix what the compilers flag,"
  not a redesign.

**Goal of Part A:** Manifold2D exists as a self-contained, Windows-green repo published to GitHub
and vendored back into Aphelyon, with the physics test suite living in and gating the standalone
repo (not Aphelyon). Behavior stays value-exact (the migrated suite passes with the counts we know).

## Reference model: how Astra works (verified)

`ThirdParty/Astra` is a **synced copy** (NOT a git submodule -- files committed directly into
Aphelyon). Astra's real source-of-truth is a separate standalone working copy `D:\dev\starworks\Astra`;
`scripts/sync_to_github.ps1` robocopies it (`/MIR`, excluding `.git .claude bin bin-int ide .vs`,
`*.user`, `.slnx/.sln`) into a GitHub clone `D:\dev\github\Astra`, which is then committed + pushed.
Astra's `premake5.lua` is a standalone `workspace` (builds `AstraTest`/`AstraBenchmark` via its own
`vendor/GoogleTest`+`GoogleBenchmark`); Aphelyon never `include`s it -- Astra is header-only, so
Aphelyon consumes it purely via `IncludeDir["Astra"]`. The machine convention exists:
`D:\dev\starworks\<Name>` = standalone source, `D:\dev\github\<Name>` = GitHub clone.

**The one thing Astra does not face:** Manifold2D is COMPILED (real `.cpp`), so Aphelyon must
build + link it, not just point an include dir at it. This drives the premake reconciliation (D4).

## Design decisions (locked)

### D1. Topology & workflow -- three snapshot-synced copies

```
D:\dev\starworks\Manifold2D   (NEW)  -- source of truth; future physics dev happens here
        | scripts\sync_to_github.ps1  (robocopy /MIR, excludes .git/build)
        v
D:\dev\github\Manifold2D              -- clone of T3mps/Manifold2D; commit + push  (PUBLISH)
        | scripts\vendor.ps1          (robocopy into Aphelyon)
        v
Aphelyon\ThirdParty\Manifold2D        -- full mirror, like ThirdParty\Astra          (VENDOR)
```

Both downstreams are **file snapshots** (no git-history transfer -- matches Astra). The GitHub repo
already has an initial commit (README + LICENSE); the first sync reconciles it (our README wins;
keep the MIT LICENSE). `vendor.ps1` mirrors `D:\dev\starworks\Manifold2D` -> `ThirdParty\Manifold2D`
excluding `.git .github bin bin-int ide .vs vendor` and generated project files (Aphelyon supplies
its own build wiring -- D4). The vendored copy carries `include/ src/ tests/ premake5.lua README
CLAUDE.md LICENSE .gitignore` (faithful mirror, like `ThirdParty/Astra` carries its `tests/`).

### D2. Standalone repo structure (Astra layout, Manifold2D-adapted)

```
Manifold2D/
  .github/workflows/ci.yml       # Part A: windows-msvc job only. Part B adds the linux job.
  .gitignore  LICENSE (MIT)  README.md  CLAUDE.md
  premake5.lua                   # workspace "Manifold2D" (D4)
  include/Manifold2D/
    Core/{FunctionRef,BitSet,Simd(+3 .inl),WorkScheduler,SimdSmoke}.hpp
    Physics/**.hpp   Geometry/**.hpp
  src/
    Version.cpp  SimdSmoke.cpp
    Physics/**.cpp   (Geometry is header-only -- no src/Geometry)
  tests/
    <migrated Catch2 suites>  (D3)
    Support/TestWorkScheduler.hpp   # std::thread IWorkScheduler for the MT tests (D3)
  scripts/
    generate_vs2022.bat  generate_linux.sh  sync_to_github.ps1  vendor.ps1
  vendor/
    Catch2/   rapidcheck/          # its own test deps (copied from Aphelyon/ThirdParty)
```

Naming echoes Astra's `include/<Name>/<Subsystem>/`. The lib content is exactly what Phase 2
produced under `ThirdParty/Manifold2D` (already `Manifold2D::`, zero Arcane deps) -- Part A moves
it into the standalone repo, not rewrites it.

### D3. Test migration split

**MOVE into `Manifold2D/tests/`** (the pure physics/geometry/SIMD/BitSet suites -- they only
touch `Manifold2D::`): all `Physics*Test.cpp` that test the library (dynamics, solver, broadphase,
narrowphase, joints, islands, contacts, queries, ccd, determinism, invariants, perf tripwire,
smoke, ...), `GeometryVec2Test`, `ConvexHullTest`, `BitSetTest`, `SimdWideTest`/`SimdWideScalarTest`
+ `Simd/SimdWideTests.inl`, `Manifold2DCoreTest`. The exact file list is enumerated by the plan's
survey; the classifier is "does it `#include <Manifold2D/...>` and nothing from `Arcane/` (engine
Scene/Render/Sandbox)".

**The MT-invariance tests** (`SolverMtInvarianceTest`, `BroadphaseMtInvarianceTest`,
`PhysicsNarrowphaseMtTest`) move too, but they currently inject Arcane's enkiTS executor. Add
`tests/Support/TestWorkScheduler.hpp` -- a `std::thread`-pool implementation of
`Manifold2D::IWorkScheduler` (mirrors Astra's `tests/Support/TestWorkerPool.hpp`) -- and repoint
those tests to it. The standalone library still creates no threads; the test pool is test-only.

**STAY in `ArcaneTests`** (they test Arcane's INTEGRATION of Manifold2D, not the library):
`PhysicsSystemTest`, `PhysicsComponentsTest`, `PhysicsDebugDrawTest`, `PhysicsDebugRichTest`,
`PhysicsDebugAccessorsTest`, `PhysicsDebugCapsuleTest`(if engine-draw), `SandboxInteractionTest`,
`SandboxHudTest`, `SandboxVisualsTest`. The plan's survey confirms each stayer includes an
`Arcane/...` engine header.

**NEW thin link-smoke in `ArcaneTests`** (`Manifold2DLinkSmokeTest.cpp`, `[manifold2d]`): builds a
tiny world, steps it, asserts a body fell -- proves the vendored Manifold2D links + runs inside
Aphelyon, so a broken vendor-in is caught by Aphelyon's build even though the full suite left.

**Consequence (accepted):** Aphelyon's `~[gpu]` assertion/case count drops by the moved suites;
the standalone repo's CI now owns physics test coverage. The determinism + value-exactness of the
moved tests is preserved (same expected values; the move is not a rewrite).

### D4. Premake reconciliation (the compiled-lib twist)

- **Standalone `premake5.lua` = a real `workspace "Manifold2D"`** (like Astra's): configs
  Debug/Release/Dist, `IncludeDir` for Manifold2D + Catch2 + rapidcheck, a `group "Dependencies"`
  that `include`s `vendor/Catch2` + `vendor/rapidcheck`, and two projects: `Manifold2D` (StaticLib,
  globs `include/**` + `src/**`, `/fp:strict`, `/arch:AVX2`, `cppdialect C++23`) and
  `Manifold2DTests` (ConsoleApp, globs `tests/**`, links `Manifold2D` + Catch2 + rapidcheck).
  Windows filter now; a `filter "system:linux"` block with best-guess flags is added in Part A but
  exercised in Part B (D5).
- **Aphelyon consumption changes:** Arcane's `premake5.lua` STOPS `include "../ThirdParty/Manifold2D"`
  (that was Phase 2's bare-project wrapper) and instead defines an **inline `project "Manifold2D"`**
  (StaticLib, `staticruntime "off"` = /MD, `floatingpoint "Strict"`, `/arch:AVX2`, `/bigobj`,
  `cppdialect C++23`) that globs the vendored `ThirdParty/Manifold2D/include/**` + `src/**`. This
  decouples Aphelyon from the vendored workspace `premake5.lua` (Aphelyon ignores it, exactly as it
  ignores Astra's). `links`/`includedirs` for Arcane, Sandbox, ArcaneTests are unchanged from Phase 2.
  `IncludeDir["Manifold2D"]` stays `ThirdParty/Manifold2D/include`.

### D5. Cross-platform scaffolding in Part A (activated in Part B)

Part A LAYS the groundwork so Part B is a drop-in, but does NOT run the g++/clang burn-down:
- `scripts/generate_linux.sh` (premake `gmake2`), alongside `generate_vs2022.bat`.
- A `filter "system:linux"` block in the standalone `premake5.lua` pre-filled with the anticipated
  flags: `-std=c++23`, `-ffp-contract=off -fno-fast-math` (the `/fp:strict` determinism analog),
  `-mavx2` (AVX2 backend; scalar fallback otherwise), `-Wall -Wextra`, `links { "pthread" }` for
  the test pool.
- A portability pass over the SIMD arch-guards + includes so the structure is Linux-ready:
  confirm `Simd.hpp`'s `#if` ladder keys on portable macros, `<immintrin.h>` (not `<intrin.h>`) for
  AVX2, and that `ARCANE_SIMD_INLINE` already abstracts `__forceinline` vs `__attribute__((always_inline))`.
- `.github/ci.yml` ships in Part A with the **windows-msvc job only** (green). Part B adds the
  **linux** job (gcc + clang) once the burn-down passes -- Part A does not ship a red Linux CI.

### D6. Non-goals (Part A)

No actual Linux/g++/clang compilation-green (Part B). No behavior/algorithm change. No git-history
transfer to GitHub (snapshot only). No benchmark migration beyond a stub (`benchmark/` seed
optional; the `[perf]` tripwire moves with the tests as a Catch2 case, not a Google-Benchmark
harness). No change to Aphelyon's non-physics code beyond the premake rewire + the moved/added
tests + the vendor. No submodule (synced-copy model, matching Astra).

## Sequencing (Part A)

1. **Build the standalone repo** at `D:\dev\starworks\Manifold2D`: create the structure (D2), move
   the Phase-2 lib content out of `ThirdParty/Manifold2D` into it, write the workspace `premake5.lua`
   (D4) + scripts (D1) + docs + `.gitignore`, vendor Catch2 + rapidcheck.
2. **Migrate tests** (D3) + the `TestWorkScheduler`; `premake vs2022` + MSBuild; the migrated suite
   passes value-exact (the counts the plan's survey pins).
3. **Windows CI** (`.github/ci.yml` windows-msvc) + first GitHub push via `D:\dev\github\Manifold2D`
   (reconcile README/LICENSE).
4. **Re-vendor** into Aphelyon (`vendor.ps1`) + rewire Arcane to the inline `Manifold2D` project (D4)
   + remove the migrated tests from `ArcaneTests` + add the link-smoke (D3); Aphelyon builds & the
   remaining `~[gpu]` + Server suites pass (new, lower physics count = the migrated-out delta).
5. **Land**: commit the Aphelyon-side changes on `manifold2d-phase3a`, review, merge to `main`,
   push Aphelyon `main` (origin/main is still @9b58c9c0 = pre-Phase-2, so this push carries Phase 2
   + Phase 3a together).

Part B (later) = steps for Linux, its own spec.

## Gates / baselines

- **Standalone repo (Windows):** `premake vs2022` + MSBuild Debug/Release/Dist clean; the migrated
  `Manifold2DTests` passes -- the moved suites keep their expected values (value-exact vs. the
  Phase-2 numbers minus what stayed in ArcaneTests). CI windows-msvc green.
- **Aphelyon after re-vendor:** builds Debug/Release/Dist; `~[gpu]` passes at its NEW count (the
  pre-Phase-3 `~[gpu]` = 177296/593 minus the migrated physics assertions/cases, plus the link-smoke
  -- the plan pins the exact expected delta from the survey); `[geometry]` moves out; determinism
  moves out; Server `CommonTests 420/66` + `AuthTests 34/12` + `CombatTests 1/1` unchanged; the
  deferred `[gpu]`/Loom desk gauntlet still applies before final reliance.

## Risks

- **Test entanglement:** a "pure" physics test that secretly pulls an `Arcane/` header would break
  the standalone build -> the plan's per-file survey classifies every migrating file by its includes
  before moving; the standalone build is the tripwire.
- **Two premake files named `premake5.lua`** (standalone workspace vs. Aphelyon's inline project in
  Arcane's own premake): resolved by Aphelyon NOT including the vendored one (D4) -- the vendored
  `premake5.lua` is inert in Aphelyon.
- **rapidcheck/Catch2 vendoring:** the standalone repo needs its own copies (Aphelyon's live in
  `ThirdParty/`); copy them into `Manifold2D/vendor/` with their premake wrappers, honoring the
  standalone workspace's static-CRT default.
- **`~[gpu]` count bookkeeping:** moving ~55 test files changes Aphelyon's headline count; the plan
  must compute + assert the new expected number rather than "unchanged," so a real regression is
  still caught.
- **First GitHub push reconciliation:** the repo has an initial README/LICENSE commit; the sync
  must merge cleanly (fetch + robocopy over the clone + commit), not force-push away history.
- **Part A must not ship a red Linux CI** (D5) -- the linux job is Part B.
