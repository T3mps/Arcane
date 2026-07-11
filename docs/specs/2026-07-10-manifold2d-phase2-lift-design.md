# Manifold2D Phase 2 -- lift to a standalone vendored library (design)

Date: 2026-07-10. Status: approved direction (user), Phase 2 of the Manifold2D extraction arc.
Prereq: Phase 1 merged to main @9b58c9c0 (`Geometry::Vec2<T>` severed glm from Core/Physics +
Core/Geometry, value-exact). Spec of Phase 1: `2026-07-10-manifold2d-phase1-vec2-design.md`.

## Context and product goal

The 2D physics engine (`Arcane/Core/src/Arcane/Physics/**`) plus its geometry kernel
(`Arcane/Core/src/Arcane/Geometry/**`) is named **Manifold2D**. Starworks tech ships as
independently-usable components, Unreal-style: **Astra** (ECS), **Manifold2D** (2D physics),
**Arcane** (full engine). A developer wanting only 2D physics must be able to take Manifold2D
without Arcane. Phase 2 physically lifts Manifold2D into `ThirdParty/Manifold2D/` as its own
vendored static library with the `Manifold2D::` namespace, reworks every consumer to use it
directly (**no strangler shim layer** -- consuming Manifold2D directly is the intended end
state), and severs its last dependencies on Arcane so it has **zero `Arcane/` includes**.

The gate is **value/behavior identity**, not binary-byte identity: symbol names change, so the
linked image differs, but every existing assertion keeps its expected value and the determinism
hash is unchanged. Any test-value drift is a migration bug, never a re-baseline.

## Survey facts (load-bearing)

- **Moving set = 68 files**: 59 under `Physics/` (incl. `Broadphase/`, `Joints/`, `Narrowphase/`,
  `Solver/`), 9 under `Geometry/` (incl. `detail/`). `Physics/PhysicsTypes.hpp` includes
  `Geometry/Vec2.hpp` -> they move as ONE unit.
- **Consumers = 73 files**: 3 engine DLL (`Arcane/Arcane/src`: `Scene/PhysicsSystem.hpp`,
  `Scene/PhysicsComponents.hpp`, `Render/PhysicsDebugDraw.cpp`), 8 Sandbox, 0 Loom/plugins,
  61 ArcaneTests, 0 Server, 1 docs example (`docs/examples/arcane-physics-example.cpp`).
- **The moving set is std-only** apart from exactly 4 header-only Core utilities (below). No
  glm/spdlog/tracy/nlohmann/enkiTS includes remain (Phase 1 severed glm; residual glm mentions
  are comments/assert-strings only).
- **Namespace surface**: `Arcane::Physics` (+ nested `StepProf`, `Island`, `JointMath`, `Math`,
  `SimdSolve`), `Arcane::Geometry` (+ `detail`). Inside the moving set, the ONLY `Arcane::`
  tokens are `Arcane::{Physics,Geometry,Simd,FunctionRef,BitSet}` and `namespace Arcane[::...]`
  -- every one maps to something that also moves, so a blanket `Arcane`->`Manifold2D` rename of
  the moved files is safe (include paths handled per-target; see D3).
- **Build**: `Arcane/Core/src/**` is ONE `**`-globbed StaticLib -- `Core` (Arcane ws, `/MD`,
  `staticruntime off`, `-md` outputdir) and `ArcaneCore` (Server ws, `/MT`). Both globs
  **auto-shrink** when Physics/Geometry leave `Core/src`. No Server code references
  physics/geometry -> Manifold2D is **Arcane-only** (no dual-CRT). Consumers linking physics
  today: **Arcane DLL** (compiles `Render/PhysicsDebugDraw.cpp`), **Sandbox**, **ArcaneTests**;
  **Loom** has zero physics refs.

## Design decisions (locked)

### D1. Layout -- own include root, headers/impl split; loosely Astra-flavored

Manifold2D is a COMPILED library (not header-only like Astra), so it uses a conventional
`include/` (public headers) + `src/` (implementation) split, with ONE `premake5.lua` (the enkiTS-style
StaticLib wrapper Arcane includes -- D4; no standalone workspace). Directory naming echoes Astra's
`include/<Name>/<Subsystem>/` so the tree stays repo-ready for the Phase 3 split.

