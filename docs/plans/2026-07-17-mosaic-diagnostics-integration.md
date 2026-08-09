# Mosaic Diagnostics Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Arcane the single owner of Mosaic/Manifold2D/Astra diagnostics — install one log sink + assert handler that forward into the engine logger, give Manifold2D a real logging site-map, and re-vendor Astra so it routes through the same seam.

**Architecture:** Adapters live in Arcane's own diagnostics homes (`Base/Log.{hpp,cpp}`, new `Base/Assert.{hpp,cpp}`), forwarding to the single `ARCANE_API Arcane::Log::Engine()` spdlog logger. Per-module inline installers (Mosaic's sink storage is per-module). No `MosaicBridge`, no plugin-ABI change, no Mosaic edit. Then Manifold2D emits logs + input-guards (standalone repo → re-vendor), and Astra is re-vendored with its committed Mosaic adoption.

**Tech Stack:** C++23, Mosaic seam (fn-ptr `LogSink`/`AssertHandler`), spdlog, Catch2, premake5/MSBuild (VS2026), `/MD` Arcane workspace.

Spec: `docs/superpowers/specs/2026-07-17-mosaic-diagnostics-integration-design.md`.

## Global Constraints

- **No Mosaic edit** — the adapter can't move into Mosaic (zero-dep invariant); nothing in `ThirdParty/Mosaic` changes.
- **No plugin-ABI change** — the plugin calls `ARCANE_API` accessors directly; the inline installer runs in the plugin's module.
- **Single engine logger, category as text** — route through `Arcane::Log::Engine()`; the source category (`record.category`) rides in the message (`"[{}] {}"`). NO "Mosaic" logger, NO per-category loggers. `record.message`/`category` are fmt ARGUMENTS (literal format string) so a stray `{}` can't fmt-inject.
- **Adapters are `noexcept`** and wrap the spdlog call in `try{}catch(...){}` — must not throw or re-enter Mosaic.
- **Messages are plain string literals** in Manifold2D (zero-dep seam, no fmt); `source_location` carries file:line.
- Build (Aphelyon): `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`. msbuild at `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` if not on PATH (it is NOT on the Bash PATH — use PowerShell or the full path).
- Regenerate when the file LIST changes (new `.cpp`/`.hpp`, new test): `cd Arcane && GenerateProjects.bat` (Arcane globs `src/**.cpp`).
- Test gate: run from the output dir — `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe ~[gpu]`. Baseline before this work: `~[gpu]` 27608/292.
- Commits: Aphelyon uses `type(scope):` messages, NO AI trailers (repo convention). Standalone Manifold2D/Astra: same, no AI trailers. Commit only at the points the plan names.
- `spdlog::level` map: Trace→trace, Debug→debug, Info→info, Warn→warn, Error→err, Critical→critical, Off→off.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/Arcane/src/Arcane/Base/Log.hpp` | modify | declare `MosaicSink()` accessor + inline `InstallMosaicSink()` |
| `Arcane/Arcane/src/Arcane/Base/Log.cpp` | modify | log-sink adapter impl + accessor |
| `Arcane/Arcane/src/Arcane/Base/Assert.hpp` | create | assert-handler accessor + inline `InstallMosaicHandler()` + `ARC_ASSERT/VERIFY/ENSURE` |
| `Arcane/Arcane/src/Arcane/Base/Assert.cpp` | create | assert-handler adapter impl + accessor |
| `Arcane/Arcane/src/Arcane/Base/Runtime.cpp` | modify | install both in the `Runtime` ctor (Arcane.dll) |
| `Arcane/Loom/src/main.cpp` | modify | install both after `Arcane::Log::Init()` (Loom.exe) |
| `Arcane/Sandbox/src/Sandbox.cpp` | modify | install both in `GamePlugin_Init` (Sandbox.dll) |
| `Arcane/PlaygroundGame/src/PlaygroundGame.cpp` | modify | install both in `GamePlugin_Init` (PlaygroundGame.dll) |
| `Arcane/Tests/src/MosaicDiagnosticsTest.cpp` | create | `[diag]` tests for the log + assert routing |
| `D:\dev\starworks\Manifold2D` (standalone) | modify | commit asserts; logging site-map; input guards |
| `ThirdParty/Manifold2D` | re-vendor | refresh from the standalone repo |
| `ThirdParty/Astra` | re-vendor | refresh from sibling `@2ad70b1` (Mosaic-adopted) |

---

# PHASE A — Arcane diagnostics integration (Aphelyon, in-place)

## Task A1: Log-sink adapter in `Base/Log`

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Base/Log.hpp`, `Arcane/Arcane/src/Arcane/Base/Log.cpp`
- Create: `Arcane/Tests/src/MosaicDiagnosticsTest.cpp`

