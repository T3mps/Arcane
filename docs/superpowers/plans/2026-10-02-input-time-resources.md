# Input and Time as Resources Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Game systems read time, gameplay input and physics through registry resources declared as `Arcane::Res<T>` / `Arcane::ResMut<T>` parameters. Game code spells only `Arcane::` names, and ReferenceProject's adapter disappears with its gameplay unchanged bit for bit.

**Architecture:**
- **Resources:** `RunLoop` publishes `Arcane::Time` before every fixed step and before Update. `ClientRuntime` publishes `Arcane::GameInput`, a header-only read view over `LocalInputUser`, every frame and every fixed step.
- **Astra:** parameter-style systems gain ordering traits and are keyed by their own type. This lands in the Astra repo first and is then vendored.
- **Registration and physics:** `ARCANE_SYSTEM` registers both system shapes. The physics commands become member functions of `PhysicsResource`, published to games as `Arcane::Physics2D`.
- **Facade:** an alias facade (`Arcane/EcsFwd.hpp`, `Arcane/Ecs.hpp`, `Arcane/Reflection.hpp`) re-exports every game-facing library name.
- **Proof:** a recorded player trajectory proves the rewrite changed no gameplay.

**Tech Stack:**
- C++23, MSVC (VS 18), premake5 `vs2026`;
- Catch2 (ArcaneTests), GoogleTest (AstraTest);
- Astra (ECS), Manifold2D (2D physics), Mosaic (core), nlohmann json.

**Spec:** `docs/superpowers/specs/2026-10-02-input-time-resources-design.md`. It is approved and committed at `b7a06b70`. Executors read the spec AND this plan.

## Global Constraints

**Repository and git**
- **Where to work:** the worktree `D:\dev\starworks\Arcane-input`, branch `feat/input-time-resources` (off Arcane main `b3908116`). Astra work goes in `D:\dev\starworks\Astra`, branch `dev`.
- **Never push**: not Arcane, not Astra, not Aphelyon.
- **Stage files BY NAME.** Never `git add -A` or `git add .`.
- **Commits:** one per task (Task 2 commits in the Astra repo, Task 3 in Arcane). Conventional messages: `feat(core): ...`, `feat(client): ...`, `feat(astra): ...`, `chore(astra): ...`, `refactor(game): ...`, `test(...)`, `docs(...)`. Every message ends with exactly these two lines:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
  ```
- **Never touch the user's untracked files.**
  - Arcane: `ArcaneAssetPipeline/ArcaneAs.25D4CEF5/`, `ArcaneEditor/ArcaneEditor/`, `out.txt`, `out/`, `docs/research/2026-10-01-linux-gcc-clang-build-inventory.md`.
  - Astra: `bench-compare/` and every other untracked file there.
  - Aphelyon: `Content/`, `Source/Game/TestComponent*`.
- **Never touch** `D:\dev\starworks\Arcane-preview`, the other `D:\dev\starworks\Arcane-*` worktrees, the main checkout `D:\dev\starworks\Arcane` (or its `bin\`), or `D:\dev\starworks\Astra-storage`.

**Building and running tests**
- **Private TEMP:** every test run in this worktree uses `D:\dev\starworks\Arcane-input\.tmp-tests` (already excluded through `info/exclude`). Before each run: `set TEMP=D:\dev\starworks\Arcane-input\.tmp-tests` and `set TMP=%TEMP%` (create it with `mkdir` if missing). Parallel worktrees share fixed `%TEMP%` paths otherwise.
- **Generate:** the projects are generated, not committed. After any change to `premake5.lua` or any added/removed `.cpp`, run from the worktree root (needs `VCPKG_ROOT=D:\dev\_shared\tools\vcpkg`):
  ```bat
  ThirdParty\premake5\premake5.exe vs2026
  ```
- **Build.** These builds run CONCURRENTLY with the node-page tracks, so keep `-m` modest:
  ```bat
  "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" Arcane.slnx -p:Configuration=Debug -p:Platform=x64 -m:4 -nr:false -v:minimal
  ```
  - Release builds the same way.
  - A layout change to a widely-included header gets a full rebuild (`-t:Rebuild`) the first time.
  - Guard on `cl.exe`/`link.exe` processes, not idle MSBuild nodes. An `LNK1104` on an exe means it is running.
- **Running ArcaneTests:** FROM THE EXE DIR (`bin\Debug-windows-x86_64-md\ArcaneTests\`): `.\ArcaneTests.exe "<tags>"`. Record the printed random-order seed.
  - Subagents run suites in the FOREGROUND only.
  - The gate runs are `"~[gpu]~[shell]"` and `"[witness]~[shell]"`. On this branch `[shell]` matches nothing, and the filter is kept for parity with the node-page lanes.
- **Astra tests:**
  ```bat
  msbuild ide\AstraTest.vcxproj -p:Configuration=Debug -p:Platform=x64 -m:4 -nr:false
  bin\Debug-windows-x86_64\AstraTest\AstraTest.exe
  ```
  New Astra tests go into EXISTING test files, so Astra needs no premake regeneration.
- **Vendoring Astra:** `powershell -ExecutionPolicy Bypass -File scripts\sync-astra.ps1 -DryRun`, then without `-DryRun`.
  - Stage by name the headers that really changed, plus `ThirdParty/Astra/VENDORED.txt`.
  - Never stage CRLF-only fan-out: `git diff --ignore-all-space --stat` tells the real changes from the noise.
  - Arcane never edits `ThirdParty/Astra` by hand.

**ABI**
- **This branch NEVER edits `kGamePluginABIVersion`** (`ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:987`, 46 on this base). Spec s9: the bump takes the next free number AT MERGE, after rebasing onto the merged node-page branch.
- **Inside this branch** every module is compiled against the same headers, so the gate's equality check stays consistent at 46.
- **Bump content:** the bump, the `// vNN` history line, and the ReferenceProject + Aphelyon restamps are the **Merge prep** section at the end of this plan. They are NOT a task of this run.

**Goldens, baselines, cvars**
- **Goldens must match UNCHANGED.** No task re-blesses. A golden difference is investigated as a bug in this branch.
- **Baselines:** count rises go into `scripts/automation-baselines.json`, following the pattern of the node-page branch's baseline commits. The `-Invocation "~[gpu]"` key is checked with `scripts/check-baselines.ps1`. This happens once, in the gate task.
- **No new tunables.** This plan adds none. If one becomes necessary, it is an Archive cvar with help text and a range, never a `constexpr`.

**Code rules for this branch**
- **Game-facing code spells `Arcane::` only** (spec s6). Engine internals may keep `Astra::`.
- **Internal fences.** In the 15 game-facing headers, internal regions that must keep library names are fenced with:
  - `// ARCANE_INTERNAL_BEGIN: <one-line reason>`
  - `// ARCANE_INTERNAL_END`

  The guard (Task 17) skips fenced lines, comments, string literals and preprocessor lines (including `#define` continuations).
- **Member functions that hide namespace aliases.** In a class that has a member function named `Registry()` or `GameInput()` (for example `Runtime`, `ClientRuntime`, `GameModule`), spell the type fully qualified as `::Arcane::Registry` or `::Arcane::GameInput`. The member function hides the namespace alias inside the class.

## Review Focus

These are the five input classes the spec implies but no headline test exercises. Each is pinned by a test in the owning task, as listed.

1. **A cached `ActionRef` across a re-Configure.** While a system's `ActionRef` holds a cached Guid, the project's input asset is re-Configured: hot edited, renamed, or switched to another project. The ref must re-resolve on the next query, answer zero (with one warning) while its action is gone, and resolve again when it returns. Pinned in **Task 6**.
2. **Play started while paused, then single-stepped.** Each single step advances `Time::fixedStep` by exactly 1, with `paused == true` and `dt == 0` in the published `Time`. A `Rebind` (Stop) then returns the counter to 0 without unpausing. Pinned in **Task 4**.
3. **A parameter system's resource disappears and returns.** The system is skipped and logs ONCE while the resource is absent, resumes when it comes back, and logs once more on the next disappearance. Pinned in **Task 2** (Astra).
4. **A scene saved by the OLD build is opened and re-saved by the new one.** The stale `"value"` key loads with one warning. The other fields keep their values, and the re-saved file no longer carries `"value"`. Pinned in **Task 13**.
5. **A game component uses `ARCANE_REFLECT_ATTR` with an attribute the facade forgot to alias.** Astra adds an attribute and `Arcane::Attr` misses it, and a game author's component fails to compile. A source scan compares Astra's `Attribute.hpp` attribute list against `Arcane/Reflection.hpp`'s aliases. Pinned in **Task 11**.

