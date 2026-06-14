# Arcane M5 — Plugin Host + Game DLL + Hot Reload — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the engine's plugin host — an `Arcane::Runtime` facade, a C plugin ABI,
a `PluginHost` (versioned copy-and-load + debounced watch + ABI check + last-good rollback),
a `PlaygroundGame.dll` that is the M4 scene behind that ABI, and a thin `Loom.exe` host — so
the engine supports an edit→rebuild→hot-reload loop with full state survival.

**Architecture:** Astra gains one additive `Registry::Load(..., const Config&)` overload (3.3)
so a restored registry keeps its work scheduler. `Arcane::Runtime` (Arcane.dll) owns the
engine-side `TypeContext` (installed in-module), a persistent `ComponentRegistry`, a swappable
`Registry`, the M4 `SystemSchedulers`/`RunLoop`/`JobSystem`, and snapshot/restore helpers.
`PluginHost` loads versioned copies of the game DLL, checks ABI, and rolls back on failure.
`PlaygroundGame.dll` implements the seven `extern "C"` entry points; its `SaveState`/`LoadState`
wrap one whole-registry `Registry::Save/Load` snapshot. `Loom.exe` boots the engine + render
plumbing and drives `RunLoop` interleaving plugin callbacks with the engine schedulers.

**Tech Stack:** C++23 (Arcane) / C++20 (Astra); Astra ECS (header-only) + `TypeContext`;
enkiTS via `JobSystem`; NVRHI + Batcher2D; glm; Win32 `LoadLibrary` (isolated seam);
GoogleTest (Astra) + Catch2 (Arcane); premake5 (Astra vs2022, Arcane vs2026); msbuild.

---

## Design notes (read before Task 1)

**Spec:** `docs/superpowers/specs/2026-06-14-arcane-m5-plugin-host.md`. This plan implements
it. Decisions already made (do not re-litigate): Astra **3.3** (the `Load` Config overload);
`Arcane::Runtime` is **Phase 1 of M5**; the demo plugin lives in **`PlaygroundGame/`** (the
reserved `Game/` slot is for the future Aphelyon client port).

**The central M5 rule — the M4 single-module-ownership rule is RELAXED via `TypeContext`.**
In M4 a Registry could be touched by exactly one module. In M5 the engine DLL **and** the game
DLL both touch the one engine Registry; they agree on component IDs because every module installs
the **same** `Astra::TypeContext` before its first `TypeID`/`Registry` use:
- **Engine module (Arcane.dll):** `Runtime`'s ctor creates the `TypeContext` and calls
  `Astra::SetTypeContext(&ctx)` *before* building the Registry.
- **Game module (the plugin):** `GamePlugin_Init`'s first line is `Astra::SetTypeContext(ctx->
  typeContext)`, then `ReRegisterComponent<T>` per game component, then register systems.
- **Host exe (Loom):** never instantiates a scene `TypeID` directly — everything scene-typed it
  needs (`SetResource<RenderContext2D>`) is routed through an Arcane.dll method
  (`Runtime::SetRenderContext`), so Loom needs no `SetTypeContext`.

**Build order:** `Arcane` → `PlaygroundGame` + the test plugins → `Loom`. Loom and ArcaneTests
post-build-copy the plugin DLLs next to their exe (the default watched path). Never link a plugin
into a host (loaded at runtime via `GetProcAddress`); never link `Core` into a plugin (Core links
into exactly ONE module per process — the plugin reaches Core types through Arcane.dll exports).

**Branches & repos:**
- Phase 0 is in `D:\dev\starworks\Astra` on branch `dev`; release via `D:\dev\github\Astra`
  (merge `dev` → `main`, cut `3.3`). Both clone `github.com/T3mps/Astra.git`.
- Phases 1-4 are in `D:\dev\starworks\Gacha` on a NEW branch `feature/arcane-m5-plugin-host`
  off `main`.

**Build commands (full MSBuild path; DASH flags under Git Bash):**
- MSBuild: `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`.
- **Astra regen:** from `D:/dev/starworks/Astra`, run `premake5 vs2022` **directly** (the
  `scripts\generate_vs2022.bat` shim ends in `pause`). If `premake5` not on PATH, use
  `D:/dev/starworks/Gacha/ThirdParty/premake5/premake5.exe vs2022`.
- **Astra build/run:** `msbuild "D:/dev/starworks/Astra/Astra.sln" -p:Configuration=Release -m`
  then `"D:/dev/starworks/Astra/bin/Release-windows-x86_64/AstraTest/AstraTest.exe"`. **Validate
  Astra in Release** — the Debug full-suite aborts on pre-existing `RelationshipGraph` asserts.
- **Arcane regen:** from `D:/dev/starworks/Gacha/Arcane`, run
  `../ThirdParty/premake5/premake5.exe vs2026` **directly** (`GenerateProjects.bat` pauses).
- **Arcane build:** `msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo`
  (and `-p:Configuration=Release`).
- **Arcane tests (FROM THE OUTPUT DIR — relative font + plugin paths):**
  `cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "<filter>"`.
  Exclude GPU on headless machines with `"~[gpu]"`.

**Engine rules:** /MD, no `/fp:fast`, UTF-8 without BOM, ASCII in comments, C++23 (Astra stays
C++20). Every engine/game component `ASTRA_REFLECT`-annotated. Commit trailer on every commit:
`Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. NEVER run `db-reset`, `clean --deep`,
`docker compose down -v`, or `tauri dev`. CI self-provisions libpq — no action needed.

---

# PHASE 0 — Astra 3.3 (`Registry::Load` keeps the work scheduler)

All Phase 0 work is in `D:\dev\starworks\Astra`.

## Task A0: Prepare the Astra `dev` branch + baseline

**Files:** none (git + build only).

- [ ] **Step 1: Switch to / create `dev`, confirm clean**

```bash
git -C "D:/dev/starworks/Astra" fetch origin
git -C "D:/dev/starworks/Astra" checkout dev || git -C "D:/dev/starworks/Astra" checkout -b dev
git -C "D:/dev/starworks/Astra" status
```

Expected: on `dev`, working tree clean (or only expected local state).

- [ ] **Step 2: Baseline build + test green BEFORE changes (Release)**

```bash
cd "D:/dev/starworks/Astra" && premake5 vs2022
msbuild "D:/dev/starworks/Astra/Astra.sln" -p:Configuration=Release -m
"D:/dev/starworks/Astra/bin/Release-windows-x86_64/AstraTest/AstraTest.exe" --gtest_brief=1
```

Expected: build succeeds; all tests pass. Record the passing count (baseline for Task A2).

## Task A1: `Registry::Load(..., const Config&)` overload (TDD)

**Files:**
- Test: `D:\dev\starworks\Astra\tests\Registry\RegistryLoadConfigTest.cpp` (create)
- Modify: `D:\dev\starworks\Astra\include\Astra\Registry\Registry.hpp`

- [ ] **Step 1: Write the failing test**

Create `tests/Registry/RegistryLoadConfigTest.cpp`:

```cpp
// Astra 3.3: Registry::Load must thread a Config (hence workScheduler) into the
// restored registry, so a hot-reload restore keeps parallel iteration. The two-arg
// Load stays sequential (default Config) -- proving the existing path is unchanged.

#include <gtest/gtest.h>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Support/TestWorkerPool.hpp>

#include <atomic>
#include <memory>
#include <unordered_set>

namespace
{
    struct Tally { int value = 0; };
}

TEST(RegistryLoadConfig, RestoredRegistryKeepsWorkScheduler)
{
    auto pool = std::make_shared<Astra::Testing::TestWorkerPool>(4);

    Astra::Registry::Config cfg;
    cfg.workScheduler = pool;
    Astra::Registry reg(cfg);
    reg.GetComponentRegistry()->RegisterComponent<Tally>();

    constexpr int kN = 4096;
    for (int i = 0; i < kN; ++i)
        reg.CreateEntityWith(Tally{i});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());

    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<Tally>();

    // NEW overload: pass the Config so the restored registry runs parallel.
    auto restored = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg, cfg);
    ASSERT_TRUE(restored.IsOk());
    Astra::Registry& r = **restored.GetValue();

    std::atomic<int> visited{0};
    r.CreateView<Tally>().ParallelForEach([&](Astra::Entity, Tally&) {
        visited.fetch_add(1, std::memory_order_relaxed);
    });
    EXPECT_EQ(visited.load(), kN);          // every entity visited exactly once
}

TEST(RegistryLoadConfig, TwoArgLoadStaysSequential)
{
    auto pool = std::make_shared<Astra::Testing::TestWorkerPool>(4);
    Astra::Registry::Config cfg; cfg.workScheduler = pool;
    Astra::Registry reg(cfg);
    reg.GetComponentRegistry()->RegisterComponent<Tally>();
    reg.CreateEntityWith(Tally{1});

    auto saved = reg.Save();
    ASSERT_TRUE(saved.IsOk());
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    creg->RegisterComponent<Tally>();

    // Existing two-arg Load: default Config -> null scheduler -> still works, sequential.
    auto restored = Astra::Registry::Load(std::span<const std::byte>(*saved.GetValue()), creg);
    ASSERT_TRUE(restored.IsOk());
    std::atomic<int> visited{0};
    (**restored.GetValue()).CreateView<Tally>().ParallelForEach(
        [&](Astra::Entity, Tally&) { visited.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(visited.load(), 1);
}
```

(If the test premake does not auto-glob `tests/**`, register the new `.cpp`.)

- [ ] **Step 2: Regenerate + build — expect FAIL**

```bash
cd "D:/dev/starworks/Astra" && premake5 vs2022
msbuild "D:/dev/starworks/Astra/Astra.sln" -p:Configuration=Release -m
```

Expected: compile error — no `Load(span, creg, Config)` overload.

- [ ] **Step 3: Add the overloads + thread `Config` through `LoadInternal`**

In `include/Astra/Registry/Registry.hpp`, the two existing `Load` overloads (the
`path` and `span` forms) currently end with `return LoadInternal(reader, std::move(componentRegistry));`.
Change those two calls to pass a default Config:

```cpp
            return LoadInternal(reader, std::move(componentRegistry), Config{});
```

Add the two new overloads immediately after the existing `span` `Load` (before `private:`):

```cpp
        // 3.3: thread a Config (hence workScheduler) into the restored registry, so a
        // hot-reload restore keeps parallel iteration. Additive; the two-arg forms above
        // delegate with a default Config and are byte-for-byte unchanged.
        static Result<std::unique_ptr<Registry>, SerializationError> Load(
            const std::filesystem::path& path,
            std::shared_ptr<ComponentRegistry> componentRegistry,
            const Config& config)
        {
            BinaryReader reader(path);
            if (reader.HasError())
                return Result<std::unique_ptr<Registry>, SerializationError>::Err(SerializationError::IOError);
            return LoadInternal(reader, std::move(componentRegistry), config);
        }

        static Result<std::unique_ptr<Registry>, SerializationError> Load(
            std::span<const std::byte> data,
            std::shared_ptr<ComponentRegistry> componentRegistry,
            const Config& config)
        {
            BinaryReader reader(data);
            return LoadInternal(reader, std::move(componentRegistry), config);
        }
```

Change `LoadInternal`'s signature and the one construction line:

```cpp
        static Result<std::unique_ptr<Registry>, SerializationError> LoadInternal(
            BinaryReader& reader,
            std::shared_ptr<ComponentRegistry> componentRegistry,
            const Config& config)
        {
            // ... header read + EntityManager::Deserialize unchanged ...

            // Construct with the caller's Config so workScheduler (and pool/chunk configs)
            // survive Load. (Was: make_unique<Registry>(componentRegistry).)
            auto registry = std::make_unique<Registry>(componentRegistry, config);

            // ... the rest (entity-manager move, archetype deserialize, relationship
            //     graph deserialize, checksum verify, return) is UNCHANGED ...
        }
```

- [ ] **Step 4: Build + run the new tests — expect PASS**

```bash
msbuild "D:/dev/starworks/Astra/Astra.sln" -p:Configuration=Release -m
"D:/dev/starworks/Astra/bin/Release-windows-x86_64/AstraTest/AstraTest.exe" --gtest_filter=RegistryLoadConfig.*
```

Expected: 2 tests pass.

- [ ] **Step 5: Commit**

```bash
git -C "D:/dev/starworks/Astra" add include/Astra/Registry/Registry.hpp tests/Registry/RegistryLoadConfigTest.cpp
git -C "D:/dev/starworks/Astra" commit -m "feat(registry): Load overload that threads Config into the restored registry

Registry::Load(.., const Config&) preserves workScheduler (and pool/chunk configs)
across deserialize, so a hot-reload restore keeps parallel iteration instead of
silently degrading to sequential. Two-arg Load unchanged (delegates default Config).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

## Task A2: Full regression (Release) — no behavior change elsewhere

**Files:** none (verification only).

- [ ] **Step 1: Full AstraTest run (Release)**

```bash
"D:/dev/starworks/Astra/bin/Release-windows-x86_64/AstraTest/AstraTest.exe" --gtest_brief=1
```

Expected: all pass; total == Task A0 baseline + 2. Confirm `Serialization/*` + `Registry*`
suites green (binary path unchanged). No commit.

## Task A3: Release — bump version, merge to main, cut `3.3`

**Files:** Modify `D:\dev\starworks\Astra\include\Astra\Core\Version.hpp`

- [ ] **Step 1: Bump MINOR on `dev`**

Edit `include/Astra/Core/Version.hpp`: `#define ASTRA_VERSION_MINOR 2` → `3` (leave `MAJOR 3`,
`PATCH 0`).

- [ ] **Step 2: Commit + push `dev`**

```bash
git -C "D:/dev/starworks/Astra" add include/Astra/Core/Version.hpp
git -C "D:/dev/starworks/Astra" commit -m "chore: bump Astra to 3.3.0

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git -C "D:/dev/starworks/Astra" push origin dev
```

- [ ] **Step 3: In the github clone, merge dev → main and cut `3.3`**

```bash
git -C "D:/dev/github/Astra" fetch origin
git -C "D:/dev/github/Astra" checkout main
git -C "D:/dev/github/Astra" merge --no-ff origin/dev -m "merge: Astra 3.3 (Registry::Load Config overload)"
git -C "D:/dev/github/Astra" push origin main
git -C "D:/dev/github/Astra" checkout -b 3.3
git -C "D:/dev/github/Astra" push -u origin 3.3
git -C "D:/dev/github/Astra" checkout main
```

Expected: `main` advanced; branch `3.3` pushed. (If `origin/dev` is missing in this clone,
`git -C "D:/dev/github/Astra" fetch origin dev` first.)

## Task A4: Re-vendor Astra 3.3 into the Gacha repo

**Files:** overwrite `D:\dev\starworks\Gacha\ThirdParty\Astra\include\Astra\**` (plain copy)

- [ ] **Step 1: Create the Gacha feature branch FIRST (so the re-vendor lands on it)**

```bash
git -C "D:/dev/starworks/Gacha" checkout main
git -C "D:/dev/starworks/Gacha" pull
git -C "D:/dev/starworks/Gacha" checkout -b feature/arcane-m5-plugin-host
```

- [ ] **Step 2: Copy the updated headers over the vendored tree**

```bash
cp -r "D:/dev/starworks/Astra/include/Astra/." "D:/dev/starworks/Gacha/ThirdParty/Astra/include/Astra/"
```

- [ ] **Step 3: Verify the vendored copy is 3.3 with the overload**

```bash
grep -n "ASTRA_VERSION_MINOR" "D:/dev/starworks/Gacha/ThirdParty/Astra/include/Astra/Core/Version.hpp"
grep -n "const Config& config" "D:/dev/starworks/Gacha/ThirdParty/Astra/include/Astra/Registry/Registry.hpp"
```

Expected: `ASTRA_VERSION_MINOR 3`; the `Load(.., const Config&)` overloads present.

- [ ] **Step 4: Build the existing Arcane workspace against 3.3 (regression gate)**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "~[gpu]"
```

Expected: builds; existing headless tests still pass (Astra 3.3 is a drop-in).

- [ ] **Step 5: Commit the re-vendor**

```bash
git -C "D:/dev/starworks/Gacha" add ThirdParty/Astra/include/Astra
git -C "D:/dev/starworks/Gacha" commit -m "chore(thirdparty): re-vendor Astra 3.3 (Registry::Load Config overload)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

# PHASE 1 — `Arcane::Runtime` facade + plugin ABI header

All remaining phases are in `D:\dev\starworks\Gacha`. The `Arcane` project globs
`src/**` so new files under `Arcane/Arcane/src/Arcane/{Base,Plugin}` need no premake edit.

## Task B0: Shared test `TypeContext` (cross-module identity for the test exe)

`TypeID<T>::Value()` caches per-module on first touch. The plugin tests read game/scene
components in the ArcaneTests module, so the test module must share ONE `TypeContext` with
the engine + plugin, installed **before any test runs** (so the first component-TypeID touch
resolves under it). M4 tests stay correct (each is internally self-consistent).

**Files:**
- Create: `Arcane\Tests\src\Helpers\TestTypeContext.hpp`
- Modify: `Arcane\Tests\src\test_main.cpp`

- [ ] **Step 1: Create `Tests/src/Helpers/TestTypeContext.hpp`**

```cpp
#pragma once

// One process-wide TypeContext for the whole ArcaneTests run. Installed in the test
// module in main() (before Catch2 runs) and injected into every Runtime a test builds,
// so the test exe, Arcane.dll, and the loaded plugin all share one component-ID space.

#include <Astra/Core/TypeContext.hpp>

namespace Arcane::Test
{
    inline Astra::TypeContext& SharedTypeContext()
    {
        static Astra::TypeContext s_ctx;
        return s_ctx;
    }
}
```

- [ ] **Step 2: Install it in `test_main.cpp` before Catch2 runs**

```cpp
// ArcaneTests runner entry point (Server CommonTests convention).
// Add new test files under Arcane/Tests/src; premake picks them up via the glob.

#include <catch2/catch_session.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Astra/Core/TypeContext.hpp>

int main(int argc, char* argv[]) {
    // Install the shared context in the TEST module BEFORE any test computes a
    // component TypeID, so engine/plugin/test agree (TypeID caches per-module).
    Astra::SetTypeContext(&Arcane::Test::SharedTypeContext());
    return Catch::Session().run(argc, argv);
}
```

- [ ] **Step 3: Build — expect PASS** (no behavior change yet; existing tests still green)

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "~[gpu]"
```

Expected: existing headless tests stay green under the shared context. Commit with B2.

## Task B1: Plugin ABI header (the only C surface)

**Files:** Create `Arcane\Arcane\src\Arcane\Plugin\PluginABI.hpp`

- [ ] **Step 1: Create `PluginABI.hpp`**

```cpp
#pragma once

// The ONLY unmangled C surface in the engine. The game DLL exports the seven
// GamePlugin_* functions; the host resolves them by name into a PluginVTable.
// EngineContext is the C++ facade handed to the plugin (Arcane::Runtime).

#include <Arcane/Base/Api.hpp>

#include <cstdint>

namespace Astra { class TypeContext; class IWorkScheduler; class BinaryWriter; class BinaryReader; }

namespace Arcane
{
    class Runtime;  // defined in Arcane.dll; the plugin holds it opaquely via EngineContext

    // Bump on ANY change to EngineContext layout or the entry-point set/signatures.
    inline constexpr uint32_t kGamePluginABIVersion = 1;

    struct EngineContext
    {
        uint32_t               abiVersion;     // == kGamePluginABIVersion at the host
        Astra::TypeContext*    typeContext;    // plugin calls Astra::SetTypeContext(this) FIRST
        Astra::IWorkScheduler* workScheduler;  // the one engine enkiTS adapter (shared instance)
        Arcane::Runtime*       engine;         // registry, schedulers, snapshot/restore, render ctx
    };

    namespace PluginEntry
    {
        inline constexpr const char* kABIVersion  = "GamePlugin_ABIVersion";
        inline constexpr const char* kInit        = "GamePlugin_Init";
        inline constexpr const char* kShutdown    = "GamePlugin_Shutdown";
        inline constexpr const char* kFixedUpdate = "GamePlugin_FixedUpdate";
        inline constexpr const char* kUpdate      = "GamePlugin_Update";
        inline constexpr const char* kSaveState   = "GamePlugin_SaveState";
        inline constexpr const char* kLoadState   = "GamePlugin_LoadState";
    }

    // Host-side resolved function-pointer table for one loaded plugin image.
    struct PluginVTable
    {
        uint32_t (*ABIVersion)()                         = nullptr;
        bool     (*Init)(EngineContext*)                 = nullptr;
        void     (*Shutdown)()                           = nullptr;
        void     (*FixedUpdate)(double dt)               = nullptr;
        void     (*Update)(double dt, double alpha)      = nullptr;
        void     (*SaveState)(Astra::BinaryWriter&)      = nullptr;
        bool     (*LoadState)(Astra::BinaryReader&)      = nullptr;
    };
}
```

- [ ] **Step 2: Build (header compiles via Runtime in B2) — no standalone test**

(No commit yet; committed with B2.)

## Task B2: `Arcane::Runtime` facade (TDD)

**Files:**
- Test: `Arcane\Tests\src\RuntimeTest.cpp` (create)
- Create: `Arcane\Arcane\src\Arcane\Base\Runtime.hpp`
- Create: `Arcane\Arcane\src\Arcane\Base\Runtime.cpp`

- [ ] **Step 1: Write the failing test**

Create `Tests/src/RuntimeTest.cpp`:

```cpp
// Runtime is the engine facade EngineContext.engine points at. It owns the substrate
// that outlives reloads: TypeContext (installed in Arcane.dll), persistent
// ComponentRegistry, a swappable Registry, schedulers, RunLoop, and the JobSystem.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/Reflection.hpp>

#include <atomic>

namespace { struct Counter { int value = 0; }; }
namespace { ASTRA_REFLECT_TYPE(Counter) ASTRA_REFLECT_FIELD(Counter, value) ASTRA_END_REFLECT_TYPE() }

// A void(Registry&) LAMBDA does NOT satisfy Astra's LambdaLike (that concept is for
// per-entity lambdas); systems must be NAMED types registered via AddSystem<T>().
namespace { struct NoOpSystem { void operator()(Astra::Registry&) const {} }; }

TEST_CASE("Runtime boots a usable substrate", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.TypeContext() != nullptr);
    REQUIRE(rt.WorkScheduler() != nullptr);
    REQUIRE(rt.WorkScheduler()->WorkerCount() >= 1);

    rt.Components()->RegisterComponent<Counter>();
    auto& reg = rt.Registry();
    for (int i = 0; i < 8; ++i) reg.CreateEntityWith(Counter{i});

    int seen = 0;
    reg.CreateView<Counter>().ForEach([&](Astra::Entity, Counter&) { ++seen; });
    CHECK(seen == 8);
}

TEST_CASE("Runtime snapshot/restore preserves state AND the scheduler", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Counter>();
    auto& reg = rt.Registry();
    constexpr int kN = 2048;
    for (int i = 0; i < kN; ++i) reg.CreateEntityWith(Counter{7});

    std::vector<std::byte> snap = rt.SnapshotRegistry();
    REQUIRE(!snap.empty());

    // Mutate the live registry, then restore the snapshot.
    reg.CreateView<Counter>().ForEach([](Astra::Entity, Counter& c) { c.value = 0; });
    REQUIRE(rt.RestoreRegistry(snap));

    std::atomic<int> visited{0};
    std::atomic<int> sum{0};
    rt.Registry().CreateView<Counter>().ParallelForEach([&](Astra::Entity, Counter& c) {
        visited.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(c.value, std::memory_order_relaxed);
    });
    CHECK(visited.load() == kN);     // state survived
    CHECK(sum.load() == 7 * kN);     // values survived (== 7, not the mutated 0)
}

TEST_CASE("Runtime ClearSystems empties all phase schedulers", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Schedulers().fixedUpdate.AddSystem<NoOpSystem>();   // each scheduler has its own
    rt.Schedulers().update.AddSystem<NoOpSystem>();        // type index, so reusing the
    rt.Schedulers().render.AddSystem<NoOpSystem>();        // same type across them is fine
    rt.ClearSystems();
    CHECK(rt.Schedulers().fixedUpdate.Empty());
    CHECK(rt.Schedulers().update.Empty());
    CHECK(rt.Schedulers().render.Empty());
}
```

- [ ] **Step 2: Build — expect FAIL** (`Arcane/Base/Runtime.hpp` missing)

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
```

- [ ] **Step 3: Create `Arcane/Base/Runtime.hpp`**

```cpp
#pragma once

// Runtime: the engine facade handed to plugins via EngineContext. Owns the substrate
// that MUST outlive plugin reloads -- the shared TypeContext (installed in THIS module),
// the persistent ComponentRegistry, the (swappable) Registry, the per-phase schedulers,
// the RunLoop, and the JobSystem. ARCANE_API: the plugin and the host both call it.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace Astra { class Registry; class ComponentRegistry; class TypeContext; class IWorkScheduler; }

namespace Arcane
{
    class Batcher2D;

    class ARCANE_API Runtime
    {
    public:
        // externalContext == null: Runtime creates+owns a TypeContext (production: Loom).
        // externalContext != null: install+use the caller's context so multiple modules
        // (a test exe + Arcane.dll + the plugin) share one component-ID space.
        explicit Runtime(Astra::TypeContext* externalContext = nullptr);
        ~Runtime();

        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        // --- substrate the plugin registers into / the host drives ---
        Astra::Registry&        Registry()      noexcept;
        SystemSchedulers&       Schedulers()    noexcept;
        RunLoop&                Loop()          noexcept;
        Astra::TypeContext*     TypeContext()   noexcept;
        Astra::IWorkScheduler*  WorkScheduler() noexcept;
        std::shared_ptr<Astra::ComponentRegistry> Components() noexcept;

        // --- render bridge: the host sets the live batcher each frame, IN this module ---
        void SetRenderContext(Batcher2D* batcher, glm::vec2 cameraOffset);

        // --- hot-reload support (plugin Save/LoadState + the host call these) ---
        std::vector<std::byte> SnapshotRegistry() const;          // Registry::Save() -> CRC'd bytes
        bool RestoreRegistry(std::span<const std::byte> bytes);   // Load(Config{sched}); swap; rebind RunLoop
        void ResetRegistry();                                     // swap in a fresh empty registry (fresh-boot reload)
        void ClearSystems();                                      // Clear() all three phase schedulers

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
```

- [ ] **Step 4: Create `Arcane/Base/Runtime.cpp`**

```cpp
#include <Arcane/Base/Runtime.hpp>

#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // RenderContext2D (instantiated IN this module)

#include <Astra/Registry/Registry.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/WorkScheduler.hpp>

namespace Arcane
{
    struct Runtime::Impl
    {
        JobSystem                                   jobs;
        std::shared_ptr<Astra::IWorkScheduler>      sched;
        std::unique_ptr<Astra::TypeContext>         ownedContext;   // null when an external one is injected
        Astra::TypeContext*                         context = nullptr;
        std::shared_ptr<Astra::ComponentRegistry>   components;
        std::unique_ptr<Astra::Registry>            registry;
        std::unique_ptr<SystemSchedulers>           schedulers;
        std::unique_ptr<RunLoop>                    loop;
        RunLoop::Config                             loopCfg;

        explicit Impl(Astra::TypeContext* external) : jobs(), sched(jobs.WorkScheduler())
        {
            if (external) { context = external; }
            else { ownedContext = std::make_unique<Astra::TypeContext>(); context = ownedContext.get(); }

            // Install the shared context in THIS module BEFORE any TypeID/Registry use.
            Astra::SetTypeContext(context);
            components = std::make_shared<Astra::ComponentRegistry>();

            Astra::Registry::Config cfg;
            cfg.workScheduler = sched;
            registry   = std::make_unique<Astra::Registry>(components, cfg);
            schedulers = std::make_unique<SystemSchedulers>(sched);
            loop       = std::make_unique<RunLoop>(*registry, *schedulers, loopCfg);
        }
    };

    Runtime::Runtime(Astra::TypeContext* externalContext)
        : m_impl(std::make_unique<Impl>(externalContext)) {}
    Runtime::~Runtime() = default;   // do not reset the module slot: a later Runtime re-installs

    Astra::Registry&  Runtime::Registry()   noexcept { return *m_impl->registry; }
    SystemSchedulers& Runtime::Schedulers() noexcept { return *m_impl->schedulers; }
    RunLoop&          Runtime::Loop()       noexcept { return *m_impl->loop; }
    Astra::TypeContext*    Runtime::TypeContext()   noexcept { return m_impl->context; }
    Astra::IWorkScheduler* Runtime::WorkScheduler() noexcept { return m_impl->sched.get(); }
    std::shared_ptr<Astra::ComponentRegistry> Runtime::Components() noexcept { return m_impl->components; }

    void Runtime::SetRenderContext(Batcher2D* batcher, glm::vec2 cameraOffset)
    {
        // SetResource<RenderContext2D> runs IN Arcane.dll so TypeID<RenderContext2D>
        // resolves here against the shared context; the host never touches a scene TypeID.
        m_impl->registry->SetResource<RenderContext2D>(RenderContext2D{batcher, cameraOffset});
    }

    std::vector<std::byte> Runtime::SnapshotRegistry() const
    {
        auto r = m_impl->registry->Save();
        return r.IsOk() ? std::move(*r.GetValue()) : std::vector<std::byte>{};
    }

    bool Runtime::RestoreRegistry(std::span<const std::byte> bytes)
    {
        Astra::Registry::Config cfg;
        cfg.workScheduler = m_impl->sched;
        auto r = Astra::Registry::Load(bytes, m_impl->components, cfg);   // 3.3 Config overload
        if (r.IsErr())
            return false;
        m_impl->registry = std::move(*r.GetValue());
        m_impl->loop = std::make_unique<RunLoop>(*m_impl->registry, *m_impl->schedulers, m_impl->loopCfg);
        return true;
    }

    void Runtime::ResetRegistry()
    {
        // Fresh-boot reload: replace the registry with an empty one (same shared
        // ComponentRegistry + scheduler) so the plugin's Init rebuilds its scene.
        Astra::Registry::Config cfg;
        cfg.workScheduler = m_impl->sched;
        m_impl->registry = std::make_unique<Astra::Registry>(m_impl->components, cfg);
        m_impl->loop = std::make_unique<RunLoop>(*m_impl->registry, *m_impl->schedulers, m_impl->loopCfg);
    }

    void Runtime::ClearSystems()
    {
        m_impl->schedulers->fixedUpdate.Clear();
        m_impl->schedulers->update.Clear();
        m_impl->schedulers->render.Clear();
    }
}
```

- [ ] **Step 5: Build + run — expect PASS** (regenerate first so the new TU is in the vcxproj)

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[runtime]"
```

Expected: 3 test cases pass.

- [ ] **Step 6: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp Arcane/Arcane/src/Arcane/Base/Runtime.hpp Arcane/Arcane/src/Arcane/Base/Runtime.cpp Arcane/Tests/src/RuntimeTest.cpp
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/base): Runtime facade + plugin ABI header

Runtime owns the reload-surviving substrate (engine-side TypeContext install,
persistent ComponentRegistry, swappable Registry, schedulers, RunLoop, JobSystem)
and the snapshot/restore + ClearSystems helpers hot reload needs. PluginABI.hpp
declares EngineContext + the seven extern C entry points + the host vtable.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

## Task B3: `RunLoop` plugin-interleave overload (TDD)

**Files:**
- Modify: `Arcane\Tests\src\RunLoopTest.cpp` (append a case)
- Modify: `Arcane\Arcane\src\Arcane\Sim\RunLoop.hpp`

- [ ] **Step 1: Append the failing test to `RunLoopTest.cpp`** (add `#include <vector>` and
  `#include <functional>` at the top if not already present)

```cpp
namespace
{
    // Named engine system (a void(Registry&) lambda fails LambdaLike). Aggregate-inited
    // by AddSystem<EngineStep>(engineFixed, order) -> new EngineStep(engineFixed, order)
    // (parenthesized aggregate init, C++20+). Records 'E' each engine fixed step.
    struct EngineStep
    {
        int&              n;
        std::vector<char>& order;
        void operator()(Astra::Registry&) const { ++n; order.push_back('E'); }
    };
}

TEST_CASE("RunLoop interleaves plugin callbacks with engine fixedUpdate", "[sim][runloop]")
{
    Astra::Registry reg;
    int engineFixed = 0;
    std::vector<char> order;
    Arcane::SystemSchedulers sch(nullptr);     // declared AFTER the locals it references
    sch.fixedUpdate.AddSystem<EngineStep>(engineFixed, order);

    Arcane::RunLoop loop(reg, sch);
    int pluginFixed = 0, pluginUpdate = 0;
    for (int i = 0; i < 60; ++i)
        loop.Advance(1.0 / 60.0,
            [&](double){ ++pluginFixed; order.push_back('P'); },   // plugin fixed step (a std::function arg, fine)
            [&](double, double){ ++pluginUpdate; });               // plugin per-frame

    CHECK(pluginFixed >= 58);
    CHECK(pluginFixed == engineFixed);     // one plugin tick per engine fixed step
    CHECK(pluginUpdate == 60);             // once per frame
    // every fixed step is plugin('P') then engine('E'), so the sequence alternates P,E:
    REQUIRE(order.size() >= 2);
    CHECK(order[order.size() - 2] == 'P');
    CHECK(order[order.size() - 1] == 'E');
}
```

- [ ] **Step 2: Build — expect FAIL** (no 3-arg `Advance`)

- [ ] **Step 3: Add the overload to `RunLoop.hpp`**

Add `#include <functional>` near the top, then add inside `class RunLoop` (after the existing
`Advance`):

```cpp
        // Host-driven variant: pluginFixed(fixedDt) runs each fixed step BEFORE the engine
        // fixedUpdate scheduler (gameplay moves transforms; propagation reads them after).
        // pluginUpdate(dt, alpha) runs once after the Update scheduler. Same spiral-of-death
        // clamp as Advance(realDt).
        double Advance(double realDt,
                       const std::function<void(double)>& pluginFixed,
                       const std::function<void(double, double)>& pluginUpdate)
        {
            const double fixedDt = 1.0 / m_cfg.fixedHz;
            m_accumulator += realDt;

            int steps = 0;
            while (m_accumulator >= fixedDt && steps < m_cfg.maxStepsPerFrame)
            {
                if (pluginFixed) pluginFixed(fixedDt);
                m_schedulers->fixedUpdate.Execute(*m_registry, &m_schedulers->executor);
                m_accumulator -= fixedDt;
                ++steps;
            }
            if (steps == m_cfg.maxStepsPerFrame && m_accumulator >= fixedDt)
                m_accumulator = 0.0;

            m_schedulers->update.Execute(*m_registry, &m_schedulers->executor);
            m_alpha = m_accumulator / fixedDt;
            if (pluginUpdate) pluginUpdate(realDt, m_alpha);
            return m_alpha;
        }
```

- [ ] **Step 4: Build + run — expect PASS**

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[runloop]"
```

Expected: both runloop cases pass.

- [ ] **Step 5: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Arcane/src/Arcane/Sim/RunLoop.hpp Arcane/Tests/src/RunLoopTest.cpp
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/sim): RunLoop overload interleaving plugin callbacks

Advance(dt, pluginFixed, pluginUpdate) runs the plugin's fixed step before the
engine fixedUpdate scheduler (gameplay then propagation) and pluginUpdate once
per frame -- the host-loop shape the plugin ABI needs. Existing Advance kept.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

# PHASE 2 — `PluginHost`: dynamic-library seam, load/reload/rollback, watcher

## Task C1: The OS dynamic-library seam

**Files:**
- Create: `Arcane\Arcane\src\Arcane\Plugin\DynamicLibrary.hpp`
- Create: `Arcane\Arcane\src\Arcane\Plugin\DynamicLibrary.cpp`

- [ ] **Step 1: Create `DynamicLibrary.hpp`**

```cpp
#pragma once

// The ONLY place the engine calls the OS dynamic-loader. Windows now; the Linux
// dlopen port (later milestone) is a single-file swap of the .cpp.

#include <filesystem>

namespace Arcane::Detail
{
    using LibHandle = void*;

    LibHandle DLOpen(const std::filesystem::path& path);  // null on failure
    void*     DLSym(LibHandle handle, const char* name);  // null if the symbol is missing
    void      DLClose(LibHandle handle);
}
```

- [ ] **Step 2: Create `DynamicLibrary.cpp`**

```cpp
#include <Arcane/Plugin/DynamicLibrary.hpp>

#if defined(_WIN32)
    #include <windows.h>   // Arcane.dll already defines WIN32_LEAN_AND_MEAN + NOMINMAX

namespace Arcane::Detail
{
    LibHandle DLOpen(const std::filesystem::path& path)
    {
        return reinterpret_cast<LibHandle>(::LoadLibraryW(path.c_str()));
    }
    void* DLSym(LibHandle handle, const char* name)
    {
        return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
    }
    void DLClose(LibHandle handle)
    {
        if (handle) ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
    }
}
#else
    #include <dlfcn.h>

namespace Arcane::Detail
{
    LibHandle DLOpen(const std::filesystem::path& path)
    {
        return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    }
    void* DLSym(LibHandle handle, const char* name) { return ::dlsym(handle, name); }
    void  DLClose(LibHandle handle) { if (handle) ::dlclose(handle); }
}
#endif
```

- [ ] **Step 3: Build — expect PASS** (regenerate to pick up the new TU)

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
```

Expected: builds (seam is inert until PluginHost uses it). Commit with C3.

## Task C2: Test-plugin source + premake projects (build-only)

**Files:**
- Create: `Arcane\Tests\plugins\HotReloadShared.hpp`
- Create: `Arcane\Tests\plugins\HotReloadPlugin.cpp`
- Create: `Arcane\Tests\plugins\PluginExport.hpp`
- Modify: `Arcane\premake5.lua` (add 3 plugin projects + ArcaneTests copy/dependson)

- [ ] **Step 1: Create `Tests/plugins/PluginExport.hpp`**

```cpp
#pragma once
#if defined(_WIN32)
  #if defined(GAME_BUILD_DLL)
    #define GAME_API __declspec(dllexport)
  #else
    #define GAME_API __declspec(dllimport)
  #endif
#else
  #define GAME_API __attribute__((visibility("default")))
#endif
```

- [ ] **Step 1b: Create `Tests/plugins/HotReloadShared.hpp`** (ONE `Pulse` type shared by the plugin TU and the test TU)

```cpp
#pragma once

// Shared so the plugin module and the test module use the SAME C++ type for Pulse
// (one type name -> one TypeContext id). A per-TU anonymous-namespace struct would be
// a distinct type per module; do not rely on anonymous-namespace name-hash equality.

#include <Astra/Reflection/Reflection.hpp>

namespace Arcane::HotReloadTest
{
    struct Pulse { int ticks = 0; };

    ASTRA_REFLECT_TYPE(Pulse)
        ASTRA_REFLECT_FIELD(Pulse, ticks)
    ASTRA_END_REFLECT_TYPE()
}
```

- [ ] **Step 2: Create `Tests/plugins/HotReloadPlugin.cpp`** (one source, two behaviors + a bad-ABI flavor via defines)

```cpp
// Minimal hot-reload test plugin. Built into three DLLs from this one source:
//   HotReloadPluginV1  -> HOTRELOAD_STEP=1,  ABI = kGamePluginABIVersion
//   HotReloadPluginV2  -> HOTRELOAD_STEP=10, ABI = kGamePluginABIVersion
//   HotReloadPluginBad -> ABI = kGamePluginABIVersion + 999 (forces rollback)
// One reflected component (Pulse) + the seven entry points. SaveState/LoadState wrap
// the whole-registry snapshot via the engine Runtime, exactly like PlaygroundGame.

#include "PluginExport.hpp"
#include "HotReloadShared.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>

#include <cstddef>
#include <vector>

#ifndef HOTRELOAD_STEP
  #define HOTRELOAD_STEP 1
#endif
#ifndef HOTRELOAD_ABI_OFFSET
  #define HOTRELOAD_ABI_OFFSET 0
#endif

using Arcane::HotReloadTest::Pulse;

namespace
{
    Arcane::EngineContext* g_ctx = nullptr;
    Astra::Entity          g_pulse{};

    void CacheHandle(Astra::Registry& reg)
    {
        g_pulse = {};
        reg.CreateView<Pulse>().ForEach([&](Astra::Entity e, Pulse&) { g_pulse = e; });
    }
}

extern "C"
{
    GAME_API uint32_t GamePlugin_ABIVersion()
    {
        return Arcane::kGamePluginABIVersion + (HOTRELOAD_ABI_OFFSET);
    }

    GAME_API bool GamePlugin_Init(Arcane::EngineContext* ctx)
    {
        Astra::SetTypeContext(ctx->typeContext);          // 1. shared context in THIS module
        g_ctx = ctx;
        ctx->engine->Components()->ReRegisterComponent<Pulse>();   // 2. descriptors -> this module

        Astra::Registry& reg = ctx->engine->Registry();
        bool exists = false;
        reg.CreateView<Pulse>().ForEach([&](Astra::Entity, Pulse&) { exists = true; });
        if (!exists)
            reg.CreateEntityWith(Pulse{0});               // fresh boot only
        CacheHandle(reg);
        return true;
    }

    GAME_API void GamePlugin_Shutdown() { g_pulse = {}; }

    GAME_API void GamePlugin_FixedUpdate(double)
    {
        if (auto* p = g_ctx->engine->Registry().GetComponent<Pulse>(g_pulse))
            p->ticks += (HOTRELOAD_STEP);                 // V1: +1, V2: +10 (observably different code)
    }

    GAME_API void GamePlugin_Update(double, double) {}

    GAME_API void GamePlugin_SaveState(Astra::BinaryWriter& w)
    {
        const std::vector<std::byte> blob = g_ctx->engine->SnapshotRegistry();
        w(static_cast<uint64_t>(blob.size()));
        w.WriteBytes(blob.data(), blob.size());
    }

    GAME_API bool GamePlugin_LoadState(Astra::BinaryReader& r)
    {
        uint64_t n = 0; r(n);
        std::vector<std::byte> blob(static_cast<size_t>(n));
        r.ReadBytes(blob.data(), static_cast<size_t>(n));
        if (r.HasError()) return false;
        if (!g_ctx->engine->RestoreRegistry(blob)) return false;
        CacheHandle(g_ctx->engine->Registry());
        return true;
    }
}
```

- [ ] **Step 3: Add the three plugin projects to `Arcane/premake5.lua`**

After the `ArcaneTests` project block, add (a helper that stamps three SharedLib projects):

```lua
-- ============================================================================
-- Hot-reload TEST plugins: one source, three DLLs (V1 step=1, V2 step=10,
-- Bad ABI). SharedLib, /MD, links Arcane (NOT Core -- one Core per process).
-- Loaded at runtime by PluginHost in ArcaneTests; never linked by the test exe.
-- ============================================================================
local function test_plugin(name, defs)
    project(name)
        location "Tests/plugins"
        kind "SharedLib"
        language "C++"
        cppdialect "C++23"
        staticruntime "off"
        targetname(name)
        targetdir ("bin/" .. outputdir .. "/" .. name)
        objdir ("bin-int/" .. outputdir .. "/" .. name)
        files { "%{prj.location}/HotReloadPlugin.cpp", "%{prj.location}/PluginExport.hpp", "%{prj.location}/HotReloadShared.hpp" }
        includedirs {
            "%{wks.location}/Arcane/src",
            "%{IncludeDir.glm}",
            "%{IncludeDir.nvrhi}",
            "%{IncludeDir.Astra}",
            "%{IncludeDir.enkiTS}",
        }
        links { "Arcane" }
        defines (defs)
        filter "system:windows"
            systemversion "latest"
            buildoptions { "/Zc:__cplusplus", "/bigobj" }
        filter "configurations:Debug"  defines { "ARCANE_DEBUG" }   runtime "Debug"   symbols "on"
        filter "configurations:Release" defines { "ARCANE_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
        filter "configurations:Dist"    defines { "ARCANE_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
        filter {}
end

test_plugin("HotReloadPluginV1",  { "GAME_BUILD_DLL", "HOTRELOAD_STEP=1",  "_CRT_SECURE_NO_WARNINGS" })
test_plugin("HotReloadPluginV2",  { "GAME_BUILD_DLL", "HOTRELOAD_STEP=10", "_CRT_SECURE_NO_WARNINGS" })
test_plugin("HotReloadPluginBad", { "GAME_BUILD_DLL", "HOTRELOAD_ABI_OFFSET=999", "_CRT_SECURE_NO_WARNINGS" })
```

In the `ArcaneTests` project, add a `dependson` and copy the plugin DLLs next to the exe.
Add **after** the existing `links { ... }` line:

```lua
    dependson { "HotReloadPluginV1", "HotReloadPluginV2", "HotReloadPluginBad" }
```

and add to the existing `postbuildcommands { ... }` block (alongside the Arcane.dll copy):

```lua
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginV1/HotReloadPluginV1.dll" "%{cfg.buildtarget.directory}/HotReloadPluginV1.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginV2/HotReloadPluginV2.dll" "%{cfg.buildtarget.directory}/HotReloadPluginV2.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginBad/HotReloadPluginBad.dll" "%{cfg.buildtarget.directory}/HotReloadPluginBad.dll"',
```

- [ ] **Step 4: Regenerate + build the plugins — expect PASS**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
ls "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/HotReloadPluginV1.dll"
```

Expected: three plugin DLLs build; the V1 copy lands next to ArcaneTests.exe. Commit with C3.

## Task C3: `PluginHost` (load / reload / rollback / watch) + tests (TDD)

**Files:**
- Test: `Arcane\Tests\src\PluginHostTest.cpp` (create)
- Create: `Arcane\Arcane\src\Arcane\Plugin\PluginHost.hpp`
- Create: `Arcane\Arcane\src\Arcane\Plugin\PluginHost.cpp`

- [ ] **Step 1: Write the failing tests**

Create `Tests/src/PluginHostTest.cpp`:

```cpp
// PluginHost: versioned copy-and-load, ABI check, last-good rollback, hot swap.
// Headless -- no window/device. Uses the HotReloadPlugin V1/V2/Bad DLLs copied
// next to ArcaneTests.exe (relative paths; run the exe FROM its output dir).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/Device.hpp>   // RenderErrorCount()

#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"   // the SAME Pulse type the plugin uses

#include <Astra/Registry/Registry.hpp>

#include <filesystem>

using Arcane::HotReloadTest::Pulse;

namespace
{
    int ReadPulse(Arcane::Runtime& rt)
    {
        int v = 0;
        rt.Registry().CreateView<Pulse>().ForEach([&](Astra::Entity, Pulse& p) { v = p.ticks; });
        return v;
    }
    void StepK(Arcane::Runtime& rt, const Arcane::PluginVTable& vt, int k)
    {
        for (int i = 0; i < k; ++i)
            rt.Loop().Advance(1.0 / 60.0, [&](double dt){ vt.FixedUpdate(dt); }, [&](double,double){});
    }
}

TEST_CASE("PluginHost loads a plugin and runs it across the ABI", "[hotreload]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();   // engine sees the type so views resolve

    Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
    REQUIRE(host.Load());
    REQUIRE(host.IsLoaded());

    StepK(rt, *host.Vtable(), 5);
    CHECK(ReadPulse(rt) == 5);                     // V1 increments by 1
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();
}

TEST_CASE("Hot swap V1->V2 preserves state AND runs the new code", "[hotreload]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();

    Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
    REQUIRE(host.Load());
    StepK(rt, *host.Vtable(), 5);
    REQUIRE(ReadPulse(rt) == 5);

    // Swap the binary under the watched path, then force a state-preserving reload.
    std::filesystem::copy_file("HotReloadPluginV2.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);
    REQUIRE(host.ForceReload());                   // SaveState -> unload -> load V2 -> Init -> LoadState

    CHECK(ReadPulse(rt) == 5);                     // STATE SURVIVED the swap
    StepK(rt, *host.Vtable(), 1);
    CHECK(ReadPulse(rt) == 15);                    // NEW CODE LIVE: V2 step is +10
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();

    // restore the fixture for re-runs
    std::filesystem::copy_file("HotReloadPluginV2.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);
}

TEST_CASE("ABI mismatch rolls back to last-good; session survives", "[hotreload]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();

    Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
    REQUIRE(host.Load());
    StepK(rt, *host.Vtable(), 3);
    REQUIRE(ReadPulse(rt) == 3);

    std::filesystem::copy_file("HotReloadPluginBad.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);
    CHECK_FALSE(host.ForceReload());               // ABI mismatch -> reload fails
    CHECK(host.IsLoaded());                        // still on last-good
    StepK(rt, *host.Vtable(), 1);
    CHECK(ReadPulse(rt) == 4);                      // last-good V1 still running, state intact
    host.Unload();

    std::filesystem::copy_file("HotReloadPluginV2.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);
}
```

- [ ] **Step 2: Build — expect FAIL** (`PluginHost.hpp` missing)

- [ ] **Step 3: Create `Arcane/Plugin/PluginHost.hpp`**

```cpp
#pragma once

#include <Arcane/Base/Api.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace Arcane
{
    class Runtime;

    // Watches a game DLL, loads versioned copies (PDB-lock dodge), checks the ABI,
    // and rolls back to the last-good image on any failure (never a lost session).
    class ARCANE_API PluginHost
    {
    public:
        PluginHost(Runtime& runtime, std::filesystem::path sourceDllPath);
        ~PluginHost();

        PluginHost(const PluginHost&) = delete;
        PluginHost& operator=(const PluginHost&) = delete;

        bool Load();                       // initial load: copy+load+ABI+Init (no state restore)
        void Unload();                     // Shutdown + ClearSystems + FreeLibrary + delete copies
        bool Reload(bool restoreState);    // full sequence; rollback on any failure
        void Poll();                       // debounced mtime watch -> Reload(true) on a stable change

        bool ForceReload() { return Reload(true);  }
        bool ReloadFresh() { return Reload(false); }

        bool                IsLoaded()   const noexcept;
        const PluginVTable* Vtable()     const noexcept;
        uint32_t            Generation() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
```

- [ ] **Step 4: Create `Arcane/Plugin/PluginHost.cpp`**

```cpp
#include <Arcane/Plugin/PluginHost.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/DynamicLibrary.hpp>

#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace Arcane
{
    namespace
    {
        struct Image
        {
            Detail::LibHandle     handle = nullptr;
            PluginVTable          vt{};
            std::filesystem::path dll;
            std::filesystem::path pdb;
            uint32_t              gen = 0;
        };

        bool Resolve(Detail::LibHandle h, PluginVTable& vt)
        {
            using Sym = void*;
            const Sym a = Detail::DLSym(h, PluginEntry::kABIVersion);
            const Sym i = Detail::DLSym(h, PluginEntry::kInit);
            const Sym s = Detail::DLSym(h, PluginEntry::kShutdown);
            const Sym f = Detail::DLSym(h, PluginEntry::kFixedUpdate);
            const Sym u = Detail::DLSym(h, PluginEntry::kUpdate);
            const Sym sv = Detail::DLSym(h, PluginEntry::kSaveState);
            const Sym ld = Detail::DLSym(h, PluginEntry::kLoadState);
            if (!a || !i || !s || !f || !u || !sv || !ld) return false;
            vt.ABIVersion  = reinterpret_cast<uint32_t(*)()>(a);
            vt.Init        = reinterpret_cast<bool(*)(EngineContext*)>(i);
            vt.Shutdown    = reinterpret_cast<void(*)()>(s);
            vt.FixedUpdate = reinterpret_cast<void(*)(double)>(f);
            vt.Update      = reinterpret_cast<void(*)(double,double)>(u);
            vt.SaveState   = reinterpret_cast<void(*)(Astra::BinaryWriter&)>(sv);
            vt.LoadState   = reinterpret_cast<bool(*)(Astra::BinaryReader&)>(ld);
            return true;
        }
    }

    struct PluginHost::Impl
    {
        Runtime&              runtime;
        std::filesystem::path source;
        std::filesystem::path tempDir;
        EngineContext         ctx{};
        Image                 current;
        uint32_t              gen = 0;

        // watcher / debounce
        std::filesystem::file_time_type lastWrite{};
        bool                            pending = false;
        std::filesystem::file_time_type pendingWrite{};
        std::chrono::steady_clock::time_point pendingSince{};

        Impl(Runtime& rt, std::filesystem::path src)
            : runtime(rt), source(std::move(src))
        {
            tempDir = std::filesystem::temp_directory_path() / "arcane_plugins";
            ctx.abiVersion    = kGamePluginABIVersion;
            ctx.typeContext   = runtime.TypeContext();
            ctx.workScheduler = runtime.WorkScheduler();
            ctx.engine        = &runtime;
        }

        bool CopyVersioned(uint32_t g, Image& out)
        {
            std::error_code ec;
            std::filesystem::create_directories(tempDir, ec);
            const std::string stem = source.stem().string();
            out.dll = tempDir / (stem + "_" + std::to_string(g) + ".dll");
            if (!std::filesystem::copy_file(source, out.dll,
                    std::filesystem::copy_options::overwrite_existing, ec) || ec)
                return false;   // source busy (msbuild writing) or missing -> not ready

            std::filesystem::path srcPdb = source; srcPdb.replace_extension(".pdb");
            if (std::filesystem::exists(srcPdb))
            {
                out.pdb = tempDir / (stem + "_" + std::to_string(g) + ".pdb");
                std::filesystem::copy_file(srcPdb, out.pdb,
                    std::filesystem::copy_options::overwrite_existing, ec);  // best-effort
            }
            out.gen = g;
            lastWrite = std::filesystem::last_write_time(source, ec);
            return true;
        }

        void DeleteFiles(const Image& img)
        {
            std::error_code ec;
            if (!img.dll.empty()) std::filesystem::remove(img.dll, ec);
            if (!img.pdb.empty()) std::filesystem::remove(img.pdb, ec);
        }

        void TeardownLive()   // Shutdown + ClearSystems + FreeLibrary; KEEP files for rollback
        {
            if (current.handle)
            {
                if (current.vt.Shutdown) current.vt.Shutdown();
                runtime.ClearSystems();
                Detail::DLClose(current.handle);
                current.handle = nullptr;
            }
        }
    };

    PluginHost::PluginHost(Runtime& runtime, std::filesystem::path src)
        : m_impl(std::make_unique<Impl>(runtime, std::move(src))) {}
    PluginHost::~PluginHost() { Unload(); }

    bool PluginHost::Load()
    {
        const uint32_t g = m_impl->gen + 1;
        Image img;
        if (!m_impl->CopyVersioned(g, img)) { ARC_ERROR("plugin: cannot copy source DLL"); return false; }
        img.handle = Detail::DLOpen(img.dll);
        const bool ok = img.handle
            && Resolve(img.handle, img.vt)
            && img.vt.ABIVersion() == kGamePluginABIVersion
            && img.vt.Init(&m_impl->ctx);
        if (!ok)
        {
            if (img.handle) Detail::DLClose(img.handle);
            m_impl->DeleteFiles(img);
            ARC_ERROR("plugin: initial load failed");
            return false;
        }
        m_impl->current = img;
        m_impl->gen = g;
        ARC_INFO("plugin loaded (gen {})", g);
        return true;
    }

    void PluginHost::Unload()
    {
        m_impl->TeardownLive();
        m_impl->DeleteFiles(m_impl->current);
        m_impl->current = Image{};
    }

    bool PluginHost::Reload(bool restoreState)
    {
        // 1. copy-first: also the lock/readiness probe. If the source is still being
        //    written by msbuild, leave the live plugin untouched and retry next Poll.
        const uint32_t nextGen = m_impl->gen + 1;
        Image next;
        if (!m_impl->CopyVersioned(nextGen, next))
            return false;

        // 2. snapshot the live plugin's state (whole-registry, via the engine).
        std::vector<std::byte> snapshot;
        if (restoreState && m_impl->current.handle && m_impl->current.vt.SaveState)
        {
            Astra::BinaryWriter w(snapshot);
            m_impl->current.vt.SaveState(w);
        }

        // 3. tear down the live plugin (keep its versioned files for rollback).
        Image previous = m_impl->current;
        m_impl->TeardownLive();

        // 4. load + (fresh-reset) + init + (optionally) restore the new image.
        if (!restoreState)
            m_impl->runtime.ResetRegistry();   // fresh boot: empty registry so Init rebuilds the scene
        next.handle = Detail::DLOpen(next.dll);
        bool ok = next.handle
            && Resolve(next.handle, next.vt)
            && next.vt.ABIVersion() == kGamePluginABIVersion
            && next.vt.Init(&m_impl->ctx);
        if (ok && restoreState)
        {
            Astra::BinaryReader r(snapshot);
            ok = next.vt.LoadState(r);
        }

        if (ok)
        {
            m_impl->current = next;
            m_impl->gen = nextGen;
            m_impl->DeleteFiles(previous);
            ARC_INFO("plugin reloaded (gen {}, snapshot {} bytes)", nextGen, snapshot.size());
            return true;
        }

        // 5. FAILURE -> rollback to the previous last-good image; never a lost session.
        if (next.handle) { if (next.vt.Shutdown) next.vt.Shutdown(); Detail::DLClose(next.handle); }
        m_impl->DeleteFiles(next);
        m_impl->runtime.ClearSystems();
        if (!previous.dll.empty())
        {
            previous.handle = Detail::DLOpen(previous.dll);
            if (previous.handle && Resolve(previous.handle, previous.vt) && previous.vt.Init(&m_impl->ctx))
            {
                if (restoreState && !snapshot.empty())
                {
                    Astra::BinaryReader r(snapshot);
                    previous.vt.LoadState(r);
                }
            }
        }
        m_impl->current = previous;
        ARC_ERROR("plugin reload failed (gen {}); rolled back to last-good", nextGen);
        return false;
    }

    void PluginHost::Poll()
    {
        std::error_code ec;
        const auto wt = std::filesystem::last_write_time(m_impl->source, ec);
        if (ec) return;
        if (wt == m_impl->lastWrite) { m_impl->pending = false; return; }

        const auto now = std::chrono::steady_clock::now();
        if (!m_impl->pending || wt != m_impl->pendingWrite)
        {
            m_impl->pending = true;
            m_impl->pendingWrite = wt;
            m_impl->pendingSince = now;
            return;
        }
        if (now - m_impl->pendingSince >= std::chrono::milliseconds(250))
        {
            m_impl->pending = false;
            Reload(true);   // copy-first inside handles a still-locked file
        }
    }

    bool                PluginHost::IsLoaded()   const noexcept { return m_impl->current.handle != nullptr; }
    const PluginVTable* PluginHost::Vtable()     const noexcept { return m_impl->current.handle ? &m_impl->current.vt : nullptr; }
    uint32_t            PluginHost::Generation() const noexcept { return m_impl->gen; }
}
```

- [ ] **Step 5: Regenerate + build + run — expect PASS** (run FROM the output dir)

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[hotreload]"
```

Expected: all `[hotreload]` cases pass — load+run, V1→V2 swap (state survived + new code live),
ABI-mismatch rollback. If the swap test flakes on a still-mapped versioned file, it is the
best-effort delete (harmless); the assertions are unaffected.

- [ ] **Step 6: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Arcane/src/Arcane/Plugin Arcane/Tests/plugins Arcane/Tests/src/PluginHostTest.cpp Arcane/premake5.lua
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/plugin): PluginHost (versioned load, ABI check, hot swap, rollback)

DynamicLibrary OS seam + PluginHost: copy-first versioned load (PDB-lock dodge),
ABI-version gate, whole-registry SaveState/LoadState across FreeLibrary/LoadLibrary,
last-good rollback. Test plugins (V1/V2/Bad) prove state-survives-swap, new-code-live,
and rollback-on-mismatch.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

# PHASE 3 — `PlaygroundGame.dll`: the first live plugin

## Task D1: `PlaygroundGame` project + plugin source

**Files:**
- Create: `Arcane\PlaygroundGame\src\GameApi.hpp`
- Create: `Arcane\PlaygroundGame\src\PlaygroundGame.cpp`
- Modify: `Arcane\premake5.lua` (add the `PlaygroundGame` project + ArcaneTests copy)

- [ ] **Step 1: Create `PlaygroundGame/src/GameApi.hpp`** (same shape as the test export header)

```cpp
#pragma once
#if defined(_WIN32)
  #if defined(GAME_BUILD_DLL)
    #define GAME_API __declspec(dllexport)
  #else
    #define GAME_API __declspec(dllimport)
  #endif
#else
  #define GAME_API __attribute__((visibility("default")))
#endif
```

- [ ] **Step 2: Create `PlaygroundGame/src/PlaygroundGame.cpp`** (M4 scene content behind the ABI)

```cpp
// PlaygroundGame: the M4 parent/child sprite scene behind the plugin ABI. The engine
// scene components/systems (Arcane/Scene/*) are header-only and instantiate in THIS
// module; the engine Registry/schedulers/RunLoop live in Arcane.dll via the Runtime.

#include "GameApi.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <vector>

namespace
{
    Arcane::EngineContext* g_ctx = nullptr;
    Astra::Entity          g_root{};
    Astra::Entity          g_orbiter{};
    Astra::Entity          g_moon{};
    double                 g_time = 0.0;

    void BuildScene(Astra::Registry& reg)
    {
        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::LocalTransform>(root, Arcane::LocalTransform{});
        reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});

        Astra::Entity orbiter = reg.CreateEntity();
        Arcane::LocalTransform ot; ot.position = glm::vec2(640.0f, 360.0f);
        reg.AddComponent<Arcane::LocalTransform>(orbiter, ot);
        reg.AddComponent<Arcane::WorldTransform>(orbiter, Arcane::WorldTransform{});
        Arcane::SpriteRenderer os; os.size = glm::vec2(48.0f); os.tint = glm::vec4(0.9f, 0.7f, 0.2f, 1.0f);
        reg.AddComponent<Arcane::SpriteRenderer>(orbiter, os);
        reg.SetParent(orbiter, root);

        Astra::Entity moon = reg.CreateEntity();
        Arcane::LocalTransform mt; mt.position = glm::vec2(80.0f, 0.0f);
        reg.AddComponent<Arcane::LocalTransform>(moon, mt);
        reg.AddComponent<Arcane::WorldTransform>(moon, Arcane::WorldTransform{});
        Arcane::SpriteRenderer ms; ms.size = glm::vec2(20.0f); ms.tint = glm::vec4(0.4f, 0.8f, 1.0f, 1.0f);
        reg.AddComponent<Arcane::SpriteRenderer>(moon, ms);
        reg.SetParent(moon, orbiter);

        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    }

    void CacheHandles(Astra::Registry& reg)
    {
        g_root = {}; g_orbiter = {}; g_moon = {};
        if (auto* sr = reg.GetResource<Arcane::SceneRoot>())
        {
            g_root = sr->entity;
            // orbiter = the root's child; moon = the orbiter's child (BFS via relations).
            reg.GetRelations(g_root).ForEachDescendant([&](Astra::Entity e, size_t depth) {
                if (depth == 1) g_orbiter = e;
                else if (depth == 2) g_moon = e;
            });
        }
    }
}

extern "C"
{
    GAME_API uint32_t GamePlugin_ABIVersion() { return Arcane::kGamePluginABIVersion; }

    GAME_API bool GamePlugin_Init(Arcane::EngineContext* ctx)
    {
        Astra::SetTypeContext(ctx->typeContext);   // 1. shared context in THIS module
        g_ctx = ctx;

        auto creg = ctx->engine->Components();
        creg->ReRegisterComponent<Arcane::LocalTransform>();   // 2. descriptors -> this module
        creg->ReRegisterComponent<Arcane::WorldTransform>();
        creg->ReRegisterComponent<Arcane::SpriteRenderer>();

        auto& sch = ctx->engine->Schedulers();                 // 3. register systems (functors in this module)
        sch.fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>();
        sch.render.AddSystem<Arcane::RenderSubmissionSystem>();

        Astra::Registry& reg = ctx->engine->Registry();
        if (!reg.GetResource<Arcane::SceneRoot>())             // build only on a fresh boot
            BuildScene(reg);
        CacheHandles(reg);
        return true;
    }

    GAME_API void GamePlugin_Shutdown() { g_root = {}; g_orbiter = {}; g_moon = {}; }

    GAME_API void GamePlugin_FixedUpdate(double dt)
    {
        g_time += dt;
        if (auto* lt = g_ctx->engine->Registry().GetComponent<Arcane::LocalTransform>(g_orbiter))
            lt->rotation = static_cast<float>(g_time);   // orbit; propagation runs after, in the engine phase
    }

    GAME_API void GamePlugin_Update(double, double) {}

    GAME_API void GamePlugin_SaveState(Astra::BinaryWriter& w)
    {
        const std::vector<std::byte> blob = g_ctx->engine->SnapshotRegistry();
        w(static_cast<uint64_t>(blob.size()));
        w.WriteBytes(blob.data(), blob.size());
    }

    GAME_API bool GamePlugin_LoadState(Astra::BinaryReader& r)
    {
        uint64_t n = 0; r(n);
        std::vector<std::byte> blob(static_cast<size_t>(n));
        r.ReadBytes(blob.data(), static_cast<size_t>(n));
        if (r.HasError()) return false;
        if (!g_ctx->engine->RestoreRegistry(blob)) return false;
        CacheHandles(g_ctx->engine->Registry());
        g_time = 0.0;   // sceneTime is transient gameplay state; the orbit resumes from current rotation
        return true;
    }
}
```

- [ ] **Step 3: Add the `PlaygroundGame` project to `Arcane/premake5.lua`** (after Playground)

```lua
-- ============================================================================
-- PlaygroundGame: the M4 scene as a game plugin (the first live ABI consumer).
-- SharedLib, /MD, links Arcane (NOT Core). Loaded by Loom at runtime. The
-- reserved Game/ slot stays empty for the future Aphelyon client port.
-- ============================================================================
project "PlaygroundGame"
    location "PlaygroundGame"
    kind "SharedLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
    files { "%{prj.location}/src/**.cpp", "%{prj.location}/src/**.hpp" }
    includedirs {
        "%{wks.location}/Arcane/src",
        "%{IncludeDir.glm}",
        "%{IncludeDir.nvrhi}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
    }
    links { "Arcane" }
    defines { "GAME_BUILD_DLL", "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING" }
    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
    filter "configurations:Debug"  defines { "ARCANE_DEBUG" }   runtime "Debug"   symbols "on"
    filter "configurations:Release" defines { "ARCANE_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"    defines { "ARCANE_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}
```

In `ArcaneTests`, add `"PlaygroundGame"` to the existing `dependson { ... }` and a copy line to
`postbuildcommands`:

```lua
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/PlaygroundGame/PlaygroundGame.dll" "%{cfg.buildtarget.directory}/PlaygroundGame.dll"',
```

- [ ] **Step 4: Regenerate + build — expect PASS**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
ls "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/PlaygroundGame.dll"
```

Expected: `PlaygroundGame.dll` builds and lands next to ArcaneTests.exe. Commit with D2.

## Task D2: `PlaygroundGame` plugin test (headless, TDD)

**Files:** Test: `Arcane\Tests\src\PlaygroundGamePluginTest.cpp` (create)

- [ ] **Step 1: Write the test**

```cpp
// PlaygroundGame across the ABI: the scene runs (orbit + transform propagation) and
// survives a real FreeLibrary/LoadLibrary reload. Headless -- no GPU.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Astra/Registry/Registry.hpp>

#include <filesystem>

namespace
{
    glm::vec2 MoonWorldPos(Arcane::Runtime& rt)
    {
        glm::vec2 pos(0.0f);
        auto& reg = rt.Registry();
        if (auto* sr = reg.GetResource<Arcane::SceneRoot>())
        {
            reg.GetRelations(sr->entity).ForEachDescendant([&](Astra::Entity e, size_t depth) {
                if (depth == 2)
                    if (auto* wt = reg.GetComponent<Arcane::WorldTransform>(e))
                        pos = glm::vec2(wt->matrix[2]);   // translation column
            });
        }
        return pos;
    }
}

TEST_CASE("PlaygroundGame runs and survives a reload", "[hotreload]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    // Scene component types are registered by the plugin's Init (ReRegisterComponent);
    // the shared TypeContext makes the test module's TypeID<WorldTransform> agree.

    Arcane::PluginHost host(rt, std::filesystem::path("PlaygroundGame.dll"));
    REQUIRE(host.Load());

    const glm::vec2 before = MoonWorldPos(rt);
    for (int i = 0; i < 30; ++i)
        rt.Loop().Advance(1.0 / 60.0,
            [&](double dt){ host.Vtable()->FixedUpdate(dt); }, [&](double,double){});
    const glm::vec2 afterSteps = MoonWorldPos(rt);
    CHECK(glm::distance(before, afterSteps) > 1.0f);    // orbit + propagation moved the moon

    REQUIRE(host.ForceReload());                        // snapshot round-trip across a real swap
    const glm::vec2 afterReload = MoonWorldPos(rt);
    CHECK(glm::distance(afterSteps, afterReload) < 1.0f);  // state survived the reload
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();
}
```

- [ ] **Step 2: Build + run — expect PASS**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[hotreload]"
```

Expected: the new case passes (alongside the Phase-2 hot-reload cases).

- [ ] **Step 3: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/PlaygroundGame Arcane/Tests/src/PlaygroundGamePluginTest.cpp Arcane/premake5.lua
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/playgroundgame): M4 scene as the first hot-reloadable plugin

PlaygroundGame.dll implements the seven entry points; Init does SetTypeContext ->
ReRegisterComponent -> register systems -> build scene; SaveState/LoadState wrap the
whole-registry snapshot. Headless test: scene runs across the ABI and survives reload.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

# PHASE 4 — `Loom.exe`: thin host + live integration

## Task E1: `Loom` project + host loop

**Files:**
- Create: `Arcane\Loom\src\main.cpp`
- Create: `Arcane\Loom\data\input_actions.json`
- Modify: `Arcane\premake5.lua` (add the `Loom` project)

- [ ] **Step 1: Create `Loom/data/input_actions.json`** (Playground actions + two reload chords)

Copy `Arcane/Playground/data/input_actions.json` and add two actions. The file is a JSON
object of actions; add entries mirroring the existing `toggle_stats` button binding:

```json
{
  "contexts": { "demo": {
    "quit":               { "type": "button", "bindings": [ { "key": "escape" } ] },
    "toggle_stats":       { "type": "button", "bindings": [ { "key": "f1" } ] },
    "swap_backend":       { "type": "button", "bindings": [ { "key": "f2" } ] },
    "reload_plugin":      { "type": "button", "bindings": [ { "key": "f5" } ] },
    "reload_plugin_fresh":{ "type": "button", "bindings": [ { "key": "f6" } ] },
    "move":               { "type": "axis2", "bindings": [ { "composite": "wasd" } ] }
  } }
}
```

(Match the exact schema of `Playground/data/input_actions.json` — copy it verbatim and add only
the `reload_plugin` / `reload_plugin_fresh` button actions; keep whatever `move`/`quit` shape the
existing file uses. The two new actions are the only required additions.)

- [ ] **Step 2: Create `Loom/src/main.cpp`** (the M4 host loop, scene content removed; plugin drives gameplay)

```cpp
// Loom -- the thin host. Engine boot + render plumbing (the M4 Playground loop, scene
// content removed) + RunLoop interleaving the plugin's FixedUpdate/Update with the engine
// schedulers + a PluginHost watching the game DLL. The scene now lives in PlaygroundGame.dll.
//   --backend dx12|vulkan   --frames N   --no-vsync   --plugin <path>   (default ./PlaygroundGame.dll)

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/ImGui/ImGuiLayer.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputDevices.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/Swapchain.hpp>
#include <Arcane/Render/TonemapPass.hpp>

#include <nvrhi/nvrhi.h>
#include <glm/glm.hpp>
#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{
    void PrintUsage()
    {
        std::printf(
            "Arcane Loom (M5: plugin host)\n"
            "  --backend dx12|vulkan   graphics backend (default dx12)\n"
            "  --frames N              render N frames then exit\n"
            "  --no-vsync              present without vsync\n"
            "  --plugin <path>         game DLL to host (default ./PlaygroundGame.dll)\n");
    }
}

int main(int argc, char** argv)
{
    Arcane::GraphicsBackend backend = Arcane::GraphicsBackend::D3D12;
    uint64_t maxFrames = 0;
    bool vsync = true;
    std::string pluginPath = "PlaygroundGame.dll";

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
        {
            ++i;
            if      (std::strcmp(argv[i], "vulkan") == 0) backend = Arcane::GraphicsBackend::Vulkan;
            else if (std::strcmp(argv[i], "dx12") == 0)   backend = Arcane::GraphicsBackend::D3D12;
            else { PrintUsage(); return 2; }
        }
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) { maxFrames = std::strtoull(argv[++i], nullptr, 10); }
        else if (std::strcmp(argv[i], "--no-vsync") == 0)               { vsync = false; }
        else if (std::strcmp(argv[i], "--plugin") == 0 && i + 1 < argc) { pluginPath = argv[++i]; }
        else { PrintUsage(); return 2; }
    }

    Arcane::Log::Init();
    ARC_INFO("{} -- Loom host, backend {}", Arcane::BuildInfo(), Arcane::ToString(backend));

    Arcane::Window window;
    Arcane::WindowDesc wd; wd.title = "Arcane Loom"; wd.vulkan = (backend == Arcane::GraphicsBackend::Vulkan);
    if (!window.Create(wd)) return 1;

    Arcane::RenderDeviceDesc dd; dd.backend = backend;
    auto device = Arcane::RenderDevice::Create(dd);                 if (!device) return 1;
    auto swapchain = device->CreateSwapchain(window, vsync);        if (!swapchain) return 1;
    auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "shaders"); if (!shaders) return 1;
    auto canvas  = Arcane::CreateCanvas(device->Nvrhi(), swapchain->Width(), swapchain->Height()); if (!canvas) return 1;
    auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders); if (!batcher) return 1;
    auto tonemap = Arcane::TonemapPass::Create(device->Nvrhi(), *shaders); if (!tonemap) return 1;
    auto imgui   = Arcane::ImGuiLayer::Create(window, *device, *shaders);  if (!imgui) return 1;

    auto inputDevices = Arcane::InputDevices::Create();
    auto input = Arcane::InputActions::Create();
    if (!inputDevices || !input || !input->LoadFile("data/input_actions.json")) return 1;
    input->SetBaseContext("demo");

    // --- engine + plugin ---
    Arcane::Runtime runtime;
    Arcane::PluginHost plugin(runtime, std::filesystem::path(pluginPath));
    if (!plugin.Load()) { ARC_ERROR("Loom: failed to load plugin '{}'", pluginPath); return 1; }

    nvrhi::CommandListHandle commandList = device->Nvrhi()->createCommandList();
    std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> backbufferFramebuffers;
    auto simPrev = std::chrono::steady_clock::now();
    auto lastFrameTime = simPrev;
    auto lastShaderPoll = simPrev;
    uint64_t frameCount = 0;
    bool running = true;

    while (running)
    {
        auto events = window.PumpEvents();
        if (events.quitRequested) break;
        if (events.resized) { backbufferFramebuffers.clear(); swapchain->Resize(events.width, events.height); canvas->Resize(swapchain->Width(), swapchain->Height()); }
        if (window.IsMinimized()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }

        {
            const auto now = std::chrono::steady_clock::now();
            const double frameDt = std::chrono::duration<double>(now - lastFrameTime).count();
            lastFrameTime = now;
            input->Update(frameDt, inputDevices->Sample(imgui->WantCaptureKeyboard(), imgui->WantCaptureMouse()));
            if (input->Pressed("quit")) break;
            if (input->Pressed("reload_plugin"))       plugin.ForceReload();
            if (input->Pressed("reload_plugin_fresh")) plugin.ReloadFresh();
        }

        // Advance the simulation: plugin FixedUpdate (gameplay) interleaved with the
        // engine fixedUpdate scheduler (transform propagation), then plugin Update.
        {
            const auto now = std::chrono::steady_clock::now();
            double simDt = std::chrono::duration<double>(now - simPrev).count();
            simPrev = now;
            if (simDt > 0.25) simDt = 0.25;
            const Arcane::PluginVTable* vt = plugin.Vtable();
            runtime.Loop().Advance(simDt,
                [&](double dt){ if (vt) vt->FixedUpdate(dt); },
                [&](double dt, double a){ if (vt) vt->Update(dt, a); });
        }

        imgui->BeginFrame();
        {
            ImGui::Begin("Loom");
            ImGui::Text("Backend: %s", Arcane::ToString(device->Backend()));
            ImGui::Text("Plugin gen: %u", plugin.Generation());
            const Arcane::Batch2DStats s = batcher->Stats();
            ImGui::Text("Quads: %u  Draws: %u", s.quads, s.drawCalls);
            ImGui::End();
        }

        nvrhi::ITexture* backbuffer = swapchain->BeginFrame();
        if (!backbuffer) continue;

        const auto now0 = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now0 - lastShaderPoll).count() >= 1.0) { shaders->Poll(); lastShaderPoll = now0; }

        commandList->open();
        commandList->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources, nvrhi::Color(0.02f, 0.02f, 0.04f, 1.0f));
        batcher->Begin(commandList, canvas->Framebuffer(), canvas->Width(), canvas->Height());

        runtime.SetRenderContext(batcher.get(), glm::vec2(0.0f, 0.0f));   // sets RenderContext2D IN Arcane.dll
        runtime.Loop().SubmitRender();                                    // game's RenderSubmissionSystem -> batcher

        batcher->End();

        nvrhi::FramebufferHandle& fb = backbufferFramebuffers[backbuffer];
        if (!fb) fb = device->Nvrhi()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(backbuffer));
        tonemap->Run(commandList, canvas->Texture(), fb);
        imgui->Render(commandList, fb);
        commandList->close();
        device->Nvrhi()->executeCommandList(commandList);
        swapchain->Present();

        plugin.Poll();   // debounced watcher: rebuild PlaygroundGame -> auto hot reload

        ++frameCount;
        if (maxFrames != 0 && frameCount >= maxFrames) running = false;
    }

    ARC_INFO("Loom exiting after {} frames", frameCount);
    device->Nvrhi()->waitForIdle();
    plugin.Unload();
    return 0;
}
```

- [ ] **Step 3: Add the `Loom` project to `Arcane/premake5.lua`** (mirrors Playground; copies the plugin)

```lua
-- ============================================================================
-- Loom: the thin host (Loom.exe). Engine boot + RunLoop + PluginHost. Hosts
-- PlaygroundGame.dll (copied beside Loom.exe -- the default watched path).
-- ============================================================================
project "Loom"
    location "Loom"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
    files { "%{prj.location}/src/**.cpp", "%{prj.location}/src/**.hpp" }
    includedirs {
        "%{wks.location}/Arcane/src",
        "%{IncludeDir.Core}",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.nvrhi}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
    }
    links { "Arcane" }
    dependson { "PlaygroundGame" }
    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=__declspec(dllimport)" }
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/Arcane/Arcane.dll" "%{cfg.buildtarget.directory}/Arcane.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/PlaygroundGame/PlaygroundGame.dll" "%{cfg.buildtarget.directory}/PlaygroundGame.dll"',
        '{COPYDIR} "%{wks.location}/shaders/generated" "%{cfg.buildtarget.directory}/shaders"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data"',
        '{COPYFILE} "%{wks.location}/Loom/data/input_actions.json" "%{cfg.buildtarget.directory}/data/input_actions.json"',
    }
    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus" }
    filter "configurations:Debug"  defines { "ARCANE_DEBUG" }   runtime "Debug"   symbols "on"
    filter "configurations:Release" defines { "ARCANE_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"    defines { "ARCANE_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}
```

- [ ] **Step 4: Regenerate + build — expect PASS**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
ls "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/Loom/Loom.exe"
ls "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/Loom/PlaygroundGame.dll"
```

Expected: `Loom.exe` builds; `Arcane.dll` + `PlaygroundGame.dll` + `shaders/` + `data/` copied beside it.

- [ ] **Step 5: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Loom Arcane/premake5.lua
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/loom): thin plugin host (engine boot + RunLoop + PluginHost)

Loom.exe boots the engine + render plumbing and hosts PlaygroundGame.dll, driving
RunLoop with plugin FixedUpdate/Update interleaved with the engine schedulers. F5
force-reloads (with state), F6 reloads fresh; the watcher auto-reloads on rebuild.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

## Task E2: `[gpu]` live slice test + scripted both-backends run

**Files:** Test: `Arcane\Tests\src\LoomSliceTest.cpp` (create)

- [ ] **Step 1: Write the `[gpu]` test** (offscreen render through the plugin's registered system)

```cpp
// [gpu] live integration: drive PlaygroundGame through a real Runtime + Batcher2D into
// an offscreen HDR canvas; assert the plugin's RenderSubmissionSystem submitted the two
// sprites and NVRHI/VK validation stayed silent. Device setup copied from SceneSliceTest
// (no headless-device helper exists; [gpu] tests call RenderDevice::Create directly).
// Asserts via batcher->Stats() only -> no scene-component TypeID is touched in the test
// module (the plugin owns the component identities under the shared context).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <glm/glm.hpp>

#include <filesystem>

namespace
{
    void RunLoomSlice(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc; desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "shaders");
        auto canvas  = Arcane::CreateCanvas(device->Nvrhi(), 1280, 720);
        auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
        REQUIRE(shaders != nullptr); REQUIRE(canvas != nullptr); REQUIRE(batcher != nullptr);

        Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
        Arcane::PluginHost host(rt, std::filesystem::path("PlaygroundGame.dll"));
        REQUIRE(host.Load());   // plugin registers scene components + systems under the shared ctx

        for (int i = 0; i < 10; ++i)
            rt.Loop().Advance(1.0 / 60.0, [&](double dt){ host.Vtable()->FixedUpdate(dt); }, [&](double,double){});

        nvrhi::CommandListHandle cl = device->Nvrhi()->createCommandList();
        cl->open();
        cl->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources, nvrhi::Color(0,0,0,1));
        batcher->Begin(cl, canvas->Framebuffer(), canvas->Width(), canvas->Height());
        rt.SetRenderContext(batcher.get(), glm::vec2(0.0f));
        rt.Loop().SubmitRender();   // plugin's RenderSubmissionSystem -> batcher
        batcher->End();
        cl->close();
        device->Nvrhi()->executeCommandList(cl);
        device->Nvrhi()->waitForIdle();

        CHECK(batcher->Stats().quads >= 2);             // orbiter + moon submitted via the plugin
        CHECK(Arcane::RenderErrorCount() == 0);
        host.Unload();
        device->Nvrhi()->runGarbageCollection();
    }
}

TEST_CASE("d3d12: Loom slice renders the plugin scene, no validation errors", "[gpu][d3d12]")
{
    RunLoomSlice(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: Loom slice renders the plugin scene, no validation errors", "[gpu][vulkan]")
{
    RunLoomSlice(Arcane::GraphicsBackend::Vulkan);
}
```

- [ ] **Step 2: Build + run both suites**

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[hotreload]" && ./ArcaneTests.exe "[gpu]"
```

Expected: hot-reload + gpu cases pass.

- [ ] **Step 3: Scripted both-backends live run (the living integration test)**

```bash
cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/Loom"
./Loom.exe --backend dx12 --frames 120 ; echo "dx12 exit: $?"
./Loom.exe --backend vulkan --frames 120 ; echo "vulkan exit: $?"
```

Expected: both exit 0 (real host + real plugin + real render path renders 120 frames).

- [ ] **Step 4: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Tests/src/LoomSliceTest.cpp
git -C "D:/dev/starworks/Gacha" commit -m "test(arcane/loom): [gpu] live slice through the plugin render path

Drives PlaygroundGame through a real Runtime + Batcher2D offscreen; asserts >=2 quads
submitted by the plugin's RenderSubmissionSystem and RenderErrorCount()==0.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

## Task E3: Full-suite green (Debug + Release, both backends) + finish

**Files:** none (verification + memory).

- [ ] **Step 1: Build + run the FULL suite, both configs**

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug   -p:Platform=x64 -m -v:minimal -nologo
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Release -p:Platform=x64 -m -v:minimal -nologo
cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests"   && ./ArcaneTests.exe
cd "D:/dev/starworks/Gacha/Arcane/bin/Release-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe
```

Expected: all tests pass in both configs (run each FROM its own output dir). Record the
assertion/case counts (for the completion memory).

- [ ] **Step 2: Scripted Loom run, both backends, Release**

```bash
cd "D:/dev/starworks/Gacha/Arcane/bin/Release-windows-x86_64-md/Loom"
./Loom.exe --backend dx12 --frames 180 ; echo "exit $?"
./Loom.exe --backend vulkan --frames 180 ; echo "exit $?"
```

Expected: both exit 0.

- [ ] **Step 3: Manual hot-reload smoke (optional, interactive — for the human reviewer)**

Run `./Loom.exe` (no `--frames`); in another shell rebuild only PlaygroundGame
(`msbuild ... -t:PlaygroundGame`); confirm Loom logs `plugin reloaded (gen N ...)` and the
scene keeps running (state preserved). Press F6 to confirm a fresh-boot reload.

- [ ] **Step 4: Update the Arcane build-system docs in `CLAUDE.md`** (one paragraph under
  "Build System (Arcane engine)") noting Loom/PlaygroundGame + the hot-reload loop, and the
  new run line:

```
bin\Debug-windows-x86_64-md\Loom\Loom.exe --backend vulkan --frames 180   # plugin host + PlaygroundGame.dll
```

Commit:

```bash
git -C "D:/dev/starworks/Gacha" add CLAUDE.md
git -C "D:/dev/starworks/Gacha" commit -m "docs(arcane): M5 plugin host -- Loom + PlaygroundGame + hot-reload loop

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

- [ ] **Step 5: Push the branch; let CI go green before merge**

```bash
git -C "D:/dev/starworks/Gacha" push -u origin feature/arcane-m5-plugin-host
```

Then (per the project workflow) let Jenkins go green, and merge to `main` only after the user
confirms. Update the memory files (`project_arcane_m5_complete.md` + repoint
`project_arcane_next_milestone.md` to M6 = physics port) once merged.

---

## Self-review checklist (run before declaring the plan done)

- **Spec coverage:** Phase 0 (Astra 3.3 Load overload) ✓; Runtime facade + EngineContext +
  PluginVTable (B1/B2) ✓; PluginHost watch/versioned-load/ABI/rollback + hotkeys (C3) ✓;
  PlaygroundGame ABI consumer + standalone Playground untouched (D1, and Playground.exe is not
  modified) ✓; scripted V1→V2 swap (state survived + new code live) + rollback + `[gpu]` live
  proof (C3, E2) ✓; RunLoop plugin interleave (B3) ✓; cross-module TypeContext/ReRegister
  contracts (B2, C2, D1) ✓; same-toolchain /MD, build order, plugin-not-linking-Core (premake
  blocks) ✓.
- **Type/name consistency:** `Runtime::{Registry,Schedulers,Loop,Components,TypeContext,
  WorkScheduler,SetRenderContext,SnapshotRegistry,RestoreRegistry,ClearSystems}`,
  `PluginVTable`, `PluginEntry::k*`, `kGamePluginABIVersion`, `GamePlugin_*`,
  `Detail::{DLOpen,DLSym,DLClose}`, `PluginHost::{Load,Unload,Reload,Poll,ForceReload,
  ReloadFresh,IsLoaded,Vtable,Generation}` are used identically across all tasks.
- **No placeholders:** every code step shows full code; every build/run step shows the command
  + expected outcome. The one "match the existing form verbatim" note (Loom's
  `input_actions.json` — copy `Playground/data/input_actions.json` and add only the two reload
  button actions) points at an existing in-repo file rather than inventing a schema. The
  `[gpu]` device setup is copied from `SceneSliceTest` (no invented helper).
- **Cross-module correctness (verified against Astra):** `Runtime` installs the shared
  `TypeContext` before building its Registry; the plugin installs it first in `Init`;
  `ArcaneTests` installs one shared context process-wide (Task B0) and injects it into every
  `Runtime`; the test-plugin's `Pulse` is a single shared type (not per-TU anonymous). Systems
  are named structs (a `void(Registry&)` lambda fails `LambdaLike`). `SystemScheduler::Empty()`
  (not `IsEmpty()`). `Registry::Load` uses the 3.3 `Config` overload so the restored registry
  keeps its scheduler. `ReloadFresh` calls `ResetRegistry()` so `Init` rebuilds the scene.
