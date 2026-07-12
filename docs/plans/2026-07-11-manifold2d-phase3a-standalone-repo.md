# Manifold2D Phase 3a -- standalone cross-platform repo (Windows-first) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (INLINE, not subagent-driven -- this plan creates dirs OUTSIDE the monorepo and pushes to a PUBLIC GitHub repo, which is outward-facing and interactive; do those steps in the main session, pausing for user confirmation before the GitHub push). Steps use checkbox (`- [ ]`) syntax.

**Goal:** Extract `ThirdParty/Manifold2D` into its own standalone, Windows-green repository
(`D:\dev\starworks\Manifold2D`, published to github.com/T3mps/Manifold2D), migrate the pure physics
tests into it, and re-vendor it back into Aphelyon -- scaffolded so the Linux port (Part B) is a
drop-in. Value/behavior identical.

**Architecture:** Astra-model synced copies: standalone source `D:\dev\starworks\Manifold2D` ->
GitHub clone `D:\dev\github\Manifold2D` (publish) -> Aphelyon `ThirdParty/Manifold2D` (vendor). The
standalone repo is a premake `workspace` (lib + `Manifold2DTests` over vendored Catch2/rapidcheck +
a `std::thread` test scheduler). Aphelyon consumes it via an inline `project "Manifold2D"` in
Arcane's premake (decoupled from the vendored workspace file, exactly as Aphelyon consumes Astra).

**Tech Stack:** C++23, premake5 (vs2022 + gmake2), MSVC (VS 2026), Catch2 v3, rapidcheck, robocopy,
GitHub (gh CLI). Spec: `docs/superpowers/specs/2026-07-11-manifold2d-phase3a-standalone-repo-design.md`
(READ IT FIRST -- D1-D6 govern).

## Global Constraints

- **This branch:** `manifold2d-phase3a` (worktree `D:\dev\starworks\Gacha\.claude\worktrees\manifold2d-phase3a`,
  = `<WT>`), based on local `main` @16786a6f (has Phase 2). Aphelyon-side changes land here.
- **Three locations** (create if absent): standalone source `D:\dev\starworks\Manifold2D` (= `<M2D>`),
  GitHub clone `D:\dev\github\Manifold2D` (= `<GH>`), Aphelyon vendored copy `<WT>\ThirdParty\Manifold2D`.
  `<M2D>` and `<GH>` are OUTSIDE the monorepo/worktree -- not git-worktree-isolated.
- **Source of the lib content:** the Phase-2 output already in `<WT>\ThirdParty\Manifold2D` (include/ +
  src/, `Manifold2D::`, zero Arcane deps). Part A MOVES it into `<M2D>`; it is not rewritten.
- **Value-exact:** the migrated tests keep their expected values. After re-vendor, Aphelyon's `~[gpu]`
  count = the pre-Phase-3 count (177296/593) MINUS the migrated assertions/cases PLUS the link-smoke;
  the plan computes the exact new number in T2/T4 (never "unchanged" -- a real regression must still fail).
- **Build (PowerShell):** premake `& "<WT>\ThirdParty\premake5\premake5.exe" <action>`; MSBuild
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"`.
  `VCPKG_ROOT` = `D:\dev\_shared\tools\vcpkg` (Aphelyon only; the standalone repo has no vcpkg deps).
  Tests foreground from the exe dir.
- **Outward-facing gate:** the GitHub push (T3) publishes to a PUBLIC repo -- PAUSE and get explicit
  user confirmation before pushing. Never force-push over the existing README/LICENSE history.
- Commits: conventional, NO AI trailers, exact paths. ASCII-only, UTF-8 no BOM. `.github`/`.gitignore`
  are real files here (not ignored) in `<M2D>`.
- **Part A ships NO red Linux CI** -- the `gmake2`/linux pieces are scaffolded (present, unexercised);
  the linux CI job + the g++/clang burn-down are Part B.

## File Structure