---

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp` | Create (T1) | The behaviour gate: drives ReferenceProject's real module through a scripted input run and compares against the recording. |
| `ArcaneTests/data/trajectory/reference_player.json` | Create (T1) | The recorded trajectory, captured BEFORE any change. |
| `premake5.lua` | Modify (T1, T14) | The `ReferenceGameUnderTest` and `TemplateSmokePlugin` test plugins; ArcaneTests `dependson` and post-build copies. |
| `D:\dev\starworks\Astra\include\Astra\System\SystemParam.hpp` | Modify (T2) | `Detail::ParamOrdering`; `FunctionSystemWrapper` exposes `KeyType` and ordering typedefs. |
| `D:\dev\starworks\Astra\include\Astra\System\SystemScheduler.hpp` | Modify (T2) | `Detail::KeyOf`; registration keyed by `KeyOf`; refuses access traits on parameter systems; `Has`/`RemoveSystem` accept `ParamFunctor`. |
| `D:\dev\starworks\Astra\tests\System\SystemParamTest.cpp` | Modify (T2) | Ordering, own-type key, refusal, log-once tests. |
| `ThirdParty/Astra/**`, `ThirdParty/Astra/VENDORED.txt` | Sync (T3) | Vendored Astra. |
| `ArcaneCore/src/Arcane/Sim/Time.hpp` | Create (T4) | `Arcane::Time`. |
| `ArcaneCore/src/Arcane/Sim/RunLoop.hpp` | Modify (T4) | Publishes `Time`, owns `m_fixedStep`/`m_elapsed`. |
| `ArcaneTests/src/RunLoopTest.cpp` | Modify (T4) | `Time` cases. |
| `ArcaneClient/src/Arcane/Input/LocalInputUser.{hpp,cpp}` | Modify (T5) | `Generation()`. |
| `ArcaneClient/src/Arcane/Input/GameInput.hpp` | Create (T6) | `Arcane::ActionRef`, `Arcane::GameInput` (header-only). |
| `ArcaneClient/src/Arcane/Client/ClientRuntime.cpp` | Modify (T6) | Publishes `GameInput`. |
| `ArcaneTests/src/GameInputTest.cpp` | Create (T6) | `GameInput`/`ActionRef` cases. |
| `ArcaneTests/src/ResourceSwapTest.cpp` | Create (T7) | `Time`/`GameInput` survive Play/Stop, scene open and hot reload. |
| `ArcaneCore/src/Arcane/Plugin/GameSystems.hpp` | Modify (T8) | Registers parameter-style systems. |
| `ArcaneTests/src/ParamSystemRegistrationTest.cpp` | Create (T8) | Factory registration of parameter-style systems. |
| `ArcaneCore/src/Arcane/Scene/PhysicsSystem.hpp` | Modify (T9) | `BodyMotion2D`, `PhysicsResource::Motion`/`SetVelocity`, `using Physics2D`. |
| `ArcaneCore/src/Arcane/Scene/Physics2D.cpp` | Create (T9) | The member function bodies (moved from `PhysicsCommands.cpp`). |
| `ArcaneCore/src/Arcane/Scene/PhysicsCommands.{hpp,cpp}` | Modify (T9) → Delete (T12) | Forwarders during T9-T11; gone at T12. |
| `ArcaneTests/src/Physics2DTest.cpp` | Create (T9) | `Physics2D` behaviour. |
| `ArcaneCore/src/Arcane/EcsFwd.hpp`, `ArcaneCore/src/Arcane/Ecs.hpp` | Create (T10) | The ECS facade. |
| `ArcaneCore/src/Arcane/Config/CVarTypes.hpp` + 11 callers | Modify (T10) | `Any(CVarFlags, CVarFlags)` → `HasFlag` (it would collide with the `Arcane::Any` alias). |
| `ArcaneTests/src/EcsFacadeTest.cpp` | Create (T10) | Alias identity checks. |
| `ArcaneCore/src/Arcane/Reflection.hpp` | Create (T11) | `ARCANE_REFLECT_*`, `Arcane::Attr`, `ARCANE_CHANGE_TRACKED`. |
| `ArcaneTests/src/ReflectionFacadeTest.cpp` | Create (T11) | Macro equivalence and the attribute-coverage scan. |
| `ReferenceProject/Source/Game/*` | Modify (T12) | The controller rewrite. |
| `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp`, `SceneSerializer.hpp` | Modify (T13) | The unknown-key warning. |
| `ReferenceProject/Content/scenes/physics.arcscene` | Modify (T13) | Drops `"value"`. |
| `ArcaneTests/src/SceneUnknownFieldTest.cpp` | Create (T13) | Unknown-key load/re-save. |
| `ArcaneEditor/src/Project/ClassTemplates.cpp` | Modify (T14) | Parameter-style system templates, `Arcane::` names. |
| `ArcaneTests/plugins/TemplateSmoke/*` | Create (T14) | Checked-in renders, compiled as `TemplateSmokePlugin.dll`. |
| `ArcaneTests/src/ClassTemplatesTest.cpp` | Modify (T14) | New expectations, render == checked-in, load + run. |
| 10 Plugin/Base/Client headers | Modify (T15) | Header sweep A. |
| 5 Scene headers | Modify (T16) | Header sweep B. |
| `ArcaneTests/src/ArcaneSpellingGuardTest.cpp` | Create (T17) | The guard. |
| `README.md`, `ArcaneCore/src/Arcane/Plugin/GameModule.hpp` | Modify (T18) | Docs. |
| `scripts/automation-baselines.json` | Modify (T19) | Baseline rise. |

---

### Task 1: Record the player trajectory (the behaviour gate) BEFORE any change

**Files:**
- Modify: `premake5.lua`. Add the `ReferenceGameUnderTest` project after the four `test_plugin(...)` calls (~:1860). Add it to ArcaneTests' `dependson` (~:1668) and to its post-build copies (~:1711-1714).
- Create: `ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp`
- Create: `ArcaneTests/data/trajectory/reference_player.json` (recorded, not hand-written)

**Interfaces:**
- Consumes (all existing):
  - `Arcane::ClientRuntime(ProcessContext&)`, `ClientRuntime::{SetInputSnapshot, UpdateGameInput, BeginGameInputFixedStep, EnsurePhysics, Loop, ResetRegistry, Core}`;
  - `Arcane::HostBoot::LoadGameplayInput(ClientRuntime&, const Project&)`, `Arcane::Project::Open(path)`;
  - `Arcane::PluginHost(ProcessContext&, path)`, `AttachRuntime`, `Load`, `FixedUpdateAll`, `UpdateAll`, `Unload`;
  - `Arcane::Scene::ReadSceneFile(path, std::string*)`, `Arcane::Scene::ApplySceneDocument(doc, reg)`;
  - `Arcane::Test::FindReferenceProjectDir()`, `Arcane::Test::Process()`.
- Produces:
  - **The `[trajectory]` test.** Every later task must keep it green UNCHANGED.
  - **The test plugin `ReferenceGameUnderTest.dll`.** It compiles `ReferenceProject/Source/Game/**` verbatim, so after Task 12 it compiles the rewritten sources and the test proves equivalence.

- [ ] **Step 1: Add the test plugin to premake.** Put it directly after `test_plugin("HotReloadPluginInitFail", ...)`, still inside `group "Tests"`:

```lua
-- ============================================================================
-- ReferenceGameUnderTest: ReferenceProject's REAL game-module sources
-- (ReferenceProject/Source/Game/**), compiled as a test plugin with the
-- include surface build/arcane.lua gives a game module. The [trajectory]
-- case (ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp) loads it through a
-- real PluginHost and replays a scripted input run. Because it compiles the
-- sources IN PLACE, the same unchanged test proves a rewrite of those
-- sources kept the gameplay bit-identical (input-seam spec s8 T6).
-- ============================================================================
project "ReferenceGameUnderTest"
    location "ArcaneTests/plugins"
    kind "SharedLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetname "ReferenceGameUnderTest"
    targetdir ("bin/" .. outputdir .. "/ReferenceGameUnderTest")
    objdir ("bin-int/" .. outputdir .. "/ReferenceGameUnderTest")
    files {
        "%{wks.location}/ReferenceProject/Source/Game/**.cpp",
        "%{wks.location}/ReferenceProject/Source/Game/**.hpp",
    }
    includedirs {
        "%{wks.location}/ReferenceProject/Source/Game",
        "%{wks.location}/ArcaneClient/src",
        "%{IncludeDir.ArcaneCore}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.Mosaic}",
        "%{IncludeDir.nlohmann}",
    }
    links { "ArcaneCore", "ArcaneClient" }
    defines {
        "IMGUI_API=__declspec(dllimport)",
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }
    filter "system:windows"
        systemversion "latest"
        -- The flags build/arcane.lua gives a real module (AVX2 included: inline
        -- header codegen shared across the DLL boundary must agree).
        buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
        fatalwarnings { "4715" }
    filter "configurations:Debug"   defines { "ARCANE_DEBUG" }             runtime "Debug"   symbols "on"
    filter "configurations:Release" defines { "ARCANE_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"    defines { "ARCANE_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}
```

In the `ArcaneTests` project:
- Extend `dependson { "HotReloadPluginV1", ... "arccook" }` with `"ReferenceGameUnderTest"`.
- Append to `postbuildcommands`, after the `HotReloadPluginInitFail.dll` copy:

```lua
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ReferenceGameUnderTest/ReferenceGameUnderTest.dll" "%{cfg.buildtarget.directory}/ReferenceGameUnderTest.dll"',
```

- [ ] **Step 2: Write the test.** Create `ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp`:

```cpp
// THE BEHAVIOUR GATE of the input-seam work (spec 2026-10-02 s8 T6).
//
// ReferenceProject's REAL module sources (built as ReferenceGameUnderTest.dll,
// premake5.lua) run headless the way ArcaneRuntime's frame does
// (ArcaneRuntime/src/RuntimeFrame.cpp: SetInputSnapshot/UpdateGameInput, then
// EnsurePhysics, then Loop().Advance with BeginGameInputFixedStep +
// FixedUpdateAll), against the authored physics scene, under a fixed scripted
// input. The player's position and velocity at every frame must equal the
// recording bit for bit. The recording was captured BEFORE the rewrite of
// ReferenceGame.cpp / PlayerController2D* (input-seam plan Task 1), so a
// green run after it is the proof that moving input and time into resources
// changed no gameplay.
//
// To re-record (only ever on purpose, and never in the same commit as a
// gameplay change): set ARCANE_RECORD_TRAJECTORY=1 and run "[trajectory]".

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Json.hpp>

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Helpers/ReferenceProjectDir.hpp"
#include "Helpers/TestTypeContext.hpp"

namespace
{
    constexpr std::uint32_t kScancodeA = 4;    // SDL_SCANCODE_A -- Player.Move negative
    constexpr std::uint32_t kScancodeD = 7;    // SDL_SCANCODE_D -- Player.Move positive
    constexpr std::uint32_t kScancodeW = 26;   // SDL_SCANCODE_W -- Player.Jump
    constexpr int    kFrames = 240;
    constexpr double kDt     = 1.0 / 60.0;

    struct Sample { float x, y, vx, vy; };

    // Settle, run right, a held jump, turn left, a SHORT hop (released early,
    // exercising the jump cut), keep running left, then brake to a stop.
    Arcane::InputSnapshot ScriptedInput(int f)
    {
        Arcane::InputSnapshot s;
        if (f >= 30 && f < 90)   s.SetScancode(kScancodeD);
        if (f >= 60 && f < 72)   s.SetScancode(kScancodeW);
        if (f >= 90 && f < 150)  s.SetScancode(kScancodeA);
        if (f >= 100 && f < 103) s.SetScancode(kScancodeW);
        return s;
    }

    std::filesystem::path FixturePath()
    {
        return Arcane::Test::FindReferenceProjectDir().parent_path() /
               "ArcaneTests" / "data" / "trajectory" / "reference_player.json";
    }

    Astra::Entity FindByName(Astra::Registry& reg, const std::string& name)
    {
        Astra::Entity found = Astra::Entity::Invalid();
        reg.CreateView<Arcane::Identity>().ForEach([&](Astra::Entity e, Arcane::Identity& id)
        {
            if (id.name == name) found = e;
        });
        return found;
    }

    std::vector<Sample> RunScript()
    {
        Arcane::ClientRuntime client(Arcane::Test::Process());
        const std::filesystem::path projectDir = Arcane::Test::FindReferenceProjectDir();
        REQUIRE_FALSE(projectDir.empty());
        auto project = Arcane::Project::Open(projectDir);
        REQUIRE(project);
        REQUIRE(Arcane::HostBoot::LoadGameplayInput(client, *project).status ==
                Arcane::HostBoot::GameplayInputLoadResult::Status::Loaded);

        Arcane::PluginHost host(Arcane::Test::Process(),
                                std::filesystem::path("ReferenceGameUnderTest.dll"));
        REQUIRE(host.AttachRuntime(client.Core()));
        REQUIRE(host.Load());

        std::string error;
        const auto scene = Arcane::Scene::ReadSceneFile(
            projectDir / "Content" / "scenes" / "physics.arcscene", &error);
        INFO(error);
        REQUIRE(scene);
        client.ResetRegistry();
        REQUIRE(Arcane::Scene::ApplySceneDocument(*scene, client.Registry()));

        const Astra::Entity player = FindByName(client.Registry(), "Pill");
        REQUIRE(player != Astra::Entity::Invalid());

        std::vector<Sample> out;
        out.reserve(kFrames);
        for (int f = 0; f < kFrames; ++f)
        {
            const Arcane::InputSnapshot snap = ScriptedInput(f);
            client.SetInputSnapshot(snap);
            client.UpdateGameInput(kDt, snap);
            client.EnsurePhysics();
            client.Loop().Advance(kDt,
                [&](double dt)           { client.BeginGameInputFixedStep(); host.FixedUpdateAll(dt); },
                [&](double dt, double a) { host.UpdateAll(dt, a); });

            const auto* t  = client.Registry().GetComponent<Arcane::Transform>(player);
            const auto* rb = client.Registry().GetComponent<Arcane::RigidBody2D>(player);
            REQUIRE(t);
            REQUIRE(rb);
            out.push_back({ t->position.x, t->position.y, rb->velocity.x, rb->velocity.y });
        }
        host.Unload();
        return out;
    }

    bool SameBits(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
}

TEST_CASE("ReferenceProject player: the scripted trajectory matches the recording bit for bit",
          "[trajectory][reference]")
{
    const std::vector<Sample> first = RunScript();
    // Self-consistency first: a run that does not reproduce ITSELF cannot be
    // compared against anything (a nondeterministic step would show up here,
    // not as a phantom "behaviour change" against the fixture).
    const std::vector<Sample> second = RunScript();
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i)
    {
        INFO("self-consistency, frame " << i);
        REQUIRE(SameBits(first[i].x, second[i].x));
        REQUIRE(SameBits(first[i].y, second[i].y));
        REQUIRE(SameBits(first[i].vx, second[i].vx));
        REQUIRE(SameBits(first[i].vy, second[i].vy));
    }

    if (const char* rec = std::getenv("ARCANE_RECORD_TRAJECTORY"); rec && std::string(rec) == "1")
    {
        nlohmann::json j;
        j["frames"] = kFrames;
        j["dt"] = kDt;
        j["entity"] = "Pill";
        j["samples"] = nlohmann::json::array();
        for (const Sample& s : first)
            j["samples"].push_back({ static_cast<double>(s.x), static_cast<double>(s.y),
                                     static_cast<double>(s.vx), static_cast<double>(s.vy) });
        std::filesystem::create_directories(FixturePath().parent_path());
        std::ofstream(FixturePath()) << j.dump(1) << '\n';
        WARN("recorded " << first.size() << " samples to " << FixturePath().generic_string());
        return;
    }

    std::ifstream in(FixturePath());
    REQUIRE(in);
    const nlohmann::json j = nlohmann::json::parse(in);
    REQUIRE(j.at("frames").get<int>() == kFrames);
    const auto& samples = j.at("samples");
    REQUIRE(samples.size() == first.size());
    for (std::size_t i = 0; i < first.size(); ++i)
    {
        INFO("frame " << i);
        CHECK(SameBits(static_cast<float>(samples[i][0].get<double>()), first[i].x));
        CHECK(SameBits(static_cast<float>(samples[i][1].get<double>()), first[i].y));
        CHECK(SameBits(static_cast<float>(samples[i][2].get<double>()), first[i].vx));
        CHECK(SameBits(static_cast<float>(samples[i][3].get<double>()), first[i].vy));
    }
}
```

- [ ] **Step 3: Generate and build Debug** (Global Constraints commands). Expected: `ReferenceGameUnderTest.dll` sits beside `ArcaneTests.exe`.

- [ ] **Step 4: Run it before recording.** It must fail ONLY on the missing fixture.
  - Run: `.\ArcaneTests.exe "[trajectory]"` from the exe dir.
  - Expected: FAIL at `REQUIRE(in)`. The self-consistency loop must PASS.
  - **If self-consistency fails, STOP and report BLOCKED.** The step is nondeterministic, and no bit-exact gate is possible until that is understood. Do not record.

- [ ] **Step 5: Record.**
  - Run: `set ARCANE_RECORD_TRAJECTORY=1` then `.\ArcaneTests.exe "[trajectory]"`, then `set ARCANE_RECORD_TRAJECTORY=`.
  - Expected: a WARN "recorded 240 samples".
  - Sanity-read the file. The x column must rise during frames 30-90 and fall during 90-150. The y column must show two jumps, and the second must be lower (the short hop).
  - **If the player never moves, STOP:** the module is not reading input (check the Player.arcinput load). Never commit a flat recording.

- [ ] **Step 6: Run again without the env var.**
  - Run: `.\ArcaneTests.exe "[trajectory]"`. Expected: PASS.
  - Then build and run Release the same way. Expected: PASS against the same fixture. If Release differs from Debug, stop and report: the gate would have to be per-configuration, which is the user's call.

- [ ] **Step 7: Commit.**

```bash
git add premake5.lua ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp ArcaneTests/data/trajectory/reference_player.json
git commit -m "test(reference): record ReferenceProject's player trajectory under scripted input -- the bit-identical behaviour gate for the input-seam rewrite (ReferenceGameUnderTest.dll compiles the real module sources)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 2: Astra — parameter systems take ordering traits, are keyed by their own type, refuse access traits; log-once pinned

**Repo:** `D:\dev\starworks\Astra`, branch `dev`. Check `git status` first: only the user's untracked files may be present. Do not stage them.

**Files:**
- Modify: `include/Astra/System/SystemParam.hpp`:
  - add `Detail::ParamOrdering` in the `Detail` namespace before `IsParamFunctor` (~:125);
  - change `FunctionSystemWrapper` (~:262).
- Modify: `include/Astra/System/SystemScheduler.hpp`:
  - `Detail::KeyOf` next to `SystemKey` (~:64);
  - the param `AddSystem` (~:246);
  - `RemoveSystem<T>`/`HasSystem<T>` constraints (~:290, ~:345);
  - `RegisterSystemImpl` key (~:1226).
- Test: `tests/System/SystemParamTest.cpp` (append).

**Interfaces:**
- Produces (used by Arcane Tasks 8 and 12):
  - `Astra::Detail::ParamOrdering<Fn>` with `BeforeTypes`, `AfterTypes`, `AmbiguousWithTypes` and `HasAccessTraits`;
  - `FunctionSystemWrapper<Fn, ...>::KeyType == Fn`;
  - a named parameter functor registered with `AddSystem(Fn{})` is found by `HasSystem<Fn>()`, removed by `RemoveSystem<Fn>()`, and is a valid target of other systems' `Before<Fn>` / `After<Fn>`.

- [ ] **Step 1: Write the failing tests.** Append to `tests/System/SystemParamTest.cpp`:

```cpp
// ---- Arcane input-seam spec 2026-10-02 s5.1: ordering on param systems, -------
// ---- own-type keys, access traits refused, absent-resource log once. ----------

#include "../Support/DiagnosticsTestGuards.hpp"
#include <string>
#include <vector>

namespace
{
    std::vector<char> g_paramOrder;

    struct TypedB { void operator()(Astra::Registry&) { g_paramOrder.push_back('B'); } };

    // Ordering-only traits on a param functor: its access comes from its params.
    struct ParamA : Astra::SystemTraits<Astra::Before<TypedB>>
    {
        void operator()(Astra::Res<Health>) { g_paramOrder.push_back('A'); }
    };
    struct ParamC : Astra::SystemTraits<Astra::After<TypedB>>
    {
        void operator()(Astra::Res<Health>) { g_paramOrder.push_back('C'); }
    };
    // A typed system ordered against a NAMED param system.
    struct TypedD : Astra::SystemTraits<Astra::After<ParamA>>
    {
        void operator()(Astra::Registry&) { g_paramOrder.push_back('D'); }
    };
    // Access declared twice (traits + params): refused at compile time.
    struct BadParam : Astra::SystemTraits<Astra::Reads<Position>>
    {
        void operator()(Astra::Res<Health>) {}
    };
    struct BadExclusive : Astra::SystemTraits<Astra::Exclusive>
    {
        void operator()(Astra::Res<Health>) {}
    };

    template<typename Fn>
    concept CanAddParamSystem = requires(Astra::SystemScheduler& s, Fn fn) { s.AddSystem(std::move(fn)); };

    struct LogCount { int errors = 0; };
    void CountingSink(const Astra::LogRecord& r, void* user) noexcept
    {
        if (r.level == Astra::LogLevel::Error) static_cast<LogCount*>(user)->errors++;
    }
}

static_assert(CanAddParamSystem<ParamA>);
static_assert(!CanAddParamSystem<BadParam>);
static_assert(!CanAddParamSystem<BadExclusive>);
static_assert(std::is_same_v<Astra::Detail::ParamOrdering<ParamA>::BeforeTypes, std::tuple<TypedB>>);
static_assert(!Astra::Detail::ParamOrdering<ParamA>::HasAccessTraits);
static_assert(Astra::Detail::ParamOrdering<BadParam>::HasAccessTraits);

TEST(SystemParam, NamedParamSystemHonoursBeforeAndAfterAgainstATypedSystem)
{
    Astra::Registry reg;
    reg.SetResource(Health{1, 1});
    g_paramOrder.clear();

    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<TypedB>().IsOk());   // registered FIRST
    ASSERT_TRUE(s.AddSystem(ParamC{}).IsOk());   // After<TypedB>
    ASSERT_TRUE(s.AddSystem(ParamA{}).IsOk());   // Before<TypedB>, registered LAST

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_EQ(std::string(g_paramOrder.begin(), g_paramOrder.end()), "ABC");
}

TEST(SystemParam, NamedParamSystemIsKeyedByItsOwnType)
{
    Astra::Registry reg;
    reg.SetResource(Health{1, 1});
    g_paramOrder.clear();

    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem<TypedD>().IsOk());   // After<ParamA>, registered first
    ASSERT_TRUE(s.AddSystem(ParamA{}).IsOk());
    EXPECT_TRUE(s.HasSystem<ParamA>());
    EXPECT_FALSE(s.AddSystem(ParamA{}).IsOk());  // AlreadyRegistered under the same key

    Astra::SequentialExecutor exec;
    s.Execute(reg, &exec);
    EXPECT_EQ(std::string(g_paramOrder.begin(), g_paramOrder.end()), "AD");

    s.RemoveSystem<ParamA>();
    EXPECT_FALSE(s.HasSystem<ParamA>());
}

TEST(SystemParam, AbsentResourceLogsOncePerDisappearance)
{
    LogCount count;
    Astra::Testing::ScopedLogSink guard(&CountingSink, &count);

    Astra::Registry reg;
    int ran = 0;
    Astra::SystemScheduler s;
    ASSERT_TRUE(s.AddSystem([&](Astra::Res<Health>) { ++ran; }).IsOk());
    Astra::SequentialExecutor exec;

    for (int i = 0; i < 3; ++i) s.Execute(reg, &exec);   // absent x3
    EXPECT_EQ(ran, 0);
    EXPECT_EQ(count.errors, 1);                            // ONE log, not three

    reg.SetResource(Health{1, 1});
    s.Execute(reg, &exec);
    EXPECT_EQ(ran, 1);                                     // resumes
    EXPECT_EQ(count.errors, 1);

    reg.RemoveResource<Health>();
    for (int i = 0; i < 3; ++i) s.Execute(reg, &exec);   // gone again
    EXPECT_EQ(ran, 1);
    EXPECT_EQ(count.errors, 2);                            // one more log for the new disappearance
}
```

- [ ] **Step 2: Build and run. They must fail to compile.**
  - Run: `msbuild ide\AstraTest.vcxproj -p:Configuration=Debug -p:Platform=x64 -m:4 -nr:false`.
  - Expected: compile errors, because `Astra::Detail::ParamOrdering` is not a member, and `CanAddParamSystem<BadParam>` is satisfied, so the `static_assert` fails.

- [ ] **Step 3: Implement `ParamOrdering` and the wrapper typedefs.** In `SystemParam.hpp`, inside `namespace Detail` and before `IsParamFunctor`:

```cpp
        // ---- Ordering on a param functor (Arcane input-seam spec s5.1) ------
        // A param functor may derive SystemTraits<...> carrying ONLY ordering
        // (Before/After/AmbiguousWith). Its ACCESS comes from its parameters;
        // traits that also declare access (Reads/Writes/ReadsResources/
        // WritesResources/Exclusive) would be a second, disagreeing source, so
        // HasAccessTraits lets the scheduler refuse them (SystemScheduler::
        // AddSystem). Functors without SystemTraits (lambdas, function
        // pointers) get empty ordering.
        template<typename Fn, typename = void>
        struct ParamOrdering
        {
            using BeforeTypes        = std::tuple<>;
            using AfterTypes         = std::tuple<>;
            using AmbiguousWithTypes = std::tuple<>;
            static constexpr bool HasAccessTraits = false;
        };
        template<typename Fn>
        struct ParamOrdering<Fn, std::void_t<typename Fn::BeforeTypes, typename Fn::AfterTypes,
                                             typename Fn::AmbiguousWithTypes,
                                             typename Fn::ReadsComponents, typename Fn::WritesComponents,
                                             typename Fn::ReadsResourceTypes, typename Fn::WritesResourceTypes>>
        {
            using BeforeTypes        = typename Fn::BeforeTypes;
            using AfterTypes         = typename Fn::AfterTypes;
            using AmbiguousWithTypes = typename Fn::AmbiguousWithTypes;
            static constexpr bool HasAccessTraits =
                std::tuple_size_v<typename Fn::ReadsComponents>     != 0 ||
                std::tuple_size_v<typename Fn::WritesComponents>    != 0 ||
                std::tuple_size_v<typename Fn::ReadsResourceTypes>  != 0 ||
                std::tuple_size_v<typename Fn::WritesResourceTypes> != 0 ||
                Fn::RequiresExclusive;
        };
```

Then replace the `FunctionSystemWrapper` class head:

```cpp
    template<typename Fn, typename... Params>
    class FunctionSystemWrapper : public SystemParamBinder<Params...>
    {
        Fn m_fn;
    public:
        // Keyed by the FUNCTOR, not the wrapper (input-seam spec s5.1): a named
        // param system is then HasSystem<Fn>/RemoveSystem<Fn>-addressable and a
        // valid Before<Fn>/After<Fn> target, exactly like a typed system. A
        // lambda's closure type still keys by its per-image anchor (SystemKey).
        using KeyType            = Fn;
        using BeforeTypes        = typename Detail::ParamOrdering<Fn>::BeforeTypes;
        using AfterTypes         = typename Detail::ParamOrdering<Fn>::AfterTypes;
        using AmbiguousWithTypes = typename Detail::ParamOrdering<Fn>::AmbiguousWithTypes;

        explicit FunctionSystemWrapper(Fn fn) : m_fn(std::move(fn)) {}
        void operator()(SystemContext& ctx)
        {
            this->Run(ctx, [this](auto&&... p) { m_fn(static_cast<decltype(p)>(p)...); });
        }
    };
```

- [ ] **Step 4: Implement the scheduler side.** In `SystemScheduler.hpp`:

  (a) In `namespace Detail`, directly after `SystemKey`:

```cpp
        // The type a system is KEYED by: T::KeyType when T declares one (the
        // param FunctionSystemWrapper names its functor), else T itself.
        template<typename T, typename = void> struct KeyOf { using type = T; };
        template<typename T> struct KeyOf<T, std::void_t<typename T::KeyType>> { using type = typename T::KeyType; };
```

  (b) In `RegisterSystemImpl`, replace `const uint64_t typeId = Detail::SystemKey<SystemType>();` with:

```cpp
            const uint64_t typeId = Detail::SystemKey<typename Detail::KeyOf<SystemType>::type>();
```

  (c) Replace the param `AddSystem(Fn&&)` overload (~:246-251) with the accepted overload plus a refused one:

```cpp
        // Param-function system: a lambda/functor whose params are
        // View<...>&/Res<T>/ResMut<T>/Commands. Access is derived from the
        // params (design §5); a named functor may add ORDERING through
        // SystemTraits<Before/After/AmbiguousWith> (input-seam spec s5.1).
        template<typename Fn>
        requires (ParamFunctor<Fn> && !Detail::ParamOrdering<std::decay_t<Fn>>::HasAccessTraits)
        ASTRA_NODISCARD Result<void, SystemError> AddSystem(Fn&& fn)
        {
            return AddParamSystemImpl(std::forward<Fn>(fn), &std::decay_t<Fn>::operator());
        }

        // Refused: a param functor whose SystemTraits ALSO declare access
        // (Reads/Writes/ReadsResources/WritesResources/Exclusive). Its params
        // already state access; two sources would disagree. Use ordering-only
        // traits, or make it a typed operator()(Registry&) system.
        template<typename Fn>
        requires (ParamFunctor<Fn> && Detail::ParamOrdering<std::decay_t<Fn>>::HasAccessTraits)
        Result<void, SystemError> AddSystem(Fn&& fn) = delete;
```

  (d) Relax both `requires (System<T> || ContextSystem<T>)` clauses (on `RemoveSystem<T>()` and `HasSystem<T>()`) to `requires (System<T> || ContextSystem<T> || ParamFunctor<T>)`, and add to their comments: "a named param functor is keyed by its own type (KeyOf)".

- [ ] **Step 5: Build and run the whole Astra suite.**
  - Run: the msbuild line, then `bin\Debug-windows-x86_64\AstraTest\AstraTest.exe`.
  - Expected: all PASS, including the existing `SiblingSameSignatureLambdasAreDistinctSystems` and `TwoSameSignatureFreeFunctionsBothRegisterAndRun`.
  - **If an existing test keyed a param lambda by its WRAPPER type** (for example `HasSystem<FunctionSystemWrapper<...>>`), change it to key by the closure type, and say so in the report.

- [ ] **Step 6: Build and run Release.** Run `-p:Configuration=Release` and `bin\Release-windows-x86_64\AstraTest\AstraTest.exe`. Expected: PASS.

- [ ] **Step 7: Commit in the Astra repo.**

```bash
git -C D:/dev/starworks/Astra add include/Astra/System/SystemParam.hpp include/Astra/System/SystemScheduler.hpp tests/System/SystemParamTest.cpp
git -C D:/dev/starworks/Astra commit -m "feat(system): param systems take ordering traits (Before/After/AmbiguousWith via SystemTraits), are keyed by their own functor type (HasSystem/RemoveSystem/Before<Fn> work), and refuse access traits; absent-resource skip pinned to log once per disappearance" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 3: Vendor Astra into Arcane-input

**Files:** `ThirdParty/Astra/include/**` (changed headers only), `ThirdParty/Astra/VENDORED.txt`.

**Interfaces:**
- Consumes: Astra `dev` HEAD from Task 2.
- **This also brings Astra's 2026-10-01 commits (`b8291b9..975cdb7`).** These are slot retirement, recycling `Clear`, ordered children, `SetParent` returning `bool`, and `GetInstanceId`. The node-page branch vendored the same commits (`31fd76d8`) with no Arcane source change, so the merge resolves to the later stamp.

- [ ] **Step 1:** Run `powershell -ExecutionPolicy Bypass -File scripts\sync-astra.ps1 -DryRun`. Expected: the list includes `System/SystemParam.hpp`, `System/SystemScheduler.hpp`, plus the 2026-10-01 entity/registry headers.
- [ ] **Step 2:** Run it without `-DryRun`. Run `git diff --ignore-all-space --stat ThirdParty/Astra` and keep only the files with real changes.
- [ ] **Step 3:** Run `ThirdParty\premake5\premake5.exe vs2026`, then a full Debug build (`-t:Rebuild` the first time: Registry layout moved).
- [ ] **Step 4:** Run `.\ArcaneTests.exe "~[gpu]~[shell]"` and `.\ArcaneTests.exe "[trajectory]"`. Expected: PASS, with seeds recorded.
  - A `[[nodiscard]]` warning-as-error on an ignored `SetParent` result is fixed at the call site with `std::ignore =` or a real check. Mirror the node-page branch's choice where it made one (`git -C D:/dev/starworks/Arcane show 31fd76d8 --stat`).
- [ ] **Step 5: Commit.**

```bash
git add ThirdParty/Astra/VENDORED.txt <each changed header by name>
git commit -m "chore(astra): vendor Astra dev (<sha>) -- param systems with ordering traits and own-type keys (input-seam spec s5.1), plus the 2026-10-01 entity/relationship batch already vendored on the node-page branch" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 4: `Arcane::Time`, published by `RunLoop`

**Files:**
- Create: `ArcaneCore/src/Arcane/Sim/Time.hpp`
- Modify: `ArcaneCore/src/Arcane/Sim/RunLoop.hpp`:
  - both `Advance` overloads (:93-117);
  - `Rebind` (:138-145);
  - `StepFixed` (:159-190);
  - the members (:192-204).
- Test: `ArcaneTests/src/RunLoopTest.cpp` (append).

**Interfaces:**
- Produces: `struct Arcane::Time { double realDt, dt, fixedDt, alpha, elapsed; std::uint64_t fixedStep; double timeScale; bool paused, inFixedStep; }`, a registry resource republished by every `Advance`.
- **Decision (spec s3 says "elapsed = fixedStep * fixedDt"):** `elapsed` ACCUMULATES `fixedDt` once per step. That equals `fixedStep * fixedDt` while `SetFixedHz` is unchanged, and it stays monotonic if a host changes the rate mid-run. The test pins equality at constant Hz.
- **Decision (alpha inside a fixed step):** during a fixed step, `Time::alpha` is the PREVIOUS frame's alpha. The fixed phase recomputes it at its end, and the Update publish carries the new one.
- The spec's `SingleStep()` is spelled `RequestSingleStep()` in code (`RunLoop.hpp:72`).

- [ ] **Step 1: Write the failing tests.** Append to `RunLoopTest.cpp`:

```cpp
// ---- Arcane::Time (input-seam spec s3) --------------------------------------
#include <Arcane/Sim/Time.hpp>

namespace
{
    // Records the Time each fixed step saw.
    struct RecordFixedTime
    {
        std::vector<Arcane::Time>* seen;
        explicit RecordFixedTime(std::vector<Arcane::Time>* s) : seen(s) {}
        void operator()(Astra::Registry& r) const { seen->push_back(*r.GetResource<Arcane::Time>()); }
    };
    struct RecordUpdateTime
    {
        std::vector<Arcane::Time>* seen;
        explicit RecordUpdateTime(std::vector<Arcane::Time>* s) : seen(s) {}
        void operator()(Astra::Registry& r) const { seen->push_back(*r.GetResource<Arcane::Time>()); }
    };
}

TEST_CASE("RunLoop publishes Time before every fixed step and before Update", "[sim][runloop][time]")
{
    Astra::Registry reg;
    std::vector<Arcane::Time> fixedSeen, updateSeen;
    Arcane::SystemSchedulers sch(nullptr);
    REQUIRE(sch.fixedUpdate.AddSystem<RecordFixedTime>(&fixedSeen).IsOk());
    REQUIRE(sch.update.AddSystem<RecordUpdateTime>(&updateSeen).IsOk());
    Arcane::RunLoop loop(reg, sch);

    for (int i = 0; i < 30; ++i) loop.Advance(1.0 / 60.0);

    REQUIRE_FALSE(fixedSeen.empty());
    REQUIRE(updateSeen.size() == 30);
    for (std::size_t i = 0; i < fixedSeen.size(); ++i)
    {
        CHECK(fixedSeen[i].fixedStep == i + 1);                       // one per step, from 1
        CHECK(fixedSeen[i].inFixedStep);
        CHECK(fixedSeen[i].fixedDt == 1.0 / 60.0);
        CHECK(fixedSeen[i].elapsed == static_cast<double>(i + 1) * (1.0 / 60.0));
    }
    const Arcane::Time& last = updateSeen.back();
    CHECK_FALSE(last.inFixedStep);
    CHECK(last.fixedStep == fixedSeen.size());
    CHECK(last.realDt == 1.0 / 60.0);
    CHECK(last.dt == 1.0 / 60.0);
    CHECK(last.alpha == loop.Alpha());
    CHECK_FALSE(last.paused);
    CHECK(last.timeScale == 1.0);
}

TEST_CASE("Time: the plugin-callback Advance publishes too, before the plugin's fixed hook", "[sim][runloop][time]")
{
    Astra::Registry reg;
    Arcane::SystemSchedulers sch(nullptr);
    Arcane::RunLoop loop(reg, sch);
    std::vector<std::uint64_t> pluginSaw;
    for (int i = 0; i < 10; ++i)
        loop.Advance(1.0 / 60.0,
            [&](double){ pluginSaw.push_back(reg.GetResource<Arcane::Time>()->fixedStep); },
            [&](double, double){ CHECK_FALSE(reg.GetResource<Arcane::Time>()->inFixedStep); });
    REQUIRE_FALSE(pluginSaw.empty());
    for (std::size_t i = 0; i < pluginSaw.size(); ++i) CHECK(pluginSaw[i] == i + 1);
}

TEST_CASE("Time: time scale shows in dt, the fixed step stays canonical", "[sim][runloop][time]")
{
    Astra::Registry reg;
    Arcane::SystemSchedulers sch(nullptr);
    Arcane::RunLoop loop(reg, sch);
    loop.SetTimeScale(0.5);
    loop.Advance(1.0 / 60.0);
    const Arcane::Time* t = reg.GetResource<Arcane::Time>();
    REQUIRE(t);
    CHECK(t->dt == 0.5 / 60.0);
    CHECK(t->realDt == 1.0 / 60.0);
    CHECK(t->fixedDt == 1.0 / 60.0);
    CHECK(t->timeScale == 0.5);
}

// Review Focus #2: Play started while paused, then single-stepped; Stop (Rebind).
TEST_CASE("Time while paused: no steps, dt 0; each single step adds exactly 1; Rebind resets the clock but not the pause",
          "[sim][runloop][time]")
{
    Astra::Registry reg;
    Arcane::SystemSchedulers sch(nullptr);
    Arcane::RunLoop loop(reg, sch);
    loop.SetPaused(true);

    for (int i = 0; i < 5; ++i) loop.Advance(1.0 / 60.0);
    const Arcane::Time* t = reg.GetResource<Arcane::Time>();
    REQUIRE(t);
    CHECK(t->fixedStep == 0);
    CHECK(t->paused);
    CHECK(t->dt == 0.0);
    CHECK(t->realDt == 1.0 / 60.0);

    for (int s = 1; s <= 3; ++s)
    {
        loop.RequestSingleStep();
        loop.Advance(1.0 / 60.0);
        t = reg.GetResource<Arcane::Time>();
        CHECK(t->fixedStep == static_cast<std::uint64_t>(s));
        CHECK(t->paused);
        CHECK(t->dt == 0.0);
    }

    Astra::Registry swapped;
    loop.Rebind(swapped);
    CHECK(loop.IsPaused());                                  // host mode survives (RunLoop.hpp:138)
    loop.RequestSingleStep();
    loop.Advance(1.0 / 60.0);
    const Arcane::Time* t2 = swapped.GetResource<Arcane::Time>();
    REQUIRE(t2);
    CHECK(t2->fixedStep == 1);                               // counter restarted for the new registry
    CHECK(t2->elapsed == 1.0 / 60.0);
}
```

- [ ] **Step 2: Build and run.** Run `.\ArcaneTests.exe "[time]"`. Expected: compile FAIL, because `Arcane/Sim/Time.hpp` is not found.

- [ ] **Step 3: Create `ArcaneCore/src/Arcane/Sim/Time.hpp`:**

```cpp
#pragma once

// Arcane::Time -- the simulation clock as a registry RESOURCE (input-seam spec
// 2026-10-02 s3). RunLoop republishes it before every fixed step and again
// before the Update scheduler, so it is always present in a world a host
// advances and survives every registry swap (Play/Stop, scene open, hot
// reload) by construction. Systems declare it as a parameter:
//
//     void operator()(Arcane::Res<Arcane::Time> time)   // time->fixedDt, time->fixedStep, ...
//
// Code outside a system reads Registry().GetResource<Arcane::Time>().
//
// Plain data with no Core includes: RunLoop.hpp sits on the plugin-facing
// include chain, which stays Core-free (RunLoop.hpp's own note).

#include <cstdint>

namespace Arcane
{
    struct Time
    {
        double        realDt      = 0.0;   // this frame's wall-clock dt, unscaled
        double        dt          = 0.0;   // realDt * timeScale; 0 while paused
        double        fixedDt     = 0.0;   // 1 / fixedHz, the canonical fixed step
        double        alpha       = 0.0;   // render interpolation; inside a fixed step it is the PREVIOUS frame's
        double        elapsed     = 0.0;   // sim time: fixedDt accumulated once per fixed step
        std::uint64_t fixedStep   = 0;     // fixed steps since the registry was bound (first step = 1)
        double        timeScale   = 1.0;
        bool          paused      = false;
        bool          inFixedStep = false; // true while the fixed phase (plugin hook + fixedUpdate) runs
    };
}
```

- [ ] **Step 4: Change `RunLoop.hpp`.**
  - Add `#include <Arcane/Sim/Time.hpp>` beside the `SystemSchedulers` include.
  - Replace both `Advance` bodies, `Rebind`, and `StepFixed`'s `runFixed` lambda. Add the members.

```cpp
        // Advance one real frame. Returns the render alpha in [0,1) for interpolation.
        double Advance(double realDt)
        {
            StepFixed(realDt, nullptr);
            PublishTime(realDt, /*inFixedStep*/ false);
            m_schedulers->update.Execute(*m_registry, &m_schedulers->executor);
            return m_alpha;
        }
