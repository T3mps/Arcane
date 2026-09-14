# Game module boilerplate — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One SDK header, `Arcane/Plugin/GameModule.hpp`, so a game module is a class deriving `Arcane::GameModule` plus one `ARCANE_GAME_MODULE(Type)` line; the engine owns its standard scene systems; a game system declares its placement with Astra's `Before`/`After`; and all three existing module sources (HotReloadPlugin, ReferenceGame, Aphelyon) are on it.

**Architecture:** `GameModule` is a base class with seven defaulted virtual hooks and accessors bound by the macro (`Context/Engine/Registry/Components/SceneRootEntity`). The macro expands to a per-module `GameModuleDetail::State` (three raw pointers, trivially destructible — the plugin contract's no-plugin-side-statics rule) and the eight `extern "C"` exports, whose bodies are today's ReferenceGame bodies moved into inline functions that instantiate in the module image (the TypeContext pin, Mosaic installs and the `ARCANE_COMPONENT` drain MUST run in the module). `Runtime::InstallEngineSystems()` grows from `PhysicsSystem` to the standard three with per-system `HasSystem` guards; `ClearSystems()` already reinstalls. Ordering rides on Astra's name-hashed `TypeID` and `SystemTraits<Before/After>` (Astra `9607fb6`, vendored).

**Tech Stack:** C++23, Astra (vendored; `SystemTraits`, `ComponentModule`, `TypeID`), Dear ImGui (dllimport from ArcaneClient), Catch2, premake5 / MSBuild (VS 18), the arcbuild driver for the Gacha Game.

**Spec:** `docs/specs/2026-09-13-game-module-boilerplate-design.md` (`b4d28f88`). §3 the header, §4 engine-owned systems + placement, §5 the three consumers, §6 ABI 29, §7 tests, §8 rollout.

## Global Constraints

- **ABI bump 28 → 29** in Task 1 (the who-registers-systems contract changes there); `ReferenceProject.arcproj` restamped in the same commit; Gacha's `Aphelyon.arcproj` restamped in Task 4's Gacha commit. No change to `EngineContext`, the export names or signatures.
- **Build ritual (bash):** msbuild = `MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nologo -v:m` (dash forms; whole solution). Regenerate with `cmd /c ".\GenerateProjects.bat"` (PowerShell) whenever `premake5.lua` changes or a `.cpp`/`.hpp` is added or removed (the vcxproj is a snapshot of the globs). Tests run **FROM** `bin/<cfg>-windows-x86_64-md/ArcaneTests/`. **An engine rebuild ⇒ rebuild `ReferenceProject.slnx` (`-t:Rebuild`) before any host launch** or the plugin refuses to load; the engine's host post-builds then restage it. The desk's running editor is usually the Release build — a Release `ArcaneEditor.exe` LNK1104 is reported, not fought.
- **Gacha Game builds through the driver:** `D:\dev\starworks\Arcane\bin\<cfg>-windows-x86_64-md\arcbuild\arcbuild.exe build --project D:\dev\starworks\Gacha\Game --config <cfg> --sdk D:\dev\starworks\Arcane`. The desk slot holds **Release**; build Release so it stays that way (probe first, restore what you found).
- **TDD, every task:** the failing test is written and RUN RED before the production change; the step names the expected failure. Task 2's RED is the new hook-order case failing against the raw plugin; Task 3's/4's RED is the module not compiling against the macro until it is converted.
- **Output hygiene:** `msbuild … > log; grep -E " error |Error\(s\)|Warning\(s\)"`; `ArcaneTests.exe "<filter>" | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED"`; never `cat` a suite log.
- **Baseline:** `scripts/automation-baselines.json` booked at Task 6 from measured `~[gpu]` runs, Debug + Release; the rise = Task 1's `[runtime]` cases + Task 2's `[hotreload]` case + Task 5's `[editor]` additions. None is `[gpu]`.
- **Golden lanes:** the runtime-scene and editor-ui lanes render the same systems in the same order (the module registered the pair; now the engine does — once either way), so `diffCount=0` is the expected verdict; the gate is run in **both configs** at Task 6 and Debug at Task 3, never assumed.
- **Git:** Arcane and Gacha `main`, both unpushed — **do not push**. One commit per task (Task 4 in Gacha). Trailers on every commit:
  ```
  Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```
  Never stage `out.txt`, `ArcaneEditor/ArcaneEditor/`, `ArcaneAssetPipeline/ArcaneAs.*/` (Arcane) nor `Game/Source/TestComponent.*` / `Game/Content/scenes/test.arcscene` (Gacha, the user's in-flight files).
- **Ledger:** `.superpowers/sdd/2026-09-13-game-module-boilerplate/progress.md` (gitignored, local): `Task N: complete (sha)` + measured counts.

---

## Plan-time rulings (each with its reason)

| # | Ruling | Why |
|---|---|---|
| R1 | **The export bodies live in `GameModuleDetail::State` inline member functions in the header; the macro only forwards.** | Readable and debuggable (a breakpoint lands in a named function, not a macro line), and the code still instantiates in the module image because the header is included there — which is what the TypeContext pin, the Mosaic installs and the `ARCANE_COMPONENT` drain require. |
| R2 | **`GamePlugin_DrawUI` calls `OnDrawUI` only when the instance exists AND `ctx->imguiContext` is non-null.** | Generalises Aphelyon's own headless guard so no module needs it. |
| R3 | **HotReloadPlugin registers `Pulse` through `ARCANE_COMPONENT`, not `Register<Pulse>()` by hand.** | The drain is the prologue path the macro must be seen to run; the by-hand path is still exercised in `GameComponentsTest`. |
| R4 | **The hook-order probe stamps `Pulse.ticks = -1` in `OnShutdown` iff `Components()` is still an open handle.** Read after `host.Unload()` through the test's own engine-side `Pulse` registration. | `PluginHost` exposes no `Module::Symbol`; the registry survives an unload and the test already registers `Pulse` engine-side so the view resolves. |
| R5 | **The System class template gains `Astra::Before<Arcane::TransformPropagationSystem>` in its default traits** (+ the `TransformSystems.hpp` include). | Gameplay that moves things is the common case and must run before propagation (UE's default tick group is PrePhysics for the same reason); reading world transforms wants `After<…>`, documented beside it. |
| R6 | **Aphelyon's clock (`m_time`) restarts at 0 on hot reload by construction** — the macro's Init constructs a fresh instance. No `OnLoadState` override. | Identical to the old `g_time = 0.0` in `LoadState`, with one fewer thing to write. |
| R7 | **The test plugins stop defining `GAME_BUILD_DLL`; `arcane.lua` keeps it** (comment fixed). | The test plugins have no other user of it; an external module may. |
| R8 | **`#Type` names the `ComponentModule`** (e.g. `"Aphelyon::Module"`), no second macro argument. | One argument, and the log line still says which module registered what. |

---

## File structure

| File | Change | Responsibility |
|---|---|---|
| `ArcaneClient/src/Arcane/Base/Runtime.cpp` | Modify (`InstallEngineSystems`, includes) | Engine owns Physics → TransformPropagation (fixedUpdate) + RenderSubmission (render). |
| `ArcaneClient/src/Arcane/Base/Runtime.hpp` | Modify (comment at :252-258) | Documents the three. |
| `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` | Modify (`:818` bump + ledger entry; `:734` comment) | ABI 29. |
| `ReferenceProject/ReferenceProject.arcproj` | Modify | `"abi": 29`. |
| `ArcaneTests/src/RuntimeEngineSystemsTest.cpp` | Create | `[runtime]` — installed set, reinstall after clear, AlreadyRegistered tolerance, Before/After placement, dangling anchor. |
| `ArcaneClient/src/Arcane/Plugin/GameModule.hpp` | Create | The class, `GameModuleDetail::State`, the two macros. |
| `ArcaneTests/plugins/HotReloadPlugin.cpp` | Rewrite | On the macro (`ARCANE_GAME_MODULE_ABI`), hook-order probe. |
| `ArcaneTests/plugins/PluginExport.hpp` | Delete | Replaced by the macro's export attribute. |
| `ArcaneTests/src/PluginHostTest.cpp` | Modify (+1 case) | `OnShutdown` runs while the handle is open. |
| `premake5.lua` (`test_plugin`) | Modify | imgui + spdlog include dirs, `IMGUI_API` define, `/utf-8`; drop `PluginExport.hpp` + `GAME_BUILD_DLL`. |
| `ReferenceProject/Source/ReferenceGame.cpp` | Rewrite | The class with no overrides + the macro. |
| `ReferenceProject/Source/GameApi.hpp` | Delete | — |
| Gacha `Game/Source/Aphelyon.cpp` | Rewrite | `OnFixedUpdate` + `OnDrawUI` + `m_time`. |
| Gacha `Game/Source/GameApi.hpp` | Delete | — |
| Gacha `Game/Aphelyon.arcproj` | Modify | `"abi": 29`. |
| `ArcaneEditor/src/Project/ClassTemplates.cpp` / `.hpp` | Modify | System template: placement idiom + `OnInit` line; Component template comment; header comment. |
| `ArcaneTests/src/ClassTemplatesTest.cpp` | Modify | Pins the new wording. |
| `ArcaneClient/src/Arcane/Plugin/GameComponents.hpp` | Modify (comment) | "the `ARCANE_GAME_MODULE` prologue". |
| `build/arcane.lua:90` | Modify (comment) | Stops naming `GameApi.hpp`. |
| `scripts/automation-baselines.json`, spec status, this plan's Closeout | Modify (Task 6) | Booking + close. |

---

### Task 1: Engine-owned scene systems + ABI 29

**Files:**
- Create: `ArcaneTests/src/RuntimeEngineSystemsTest.cpp`
- Modify: `ArcaneClient/src/Arcane/Base/Runtime.cpp` (`InstallEngineSystems` at `:419-426`; includes at `:12-15`)
- Modify: `ArcaneClient/src/Arcane/Base/Runtime.hpp` (comment `:252-258`)
- Modify: `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` (`:818`; the ledger above it; the `:734` comment)
- Modify: `ReferenceProject/ReferenceProject.arcproj:6`

**Interfaces:**
- Consumes: `Arcane::Runtime::{Schedulers(), ClearSystems(), Loop()}`; `Astra::SystemScheduler::{HasSystem<T>(), AddSystem<T>() -> Result}`; `Arcane::{PhysicsSystem, TransformPropagationSystem, RenderSubmissionSystem}`; `Arcane::Test::SharedTypeContext()` (`ArcaneTests/src/Helpers/TestTypeContext.hpp`).
- Produces: the contract every later task relies on — after `Runtime` construction and after every `ClearSystems()`, `fixedUpdate` has `PhysicsSystem` then `TransformPropagationSystem`, `render` has `RenderSubmissionSystem`; a module registering either again gets `AlreadyRegistered`.

- [ ] **Step 1: Write the failing tests** — create `ArcaneTests/src/RuntimeEngineSystemsTest.cpp`:

```cpp
// Engine-owned scene systems (spec docs/specs/2026-09-13-game-module-boilerplate-
// design.md s4): Runtime::InstallEngineSystems owns the standard three, they
// survive ClearSystems (module unload / hot reload), a module that still
// registers them is harmlessly refused, and a game system PLACES itself with
// Astra's Before/After against the engine's types -- across the DLL boundary,
// because Astra keys systems by a hash of the type NAME.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <string>
#include <vector>

// NAMESPACED probes, not anonymous: Astra keys a system by TypeID<T>::Hash(),
// a hash of the type name, and an anonymous-namespace name is per-TU. The
// Before/After anchors below name engine types by the same rule -- which is
// exactly what a game module compiled into another DLL does.
namespace Arcane::Test::EngineSystems
{
    inline std::vector<std::string> g_order;

    struct BeforePropagation : Astra::SystemTraits<Astra::Before<Arcane::TransformPropagationSystem>>
    {
        void operator()(Astra::Registry&) { g_order.push_back("before"); }
    };
    struct AfterPropagation : Astra::SystemTraits<Astra::After<Arcane::TransformPropagationSystem>>
    {
        void operator()(Astra::Registry&) { g_order.push_back("after"); }
    };
    struct Untraited
    {
        void operator()(Astra::Registry&) { g_order.push_back("untraited"); }
    };
    // Complete, never registered: an anchor the scheduler cannot resolve.
    struct NeverRegistered { void operator()(Astra::Registry&) {} };
    struct NamesDangling : Astra::SystemTraits<Astra::Before<NeverRegistered>>
    {
        void operator()(Astra::Registry&) { g_order.push_back("dangling"); }
    };

    inline void OneFixedStep(Arcane::Runtime& rt)
    {
        // 1/60 s at the default 60 Hz = exactly one fixed step (PluginHostTest's StepK shape).
        rt.Loop().Advance(1.0 / 60.0, [](double) {}, [](double, double) {});
    }
}

using namespace Arcane::Test::EngineSystems;

TEST_CASE("Runtime installs the engine's standard systems and reinstalls them after ClearSystems", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    auto& sch = rt.Schedulers();

    CHECK(sch.fixedUpdate.HasSystem<Arcane::PhysicsSystem>());
    CHECK(sch.fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>());
    CHECK(sch.render.HasSystem<Arcane::RenderSubmissionSystem>());
    CHECK_FALSE(sch.update.HasSystem<Arcane::TransformPropagationSystem>());

    rt.ClearSystems();   // what PluginHost does around a module unload / reload
    CHECK(sch.fixedUpdate.HasSystem<Arcane::PhysicsSystem>());
    CHECK(sch.fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>());
    CHECK(sch.render.HasSystem<Arcane::RenderSubmissionSystem>());
}

TEST_CASE("A module built against ABI 28 that still registers the pair is refused harmlessly", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    auto& sch = rt.Schedulers();
    // AlreadyRegistered -- the only failure AddSystem<T> has for a known T; the
    // old modules std::ignore it, so an unconverted DLL keeps working.
    CHECK(sch.fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>().IsErr());
    CHECK(sch.render.AddSystem<Arcane::RenderSubmissionSystem>().IsErr());
}

TEST_CASE("A game system places itself with Before/After against the engine's systems", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    auto& fixed = rt.Schedulers().fixedUpdate;
    g_order.clear();

    // Registered AFTER the engine's install and in this insertion order:
    // untraited first, so insertion order alone would run it FIRST.
    REQUIRE(fixed.AddSystem<Untraited>().IsOk());
    REQUIRE(fixed.AddSystem<AfterPropagation>().IsOk());
    REQUIRE(fixed.AddSystem<BeforePropagation>().IsOk());

    OneFixedStep(rt);

    REQUIRE(g_order.size() == 3);
    // Before<Propagation> runs before After<Propagation> regardless of insertion;
    // the untraited one keeps its insertion slot relative to the engine's
    // systems (it was added after them) and is unconstrained against the probes.
    const auto pos = [&](const char* s) {
        for (std::size_t i = 0; i < g_order.size(); ++i) if (g_order[i] == s) return i;
        return g_order.size();
    };
    CHECK(pos("before") < pos("after"));
    CHECK(pos("untraited") < g_order.size());
}

TEST_CASE("An ordering anchor that is not registered adds no constraint and no error", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    auto& fixed = rt.Schedulers().fixedUpdate;
    g_order.clear();

    // Before<NeverRegistered>: the headless-host case (a module naming
    // RenderSubmissionSystem in a host that never installed it).
    REQUIRE(fixed.AddSystem<NamesDangling>().IsOk());
    OneFixedStep(rt);
    REQUIRE(g_order.size() == 1);
    CHECK(g_order[0] == "dangling");
}
```

- [ ] **Step 2: Regenerate, build, run RED**

Run (PowerShell): `cd D:\dev\starworks\Arcane; cmd /c ".\GenerateProjects.bat"`
Run (bash): Debug build; then `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "[runtime]" | grep -E "FAILED|test cases|passed"`.
Expected: the first case FAILS on `HasSystem<Arcane::TransformPropagationSystem>()` and `HasSystem<Arcane::RenderSubmissionSystem>()` (only Physics is installed today); the second FAILS (`AddSystem` succeeds today). The placement cases may pass or fail — they are pinned for regression, not the RED.

- [ ] **Step 3: `Runtime::InstallEngineSystems`** — in `Runtime.cpp`, add after `:15`:

```cpp
#include <Arcane/Scene/RenderSystems.hpp>       // RenderSubmissionSystem (engine-owned, instantiated IN this module)
#include <Arcane/Scene/TransformSystems.hpp>    // TransformPropagationSystem (engine-owned, instantiated IN this module)
```

and replace `InstallEngineSystems` (`:419-426`) with:

```cpp
    void Runtime::InstallEngineSystems()
    {
        // The engine's STANDARD systems, owned here (spec docs/specs/2026-09-13-
        // game-module-boilerplate-design.md s4.1) -- the UE/DOTS shape: the engine
        // ticks the world; a game module registers only its own systems and
        // places them with Astra::Before/After against these types. Each behind
        // its own HasSystem guard: AlreadyRegistered is the only failure and
        // this runs from the ctor AND after every ClearSystems. Order within a
        // scheduler: PhysicsSystem declares Before<TransformPropagationSystem>;
        // insertion order carries the rest (Astra's reorder is stable).
        auto& fixed  = m_impl->schedulers->fixedUpdate;
        auto& render = m_impl->schedulers->render;
        if (!fixed.HasSystem<PhysicsSystem>())
        {
            const float fixedDt = static_cast<float>(1.0 / m_impl->loopCfg.fixedHz);
            std::ignore = fixed.AddSystem<PhysicsSystem>(fixedDt, /*stepWorld*/ true);
        }
        if (!fixed.HasSystem<TransformPropagationSystem>())
            std::ignore = fixed.AddSystem<TransformPropagationSystem>();
        if (!render.HasSystem<RenderSubmissionSystem>())
            std::ignore = render.AddSystem<RenderSubmissionSystem>();
    }
```

In `Runtime.hpp` replace the comment lines `:254-258` ("PhysicsWorld. InstallEngineSystems adds the engine's own systems … keeps it.") with:

```cpp
        // PhysicsWorld. InstallEngineSystems adds the engine's own systems --
        // the STANDARD THREE (2026-09-13 game-module boilerplate spec s4.1):
        // PhysicsSystem then TransformPropagationSystem into fixedUpdate,
        // RenderSubmissionSystem into render; the ctor calls it, and
        // ClearSystems calls it again after clearing, so every PluginHost
        // load/reload/unload path keeps them. Idempotent (per-system HasSystem
        // guards). A game module registers ONLY its own systems and places them
        // with Astra::Before/After against these types (GameModule.hpp).
```

- [ ] **Step 4: ABI 29** — in `PluginABI.hpp` change `:818` to `inline constexpr uint32_t kGamePluginABIVersion = 29;` and insert this ledger entry directly above it (after the v28 entry's last line):

```cpp
    // v29 (2026-09-13, game-module boilerplate): the ENGINE now registers
    //     TransformPropagationSystem (fixedUpdate) and RenderSubmissionSystem
    //     (render) in Runtime::InstallEngineSystems beside PhysicsSystem; a
    //     game module registers ONLY its own systems and places them with
    //     Astra::Before/After. Export names, signatures and EngineContext are
    //     UNCHANGED -- a v28 module's own AddSystem<> of the pair returns
    //     AlreadyRegistered (std::ignore'd, harmless), so nothing breaks at
    //     load; the bump makes the contract change visible through the gate
    //     instead of letting it pass by accident. Arcane/Plugin/GameModule.hpp
    //     (ARCANE_GAME_MODULE) is the SDK face of the same contract.
    //     ReferenceProject.arcproj restamped with this change; Gacha's Game
    //     restamp (28 -> 29) is this plan's Task 4, in that repo, with the
    //     Aphelyon.cpp conversion -- not deferred.
```

Also rewrite the `:734-736` comment fragment "and AddSystem<TransformPropagationSystem/RenderSubmission System> -- header-only systems whose bodies change under them" to "(the pair the engine registers itself since v29 -- header-only systems whose bodies change under them)".

`ReferenceProject/ReferenceProject.arcproj:6` → `"abi": 29`.

- [ ] **Step 5: Build, rebuild ReferenceProject (both configs), run GREEN**

Run (bash): Debug build of `Arcane.slnx`; then `MSYS_NO_PATHCONV=1 "$MSB" ReferenceProject/ReferenceProject.slnx -p:Configuration=Debug -t:Rebuild -m -nologo -v:m` (the engine changed; the module must relink against it), then Debug build of `Arcane.slnx` again (the hosts restage `ReferenceProject/`). Then `./ArcaneTests.exe "[runtime]"` → all four new cases pass; `./ArcaneTests.exe "[hotreload]"` → still green (the raw plugin's extra `AddSystem` is the AlreadyRegistered path — note it does not register the pair at all, so nothing to refuse; the case is the ReferenceGame DLL, exercised by `[plugin]`/`[witness]` cases and the gate).

- [ ] **Step 6: Commit (Arcane)**

```bash
cd /d/dev/starworks/Arcane && git add ArcaneClient/src/Arcane/Base/Runtime.cpp ArcaneClient/src/Arcane/Base/Runtime.hpp ArcaneClient/src/Arcane/Plugin/PluginABI.hpp ReferenceProject/ReferenceProject.arcproj ArcaneTests/src/RuntimeEngineSystemsTest.cpp && git commit -q -F - <<'EOF'
feat(runtime)!: the engine owns its standard scene systems -- TransformPropagation + RenderSubmission join PhysicsSystem in InstallEngineSystems; ABI 29 (game-module boilerplate Task 1)

A game module registers only its own systems and places them with
Astra::Before/After against the engine's types (name-hashed TypeID, so it
works across the DLL boundary). A v28 module's own AddSystem of the pair
returns AlreadyRegistered, harmlessly. ReferenceProject.arcproj restamped.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
```

---

### Task 2: `GameModule.hpp` + HotReloadPlugin on the macro (the plugin test vehicle)

**Files:**
- Modify: `ArcaneTests/src/PluginHostTest.cpp` (+1 case after the first `TEST_CASE`, `:58`)
- Create: `ArcaneClient/src/Arcane/Plugin/GameModule.hpp`
- Rewrite: `ArcaneTests/plugins/HotReloadPlugin.cpp`
- Delete: `ArcaneTests/plugins/PluginExport.hpp`
- Modify: `premake5.lua` (`test_plugin`, `:1211-1244`)

**Interfaces:**
- Consumes: `EngineContext{typeContext, engine, imguiContext, imguiAlloc, imguiFree, imguiUserData}` (`PluginABI.hpp:835-848`); `Runtime::{Components(), Registry(), SnapshotRegistry(), RestoreRegistry()}`; `Astra::ComponentModule::Open(shared_ptr<ComponentRegistry>, string_view)` + `explicit operator bool`; `Arcane::Game::RegisterComponents(ComponentModule&) -> size_t`; `Log::InstallMosaicSink()`, `Assert::InstallMosaicHandler()`; `Arcane::SceneRoot{entity}` (`Scene/SceneResources.hpp`); `PluginEntry::k*` names.
- Produces (used by Tasks 3–5):
  ```cpp
  namespace Arcane {
      class GameModule { /* OnInit/OnShutdown/OnFixedUpdate/OnUpdate/OnDrawUI/OnSaveState/OnLoadState;
                            Context()/Engine()/Registry()/Components()/SceneRootEntity(); BindForMacro_ */ };
      namespace GameModuleDetail { struct State { EngineContext* ctx; Astra::ComponentModule* components; GameModule* instance;
                                                  template<class T> bool Init(EngineContext*, const char*); void Shutdown();
                                                  void SaveState(Astra::BinaryWriter&); bool LoadState(Astra::BinaryReader&); }; }
  }
  #define ARCANE_GAME_MODULE(Type)          ARCANE_GAME_MODULE_ABI(Type, ::Arcane::kGamePluginABIVersion)
  #define ARCANE_GAME_MODULE_ABI(Type, Abi) /* the eight exports over a per-module State */
  ```

- [ ] **Step 1: Write the failing test** — in `ArcaneTests/src/PluginHostTest.cpp`, after the first case (ends `:58`), add:

```cpp
// The GameModule hook-order probe (spec 2026-09-13 s7): ARCANE_GAME_MODULE's
// Shutdown calls OnShutdown BEFORE it closes the module's ComponentModule
// handle (the instance goes first so a module can still touch its own
// components). HotReloadPlugin's OnShutdown stamps the pulse with -1 ONLY if
// its Components() handle is still open at that moment; the registry outlives
// the unload, and this test's own engine-side Pulse registration keeps the
// view resolvable after the module's descriptor is gone.
TEST_CASE("GameModule: OnShutdown runs while the module's component handle is still open", "[hotreload]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();

    Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
    REQUIRE(host.Load());
    StepK(rt, *host.Vtable(), 2);
    REQUIRE(ReadPulse(rt) == 2);

    host.Unload();                                 // Shutdown -> OnShutdown (stamps -1) -> handle closes -> unmap
    CHECK(ReadPulse(rt) == -1);
}
```

- [ ] **Step 2: Build, run RED**

Run (bash): Debug build; `./ArcaneTests.exe "*OnShutdown runs while*"`.
Expected: FAIL — `ReadPulse(rt) == -1` reads `2` (the raw plugin's `GamePlugin_Shutdown` stamps nothing).

- [ ] **Step 3: Write the header** — create `ArcaneClient/src/Arcane/Plugin/GameModule.hpp`:

```cpp
#pragma once

// GameModule: the SDK's game-module boilerplate, written ONCE (spec docs/specs/
// 2026-09-13-game-module-boilerplate-design.md) -- Unreal's
// IMPLEMENT_PRIMARY_GAME_MODULE shaped for Arcane's plugin ABI. A module is a
// class deriving GameModule plus one line:
//
//   struct Module final : Arcane::GameModule
//   {
//       void OnDrawUI() override { /* HUD */ }
//   };
//   ARCANE_GAME_MODULE(MyGame::Module)
//
// The macro emits the eight exports the host resolves (PluginEntry::k*,
// PluginABI.hpp) and everything a module used to copy: the shared TypeContext
// pin, the Mosaic log-sink + assert-handler installs, the ImGui context/
// allocator adoption, this module's Astra::ComponentModule with the
// ARCANE_COMPONENT drain (GameComponents.hpp), and the registry Save/LoadState
// round-trip for hot reload. Every hook has a default; override what the
// module needs. THE ENGINE OWNS ITS STANDARD SYSTEMS (Runtime::
// InstallEngineSystems: PhysicsSystem -> TransformPropagationSystem in
// fixedUpdate, RenderSubmissionSystem in render) -- a module registers ONLY its
// own systems, in OnInit, and places them with Astra::Before<...> /
// Astra::After<...> against the engine's types (Astra keys systems by a hash
// of the type NAME, so that works across the DLL boundary).
//
// EVERYTHING HERE INSTANTIATES IN THE MODULE IMAGE, on purpose: SetTypeContext,
// the Mosaic installs and RegisterComponents each act on THIS module's own
// per-module state, and the ComponentModule handle owns descriptors that point
// into this image. That is why the bodies are inline in a header rather than
// exported from ArcaneClient.dll.

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/GameComponents.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // SceneRoot: SceneRootEntity() + the Save/LoadState root id

#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <imgui.h>   // ABI v2: adopt the host's ImGui context/allocators (imported from ArcaneClient.dll)

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
  #define ARCANE_GAME_MODULE_EXPORT __declspec(dllexport)
#else
  #define ARCANE_GAME_MODULE_EXPORT __attribute__((visibility("default")))
#endif

namespace Arcane
{
    class GameModule
    {
    public:
        virtual ~GameModule() = default;

        // ---- hooks (every one defaulted; override what the module needs) ----

        // After the prologue (TypeContext, Mosaic, ImGui, this module's
        // ComponentModule drained). Register the module's OWN systems here.
        // false aborts the load (the host reports "initial load failed").
        virtual bool OnInit(EngineContext& ctx) { (void)ctx; return true; }
        // Before the ComponentModule handle closes and before the image unmaps;
        // Context()/Registry()/Components() are still valid here.
        virtual void OnShutdown() {}
        virtual void OnFixedUpdate(double dt) { (void)dt; }
        virtual void OnUpdate(double dt, double alpha) { (void)dt; (void)alpha; }
        // Between the host's ImGui BeginFrame and Render. Not called in a headless
        // host (no ImGui context) -- no guard needed in the override.
        virtual void OnDrawUI() {}
        // Module extras, written AFTER the registry blob / read AFTER the registry
        // restore. The registry round-trip itself is the macro's (never overridden).
        virtual void OnSaveState(Astra::BinaryWriter& w) { (void)w; }
        virtual bool OnLoadState(Astra::BinaryReader& r) { (void)r; return true; }

        // ---- what the prologue established (valid from OnInit to OnShutdown) ----

        [[nodiscard]] EngineContext& Context() const noexcept
        {
            ARC_ASSERT(m_ctx != nullptr, "GameModule::Context() outside the OnInit..OnShutdown window");
            return *m_ctx;
        }
        [[nodiscard]] Runtime&         Engine()   const noexcept { return *Context().engine; }
        [[nodiscard]] Astra::Registry& Registry() const noexcept { return Engine().Registry(); }
        [[nodiscard]] Astra::ComponentModule& Components() const noexcept
        {
            ARC_ASSERT(m_components != nullptr, "GameModule::Components() outside the OnInit..OnShutdown window");
            return *m_components;
        }
        // The scene's root entity, read from the registry ON DEMAND -- never
        // cached: Init runs before the host loads the boot scene, and File > Open
        // Scene swaps the whole registry. Invalid when no scene is loaded.
        [[nodiscard]] Astra::Entity SceneRootEntity() const
        {
            const SceneRoot* sr = Registry().GetResource<SceneRoot>();
            return sr ? sr->entity : Astra::Entity::Invalid();
        }

        // Bound by ARCANE_GAME_MODULE's Init before OnInit runs. Not for modules.
        void BindForMacro_(EngineContext* ctx, Astra::ComponentModule* components) noexcept
        {
            m_ctx        = ctx;
            m_components = components;
        }

    private:
        EngineContext*          m_ctx        = nullptr;
        Astra::ComponentModule* m_components = nullptr;
    };

    namespace GameModuleDetail
    {
        // The per-module state behind the exports: three RAW pointers, so the
        // object is trivially destructible. The ComponentModule contract's
        // "NEVER a plugin-side static/global object" rule: a static whose
        // destructor does live cleanup (a ComponentModule by value, an optional,
        // a unique_ptr) runs it during FreeLibrary at DLL_PROCESS_DETACH, under
        // the loader lock. Raw pointers have no destructor; a skipped Shutdown
        // degrades to the contract's forget semantics (the handle leaks and the
        // host's UnregisterModuleRange net catches the descriptor half).
        struct State
        {
            EngineContext*          ctx        = nullptr;
            Astra::ComponentModule* components = nullptr;
            GameModule*             instance   = nullptr;

            template <typename Type>
            bool Init(EngineContext* c, const char* name)
            {
                static_assert(std::is_base_of_v<GameModule, Type>,
                              "ARCANE_GAME_MODULE(Type): Type must derive from Arcane::GameModule");

                // 1. The shared reflection context in THIS module, and this
                // module's Mosaic log/assert routing into the engine's.
                Astra::SetTypeContext(c->typeContext);
                Log::InstallMosaicSink();
                Assert::InstallMosaicHandler();
                ctx = c;

                // ABI v2: adopt the host's ImGui context + allocators so OnDrawUI
                // draws into the host's single GImGui. Null in a headless host.
                if (c->imguiContext)
                {
                    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(c->imguiContext));
                    ImGui::SetAllocatorFunctions(
                        reinterpret_cast<ImGuiMemAllocFunc>(c->imguiAlloc),
                        reinterpret_cast<ImGuiMemFreeFunc>(c->imguiFree),
                        c->imguiUserData);
                }

                // 2. This module's own component types: open the handle, drain the
                // ARCANE_COMPONENT registrar into it. Every component added under
                // Source/ (Assets -> Create -> C++ Class, or one ARCANE_COMPONENT
                // line by hand) registers here with no edit to the module.
                components = new Astra::ComponentModule(
                    Astra::ComponentModule::Open(c->engine->Components(), name));
                if (!*components)
                {
                    delete components;   // empty handle: safe to destroy here (still mapped)
                    components = nullptr;
                    ctx        = nullptr;
                    return false;        // SetTypeContext above makes this unreachable; fail loudly if not
                }
                const std::size_t count = Game::RegisterComponents(*components);
                ARC_INFO("{}: registered {} module component type(s)", name, count);

                // 3. The module itself. Systems are the module's to register in
                // OnInit -- the engine's standard ones are already installed.
                instance = new Type();
                instance->BindForMacro_(c, components);
                if (!instance->OnInit(*c))
                {
                    delete instance;   instance   = nullptr;
                    delete components; components = nullptr;
                    ctx = nullptr;
                    return false;
                }
                return true;
            }

            void Shutdown()
            {
                // Instance first (its destructor may still touch its own
                // components), then the handle -- which releases this module's
                // descriptors + meta BEFORE the image unmaps.
                if (instance)
                {
                    instance->OnShutdown();
                    delete instance;
                    instance = nullptr;
                }
                delete components;
                components = nullptr;
                ctx        = nullptr;
            }

            void SaveState(Astra::BinaryWriter& w)
            {
                // Persist the scene-root entity id explicitly -- resources are not
                // part of the registry snapshot, so LoadState must re-set SceneRoot
                // after the restore. Read live: whatever scene is loaded NOW is the
                // one a hot reload has to bring back.
                const Astra::Entity root = instance ? instance->SceneRootEntity() : Astra::Entity::Invalid();
                w(static_cast<uint64_t>(root));

                auto snap = ctx->engine->SnapshotRegistry();
                if (snap.IsErr())
                {
                    // Snapshot failed: write a zero-length blob so LoadState fails
                    // cleanly (RestoreRegistry rejects an empty frame) instead of
                    // masking the loss. No extras follow a failed blob.
                    w(static_cast<uint64_t>(0));
                    return;
                }
                const std::vector<std::byte>& blob = *snap.GetValue();
                w(static_cast<uint64_t>(blob.size()));
                w.WriteBytes(blob.data(), blob.size());

                if (instance)
                    instance->OnSaveState(w);
            }

            bool LoadState(Astra::BinaryReader& r)
            {
                uint64_t rootRaw = 0; r(rootRaw);
                const Astra::Entity savedRoot(static_cast<Astra::Entity::StorageType>(rootRaw));

                uint64_t n = 0; r(n);
                std::vector<std::byte> blob(static_cast<std::size_t>(n));
                r.ReadBytes(blob.data(), static_cast<std::size_t>(n));
                if (r.HasError()) return false;
                if (!ctx->engine->RestoreRegistry(blob)) return false;

                // SceneRoot is a resource; the snapshot does not carry it. A zero id
                // means the reload happened with no scene loaded -- a legitimate
                // state; leave the resource unset rather than publish an invalid root.
                if (savedRoot.IsValid())
                    ctx->engine->Registry().SetResource<SceneRoot>(SceneRoot{savedRoot});

                return instance ? instance->OnLoadState(r) : true;
            }
        };
    }
}

// The one-argument face: the module reports the SDK's own ABI version.
#define ARCANE_GAME_MODULE(Type) ARCANE_GAME_MODULE_ABI(Type, ::Arcane::kGamePluginABIVersion)

// The two-argument form exists for ONE caller: the HotReloadPluginBad test
// build, which must report a version the host's gate refuses. A real module
// never passes anything but the SDK's constant.
#define ARCANE_GAME_MODULE_ABI(Type, Abi)                                                          \
    namespace { ::Arcane::GameModuleDetail::State arcane_game_module_state_; }                     \
    extern "C"                                                                                     \
    {                                                                                              \
        ARCANE_GAME_MODULE_EXPORT uint32_t GamePlugin_ABIVersion()                                 \
        { return static_cast<uint32_t>(Abi); }                                                     \
        ARCANE_GAME_MODULE_EXPORT bool GamePlugin_Init(::Arcane::EngineContext* ctx)               \
        { return arcane_game_module_state_.Init<Type>(ctx, #Type); }                                \
        ARCANE_GAME_MODULE_EXPORT void GamePlugin_Shutdown()                                       \
        { arcane_game_module_state_.Shutdown(); }                                                  \
        ARCANE_GAME_MODULE_EXPORT void GamePlugin_FixedUpdate(double dt)                           \
        { if (auto* m = arcane_game_module_state_.instance) m->OnFixedUpdate(dt); }                \
        ARCANE_GAME_MODULE_EXPORT void GamePlugin_Update(double dt, double alpha)                  \
        { if (auto* m = arcane_game_module_state_.instance) m->OnUpdate(dt, alpha); }              \
        ARCANE_GAME_MODULE_EXPORT void GamePlugin_DrawUI()                                         \
        {                                                                                          \
            auto& s = arcane_game_module_state_;                                                   \
            if (s.instance && s.ctx && s.ctx->imguiContext) s.instance->OnDrawUI();                \
        }                                                                                          \
        ARCANE_GAME_MODULE_EXPORT void GamePlugin_SaveState(::Astra::BinaryWriter& w)              \
        { arcane_game_module_state_.SaveState(w); }                                                \
        ARCANE_GAME_MODULE_EXPORT bool GamePlugin_LoadState(::Astra::BinaryReader& r)              \
        { return arcane_game_module_state_.LoadState(r); }                                         \
    }
```

- [ ] **Step 4: Rewrite the test plugin** — replace `ArcaneTests/plugins/HotReloadPlugin.cpp` with:

```cpp
// Minimal hot-reload test plugin. Built into three DLLs from this one source:
//   HotReloadPluginV1  -> HOTRELOAD_STEP=1,  ABI = kGamePluginABIVersion
//   HotReloadPluginV2  -> HOTRELOAD_STEP=10, ABI = kGamePluginABIVersion
//   HotReloadPluginBad -> ABI = kGamePluginABIVersion + 999 (forces rollback)
// Built ON the SDK's ARCANE_GAME_MODULE (Arcane/Plugin/GameModule.hpp), so the
// [hotreload] suite is the macro's plugin test: the prologue (Pulse arrives
// through the ARCANE_COMPONENT drain), the base Save/LoadState round-trip plus
// this module's extras, the Shutdown order (the -1 stamp below), and the
// ABI-override seam the Bad build exists to trip.

#include "HotReloadShared.hpp"

#include <Arcane/Plugin/GameModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cstdint>

#ifndef HOTRELOAD_STEP
  #define HOTRELOAD_STEP 1
#endif
#ifndef HOTRELOAD_ABI_OFFSET
  #define HOTRELOAD_ABI_OFFSET 0
#endif

// One reflected component (shared header), registered through the drain the
// macro's Init performs -- the same path a wizard-made component takes.
ARCANE_COMPONENT(Arcane::HotReloadTest::Pulse)

namespace Arcane::HotReloadTest
{
    struct Module final : Arcane::GameModule
    {
        Astra::Entity pulse = Astra::Entity::Invalid();

        void CacheHandle()
        {
            pulse = Astra::Entity::Invalid();
            Registry().CreateView<Pulse>().ForEach([&](Astra::Entity e, Pulse&) { pulse = e; });
        }

        bool OnInit(Arcane::EngineContext&) override
        {
            bool exists = false;
            Registry().CreateView<Pulse>().ForEach([&](Astra::Entity, Pulse&) { exists = true; });
            if (!exists)
                Registry().CreateEntityWith(Pulse{0});   // fresh boot only
            CacheHandle();
            return true;
        }

        void OnFixedUpdate(double) override
        {
            if (auto* p = Registry().GetComponent<Pulse>(pulse))
                p->ticks += (HOTRELOAD_STEP);            // V1: +1, V2: +10 (observably different code)
        }

        // The hook-order probe (PluginHostTest "OnShutdown runs while the
        // module's component handle is still open"): stamp -1 ONLY if this
        // module's ComponentModule handle is still open here -- i.e. the macro
        // tore the instance down BEFORE the handle.
        void OnShutdown() override
        {
            if (static_cast<bool>(Components()))
                if (auto* p = Registry().GetComponent<Pulse>(pulse))
                    p->ticks = -1;
        }

        // Extras AFTER the base's registry blob: the pulse entity id, so
        // OnLoadState can prove the base restored the entity it re-finds by view.
        void OnSaveState(Astra::BinaryWriter& w) override
        {
            w(static_cast<uint64_t>(pulse));
        }
        bool OnLoadState(Astra::BinaryReader& r) override
        {
            uint64_t saved = 0; r(saved);
            CacheHandle();
            return !r.HasError() && static_cast<uint64_t>(pulse) == saved;
        }
    };
}

ARCANE_GAME_MODULE_ABI(Arcane::HotReloadTest::Module,
                       ::Arcane::kGamePluginABIVersion + (HOTRELOAD_ABI_OFFSET))
```

Delete `ArcaneTests/plugins/PluginExport.hpp`.

- [ ] **Step 5: premake `test_plugin`** — replace `:1221-1231` (`files`, `includedirs`, `links`, `defines`) with:

```lua
        files { "%{prj.location}/HotReloadPlugin.cpp", "%{prj.location}/HotReloadShared.hpp" }
        -- The include surface a game module gets from build/arcane.lua, minus the
        -- project's own Source/: GameModule.hpp pulls Log.hpp (spdlog) and imgui.h
        -- (the ABI v2 handoff) on top of what Runtime.hpp already needed.
        includedirs {
            "%{wks.location}/ArcaneClient/src",
            "%{IncludeDir.ArcaneCore}",   -- Runtime.hpp (plugin API) includes <Arcane/Guid.hpp>
            "%{IncludeDir.glm}",
            "%{IncludeDir.Astra}",
            "%{IncludeDir.enkiTS}",
            "%{IncludeDir.Mosaic}",   -- Astra headers now #include <Mosaic/...> (Mosaic-seam adoption)
            "%{IncludeDir.imgui}",
            "%{IncludeDir.spdlog}",
        }
        links { "ArcaneClient" }
        -- IMGUI_API=dllimport: adopt ArcaneClient.dll's single GImGui, exactly as
        -- arcane.lua does for a real module.
        defines (defs)
        defines { "IMGUI_API=__declspec(dllimport)" }
```

and `:1234` `buildoptions { "/Zc:__cplusplus", "/bigobj" }` → `buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj" }` (spdlog/fmt want `/utf-8`, as arcane.lua sets). Replace `:1242-1244` with:

```lua
test_plugin("HotReloadPluginV1",  { "HOTRELOAD_STEP=1",          "_CRT_SECURE_NO_WARNINGS" })
test_plugin("HotReloadPluginV2",  { "HOTRELOAD_STEP=10",         "_CRT_SECURE_NO_WARNINGS" })
test_plugin("HotReloadPluginBad", { "HOTRELOAD_ABI_OFFSET=999",  "_CRT_SECURE_NO_WARNINGS" })
```

and the block comment at `:1206-1210` gains one line: `-- Built ON Arcane/Plugin/GameModule.hpp (ARCANE_GAME_MODULE_ABI) -- the macro's plugin test vehicle.`

- [ ] **Step 6: Regenerate, build, run GREEN**

Run (PowerShell): `cmd /c ".\GenerateProjects.bat"`.
Run (bash): Debug build → 0 errors (the three plugin DLLs rebuild on the macro); `./ArcaneTests.exe "[hotreload]"` → every existing case still passes AND the new one; `./ArcaneTests.exe "[plugin],[runtime],[witness-unit]"` green. Expected `[hotreload]`: `All tests passed (… in N+1 test cases)`.

- [ ] **Step 7: Commit (Arcane)**

```bash
cd /d/dev/starworks/Arcane && git add ArcaneClient/src/Arcane/Plugin/GameModule.hpp ArcaneTests/plugins/HotReloadPlugin.cpp ArcaneTests/src/PluginHostTest.cpp premake5.lua && git rm -q ArcaneTests/plugins/PluginExport.hpp && git commit -q -F - <<'EOF'
feat(plugin): Arcane::GameModule + ARCANE_GAME_MODULE -- the module boilerplate once, in the SDK; HotReloadPlugin V1/V2/Bad built on it (game-module boilerplate Task 2)

Eight exports over a per-module trivially-destructible State; the
prologue, the ComponentModule drain and the registry Save/LoadState
round-trip instantiate in the module image. [hotreload] is the macro's
plugin test; +1 case pins OnShutdown before the handle closes.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
```

---

### Task 3: ReferenceGame on the macro + the Debug gate

**Files:**
- Rewrite: `ReferenceProject/Source/ReferenceGame.cpp`
- Delete: `ReferenceProject/Source/GameApi.hpp`

**Interfaces:** consumes Task 2's header only.

- [ ] **Step 1: RED — the conversion's failing build.** Delete `ReferenceProject/Source/GameApi.hpp` first and rebuild `ReferenceProject.slnx` (Debug, `-t:Rebuild`): expected `fatal error C1083: Cannot open include file: 'GameApi.hpp'` — the old module cannot exist without its copy of the export macro. (This is the honest RED: there is no unit test of a file's *shape*; the gate in Step 4 is the product test.)

- [ ] **Step 2: Rewrite the module** — `ReferenceProject/Source/ReferenceGame.cpp`:

```cpp
// ReferenceGame: ReferenceProject's game module -- the minimal end-to-end proof of
// the product flow. The SCENE is data (Content/scenes/main.arcscene, loaded by
// the host through the manifest's bootScene); the ENGINE owns the standard
// systems (Runtime::InstallEngineSystems: physics -> transform propagation in
// fixedUpdate, render submission in render); this module's whole job is to EXIST
// on the ABI -- which ARCANE_GAME_MODULE provides in full: the shared TypeContext
// pin, the Mosaic sink/assert installs, ImGui adoption, this module's
// Astra::ComponentModule with the ARCANE_COMPONENT drain (a component added
// under Source/ -- Assets -> Create -> C++ Class, or one ARCANE_COMPONENT line
// by hand -- is live after a rebuild with no edit here), and the registry
// Save/LoadState round-trip for hot reload. Every hook has a default; this
// module overrides NONE -- the proof that the defaults are the whole common
// case. See Arcane/Plugin/GameModule.hpp; HotReloadPlugin.cpp is the same
// macro with overrides.

#include <Arcane/Plugin/GameModule.hpp>

namespace ReferenceGame
{
    struct Module final : Arcane::GameModule {};
}

ARCANE_GAME_MODULE(ReferenceGame::Module)
```

- [ ] **Step 3: Build GREEN + restage** — `ReferenceProject.slnx` Debug `-t:Rebuild` → 0 errors, the log names exactly one compile (`ReferenceGame.cpp`); then `Arcane.slnx` Debug (restages the module into the hosts). Prove the load: `cd bin/Debug-windows-x86_64-md/ArcaneRuntime && ./ArcaneRuntime.exe --headless --project ReferenceProject --frames 30 2>&1 | grep -E "ReferenceGame::Module: registered|initial load failed|plugin"` → the `registered 0 module component type(s)` line, no failure.

- [ ] **Step 4: Debug golden gate** — `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug` → `4 lane(s) passed, 0 red`, every lane `diffCount=0`. If a runtime-scene lane differs, STOP: the engine-owned pair is in a different position than the module's registration was — diagnose against Task 1's order test before touching anything else.

- [ ] **Step 5: Commit (Arcane)**

```bash
cd /d/dev/starworks/Arcane && git add ReferenceProject/Source/ReferenceGame.cpp && git rm -q ReferenceProject/Source/GameApi.hpp && git commit -q -F - <<'EOF'
refactor(reference): ReferenceGame on ARCANE_GAME_MODULE -- a class with no overrides; GameApi.hpp gone (game-module boilerplate Task 3)

Debug golden gate 4/4, diffCount=0: the engine registers the same pair the
module used to, in the same order.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
```

---

### Task 4: Aphelyon on the macro (Gacha) + restamp 29

**Files (Gacha):**
- Rewrite: `Game/Source/Aphelyon.cpp`
- Delete: `Game/Source/GameApi.hpp`
- Modify: `Game/Aphelyon.arcproj:6` → `"abi": 29`

- [ ] **Step 1: Probe the slot, then RED** — `arcbuild probe --project D:\dev\starworks\Gacha\Game --config Release` (record the flavor; the desk's is Release). Delete `Game/Source/GameApi.hpp`; `arcbuild build … --config Release` → expected `[msbuild] … error C1083: Cannot open include file: 'GameApi.hpp'`, exit 1.

- [ ] **Step 2: Rewrite the module** — `Game/Source/Aphelyon.cpp`:

```cpp
// Aphelyon -- the game client's primary module, built as an EXTERNAL project against
// the Arcane engine SDK and hosted through the ABI-versioned plugin host. Built on
// the SDK's ARCANE_GAME_MODULE (Arcane/Plugin/GameModule.hpp): the exports, the
// TypeContext/Mosaic/ImGui prologue, this module's ComponentModule with the
// ARCANE_COMPONENT drain, and the registry Save/LoadState round-trip are the
// macro's. The ENGINE owns the standard systems (physics -> transform propagation,
// render submission); a gameplay system added under Source/ registers itself in
// OnInit and places itself with Astra::Before/After against the engine's types.
//
// It does NOT build a scene. Scenes are DATA: they come from a .arcscene loaded by the
// host (the project's bootScene, or File > Open Scene in the editor), and this module's
// job is to make its component types and systems exist so that data can be interpreted.
// Code that spawns entities at Init would fight the loaded scene for ownership of the
// registry -- an earlier seed version did exactly that, and it is what this cut removed.
//
// What is left here is what is Aphelyon's: a tiny HUD that visibly proves the external
// module is live in the host.

#include <Arcane/Plugin/GameModule.hpp>

#include <imgui.h>

namespace Aphelyon
{
    struct Module final : Arcane::GameModule
    {
        void OnFixedUpdate(double dt) override { m_time += dt; }

        // Host calls this between ImGuiLayer BeginFrame and Render; the macro
        // skips it in a headless host, so no guard here.
        void OnDrawUI() override
        {
            if (ImGui::Begin("Aphelyon"))
            {
                ImGui::TextUnformatted("Aphelyon client -- hosted via engine-as-SDK.");
                ImGui::Text("scene time: %.1fs", m_time);
            }
            ImGui::End();
        }

        // A hot reload constructs a fresh Module (the macro's Init), so the clock
        // restarts at 0 exactly as the old LoadState reset it.
        double m_time = 0.0;
    };
}

ARCANE_GAME_MODULE(Aphelyon::Module)
```

`Game/Aphelyon.arcproj:6` → `"abi": 29`.

- [ ] **Step 3: Build GREEN through the driver, prove the load** — `arcbuild build … --config Release` → probe row `state=match … -> plain build`, exactly **one compile** (`Aphelyon.cpp`; `TestComponent.cpp` is untouched) + one link, 0 errors. Then `cd D:\dev\starworks\Arcane\bin\Release-windows-x86_64-md\ArcaneRuntime && ./ArcaneRuntime.exe --headless --project D:\dev\starworks\Gacha\Game --frames 30 2>&1 | grep -E "Aphelyon::Module: registered|initial load failed|abi"` → `registered 1 module component type(s)` (the user's `TestComponent`), no failure, no ABI complaint (29 == 29).

- [ ] **Step 4: Commit (Gacha) — the user's untracked files stay out**

```bash
cd /d/dev/starworks/Gacha && git add Game/Source/Aphelyon.cpp Game/Aphelyon.arcproj && git rm -q Game/Source/GameApi.hpp && git commit -q -F - <<'EOF'
refactor(game): Aphelyon on ARCANE_GAME_MODULE -- OnFixedUpdate + the HUD; GameApi.hpp gone; restamp Aphelyon.arcproj to engine ABI 29

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
git status --short   # TestComponent.* and test.arcscene must still be listed, untouched
```

---

### Task 5: The wizard's templates and the SDK comments

**Files:**
- Modify: `ArcaneTests/src/ClassTemplatesTest.cpp` (`:69-90`, the System case; the Component case gains one CHECK_FALSE)
- Modify: `ArcaneEditor/src/Project/ClassTemplates.cpp` (`kComponentSource` `:104-108`; `kSystemHeader` `:110-137`)
- Modify: `ArcaneEditor/src/Project/ClassTemplates.hpp:17-20`
- Modify: `ArcaneClient/src/Arcane/Plugin/GameComponents.hpp:10-13`
- Modify: `build/arcane.lua:90`

- [ ] **Step 1: Write the failing pins** — in `ClassTemplatesTest.cpp`'s System case replace the two lines after the "Systems stay EXPLICIT" comment (`:85-88`) with:

```cpp
    // Systems stay EXPLICIT (their order is a design act): the note carries the
    // exact OnInit line, and the default traits PLACE the system before the
    // engine's TransformPropagationSystem (the gameplay-moves-things case; the
    // note names After<> for the read-world-transforms case). The engine owns
    // the standard systems, so nothing here mentions GamePlugin_Init.
    CHECK(Has(r.header, "#include <Arcane/Scene/TransformSystems.hpp>"));
    CHECK(Has(r.header, "Astra::Before<Arcane::TransformPropagationSystem>"));
    CHECK(Has(r.header, "Astra::After<"));
    CHECK(Has(r.header, "OnInit"));
    CHECK(Has(r.header, "AddSystem<Aphelyon::Movement>()"));
    CHECK_FALSE(Has(r.header, "GamePlugin_Init"));
```

and in the Component case add `CHECK_FALSE(Has(r.source, "GamePlugin_Init"));` + `CHECK(Has(r.source, "ARCANE_GAME_MODULE"));`.

- [ ] **Step 2: Build, run RED** — `./ArcaneTests.exe "*ClassTemplates*"` → the System case FAILS on `TransformSystems.hpp` / `Before<Arcane::TransformPropagationSystem>` / `OnInit`; the Component case on `ARCANE_GAME_MODULE`.

- [ ] **Step 3: The templates** — `kComponentSource`'s comment becomes:

```
// The one registration line: the ARCANE_GAME_MODULE prologue (Arcane/Plugin/
// GameModule.hpp) drains every ARCANE_COMPONENT of the module into its
// ComponentModule (Arcane::Game::RegisterComponents). One .cpp per type.
```

`kSystemHeader` becomes:

```
#pragma once

// {{CLASS}}: a system -- a functor the scheduler runs over the registry each
// step. Declare what it reads and writes in the SystemTraits so the scheduler
// can order and parallelise it.
//
// PLACEMENT. The engine owns its standard systems (Runtime::InstallEngineSystems:
// PhysicsSystem -> TransformPropagationSystem in fixedUpdate, RenderSubmission
// System in render). Say where THIS one runs relative to them in the traits:
// Astra::Before<Arcane::TransformPropagationSystem> (the default below: move
// things, THEN the engine propagates) or Astra::After<...> (read the propagated
// WorldTransform). Astra orders by the type NAME, so naming an engine system
// from a game module is fine; an anchor the host never installed adds no edge.
//
// Systems are registered EXPLICITLY, because their order is a design act.
// Add this line to your module's OnInit (Arcane/Plugin/GameModule.hpp):
//
//     std::ignore = ctx.engine->Schedulers().fixedUpdate.AddSystem<{{NS}}::{{CLASS}}>();
//
// (fixedUpdate for simulation, render for submission-time work.)

#include <Arcane/Scene/TransformSystems.hpp>   // the placement anchor

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

namespace {{NS}}
{
    struct {{CLASS}}
        : Astra::SystemTraits<Astra::Reads<>, Astra::Writes<>,
                              Astra::Before<Arcane::TransformPropagationSystem>>
    {
        void operator()(Astra::Registry& reg)
        {
            (void)reg;
        }
    };
}
```

`ClassTemplates.hpp:17-20` → `//   System     -- a header-only Astra::SystemTraits functor placed Before the engine's TransformPropagationSystem by default, with the paste-ready AddSystem line for the module's OnInit in its comment: systems stay EXPLICIT because their scheduler order is a design act.`

`GameComponents.hpp:10-13` (the `// GamePlugin_Init, ONCE …` example) → `//   // ARCANE_GAME_MODULE(MyGame::Module) -- Arcane/Plugin/GameModule.hpp -- does the rest:` / `//   // opens this module's ComponentModule and drains the registrar into it.`; and `:16` "RegisterComponents drains" stays. `arcane.lua:90` comment → `-- kept for external modules that still use a GAME_API of their own; the SDK's ARCANE_GAME_MODULE needs no define`.

- [ ] **Step 4: Build, run GREEN** — Debug build; `./ArcaneTests.exe "*ClassTemplates*"` all pass; `[editor]` green. Product check of the template: render a System through the wizard path is desk-owed; instead compile-check the rendered text by dropping a `Probe.hpp` rendered from the template into `ReferenceProject/Source/` temporarily, `-t:Rebuild` ReferenceProject (it compiles: the `Before<Arcane::TransformPropagationSystem>` anchor resolves), then delete `Probe.hpp` and rebuild.

- [ ] **Step 5: Commit (Arcane)**

```bash
cd /d/dev/starworks/Arcane && git add ArcaneEditor/src/Project/ClassTemplates.cpp ArcaneEditor/src/Project/ClassTemplates.hpp ArcaneTests/src/ClassTemplatesTest.cpp ArcaneClient/src/Arcane/Plugin/GameComponents.hpp build/arcane.lua && git commit -q -F - <<'EOF'
feat(editor): the System class template places itself Before<TransformPropagationSystem> and names the module's OnInit; SDK comments follow ARCANE_GAME_MODULE (game-module boilerplate Task 5)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
```

---

### Task 6: Closeout — sweep, suites, baselines, gate both configs, docs, memory

- [ ] **Step 1: The sweep the spec promised** — from both repo roots:
`grep -rnE 'GamePlugin_(ABIVersion|Init|Shutdown|FixedUpdate|Update|DrawUI|SaveState|LoadState)\s*\(' --include=*.cpp --include=*.hpp . | grep -vE 'bin/|GameModule\.hpp|Plugin\.cpp|PluginHost\.cpp|PluginABI\.hpp'` → no hand-spelled definitions; `find . -name GameApi.hpp -o -name PluginExport.hpp | grep -v bin/` → nothing; `grep -rn 'GAME_API' --include=*.cpp --include=*.hpp --include=*.lua . | grep -v bin/` → only `arcane.lua`'s define + comment.

- [ ] **Step 2: Rebuild everything both configs** — `ReferenceProject.slnx -t:Rebuild` then `Arcane.slnx`, Debug then Release (LNK1104 on the Release editor exe is reported if the desk holds it).

- [ ] **Step 3: Suites + baselines** — `~[gpu]` Debug + Release from the exe dir with `-r console -r json::out=D:\tmp\at-<cfg>.json` (bash's `/tmp` in a `::out=` argument is NOT path-converted — it lands on `D:\tmp`); `check-baselines.ps1 -ReportPath D:\tmp\at-<cfg>.json -Configuration <cfg> -Invocation '~[gpu]'` → a rise of (+4 `[runtime]` +1 `[hotreload]` cases, assertions measured); book with a layout-preserving text edit (one-line rows) and a `measured` note naming this plan and the seeds; re-run → `+0/+0 exit 0`.

- [ ] **Step 4: Golden gate both configs** — `golden-gate.ps1 -Configuration Debug` and `-Configuration Release` → 4/4 each, `diffCount=0`.

- [ ] **Step 5: Docs + memory** — spec status line → implemented, plan path; append `## Closeout` here (shas, counts, gate lanes, the one-compile proof from Task 4, owed: the wizard desk pass now on the macro; the Astra "dangling ordering constraint" diagnostic follow-up; Hub scaffold if it ever writes a module .cpp). Memory: update `project_arcane_source_ide_surface_arc.md` (GameModule.hpp DONE, sha), `project_arcane_core_shared_dll_direction.md` (step 1 done → the Core-DLL spec is NEXT), `MEMORY.md` lines.

- [ ] **Step 6: Commit (Arcane)** — `scripts/automation-baselines.json`, the spec, this plan:

```bash
cd /d/dev/starworks/Arcane && git add scripts/automation-baselines.json docs/specs/2026-09-13-game-module-boilerplate-design.md docs/plans/2026-09-13-game-module-boilerplate-plan.md && git commit -q -F - <<'EOF'
docs(plugin): close the game-module boilerplate arc -- baselines booked, spec implemented, plan closeout (Task 6)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
```

---

## Self-review

**Spec coverage.** §3.1 class + accessors → T2 header (all seven hooks, five accessors, Debug asserts). §3.2 eight exports, prologue order, failure teardown, Shutdown order, Save/Load bodies + extras, raw pointers, `ARCANE_GAME_MODULE_ABI` → T2. §3.3 ReferenceGame with no overrides → T3. §4.1 the standard three + per-system guards + reinstall → T1 (with tests); §4.2 placement idiom + cross-DLL + dangling anchor + insertion order → T1 tests + T5 template; §4.3 → T1 comment. §5.1 HotReloadPlugin (ARCANE_COMPONENT drain, extras, `_ABI` form, `PluginExport.hpp` gone) → T2; §5.2 → T3; §5.3 Aphelyon (`OnFixedUpdate`, HUD, `m_time`, `GameApi.hpp` gone) → T4; §5.4 template + `ClassTemplatesTest` → T5; §5.5 `arcane.lua` comment → T5; §5.6 `PluginABI.hpp:734` → T1; §5.7 `GameComponents.hpp` → T5. §6 ABI 29 + both restamps → T1/T4. §7 `[hotreload]` on the macro + the hook-order case → T2; `[runtime]` install/reinstall/order → T1; placement (before/after/untraited/dangling) → T1; `[editor]` pins → T5; product (both modules, one compile via arcbuild, gate 4/4 both configs) → T3/T4/T6. §8 order honoured: T1 → T2 (HotReload) → T3 (ReferenceGame) → T4 (Aphelyon) → T5 → T6 with the sweep.

**Placeholders.** None; every code step carries its code; every run names its expected outcome.

**Type consistency.** `GameModule::{OnInit(EngineContext&) -> bool, OnShutdown, OnFixedUpdate(double), OnUpdate(double,double), OnDrawUI, OnSaveState(BinaryWriter&), OnLoadState(BinaryReader&) -> bool, Context, Engine, Registry, Components, SceneRootEntity, BindForMacro_}` — same in T2's header, T2's plugin, T3, T4. `GameModuleDetail::State::{Init<Type>(EngineContext*, const char*), Shutdown, SaveState, LoadState}` used by the macro exactly as declared. `ARCANE_GAME_MODULE_ABI(Type, Abi)` — T2 plugin passes `(Module, kGamePluginABIVersion + (HOTRELOAD_ABI_OFFSET))`. Runtime names (`Schedulers().fixedUpdate/render`, `ClearSystems`, `Loop().Advance(dt, fixedFn, updateFn)`) match `Runtime.hpp`/`RunLoop.hpp` as read.


---

## Closeout (2026-09-13)

**Commits (Arcane, unpushed):** T1 `51730bcf` engine-owned systems + ABI 29 · T2 `4865d4f6` GameModule.hpp + HotReloadPlugin on the macro · T3 `a5d77e30` ReferenceGame · T5 `fac7487a` templates/comments · T6 = this booking commit. **Gacha (unpushed):** T4 `fac7487a` Aphelyon + restamp 29.

**What the modules became:** ReferenceGame 155 → 23 lines (a class with no overrides); Aphelyon 190 → 47 (OnFixedUpdate + the HUD); HotReloadPlugin 123 → 86 (with the probe and the extras). Sweep: the only `GamePlugin_*` definitions in either tree are inside `GameModule.hpp`; no `GameApi.hpp` / `PluginExport.hpp` remain; `GAME_API` survives only as `arcane.lua`'s define.

**Deviation from the plan (R4):** `PluginHost::Unload()` resets the registry, so the OnShutdown probe could not stamp a component; it logs `HotReloadPlugin: OnShutdown with handle open|closed` through `Log::Engine()` (the DLL's one logger) and the test captures it with a `callback_sink_mt` — the `SerializationNegativeTest` shape. Also: three physics-arc `RuntimeTest` cases pinned "the module adds propagation" and were re-pinned to the engine-owned contract in T1.

**Suites (`~[gpu]`, FROM the exe dir, after ReferenceProject.slnx `/t:Rebuild` then Arcane.slnx, both configs, 0 errors):** Debug 56963 / 1709 passed (1713 cases, 4 SKIP), seed 2310081804; Release 56963 / 1709, seed 2475943271. Guard +30/+5 before booking (+4 `[runtime]`, +1 `[hotreload]`), +0/+0 after. Per task: T1 `[runtime]` 115/21 · T2 `[hotreload]` 76/11 · T5 ClassTemplates 62/6, `[editor]` 4277/368.

**Golden gate:** Debug 4/4 at T3 and again at close; Release 4/4 at close; `diffCount=0` every lane — the engine registers the same pair the modules used to, in the same order.

**Product:** Aphelyon via `arcbuild build --config Release` = 1 compile (`Aphelyon.cpp`) + 1 link, probe `match`; the Release `ArcaneRuntime` loads it headless: `Aphelyon::Module: registered 1 module component type(s)` (the user's wizard-made TestComponent), ABI 29 accepted. ReferenceGame likewise (`registered 0`). The rendered System template compile-checked inside ReferenceProject (temporary Probe.hpp/.cpp, removed).

**Owed:** the wizard's desk pass (now on the macro: Create → VS opens the .cpp → Rebuild Game Module = 1 compile + link → Add Component lists it); an Astra "dangling ordering constraint" diagnostic at plan build (a misspelled `Before<>` anchor is silent today); the Hub scaffold, if it ever writes a module .cpp, emits the macro form.