```
ThirdParty/Manifold2D/
  premake5.lua                       # ONE file: enkiTS-shaped StaticLib wrapper (D4)
  README.md                          # component overview (repo-readiness marker)
  include/Manifold2D/                # PUBLIC HEADERS (the include root)
    Core/                            #   the 4 lifted primitives (flat ns Manifold2D::)
      FunctionRef.hpp                #     copied from Arcane::FunctionRef
      BitSet.hpp                     #     moved  from Arcane::BitSet
      Simd.hpp + Simd_{Scalar,AVX2,NEON}.inl   # moved from Arcane::Simd
      WorkScheduler.hpp              #     IWorkScheduler + SerialWorkScheduler (see D2)
    Physics/**.hpp                   #   physics headers (ns Manifold2D::Physics), T2
    Geometry/**.hpp                  #   geometry headers (ns Manifold2D::Geometry), T2
  src/                               # IMPLEMENTATION (.cpp; NOT on the public include path)
    Version.cpp                      #   1 trivial TU (LibraryVersion) so the lib is non-empty
    Physics/**.cpp                   #   physics .cpp (subtree mirrors include/), T2
    Geometry/**.cpp                  #   geometry .cpp, T2
```

`IncludeDir["Manifold2D"] = ThirdParty/Manifold2D/include`, so `#include <Manifold2D/Physics/...>`,
`<Manifold2D/Geometry/...>`, `<Manifold2D/Core/...>` resolve. The `src/` .cpp reach their headers via
that same public path (`include/` is on the lib's includedirs; headers reference each other by
`<Manifold2D/...>` path, not relative -- so the split does not break header-to-header includes).
Primitives sit under `Manifold2D/Core/` (mirroring `Astra/Core/`) in the **flat** `namespace Manifold2D`
(matching how they are top-level `Arcane::` today); Physics/Geometry keep their sub-namespaces
`Manifold2D::Physics` / `Manifold2D::Geometry`. Fuller repo-mirroring (standalone workspace, `tests/`,
`scripts/`, `.github`) is deferred to the Phase 3 split; Phase 2 keeps the 61 tests in ArcaneTests.

### D2. Threading = an injected seam (Astra's IWorkScheduler model)

Arcane's `ITaskExecutor` (`Jobs/TaskExecutor.hpp`) is already the seam by design -- its comment
reads "presentation-free, Astra-free, no global state -> safe in Core" and it ships
`SerialTaskExecutor` (runs the range inline as worker 0) as the inject-nothing default, the exact
analog of Astra's null-scheduler = sequential-inline. On lift it becomes Manifold2D's own seam,
renamed to Astra's vocabulary:

- `Arcane::ITaskExecutor`      -> `Manifold2D::IWorkScheduler`
- `Arcane::SerialTaskExecutor` -> `Manifold2D::SerialWorkScheduler`
- file `Jobs/TaskExecutor.hpp` -> `Manifold2D/Core/WorkScheduler.hpp`
- methods **unchanged**: `ParallelFor(count, minBatch, FunctionRef<void(begin,end,worker)>)` +
  `WorkerCount()`. (The 3rd `worker` arg is a Manifold2D superset for per-worker scratch; keep it.)

Manifold2D creates no threads; the consumer injects a scheduler (null -> `SerialWorkScheduler`).
Arcane **keeps** its own `Arcane::ITaskExecutor` for the engine's non-physics jobs (Runtime,
JobSystem, Sandbox). Arcane's enkiTS-backed executor drives Manifold2D through a small adapter
(`Manifold2D::IWorkScheduler`-facing, forwarding to the engine executor) placed at the one engine
bridge (`Arcane/Scene/PhysicsSystem`). This is a **copy** (both interfaces coexist), dependency
arrow Arcane -> Manifold2D only.

### D3. Per-util independence boundary (zero Arcane deps after Phase 2)