```

```cpp
        double Advance(double realDt,
                       const std::function<void(double)>& pluginFixed,
                       const std::function<void(double, double)>& pluginUpdate)
        {
            StepFixed(realDt, &pluginFixed);
            PublishTime(realDt, /*inFixedStep*/ false);
            m_schedulers->update.Execute(*m_registry, &m_schedulers->executor);
            if (pluginUpdate) pluginUpdate(realDt, m_alpha);
            return m_alpha;
        }
```

In `Rebind`, after `m_singleStep = false;`:

```cpp
            // The sim clock belongs to the registry's run too (input-seam spec s3):
            // Play starts at step 0. The next Advance republishes Time into the
            // new registry.
            m_fixedStep = 0;
            m_elapsed   = 0.0;
```

In `StepFixed`, the `runFixed` lambda becomes:

```cpp
            const auto runFixed = [&]
            {
                ++m_fixedStep;
                m_elapsed += fixedDt;
                PublishTime(realDt, /*inFixedStep*/ true);
                if (pluginFixed && *pluginFixed) (*pluginFixed)(fixedDt);
                m_schedulers->fixedUpdate.Execute(*m_registry, &m_schedulers->executor);
            };
```

Add this private helper before `StepFixed`:

```cpp
        // Republish Arcane::Time (input-seam spec s3). Called before every fixed
        // step and before Update, so a registry swapped in between frames gets
        // it on the very next pass and no system ever runs without it.
        void PublishTime(double realDt, bool inFixedStep)
        {
            Time t;
            t.realDt      = realDt;
            t.paused      = m_paused;
            t.timeScale   = m_timeScale;
            t.dt          = m_paused ? 0.0 : realDt * m_timeScale;
            t.fixedDt     = 1.0 / m_cfg.fixedHz;
            t.alpha       = m_alpha;
            t.elapsed     = m_elapsed;
            t.fixedStep   = m_fixedStep;
            t.inFixedStep = inFixedStep;
            m_registry->SetResource<Time>(std::move(t));
        }
```

Add these members after `bool m_singleStep = false;`:

```cpp
        // The sim clock published as Arcane::Time; reset by Rebind.
        std::uint64_t m_fixedStep = 0;
        double        m_elapsed   = 0.0;
```

- [ ] **Step 5: Build Debug and run the tests.**
  - Run: `.\ArcaneTests.exe "[time]"`, then `"[sim]"`, then `"[trajectory]"`. Expected: PASS.
  - Then run `.\ArcaneTests.exe "~[gpu]~[shell]"`. Expected: PASS.
  - **If a scene or snapshot test now sees an extra resource, the test is wrong about "exactly these resources".** Resources are excluded from `Registry::Save`, so the exception is a test that counts registry resources. Update it to tolerate `Arcane::Time` and say so in the report.

- [ ] **Step 6: Commit.**

```bash
git add ArcaneCore/src/Arcane/Sim/Time.hpp ArcaneCore/src/Arcane/Sim/RunLoop.hpp ArcaneTests/src/RunLoopTest.cpp
git commit -m "feat(core): Arcane::Time -- RunLoop publishes the sim clock (step counter, elapsed, dt/realDt/fixedDt, alpha, pause, scale) as a registry resource before every fixed step and before Update; Rebind restarts it" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 5: `LocalInputUser::Generation()`

**Files:**
- Modify: `ArcaneClient/src/Arcane/Input/LocalInputUser.hpp`:
  - the public accessor beside `Configured()` (:31);
  - the member at the END of the private members (:98).
- Modify: `ArcaneClient/src/Arcane/Input/LocalInputUser.cpp`: `Configure` (:15-88) and `Clear` (:90-103).
- Test: `ArcaneTests/src/ClientRuntimeTest.cpp` (append).

**Interfaces:**
- Produces: `std::uint64_t LocalInputUser::Generation() const noexcept`. It is strictly greater after every successful `Configure` (cold or same-project re-entry) and after every `Clear`. A FAILED `Configure` leaves it unchanged.
- **This layout change is the branch's one ABI-relevant change** (spec s4). It is NOT bumped here (Global Constraints, ABI).

- [ ] **Step 1: Write the failing test.** Append to `ClientRuntimeTest.cpp`:

```cpp
TEST_CASE("LocalInputUser::Generation moves on every Configure and Clear, never on a failed Configure", "[client][input]")
{
    Arcane::LocalInputUser user;
    const auto project = *Arcane::Guid::FromString("66666666-6666-4666-8666-666666666666");
    const std::uint64_t g0 = user.Generation();

    REQUIRE(user.Configure(RuntimeInputAsset(), project));          // cold
    const std::uint64_t g1 = user.Generation();
    CHECK(g1 > g0);

    REQUIRE(user.Configure(RuntimeInputAsset(), project));          // same-project re-entry
    const std::uint64_t g2 = user.Generation();
    CHECK(g2 > g1);

    CHECK_FALSE(user.Configure(RuntimeInputAsset(), Arcane::Guid::Nil()));   // refused
    CHECK(user.Generation() == g2);

    user.Clear();
    CHECK(user.Generation() > g2);
}
```

- [ ] **Step 2:** Run `.\ArcaneTests.exe "[client][input]"`. Expected: compile FAIL, because `Generation` is not a member.

- [ ] **Step 3: Implement.** In `LocalInputUser.hpp`, after `Configured()`:

```cpp
        // Bumps on every successful Configure (cold or same-project re-entry)
        // and every Clear: an ActionRef (GameInput.hpp) re-resolves its cached
        // action id when this moves (input-seam spec s4).
        [[nodiscard]] std::uint64_t Generation() const noexcept { return generation_; }
```

Add `#include <cstdint>` to the includes. As the LAST private member (after `reportedQueries_`), add:

```cpp
        std::uint64_t generation_ = 0;
```

In `LocalInputUser.cpp`:
- **Re-entry path:** add `++generation_;` immediately before its `return true;` (after `rebind_ = {};`).
- **Cold path:** add `++generation_;` immediately before the final `return true;`.
- **`Clear()`:** add `++generation_;` as the last statement. Do NOT reset it to 0 inside `Clear()`.

- [ ] **Step 4: Run tests.** Run `.\ArcaneTests.exe "[client][input]"` and `"[trajectory]"`. Expected: PASS.

- [ ] **Step 5: Commit.**

```bash
git add ArcaneClient/src/Arcane/Input/LocalInputUser.hpp ArcaneClient/src/Arcane/Input/LocalInputUser.cpp ArcaneTests/src/ClientRuntimeTest.cpp
git commit -m "feat(client): LocalInputUser::Generation -- bumps on every successful Configure and Clear so cached action ids know to re-resolve (layout change; ABI bump owed at merge)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 6: `Arcane::GameInput` and `Arcane::ActionRef`, published by `ClientRuntime`

**Files:**
- Create: `ArcaneClient/src/Arcane/Input/GameInput.hpp` (header-only)
- Modify: `ArcaneClient/src/Arcane/Client/ClientRuntime.cpp`: `ConfigureGameInput`, `UpdateGameInput`, `BeginGameInputFixedStep` (:72-79).
- Create: `ArcaneTests/src/GameInputTest.cpp`

**Interfaces:**
- Consumes: `LocalInputUser::{Generation, FindAction, Value, Down, Pressed, Released, PressedThisFixedStep, ReleasedThisFixedStep}`.
- Produces:
  - `class Arcane::ActionRef { ActionRef(std::string_view map, std::string_view action); const std::string& Map() const; const std::string& Action() const; }`;
  - `class Arcane::GameInput`:
    - `GameInput()`; `explicit GameInput(const LocalInputUser*)`;
    - `bool HasUser() const`; `std::uint64_t Generation() const`;
    - `std::optional<Guid> FindAction(std::string_view, std::string_view) const`;
    - `std::optional<Guid> Resolve(const ActionRef&) const`;
    - `InputActionValue Value(const ActionRef&) const`;
    - `bool Down/Pressed/Released/PressedThisFixedStep/ReleasedThisFixedStep(const ActionRef&) const`;
    - the same five plus `Value` overloaded on `const Guid&`.
  - Every client world carries the resource after the first `ConfigureGameInput`/`UpdateGameInput`/`BeginGameInputFixedStep`.
- **Decision (spec s4 sketched `class ARCANE_API GameInput`):** it is HEADER-ONLY with no export. Every method forwards inline to `LocalInputUser`'s already-exported methods, so ArcaneClient.dll gains no exports and the type adds no ABI surface.

- [ ] **Step 1: Write the failing tests.** Create `ArcaneTests/src/GameInputTest.cpp`:

```cpp
// Arcane::GameInput / Arcane::ActionRef (input-seam spec 2026-10-02 s4): the
// read-only gameplay-input view a client world publishes as a resource, and
// the by-name action handle that re-resolves across a reconfigure.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Input/GameInput.hpp>
#include <Arcane/Input/InputActionAsset.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Input/LocalInputUser.hpp>

#include <Json.hpp>

#include "Helpers/TestTypeContext.hpp"

namespace
{
    constexpr std::uint32_t kScancodeW = 26;   // SDL_SCANCODE_W

    // Player.Jump bound to W; `jumpName` lets a test rename the action.
    Arcane::InputActionAsset JumpAsset(const char* jumpName = "Jump")
    {
        const nlohmann::json j = nlohmann::json::parse(std::string(R"({
          "version": 1, "id": "77777777-7777-4777-8777-777777777777",
          "defaultMap": "77777777-7777-4777-8777-000000000001",
          "actionMaps": [ { "id": "77777777-7777-4777-8777-000000000001", "name": "Player",
            "actions": [ { "id": "77777777-7777-4777-8777-000000000002", "name": ")") + jumpName + R"(",
              "type": "Button",
              "bindings": [ { "id": "77777777-7777-4777-8777-000000000003", "path": "<Keyboard>/scancode/w" } ] } ] } ] })");
        auto asset = Arcane::InputActionAsset::FromJson(j);
        REQUIRE(asset);
        return *asset;
    }
    const Arcane::Guid kProject = *Arcane::Guid::FromString("88888888-8888-4888-8888-888888888888");
}

TEST_CASE("GameInput with no user answers zero and false", "[client][input][gameinput]")
{
    const Arcane::GameInput none;
    Arcane::ActionRef jump{"Player", "Jump"};
    CHECK_FALSE(none.HasUser());
    CHECK(none.Generation() == 0);
    CHECK_FALSE(none.Resolve(jump).has_value());
    CHECK_FALSE(none.Down(jump));
    CHECK_FALSE(none.PressedThisFixedStep(jump));
    CHECK(none.Value(jump).scalar == 0.0f);
}

TEST_CASE("GameInput reads the user's live state through an ActionRef", "[client][input][gameinput]")
{
    Arcane::LocalInputUser user;
    REQUIRE(user.Configure(JumpAsset(), kProject));
    const Arcane::GameInput in{&user};
    Arcane::ActionRef jump{"Player", "Jump"};

    Arcane::InputSnapshot held;
    held.SetScancode(kScancodeW);
    user.Update(1.0 / 60.0, held);
    CHECK(in.Down(jump));
    CHECK(in.Pressed(jump));
    user.BeginFixedStep();
    CHECK(in.PressedThisFixedStep(jump));
    user.BeginFixedStep();                     // second fixed step of the same frame
    CHECK_FALSE(in.PressedThisFixedStep(jump));   // the edge belongs to the first only
    CHECK(in.Down(jump));
}

// Review Focus #1: a cached ActionRef across a re-Configure (hot edit, rename, project switch).
TEST_CASE("ActionRef re-resolves when the input asset is reconfigured, answers zero while its action is gone",
          "[client][input][gameinput]")
{
    Arcane::LocalInputUser user;
    REQUIRE(user.Configure(JumpAsset("Jump"), kProject));
    const Arcane::GameInput in{&user};
    Arcane::ActionRef jump{"Player", "Jump"};
    const auto first = in.Resolve(jump);
    REQUIRE(first);

    REQUIRE(user.Configure(JumpAsset("Leap"), kProject));      // same project, action renamed
    CHECK_FALSE(in.Resolve(jump).has_value());
    Arcane::InputSnapshot held;
    held.SetScancode(kScancodeW);
    user.Update(1.0 / 60.0, held);
    CHECK_FALSE(in.Down(jump));                                  // zero, not the stale id

    REQUIRE(user.Configure(JumpAsset("Jump"), kProject));      // renamed back
    const auto again = in.Resolve(jump);
    REQUIRE(again);
    CHECK(*again == *first);

    user.Clear();                                                // project closed
    CHECK_FALSE(in.Resolve(jump).has_value());
}

TEST_CASE("ClientRuntime publishes GameInput on configure, every frame and every fixed step", "[client][input][gameinput]")
{
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    CHECK(runtime.Registry().GetResource<Arcane::GameInput>() == nullptr);

    runtime.UpdateGameInput(1.0 / 60.0, {});                     // unconfigured still publishes
    const auto* unconfigured = runtime.Registry().GetResource<Arcane::GameInput>();
    REQUIRE(unconfigured);
    CHECK(unconfigured->HasUser());
    CHECK_FALSE(unconfigured->Down(Arcane::ActionRef{"Player", "Jump"}));

    runtime.ResetRegistry();                                     // a swap drops resources...
    CHECK(runtime.Registry().GetResource<Arcane::GameInput>() == nullptr);
    runtime.BeginGameInputFixedStep();                           // ...the next pass republishes
    CHECK(runtime.Registry().GetResource<Arcane::GameInput>() != nullptr);

    runtime.ResetRegistry();
    REQUIRE(runtime.ConfigureGameInput(JumpAsset(), kProject));
    const auto* configured = runtime.Registry().GetResource<Arcane::GameInput>();
    REQUIRE(configured);
    CHECK(configured->Resolve(Arcane::ActionRef{"Player", "Jump"}).has_value());
}
```

**Before writing that fixture,** check `ArcaneClient/src/Arcane/Input/InputActionAsset.hpp` for the real JSON entry point (`FromJson`/`Parse`) and its return type. `ClientRuntimeTest.cpp`'s `RuntimeInputAsset()` helper shows the working spelling; mirror it exactly.

- [ ] **Step 2:** Run `.\ArcaneTests.exe "[gameinput]"`. Expected: compile FAIL, because `Arcane/Input/GameInput.hpp` is not found.

- [ ] **Step 3: Create `ArcaneClient/src/Arcane/Input/GameInput.hpp`:**

```cpp
#pragma once

// Arcane::GameInput -- the READ-ONLY gameplay-input view a client world
// publishes as a registry resource (input-seam spec 2026-10-02 s4), and
// Arcane::ActionRef, a by-name action handle. Systems declare it as a param:
//
//     Arcane::ActionRef jump{"Player", "Jump"};             // a system member
//     void operator()(Arcane::Res<Arcane::GameInput> input) // input->PressedThisFixedStep(jump)
//
// Query only: maps, control schemes, rebinding and profiles stay on
// Client()->GameInput() (the LocalInputUser). Every client world carries one,
// even with no input asset configured (it then answers zero/false); a server
// world has none. Header-only: every call forwards to LocalInputUser's
// exported methods, so this adds no DLL surface.