| File | Responsibility |
|---|---|
| `<M2D>/premake5.lua` (new) | Standalone `workspace "Manifold2D"`: Manifold2D lib + Manifold2DTests. |
| `<M2D>/include/Manifold2D/**`, `<M2D>/src/**` (moved) | The Phase-2 lib content. |
| `<M2D>/tests/**` (migrated) | The pure physics/geometry/SIMD/BitSet Catch2 suites. |
| `<M2D>/tests/Support/TestWorkScheduler.hpp` (new) | `std::thread` `Manifold2D::IWorkScheduler` for MT tests. |
| `<M2D>/vendor/Catch2`, `<M2D>/vendor/rapidcheck` (copied) | Standalone test deps + their premake wrappers. |
| `<M2D>/scripts/{generate_vs2022.bat,generate_linux.sh,sync_to_github.ps1,vendor.ps1}` (new) | Gen + sync + vendor. |
| `<M2D>/{.gitignore,LICENSE,README.md,CLAUDE.md}` (new) | Repo-root files. |
| `<M2D>/.github/workflows/ci.yml` (new) | windows-msvc job (linux job = Part B). |
| `<WT>/Arcane/premake5.lua` (modify) | Replace `include "../ThirdParty/Manifold2D"` with inline `project "Manifold2D"`. |
| `<WT>/Arcane/Tests/src/Manifold2DLinkSmokeTest.cpp` (new) | Thin vendored-lib link smoke (`[manifold2d]`). |
| `<WT>/Arcane/Tests/src/<migrated tests>` (delete) | Removed from ArcaneTests (now in `<M2D>/tests`). |
| `<WT>/ThirdParty/Manifold2D/**` (re-vendored) | Full mirror of `<M2D>` via `vendor.ps1`. |

---

### Task 1: Stand up the standalone repo skeleton + move the lib content

**Files:** create `<M2D>/` tree (structure above), move `<WT>\ThirdParty\Manifold2D\{include,src}` into it.

- [ ] **Step 1: Survey the exact migrating test set (feeds T2; do it now while both trees exist).**
  From `<WT>`, classify every `Arcane/Tests/src/*` file that references Manifold2D:
  ```bash
  # MOVERS: reference Manifold2D but NO Arcane engine header
  for f in $(rg -l "Manifold2D::|<Manifold2D/" Arcane/Tests/src); do
    if rg -q "<Arcane/(Scene|Render|Base|Jobs|Platform)/|Arcane::PhysicsSystem|Arcane::PhysicsResource|Arcane::PhysicsBodyRef|Arcane::EngineContext|Sandbox" "$f"; then
      echo "STAY  $f"; else echo "MOVE  $f"; fi
  done | sort | tee "$TMP/m2d-test-split.txt"
  ```
  Record the MOVE list + STAY list in the task report. Expected STAY: `PhysicsSystemTest`,
  `PhysicsComponentsTest`, `PhysicsDebugDrawTest`, `PhysicsDebugRichTest`, `PhysicsDebugAccessorsTest`,
  `SandboxInteractionTest`, `SandboxHudTest`, `SandboxVisualsTest` (+ verify `PhysicsDebugCapsuleTest`).
  Expected MOVE: the rest (~55: dynamics/solver/broadphase/narrowphase/joints/island/contact/queries/
  ccd/determinism/invariants/perf/smoke/rotation/... + `GeometryVec2Test`, `ConvexHullTest`, `BitSetTest`,
  `SimdWideTest`, `SimdWideScalarTest`, `Manifold2DCoreTest`, the 3 MT-invariance tests). ALSO note the
  shared `.inl`: `Arcane/Tests/src/Simd/SimdWideTests.inl` moves with the SIMD tests.
- [ ] **Step 2: Record the pre-migration baseline counts.** From `<WT>\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`
  (already built): `.\ArcaneTests.exe "~[gpu]"` -> confirm 177296/593; capture `[geometry]`, `[determinism]`,
  `[simd]`, `[bitset]`, `[manifold2d]` subset counts. These pin the "what left" arithmetic for T4.