**Interfaces:**
- Produces: `ARCANE_API Mosaic::LogSink Arcane::Log::MosaicSink() noexcept`; `inline void Arcane::Log::InstallMosaicSink() noexcept`.

- [ ] **Step 1: Write the failing test.** Create `Arcane/Tests/src/MosaicDiagnosticsTest.cpp`:

```cpp
// Epic: Mosaic diagnostics integration. Verifies the Arcane log sink + assert
// handler route Mosaic records into the engine logger. CPU-only ([diag]).

#include <algorithm>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <spdlog/sinks/callback_sink.h>

#include <Arcane/Base/Log.hpp>
#include <Mosaic/Log.hpp>

namespace
{
    // Attach a capturing callback sink to the engine logger; returns it so the
    // caller can remove it afterward (keeps other tests clean).
    std::shared_ptr<spdlog::sinks::callback_sink_mt> AttachCapture(std::string& out)
    {
        auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&out](const spdlog::details::log_msg& m) { out.assign(m.payload.data(), m.payload.size()); });
        Arcane::Log::Engine()->sinks().push_back(cb);
        return cb;
    }
    void DetachCapture(const std::shared_ptr<spdlog::sinks::callback_sink_mt>& cb)
    {
        auto& sinks = Arcane::Log::Engine()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), cb), sinks.end());
    }
}

TEST_CASE("Mosaic log records route to the Arcane engine logger", "[diag]")
{
    std::string captured;
    auto cb = AttachCapture(captured);
    Arcane::Log::InstallMosaicSink();          // install into THIS module
    MOSAIC_LOG_WARN("diag-log-message");
    DetachCapture(cb);

    CHECK(captured.find("diag-log-message") != std::string::npos);
    CHECK(captured.find("Mosaic") != std::string::npos);   // default category rides in the line
}
```

- [ ] **Step 2: Regenerate + build + run to verify it fails.**

Run: `cd Arcane && GenerateProjects.bat` then build. Then `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[diag]"`
Expected: build fails (`InstallMosaicSink` undefined).

- [ ] **Step 3: Add the accessor + inline installer to `Log.hpp`.** After the includes (add `#include <Mosaic/Log.hpp>`), inside `namespace Arcane::Log`, after `Engine()`:

```cpp
    // Mosaic diagnostics: the log SINK that forwards Mosaic/Manifold2D/Astra
    // records into the engine logger (Engine()). Defined in Log.cpp so it lives
    // once, in Arcane.dll, routing every module's records to one spdlog instance.
    ARCANE_API Mosaic::LogSink MosaicSink() noexcept;

    // Install the sink into the CALLING module's Mosaic storage. Inline on
    // purpose: Mosaic's g_logSink is a per-module inline atomic, so each module
    // (Arcane.dll, Loom.exe, the plugin, tests) installs into its own copy.
    inline void InstallMosaicSink() noexcept { Mosaic::SetLogSink(MosaicSink(), nullptr); }
```

- [ ] **Step 4: Add the adapter impl to `Log.cpp`.** Add includes (`#include <Mosaic/Log.hpp>`, `#include <spdlog/spdlog.h>` is already via Log.hpp). In an anonymous namespace:

```cpp
namespace
{
    spdlog::level::level_enum ToSpd(Mosaic::LogLevel l) noexcept
    {
        switch (l)
        {
            case Mosaic::LogLevel::Trace:    return spdlog::level::trace;
            case Mosaic::LogLevel::Debug:    return spdlog::level::debug;
            case Mosaic::LogLevel::Info:     return spdlog::level::info;
            case Mosaic::LogLevel::Warn:     return spdlog::level::warn;
            case Mosaic::LogLevel::Error:    return spdlog::level::err;
            case Mosaic::LogLevel::Critical: return spdlog::level::critical;
            case Mosaic::LogLevel::Off:      return spdlog::level::off;
        }
        return spdlog::level::info;
    }

    // noexcept sink: forward into the engine logger. category + message are fmt
    // ARGUMENTS (literal format) so a stray {} in a message cannot fmt-inject.
    void MosaicLogSinkImpl(const Mosaic::LogRecord& r, void* /*user*/) noexcept
    {
        try
        {
            Arcane::Log::Engine()->log(
                spdlog::source_loc{r.location.file_name(),
                                   static_cast<int>(r.location.line()),
                                   r.location.function_name()},
                ToSpd(r.level), "[{}] {}", r.category, r.message);
        }
        catch (...) {}
    }
}
```

Then, inside `namespace Arcane::Log`, add:

```cpp
    Mosaic::LogSink MosaicSink() noexcept { return &MosaicLogSinkImpl; }
```

- [ ] **Step 5: Build + run to verify pass.**

Run: build, then `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[diag]"`
Expected: the log test passes (1 case).

- [ ] **Step 6: Verify no regression.**

Run (from output dir): `ArcaneTests.exe ~[gpu]`
Expected: 27608/292 + 1 = the diag case; existing cases unchanged. Do NOT commit yet (Task A3 commits Phase A).

---

## Task A2: Assert-handler adapter + `ARC_*` macros in new `Base/Assert`

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Base/Assert.hpp`, `Arcane/Arcane/src/Arcane/Base/Assert.cpp`
- Test: `Arcane/Tests/src/MosaicDiagnosticsTest.cpp` (append)

**Interfaces:**
- Consumes: `Arcane::Log::Engine()` (Task A1 file, existing).
- Produces: `ARCANE_API Mosaic::AssertHandler Arcane::Assert::MosaicHandler() noexcept`; `inline void Arcane::Assert::InstallMosaicHandler() noexcept`; `ARC_ASSERT/ARC_VERIFY/ARC_ENSURE`.

- [ ] **Step 1: Write the failing test** (append to `MosaicDiagnosticsTest.cpp`). Add `#include <Arcane/Base/Assert.hpp>` and `#include <Mosaic/Assert.hpp>` at the top, then:

```cpp
TEST_CASE("Mosaic assert failures route through the Arcane handler (ENSURE, non-fatal)", "[diag]")
{
    std::string captured;
    auto cb = AttachCapture(captured);
    Arcane::Assert::InstallMosaicHandler();     // install into THIS module
    const bool ok = MOSAIC_ENSURE(false, "diag-ensure-msg");   // non-fatal, returns false
    DetachCapture(cb);

    CHECK_FALSE(ok);
    CHECK(captured.find("diag-ensure-msg") != std::string::npos);
    CHECK(captured.find("assertion failed") != std::string::npos);
}
```

- [ ] **Step 2: Build + run to verify it fails.**

Run: `cd Arcane && GenerateProjects.bat` (new files coming) then build; then `ArcaneTests.exe "[diag]"`
Expected: build fails (`Arcane/Base/Assert.hpp` not found / `InstallMosaicHandler` undefined).

- [ ] **Step 3: Create `Base/Assert.hpp`.**