#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/LocalInputUser.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace Arcane
{
    // A gameplay action named by map + action. Resolves lazily against the
    // GameInput that queries it and re-resolves whenever the input user's
    // Generation() moves (a hot-edited asset, a project switch). An action that
    // does not exist answers zero/false and warns ONCE per generation.
    class ActionRef
    {
    public:
        ActionRef(std::string_view map, std::string_view action) : m_map(map), m_action(action) {}

        [[nodiscard]] const std::string& Map() const noexcept    { return m_map; }
        [[nodiscard]] const std::string& Action() const noexcept { return m_action; }

    private:
        friend class GameInput;
        static constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();

        std::string m_map;
        std::string m_action;
        mutable std::optional<Guid> m_id;
        mutable std::uint64_t m_resolvedGeneration = kNever;
        mutable std::uint64_t m_warnedGeneration   = kNever;
    };

    class GameInput
    {
    public:
        GameInput() = default;
        explicit GameInput(const LocalInputUser* user) noexcept : m_user(user) {}

        [[nodiscard]] bool HasUser() const noexcept { return m_user != nullptr; }
        [[nodiscard]] std::uint64_t Generation() const noexcept { return m_user ? m_user->Generation() : 0; }

        [[nodiscard]] std::optional<Guid> FindAction(std::string_view map, std::string_view action) const
        {
            return m_user ? m_user->FindAction(map, action) : std::nullopt;
        }

        [[nodiscard]] std::optional<Guid> Resolve(const ActionRef& ref) const
        {
            if (!m_user) return std::nullopt;
            const std::uint64_t gen = m_user->Generation();
            if (ref.m_resolvedGeneration != gen)
            {
                ref.m_id = m_user->FindAction(ref.m_map, ref.m_action);
                ref.m_resolvedGeneration = gen;
            }
            if (!ref.m_id && ref.m_warnedGeneration != gen)
            {
                ref.m_warnedGeneration = gen;
                ARC_WARN("input: action '{}.{}' is not in the selected gameplay input asset -- it reads as zero",
                         ref.m_map, ref.m_action);
            }
            return ref.m_id;
        }

        [[nodiscard]] InputActionValue Value(const ActionRef& r) const { const auto id = Resolve(r); return id ? Value(*id) : InputActionValue{}; }
        [[nodiscard]] bool Down(const ActionRef& r) const                  { const auto id = Resolve(r); return id && Down(*id); }
        [[nodiscard]] bool Pressed(const ActionRef& r) const               { const auto id = Resolve(r); return id && Pressed(*id); }
        [[nodiscard]] bool Released(const ActionRef& r) const              { const auto id = Resolve(r); return id && Released(*id); }
        [[nodiscard]] bool PressedThisFixedStep(const ActionRef& r) const  { const auto id = Resolve(r); return id && PressedThisFixedStep(*id); }
        [[nodiscard]] bool ReleasedThisFixedStep(const ActionRef& r) const { const auto id = Resolve(r); return id && ReleasedThisFixedStep(*id); }

        [[nodiscard]] InputActionValue Value(const Guid& id) const { return m_user ? m_user->Value(id) : InputActionValue{}; }
        [[nodiscard]] bool Down(const Guid& id) const                  { return m_user && m_user->Down(id); }
        [[nodiscard]] bool Pressed(const Guid& id) const               { return m_user && m_user->Pressed(id); }
        [[nodiscard]] bool Released(const Guid& id) const              { return m_user && m_user->Released(id); }
        [[nodiscard]] bool PressedThisFixedStep(const Guid& id) const  { return m_user && m_user->PressedThisFixedStep(id); }
        [[nodiscard]] bool ReleasedThisFixedStep(const Guid& id) const { return m_user && m_user->ReleasedThisFixedStep(id); }

    private:
        const LocalInputUser* m_user = nullptr;
    };
}
```

Check that `Arcane/Guid.hpp` is the real include path for `Guid` (the trajectory test's includes and `LocalInputUser.hpp` show the spelling). Fix the include if it differs.

- [ ] **Step 4: Publish from `ClientRuntime.cpp`.**
  - Add `#include <Arcane/Input/GameInput.hpp>`.
  - Inside `ClientRuntime`, the member function `GameInput()` HIDES the type (Global Constraints), so qualify it.

```cpp
    // Publish the read-only gameplay-input view (input-seam spec s4). Called on
    // every configure, every frame and every fixed step, so a swapped-in
    // registry carries it again on the very next pass.
    static void PublishGameInput(Runtime& core, const LocalInputUser& user)
    {
        core.Registry().SetResource<::Arcane::GameInput>(::Arcane::GameInput{&user});
    }

    bool ClientRuntime::ConfigureGameInput(const InputActionAsset& asset, const Guid& projectId)
    {
        const bool ok = m_pres.gameInput.Configure(asset, projectId);
        PublishGameInput(m_core, m_pres.gameInput);
        return ok;
    }
    void ClientRuntime::UpdateGameInput(double dt, const InputSnapshot& snapshot)
    {
        m_pres.gameInput.Update(dt, snapshot);
        PublishGameInput(m_core, m_pres.gameInput);
    }
    void ClientRuntime::BeginGameInputFixedStep()
    {
        m_pres.gameInput.BeginFixedStep();
        PublishGameInput(m_core, m_pres.gameInput);
    }
```

Put `PublishGameInput` in the file's anonymous or `static` scope, above these definitions.

- [ ] **Step 5: Build and run tests.** Run `ThirdParty\premake5\premake5.exe vs2026` (a new test .cpp), build, then run `.\ArcaneTests.exe "[gameinput]"`, `"[client][input]"` and `"[trajectory]"`. Expected: PASS.

- [ ] **Step 6: Commit.**

```bash
git add ArcaneClient/src/Arcane/Input/GameInput.hpp ArcaneClient/src/Arcane/Client/ClientRuntime.cpp ArcaneTests/src/GameInputTest.cpp
git commit -m "feat(client): Arcane::GameInput + Arcane::ActionRef -- a read-only gameplay-input resource every client world publishes on configure, every frame and every fixed step; ActionRef re-resolves across a reconfigure and warns once per generation for a missing action" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 7: `Time` and `GameInput` survive every registry swap

**Files:**
- Create: `ArcaneTests/src/ResourceSwapTest.cpp`

**Interfaces:**
- Consumes:
  - `Arcane::Editor::PlaySession::{Play, Stop}` (`ArcaneEditor/src/App/PlayMode.hpp:96-103`, source-compiled into ArcaneTests);
  - `ClientRuntime::{ResetRegistry, Loop, UpdateGameInput, Core}`;
  - `PluginHost` with `HotReloadPluginV1.dll`, `ForceReload`.
- Produces: the T3 swap guarantee from the spec (s8).

- [ ] **Step 1: Write the test.** It is expected to PASS at once, because the publishing design makes it hold. It is still written before any further change, to pin the guarantee. Create `ArcaneTests/src/ResourceSwapTest.cpp`:

```cpp
// Input-seam spec 2026-10-02 s8 T3: Arcane::Time and Arcane::GameInput are
// present after EVERY registry swap a host performs -- Play, Stop, scene open
// (ResetRegistry) and hot reload -- because RunLoop and ClientRuntime
// republish them on the very next pass. A resource is NOT in a registry
// snapshot, so a design that published once at boot would lose both here.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Input/GameInput.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Sim/Time.hpp>

#include <Astra/Registry/Registry.hpp>

#include <App/PlayMode.hpp>

#include <filesystem>

#include "Helpers/TestTypeContext.hpp"

namespace
{
    void Frame(Arcane::ClientRuntime& c, Arcane::PluginHost* host = nullptr)
    {
        c.UpdateGameInput(1.0 / 60.0, {});
        c.Loop().Advance(1.0 / 60.0,
            [&](double dt)           { c.BeginGameInputFixedStep(); if (host) host->FixedUpdateAll(dt); },
            [&](double dt, double a) { if (host) host->UpdateAll(dt, a); });
    }
    bool HasBoth(Arcane::ClientRuntime& c)
    {
        return c.Registry().GetResource<Arcane::Time>() != nullptr &&
               c.Registry().GetResource<Arcane::GameInput>() != nullptr;
    }
}

TEST_CASE("Time and GameInput are back one frame after Play, Stop and a scene open", "[client][time][gameinput][editor]")
{
    Arcane::ClientRuntime client(Arcane::Test::Process());
    Arcane::RegisterSceneComponents(client.Registry());
    client.Registry().CreateEntity();
    Frame(client);
    REQUIRE(HasBoth(client));

    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(client.Core()));
    Frame(client);
    CHECK(HasBoth(client));
    const std::uint64_t playSteps = client.Registry().GetResource<Arcane::Time>()->fixedStep;

    REQUIRE(play.Stop(client.Core()));             // RestoreRegistry: resources gone with the swap
    Frame(client);
    CHECK(HasBoth(client));
    CHECK(client.Registry().GetResource<Arcane::Time>()->fixedStep <= playSteps);   // Rebind restarted the clock
    CHECK(client.Registry().GetResource<Arcane::Time>()->paused);                   // Stop re-paused

    client.ResetRegistry();                         // File > Open Scene's swap
    Frame(client);
    CHECK(HasBoth(client));
}

TEST_CASE("Time and GameInput are back one frame after a module hot reload", "[client][time][gameinput][hotreload]")
{
    std::error_code ec;
    std::filesystem::copy_file("HotReloadPluginV1.dll", "ResourceSwapPlugin.dll",
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);

    Arcane::ClientRuntime client(Arcane::Test::Process());
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("ResourceSwapPlugin.dll"));
    REQUIRE(host.AttachRuntime(client.Core()));
    REQUIRE(host.Load());
    Frame(client, &host);
    REQUIRE(HasBoth(client));

    REQUIRE(host.ForceReload());                    // SaveState -> unload -> load -> LoadState
    Frame(client, &host);
    CHECK(HasBoth(client));

    host.Unload();
    std::filesystem::remove("ResourceSwapPlugin.dll", ec);
}
```

**Check before running:** `PlaySession::Play(Arcane::Runtime&, PluginHost* = nullptr, ...)` takes the CORE runtime, as shown here. Confirm against `PlayMode.hpp:96` and adjust the call if the signature differs.

- [ ] **Step 2: Generate, build and run.** Run `.\ArcaneTests.exe "[time][gameinput]"`. Expected: PASS.
  - **If a case fails, the resource is NOT republished on some path.** Fix the publisher (RunLoop or ClientRuntime). Never weaken the test.
- [ ] **Step 3: Commit.**

```bash
git add ArcaneTests/src/ResourceSwapTest.cpp
git commit -m "test(client): Time and GameInput are present one frame after Play, Stop, scene open and hot reload (input-seam spec s8 T3)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 8: `ARCANE_SYSTEM` (and `RegisterSystem<T>`) register parameter-style systems

**Files:**
- Modify: `ArcaneCore/src/Arcane/Plugin/GameSystems.hpp:52-65` (`Detail::AddSystemFactory`)
- Create: `ArcaneTests/src/ParamSystemRegistrationTest.cpp`

**Interfaces:**
- Consumes: Astra `ParamFunctor`, the param `SystemScheduler::AddSystem(Fn&&)`, and `KeyOf` (Task 2/3).
- Produces: `Arcane::Game::Detail::AddSystemFactory<T>` instantiates `T` through Astra's parameter path when `Astra::ParamFunctor<T>` holds, and through the typed path otherwise. `ARCANE_SYSTEM` and `GameModule::RegisterSystem<T>` both go through it.
- **The tests call `Detail::AddSystemFactory` DIRECTLY, never `ARCANE_SYSTEM`.** `RoleMaskTest.cpp:44` asserts the test exe holds exactly ONE automatic registrar, and a new `ARCANE_SYSTEM` in ArcaneTests would break that.

- [ ] **Step 1: Write the failing test.** Create `ArcaneTests/src/ParamSystemRegistrationTest.cpp`:

```cpp
// Input-seam spec 2026-10-02 s5.2: the game-module system factory registers a
// PARAMETER-style system (operator() taking View&/Res/ResMut/Commands) through
// Astra's param path, keyed by its own type and ordered by its traits.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Plugin/GameSystems.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>
#include <Arcane/Sim/Time.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <string>

namespace
{
    std::string g_order;

    struct TypedLater { void operator()(Astra::Registry&) { g_order += 'T'; } };

    struct ParamProbe : Astra::SystemTraits<Astra::Before<TypedLater>>
    {
        inline static std::uint64_t lastStep = 0;
        void operator()(Astra::Res<Arcane::Time> time) { lastStep = time->fixedStep; g_order += 'P'; }
    };
}

TEST_CASE("AddSystemFactory instantiates a parameter-style system, keyed by its own type, ordered by its traits",
          "[runtime][systems][param]")
{
    Arcane::SystemFactoryTable table;
    const int owner = 1;
    table.BeginOwner(&owner);
    Arcane::Game::Detail::AddSystemFactory<TypedLater>(table, Arcane::RoleMask::Both, Arcane::SystemPhase::FixedUpdate);
    Arcane::Game::Detail::AddSystemFactory<ParamProbe>(table, Arcane::RoleMask::Both, Arcane::SystemPhase::FixedUpdate);
    table.EndOwner();

    Arcane::SystemSchedulers sch(nullptr);
    CHECK(table.InstantiateInto(sch, Arcane::NetMode::Standalone) == 2);
    CHECK(sch.fixedUpdate.HasSystem<ParamProbe>());

    Astra::Registry reg;
    Arcane::RunLoop loop(reg, sch);
    g_order.clear();
    ParamProbe::lastStep = 0;
    loop.RequestSingleStep();
    loop.SetPaused(true);
    loop.Advance(1.0 / 60.0);
    CHECK(ParamProbe::lastStep == 1);   // it read the published Time
    CHECK(g_order == "PT");            // Before<TypedLater> honoured though registered second

    table.ClearOwner(&owner);
}
```

- [ ] **Step 2:** Run `.\ArcaneTests.exe "[param]"` (after premake and a build). Expected: compile FAIL inside `AddSystemFactory`: `scheduler.AddSystem<ParamProbe>()` has no viable overload, because the typed path needs `System<T>`.

- [ ] **Step 3: Implement.** In `GameSystems.hpp`, `Detail::AddSystemFactory`'s lambda becomes:

```cpp
                [args...](Astra::SystemScheduler& scheduler)
                {
                    // Two system shapes (input-seam spec s5.2): a PARAMETER
                    // system (operator() over View&/Res/ResMut/Commands --
                    // the game-facing style) goes through Astra's param path,
                    // keyed by its own type and ordered by its SystemTraits;
                    // a registry-style system (operator()(Registry&) + traits)
                    // through the typed path, as before.
                    if constexpr (Astra::ParamFunctor<System>)
                        std::ignore = scheduler.AddSystem(System{args...});
                    else
                        std::ignore = scheduler.AddSystem<System>(args...);
                },
```

Make sure `GameSystems.hpp` includes `<Astra/System/System.hpp>` (it brings `SystemParam.hpp`). Add it if it is missing.

- [ ] **Step 4: Run tests.** Run `.\ArcaneTests.exe "[param]"`, `"[systems]"`, `"[netmode]"` and `"[trajectory]"`. Expected: PASS.
- [ ] **Step 5: Commit.**

```bash
git add ArcaneCore/src/Arcane/Plugin/GameSystems.hpp ArcaneTests/src/ParamSystemRegistrationTest.cpp
git commit -m "feat(core): ARCANE_SYSTEM / RegisterSystem register parameter-style systems through Astra's param path (own-type key, ordering traits) -- the game-facing system shape" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 9: `Arcane::Physics2D` — `Motion` / `SetVelocity` on `PhysicsResource`

**Files:**
- Modify: `ArcaneCore/src/Arcane/Scene/PhysicsSystem.hpp`. Move `BodyMotion2D` here (before `PhysicsResource`, :115). Add the two member declarations to `PhysicsResource`. Add `using Physics2D = PhysicsResource;` after it.
- Create: `ArcaneCore/src/Arcane/Scene/Physics2D.cpp`. It takes the bodies and the floor-support helper from `PhysicsCommands.cpp`.
- Modify: `ArcaneCore/src/Arcane/Scene/PhysicsCommands.hpp/.cpp`. They become thin FORWARDERS for now, so ReferenceProject (and the Task 1 gate) still build. Task 12 deletes them.
- Create: `ArcaneTests/src/Physics2DTest.cpp`

**Interfaces:**
- Produces:
  - `struct Arcane::BodyMotion2D { float velocityX, velocityY; bool bodyReady, supported; }` (unchanged fields, new home);
  - `ARCANE_CORE_API BodyMotion2D PhysicsResource::Motion(Astra::Entity, const RigidBody2D&) const;`
  - `ARCANE_CORE_API void PhysicsResource::SetVelocity(Astra::Entity, RigidBody2D&, float x, float y);`
  - `using Arcane::Physics2D = PhysicsResource;`
- The handle comes from `entityToBody` (`PhysicsSystem.hpp:118`), never `PhysicsBodyRef` (spec s5.3).

- [ ] **Step 1: Write the failing tests.** Create `ArcaneTests/src/Physics2DTest.cpp`:

```cpp
// Arcane::Physics2D (input-seam spec 2026-10-02 s5.3): the game-facing physics
// commands as members of the published PhysicsResource, taking the body handle
// from entityToBody. Behaviour must equal the deleted free functions'.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cmath>
#include <limits>

#include "Helpers/TestTypeContext.hpp"

namespace
{
    // A dynamic unit box resting on a static ground box.
    struct World
    {
        Arcane::Runtime rt{Arcane::Test::Process()};
        Astra::Entity ground, box;
        World()
        {
            auto& reg = rt.Registry();
            Arcane::RegisterSceneComponents(reg);
            ground = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(ground, Arcane::Transform{ .position = {0.0f, -0.5f, 0.0f}, .scale = {20.0f, 1.0f, 1.0f} });
            Arcane::RigidBody2D sb; sb.type = Arcane::Phys::BodyType::Static;
            reg.AddComponent<Arcane::RigidBody2D>(ground, sb);
            reg.AddComponent<Arcane::Collider2D>(ground, Arcane::Collider2D{});
            box = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(box, Arcane::Transform{ .position = {0.0f, 0.5f, 0.0f} });
            Arcane::RigidBody2D db; db.type = Arcane::Phys::BodyType::Dynamic;
            reg.AddComponent<Arcane::RigidBody2D>(box, db);
            reg.AddComponent<Arcane::Collider2D>(box, Arcane::Collider2D{});
        }
        void Step(int n)
        {
            for (int i = 0; i < n; ++i) { rt.EnsurePhysics(); rt.Loop().Advance(1.0 / 60.0); }
        }
        Arcane::Physics2D* Physics() { return rt.Registry().GetResource<Arcane::Physics2D>(); }
        Arcane::RigidBody2D& Body(Astra::Entity e) { return *rt.Registry().GetComponent<Arcane::RigidBody2D>(e); }
    };
}

TEST_CASE("Physics2D is the PhysicsResource", "[physics][physics2d]")
{
    STATIC_REQUIRE(std::is_same_v<Arcane::Physics2D, Arcane::PhysicsResource>);
}

TEST_CASE("Physics2D::Motion before the body exists reads RigidBody2D, bodyReady false", "[physics][physics2d]")
{
    World w;
    w.rt.EnsurePhysics();                                   // world minted, no step yet: no bodies
    Arcane::RigidBody2D& rb = w.Body(w.box);
    rb.velocity = {1.5f, -2.0f};
    const Arcane::BodyMotion2D m = w.Physics()->Motion(w.box, rb);
    CHECK_FALSE(m.bodyReady);
    CHECK(m.velocityX == 1.5f);
    CHECK(m.velocityY == -2.0f);

    w.Physics()->SetVelocity(w.box, rb, 3.0f, 0.0f);       // unminted: authored mint velocity
    CHECK(rb.velocity.x == 3.0f);
}

TEST_CASE("Physics2D::SetVelocity drives the live body; a resting body reads as supported", "[physics][physics2d]")
{
    World w;
    w.Step(60);                                             // fall, land, settle (sleep allowed)
    Arcane::RigidBody2D& rb = w.Body(w.box);
    Arcane::BodyMotion2D m = w.Physics()->Motion(w.box, rb);
    CHECK(m.bodyReady);
    CHECK(m.supported);                                     // contacts or, asleep, the shape cast

    w.Physics()->SetVelocity(w.box, rb, 2.0f, 0.0f);
    m = w.Physics()->Motion(w.box, rb);
    CHECK(m.velocityX == 2.0f);
    CHECK(rb.velocity.x == 2.0f);
}

TEST_CASE("Physics2D ignores non-finite input and non-dynamic bodies", "[physics][physics2d]")
{
    World w;
    w.Step(2);
    Arcane::RigidBody2D& rb = w.Body(w.box);
    const glm::vec2 before = rb.velocity;
    w.Physics()->SetVelocity(w.box, rb, std::numeric_limits<float>::quiet_NaN(), 0.0f);
    w.Physics()->SetVelocity(w.box, rb, 0.0f, std::numeric_limits<float>::infinity());
    CHECK(rb.velocity == before);

    Arcane::RigidBody2D& sb = w.Body(w.ground);
    w.Physics()->SetVelocity(w.ground, sb, 5.0f, 5.0f);
    CHECK(sb.velocity == glm::vec2(0.0f, 0.0f));
    const Arcane::BodyMotion2D gm = w.Physics()->Motion(w.ground, sb);
    CHECK_FALSE(gm.bodyReady);
    CHECK_FALSE(gm.supported);
}
```

**Check before running:**
- The `Transform` aggregate field names: `Components.hpp` has `position`/`rotation`/`scale`. Fix the designated initializers if the order differs.
- `Collider2D{}` default must be a unit box (`PhysicsComponents.hpp`). If its default is not a box shape, set `kind`/size explicitly.
- `Runtime::EnsurePhysics()` is public (`Runtime.hpp:293`).

- [ ] **Step 2:** Run `.\ArcaneTests.exe "[physics2d]"`. Expected: compile FAIL: `Physics2D` is not a member of `Arcane`.

- [ ] **Step 3: Implement the header side.**
  - In `PhysicsSystem.hpp`, before `struct PhysicsResource`, add `BodyMotion2D` (moved verbatim from `PhysicsCommands.hpp`):

```cpp
    // What a controller reads back from its body (input-seam spec s5.3).
    // Before the body is minted, velocity comes from RigidBody2D and
    // bodyReady is false.
    struct BodyMotion2D
    {
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        bool bodyReady = false;
        bool supported = false;
    };