| Util (today) | Disposition | Rationale |
|---|---|---|
| `Jobs/TaskExecutor.hpp` (`ITaskExecutor`/`SerialTaskExecutor`) | **COPY** -> `Manifold2D/Core/WorkScheduler.hpp` (renamed, D2) | The injected seam. Arcane keeps its own for engine jobs. |
| `Util/BitSet.hpp` (`Arcane::BitSet`) | **MOVE** -> `Manifold2D::BitSet` | Physics-only production user (`ConstraintGraph::m_npStateBits`, the per-worker lock-free contact-state bitset that keeps narrowphase-MT byte-identical -- a Box2D v3 `contactStateBitSet` port). Its unit test travels. |
| `Math/Simd.hpp` (+3 `.inl`) (`Arcane::Simd`) | **MOVE** -> `Manifold2D::Simd` | Zero production use outside physics/geometry (only `SimdSmoke.cpp` = its own smoke test + `GeometryVec2Test`). Astra precedent: carries its own `Astra/Core/Simd.hpp`. |
| `Util/FunctionRef.hpp` (`Arcane::FunctionRef`) | **COPY** -> `Manifold2D::FunctionRef` | Leaf vocabulary (non-owning `std::function`-lite) pervading 13 physics files' PUBLIC signatures (`ContactPool::ForEach`, `PhysicsWorld` queries, `DynamicTree`/`SpatialGrid` traversal, `SolverStages` hooks). Arcane keeps its own (used by `OffscreenCanvas` + Arcane's `TaskExecutor`). Tiny stable header; a shared "foundation" lib is the someday-fix (YAGNI now -- would re-couple Manifold2D to an external dep). |

Include-path remap applied to the moved files (the non-blanket part of the rename):
`<Arcane/Physics/...>`->`<Manifold2D/Physics/...>`, `<Arcane/Geometry/...>`->`<Manifold2D/Geometry/...>`,
`<Arcane/Util/BitSet.hpp>`->`<Manifold2D/Core/BitSet.hpp>`,
`<Arcane/Util/FunctionRef.hpp>`->`<Manifold2D/Core/FunctionRef.hpp>`,
`<Arcane/Jobs/TaskExecutor.hpp>`->`<Manifold2D/Core/WorkScheduler.hpp>`,
`<Arcane/Math/Simd.hpp>`->`<Manifold2D/Core/Simd.hpp>` (and the `.inl` self-includes inside Simd.hpp).

### D4. Premake wrapper (enkiTS-shaped StaticLib)

`ThirdParty/Manifold2D/premake5.lua`: bare `project "Manifold2D"` (no workspace), `kind "StaticLib"`,
`language "C++"`, `cppdialect "C++23"`, `location(THIRDPARTY_PROJECT_LOCATION or ".")`,
`staticruntime(THIRDPARTY_STATICRUNTIME or "on")`, `floatingpoint "Strict"` (determinism),
`targetdir/objdir` off the workspace `outputdir`, `files { "include/**.hpp", "include/**.inl",
"src/**.cpp" }`, `includedirs { "include" }`, `defines { "_CRT_SECURE_NO_WARNINGS" }`, windows
filter `systemversion "latest"` + `buildoptions { "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }`,
and Debug/Release/Dist config filters mirroring `Core` (`ARCANE_*` defines + NDEBUG on Release/Dist).
Wired into `Arcane/premake5.lua`: add `IncludeDir["Manifold2D"]`, `include "../ThirdParty/Manifold2D"`
in `group "Dependencies"`, and add `"Manifold2D"` to the `links`+`includedirs` of `Arcane`, `Sandbox`,
`ArcaneTests`. The `Core` project (Arcane ws) also gets `IncludeDir.Manifold2D` on its `includedirs`
transiently (its still-resident physics uses Manifold2D/Core primitives until the move). Server: no
edits needed for correctness (the `**` glob drops the moved files); the now-dead `IncludeDir["glm"]`
+ its two comments are removed as cleanup.

### D5. Consumer rework (no shims)

Every consumer switches include paths to `<Manifold2D/...>` and symbol references
`Arcane::Physics`->`Manifold2D::Physics`, `Arcane::Geometry`->`Manifold2D::Geometry`. Pattern-specific:
- Tests (61) + docs example: file-scope `using namespace Arcane::Physics;` -> `Manifold2D::Physics;`
  (mechanical). Two test-local helper namespaces (`PhysicsDeterminismTest.cpp:79`,
  `PhysicsPhase2HarnessTest.cpp:96`) re-open `namespace Arcane::Physics::Foo` -> `Manifold2D::Physics::Foo`.
- Sandbox (8): local `namespace Phys = Arcane::Physics;` / `Geo = Arcane::Geometry;` alias
  declarations re-point to `Manifold2D::`. Consumer-local aliases are ordinary C++, not a shim
  layer -- keep them. Forward-decl `Interaction.hpp:46 namespace Arcane::Physics { class PhysicsWorld; }`
  -> `Manifold2D::Physics`.