```cpp
#pragma once

// Engine assert seam (Base module): Arcane's ergonomic ARC_ASSERT/VERIFY/ENSURE
// (thin wrappers over Mosaic's guards, the parallel to ARC_* in Log.hpp) plus the
// Mosaic assert HANDLER that routes a failure through the engine logger. The
// handler is defined in Assert.cpp (once, in Arcane.dll); the installer is inline
// so each module installs into its own per-module Mosaic storage.

#include <Arcane/Base/Api.hpp>

#include <Mosaic/Assert.hpp>

namespace Arcane::Assert
{
    ARCANE_API Mosaic::AssertHandler MosaicHandler() noexcept;

    inline void InstallMosaicHandler() noexcept { Mosaic::SetAssertHandler(MosaicHandler(), nullptr); }
}

// Arcane engine asserts -> Mosaic guards (flow through the installed handler).
#define ARC_ASSERT(cond, msg)  MOSAIC_ASSERT(cond, msg)
#define ARC_VERIFY(cond, msg)  MOSAIC_VERIFY(cond, msg)
#define ARC_ENSURE(cond, msg)  MOSAIC_ENSURE(cond, msg)
```

- [ ] **Step 4: Create `Base/Assert.cpp`.**

```cpp
#include <Arcane/Base/Assert.hpp>

#include <Arcane/Base/Log.hpp>   // Arcane::Log::Engine()

#include <spdlog/spdlog.h>

namespace
{
    // noexcept: log the failure Critical through the engine logger with the
    // stringized condition + file:line, then ask Mosaic to Break (FailFatal still
    // breaks-if-debugger then aborts). No "Mosaic" logger -- the engine logger owns it.
    Mosaic::AssertAction MosaicAssertHandlerImpl(const Mosaic::AssertContext& c, void* /*user*/) noexcept
    {
        try
        {
            Arcane::Log::Engine()->log(
                spdlog::source_loc{c.location.file_name(),
                                   static_cast<int>(c.location.line()),
                                   c.location.function_name()},
                spdlog::level::critical, "assertion failed: {}{}",
                c.expression ? c.expression : "<expr>",
                c.message ? std::string(" - ") + c.message : std::string());
        }
        catch (...) {}
        return Mosaic::AssertAction::Break;
    }
}

namespace Arcane::Assert
{
    Mosaic::AssertHandler MosaicHandler() noexcept { return &MosaicAssertHandlerImpl; }
}
```

- [ ] **Step 5: Build + run to verify pass.**

Run: build, then `ArcaneTests.exe "[diag]"`
Expected: 2 `[diag]` cases pass.

- [ ] **Step 6: Verify no regression.**

Run (from output dir): `ArcaneTests.exe ~[gpu]`
Expected: 27608/292 + 2 diag cases. Do NOT commit yet.

---

## Task A3: Wire install sites + land Phase A

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.cpp`, `Arcane/Loom/src/main.cpp`, `Arcane/Sandbox/src/Sandbox.cpp`, `Arcane/PlaygroundGame/src/PlaygroundGame.cpp`

**Interfaces:** consumes `Arcane::Log::InstallMosaicSink()` + `Arcane::Assert::InstallMosaicHandler()` (Tasks A1/A2).

- [ ] **Step 1: Install in the `Runtime` ctor (Arcane.dll).** In `Runtime.cpp`, add includes `#include <Arcane/Base/Log.hpp>` + `#include <Arcane/Base/Assert.hpp>` (if not present), and at the TOP of the `Runtime::Runtime(...)` constructor body:

```cpp
    // Mosaic diagnostics: install the log sink + assert handler into THIS module
    // (Arcane.dll) so Astra/Manifold2D/Mosaic code running here routes to the
    // engine logger. Each module installs its own (per-module Mosaic storage).
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
```

- [ ] **Step 2: Install in Loom `main` (Loom.exe).** In `Arcane/Loom/src/main.cpp`, immediately after the existing `Arcane::Log::Init();` (line ~12), add:

```cpp
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
```

Add `#include <Arcane/Base/Assert.hpp>` if not already included (main.cpp already includes `Base/Log.hpp` for `Log::Init`).