```

  - Inside `struct PhysicsResource`, after `std::uint32_t reconciled = 0;`:

```cpp
        // ---- The game-facing commands (input-seam spec s5.3) -----------------
        // Exported: PhysicsWorld is linked inside ArcaneCore, so a game module
        // must call these rather than link Manifold2D itself. The body handle
        // comes from entityToBody (never PhysicsBodyRef: a game's view need not
        // name it, and before the first fixed step it does not exist yet).
        // Read the live dynamic body's velocity and floor support.
        ARCANE_CORE_API BodyMotion2D Motion(Astra::Entity entity, const RigidBody2D& body) const;
        // Set both axes on the live body, or the authored mint velocity before it
        // exists. Non-finite input and non-dynamic bodies are ignored.
        ARCANE_CORE_API void SetVelocity(Astra::Entity entity, RigidBody2D& body, float velocityX, float velocityY);
```

  - After the struct's closing `};`:

```cpp
    // The game-facing name (input-seam spec s5.3). The SAME type, so a system
    // taking ResMut<Physics2D> conflicts with PhysicsSystem in the scheduler.
    using Physics2D = PhysicsResource;
```

  - Make sure `PhysicsSystem.hpp` includes `<Arcane/Core/Api.hpp>` (add it if absent).

- [ ] **Step 4: Create `ArcaneCore/src/Arcane/Scene/Physics2D.cpp`.** Move `HasFloorSupport` and the two bodies here, rewritten as members:

```cpp
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>

#include <cmath>

namespace Arcane
{
    namespace
    {
        // (move HasFloorSupport here VERBATIM from PhysicsCommands.cpp:14-45)
    }

    BodyMotion2D PhysicsResource::Motion(Astra::Entity entity, const RigidBody2D& body) const
    {
        BodyMotion2D motion;
        if (body.type != Phys::BodyType::Dynamic)
            return motion;
        motion.velocityX = body.velocity.x;
        motion.velocityY = body.velocity.y;

        const auto it = entityToBody.find(entity);
        if (!world || it == entityToBody.end() || !world->IsValid(it->second))
            return motion;
        const Phys::Vec2 velocity = world->Velocity(it->second);
        motion.velocityX = static_cast<float>(velocity.x);
        motion.velocityY = static_cast<float>(velocity.y);
        motion.bodyReady = true;
        if (velocity.y <= Phys::Real(0))
            motion.supported = HasFloorSupport(*world, it->second);
        return motion;
    }

    void PhysicsResource::SetVelocity(Astra::Entity entity, RigidBody2D& body,
                                      float velocityX, float velocityY)
    {
        if (!std::isfinite(velocityX) || !std::isfinite(velocityY))
            return;
        if (body.type != Phys::BodyType::Dynamic)
            return;
        body.velocity = glm::vec2(velocityX, velocityY);

        const auto it = entityToBody.find(entity);
        if (world && it != entityToBody.end() && world->IsValid(it->second))
            world->SetVelocity(it->second, Phys::Vec2(velocityX, velocityY));
    }
}
```

**Parity:** the old functions looked the body up through `PhysicsBodyRef`, the new ones through `entityToBody`. PASS 2's mint writes both in the same step (`PhysicsSystem.hpp:470`), so they agree. If a reviewer finds a path where one is set without the other, report it and do not paper over it.

- [ ] **Step 5: Turn the old free functions into forwarders** (deleted in Task 12). In `PhysicsCommands.hpp`, include `<Arcane/Scene/PhysicsSystem.hpp>` and delete the `BodyMotion2D` definition (it now comes from there). Keep the two declarations. `PhysicsCommands.cpp` becomes:

```cpp
#include <Arcane/Scene/PhysicsCommands.hpp>

#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>

#include <Astra/Registry/Registry.hpp>

// TEMPORARY forwarders (input-seam plan Task 9 -> deleted by Task 12): the
// ReferenceProject sources keep compiling until the controller moves to
// Arcane::Physics2D.
namespace Arcane
{
    BodyMotion2D GetBodyMotion2D(Astra::Registry& registry, Astra::Entity entity)
    {
        const RigidBody2D* body = registry.GetComponent<RigidBody2D>(entity);
        if (!body) return {};
        if (const PhysicsResource* physics = registry.GetResource<PhysicsResource>())
            return physics->Motion(entity, *body);
        BodyMotion2D motion;
        if (body->type == Phys::BodyType::Dynamic)
        {
            motion.velocityX = body->velocity.x;
            motion.velocityY = body->velocity.y;
        }
        return motion;
    }

    void SetBodyVelocity2D(Astra::Registry& registry, Astra::Entity entity, float velocityX, float velocityY)
    {
        RigidBody2D* body = registry.GetComponent<RigidBody2D>(entity);
        if (!body) return;
        if (PhysicsResource* physics = registry.GetResource<PhysicsResource>())
        {
            physics->SetVelocity(entity, *body, velocityX, velocityY);
            return;
        }
        if (std::isfinite(velocityX) && std::isfinite(velocityY) && body->type == Phys::BodyType::Dynamic)
            body->velocity = glm::vec2(velocityX, velocityY);
    }
}
```

Add `#include <cmath>` there.

- [ ] **Step 6: Build and run tests.** Run `ThirdParty\premake5\premake5.exe vs2026`, build, then run `.\ArcaneTests.exe "[physics2d]"`, `"[physics]"` and **`"[trajectory]"` (must stay bit-identical)**. Expected: PASS.
- [ ] **Step 7: Commit.**

```bash
git add ArcaneCore/src/Arcane/Scene/PhysicsSystem.hpp ArcaneCore/src/Arcane/Scene/Physics2D.cpp ArcaneCore/src/Arcane/Scene/PhysicsCommands.hpp ArcaneCore/src/Arcane/Scene/PhysicsCommands.cpp ArcaneTests/src/Physics2DTest.cpp
git commit -m "feat(core): Arcane::Physics2D -- Motion/SetVelocity as exported members of the published PhysicsResource (handle via entityToBody); the Registry& free functions forward to them until the controller moves" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 10: The ECS facade — `Arcane/EcsFwd.hpp` + `Arcane/Ecs.hpp` (and the CVar `Any` rename)

**Files:**
- Create: `ArcaneCore/src/Arcane/EcsFwd.hpp`, `ArcaneCore/src/Arcane/Ecs.hpp`
- Modify: `ArcaneCore/src/Arcane/Config/CVarTypes.hpp:57` plus every caller of `Any(CVarFlags, CVarFlags)`. List them with:
  ```
  rg -n "\bAny\(" ArcaneCore ArcaneClient ArcaneEditor ArcaneRuntime ArcaneServer ArcaneTests --glob "*.{hpp,cpp}"
  ```
  and skip the member calls `.Any()`/`->Any()`. There are 11 today.
- Create: `ArcaneTests/src/EcsFacadeTest.cpp`

**Interfaces:**
- Produces:
  - **`EcsFwd.hpp`** (forward declarations + non-template aliases, for the light headers that today only forward-declare): `Arcane::Registry`, `ComponentRegistry`, `TypeContext`, `BinaryWriter`, `BinaryReader`, `ComponentModule`, `SystemScheduler`, `IWorkScheduler`.
  - **`Ecs.hpp`** (everything, including real headers):
    - all of the above;
    - `Entity`, `View<...>`, `Res<T>`, `ResMut<T>`, `Commands`;
    - `SystemTraits<...>`, `Reads<...>`, `Writes<...>`, `Before<...>`, `After<...>`, `AmbiguousWith<...>`, `Exclusive`, `ReadsResources<...>`, `WritesResources<...>`;
    - `Not<T>`, `With<T>`, `Changed<T>`, `Added<T>`, `Optional<T>`, `Any<...>`, `OneOf<...>`, `IncludeDisabled<T>`;
    - `Tick`, `Result<T, E>`, `SerializationError`;
    - `Arcane::Time` (re-exported by including `Arcane/Sim/Time.hpp`).
- **Decision (a spec gap found while reading the code):** `Arcane::Any(CVarFlags, CVarFlags)` (`CVarTypes.hpp:57`) would collide with the `Arcane::Any<...>` alias template. It is renamed `HasFlag`, a behaviour-free rename.
- `Reads<>`/`Writes<>` are aliased too (spec s6.1 omitted them). Engine registry-style systems keep using them, and parameter-style systems never need them.

- [ ] **Step 1: Write the failing test.** Create `ArcaneTests/src/EcsFacadeTest.cpp`:

```cpp
// The Arcane:: ECS facade (input-seam spec 2026-10-02 s6.1): every alias is
// the SAME type as the library's, so it changes no ABI and no serialized name.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Ecs.hpp>

#include <Astra/Astra.hpp>

#include <type_traits>

namespace { struct C {}; struct S { void operator()(Astra::Registry&) {} }; }

TEST_CASE("Arcane ECS aliases are the library types", "[facade]")
{
    STATIC_REQUIRE(std::is_same_v<Arcane::Registry, Astra::Registry>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Entity, Astra::Entity>);
    STATIC_REQUIRE(std::is_same_v<Arcane::View<C>, Astra::View<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Res<C>, Astra::Res<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ResMut<C>, Astra::ResMut<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Commands, Astra::Commands>);
    STATIC_REQUIRE(std::is_same_v<Arcane::SystemTraits<Arcane::Before<S>>, Astra::SystemTraits<Astra::Before<S>>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::After<S>, Astra::After<S>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::AmbiguousWith<S>, Astra::AmbiguousWith<S>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Reads<C>, Astra::Reads<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Writes<C>, Astra::Writes<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Exclusive, Astra::Exclusive>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ReadsResources<C>, Astra::ReadsResources<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::WritesResources<C>, Astra::WritesResources<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Not<C>, Astra::Not<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::With<C>, Astra::With<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Changed<C>, Astra::Changed<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Added<C>, Astra::Added<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Optional<C>, Astra::Optional<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Any<C>, Astra::Any<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::OneOf<C>, Astra::OneOf<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::IncludeDisabled<C>, Astra::IncludeDisabled<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::BinaryWriter, Astra::BinaryWriter>);
    STATIC_REQUIRE(std::is_same_v<Arcane::BinaryReader, Astra::BinaryReader>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ComponentModule, Astra::ComponentModule>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ComponentRegistry, Astra::ComponentRegistry>);
    STATIC_REQUIRE(std::is_same_v<Arcane::TypeContext, Astra::TypeContext>);
    STATIC_REQUIRE(std::is_same_v<Arcane::SystemScheduler, Astra::SystemScheduler>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Tick, Astra::Tick>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Result<int, Arcane::SerializationError>, Astra::Result<int, Astra::SerializationError>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::IWorkScheduler, Mosaic::IWorkScheduler>);
}

TEST_CASE("CVar flag test is HasFlag (Any is the query filter now)", "[facade][cvar]")
{
    CHECK(Arcane::HasFlag(Arcane::CVarFlags::Archive | Arcane::CVarFlags::Hidden, Arcane::CVarFlags::Hidden));
    CHECK_FALSE(Arcane::HasFlag(Arcane::CVarFlags::Archive, Arcane::CVarFlags::Hidden));
}
```

For the second case, add `#include <Arcane/Config/CVarTypes.hpp>`. Check `CVarFlags::Archive` exists (`CVarTypes.hpp:34` area). Use whichever two flags the enum really has.

- [ ] **Step 2:** Run `.\ArcaneTests.exe "[facade]"` after premake and a build. Expected: compile FAIL: `Arcane/Ecs.hpp` is not found.

- [ ] **Step 3: Create `ArcaneCore/src/Arcane/EcsFwd.hpp`:**

```cpp
#pragma once

// The light half of the Arcane:: ECS facade (input-seam spec 2026-10-02 s6):
// forward declarations + aliases for headers that must not pull the full
// Astra headers (Runtime.hpp, PluginABI.hpp, ProcessContext.hpp,
// SystemFactory.hpp). Game code includes <Arcane/Ecs.hpp> instead.
// Aliases, so the SAME types: no ABI or serialization change.

// ARCANE_INTERNAL_BEGIN: the facade's library side
namespace Astra
{
    class Registry;
    class ComponentRegistry;
    class TypeContext;
    class BinaryWriter;
    class BinaryReader;
    class ComponentModule;
    class SystemScheduler;
}
namespace Mosaic { struct IWorkScheduler; }

namespace Arcane
{
    using Registry          = Astra::Registry;
    using ComponentRegistry = Astra::ComponentRegistry;
    using TypeContext       = Astra::TypeContext;
    using BinaryWriter      = Astra::BinaryWriter;
    using BinaryReader      = Astra::BinaryReader;
    using ComponentModule   = Astra::ComponentModule;
    using SystemScheduler   = Astra::SystemScheduler;
    using IWorkScheduler    = Mosaic::IWorkScheduler;
}
// ARCANE_INTERNAL_END
```

- [ ] **Step 4: Create `ArcaneCore/src/Arcane/Ecs.hpp`:**

```cpp
#pragma once

// The Arcane:: ECS facade (input-seam spec 2026-10-02 s6.1). Game code spells
// ONLY Arcane:: names; the standalone libraries (Astra, Mosaic, Manifold2D)
// keep their own namespaces underneath. Bevy's prelude is the model: one
// include, one namespace. Every name is an ALIAS of the library type, so this
// changes no ABI and no serialized type name.
//
//     struct Mover : Arcane::SystemTraits<Arcane::Before<Arcane::PhysicsSystem>>
//     {
//         void operator()(Arcane::View<Arcane::Transform>& view, Arcane::Res<Arcane::Time> time);
//     };

#include <Arcane/EcsFwd.hpp>
#include <Arcane/Sim/Time.hpp>

// ARCANE_INTERNAL_BEGIN: the facade's library side
#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Core/Result.hpp>
#include <Astra/Core/Tick.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Query.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/SerializationError.hpp>
#include <Astra/System/System.hpp>
#include <Astra/System/SystemParam.hpp>
#include <Astra/System/SystemScheduler.hpp>

namespace Arcane
{
    // Entities and queries
    using Entity = Astra::Entity;
    template<typename... C> using View            = Astra::View<C...>;
    template<typename T>    using Not             = Astra::Not<T>;
    template<typename T>    using With            = Astra::With<T>;
    template<typename T>    using Changed         = Astra::Changed<T>;
    template<typename T>    using Added           = Astra::Added<T>;
    template<typename T>    using Optional        = Astra::Optional<T>;
    template<typename... T> using Any             = Astra::Any<T...>;
    template<typename... T> using OneOf           = Astra::OneOf<T...>;
    template<typename T>    using IncludeDisabled = Astra::IncludeDisabled<T>;

    // System parameters
    template<typename T> using Res    = Astra::Res<T>;
    template<typename T> using ResMut = Astra::ResMut<T>;
    using Commands = Astra::Commands;

    // System traits (parameter systems use ordering only; Reads/Writes/
    // ReadsResources/WritesResources/Exclusive are for registry-style systems)
    template<typename... T> using SystemTraits    = Astra::SystemTraits<T...>;
    template<typename... S> using Before          = Astra::Before<S...>;
    template<typename... S> using After           = Astra::After<S...>;
    template<typename... S> using AmbiguousWith   = Astra::AmbiguousWith<S...>;
    template<typename... C> using Reads           = Astra::Reads<C...>;
    template<typename... C> using Writes          = Astra::Writes<C...>;
    template<typename... R> using ReadsResources  = Astra::ReadsResources<R...>;
    template<typename... R> using WritesResources = Astra::WritesResources<R...>;
    using Exclusive = Astra::Exclusive;

    // Misc vocabulary module code meets
    using Tick = Astra::Tick;
    template<typename T, typename E> using Result = Astra::Result<T, E>;
    using SerializationError = Astra::SerializationError;
}
// ARCANE_INTERNAL_END
```

Verify each include path exists under `ThirdParty/Astra/include/Astra/` (for example `rg --files ThirdParty/Astra/include | rg "Tick.hpp|Result.hpp|Query.hpp|SerializationError.hpp"`). Fix any path that differs. Check whether `Astra::Result`'s primary template has a defaulted `E`; if it does, mirror the default in the alias (`template<typename T, typename E = <default>>`).

- [ ] **Step 5: Rename `Any(CVarFlags, CVarFlags)` → `HasFlag`.** Change it in `CVarTypes.hpp:57` and at all 11 callers. This is a mechanical rename with no behaviour change. Then confirm: `rg -n "\bAny\((slot|e|flags|desc|.*CVarFlags)" --glob "*.{hpp,cpp}"` must return nothing in Arcane code.

- [ ] **Step 6: Build and run tests.** Run `.\ArcaneTests.exe "[facade]"`, `"[cvar]"`, then `"~[gpu]~[shell]"`. Expected: PASS.
  - **If an existing translation unit inside `namespace Arcane` now fails** on an ambiguous unqualified `View`/`Entity`/`Registry` (alias vs a `using namespace Astra;`), qualify that use site. A grep at planning time found no `using namespace Astra` in Arcane.
- [ ] **Step 7: Commit.**

```bash
git add ArcaneCore/src/Arcane/EcsFwd.hpp ArcaneCore/src/Arcane/Ecs.hpp ArcaneCore/src/Arcane/Config/CVarTypes.hpp <each renamed caller> ArcaneTests/src/EcsFacadeTest.cpp
git commit -m "feat(core): the Arcane:: ECS facade -- Arcane/EcsFwd.hpp + Arcane/Ecs.hpp alias every game-facing Astra/Mosaic name (same types: no ABI, no serialization change); CVar Any(flags) renamed HasFlag to free Arcane::Any for the query filter" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 11: The reflection facade — `Arcane/Reflection.hpp`, `Arcane::Attr`, `ARCANE_CHANGE_TRACKED`

**Files:**
- Create: `ArcaneCore/src/Arcane/Reflection.hpp`
- Create: `ArcaneTests/src/ReflectionFacadeTest.cpp`

**Interfaces:**
- Produces:
  - **Macros:**
    - `ARCANE_REFLECT_TYPE`, `ARCANE_REFLECT_FIELD`, `ARCANE_END_REFLECT_TYPE`, `ARCANE_REFLECT_TYPE_ATTR`, `ARCANE_REFLECT_TYPE_END`;
    - `ARCANE_REFLECT_ENUM`, `ARCANE_REFLECT_ENUM_VALUE`, `ARCANE_REFLECT_ENUM_VALUE_NAMED`, `ARCANE_REFLECT_ENUM_VALUE_FULL`, `ARCANE_REFLECT_ENUM_FLAGS`, `ARCANE_END_REFLECT_ENUM`, `ARCANE_REFLECT_ENUM_END`;
    - `ARCANE_REFLECT_ATTR(AttrType, ...)` → `.Attr<::Arcane::Attr::AttrType>(__VA_ARGS__)`;
    - `ARCANE_CHANGE_TRACKED`.
  - **The attribute aliases:** `namespace Arcane::Attr` with `using ::Astra::<X>;` for all 15 attributes: Range, Hidden, ReadOnly, DisplayName, Tooltip, Category, Serializable, ColorFormat, AngleFormat, Multiline, FilePath, DragSpeed, Deprecated, AliasName, Precision.
- **Decision (a spec error found while reading the code):** spec s6.1 put the attribute aliases directly in `Arcane::`. But `Arcane::Hidden` already exists as the Outliner-eye tag COMPONENT (`ArcaneCore/src/Arcane/Scene/Components.hpp:302`). It is serialized by name as `"Arcane::Hidden"` and can never be renamed. So the attributes live in `Arcane::Attr`. Authors still write `ARCANE_REFLECT_ATTR(Hidden)` (the macro qualifies the name), and an `AngleFormat` argument is spelled `Arcane::Attr::AngleFormat::Unit::Degrees`.

- [ ] **Step 1: Write the failing test.** Create `ArcaneTests/src/ReflectionFacadeTest.cpp`:

```cpp
// The reflection facade (input-seam spec 2026-10-02 s6.1): ARCANE_REFLECT_*
// produce the same metadata as ASTRA_REFLECT_*, every Astra attribute has an
// Arcane::Attr alias (Review Focus #5), and ARCANE_CHANGE_TRACKED spells
// Astra's change-tracking member.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Reflection.hpp>

#include <Astra/Reflection/MetaRegistry.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "Helpers/ReferenceProjectDir.hpp"

namespace FacadeProbe
{
    struct Probe
    {
        ARCANE_CHANGE_TRACKED
        float speed = 1.0f;
        float angle = 0.0f;
        bool  transient = false;
    };

    ARCANE_REFLECT_TYPE(Probe)
        ARCANE_REFLECT_FIELD(Probe, speed)
            ARCANE_REFLECT_ATTR(Range, 0.0f, 10.0f)
            ARCANE_REFLECT_ATTR(Tooltip, "metres per second")
            ARCANE_REFLECT_ATTR(Category, "Motion")
            ARCANE_REFLECT_ATTR(DragSpeed, 0.1f)
            ARCANE_REFLECT_ATTR(Precision, 2)
        ARCANE_REFLECT_FIELD(Probe, angle)
            ARCANE_REFLECT_ATTR(AngleFormat, Arcane::Attr::AngleFormat::Unit::Degrees)
        ARCANE_REFLECT_FIELD(Probe, transient)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
    ARCANE_END_REFLECT_TYPE()
}

TEST_CASE("ARCANE_REFLECT_* register the same metadata ASTRA_REFLECT_* would", "[facade][reflection]")
{
    STATIC_REQUIRE(FacadeProbe::Probe::AstraChangeTracked);
    const Astra::TypeMeta* meta = Astra::GetMeta(Astra::TypeID<FacadeProbe::Probe>::Hash());
    REQUIRE(meta);
    REQUIRE(meta->fields.size() == 3);
    const Astra::FieldInfo& speed = meta->fields[0];
    CHECK(speed.name == "speed");
    CHECK(speed.HasAttribute<Astra::Range>());
    CHECK(speed.HasAttribute<Astra::Tooltip>());
    const Astra::FieldInfo& transient = meta->fields[2];
    CHECK(transient.HasAttribute<Astra::Hidden>());
    CHECK(transient.HasAttribute<Astra::Serializable>());
}

