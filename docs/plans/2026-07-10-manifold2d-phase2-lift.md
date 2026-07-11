# Manifold2D Phase 2 -- lift to a standalone vendored library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Physically lift Physics+Geometry (68 files) into `ThirdParty/Manifold2D/` as a standalone
static library in the `Manifold2D::` namespace, rework all 73 consumers to use it directly (no shim
layer), and sever its last 4 Core-util dependencies so it has ZERO `Arcane/` includes -- value/behavior
identical to today.

**Architecture:** Three tasks. T1 scaffolds the lib + its 4 `Core/` primitives (additive, existing
suite unchanged). T2 is the atomic cutover: `git mv` the moved set, blanket-rename `Arcane`->`Manifold2D`
in the moved files (with per-target include remap + `ITaskExecutor`->`IWorkScheduler`), rework every
consumer, add the engine threading adapter; the Core globs auto-shrink. T3 removes the now-dead Core
copies + does grep-proofs, docs, and the GPU/Loom/Dist gauntlet. Threading is an injected seam
(`Manifold2D::IWorkScheduler`, Astra-style); Arcane keeps its own `ITaskExecutor` and adapts its enki
executor at the one `Scene/PhysicsSystem` bridge.

**Tech Stack:** C++23, MSVC (VS 2026), premake5 -> vs2026, Catch2 v3. Spec:
`docs/superpowers/specs/2026-07-10-manifold2d-phase2-lift-design.md` (READ IT FIRST -- decisions
D1-D6 govern).

## Global Constraints

- Worktree `<WT>` = `D:\dev\starworks\Gacha\.claude\worktrees\manifold2d-lift`, branch
  `worktree-manifold2d-lift`. Do all work here. Do not touch the primary checkout `D:\dev\starworks\Gacha`.
- **Value-exactness is the gate:** any changed expected value in an existing test = a migration bug,
  NEVER a re-baseline. Baselines entering (main @9b58c9c0): Debug `~[gpu]` **177282 / 589**; `[gpu]`
  **611 / 37**; `[geometry]` **118815 / 17**; Server `CommonTests 420/66`, `AuthTests 34/12`,
  `CombatTests 1/1`. Binary-byte identity is NOT expected (symbol names change); the `[determinism]`
  hash IS (it hashes simulation values, not symbols).
- ASCII-only new text, UTF-8 no BOM. clangd/LSP diagnostics are NOISE (no compile_commands); MSBuild
  is the only gate.
- **Force-rebuild discipline (bit twice on this codebase):** after header edits, `/t:Rebuild` (or the
  affected project `:Rebuild`) before trusting red/green -- MSVC incremental links stale inline defs
  from unrecompiled TUs.