- [ ] **Step 3: Install in both plugins' `GamePlugin_Init`.** In `Arcane/Sandbox/src/Sandbox.cpp` and `Arcane/PlaygroundGame/src/PlaygroundGame.cpp`, inside `GamePlugin_Init` (near the top, after `Astra::SetTypeContext(ctx->typeContext);`), add:

```cpp
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
```

Add `#include <Arcane/Base/Log.hpp>` + `#include <Arcane/Base/Assert.hpp>` to each if not present.

- [ ] **Step 4: Build all + gate.**

Run: `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`
Then (from output dir): `ArcaneTests.exe ~[gpu]` and `ArcaneTests.exe "[diag]"`
Expected: full gate green (27608/292 + 2 diag), Sandbox.dll + PlaygroundGame.dll relink.

- [ ] **Step 5: Commit Phase A** (Aphelyon; no AI trailers).

```
feat(arcane): route Mosaic/Manifold2D/Astra diagnostics through the engine logger

Arcane installs one Mosaic log sink + assert handler (Base/Log + new Base/Assert)
that forward records into the single Arcane::Log::Engine() spdlog logger, with the
source category carried as text (no Mosaic logger). Per-module inline installers
(Runtime ctor, Loom main, both plugins' Init) cover Mosaic's per-module sink
storage. Adds ergonomic ARC_ASSERT/VERIFY/ENSURE. No Mosaic edit, no plugin-ABI
change. Gate: ~[gpu] unchanged + 2 [diag] cases.
```

---

# PHASE B — Manifold2D diagnostics content (standalone repo → re-vendor)

All edits in `D:\dev\starworks\Manifold2D`. Build there: `scripts\generate_vs2022.bat` then MSBuild `Manifold2D.sln` Debug, run `bin\Debug-windows-x86_64\Manifold2DTests\Manifold2DTests.exe`.

## Task B1: Commit the assert migration (standalone)

- [ ] **Step 1: Verify the 7 dirty assert files build + test green** in the standalone repo (they already are per prior work): the 7 files are `include/Manifold2D/Physics/Contact.hpp`, `include/Manifold2D/Physics/Solver/ContactConstraintSimd.hpp`, `src/Physics/Broadphase/DynamicTree.cpp`, `src/Physics/Broadphase/TileGrid.cpp`, `src/Physics/ConstraintGraph.cpp`, `src/Physics/PhysicsWorld.cpp`, `src/Physics/Solver/SoftStep.cpp`. Build + run `Manifold2DTests.exe` — expect green.

- [ ] **Step 2: Commit** (standalone, no AI trailers):

```
refactor: migrate raw assert() to MOSAIC_ASSERT

14 asserts across 7 files now use MOSAIC_ASSERT (Mosaic's guard seam) instead of
<cassert>; #include <cassert> -> #include <Mosaic/Assert.hpp>. Guards only -- no
arithmetic change; suite value-exact.
```

## Task B2: Logging site-map + input-validation guards (standalone)

**Guidance (apply the pattern; find exact sites in the standalone repo):**
Per `.cpp`, add `#define MOSAIC_LOG_CATEGORY "Manifold2D.<Subsystem>"` before `#include <Mosaic/Log.hpp>` — categories: `Broadphase` (DynamicTree, SpatialGrid/Hash, TileGrid), `Solver` (SoftStep, ContactConstraint), `World` (PhysicsWorld), `Graph` (ConstraintGraph), `Contact`.

- [ ] **Step 1: WARN sites (behavior-neutral).** For each `IsValid`-guarded body mutator in `PhysicsWorld.cpp` that silently returns on a stale/invalid handle (`SetPosition`, `SetVelocity`, `ApplyForce`, `ApplyImpulse`, `SetAngle`, `SetAwake`, … — locate the ~12 `if (!IsValid(h)) return;` guards), emit before the return:
```cpp
MOSAIC_LOG_WARN("operation on a stale/invalid BodyHandle ignored");
```
Add the same at the SaneBox non-finite-AABB drop sites (`SpatialHash.cpp` ~line 112, `SpatialGrid`): `MOSAIC_LOG_WARN("non-finite AABB dropped from broadphase");`