// Review Focus #5: Astra grows an attribute, the facade misses it, and a game
// author's ARCANE_REFLECT_ATTR fails to compile. The vendored Attribute.hpp is
// the list; Arcane/Reflection.hpp must alias every entry.
TEST_CASE("every Astra reflection attribute has an Arcane::Attr alias", "[facade][reflection]")
{
    const std::filesystem::path root = Arcane::Test::FindReferenceProjectDir().parent_path();
    auto slurp = [](const std::filesystem::path& p)
    {
        std::ifstream in(p);
        REQUIRE(in);
        std::stringstream ss; ss << in.rdbuf(); return ss.str();
    };
    const std::string attributes = slurp(root / "ThirdParty/Astra/include/Astra/Reflection/Attribute.hpp");
    const std::string facade     = slurp(root / "ArcaneCore/src/Arcane/Reflection.hpp");

    std::set<std::string> astra;
    const std::regex decl(R"(struct\s+(\w+)\s*:\s*AttributeBase<)");
    for (std::sregex_iterator it(attributes.begin(), attributes.end(), decl), end; it != end; ++it)
        astra.insert((*it)[1].str());
    REQUIRE(astra.size() >= 15);

    for (const std::string& name : astra)
    {
        INFO("Astra attribute without an Arcane::Attr alias: " << name);
        CHECK(facade.find("using ::Astra::" + name + ";") != std::string::npos);
    }
}
```

**Check before running:** read `Astra/Reflection/FieldInfo.hpp` for the real accessor names (`fields`, `name`, `HasAttribute<T>` or `GetAttribute<T>` / `ForEachAttribute<T>`), and how `Astra::GetMeta` is keyed. `ReflectionJson.hpp:587-597` shows `field.name` and `field.ForEachAttribute<Astra::AliasName>`. Write the metadata checks with the real API; if `HasAttribute` does not exist, use `ForEachAttribute` with a counter.

- [ ] **Step 2:** Run `.\ArcaneTests.exe "[reflection]"` after premake and a build. Expected: compile FAIL: `Arcane/Reflection.hpp` is not found.

- [ ] **Step 3: Create `ArcaneCore/src/Arcane/Reflection.hpp`:**

```cpp
#pragma once

// The Arcane:: reflection facade (input-seam spec 2026-10-02 s6.1). Game code
// reflects its components with ARCANE_REFLECT_* and never spells ASTRA_*:
//
//     ARCANE_REFLECT_TYPE(Health)
//         ARCANE_REFLECT_FIELD(Health, current)
//             ARCANE_REFLECT_ATTR(Range, 0.0f, 100.0f)
//     ARCANE_END_REFLECT_TYPE()
//
// The macros forward to Astra's, so the registered metadata is identical.
// ATTRIBUTES live in Arcane::Attr, not Arcane:: -- Arcane::Hidden is already
// the Outliner-eye tag component (Scene/Components.hpp), serialized by name.
// ARCANE_REFLECT_ATTR qualifies for you: ARCANE_REFLECT_ATTR(Hidden). An
// AngleFormat argument is Arcane::Attr::AngleFormat::Unit::Degrees.

// ARCANE_INTERNAL_BEGIN: the facade's library side
#include <Astra/Reflection/Attribute.hpp>
#include <Astra/Reflection/Reflection.hpp>

namespace Arcane::Attr
{
    using ::Astra::Range;
    using ::Astra::Hidden;
    using ::Astra::ReadOnly;
    using ::Astra::DisplayName;
    using ::Astra::Tooltip;
    using ::Astra::Category;
    using ::Astra::Serializable;
    using ::Astra::ColorFormat;
    using ::Astra::AngleFormat;
    using ::Astra::Multiline;
    using ::Astra::FilePath;
    using ::Astra::DragSpeed;
    using ::Astra::Deprecated;
    using ::Astra::AliasName;
    using ::Astra::Precision;
}

#define ARCANE_REFLECT_TYPE(Type)                    ASTRA_REFLECT_TYPE(Type)
#define ARCANE_REFLECT_FIELD(Type, FieldName)        ASTRA_REFLECT_FIELD(Type, FieldName)
#define ARCANE_REFLECT_ATTR(AttrType, ...)           .Attr<::Arcane::Attr::AttrType>(__VA_ARGS__)
#define ARCANE_REFLECT_TYPE_ATTR(AttrType, ...)      ; _astra_builder_.TypeAttr<::Arcane::Attr::AttrType>(__VA_ARGS__)
#define ARCANE_REFLECT_TYPE_END()                    ASTRA_REFLECT_TYPE_END()
#define ARCANE_END_REFLECT_TYPE()                    ASTRA_END_REFLECT_TYPE()
#define ARCANE_REFLECT_ENUM(EnumType)                ASTRA_REFLECT_ENUM(EnumType)
#define ARCANE_REFLECT_ENUM_VALUE(EnumType, Value)   ASTRA_REFLECT_ENUM_VALUE(EnumType, Value)
#define ARCANE_REFLECT_ENUM_VALUE_NAMED(EnumType, Value, DisplayName) \
    ASTRA_REFLECT_ENUM_VALUE_NAMED(EnumType, Value, DisplayName)
#define ARCANE_REFLECT_ENUM_VALUE_FULL(EnumType, Value, DisplayName, Description) \
    ASTRA_REFLECT_ENUM_VALUE_FULL(EnumType, Value, DisplayName, Description)
#define ARCANE_REFLECT_ENUM_FLAGS()                  ASTRA_REFLECT_ENUM_FLAGS()
#define ARCANE_REFLECT_ENUM_END()                    ASTRA_REFLECT_ENUM_END()
#define ARCANE_END_REFLECT_ENUM()                    ASTRA_END_REFLECT_ENUM()

// Opt a component into Astra's per-component change tracking (Changed<T>
// queries). Write it inside the struct body.
#define ARCANE_CHANGE_TRACKED static constexpr bool AstraChangeTracked = true;
// ARCANE_INTERNAL_END
```

**Verify against the macro bodies before committing:**
- `ARCANE_REFLECT_TYPE_ATTR` must mirror `ASTRA_REFLECT_TYPE_ATTR` exactly except for the namespace (`Astra/Reflection/Macros.hpp:101-102`). If the builder variable has another name there, copy it.
- `ASTRA_REFLECT_ENUM_VALUE`'s parameter list (`Macros.hpp:136-160`) must match each forwarding macro's.

- [ ] **Step 4: Build and run tests.** Run `.\ArcaneTests.exe "[reflection]"` and `"[facade]"`. Expected: PASS.
- [ ] **Step 5: Commit.**

```bash
git add ArcaneCore/src/Arcane/Reflection.hpp ArcaneTests/src/ReflectionFacadeTest.cpp
git commit -m "feat(core): the Arcane:: reflection facade -- ARCANE_REFLECT_* forward to Astra's, attributes aliased in Arcane::Attr (Arcane::Hidden is the tag component), ARCANE_CHANGE_TRACKED; a source scan keeps the alias list complete" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 12: Rewrite the controller — parameter style, `ReferenceGame` init-only, four fields gone, `PhysicsCommands` deleted

**Files:**
- Modify: `ReferenceProject/Source/Game/PlayerController2DSystem.hpp` (whole file), `PlayerController2D.hpp` (whole file), `ReferenceGame.cpp` (whole file)
- Delete: `ArcaneCore/src/Arcane/Scene/PhysicsCommands.hpp`, `ArcaneCore/src/Arcane/Scene/PhysicsCommands.cpp`
- Modify: `ArcaneCore/src/Arcane/Plugin/GameModule.hpp:85`, the `OnFixedUpdate` comment.

**Interfaces:**
- Consumes:
  - `Arcane::Time` (T4); `Arcane::GameInput`, `Arcane::ActionRef` (T6);
  - parameter registration (T8); `Arcane::Physics2D` (T9);
  - `Arcane/Ecs.hpp` (T10); `Arcane/Reflection.hpp` (T11).
- Produces: ReferenceProject in the target shape (spec s5.4). **The Task 1 `[trajectory]` test passes UNCHANGED.**

- [ ] **Step 1: Confirm the gate is green on the old sources.** Run `.\ArcaneTests.exe "[trajectory]"`. Expected: PASS.

- [ ] **Step 2: Replace `PlayerController2D.hpp`:**

```cpp
#pragma once

// PlayerController2D: a component -- plain data on an entity. Reflected so the editor's
// Inspector can show and edit it, scenes can save it, and the Add Component
// catalog can offer it. Registered with the game module by the
// ARCANE_COMPONENT line in PlayerController2D.cpp; nothing else to wire.
//
// Authored tuning plus the controller's own per-body state. Input and time are
// NOT copied in here: PlayerController2DSystem reads them as resources.

#include <Arcane/Reflection.hpp>

namespace ReferenceProject
{
    struct PlayerController2D
    {
        float moveSpeed = 5.0f;           // m/s, not pixels per second
        float jumpSpeed = 5.0f;           // m/s upward
        float groundAcceleration = 45.0f; // m/s^2
        float groundBraking = 55.0f;
        float turnAcceleration = 60.0f;
        float airAcceleration = 25.0f;
        float coyoteTime = 0.10f;         // seconds after leaving a ledge
        float jumpBufferTime = 0.10f;     // seconds before landing
        float jumpCutMultiplier = 0.45f;
        float coyoteRemaining = 0.0f;
        float jumpBufferRemaining = 0.0f;
        bool jumpConsumed = false;
        bool jumpCutArmed = false;
    };

    ARCANE_REFLECT_TYPE(PlayerController2D)
        ARCANE_REFLECT_FIELD(PlayerController2D, moveSpeed)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpSpeed)
        ARCANE_REFLECT_FIELD(PlayerController2D, groundAcceleration)
        ARCANE_REFLECT_FIELD(PlayerController2D, groundBraking)
        ARCANE_REFLECT_FIELD(PlayerController2D, turnAcceleration)
        ARCANE_REFLECT_FIELD(PlayerController2D, airAcceleration)
        ARCANE_REFLECT_FIELD(PlayerController2D, coyoteTime)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpBufferTime)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpCutMultiplier)
        ARCANE_REFLECT_FIELD(PlayerController2D, coyoteRemaining)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpBufferRemaining)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpConsumed)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
        ARCANE_REFLECT_FIELD(PlayerController2D, jumpCutArmed)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
    ARCANE_END_REFLECT_TYPE()
}
```

- [ ] **Step 3: Replace `PlayerController2DSystem.hpp`.** Keep the movement maths line for line: the same operations in the same order on the same float values. That is what keeps the trajectory bit-identical.

```cpp
#pragma once

// PlayerController2DSystem: a system -- a functor the scheduler runs each fixed
// step. Its PARAMETERS say what it touches, so the scheduler can order and
// parallelise it: the controller + body view, the sim clock, gameplay input and
// the physics commands. The one trait is ordering: it moves the body before
// PhysicsSystem steps.
//
// PlayerController2DSystem.cpp declares the phase and role with ARCANE_SYSTEM;
// the game-module prologue discovers it. It reads locally resolved gameplay
// actions, so it runs on the client role; an authoritative network game would
// route commands to a server system.

#include <Arcane/Ecs.hpp>
#include <Arcane/Input/GameInput.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>

#include "PlayerController2D.hpp"

#include <algorithm>

namespace ReferenceProject
{
    struct PlayerController2DSystem : Arcane::SystemTraits<Arcane::Before<Arcane::PhysicsSystem>>
    {
        Arcane::ActionRef move{"Player", "Move"};
        Arcane::ActionRef jump{"Player", "Jump"};

        static float MoveTowards(float current, float target, float distance)
        {
            if (current < target)
            {
                return std::min(current + distance, target);
            }
            return std::max(current - distance, target);
        }

        void operator()(Arcane::View<PlayerController2D, Arcane::RigidBody2D>& view,
                        Arcane::Res<Arcane::Time> time,
                        Arcane::Res<Arcane::GameInput> input,
                        Arcane::ResMut<Arcane::Physics2D> physics)
        {
            const float fixedDt  = static_cast<float>(time->fixedDt);
            const float axis     = input->Value(move).scalar;
            const bool  jumped   = input->PressedThisFixedStep(jump);
            const bool  jumpHeld = input->Down(jump);

            view.ForEach([&](Arcane::Entity entity, PlayerController2D& controller, Arcane::RigidBody2D& body)
            {
                const float dt = std::clamp(fixedDt, 0.0f, 0.05f);
                const Arcane::BodyMotion2D motion = physics->Motion(entity, body);
                if (motion.supported)
                {
                    controller.coyoteRemaining = std::max(0.0f, controller.coyoteTime);
                    controller.jumpConsumed = false;
                    controller.jumpCutArmed = false;
                }
                if (jumped)
                    controller.jumpBufferRemaining = std::max(dt, controller.jumpBufferTime);

                const float inputX = std::clamp(axis, -1.0f, 1.0f);
                const float targetX = inputX * std::max(0.0f, controller.moveSpeed);
                // Braking and turns get their own rates so A/D responds quickly
                // without making midair direction changes feel identical to ground.
                float rate = motion.supported ? controller.groundAcceleration : controller.airAcceleration;
                if (motion.supported && inputX == 0.0f)
                {
                    rate = controller.groundBraking;
                }
                else if (motion.supported && motion.velocityX * targetX < 0.0f)
                {
                    rate = controller.turnAcceleration;
                }
                const float nextX = MoveTowards(motion.velocityX, targetX,
                                               std::max(0.0f, rate) * dt);
                float nextY = motion.velocityY;

                if (motion.bodyReady && controller.jumpBufferRemaining > 0.0f && controller.coyoteRemaining > 0.0f && !controller.jumpConsumed)
                {
                    nextY = std::max(0.0f, controller.jumpSpeed);
                    controller.jumpBufferRemaining = 0.0f;
                    controller.coyoteRemaining = 0.0f;
                    controller.jumpConsumed = true;
                    controller.jumpCutArmed = true;
                }
                // Releasing W/Space during ascent cuts the upward velocity once.
                // Gravity then finishes the short hop naturally; holding gives
                // the full arc without injecting extra force each frame.
                if (controller.jumpCutArmed && !jumpHeld && nextY > 0.0f)
                {
                    nextY *= std::clamp(controller.jumpCutMultiplier, 0.0f, 1.0f);
                    controller.jumpCutArmed = false;
                }
                if (nextY <= 0.0f)
                {
                    controller.jumpCutArmed = false;
                }

                // Physics2D handles both live and not-yet-minted bodies.
                physics->SetVelocity(entity, body, nextX, nextY);
                if (!motion.supported)
                {
                    controller.coyoteRemaining = std::max(0.0f, controller.coyoteRemaining - dt);
                }
                controller.jumpBufferRemaining = std::max(0.0f, controller.jumpBufferRemaining - dt);
            });
        }
    };
}
```

Compare with the old body, line by line:
- `controller.fixedDt` → `fixedDt` (the same float: `static_cast<float>(dt)` of the same `1/60` double);
- `controller.value` → `axis`;
- `controller.jumpRequested` → `jumped`;
- `controller.jumpHeld` → `jumpHeld`;
- `GetBodyMotion2D(reg, entity)` → `physics->Motion(entity, body)`;
- `SetBodyVelocity2D(reg, ...)` → `physics->SetVelocity(entity, body, ...)`;
- the trailing `controller.jumpRequested = false;` goes, because there is no copied pulse to clear.

- [ ] **Step 4: Replace `ReferenceGame.cpp`:**

```cpp
#include <Arcane/Plugin/GameModule.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Base/Log.hpp>

namespace ReferenceGame
{
    // The module's only job: refuse to load without the two actions the player
    // controller needs. Input and time reach the controller as resources
    // (PlayerController2DSystem), never copied in here.
    struct Module final : Arcane::GameModule
    {
        bool OnInit(Arcane::EngineContext&) override
        {
            if (!Client()) return true; // server has no local input device
            if (!Client()->GameInput().FindAction("Player", "Move") ||
                !Client()->GameInput().FindAction("Player", "Jump"))
            {
                ARC_ERROR("ReferenceGame: Player.Move and Player.Jump are required in the selected gameplay input asset");
                return false;
            }
            return true;
        }
    };
}

ARCANE_GAME_MODULE(ReferenceGame::Module)
```

- [ ] **Step 5: Delete the forwarders.**
  - Run `git rm ArcaneCore/src/Arcane/Scene/PhysicsCommands.hpp ArcaneCore/src/Arcane/Scene/PhysicsCommands.cpp`.
  - Then `rg -n "PhysicsCommands|GetBodyMotion2D|SetBodyVelocity2D" --glob "*.{hpp,cpp,lua}"` must find only history comments in `PluginABI.hpp`. Leave those as they are: they are the ABI log.

- [ ] **Step 6: Update `GameModule.hpp:85`'s hook comment.** Above `virtual void OnFixedUpdate(double dt)`:

```cpp
        // Runs each fixed step before the fixedUpdate scheduler. Gameplay belongs
        // in SYSTEMS, which read time and input as resources (Arcane::Res<Arcane::
        // Time>, Arcane::Res<Arcane::GameInput>) -- never copy values into
        // components from here (input-seam spec 2026-10-02).
```

- [ ] **Step 7: Generate, build Debug, run THE GATE.**
  - Run: `ThirdParty\premake5\premake5.exe vs2026` (files were deleted), build, then `.\ArcaneTests.exe "[trajectory]"`.
  - Expected: **PASS, bit-identical, with the test file and fixture untouched.**
  - **If it differs, do NOT re-record.** Find the arithmetic or order divergence (dt float, the clamp order, `Motion` vs the old lookup) and fix the code.
  - Then run `"~[gpu]~[shell]"`.
- [ ] **Step 8: Build Release** and run `"[trajectory]"`. Expected: PASS.
- [ ] **Step 9: Build the real module through arcbuild, both configs, ending on Debug:**

```bat
bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe build --project ReferenceProject --config Release
bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe build --project ReferenceProject --config Debug
```

Expected: both succeed.

- [ ] **Step 10: Commit.**

```bash
git add ReferenceProject/Source/Game/PlayerController2D.hpp ReferenceProject/Source/Game/PlayerController2DSystem.hpp ReferenceProject/Source/Game/ReferenceGame.cpp ArcaneCore/src/Arcane/Plugin/GameModule.hpp
git commit -m "refactor(game): ReferenceProject's controller reads Time, GameInput and Physics2D as resources (parameter-style system) -- ReferenceGame's per-step copy and PlayerController2D's four copied fields (incl. the serialized live 'value') are gone; PhysicsCommands deleted; the recorded trajectory is bit-identical" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

The `git rm` from Step 5 is already staged.

---

### Task 13: Scenes — an unknown key warns once; `physics.arcscene` drops `"value"`

**Files:**
- Modify: `ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp`:
  - `Find` (:585-599) records consumed keys;
  - add `UnconsumedKeys()` beside `HasError()` (:575).
- Modify: `ArcaneCore/src/Arcane/Serialization/SceneSerializer.hpp`: `Detail::AddComponentByTypeName` (~:354-378) warns once per (type, key).
- Modify: `ReferenceProject/Content/scenes/physics.arcscene:396`. Remove the `"value": 0.0` line and the trailing comma on `"turnAcceleration": 60.0,`.
- Create: `ArcaneTests/src/SceneUnknownFieldTest.cpp`

**Interfaces:**
- **Today's behaviour (verified):** the JSON reader looks keys up by field name only (`ReflectionJson.hpp:587`), so an unknown key is ALREADY silently ignored, and old scenes already load.
- **This task adds the warning** spec s7 requires.
- Produces:
  - `std::vector<std::string> ReflectionJsonReader::UnconsumedKeys() const`;
  - `bool Arcane::Scene::Detail::NoteUnknownField(std::string_view type, std::string_view key)`, which returns true the FIRST time for a pair, process-wide.

- [ ] **Step 1: Write the failing test.** Create `ArcaneTests/src/SceneUnknownFieldTest.cpp`:

```cpp
// Input-seam spec 2026-10-02 s5.5 / s7: a scene carrying a field its type no
// longer reflects still loads, keeps every other value, warns ONCE per type and
// field, and a re-save drops the stale key (Review Focus #4: a scene saved by
// the old build re-opened and re-saved by the new one).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Serialization/ReflectionJson.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Json.hpp>

TEST_CASE("NoteUnknownField is true once per type and key", "[scene][serialization]")
{
    CHECK(Arcane::Scene::Detail::NoteUnknownField("Probe::TypeA", "legacyKey"));
    CHECK_FALSE(Arcane::Scene::Detail::NoteUnknownField("Probe::TypeA", "legacyKey"));
    CHECK(Arcane::Scene::Detail::NoteUnknownField("Probe::TypeA", "otherKey"));
    CHECK(Arcane::Scene::Detail::NoteUnknownField("Probe::TypeB", "legacyKey"));
}

TEST_CASE("ReflectionJsonReader reports the keys it never consumed", "[scene][serialization]")
{
    Arcane::RigidBody2D body;
    const nlohmann::json fields = { { "mass", 2.5 }, { "legacyKey", 1 } };
    Arcane::ReflectionJsonReader reader(fields);
    Astra::GetMeta(Astra::TypeID<Arcane::RigidBody2D>::Hash())->VisitFields(&body, reader);
    CHECK_FALSE(reader.HasError());
    CHECK(body.mass == 2.5f);
    REQUIRE(reader.UnconsumedKeys() == std::vector<std::string>{ "legacyKey" });
}