- Engine DLL (3): sits in `namespace Arcane` and writes bare `Physics::X`. After the rename there
  is no `Arcane::Physics`, so add a file-scope `namespace Phys = Manifold2D::Physics;` alias (and
  `Geo` where needed) and rewrite `Physics::` -> `Phys::` -- OR fully qualify to `Manifold2D::Physics::`.
  `PhysicsComponents.hpp:152` enum-reflection block re-opens the namespace (reflects `BodyType`/
  `ShapeKind`, now `Manifold2D::Physics::`) -> move that block to `namespace Manifold2D::Physics`.
  `PhysicsDebugDraw.hpp:31` forward-decls in `namespace Physics` (inside Arcane) -> forward-decl in
  `namespace Manifold2D::Physics`.

### D6. Non-goals

No behavior/algorithm change. No solver adoption of `Vec2<f32w>` (Phase 1 deferral stands). No
big-worlds double (deferred, post-Phase-2). No joints roster change (deferred). No shared-foundation
lib (the FunctionRef/scheduler DRY question is deferred; vendoring keeps Manifold2D dependency-free
now). No repo split (Phase 3). Arcane keeps its own `FunctionRef` + `ITaskExecutor` (untouched).

## Task outline (see the plan for executable steps)

- **T1** -- Scaffold `ThirdParty/Manifold2D/` + the 4 `Core/` primitives (`Manifold2D::`, final
  location) + the premake wrapper + workspace wiring + a `[manifold2d]` smoke test. Purely
  additive; existing suite unchanged. Green.
- **T2** -- The cutover: `git mv` Physics+Geometry into `Manifold2D/{Physics,Geometry}`; blanket
  `Arcane`->`Manifold2D` rename of the moved files with per-target include remap (D3) +
  `ITaskExecutor`->`IWorkScheduler`/`SerialTaskExecutor`->`SerialWorkScheduler`; rework all 73
  consumers (D5); add the engine `IWorkScheduler` adapter (D2); Core/ArcaneCore globs auto-shrink;
  regen premake (Arcane + Server). Full value-exact suite + Server. Green.
- **T3** -- Cleanup: remove Core's now-dead `Arcane::BitSet` + `Arcane::Simd` (retarget/relocate
  `SimdSmoke`, update `BitSetTest` to `Manifold2D::`); remove dead Server `glm` include + comments;
  grep-proofs (no `<Arcane/` in the Manifold2D tree; no `Arcane::Physics`/`Arcane::Geometry`
  repo-wide); CLAUDE.md + docs; exit gauntlet (`[gpu]`, Loom both backends, Dist). Green.

## Gates / baselines to hold (main @9b58c9c0, post-Phase-1)

Debug `~[gpu]` **177282 / 589**; `[gpu]` **611 / 37**; `[geometry]` **118815 / 17**; `[determinism]`
run-twice hash stable; `[perf]` neutral (Manifold2D is the hottest code -- verify); Release + Dist
build clean; Server `CommonTests 420/66`, `AuthTests 34/12`, `CombatTests 1/1`; `Loom --frames 180`
dx12 + vulkan exit 0. Plus new: `[manifold2d]` primitive smoke (T1). Every existing expected value
is UNCHANGED.

## Risks

- **Blanket-rename over-reach** -> the survey proved the moved set's only `Arcane::` tokens all map;
  the rename script targets the moved files only, and T3's grep-proofs catch stragglers.
- **Engine bare `Physics::` breakage** -> D5 handles the 3 engine files explicitly (alias or qualify).
- **Empty StaticLib in T1** -> the trivial `src/Manifold2D.cpp` TU guarantees a non-empty lib
  (LNK4221 avoided).
- **Two copies of Manifold2D in one process** (Arcane.dll + Sandbox.dll both static-link it) --
  inherits Core's existing, knowingly-accepted contract: global-state-free + `/fp:strict` +
  identical layout, physics objects owned within one module. Keep Manifold2D global-state-free.
- **enkiTS adapter FunctionRef mismatch** (engine `Arcane::FunctionRef` vs `Manifold2D::FunctionRef`)
  -> the adapter wraps the callback in a lambda; trivial, localized to one file.