- **Run test exes in the FOREGROUND**, from the exe's own directory (backgrounded runs stall).
- **Commands (PowerShell only; Git Bash mangles switches):**
  - Regen (whenever a globbed file is added/removed/moved, or a premake file changes), from
    `<WT>\Arcane\`: `& "<WT>\ThirdParty\premake5\premake5.exe" vs2026` -- and ALSO from `<WT>\Server\`
    for the Server gate. `VCPKG_ROOT` is already `D:\dev\_shared\tools\vcpkg`; NEVER override it (SDL3
    resolves from there; a fresh worktree does NOT need to re-run vcpkg).
  - Build Arcane, from `<WT>\Arcane\`: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nologo /v:minimal`
    (also `/p:Configuration=Release` and `Dist` where a task's gate calls for them).
  - Build Server, from `<WT>\Server\`: same MSBuild on `Aphelyon.slnx /p:Configuration=Debug`.
  - Test, from `<WT>\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`: `.\ArcaneTests.exe "<filter>"`.
    (The `-md` suffix is the Arcane workspace's dynamic-CRT bin dir.)
- **GPU/display hazard:** `[gpu]` tests + Loom windowed runs SIGSEGV under Parsec/virtual-display
  sessions. The dev-loop gate is `~[gpu]`. If the machine is in a remote-desktop session, DEFER the
  `[gpu]`/Loom lines (T3 gauntlet) to a desk session and say so in the report; do not treat their
  absence as a pass or fail.
- Commits: conventional style, NO AI trailers, `git commit -F <tempfile>` (or Git Bash heredoc), stage
  exact paths, never `git add -A`.

## File Structure

Manifold2D is a compiled lib with a conventional `include/` (headers) + `src/` (`.cpp`) split, ONE
`premake5.lua` (enkiTS-shaped StaticLib wrapper), naming echoing Astra's `include/<Name>/<Subsystem>/`.

| File | Responsibility |
|---|---|
| `ThirdParty/Manifold2D/premake5.lua` (new) | ONE enkiTS-shaped StaticLib wrapper globbing `include/**.{hpp,inl}` + `src/**.cpp` (spec D4). |
| `ThirdParty/Manifold2D/README.md` (new) | Component overview (repo-readiness marker). |
| `ThirdParty/Manifold2D/src/Version.cpp` (new) | 1 trivial TU (`LibraryVersion`) so the lib is non-empty. |
| `ThirdParty/Manifold2D/include/Manifold2D/Core/FunctionRef.hpp` (new, copy) | `Manifold2D::FunctionRef`. |
| `ThirdParty/Manifold2D/include/Manifold2D/Core/BitSet.hpp` (created T1; Core copy deleted T3) | `Manifold2D::BitSet`. |
| `ThirdParty/Manifold2D/include/Manifold2D/Core/Simd.hpp` + `Simd_{Scalar,AVX2,NEON}.inl` (new, moved) | `Manifold2D::Simd`. |
| `ThirdParty/Manifold2D/include/Manifold2D/Core/WorkScheduler.hpp` (new, from TaskExecutor.hpp) | `Manifold2D::IWorkScheduler` + `SerialWorkScheduler`. |
| `ThirdParty/Manifold2D/include/Manifold2D/{Physics,Geometry}/**.hpp` (moved in T2) | the moved-set HEADERS. |
| `ThirdParty/Manifold2D/src/{Physics,Geometry}/**.cpp` (moved in T2) | the moved-set IMPLEMENTATION. |
| `Arcane/Tests/src/Manifold2DCoreTest.cpp` (new, T1) | `[manifold2d]` primitive smoke test (stays in ArcaneTests). |
| `Arcane/premake5.lua` (modify) | `IncludeDir.Manifold2D`, `include`, links (T1). |
| `Server/premake5.lua` (modify, T3) | drop dead `IncludeDir["glm"]` + comments. |
| `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp` (modify, T2) | the `Manifold2D::IWorkScheduler` adapter + rework. |
| the other 72 consumers (modify, T2) | include-path + namespace rework. |

---

### Task 1: Scaffold Manifold2D + the 4 `Core/` primitives (additive)

**Files:**
- Create: `ThirdParty/Manifold2D/premake5.lua`, `ThirdParty/Manifold2D/src/Version.cpp`,
  `ThirdParty/Manifold2D/README.md`
- Create: `ThirdParty/Manifold2D/include/Manifold2D/Core/{FunctionRef,BitSet,Simd,WorkScheduler}.hpp`,
  `.../Core/Simd_{Scalar,AVX2,NEON}.inl`
- Create: `Arcane/Tests/src/Manifold2DCoreTest.cpp`
- Modify: `Arcane/premake5.lua`

**Interfaces:**
- Produces (T2 relies on EXACTLY these): headers under `<Manifold2D/Core/...>` declaring, in
  `namespace Manifold2D`: `FunctionRef<...>` (same surface as `Arcane::FunctionRef`), `BitSet`
  (same surface as `Arcane::BitSet`), the `Simd` namespace (`f32w`/`b32w`/`i32w`, same as
  `Arcane::Simd`), `IWorkScheduler` (was `ITaskExecutor`) with
  `ParallelFor(std::size_t,std::size_t,FunctionRef<void(std::size_t,std::size_t,std::uint32_t)>)` +
  `WorkerCount()`, and `SerialWorkScheduler` (was `SerialTaskExecutor`).
- Arcane keeps `Arcane::FunctionRef` + `Arcane::ITaskExecutor`/`SerialTaskExecutor` UNTOUCHED.

- [ ] **Step 1: Create the primitive headers by copy-and-rename.** From `<WT>` (Git Bash ok for
  file ops; PowerShell for builds):
  ```bash
  mkdir -p ThirdParty/Manifold2D/include/Manifold2D/Core ThirdParty/Manifold2D/src
  # FunctionRef (COPY -- Arcane keeps its own): std-only, just rename the namespace + guard comments.
  sed 's/namespace Arcane/namespace Manifold2D/; s/Arcane::FunctionRef/Manifold2D::FunctionRef/g' \
    Arcane/Core/src/Arcane/Util/FunctionRef.hpp > ThirdParty/Manifold2D/include/Manifold2D/Core/FunctionRef.hpp
  # BitSet (MOVE origin created here; Core copy deleted in T3): std-only, rename namespace.
  sed 's/namespace Arcane/namespace Manifold2D/; s/Arcane::BitSet/Manifold2D::BitSet/g' \
    Arcane/Core/src/Arcane/Util/BitSet.hpp > ThirdParty/Manifold2D/include/Manifold2D/Core/BitSet.hpp
  # Simd (+ inls): rename namespace AND the .inl self-include paths.
  sed -e 's/namespace Arcane/namespace Manifold2D/' \
      -e 's|<Arcane/Math/Simd_|<Manifold2D/Core/Simd_|g' \
      -e 's/Arcane::Simd/Manifold2D::Simd/g' \
    Arcane/Core/src/Arcane/Math/Simd.hpp > ThirdParty/Manifold2D/include/Manifold2D/Core/Simd.hpp
  for inl in Scalar AVX2 NEON; do
    sed -e 's/namespace Arcane/namespace Manifold2D/' -e 's/Arcane::Simd/Manifold2D::Simd/g' \
      Arcane/Core/src/Arcane/Math/Simd_$inl.inl > ThirdParty/Manifold2D/include/Manifold2D/Core/Simd_$inl.inl
  done
  # WorkScheduler (COPY of TaskExecutor.hpp, renamed to Astra vocabulary):
  sed -e 's/namespace Arcane/namespace Manifold2D/' \
      -e 's|<Arcane/Util/FunctionRef.hpp>|<Manifold2D/Core/FunctionRef.hpp>|' \
      -e 's/\bITaskExecutor\b/IWorkScheduler/g' \
      -e 's/\bSerialTaskExecutor\b/SerialWorkScheduler/g' \
    Arcane/Core/src/Arcane/Jobs/TaskExecutor.hpp > ThirdParty/Manifold2D/include/Manifold2D/Core/WorkScheduler.hpp
  ```
  Then READ each generated header and hand-verify: (a) namespace is `Manifold2D` (top-level, flat),
  (b) no `Arcane` token remains (`rg -n 'Arcane' ThirdParty/Manifold2D/include/Manifold2D/Core` -> ZERO),
  (c) `Simd.hpp` still gates on the same `ARCANE_SIMD_*`/arch macros (those are DEFINE names, not the
  `Arcane` namespace -- if the .inl dispatch uses a macro literally named with `ARCANE_`, LEAVE it; it
  is a build macro, not a namespace token -- confirm by reading Simd.hpp's `#if` ladder). Fix the guard
  comment at the top of each to say "Manifold2D" and cite this spec.
- [ ] **Step 2: Update the WorkScheduler.hpp header comment.** Its banner still describes the engine
  seam. Rewrite it to: `Manifold2D::IWorkScheduler` -- the injected data-parallel seam (Astra's
  IWorkScheduler model). Manifold2D creates no threads; `SerialWorkScheduler` is the deterministic
  default when the consumer injects none. The host (e.g. Arcane) adapts its own executor to this
  interface. Keep the `ParallelFor` contract text verbatim (the per-worker-disjoint-writes guarantee
  the solver relies on).
- [ ] **Step 3: Create the trivial TU + README.** `ThirdParty/Manifold2D/src/Version.cpp`:
  ```cpp
  // One translation unit so the Manifold2D static library is never empty (avoids
  // MSVC LNK4221) in Task 1, before the Physics/Geometry .cpp populate src/ in Task 2.
  #include <cstdint>

  namespace Manifold2D
  {
      // Bumped when the vendored snapshot changes; also proves the lib links.
      std::uint32_t LibraryVersion() noexcept { return 2000; } // Phase 2 == 2.0.0
  }
  ```
  And a short `ThirdParty/Manifold2D/README.md` (repo-readiness marker): one paragraph naming
  Manifold2D as Starworks' standalone 2D physics + geometry library (Astra = ECS, Manifold2D = 2D
  physics, Arcane = full engine), noting the `include/` (public headers, `Manifold2D::` namespace) +
  `src/` (impl) layout, the injected `Manifold2D::IWorkScheduler` threading seam (host supplies the
  scheduler; `SerialWorkScheduler` is the no-inject default), and that it has zero external deps. Keep
  it brief; it is the seed of the Phase 3 standalone repo's README.
- [ ] **Step 4: Write the premake wrapper.** `ThirdParty/Manifold2D/premake5.lua` (enkiTS-shaped, spec D4):
  ```lua
  -- Manifold2D premake5 wrapper -- Starworks 2D physics + geometry, lifted from
  -- Arcane/Core in Phase 2. Standalone StaticLib (zero Arcane deps); consumed by
  -- the Arcane (/MD) workspace. Honors the shared-wrapper globals so it COULD be
  -- built static-CRT later without edits (no Server consumer today).
  project "Manifold2D"
      kind "StaticLib"
      language "C++"
      cppdialect "C++23"
      location(THIRDPARTY_PROJECT_LOCATION or ".")
      staticruntime(THIRDPARTY_STATICRUNTIME or "on")
      floatingpoint "Strict"   -- determinism: /fp:fast is banned engine-wide

      targetdir ("bin/" .. outputdir .. "/%{prj.name}")
      objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

      files {
          "include/**.hpp",
          "include/**.inl",
          "src/**.cpp",
      }

      includedirs { "include" }

      defines { "_CRT_SECURE_NO_WARNINGS" }

      filter "system:windows"
          systemversion "latest"
          buildoptions { "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }

      filter "configurations:Debug"
          runtime "Debug"
          symbols "on"
          defines { "ARCANE_DEBUG" }

      filter "configurations:Release"
          runtime "Release"
          optimize "on"
          defines { "ARCANE_RELEASE", "NDEBUG" }

      filter "configurations:Dist"
          runtime "Release"
          optimize "on"
          defines { "ARCANE_DIST", "NDEBUG" }
  ```
  NOTE: mirror the `Core` project's actual Debug/Release/Dist `defines` -- open `Arcane/premake5.lua`,
  find the `Core` project's `filter "configurations:*"` blocks, and copy the EXACT define names (the
  `ARCANE_DEBUG/RELEASE/DIST` above are the expected set; correct them if Core differs).
- [ ] **Step 4b: Verify Simd's arch macros survive standalone.** `Manifold2D/Core/Simd.hpp` selects
  Scalar/AVX2/NEON via preprocessor. Confirm the selecting macros are defined by the compiler/premake
  (`/arch:AVX2` is set in the wrapper's windows filter) and NOT by an `Arcane`-project-only define. If
  Simd.hpp keys on a custom define set only by the `Core`/`Arcane` project, add that define to the
  Manifold2D wrapper too. (Read Simd.hpp's `#if` ladder to confirm.)
- [ ] **Step 5: Wire the workspace.** In `Arcane/premake5.lua`:
  - Add to the `IncludeDir` table: `IncludeDir["Manifold2D"] = "%{wks.location}/../ThirdParty/Manifold2D/include"`.
  - Add to `group "Dependencies"` (with the other `include`s): `include "../ThirdParty/Manifold2D"`.
  - Add `"%{IncludeDir.Manifold2D}"` to `includedirs` AND `"Manifold2D"` to `links` for the projects:
    `Arcane` (DLL), `Sandbox`, `ArcaneTests`. ALSO add `"%{IncludeDir.Manifold2D}"` to the `Core`
    project's `includedirs` (its still-resident physics will include Manifold2D/Core primitives in T2).
    Do NOT add Manifold2D to `Loom` (zero physics refs) or `PlaygroundGame`.
- [ ] **Step 6: Write the smoke test.** `Arcane/Tests/src/Manifold2DCoreTest.cpp`:
  ```cpp
  // Manifold2D::Core primitive smoke (Phase 2 Task 1). Proves the lifted primitives
  // compile + link from their new <Manifold2D/Core/...> home in namespace Manifold2D,
  // with zero Arcane dependency. Deeper semantics are covered by the physics/geometry
  // suites once they move (Task 2); this is a link+surface tripwire.
  #include <cstdint>
  #include <catch2/catch_test_macros.hpp>
  #include <Manifold2D/Core/FunctionRef.hpp>
  #include <Manifold2D/Core/BitSet.hpp>
  #include <Manifold2D/Core/Simd.hpp>
  #include <Manifold2D/Core/WorkScheduler.hpp>

  TEST_CASE("Manifold2D::FunctionRef binds and calls", "[manifold2d]")
  {
      int seen = 0;
      auto lam = [&](int v) { seen = v; };
      Manifold2D::FunctionRef<void(int)> ref = lam;
      ref(42);
      CHECK(seen == 42);
  }

  TEST_CASE("Manifold2D::BitSet set/test/count", "[manifold2d]")
  {
      Manifold2D::BitSet b;
      b.Resize(70);           // NOTE: match the ACTUAL BitSet API (Resize/Set/Test or similar) --
      b.Set(3); b.Set(64);    // read Manifold2D/Core/BitSet.hpp and adapt these calls; the ASSERTION
      CHECK(b.Test(3));       // intent (a set bit reads back set, an unset bit reads back clear) holds.
      CHECK(b.Test(64));
      CHECK_FALSE(b.Test(5));
  }

  TEST_CASE("Manifold2D::SerialWorkScheduler runs a range inline as worker 0", "[manifold2d]")
  {
      Manifold2D::SerialWorkScheduler sched;
      CHECK(sched.WorkerCount() == 1u);
      std::uint32_t calls = 0, lastWorker = 99; std::size_t total = 0;
      sched.ParallelFor(10, 1, [&](std::size_t b, std::size_t e, std::uint32_t w) {
          ++calls; lastWorker = w; total += (e - b);
      });
      CHECK(calls == 1u);
      CHECK(lastWorker == 0u);
      CHECK(total == 10u);
  }

  TEST_CASE("Manifold2D::Simd f32w lane round-trip", "[manifold2d][simd]")
  {
      using Manifold2D::Simd::f32w;
      // NOTE to implementer: adapt load/store to the ACTUAL Simd API (read Simd.hpp +
      // the existing SimdSmoke.cpp for the house pattern). Assertion intent: a broadcast
      // (or per-lane load) followed by an add reads back the expected per-lane value.
      alignas(32) float in[f32w::kLanes], out[f32w::kLanes];
      for (int i = 0; i < int(f32w::kLanes); ++i) in[i] = float(i);
      f32w a = f32w::Load(in);
      f32w b = a + a;
      b.Store(out);
      for (int i = 0; i < int(f32w::kLanes); ++i) CHECK(out[i] == in[i] + in[i]);
  }
  ```
- [ ] **Step 7: Regen + build + verify.** From `<WT>\Arcane\` (PowerShell): regen premake, then
  `/t:Rebuild` Debug. Expect: Manifold2D project builds (a non-empty `.lib`, no LNK4221). Run from the
  ArcaneTests exe dir: `.\ArcaneTests.exe "[manifold2d]"` -> the 4 new cases PASS. Then full
  `.\ArcaneTests.exe "~[gpu]"` -> **177282 / 589 plus the new `[manifold2d]` cases only**, ALL PASS,
  every existing value unchanged.
- [ ] **Step 8: Commit.**
  ```bash
  git add ThirdParty/Manifold2D Arcane/premake5.lua Arcane/Tests/src/Manifold2DCoreTest.cpp \
          docs/superpowers/specs/2026-07-10-manifold2d-phase2-lift-design.md \
          docs/superpowers/plans/2026-07-10-manifold2d-phase2-lift.md
  git commit -F <msgfile>   # feat(manifold2d): scaffold ThirdParty/Manifold2D StaticLib + Core primitives (FunctionRef/BitSet/Simd/IWorkScheduler), Astra-aligned
  ```

---

### Task 2: The cutover -- move Physics+Geometry, rename, rework all consumers

**Files:**
- Move (SPLIT by extension): headers `.hpp`/`.inl` -> `ThirdParty/Manifold2D/include/Manifold2D/{Physics,Geometry}/**`;
  implementation `.cpp` -> `ThirdParty/Manifold2D/src/{Physics,Geometry}/**` (subtree mirrored). 68 files total.
- Modify (moved files): blanket rename + include remap (below).
- Modify (73 consumers): 3 engine DLL, 8 Sandbox, 61 ArcaneTests, 1 docs example (see spec D5).
- Create: the engine `IWorkScheduler` adapter (in `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp`
  or a small sibling header).
- Modify: none of `Arcane/premake5.lua` needed beyond T1 (globs auto-adjust); regen only.

**Interfaces:**
- Consumes: T1's `<Manifold2D/Core/...>` primitives.
- Produces: `Manifold2D::Physics::*`, `Manifold2D::Geometry::*` reachable via `<Manifold2D/Physics/...>`
  / `<Manifold2D/Geometry/...>`; `Arcane/Core/src/Arcane/{Physics,Geometry}` no longer exist; the
  Manifold2D tree has zero `<Arcane/...>` includes.

- [ ] **Step 1: Move the files with git, SPLIT by extension (preserves history).** Headers go to
  `include/`, `.cpp` to `src/`, each preserving its `Broadphase/`/`Joints/`/`Narrowphase/`/`Solver/`/
  `detail/` subtree. From `<WT>`:
  ```bash
  for sub in Physics Geometry; do
    SRC="Arcane/Core/src/Arcane/$sub"
    # 1) .cpp -> src/<sub>/<subtree> (git mv one file at a time, creating dirs)
    (cd "$SRC" && find . -name '*.cpp') | sed 's|^\./||' | while read -r rel; do
      dst="ThirdParty/Manifold2D/src/$sub/$rel"
      mkdir -p "$(dirname "$dst")"
      git mv "$SRC/$rel" "$dst"
    done
    # 2) whatever remains (.hpp/.inl + now-empty subdirs) -> include/Manifold2D/<sub>
    git mv "$SRC" "ThirdParty/Manifold2D/include/Manifold2D/$sub"
  done
  # sanity: Arcane/Core/src/Arcane/{Physics,Geometry} are gone; include/ has only headers, src/ only .cpp
  find ThirdParty/Manifold2D/include/Manifold2D/Physics ThirdParty/Manifold2D/include/Manifold2D/Geometry -name '*.cpp'  # -> empty
  ```
- [ ] **Step 2: Rename the moved files (blanket + per-target includes + symbols).** The survey proved
  every `Arcane::` token in the moved set maps to something that also moves, so blanket `Arcane`->
  `Manifold2D` is safe EXCEPT include paths (which need per-target remap) and the two scheduler symbols.
  Apply over BOTH the moved headers (`include/Manifold2D/{Physics,Geometry}`) AND the moved impl
  (`src/{Physics,Geometry}`):
  ```bash
  files=$(rg -l --glob '!*.md' '.' \
            ThirdParty/Manifold2D/include/Manifold2D/Physics \
            ThirdParty/Manifold2D/include/Manifold2D/Geometry \
            ThirdParty/Manifold2D/src/Physics \
            ThirdParty/Manifold2D/src/Geometry)
  for f in $files; do
    sed -i \
      -e 's|<Arcane/Physics/|<Manifold2D/Physics/|g' \
      -e 's|<Arcane/Geometry/|<Manifold2D/Geometry/|g' \
      -e 's|<Arcane/Util/BitSet.hpp>|<Manifold2D/Core/BitSet.hpp>|g' \
      -e 's|<Arcane/Util/FunctionRef.hpp>|<Manifold2D/Core/FunctionRef.hpp>|g' \
      -e 's|<Arcane/Jobs/TaskExecutor.hpp>|<Manifold2D/Core/WorkScheduler.hpp>|g' \
      -e 's|<Arcane/Math/Simd.hpp>|<Manifold2D/Core/Simd.hpp>|g' \
      -e 's/\bITaskExecutor\b/IWorkScheduler/g' \
      -e 's/\bSerialTaskExecutor\b/SerialWorkScheduler/g' \
      -e 's/\bnamespace Arcane\b/namespace Manifold2D/g' \
      -e 's/\bArcane::/Manifold2D::/g' \
      "$f"
  done
  ```
  Then GREP-PROOF the moved tree: `rg -n "Arcane" ThirdParty/Manifold2D/include/Manifold2D/{Physics,Geometry}
  ThirdParty/Manifold2D/src/{Physics,Geometry}` -> ZERO hits (includes, namespaces, and
  `Arcane::` all gone). If any `ARCANE_`-prefixed BUILD MACRO appears (e.g. `ARCANE_DEBUG`,
  `ARCANE_ASSERT`), that is NOT a namespace token -- the `\bArcane::\b`/`\bnamespace Arcane\b`
  patterns above won't have touched it, and it must STAY (it is defined by the premake config). Only a
  bare `Arcane` word or `<Arcane/...>` path is a defect here; re-inspect any such hit.
- [ ] **Step 3: Rework the 61 test consumers + docs example (mechanical).** These use file-scope
  `using namespace Arcane::Physics;` / `Arcane::Geometry;` and `Arcane::Physics::`/`Arcane::Geometry::`
  qualifiers, plus 2 test-local helper-namespace re-opens. Apply over `Arcane/Tests/src` (the 61
  physics/geometry/sandbox test files) and `docs/examples/arcane-physics-example.cpp`:
  ```bash
  tf=$(rg -l "Arcane::(Physics|Geometry)|<Arcane/(Physics|Geometry)/" Arcane/Tests/src docs/examples)
  for f in $tf; do
    sed -i \
      -e 's|<Arcane/Physics/|<Manifold2D/Physics/|g' \
      -e 's|<Arcane/Geometry/|<Manifold2D/Geometry/|g' \
      -e 's/\bArcane::Physics\b/Manifold2D::Physics/g' \
      -e 's/\bArcane::Geometry\b/Manifold2D::Geometry/g' \
      "$f"
  done
  ```
  Then handle the tests that ALSO reference the moved primitives directly (from the survey):
  `PhysicsSimdSolverTest.cpp`, `SolverMtInvarianceTest.cpp`, `BroadphaseMtInvarianceTest.cpp`,
  `PhysicsNarrowphaseMtTest.cpp`, `TaskExecutorTest.cpp`(only if it tests physics -- it does NOT, it
  tests `Arcane::ITaskExecutor`; LEAVE it), and any test including `<Arcane/Jobs/TaskExecutor.hpp>` to
  drive physics. For each physics test that injects an executor, switch `<Arcane/Jobs/TaskExecutor.hpp>`
  -> `<Manifold2D/Core/WorkScheduler.hpp>` and `Arcane::ITaskExecutor`/`SerialTaskExecutor` ->
  `Manifold2D::IWorkScheduler`/`SerialWorkScheduler`. (Grep to find them:
  `rg -l "ITaskExecutor|SerialTaskExecutor" Arcane/Tests/src` then triage: physics-driving ones flip,
  the pure `Arcane::ITaskExecutor` engine test `TaskExecutorTest.cpp`/`RuntimeTest.cpp` stay.)
  `GeometryVec2Test.cpp` uses `<Arcane/Math/Simd.hpp>` for the f32w case -> `<Manifold2D/Core/Simd.hpp>`
  + `Arcane::Simd` -> `Manifold2D::Simd`.
- [ ] **Step 4: Rework the 8 Sandbox consumers.** Include paths `<Arcane/{Physics,Geometry}/...>` ->
  `<Manifold2D/...>`; the local `namespace Phys = Arcane::Physics;` / `Geo = Arcane::Geometry;` alias
  declarations -> `Manifold2D::`; the forward-decl `Interaction.hpp:46 namespace Arcane::Physics {
  class PhysicsWorld; }` -> `namespace Manifold2D::Physics { class PhysicsWorld; }`. Same scripted sed
  as Step 3 works for the includes + `Arcane::Physics`/`Arcane::Geometry` tokens; hand-verify the
  forward-decl and the alias lines afterward.
- [ ] **Step 5: Rework the 3 engine-DLL consumers (needs care -- spec D5).**
  - `Scene/PhysicsComponents.hpp`: include-path + `Physics::` refs. It sits in `namespace Arcane` and
    writes bare `Physics::BodyType` etc. Add at file scope (after includes): `namespace { namespace Phys
    = Manifold2D::Physics; }` is WRONG inside a header (anonymous ns in header) -- instead write the
    alias INSIDE the code's own namespace: in each place add `namespace Phys = Manifold2D::Physics;` in
    the enclosing `namespace Arcane {` block, then `Physics::` -> `Phys::`. The `:152` reflection
    re-open `namespace Arcane::Physics { ... }` (reflects `BodyType`/`ShapeKind`) -> change to
    `namespace Manifold2D::Physics { ... }` (the reflected types now live there). Verify the reflection
    macro works from that namespace (read how the macro resolves the type).
  - `Scene/PhysicsSystem.hpp`: include-path + heavy bare `Physics::` usage (lines ~89-266). Add
    `namespace Phys = Manifold2D::Physics;` inside its `namespace Arcane {` and rewrite `Physics::` ->
    `Phys::` (scripted sed on this file: `s/\bPhysics::/Phys::/g` AFTER adding the alias -- but GUARD
    against hitting `Manifold2D::Physics::` which would become `Manifold2D::Phys::`; do the alias-add,
    then `s/([^:])\bPhysics::/\1Phys::/g`, then eyeball). This file also hosts the ADAPTER (Step 6).
  - `Render/PhysicsDebugDraw.{hpp,cpp}`: `.cpp` include-paths + `Arcane::Physics`/bare `Physics::`
    refs; `.hpp:31` forward-decls in `namespace Physics` (inside Arcane) -> forward-decl in
    `namespace Manifold2D::Physics`. Update the `.cpp` includes + qualifiers like the others.
- [ ] **Step 6: Add the engine threading adapter.** The engine's enkiTS executor implements
  `Arcane::ITaskExecutor`; Manifold2D wants a `Manifold2D::IWorkScheduler`. Add a small adapter where
  the engine constructs/drives the physics world (in `Scene/PhysicsSystem.hpp`, near where it holds the
  `PhysicsWorld` + injects the executor):
  ```cpp
  // Bridges the engine's Arcane::ITaskExecutor to Manifold2D's injected
  // IWorkScheduler seam (Manifold2D owns its threading interface; Arcane supplies
  // the enki-backed implementation). Forwards ParallelFor; wraps the callback
  // because the FunctionRef types differ across the two libraries.
  class ArcaneWorkScheduler final : public Manifold2D::IWorkScheduler
  {
  public:
      explicit ArcaneWorkScheduler(Arcane::ITaskExecutor& exec) noexcept : m_exec(exec) {}
      void ParallelFor(std::size_t count, std::size_t minBatch,
                       Manifold2D::FunctionRef<void(std::size_t, std::size_t, std::uint32_t)> fn) override
      {
          m_exec.ParallelFor(count, minBatch,
              [&](std::size_t b, std::size_t e, std::uint32_t w) { fn(b, e, w); });
      }
      std::uint32_t WorkerCount() const noexcept override { return m_exec.WorkerCount(); }
  private:
      Arcane::ITaskExecutor& m_exec;
  };
  ```
  Wire it: wherever `PhysicsSystem` currently calls `world.SetExecutor(engineExecutor)` (which used to
  take an `Arcane::ITaskExecutor*`), it now holds an `ArcaneWorkScheduler` member built from the engine
  executor and passes `&adapter` (a `Manifold2D::IWorkScheduler*`). Read the current SetExecutor
  call-site in `PhysicsSystem.hpp` and adapt exactly. If the Sandbox also injects an executor into the
  world directly, apply the same adapter there (`SandboxApp.cpp` includes `<Arcane/Jobs/TaskExecutor.hpp>`
  -- check whether it feeds the physics world an executor; if so, adapt).
- [ ] **Step 7: Regen + build the WHOLE solution + triage.** From `<WT>\Arcane\`: regen premake (files
  moved -> globs changed), `/t:Rebuild` Debug. Fix compile/link fallout (expected classes: a missed
  bare `Physics::` in an engine file; a test whose executor injection wasn't flipped; a forward-decl
  mismatch). Record every non-mechanical fix in the task report (file:line + fix).
- [ ] **Step 8: Full value-exact gate.**
  - `.\ArcaneTests.exe "~[gpu]"` from the ArcaneTests exe dir -> ALL PASS, **177282 / 589 + [manifold2d]**,
    every existing value unchanged.
  - `.\ArcaneTests.exe "[determinism]"` twice back-to-back -> green both, identical.
  - `.\ArcaneTests.exe "[geometry]"` -> **118815 / 17** unchanged.
- [ ] **Step 9: Server gate (the glob auto-shrink).** From `<WT>\Server\`: regen premake, build Debug.
  ArcaneCore must now compile WITHOUT the physics/geometry files (they left `Core/src`). Run
  `CommonTests` (420/66), `AuthTests` (34/12), `CombatTests` (1/1) foreground from their own bin dirs
  -> unchanged. (Server still references `IncludeDir["glm"]`; harmless now, removed in T3.)
- [ ] **Step 10: Commit.**
  ```bash
  git add -- ThirdParty/Manifold2D Arcane/Core/src Arcane/Arcane/src Arcane/Sandbox/src \
             Arcane/Tests/src docs/examples/arcane-physics-example.cpp
  # (git mv already staged the moves; add the edits. Verify `git status` shows the Core dirs deleted
  #  and the Manifold2D tree added, plus consumer edits. NEVER `git add -A`.)
  git commit -F <msgfile>   # refactor(manifold2d)!: lift Physics+Geometry to ThirdParty/Manifold2D, rename to Manifold2D::, rework all consumers (no shims); IWorkScheduler seam adapted engine-side (value-exact)
  ```

---

### Task 3: Cleanup, grep-proofs, docs, exit gauntlet

**Files:**
- Delete: `Arcane/Core/src/Arcane/Util/BitSet.hpp`, `Arcane/Core/src/Arcane/Math/Simd.hpp` +
  `Simd_{Scalar,AVX2,NEON}.inl`, `Arcane/Core/src/Arcane/Math/SimdSmoke.cpp` (retarget -- below).
- Modify: `Arcane/Tests/src/BitSetTest.cpp`, `Arcane/Tests/src/SimdSmoke*`/wherever the Simd suite
  lives, `Server/premake5.lua`, `CLAUDE.md`.

- [ ] **Step 1: Confirm the Core copies are dead, then remove.** After T2, physics (the only production
  user of `Arcane::BitSet`/`Arcane::Simd`) has left Core. Confirm: `rg -n "Arcane::BitSet|<Arcane/Util/BitSet.hpp>"
  Arcane Server` should show ONLY `BitSetTest.cpp`; `rg -n "Arcane::Simd|<Arcane/Math/Simd" Arcane Server`
  should show ONLY `SimdSmoke.cpp` + the Simd unit test. (If anything ELSE appears, STOP -- something in
  the engine used them; re-evaluate before deleting.) Then:
  - **BitSet:** delete `Arcane/Core/src/Arcane/Util/BitSet.hpp`; repoint `Arcane/Tests/src/BitSetTest.cpp`
    to `#include <Manifold2D/Core/BitSet.hpp>` + `Manifold2D::BitSet`.
  - **Simd:** delete `Arcane/Core/src/Arcane/Math/Simd.hpp` + the 3 `.inl`. `SimdSmoke.cpp` (Core) tested
    `Arcane::Simd` -- move its assertions onto `Manifold2D::Simd`: `git mv
    Arcane/Core/src/Arcane/Math/SimdSmoke.cpp Arcane/Tests/src/Manifold2DSimdTest.cpp` (it belongs in the
    test project now, not the Core lib), retarget the include to `<Manifold2D/Core/Simd.hpp>` + namespace,
    and retag its Catch2 tags to `[manifold2d][simd]`. Verify it was actually a Catch2 test file (read it);
    if instead it was a runtime self-check compiled into Core, relocate accordingly (a Manifold2D-side
    self-test or an ArcaneTests case). The `Arcane/Core/src/Arcane/Math/` dir may now be empty -- if so,
    it disappears; if other Math files remain, leave them.
- [ ] **Step 2: Remove the dead Server glm include.** In `Server/premake5.lua` delete
  `IncludeDir["glm"]` (line ~57) and the two physics-rationale comments (lines ~57, ~93) that reference
  the now-departed physics module. (Confirm nothing else in Server uses glm first: `rg -n "glm"
  Server` -> only the premake lines.)
- [ ] **Step 3: Grep-proofs (the standalone contract).** From `<WT>`:
  - `rg -n "Arcane" ThirdParty/Manifold2D` -> ZERO (the vendored lib is Arcane-free: no includes, no
    namespace, no `ARCANE_` macro leaked in -- if a build macro is genuinely needed, it must be defined
    by the wrapper, not inherited from an Arcane name; re-inspect any hit).
  - `rg -n "Arcane::Physics|Arcane::Geometry" Arcane Server docs` -> ZERO (every consumer reworked).
  - `rg -n "<Arcane/(Physics|Geometry)/" Arcane Server docs` -> ZERO (no stale include paths).
  - `rg -n "namespace Arcane::Physics|namespace Arcane::Geometry" Arcane` -> ZERO.
- [ ] **Step 4: Rebuild + full re-gate after the deletions.** From `<WT>\Arcane\`: regen (files removed),
  `/t:Rebuild` Debug; `.\ArcaneTests.exe "~[gpu]"` -> ALL PASS, count = T2's (minus any SimdSmoke cases
  that merged into `[manifold2d]`, plus none new); `[manifold2d]` includes the migrated Simd cases. From
  `<WT>\Server\`: regen + build + the 3 suites, unchanged.
- [ ] **Step 5: Release + Dist + [perf].** From `<WT>\Arcane\`: build `/p:Configuration=Release`, run
  `.\ArcaneTests.exe "[perf]"` from the Release ArcaneTests dir -> tripwire green (Manifold2D is the
  hottest code; expect neutral -- a static-lib boundary does not change inlining within the lib). Build
  `/p:Configuration=Dist` -> clean.
- [ ] **Step 6: CLAUDE.md + docs.** Update the Arcane build-system section: note that the 2D physics +
  geometry engine now lives in `ThirdParty/Manifold2D/` as a standalone `Manifold2D::` static library
  (Astra-style pickable component; own `Core/` primitives incl. the `IWorkScheduler` threading seam;
  zero Arcane deps), consumed by the Arcane DLL / Sandbox / ArcaneTests; Server no longer compiles it.
  Update the "Build System (Arcane engine)" Core description (Core no longer contains Physics/Geometry).
  Fix any other CLAUDE.md line that locates physics under `Arcane/Core/src/Arcane/Physics`.
- [ ] **Step 7: Exit gauntlet** (display permitting -- if in a Parsec/virtual-display session, DEFER
  these to the desk and say so): `.\ArcaneTests.exe "[gpu]"` -> **611 / 37** unchanged; from
  `<WT>\Arcane\bin\Debug-windows-x86_64-md\Loom\`: `Loom.exe --backend vulkan --frames 180` then
  `--backend dx12 --frames 180` (both exit 0 -- Sandbox links Manifold2D and drives the physics scenes).
- [ ] **Step 8: Commit.**
  ```bash
  git add -- Arcane/Core/src Arcane/Tests/src Server/premake5.lua CLAUDE.md
  git commit -F <msgfile>   # chore(manifold2d): remove dead Core BitSet/Simd copies + Server glm, migrate SimdSmoke, grep-proof standalone, CLAUDE.md
  ```

---

## Self-Review Notes

- **Spec coverage:** D1 layout -> T1 (Core primitives + wrapper) + T2 (Physics/Geometry move). D2
  IWorkScheduler seam -> T1 Steps 1-2 (rename) + T2 Step 6 (adapter). D3 per-util disposition -> T1
  (copy FunctionRef/WorkScheduler, create BitSet/Simd) + T2 (physics switches to Manifold2D primitives)
  + T3 (delete dead Core BitSet/Simd, keep Arcane FunctionRef/ITaskExecutor). D4 premake -> T1 Step 4-5.
  D5 consumer rework -> T2 Steps 3-5. D6 non-goals -> nothing in any task changes behavior.
- **Green at every task:** T1 additive (existing suite unchanged + smoke). T2 the atomic cutover
  (value-exact suite + Server). T3 dead-code removal (re-gate). No intermediate red state is shipped.
- **Adaptation notes are pinned, not vague:** the BitSet/Simd smoke-test API calls and the SimdSmoke
  relocation each cite the exact file to read and pin the assertion INTENT that must not change; the
  engine `Physics::`->`Phys::` rewrite pins the guard against double-qualification.
- **Risk that a `git mv`'d file's history-vs-content confuses the sed:** Step 2 operates on the NEW
  paths post-`git mv`; the grep-proof at the end of each step is the tripwire.