TEST_CASE("an old-build scene with a removed field loads, keeps its values, and re-saves without the key",
          "[scene][serialization]")
{
    Astra::Registry authored;
    Arcane::RegisterSceneComponents(authored);
    const Astra::Entity e = authored.CreateEntity();
    Arcane::RigidBody2D rb;
    rb.mass = 3.25f;
    authored.AddComponent<Arcane::RigidBody2D>(e, rb);
    nlohmann::json doc = Arcane::Scene::SaveJson(authored);

    // Simulate the OLD build: a key this build no longer reflects.
    bool injected = false;
    for (auto& entity : doc["entities"])
        if (entity["components"].contains("Arcane::RigidBody2D"))
        {
            entity["components"]["Arcane::RigidBody2D"]["staleFromOldBuild"] = 0.0;
            injected = true;
        }
    REQUIRE(injected);

    Astra::Registry loaded;
    Arcane::RegisterSceneComponents(loaded);
    REQUIRE(Arcane::Scene::LoadJson(loaded, doc));
    float mass = 0.0f;
    loaded.CreateView<Arcane::RigidBody2D>().ForEach([&](Astra::Entity, Arcane::RigidBody2D& b) { mass = b.mass; });
    CHECK(mass == 3.25f);

    const nlohmann::json resaved = Arcane::Scene::SaveJson(loaded);
    for (const auto& entity : resaved["entities"])
        if (entity["components"].contains("Arcane::RigidBody2D"))
            CHECK_FALSE(entity["components"]["Arcane::RigidBody2D"].contains("staleFromOldBuild"));
}
```

**Check before running:**
- The real "visit a type's fields with a reader" call. `SceneSerializer.hpp:366-367` uses `desc->visitFields(buf, reader)` through a `ComponentDescriptor`. Mirror whatever path compiles: getting the descriptor from `RegisterSceneComponents`' registry is the safest. Replace the `GetMeta(...)->VisitFields` line accordingly.
- That `RegisterSceneComponents` covers `RigidBody2D`. Otherwise add `Arcane::RegisterPhysicsComponents(reg)` (`PhysicsComponents.hpp:281`).

- [ ] **Step 2:** Run `.\ArcaneTests.exe "[serialization]"`. Expected: compile FAIL: `NoteUnknownField` and `UnconsumedKeys` are not members.

- [ ] **Step 3: Implement the reader side.** In `ReflectionJsonReader`:
  - add a member `mutable std::unordered_set<std::string> m_consumed;` (include `<unordered_set>`);
  - in `Find`, insert the key that matched: `m_consumed.insert(std::string(field.name));` on a direct hit, and `m_consumed.insert(std::string(a.name));` on an alias hit;
  - add the public accessor:

```cpp
        // Top-level keys of the input object no reflected field (or AliasName)
        // read -- a field removed or renamed since the file was written. Not an
        // error: absent-vs-present tolerance is the forward/back-compat story;
        // the scene loader warns once per type and key (input-seam spec s7).
        ASTRA_NODISCARD std::vector<std::string> UnconsumedKeys() const
        {
            std::vector<std::string> out;
            if (!m_in.is_object()) return out;
            for (auto it = m_in.begin(); it != m_in.end(); ++it)
                if (!m_consumed.contains(it.key())) out.push_back(it.key());
            return out;
        }
```

If the reader visits a field WITHOUT calling `Find` (for example it skips `Serializable(false)` fields before lookup), an old file that carried such a key will be reported as unconsumed. That is acceptable: it only warns.

- [ ] **Step 4: Implement the loader side.** In `SceneSerializer.hpp`'s `namespace Detail`, before `AddComponentByTypeName`:

```cpp
        // True the FIRST time this process meets (type, key) as an unknown
        // field, so a scene loaded every frame of an editor session warns once.
        inline bool NoteUnknownField(std::string_view type, std::string_view key)
        {
            static std::mutex m;
            static std::set<std::pair<std::string, std::string>> seen;
            std::lock_guard lock(m);
            return seen.emplace(std::string(type), std::string(key)).second;
        }
```

Include `<mutex>`, `<set>`, `<utility>`. In `AddComponentByTypeName`, right after `desc->visitFields(buf, reader);`:

```cpp
                for (const std::string& key : reader.UnconsumedKeys())
                    if (NoteUnknownField(typeName, key))
                        ARC_WARN("scene load: \"{}\" has no field \"{}\" -- its value is ignored and a re-save "
                                 "drops it (a field removed or renamed; an AliasName keeps a rename)",
                                 typeName, key);
```

- [ ] **Step 5: Fix the shipped scene.** Edit `ReferenceProject/Content/scenes/physics.arcscene`: in the `ReferenceProject::PlayerController2D` block, delete `"value": 0.0` and the comma before it. Then check that no other ReferenceProject scene carries a now-unknown key: run `.\ArcaneTests.exe "[scene]"` with the warning enabled, plus a headless runtime boot (`ArcaneRuntime --project ReferenceProject --headless --frames 5` from the Debug runtime dir), and read the log for `has no field`.
  - **Any hit in a ReferenceProject scene is fixed in the scene file in this task.** Never silence the warning.

- [ ] **Step 6: Run the tests.** Run `.\ArcaneTests.exe "[serialization]"`, `"[scene]"`, `"[trajectory]"` and `"~[gpu]~[shell]"`. Expected: PASS.
  - `[trajectory]` loads `physics.arcscene` from source, and the removed key was never read, so it stays bit-identical.
- [ ] **Step 7: Commit.**

```bash
git add ArcaneCore/src/Arcane/Serialization/ReflectionJson.hpp ArcaneCore/src/Arcane/Serialization/SceneSerializer.hpp ReferenceProject/Content/scenes/physics.arcscene ArcaneTests/src/SceneUnknownFieldTest.cpp
git commit -m "feat(core): a scene field its type no longer reflects warns once per type and key (still loads, re-save drops it); physics.arcscene loses PlayerController2D's stale 'value'" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 14: Class templates emit the parameter style and `Arcane::` names; a compiled smoke plugin pins them

**Files:**
- Modify: `ArcaneEditor/src/Project/ClassTemplates.cpp:130-221` (`kComponentHeader`, `kFixedUpdateSystemHeader`, `kUnanchoredSystemHeader`)
- Create: `ArcaneTests/plugins/TemplateSmoke/SmokeComponent.hpp`, `SmokeComponent.cpp`, `SmokeSystem.hpp`, `SmokeSystem.cpp`, `SmokeModule.cpp`. These are checked-in renders.
- Modify: `premake5.lua`: a `TemplateSmokePlugin` project, plus an ArcaneTests `dependson` and post-build copy, the same shape as Task 1's.
- Modify: `ArcaneTests/src/ClassTemplatesTest.cpp`

**Interfaces:**
- Produces:
  - **`Render(Kind::Component, ...)`:** a header including `<Arcane/Reflection.hpp>`, using `ARCANE_REFLECT_TYPE` / `ARCANE_REFLECT_FIELD` / `ARCANE_END_REFLECT_TYPE`.
  - **`Render(Kind::System, ...)`:** a parameter-style functor taking `Arcane::Res<Arcane::Time> time`, deriving `Arcane::SystemTraits<Arcane::Before<Arcane::TransformPropagationSystem>>` (FixedUpdate) or with no traits base (Update/Render), and including `<Arcane/Ecs.hpp>`.
  - **The smoke plugin** compiles the renders for (Component `SmokeComponent`, System `SmokeSystem`, project `TemplateSmoke`), so a template change that does not compile fails the build.
- **Decision (spec s8 T8):** `ClassTemplatesTest` never compiled generated code. The compile proof is therefore the checked-in renders, built as a real module, plus a test that asserts `Render(...)` still equals those files byte for byte. A drift fails one or the other.

- [ ] **Step 1: Update `ClassTemplatesTest.cpp`'s expectations first.**
  - **Component case:** replace the `Astra/Reflection/Reflection.hpp` / `ASTRA_REFLECT_TYPE(Health)` / `ASTRA_END_REFLECT_TYPE()` checks with `#include <Arcane/Reflection.hpp>` / `ARCANE_REFLECT_TYPE(Health)` / `ARCANE_END_REFLECT_TYPE()`, and add `CHECK_FALSE(Has(r.header, "ASTRA_"));`.
  - **System case:** replace the Astra include/trait/operator checks with:

```cpp
    CHECK(Has(r.header, "#include <Arcane/Ecs.hpp>"));
    CHECK(Has(r.header, "Arcane::SystemTraits<Arcane::Before<Arcane::TransformPropagationSystem>>"));
    CHECK(Has(r.header, "void operator()(Arcane::Res<Arcane::Time> time)"));
    CHECK_FALSE(Has(r.header, "Astra::"));
    CHECK_FALSE(Has(r.header, "Registry& reg"));
```

  - **"maps every phase and role" case:** change the anchor expectation to `"Arcane::Before<Arcane::TransformPropagationSystem>"`.
  - **Append:**

```cpp
#include "Helpers/ReferenceProjectDir.hpp"
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>
#include <Arcane/Sim/Time.hpp>
#include "Helpers/TestTypeContext.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
    std::string Slurp(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf(); return ss.str();
    }
    std::filesystem::path SmokeDir()
    {
        return Arcane::Test::FindReferenceProjectDir().parent_path() / "ArcaneTests" / "plugins" / "TemplateSmoke";
    }
}

TEST_CASE("ClassTemplates renders equal the compiled TemplateSmoke sources byte for byte", "[editor][templates]")
{
    const auto component = ClassTemplates::Render(ClassTemplates::Kind::Component, "SmokeComponent", "TemplateSmoke");
    const auto system    = ClassTemplates::Render(ClassTemplates::Kind::System, "SmokeSystem", "TemplateSmoke");
    CHECK(Slurp(SmokeDir() / "SmokeComponent.hpp") == component.header);
    CHECK(Slurp(SmokeDir() / "SmokeComponent.cpp") == component.source);
    CHECK(Slurp(SmokeDir() / "SmokeSystem.hpp")    == system.header);
    CHECK(Slurp(SmokeDir() / "SmokeSystem.cpp")    == system.source);
}

TEST_CASE("the rendered component and parameter-style system load as a module and the system runs", "[editor][templates][hotreload]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("TemplateSmokePlugin.dll"));
    REQUIRE(host.AttachRuntime(rt));
    REQUIRE(host.Load());
    // The rendered system registered through ARCANE_SYSTEM's PARAMETER path...
    bool registered = false;
    for (const Arcane::SystemFactoryEntry& e : Arcane::Test::Process().SystemFactories().Entries())
        if (e.name.find("SmokeSystem") != std::string::npos) registered = true;
    CHECK(registered);
    // ...and runs: its only parameter is Res<Time>, which RunLoop publishes, so
    // a skip would log "param-system skipped" and a crash would end the test.
    for (int i = 0; i < 3; ++i)
        rt.Loop().Advance(1.0 / 60.0, [&](double dt) { host.FixedUpdateAll(dt); }, [&](double dt, double a) { host.UpdateAll(dt, a); });
    CHECK(rt.Registry().GetResource<Arcane::Time>() != nullptr);
    CHECK(host.IsLoaded());
    host.Unload();
}
```

- [ ] **Step 2:** Run `.\ArcaneTests.exe "[templates]"`. Expected: FAIL (templates unchanged; smoke files missing).

- [ ] **Step 3: Rewrite the three template literals in `ClassTemplates.cpp`.**

`kComponentHeader`:

```cpp
        constexpr std::string_view kComponentHeader = R"(#pragma once

// {{CLASS}}: a component -- plain data on an entity. Reflected so the editor's
// Inspector can show and edit it, scenes can save it, and the Add Component
// catalog can offer it. Registered with the game module by the
// ARCANE_COMPONENT line in {{CLASS}}.cpp; nothing else to wire.

#include <Arcane/Reflection.hpp>

namespace {{NS}}
{
    struct {{CLASS}}
    {
        float value = 0.0f;
    };

    ARCANE_REFLECT_TYPE({{CLASS}})
        ARCANE_REFLECT_FIELD({{CLASS}}, value)
    ARCANE_END_REFLECT_TYPE()
}
)";
```

`kFixedUpdateSystemHeader`:

```cpp
        constexpr std::string_view kFixedUpdateSystemHeader = R"(#pragma once

// {{CLASS}}: a system -- a functor the scheduler runs each step. Its
// PARAMETERS say what it touches, so the scheduler can order and parallelise
// it: views over components and engine resources such as the sim clock.
//
//     void operator()(Arcane::View<Arcane::Transform>& view,
//                     Arcane::Res<Arcane::Time> time,
//                     Arcane::Res<Arcane::GameInput> input)   // #include <Arcane/Input/GameInput.hpp>
//
// Fixed-update systems run before transform propagation by default so gameplay
// can move local transforms first. Registrar discovery order is irrelevant:
// scheduler order is expressed only through Before<> and After<> traits.
// The ARCANE_SYSTEM declaration that selects phase and network role is in
// {{CLASS}}.cpp.

#include <Arcane/Ecs.hpp>
#include <Arcane/Scene/TransformSystems.hpp>   // the placement anchor

namespace {{NS}}
{
    struct {{CLASS}} : Arcane::SystemTraits<Arcane::Before<Arcane::TransformPropagationSystem>>
    {
        void operator()(Arcane::Res<Arcane::Time> time)
        {
            (void)time;
        }
    };
}
)";
```

`kUnanchoredSystemHeader`:

```cpp
        constexpr std::string_view kUnanchoredSystemHeader = R"(#pragma once

// {{CLASS}}: a system -- a functor the scheduler runs each step. Its
// PARAMETERS say what it touches, so the scheduler can order and parallelise
// it: views over components and engine resources such as the sim clock.
//
//     void operator()(Arcane::View<Arcane::Transform>& view,
//                     Arcane::Res<Arcane::Time> time)
//
// Fixed-step transform propagation is not installed in the Update or Render
// scheduler, so this template invents no irrelevant edge. Registrar discovery
// order is irrelevant: derive Arcane::SystemTraits<Arcane::Before<...>> or
// After<...> whenever scheduler order matters. The ARCANE_SYSTEM declaration
// that selects phase and network role is in {{CLASS}}.cpp.

#include <Arcane/Ecs.hpp>

namespace {{NS}}
{
    struct {{CLASS}}
    {
        void operator()(Arcane::Res<Arcane::Time> time)
        {
            (void)time;
        }
    };
}
)";
```

`kSystemSource` and `kComponentSource` are unchanged: they already spell only `Arcane::`.

- [ ] **Step 4: Create the checked-in renders.**
  - Generate `SmokeComponent.hpp/.cpp` and `SmokeSystem.hpp/.cpp` FROM the code by rendering: write a throwaway one-off print in a scratch test, or copy the strings with `{{CLASS}}` / `{{NS}}` / `{{ROLE}}` / `{{PHASE}}` substituted exactly as `Render` does, with Role Both and Phase FixedUpdate defaults.
  - Byte equality is asserted, so line endings must be LF. Add `ArcaneTests/plugins/TemplateSmoke/* -text` to `.gitattributes` if needed, so Windows checkout does not convert them.
  - Then add `SmokeModule.cpp`, which is NOT a render:

```cpp
// The TemplateSmoke module: the editor's rendered Component + System
// (ClassTemplatesTest asserts these files ARE the renders) compiled as a real
// game module, so a template that stops compiling fails the build.
#include <Arcane/Plugin/GameModule.hpp>

namespace TemplateSmoke
{
    struct Module final : Arcane::GameModule {};
}

ARCANE_GAME_MODULE(TemplateSmoke::Module)
```

- [ ] **Step 5: Add `TemplateSmokePlugin` to `premake5.lua`.**
  - Copy Task 1's `ReferenceGameUnderTest` block. Change the name to `TemplateSmokePlugin`, `files { "%{wks.location}/ArcaneTests/plugins/TemplateSmoke/**.cpp", "%{wks.location}/ArcaneTests/plugins/TemplateSmoke/**.hpp" }`, and the first include dir to `ArcaneTests/plugins/TemplateSmoke`.
  - Add it to ArcaneTests' `dependson` and post-build copies (`TemplateSmokePlugin.dll`).
- [ ] **Step 6: Generate, build Debug, run tests.** Run `.\ArcaneTests.exe "[templates]"`, `"[editor]"` and `"[trajectory]"`. Expected: PASS.
- [ ] **Step 7: Commit.**

```bash
git add ArcaneEditor/src/Project/ClassTemplates.cpp ArcaneTests/src/ClassTemplatesTest.cpp ArcaneTests/plugins/TemplateSmoke/SmokeComponent.hpp ArcaneTests/plugins/TemplateSmoke/SmokeComponent.cpp ArcaneTests/plugins/TemplateSmoke/SmokeSystem.hpp ArcaneTests/plugins/TemplateSmoke/SmokeSystem.cpp ArcaneTests/plugins/TemplateSmoke/SmokeModule.cpp premake5.lua
git commit -m "feat(editor): C++ class templates emit parameter-style systems (Res<Time>) and Arcane:: names only; the renders are checked in and compiled as TemplateSmokePlugin so a template that stops compiling fails the build" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

Also stage `.gitattributes` if Step 4 touched it.

---

### Task 15: Header sweep A — the Plugin, Base and Client game-facing headers

**Files (10):**
- `ArcaneCore/src/Arcane/Plugin/`: `GameModule.hpp`, `GameComponents.hpp`, `GameSystems.hpp`, `SystemFactory.hpp`, `PluginABI.hpp`;
- `ArcaneCore/src/Arcane/Base/`: `ProcessContext.hpp`, `Runtime.hpp`, `Assert.hpp`, `Log.hpp`;
- `ArcaneClient/src/Arcane/Client/ClientRuntime.hpp`.

Together with Task 16's five Scene headers, these ten are the spec's 15 surviving game-facing headers.

**Interfaces:**
- Consumes: `Arcane/EcsFwd.hpp`, `Arcane/Ecs.hpp` (T10).
- Produces: every PUBLIC declaration a module reads in these headers spells `Arcane::`. That covers member function signatures, return types, parameter types, data members of `ARCANE_API` structs a module touches (for example `EngineContext::typeContext`/`workScheduler`), and the doc examples in comments. Remaining library spellings are inside `ARCANE_INTERNAL_BEGIN/END` fences, comments, preprocessor lines or string literals.
- **The replacement table** (in declarations only):

| Library spelling | Arcane spelling |
|---|---|
| `Astra::Registry` | `::Arcane::Registry` inside a class with a `Registry()` member, else `Arcane::Registry` |
| `Astra::Entity` | `Arcane::Entity` |
| `Astra::ComponentModule` / `ComponentRegistry` / `TypeContext` | `Arcane::` same name |
| `Astra::BinaryWriter` / `BinaryReader` | `Arcane::` same name |
| `Astra::SystemScheduler` | `Arcane::SystemScheduler` |
| `Astra::Result<...>`, `Astra::SerializationError`, `Astra::Tick` | `Arcane::` same name |
| `Mosaic::IWorkScheduler` | `Arcane::IWorkScheduler` |
| `Astra::Before`/`After` in doc comments | `Arcane::Before`/`After` |

**Recipe per header:**
1. Where the header forward-declares Astra/Mosaic types today (`Runtime.hpp:36-37`, `PluginABI.hpp:12-13`, `ProcessContext.hpp:17`), replace the forward declarations with `#include <Arcane/EcsFwd.hpp>`. Where it includes full Astra headers already (`GameModule.hpp`, `GameComponents.hpp`), add `#include <Arcane/Ecs.hpp>`.
2. Rewrite the public declarations per the table.
3. **Fence what must stay.**
   - Macro bodies are preprocessor lines and are skipped automatically: `ARCANE_GAME_MODULE`, `ARCANE_COMPONENT`, `ARCANE_SYSTEM`.
   - Fence any NON-preprocessor implementation inside these headers that must name library internals. Examples: `GameModule`'s inline prologue bodies (`Astra::SetTypeContext`, `ModuleResidency`), `GameComponents`' drain, and `Assert.hpp`/`Log.hpp`'s `Mosaic::` installs. Each fence carries a one-line reason.
4. Check the result:

```bat
rg -n "Astra::|ASTRA_|Manifold2D|Mosaic::" <header>
```

Every hit must be in a comment, a `#` line or `#define` continuation, a string, or inside a fence.

- [ ] **Step 1:** Build Debug and run `"[trajectory]"` and `"~[gpu]~[shell]"` before starting. Expected: PASS (baseline).
- [ ] **Step 2:** Sweep `GameModule.hpp`, `GameComponents.hpp` and `GameSystems.hpp`. Build.
  - In `GameModule`, the members `Registry()`, `Components()`, `SceneRootEntity()`, `OnSaveState(Arcane::BinaryWriter&)` and `OnLoadState(Arcane::BinaryReader&)` change spelling only.
  - Existing overrides in modules still compile, because the types are identical.
- [ ] **Step 3:** Sweep `SystemFactory.hpp`, `PluginABI.hpp` and `ProcessContext.hpp`. Build.
  - In `PluginABI.hpp`, the `vNN` history comments are comments, so leave them verbatim. That file's ABI changes NOT: the types are identical.
- [ ] **Step 4:** Sweep `Runtime.hpp`, `ClientRuntime.hpp`, `Assert.hpp` and `Log.hpp`. Build Debug and Release.
- [ ] **Step 5: Verify behaviour is unchanged.** Run `.\ArcaneTests.exe "~[gpu]~[shell]"`, `"[witness]~[shell]"` and `"[trajectory]"`, then `arcbuild build --project ReferenceProject --config Debug`. Expected: PASS and build OK.
- [ ] **Step 6: Commit.**

```bash
git add <the ten headers by name>
git commit -m "refactor(core): game-facing Plugin/Base/Client headers spell Arcane:: in their public declarations (EcsFwd/Ecs aliases); library internals fenced ARCANE_INTERNAL (input-seam spec s6.2)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 16: Header sweep B — the Scene game-facing headers

**Files (5):** `ArcaneCore/src/Arcane/Scene/SceneResources.hpp`, `Components.hpp`, `TransformSystems.hpp`, `PhysicsComponents.hpp`, `PhysicsSystem.hpp`.

**Interfaces:**
- Consumes: `Arcane/Ecs.hpp`, `Arcane/Reflection.hpp` (T10, T11).
- Produces:
  - **Reflection blocks** in these headers use `ARCANE_REFLECT_*`. `ASTRA_REFLECT_ATTR(X, ...)` → `ARCANE_REFLECT_ATTR(X, ...)` and `Astra::AngleFormat::Unit::Degrees` → `Arcane::Attr::AngleFormat::Unit::Degrees`. The registered metadata is identical.
  - **Engine systems' traits** spell `Arcane::SystemTraits<Arcane::Reads<...>, Arcane::Writes<...>, Arcane::Before<...>>`.
  - `static constexpr bool AstraChangeTracked = true;` → `ARCANE_CHANGE_TRACKED`.
  - **System bodies** (the `operator()` implementations: PhysicsSystem's passes, TransformPropagation) are fenced `ARCANE_INTERNAL`.
  - **The two Manifold2D alias lines** (`namespace Phys = Manifold2D::Physics;` at `PhysicsComponents.hpp:55` and `PhysicsSystem.hpp:97`) are fenced. They ARE the facade for Manifold2D, and games spell `Arcane::Phys::BodyType`.

- [ ] **Step 1:** Sweep `Components.hpp` and `SceneResources.hpp`.
  - Reflection blocks: a mechanical `ASTRA_REFLECT_` → `ARCANE_REFLECT_` replacement within those blocks, plus the `AngleFormat` argument.
  - Includes: `#include <Arcane/Reflection.hpp>` in place of `<Astra/Reflection/Reflection.hpp>`.
  - Build, then run `.\ArcaneTests.exe "[scene]"` and `"[serialization]"`. Expected: PASS. The JSON round-trip tests prove the metadata is unchanged.