- [ ] **Step 3: Create the standalone tree + move the lib content.** (Git Bash.)
  ```bash
  M2D="/d/dev/starworks/Manifold2D"
  mkdir -p "$M2D"/{include,src,tests/Support,scripts,vendor,.github/workflows}
  # Move the Phase-2 lib content OUT of the Aphelyon worktree copy into the standalone source:
  cp -r "/d/dev/starworks/Gacha/.claude/worktrees/manifold2d-phase3a/ThirdParty/Manifold2D/include/." "$M2D/include/"
  cp -r "/d/dev/starworks/Gacha/.claude/worktrees/manifold2d-phase3a/ThirdParty/Manifold2D/src/."     "$M2D/src/"
  # (T4's vendor.ps1 will re-materialize ThirdParty/Manifold2D from <M2D>; do NOT delete it in the WT yet.)
  ```
- [ ] **Step 4: Vendor Catch2 + rapidcheck into the standalone repo.** Copy from Aphelyon's ThirdParty
  (their premake wrappers honor `THIRDPARTY_STATICRUNTIME or "on"` -> static CRT by default, correct for
  a standalone test exe):
  ```bash
  cp -r "/d/dev/starworks/Gacha/ThirdParty/Catch2"     "$M2D/vendor/Catch2"
  cp -r "/d/dev/starworks/Gacha/ThirdParty/rapidcheck" "$M2D/vendor/rapidcheck"
  rm -rf "$M2D"/vendor/*/{bin,bin-int,ide,ide-md,.vs}   # drop any build artifacts
  ```
- [ ] **Step 5: Write the repo-root files.** `<M2D>/.gitignore` (ignore `bin/ bin-int/ ide/ ide-md/ .vs/
  *.user *.slnx *.sln`), `<M2D>/LICENSE` (MIT, "Copyright (c) 2026 Ethan Temprovich / Starworks" -- match
  the text already on github.com/T3mps/Manifold2D; fetch it with `gh api repos/T3mps/Manifold2D/contents/LICENSE`
  if unsure), `<M2D>/README.md` (component overview: Manifold2D = deterministic 2D physics modeled on
  Box2D v3; `Manifold2D::` namespace; zero deps; inject an `IWorkScheduler` for MT; MKS units; build via
  `scripts/generate_vs2022.bat`), `<M2D>/CLAUDE.md` (short: layout, `Manifold2D::` namespace, the
  IWorkScheduler seam, "no threads created by the lib", build/test commands).
- [ ] **Step 6: Write the workspace premake + gen scripts** (see Task 2 for the exact `premake5.lua`,
  which references the tests project). Write `scripts/generate_vs2022.bat`
  (`@echo off` + `"%~dp0..\..\Gacha\ThirdParty\premake5\premake5.exe" vs2022`? -- NO: the standalone repo
  must not depend on Aphelyon; copy `premake5.exe` into `<M2D>/vendor/premake5/` and call that) and
  `scripts/generate_linux.sh` (`premake5 gmake2`). Copy premake5.exe:
  `cp -r "/d/dev/starworks/Gacha/ThirdParty/premake5" "$M2D/vendor/premake5"`.
- [ ] **Step 7: git init + first local commit** (NOT pushed yet).
  ```bash
  cd "$M2D" && git init -q && git add -A && git commit -q -m "chore: initial import -- Manifold2D lib + scaffolding (from Aphelyon Phase 2)"
  ```
- [ ] **Step 8: Report** the tree + the T1-Step-1 test split to the task report. (No Aphelyon commit yet;
  Aphelyon-side changes are T4.)

---

### Task 2: Migrate tests + TestWorkScheduler; standalone Windows-green

**Files:** create `<M2D>/premake5.lua`, `<M2D>/tests/Support/TestWorkScheduler.hpp`; move the MOVE-list
test files into `<M2D>/tests/`.

**Interfaces:**
- Produces: `Manifold2D::Testing::TestWorkScheduler` (a `Manifold2D::IWorkScheduler` impl) for the MT tests.
- Consumes: T1's lib content + vendored Catch2/rapidcheck.

- [ ] **Step 1: Move the MOVE-list test files** (from T1-Step-1) into `<M2D>/tests/`, preserving the
  `Simd/` subdir:
  ```bash
  M2D="/d/dev/starworks/Manifold2D"; WT="/d/dev/starworks/Gacha/.claude/worktrees/manifold2d-phase3a"
  # (use the MOVE list from $TMP/m2d-test-split.txt)
  while read -r verdict f; do [ "$verdict" = MOVE ] && { mkdir -p "$M2D/tests/$(dirname "${f#Arcane/Tests/src/}")"; cp "$WT/$f" "$M2D/tests/${f#Arcane/Tests/src/}"; }; done < "$TMP/m2d-test-split.txt"
  cp "$WT/Arcane/Tests/src/Simd/SimdWideTests.inl" "$M2D/tests/Simd/SimdWideTests.inl"
  ```
  (Deletion from ArcaneTests happens in T4, after the standalone repo is proven green -- keep both until then.)