- [ ] **Step 2: DEBUG / TRACE / INFO sites (behavior-neutral).**
  - DEBUG at capacity growth (`Grow`/`EnsureCapacity`): `MOSAIC_LOG_DEBUG("storage grew");`
  - TRACE at body/fixture/joint Add/Remove: `MOSAIC_LOG_TRACE("body added");` etc.
  - INFO — exactly ONE, in the `PhysicsWorld` constructor: `MOSAIC_LOG_INFO("PhysicsWorld created");` (a one-line world-config/SIMD banner).
  - Do NOT log designed clamps (velocity cap `SoftStep.cpp:366-381`, friction/motor `Joints.cpp:312/395`).

- [ ] **Step 3: Input-validation guards (BEHAVIOR CHANGE — write tests first).** In the standalone Catch2 suite, add RED tests: `AddBody` with a non-finite position is rejected (returns the invalid handle / no body created); `SetPosition`/`SetVelocity` with NaN/inf is a no-op; `AddFixture` with a degenerate shape (zero/NaN extent) is rejected. Then implement the guards at those entry points — reject the bad input AND `MOSAIC_LOG_WARN("rejected non-finite <thing>")`. Run the suite green.

- [ ] **Step 4: Build + full standalone gate.** `Manifold2DTests.exe` — all green (prior count + the new input-guard cases). Logging sites are determinism-neutral; the guards add the only new cases.

- [ ] **Step 5: Commit** (standalone, no AI trailers):

```
feat: logging site-map + input-validation guards

Per-subsystem MOSAIC_LOG_CATEGORY; WARN on stale-handle no-op mutators + SaneBox
non-finite drops; DEBUG on capacity growth; TRACE on lifecycle; one INFO world
banner. New input-validation guards reject non-finite AddBody/SetPosition/
SetVelocity + degenerate AddFixture (WARN). Designed clamps stay silent.
```

## Task B3: Re-vendor Manifold2D into Aphelyon + gate

- [ ] **Step 1: Re-vendor.** From the standalone repo run `scripts\vendor.ps1` targeting Aphelyon's `ThirdParty/Manifold2D` (it excludes `ThirdParty` so no nested Mosaic). Confirm `git -C D:\dev\starworks\Gacha status --porcelain ThirdParty/Manifold2D` shows the asserts (`grep -c MOSAIC_ASSERT ThirdParty/Manifold2D/src/Physics/PhysicsWorld.cpp` > 0) + the new log/guard code.

- [ ] **Step 2: Build Aphelyon + gate.** `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`, then (output dir) `ArcaneTests.exe ~[gpu]`. Expected green. With the Phase-A sink installed, spawning a body / forcing a stale-handle op in the Sandbox now logs through the engine logger (desk-observable; the link-smoke test still passes).

- [ ] **Step 3: Commit Phase B re-vendor** (Aphelyon, no AI trailers):

```
chore(deps): re-vendor Manifold2D (asserts + logging site-map + input guards)

Refreshes ThirdParty/Manifold2D from the standalone repo: MOSAIC_ASSERT migration,
the per-subsystem logging site-map, and non-finite/degenerate input-validation
guards. With the Phase-A sink installed these now surface in the engine logger.
Gate: ~[gpu] green.
```

---

# PHASE C — Astra re-vendor (sibling → Aphelyon, gated)

The sibling Astra (`D:\dev\starworks\Astra` @2ad70b1) already has the Mosaic Log/Assert adoption committed (`Core/Log.hpp`+`Core/Assert.hpp` re-export Mosaic). Aphelyon's vendored Astra is behind (missing those + `System/SystemContext.hpp`; content deltas in shared files). Re-vendoring catches it up.