- [ ] **Step 2:** Sweep `TransformSystems.hpp`, `PhysicsComponents.hpp` and `PhysicsSystem.hpp`. Rewrite the trait spellings, the reflection blocks and `AstraChangeTracked`, fence the system bodies and the `Phys` alias lines, then build.
- [ ] **Step 3: Verify.**
  - Run `rg -n "Astra::|ASTRA_|Manifold2D|Mosaic::"` on each file. Every hit is in a comment, a `#` line, a string, or a fence.
  - Run `.\ArcaneTests.exe "~[gpu]~[shell]"`, `"[witness]~[shell]"` and `"[trajectory]"`, build Release, then `arcbuild build --project ReferenceProject --config Debug`. Expected: PASS.
- [ ] **Step 4: Commit.**

```bash
git add ArcaneCore/src/Arcane/Scene/SceneResources.hpp ArcaneCore/src/Arcane/Scene/Components.hpp ArcaneCore/src/Arcane/Scene/TransformSystems.hpp ArcaneCore/src/Arcane/Scene/PhysicsComponents.hpp ArcaneCore/src/Arcane/Scene/PhysicsSystem.hpp
git commit -m "refactor(core): game-facing Scene headers reflect with ARCANE_REFLECT_* and spell Arcane:: traits; system bodies and the Manifold2D alias fenced ARCANE_INTERNAL (input-seam spec s6.2)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 17: The `Arcane::`-only guard

**Files:**
- Create: `ArcaneTests/src/ArcaneSpellingGuardTest.cpp`

**Interfaces:**
- Consumes: `Arcane::Test::FindReferenceProjectDir()`, `ClassTemplates::Render`.
- Produces: the rule "game-facing code spells `Arcane::` only" (spec s6.3), stated beside the checked-file list.
- **Decision (spec s6.3 said "public declarations"):** "public declarations" is made mechanical as "every line not in a comment, string, preprocessor directive (including `#define` continuations), or `ARCANE_INTERNAL_BEGIN/END` fence". The fences mark the internal regions explicitly.

- [ ] **Step 1: Write the test.** Create `ArcaneTests/src/ArcaneSpellingGuardTest.cpp`:

```cpp
// THE RULE (input-seam spec 2026-10-02 s6.3): GAME-FACING CODE SPELLS
// Arcane:: ONLY. The standalone libraries keep their namespaces underneath;
// Arcane/Ecs.hpp, Arcane/EcsFwd.hpp and Arcane/Reflection.hpp re-export what
// game code needs. This test fails when a library spelling -- Astra::,
// ASTRA_*, Manifold2D, Mosaic:: -- appears in game-facing code OUTSIDE a
// comment, a string literal, a preprocessor directive (incl. #define
// continuations) or an `// ARCANE_INTERNAL_BEGIN: <why>` ... `// ARCANE_INTERNAL_END`
// fence.
//
// Game-facing = ReferenceProject's game sources, every editor C++ template
// render, and the 15 headers a game module reads (kGameFacingHeaders).

#include <catch2/catch_test_macros.hpp>

#include <Project/ClassTemplates.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "Helpers/ReferenceProjectDir.hpp"

namespace
{
    constexpr const char* kGameFacingHeaders[] = {
        "ArcaneCore/src/Arcane/Plugin/GameModule.hpp",
        "ArcaneCore/src/Arcane/Plugin/GameComponents.hpp",
        "ArcaneCore/src/Arcane/Plugin/GameSystems.hpp",
        "ArcaneCore/src/Arcane/Plugin/SystemFactory.hpp",
        "ArcaneCore/src/Arcane/Plugin/PluginABI.hpp",
        "ArcaneCore/src/Arcane/Base/ProcessContext.hpp",
        "ArcaneCore/src/Arcane/Base/Runtime.hpp",
        "ArcaneCore/src/Arcane/Base/Assert.hpp",
        "ArcaneCore/src/Arcane/Base/Log.hpp",
        "ArcaneClient/src/Arcane/Client/ClientRuntime.hpp",
        "ArcaneCore/src/Arcane/Scene/SceneResources.hpp",
        "ArcaneCore/src/Arcane/Scene/Components.hpp",
        "ArcaneCore/src/Arcane/Scene/TransformSystems.hpp",
        "ArcaneCore/src/Arcane/Scene/PhysicsComponents.hpp",
        "ArcaneCore/src/Arcane/Scene/PhysicsSystem.hpp",
    };

    struct Hit { std::string where; int line; std::string text; };

    // Line-oriented scan with comment/string/preprocessor/fence stripping.
    std::vector<Hit> Scan(const std::string& where, const std::string& text)
    {
        static const std::regex library(R"((\bAstra::|\bASTRA_[A-Z_]+|\bManifold2D\b|\bMosaic::))");
        std::vector<Hit> hits;
        std::istringstream in(text);
        std::string raw;
        int lineNo = 0;
        bool inBlock = false, inFence = false, inDefine = false;
        while (std::getline(in, raw))
        {
            ++lineNo;
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            if (raw.find("ARCANE_INTERNAL_BEGIN") != std::string::npos) { inFence = true;  continue; }
            if (raw.find("ARCANE_INTERNAL_END")   != std::string::npos) { inFence = false; continue; }
            if (inFence) continue;
            if (inDefine) { inDefine = !raw.empty() && raw.back() == '\\'; continue; }

            std::string code;
            for (std::size_t i = 0; i < raw.size(); ++i)
            {
                if (inBlock)
                {
                    if (raw.compare(i, 2, "*/") == 0) { inBlock = false; ++i; }
                    continue;
                }
                if (raw.compare(i, 2, "/*") == 0) { inBlock = true; ++i; continue; }
                if (raw.compare(i, 2, "//") == 0) break;
                if (raw[i] == '"')
                {
                    // R"( ... )" raw strings on one line, and ordinary strings
                    const bool rawStr = i > 0 && raw[i - 1] == 'R';
                    const std::string close = rawStr ? ")\"" : "\"";
                    std::size_t j = i + 1;
                    while (j < raw.size())
                    {
                        if (!rawStr && raw[j] == '\\') { j += 2; continue; }
                        if (raw.compare(j, close.size(), close) == 0) break;
                        ++j;
                    }
                    i = j + close.size() - 1;
                    code += "\"\"";
                    continue;
                }
                code += raw[i];
            }
            const auto first = code.find_first_not_of(" \t");
            if (first != std::string::npos && code[first] == '#')
            {
                inDefine = !raw.empty() && raw.back() == '\\';
                continue;
            }
            if (std::regex_search(code, library))
                hits.push_back({ where, lineNo, raw });
        }
        return hits;
    }

    std::string Slurp(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf(); return ss.str();
    }

    void Report(const std::vector<Hit>& hits)
    {
        for (const Hit& h : hits)
            UNSCOPED_INFO(h.where << ":" << h.line << ": " << h.text);
        CHECK(hits.empty());
    }
}

TEST_CASE("guard: the scanner skips comments, strings, preprocessor lines and fences", "[guard]")
{
    const std::string sample =
        "// Astra::Registry in a comment\n"
        "const char* s = \"Astra::Registry\";\n"
        "#include <Astra/Registry/Registry.hpp>\n"
        "#define M(x) \\\n"
        "    Astra::Thing(x)\n"
        "// ARCANE_INTERNAL_BEGIN: test\n"
        "Astra::Registry hidden;\n"
        "// ARCANE_INTERNAL_END\n"
        "Arcane::Registry fine;\n"
        "Astra::Registry leaked;\n";
    const auto hits = Scan("sample", sample);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].line == 10);
}

TEST_CASE("guard: ReferenceProject's game sources spell Arcane:: only", "[guard]")
{
    const auto root = Arcane::Test::FindReferenceProjectDir();
    REQUIRE_FALSE(root.empty());
    std::vector<Hit> hits;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "Source"))
    {
        const auto ext = entry.path().extension();
        if (ext != ".hpp" && ext != ".cpp") continue;
        auto h = Scan(entry.path().generic_string(), Slurp(entry.path()));
        hits.insert(hits.end(), h.begin(), h.end());
    }
    Report(hits);
}

TEST_CASE("guard: every editor C++ template render spells Arcane:: only", "[guard]")
{
    using namespace Arcane::Editor;
    std::vector<Hit> hits;
    auto scan = [&](const char* what, const ClassTemplates::Rendered& r)
    {
        auto a = Scan(std::string(what) + " header", r.header);
        auto b = Scan(std::string(what) + " source", r.source);
        hits.insert(hits.end(), a.begin(), a.end());
        hits.insert(hits.end(), b.begin(), b.end());
    };
    scan("component", ClassTemplates::Render(ClassTemplates::Kind::Component, "C", "P"));
    scan("plain", ClassTemplates::Render(ClassTemplates::Kind::PlainClass, "C", "P"));
    for (int phase = 0; phase < ClassTemplates::kSystemPhaseChoiceCount; ++phase)
        for (int role = 0; role < ClassTemplates::kSystemRoleChoiceCount; ++role)
            scan("system", ClassTemplates::Render(ClassTemplates::Kind::System, "S", "P",
                                                  ClassTemplates::SystemOptionsForChoiceIndices(phase, role)));
    Report(hits);
}

TEST_CASE("guard: the 15 game-facing engine headers spell Arcane:: outside comments, preprocessor lines and fences", "[guard]")
{
    const auto root = Arcane::Test::FindReferenceProjectDir().parent_path();
    std::vector<Hit> hits;
    for (const char* rel : kGameFacingHeaders)
    {
        const auto path = root / rel;
        INFO(path.generic_string());
        REQUIRE(std::filesystem::exists(path));
        auto h = Scan(rel, Slurp(path));
        hits.insert(hits.end(), h.begin(), h.end());
    }
    Report(hits);
}
```

**Check before running:** the `ClassTemplates::Kind` enumerator for the plain class (`ClassTemplatesTest.cpp:173` uses it). Use its real name.

- [ ] **Step 2: Generate, build, run.** Run `.\ArcaneTests.exe "[guard]"`. Expected: PASS, because Tasks 12, 14, 15 and 16 cleaned every surface.
  - **Any hit is fixed at the source:** rename it to the `Arcane::` alias, or fence it with a reason if it is genuinely internal. Never fix it by editing the scanner. In the report, name each hit and how you resolved it.
- [ ] **Step 3: Mutation check.** Temporarily add `Astra::Registry* probe = nullptr;` to `ReferenceProject/Source/Game/ReferenceGame.cpp`, build, run `"[guard]"`, and expect FAIL naming that line. Then revert the edit (`git checkout -- ReferenceProject/Source/Game/ReferenceGame.cpp`).
- [ ] **Step 4: Commit.**

```bash
git add ArcaneTests/src/ArcaneSpellingGuardTest.cpp
git commit -m "test(core): the Arcane::-only guard -- ReferenceProject's game sources, every C++ template render and the 15 game-facing headers carry no Astra/ASTRA_/Manifold2D/Mosaic spelling outside comments, strings, preprocessor lines and ARCANE_INTERNAL fences" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 18: Docs — README's gameplay-code section

**Files:**
- Modify: `README.md`:
  - the `ArcaneClient` row (:14) mentions the `Arcane::` facade;
  - a new subsection under "## Using the engine as an SDK" (:168), after the premake snippet.

**Interfaces:**
- Consumes: the final shapes of Tasks 4-14.
- The Aphelyon example (`D:\dev\starworks\Aphelyon\docs\examples\arcane-physics-example.cpp`) is NOT edited here. It is part of **Merge prep**.

- [ ] **Step 1: Add the subsection.** After the `arcane_game_module("MyGame")` code block and its following paragraph:

````markdown
### Writing gameplay code

Game code spells only `Arcane::` names. The ECS (Astra), the 2D physics
(Manifold2D) and the core library (Mosaic) are standalone libraries with
their own namespaces underneath; `<Arcane/Ecs.hpp>` and
`<Arcane/Reflection.hpp>` re-export everything a game module needs.

A **component** is reflected plain data:

```cpp
#include <Arcane/Reflection.hpp>

struct Health { float current = 100.0f; };
ARCANE_REFLECT_TYPE(Health)
    ARCANE_REFLECT_FIELD(Health, current)
        ARCANE_REFLECT_ATTR(Range, 0.0f, 100.0f)
ARCANE_END_REFLECT_TYPE()
```

A **system** declares what it touches as parameters -- component views and
engine resources -- and the scheduler orders and parallelises it from that:

```cpp
#include <Arcane/Ecs.hpp>
#include <Arcane/Input/GameInput.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>

struct Jumper : Arcane::SystemTraits<Arcane::Before<Arcane::PhysicsSystem>>
{
    Arcane::ActionRef jump{"Player", "Jump"};

    void operator()(Arcane::View<Arcane::RigidBody2D>& view,
                    Arcane::Res<Arcane::Time> time,          // fixedDt, fixedStep, elapsed, ...
                    Arcane::Res<Arcane::GameInput> input,    // actions from the project's input asset
                    Arcane::ResMut<Arcane::Physics2D> physics)
    {
        if (!input->PressedThisFixedStep(jump)) return;
        view.ForEach([&](Arcane::Entity e, Arcane::RigidBody2D& body)
        {
            const Arcane::BodyMotion2D m = physics->Motion(e, body);
            if (m.supported) physics->SetVelocity(e, body, m.velocityX, 6.0f);
        });
    }
};
```

and is registered with one line in its `.cpp`:
`ARCANE_SYSTEM(MyGame::Jumper, Arcane::RoleMask::Client, Arcane::SystemPhase::FixedUpdate)`.

- **Resources:** `Time` is present in every world. `GameInput` is present in every client world, and a server world has none. A system whose resource is missing is skipped with one log line.
- **Code outside a system** (a module's `OnUpdate`/`OnDrawUI`) reads the same data with `Registry().GetResource<Arcane::Time>()`. There are no global accessors: one process can hold several worlds (edit, Play, an embedded server, tests).
- **Templates:** the editor's *Create -> C++ Class* templates emit this shape.
````

- [ ] **Step 2: The table row.** In the `ArcaneClient` row (:14), after "ECS runtime (Astra)", add ", exposed to game code as the `Arcane::` facade".
- [ ] **Step 3: Commit.**

```bash
git add README.md
git commit -m "docs: README 'Writing gameplay code' -- components with ARCANE_REFLECT_*, parameter-style systems over Res<Time>/Res<GameInput>/ResMut<Physics2D>, Arcane:: only" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 19: GATE

**Files:**
- Modify: `scripts/automation-baselines.json` (count rise only)
- Create: `.superpowers/sdd/2026-10-02-input-time-resources/gate.md` (the record). Use whatever SDD record folder the controller names. If none is named, report the record in the task report instead.

**Interfaces:**
- Consumes: Tasks 1-18.
- Produces: a green branch, ready for **Merge prep**.

- [ ] **Step 1: Delete the exe-dir `imgui.ini`** under `bin\Debug-windows-x86_64-md\ArcaneEditor\` and `bin\Release-windows-x86_64-md\ArcaneEditor\`. It vetoes authored UI changes and dirties goldens.
- [ ] **Step 2: Full builds.** Regenerate, then build `Arcane.slnx` Release and Debug (`-m:4 -nr:false`). Then run `arcbuild build --project ReferenceProject` for Release, then Debug (end on Debug: single-slot `Binaries\`).
- [ ] **Step 3: ArcaneTests, both configs, from each exe dir.** Run `.\ArcaneTests.exe "~[gpu]~[shell]"` and `.\ArcaneTests.exe "[witness]~[shell]"`. Record each seed, pass count and assertion count. Expected: all PASS, and `[trajectory]` passes in both.
- [ ] **Step 4: Golden gate.** Run `powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Release`, then `-Configuration Debug`.
  - Expected: green with NO re-bless.
  - **A difference is a bug in this branch.** Diff it, root-cause it, fix forward, and re-run. Never bless.
- [ ] **Step 5: Baselines.**
  - Run `powershell -ExecutionPolicy Bypass -File scripts\check-baselines.ps1 -Invocation "~[gpu]"` for both configs.
  - A RISE is expected: the new cases. Commit the new counts into `scripts/automation-baselines.json`, following the existing entries' note style. Name this branch's added suites: trajectory, time, gameinput, swaps, param registration, physics2d, facade, reflection, scene unknown-field, templates, guard.
  - A DROP is a failure. Find the lost cases.
- [ ] **Step 6: The `Arcane::` audit** (spec s2 success criteria). Run `rg -n "OnFixedUpdate|jumpRequested|fixedDt|GetBodyMotion2D|SetBodyVelocity2D" ReferenceProject/Source`. Expected: no hits. `ReferenceGame.cpp` is init-only.
- [ ] **Step 7: Record the gate.** Write the record (or put it in the report): commits `b7a06b70..HEAD`, seeds, counts, golden result, arcbuild result, and the open items carried to **Merge prep**.
- [ ] **Step 8: Commit.**

```bash
git add scripts/automation-baselines.json
git commit -m "test(ci): input-seam gate baselines -- <old> -> <new> assertions / cases in Debug and Release (+trajectory, time, gameinput, swaps, param registration, physics2d, facade, reflection, scene unknown-field, templates, guard)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

## Merge prep (after the node-page phase merges; NOT part of this branch's run)

Spec s9. The controller does this when it integrates. It is listed here so nothing is lost.

1. **Rebase** `feat/input-time-resources` onto the merged node-page work.
   - Expected conflicts: `ThirdParty/Astra/**` (take the later vendor, re-run `sync-astra.ps1`), `scripts/automation-baselines.json` (re-measure), and possibly CVar files (the `HasFlag` rename vs the node-page T3-D2 cvar work; re-apply the rename to new callers).
   - Re-run Task 19's gate.
2. **ONE ABI bump to the next free number** (49 or 50 when the node-page phase lands, or later). Add a `// vNN (date, input-seam):` history line in `PluginABI.hpp` naming:
   - `LocalInputUser` gained `generation_` (layout of the class held by value inside `ARCANE_API ClientRuntime`);
   - `PhysicsResource` gained exported `Motion`/`SetVelocity`;
   - `GetBodyMotion2D`/`SetBodyVelocity2D` exports removed;
   - `Arcane::Time` and `Arcane::GameInput` registry resources added (new types);
   - Astra param systems keyed by functor type.
3. **Restamp** `ReferenceProject/ReferenceProject.arcproj` (`engine.abi`) in the bump commit.
4. **Aphelyon** (`D:\dev\starworks\Aphelyon`, its own commit, never pushed):
   - restamp `Game/Aphelyon.arcproj`;
   - sweep `docs/examples/arcane-physics-example.cpp` to `Arcane::` (its `using namespace Manifold2D::Physics` becomes `Arcane::Phys::`, and any `Astra::` becomes the alias);
   - rebuild the module through `arcbuild`, both configs, ending on Debug.
5. **Re-run the full gate** on the merged result, including the golden gate in both configs, ending on Debug.

## Self-review notes (writing-plans checklist, done)

**Spec coverage**

| Spec section | Task(s) |
|---|---|
| s3 | 4 |
| s4 | 5, 6 |
| s5.1 | 2, 3 |
| s5.2 | 8 |
| s5.3 | 9, 12 |
| s5.4 | 12 |
| s5.5 | 13 |
| s6.1 | 10, 11 |
| s6.2 | 12, 14, 15, 16, 18 (Aphelyon doc at Merge prep) |
| s6.3 | 17 |
| s7 | 2 (absent), 6 (unknown action), 9 (non-finite), 12 (OnInit refusal), 13 (unknown field) |
| s8 T1-T10 | 2, 4, 7, 6, 9, 1+12, 13, 14, 17, 19 |
| s9 | order; Merge prep |
| s10 | decisions honoured; owed items untouched |

**Spec errors and gaps found while reading the code, and how they were decided**
1. **`SingleStep()` is `RequestSingleStep()`** (`RunLoop.hpp:72`).
2. **The attribute aliases cannot live in `Arcane::`.** `Arcane::Hidden` is the tag component (`Components.hpp:302`), so they go in `Arcane::Attr` (Task 11).
3. **`Arcane::Any(CVarFlags, CVarFlags)`** (`CVarTypes.hpp:57`) collides with the `Any<...>` alias, so it is renamed `HasFlag` (Task 10).
4. **Named parameter systems were keyed by their wrapper** (`SystemScheduler.hpp:1226`), so `Before<ParamSystem>` and `HasSystem<ParamSystem>` could never match. They are now keyed by the functor (Task 2).
5. **The absent-resource log-once already exists** (`SystemParam.hpp`, `m_loggedMissing`). Task 2 pins it and does not rewrite it.
6. **Unknown scene keys are already tolerated silently** (`ReflectionJson.hpp:587`). Task 13 adds only the warning.
7. **`GameInput` is header-only, not `ARCANE_API`** (Task 6).
8. **`Time::elapsed` accumulates;** alpha inside a fixed step is the previous frame's (Task 4).
9. **`ClassTemplatesTest` compiled nothing,** so the compile proof is the checked-in-renders smoke plugin (Task 14).
10. **"Public declarations" made mechanical** with `ARCANE_INTERNAL` fences (Tasks 15-17).
11. **`Reads`/`Writes` are added to the facade,** because engine registry-style systems in the swept headers need them (Task 10).
12. **The Astra vendor in Task 3 also brings `b8291b9..975cdb7`,** already vendored on the node-page branch.
13. **The ABI bump is not in this branch.** It happens at Merge prep.

**Type consistency:** these names are used identically in every task that touches them:
- `Arcane::Time` fields; `GameInput::Resolve`; `ActionRef(map, action)`;
- `PhysicsResource::Motion(Entity, const RigidBody2D&)`, `SetVelocity(Entity, RigidBody2D&, float, float)`;
- `BodyMotion2D`; `Detail::ParamOrdering`, `Detail::KeyOf`;
- `NoteUnknownField`, `UnconsumedKeys`;
- `HasFlag`; `ARCANE_INTERNAL_BEGIN/END`.