- [ ] **Step 2: Write `tests/Support/TestWorkScheduler.hpp`** -- a real-threads `IWorkScheduler` for the
  MT-invariance tests (the lib creates no threads; this is test-only, mirroring Astra's `TestWorkerPool`):
  ```cpp
  #pragma once
  // Test-only std::thread pool implementing Manifold2D::IWorkScheduler, so the MT-invariance
  // suites can exercise parallel ParallelFor. The library itself creates no threads.
  #include <Manifold2D/Core/WorkScheduler.hpp>
  #include <atomic>
  #include <cstdint>
  #include <functional>
  #include <thread>
  #include <vector>

  namespace Manifold2D::Testing
  {
      // Minimal blocking parallel-for: partitions [0,count) into WorkerCount() contiguous
      // ranges and runs them on worker threads (calling thread participates as worker 0).
      // Deterministic partitioning (contiguous, ascending) so results match the serial path.
      class TestWorkScheduler final : public Manifold2D::IWorkScheduler
      {
      public:
          explicit TestWorkScheduler(std::uint32_t workers)
              : m_workers(workers < 1 ? 1u : workers) {}

          void ParallelFor(std::size_t count, std::size_t minBatch,
                           Manifold2D::FunctionRef<void(std::size_t, std::size_t, std::uint32_t)> fn) override
          {
              if (count == 0) return;
              std::uint32_t w = m_workers;
              std::size_t grain = (count + w - 1) / w;
              if (grain < (minBatch ? minBatch : 1)) grain = (minBatch ? minBatch : 1);
              // spawn workers 1..k for the tail ranges; run range 0 inline as worker 0.
              std::vector<std::thread> pool;
              std::uint32_t wid = 0;
              for (std::size_t begin = 0; begin < count; begin += grain, ++wid)
              {
                  std::size_t end = begin + grain; if (end > count) end = count;
                  if (begin == 0) continue; // range 0 runs inline below
                  pool.emplace_back([&, begin, end, wid] { fn(begin, end, wid); });
              }
              std::size_t firstEnd = grain < count ? grain : count;
              fn(0, firstEnd, 0);
              for (auto& t : pool) t.join();
          }

          std::uint32_t WorkerCount() const noexcept override { return m_workers; }

      private:
          std::uint32_t m_workers;
      };
  }
  ```
  NOTE to implementer: the ORIGINAL enki executor guarantees each concurrently-running sub-range a
  DISTINCT worker id in `[0,WorkerCount())` for per-worker scratch. This pool assigns ascending ids but
  spawns a thread per range -- if a moved MT test sizes per-worker scratch by `WorkerCount()` and indexes
  by the `worker` arg, ensure the number of ranges never exceeds `WorkerCount()` (set grain so range-count
  == workers: `grain = ceil(count/workers)`, which the code does). Verify against the actual MT-test
  assertions; if a test needs the exact enki semantics, mirror them.
- [ ] **Step 3: Repoint the MT tests** (`SolverMtInvarianceTest`, `BroadphaseMtInvarianceTest`,
  `PhysicsNarrowphaseMtTest`) in `<M2D>/tests/`: replace their `#include <Arcane/Jobs/...>` / enki
  executor construction with `#include "Support/TestWorkScheduler.hpp"` +
  `Manifold2D::Testing::TestWorkScheduler sched(N);`. Any `Manifold2D::SerialWorkScheduler` usage stays.
  (Grep the three files for how they build the executor today and swap exactly.)
- [ ] **Step 4: Write `<M2D>/premake5.lua`** -- the standalone workspace:
  ```lua
  workspace "Manifold2D"
      architecture "x64"
      configurations { "Debug", "Release", "Dist" }
      startproject "Manifold2DTests"
      location "."
      outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

      IncludeDir = {}
      IncludeDir["Manifold2D"] = "include"
      IncludeDir["Catch2"]     = "vendor/Catch2/src"
      IncludeDir["rapidcheck"] = "vendor/rapidcheck/include"
      IncludeDir["rapidcheck_catch"] = "vendor/rapidcheck/extras/catch/include"

      -- static-CRT wrappers (Catch2/rapidcheck honor these globals; defaults = static)
      THIRDPARTY_STATICRUNTIME    = "on"
      THIRDPARTY_PROJECT_LOCATION = "."

      filter "system:windows"
          systemversion "latest"
          buildoptions { "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
      filter "system:linux"                                    -- PART A SCAFFOLD (exercised in Part B)
          buildoptions { "-mavx2", "-ffp-contract=off", "-fno-fast-math", "-Wall", "-Wextra" }
          links { "pthread" }
      filter {}

      group "Dependencies"
          include "vendor/Catch2"
          include "vendor/rapidcheck"
      group ""

      project "Manifold2D"
          kind "StaticLib"
          language "C++"
          cppdialect "C++23"
          location "ide"
          staticruntime "on"
          floatingpoint "Strict"
          targetdir ("bin/" .. outputdir .. "/%{prj.name}")
          objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")
          files { "include/**.hpp", "include/**.inl", "src/**.cpp" }
          includedirs { "%{IncludeDir.Manifold2D}" }
          filter "configurations:Debug"   runtime "Debug"   symbols "on"                       defines { "MANIFOLD2D_DEBUG" }
          filter "configurations:Release"  runtime "Release" optimize "speed" symbols "on"      defines { "MANIFOLD2D_RELEASE", "NDEBUG" }
          filter "configurations:Dist"     runtime "Release" optimize "speed" symbols "off"     defines { "MANIFOLD2D_DIST", "NDEBUG" }
          filter {}

      project "Manifold2DTests"
          kind "ConsoleApp"
          language "C++"
          cppdialect "C++23"
          location "ide"
          staticruntime "on"
          floatingpoint "Strict"
          targetdir ("bin/" .. outputdir .. "/%{prj.name}")
          objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")
          files { "tests/**.hpp", "tests/**.inl", "tests/**.cpp" }
          includedirs { "%{IncludeDir.Manifold2D}", "%{IncludeDir.Catch2}", "%{IncludeDir.rapidcheck}", "%{IncludeDir.rapidcheck_catch}", "tests" }
          links { "Manifold2D", "Catch2", "rapidcheck" }
          filter "system:windows" systemversion "latest" buildoptions { "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
          filter "configurations:Debug"   runtime "Debug"   symbols "on"                    defines { "MANIFOLD2D_DEBUG" }
          filter "configurations:Release"  runtime "Release" optimize "speed" symbols "on"
          filter "configurations:Dist"     runtime "Release" optimize "speed" symbols "off"
          filter {}
  ```
  NOTE: adapt to the ACTUAL Catch2/rapidcheck wrapper project names + include layout (read
  `vendor/Catch2/premake5.lua` + `vendor/rapidcheck/premake5.lua` -- the Aphelyon copies -- for the exact
  `project "..."` names and `includedirs`; the `%{IncludeDir.*}` above assume Aphelyon's layout). If the
  tests include Catch2 via `<catch2/catch_test_macros.hpp>` and rapidcheck via `<rapidcheck.h>` /
  `<rapidcheck/catch.h>`, the include dirs above resolve them.
- [ ] **Step 5: Generate + build + run (Windows-green).** From `<M2D>`:
  `.\vendor\premake5\premake5.exe vs2022`; MSBuild `Manifold2D.slnx` (or `.sln`) Debug; run
  `bin\Debug-windows-x86_64\Manifold2DTests\Manifold2DTests.exe` foreground. Triage: missing includes
  (a moved test that pulled an Arcane header slipped the classifier -> it's a STAYER, move it back),
  the MT-scheduler semantics, Catch2/rapidcheck wiring. Expect the moved suites to PASS with their
  known expected values (value-exact). Also build Release + Dist clean. Record the passing count.
- [ ] **Step 6: Commit to the standalone repo** (`git -C <M2D> add -A && commit -m "test: migrate physics/geometry suites + TestWorkScheduler; Windows-green"`).

---

### Task 3: Windows CI + first GitHub push (OUTWARD-FACING -- confirm before pushing)

- [ ] **Step 1: Write `<M2D>/.github/workflows/ci.yml`** -- windows-msvc only (linux job = Part B):
  ```yaml
  name: CI
  on: [push, pull_request]
  jobs:
    windows-msvc:
      runs-on: windows-latest
      steps:
        - uses: actions/checkout@v4
        - name: Download premake
          run: |
            curl -L -o premake.zip https://github.com/premake/premake-core/releases/download/v5.0.0-beta6/premake-5.0.0-beta6-windows.zip
            tar -xf premake.zip
        - name: Generate
          run: .\premake5.exe vs2022
        - name: Build
          run: msbuild Manifold2D.sln /p:Configuration=Release /m
        - name: Test
          run: .\bin\Release-windows-x86_64\Manifold2DTests\Manifold2DTests.exe
  ```
  NOTE: if premake emits `.slnx` (VS2026 solution) rather than `.sln`, CI's `windows-latest` MSBuild may
  not accept `.slnx` -- generate `vs2022` (emits `.sln`) in CI, and confirm the runner's MSBuild builds it.
  Commit to `<M2D>`.
- [ ] **Step 2: Write `scripts/sync_to_github.ps1`** (Astra-shaped robocopy `<M2D>` -> `<GH>`):
  ```powershell
  # Mirror the standalone working copy into the GitHub clone (excludes .git + build artifacts).
  $src = "D:\dev\starworks\Manifold2D"
  $dst = "D:\dev\github\Manifold2D"
  robocopy $src $dst /MIR /NFL /NDL /NJH `
      /XD .git bin bin-int ide ide-md .vs `
      /XF *.user *.slnx *.sln
  if ($LASTEXITCODE -ge 8) { Write-Error "robocopy failed ($LASTEXITCODE)"; exit 1 }
  Write-Host "Synced. Review with: git -C $dst status"; exit 0
  ```
  Commit to `<M2D>`.
- [ ] **Step 3: Clone the GitHub repo + sync.**
  ```powershell
  if (-not (Test-Path "D:\dev\github\Manifold2D")) { git clone https://github.com/T3mps/Manifold2D.git "D:\dev\github\Manifold2D" }
  & "D:\dev\starworks\Manifold2D\scripts\sync_to_github.ps1"
  ```
  This overlays our content onto the clone (which has the initial README/LICENSE). Our README overwrites
  theirs (intended); the MIT LICENSE should match (verify `git -C D:\dev\github\Manifold2D diff -- LICENSE`
  is empty or an acceptable formatting delta).
- [ ] **Step 4: PAUSE -- show the user `git -C D:\dev\github\Manifold2D status` + `git diff --stat` and get
  explicit confirmation before pushing to the PUBLIC repo.** Then:
  ```powershell
  git -C "D:\dev\github\Manifold2D" add -A
  git -C "D:\dev\github\Manifold2D" commit -m "Manifold2D: initial import -- deterministic 2D physics (Box2D v3 model), Windows CI"
  git -C "D:\dev\github\Manifold2D" push origin main
  ```
  Confirm CI goes green on the push (windows-msvc).

---

### Task 4: Re-vendor into Aphelyon + rewire Arcane + prune migrated tests + link-smoke

**Files:** `<M2D>/scripts/vendor.ps1` (new), `<WT>/Arcane/premake5.lua` (modify), delete migrated tests
from `<WT>/Arcane/Tests/src`, `<WT>/Arcane/Tests/src/Manifold2DLinkSmokeTest.cpp` (new), re-materialize
`<WT>/ThirdParty/Manifold2D`.

- [ ] **Step 1: Write `<M2D>/scripts/vendor.ps1`** (robocopy `<M2D>` -> Aphelyon `ThirdParty/Manifold2D`,
  excluding standalone-only build/vendor bits Aphelyon supplies itself):
  ```powershell
  # Vendor the standalone Manifold2D into Aphelyon as a full mirror (like ThirdParty/Astra).
  param([string]$Aphelyon = "D:\dev\starworks\Gacha")
  $src = "D:\dev\starworks\Manifold2D"
  $dst = "$Aphelyon\ThirdParty\Manifold2D"
  robocopy $src $dst /MIR /NFL /NDL /NJH `
      /XD .git bin bin-int ide ide-md .vs vendor `
      /XF *.user *.slnx *.sln
  if ($LASTEXITCODE -ge 8) { Write-Error "robocopy failed ($LASTEXITCODE)"; exit 1 }
  Write-Host "Vendored -> $dst"; exit 0
  ```
  (Excludes `vendor/` -- Aphelyon has its own Catch2/rapidcheck; and the standalone `premake5.lua`
  workspace comes across but Aphelyon ignores it, D4.) Run it targeting `<WT>`:
  `& "D:\dev\starworks\Manifold2D\scripts\vendor.ps1" -Aphelyon "<WT>"`.
- [ ] **Step 2: Rewire Arcane's premake** (`<WT>/Arcane/premake5.lua`): DELETE
  `include "../ThirdParty/Manifold2D"` from `group "Dependencies"`. ADD an inline `project "Manifold2D"`
  (place it near the `Core` project) globbing the vendored source:
  ```lua
      project "Manifold2D"
          kind "StaticLib"
          language "C++"
          cppdialect "C++23"
          location "Manifold2D"
          staticruntime "off"                         -- /MD (engine boundary)
          floatingpoint "Strict"
          targetdir ("bin/" .. outputdir .. "/%{prj.name}")
          objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")
          files { "%{wks.location}/../ThirdParty/Manifold2D/include/**.hpp",
                  "%{wks.location}/../ThirdParty/Manifold2D/include/**.inl",
                  "%{wks.location}/../ThirdParty/Manifold2D/src/**.cpp" }
          includedirs { "%{IncludeDir.Manifold2D}" }
          defines { "_CRT_SECURE_NO_WARNINGS" }
          filter "system:windows" systemversion "latest" buildoptions { "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
          filter "configurations:Debug"   runtime "Debug"   symbols "on"                    defines { "ARCANE_DEBUG" }
          filter "configurations:Release"  runtime "Release" optimize "speed" symbols "on"   defines { "ARCANE_RELEASE", "NDEBUG" }
          filter "configurations:Dist"     runtime "Release" optimize "speed" symbols "off"  defines { "ARCANE_DIST", "NDEBUG" }
          filter {}
  ```
  `IncludeDir["Manifold2D"]` stays `ThirdParty/Manifold2D/include`. `links { "Manifold2D" }` on
  Arcane/Sandbox/ArcaneTests already exist from Phase 2 -- keep them.
- [ ] **Step 3: Delete the migrated test files from ArcaneTests.** `git rm` the MOVE-list files (T1-Step-1)
  from `<WT>/Arcane/Tests/src` (+ `Simd/SimdWideTests.inl` + the `Simd/` dir if empty). Keep the STAY list.
- [ ] **Step 4: Add the link-smoke** `<WT>/Arcane/Tests/src/Manifold2DLinkSmokeTest.cpp`:
  ```cpp
  // Proves the vendored Manifold2D library links + runs inside Aphelyon (the full physics
  // suite now lives in the standalone Manifold2D repo). If the vendor-in breaks, this fails.
  #include <catch2/catch_test_macros.hpp>
  #include <Manifold2D/Physics/PhysicsWorld.hpp>

  TEST_CASE("Manifold2D vendored lib links and a body falls under gravity", "[manifold2d]")
  {
      using namespace Manifold2D::Physics;
      // NOTE to implementer: adapt to the ACTUAL world/body API (read PhysicsWorld.hpp).
      // Intent: create a world with downward gravity, add one dynamic body, step ~30 times,
      // assert its Y increased (engine is Y-down) -- i.e. the linked simulation actually runs.
      WorldDef wd{}; wd.gravity = { 0.0f, 10.0f };
      PhysicsWorld world(wd);
      BodyDef bd{}; bd.type = BodyType::Dynamic; bd.position = { 0.0f, 0.0f };
      BodyHandle b = world.CreateBody(bd);
      const float y0 = world.GetBody(b).position.y;
      for (int i = 0; i < 30; ++i) world.Step(1.0f / 60.0f);
      CHECK(world.GetBody(b).position.y > y0);
  }
  ```
- [ ] **Step 5: Regen + build Aphelyon + gates.** From `<WT>\Arcane\`: premake `vs2026`; `/t:Rebuild` Debug.
  Compute the NEW expected `~[gpu]` count = 177296 minus the migrated assertions (from T2-Step-5's passing
  count) plus the link-smoke's asserts; run `.\ArcaneTests.exe "~[gpu]"` and assert it EQUALS that number
  (not "unchanged"). The moved tags (`[geometry]`, `[simd]`, `[bitset]`, `[determinism]`) should now report
  0 in ArcaneTests. `[manifold2d]` = just the link-smoke. Build Release + Dist clean. From `<WT>\Server\`:
  regen + build (ArcaneCore already physics-free) + `CommonTests 420/66`/`AuthTests 34/12`/`CombatTests 1/1`.
- [ ] **Step 6: Commit the Aphelyon-side changes** on `manifold2d-phase3a`:
  ```bash
  git add ThirdParty/Manifold2D Arcane/premake5.lua Arcane/Tests/src docs/superpowers
  git rm <the migrated test paths>
  git commit -F <msg>   # refactor(manifold2d): vendor standalone repo; Arcane inline project; migrate physics tests out to Manifold2D repo; add link-smoke
  ```

---

### Task 5: Land

- [ ] **Step 1: Final Aphelyon gate** -- full `~[gpu]` at the new count + Server suites green (re-run after
  the commit to be sure). Grep-proof: `rg "include \"../ThirdParty/Manifold2D\"" Arcane/premake5.lua` -> ZERO
  (the include is gone); `rg -l "<Manifold2D/" Arcane/Tests/src` -> only the STAY list + the link-smoke.
- [ ] **Step 2: CLAUDE.md** -- update the Arcane build-system note: Manifold2D is now its OWN repo
  (github.com/T3mps/Manifold2D), developed standalone at `D:\dev\starworks\Manifold2D`, synced to GitHub +
  vendored into `ThirdParty/Manifold2D`; Aphelyon builds it via Arcane's inline project; the physics test
  suite lives in the standalone repo (Aphelyon keeps only a link-smoke). Note Part B (Linux) is pending.
- [ ] **Step 3: Merge to main + push** (user is the merger). FF `manifold2d-phase3a` -> `main`, then
  `git push origin main` -- this carries Phase 2 + Phase 3a to origin together (origin/main was @9b58c9c0).
  The deferred `[gpu]`/Loom desk gauntlet + Aphelyon CI apply. Then Part B (Linux) as its own cycle.

---

## Self-Review

- **Spec coverage:** D1 topology -> T1 (create <M2D>) + T3 (sync_to_github) + T4 (vendor.ps1). D2 structure
  -> T1/T2. D3 test split -> T1-Step1 (classify) + T2 (move + TestWorkScheduler) + T4 (delete from ArcaneTests
  + link-smoke). D4 premake -> T2 (standalone workspace) + T4-Step2 (Arcane inline project). D5 Linux scaffold
  -> T2-Step4 (`filter "system:linux"`) + T2-Step6/generate_linux.sh + T3 (windows-only CI). D6 non-goals ->
  no linux-green, no history transfer, snapshot sync.
- **No placeholders:** the two "adapt to actual API" notes (TestWorkScheduler MT semantics, link-smoke world
  API, premake Catch2/rapidcheck names) each cite the exact file to read and pin the invariant that must hold.
  The test MOVE list is computed by a concrete classifier script, not hand-waved.
- **Type consistency:** `Manifold2D::Testing::TestWorkScheduler` / `Manifold2D::IWorkScheduler` /
  `Manifold2D::FunctionRef` names match across T2. The inline Arcane `project "Manifold2D"` and the standalone
  workspace both glob `include/**`+`src/**`, `/fp:strict`, `/arch:AVX2` (CRT differs: /MD Aphelyon vs static
  standalone -- intentional, D4).
- **Outward-facing:** T3-Step4 is a hard PAUSE-for-confirmation gate before the public push.