## Task C1: Prepare + assess (CHECKPOINT)

- [ ] **Step 1: Sibling must be clean-for-vendor.** The sibling has 3 uncommitted WIP files (`SystemContext.hpp`, `SystemExecutor.hpp`, `SystemContextTest.cpp`) unrelated to Mosaic — these must NOT be vendored. Either the user commits/finishes them, or `git -C D:\dev\starworks\Astra stash` the WIP so the vendor copies the clean `@2ad70b1` state. Confirm `git -C D:\dev\starworks\Astra status --porcelain` is clean before vendoring.

- [ ] **Step 2: Run AstraTest in the sibling** to confirm `@2ad70b1` is green (`AstraTest` per Astra's build).

- [ ] **Step 3: Assess the diff.** Do a dry re-vendor into a scratch copy (or vendor then `git -C D:\dev\starworks\Gacha diff --stat ThirdParty/Astra`) and REVIEW: the expected adds are `Core/Log.hpp`, `Core/Assert.hpp`, `System/SystemContext.hpp`; everything else should be content updates. **CHECKPOINT: surface the diff stat to the user before committing** — a re-vendor pulls ALL Astra changes since the last vendor, so confirm nothing unexpected (API/behavior change Arcane relies on) rides along.

## Task C2: Re-vendor + wire + gate

- [ ] **Step 1: Re-vendor** Astra's `include/` into `ThirdParty/Astra` (Astra is header-only) from the clean sibling `@2ad70b1`.

- [ ] **Step 2: Ensure Mosaic is reachable for Astra TUs.** Astra's `Core/Log.hpp`/`Assert.hpp` `#include <Mosaic/...>`. Confirm every Aphelyon project that compiles Astra-including TUs has `IncludeDir.Mosaic` (already wired for the Arcane projects). If a project fails to find `<Mosaic/...>`, add the include dir to that project in `Arcane/premake5.lua` + `GenerateProjects.bat`.

- [ ] **Step 3: Build Debug + Release + gate.** `msbuild Arcane.slnx /p:Configuration=Debug /m` then Release; run `~[gpu]` from each output dir. Expected: 27608/292-equivalent green on both (test count may shift if Astra behavior changed — investigate any delta, don't paper over it). Astra's `ASTRA_LOG_*`/asserts now route through the Mosaic seam → the engine logger.

- [ ] **Step 4: Commit Phase C** (Aphelyon, no AI trailers):

```
chore(deps): re-vendor Astra with Mosaic Log/Assert adoption

Refreshes ThirdParty/Astra from sibling @2ad70b1: Core/Log.hpp + Core/Assert.hpp
now re-export Mosaic's seam (+ SystemContext and content updates since the last
vendor). Astra diagnostics + asserts now route through the Arcane sink installed
in Phase A. Gate: Debug + Release ~[gpu] green.
```

---

## Self-Review

**Spec coverage:**
- A: log sink (Base/Log) + assert handler + ARC_* (Base/Assert) + install sites + tests → Tasks A1/A2/A3. ✔
- Single-engine-logger routing, category-as-text, no Mosaic logger, noexcept adapters → A1/A2 code. ✔
- B1 assert commit, B2 site-map + input guards, B3 re-vendor → Phase B. ✔
- C sibling-clean + assess + re-vendor + Mosaic include + gate → Phase C. ✔
- No Mosaic edit / no ABI change → Global Constraints + A install via accessors. ✔

**Placeholder scan:** Phase A carries full code; Phase B logging sites are described by category+severity+function+representative code (a pattern pass over sites the executor locates in the standalone repo — appropriate, not a placeholder); no TBD/TODO. ✔

**Type consistency:** `Arcane::Log::MosaicSink()`/`InstallMosaicSink()`, `Arcane::Assert::MosaicHandler()`/`InstallMosaicHandler()`, `ARC_ASSERT/VERIFY/ENSURE`, `ToSpd`, `[diag]` tag used consistently A1↔A2↔A3. ✔
