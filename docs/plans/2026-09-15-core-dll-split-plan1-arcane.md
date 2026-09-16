# Core-DLL split, Plan 1 (Arcane) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `ArcaneCore.dll` becomes the shared headless engine DLL every host links; `ArcaneClient.dll` links it and keeps presentation; `Runtime` splits into a Core `Runtime` and a Client `ClientRuntime`; one `ProcessContext` per process owns the Astra `TypeContext`; a game module registers role-masked system factories once and every `Runtime` instantiates by its `NetMode`; `ArcaneServer.exe` is a real Core-only dedicated-server host with a `--report` witness; the editor's play-mode dropdown grows the listen-server, embedded-server and separate-server-process modes; ABI 29 → 30.

**Architecture:** Two DLLs, one seam (spec §1). Core = today's ArcaneCore + `Base`/`Config`/`Jobs`/`Material`/`Mesh`/`Plugin`/`Project`/`Scene`/`Serialization`/`Sim`/`Sprite`/`Assets` lifted out of ArcaneClient, exported with a new `ARCANE_CORE_API`. The Core `Runtime` (registry, schedulers, loop, project, hot-reload, physics) is owned by a Client `ClientRuntime` that adds audio/input/ImGui/render-bridge; Core reaches back into presentation only through a small `IClientHooks` interface the Client implements (Mosaic's `IWorkScheduler` is the precedent for an interface defined below the seam and implemented above it). `PluginHost` moves to Core, becomes `ProcessContext`-scoped, and serves N attached `Runtime`s with a snapshot-all/reload/restore-all transaction. Nothing renders differently: `golden-gate.ps1` 4/4 in both configs is the regression net.

**Tech Stack:** C++23, premake5 / MSBuild (VS 18), Astra (vendored), Mosaic, Manifold2D, enkiTS, Catch2, the arcbuild driver for the Gacha Game, PowerShell gate scripts.

**Spec:** `docs/specs/2026-09-15-core-dll-split-design.md` (`1b290cf2`, approved 2026-09-15). §1 the two DLLs + prep, §2 the `Runtime` split, §3 `ProcessContext`, §4 net mode + role masks, §5 hot reload with N Runtimes, §6 `ArcaneServer`, §7 PIE, §8 macro/ABI/Gacha, §9 tests, §11 the seven steps, §13 rulings R1–R10. Context brief: `docs/research/2026-09-15-core-dll-split-context-brief.md` (the six edges, the per-module statics table, `Runtime`'s member split).

## Global Constraints

- **Repo:** everything here is the Arcane repo (`D:\dev\starworks\Arcane`, branch `main`) except two small Gacha-repo commits called out in Task 2 and Task 5. HEAD at plan time: `1b290cf2`, 3 commits unpushed (`2c6691e2`, `ca0169c4`, `1b290cf2`). **Do not push.** One commit per task (two where a Gacha commit pairs with it). Trailers on every commit:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```
  Never stage `out.txt`, `ArcaneEditor/ArcaneEditor/`, `ArcaneAssetPipeline/ArcaneAs.*/`, `arcbuild/arcbuild/` (Arcane strays), nor `Game/Source/TestComponent.*` / `Game/Content/scenes/test.arcscene` (Gacha, the user's in-flight files).
- **Build ritual (bash):** regenerate with `ThirdParty/premake5/premake5.exe vs2026` **from the repo root** (never `GenerateProjects.bat` under the Bash tool — it hangs) whenever `premake5.lua` changes or a `.cpp`/`.hpp` is added, removed or MOVED (the vcxproj is a snapshot of the globs). msbuild = `MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nologo -v:m` (whole solution, dash forms). **An engine rebuild ⇒ `ReferenceProject.slnx` `-t:Rebuild` for the SAME configuration before any host launch or witness run**, then rebuild `Arcane.slnx` again so the hosts' post-builds restage `ReferenceProject/Binaries/ReferenceGame.dll` (single-slot: the `plugin: initial load failed` trap). After ANY Release gate run flip the slot back to Debug (`ci\msbuild.cmd ReferenceProject\ReferenceProject.slnx -t:Rebuild -p:Configuration=Debug`) before a Debug `[gpu]`/witness run.
- **Tests run FROM the exe dir** `bin/<cfg>-windows-x86_64-md/ArcaneTests/`, **FOREGROUND only** (never a background suite wait). Output hygiene: `msbuild … > log; grep -E " error |Error\(s\)|Warning\(s\)"`; `ArcaneTests.exe "<filter>" | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED"`; never `cat` a suite log. Capture the seed of any failing run.
- **Every task ends green:** the task's own filter, then `~[gpu]` (Debug; Release at Tasks 2, 4, 5, 7 and the closeout), then `golden-gate.ps1` **both configs** at Tasks 1, 2, 4 and the closeout (`powershell -ExecutionPolicy Bypass -File scripts/golden-gate.ps1 [-Configuration Release]`; verdict = 4/4 lanes `diffCount=0`; a nonzero `diffCount` is a defect in this arc — **there is no re-bless in this arc**, spec §9). The thumbnail golden set (`[thumbs][golden]`, `[gpu]`) must not move either.
- **Baselines are measured, never recalled.** `scripts/automation-baselines.json` commits 56963 / 1709 (`~[gpu]`, Debug and Release). The 2026-09-15 desk measurement after the F2c debts arc read 57269 / 1753 — if the guard reports an unbooked rise at Task 1, book it FIRST as a backfill (the file's own 2026-09-11 backfill convention) in Task 1's commit, so every later delta is attributable to this plan. `powershell -File scripts/check-baselines.ps1` after each task's `-r json` run.
- **Gacha Game builds through the driver:** `D:\dev\starworks\Arcane\bin\<cfg>-windows-x86_64-md\arcbuild\arcbuild.exe build --project D:\dev\starworks\Gacha\Game --config <cfg> --sdk D:\dev\starworks\Arcane`. Probe first (`probe`, exit 3 = would-rebuild) and restore the config the slot held (the desk slot is usually Release).
- **TDD, every task:** the failing test is written and RUN RED before the production change, and the step names the expected failure. For the mechanical tasks (2, 4) RED is a compile/link failure the step names.
- **ABI 30 once, at Task 5.** `EngineContext` gains exactly `process`, `client`, `netMode` and nothing else (spec §2, §8, R7). No bump at Tasks 1–4; the Task 5 commit carries both restamps and the module rebuilds.
- **C4251 stays disabled; everything stays `/MD`** (`premake5.lua:31-39`); `ArcaneCore` is already `staticruntime "off"`.
- **Ledger:** `.superpowers/sdd/2026-09-15-core-dll-split-plan1-arcane/progress.md` (gitignored, local): `Task N: complete (sha)` + the measured counts and seeds.

---

## Plan-time rulings (each with its reason)

| # | Ruling | Why |
|---|---|---|
| P1 | **`Render/MeshBuilder.{hpp,cpp}` moves WHOLESALE to `Mesh/MeshBuilder.{hpp,cpp}`** (not just `MeshData`/`MeshBounds` carved out). | The file's own header declares it "PURE AND DEVICE-FREE -- no NRI, no GPU type", and `Mesh/MeshAsset.cpp:286-290` (Core-bound) calls its five generators — carving out only the two structs would leave a Core→Client call edge. The spec's §1.3 item 1 is satisfied by the superset. |
| P2 | **Task 1's "member lift" = a `RuntimePresentation` struct (Client) holding audio/input/ImGui-handoff/camera, held by `Runtime::Impl`; `Runtime`'s public API is unchanged until Task 4.** | The spec sequences the lift (step 1) before the host rewire (step 4). A struct with a complete type keeps step 1 a pure refactor with no host edits; step 4 then cuts along the seam this struct draws. |
| P3 | **Task 2 moves every Core-bound folder EXCEPT `Base/Runtime.*` and `Plugin/PluginHost.*`, which move at Task 4 with the split. `Base/Log`, `Base/Diagnostics` and the rest of `Base/` move at Task 2.** | `Runtime.cpp` still includes the Client `RuntimePresentation` (P2) and `PluginHost.cpp` calls `Runtime`, so neither can live in Core before the split; every other Core-bound TU logs through `ARC_*` and publishes diagnostics, so `Log`/`Diagnostics` must be in Core the moment anything else is. Task 3 is therefore `ProcessContext` alone — the spec's step-3 "move" is ownership, not files. |
| P4 | **A Gacha-repo commit at Task 2 narrows `Server/premake5.lua`'s from-source `ArcaneCore` glob to an explicit file list.** | `Server/premake5.lua:92-95` globs `ArcaneCore/src/**.cpp`; after Task 2 that glob would pull `Scene/`, `Plugin/`, `Jobs/JobSystem.cpp` (Astra/Manifold2D/enkiTS) into a build that has none of those include paths. The spec's §8 statement ("the Arcane-side change does not remove the sources it compiles") is true but incomplete: the move ADDS sources under the glob. The explicit list is backward-compatible, so it lands BEFORE the Arcane move. Not the `/MD` migration (Plan 2). |
| P5 | **`ClientRuntime` forwards the headless surface as one-line inline aliases (`Registry()`, `Loop()`, `OpenProject(...)`, …) in addition to `Core()`.** | 160 `m_runtime->` sites across the two hosts; a mechanical `Core().` sweep is review noise. Aliases add no semantics: the module still receives `ctx->engine` (Core) and `ctx->client` (Client) separately. |
| P6 | **`IClientHooks` (Core, `Arcane/Plugin/ClientHooks.hpp`) is the one Core→Client reach-back: `SaveUiContext`/`RestoreUiContext` (today's `ImGuiContextGuard`), `OnModuleTeardown` (today's `ResetAudio`), `OnSystemsCleared` (reinstalls `RenderSubmissionSystem`), `FillEngineContext` (the four ImGui `void*`s + the client pointer).** `ClientRuntime` implements it and attaches itself to its `Runtime`. | `PluginHost` moves to Core and today calls four presentation things. An interface defined in Core and implemented in Client is the workspace's existing shape (`Mosaic::IWorkScheduler`); "no class straddles the seam" (spec §2) is about class DEFINITIONS, and this one is defined in one DLL. Null hooks = a headless host; every call site null-checks. |
| P7 | **`RenderSubmissionSystem` is installed by `ClientRuntime`, not by `Runtime::InstallEngineSystems`.** Core installs `PhysicsSystem` → `TransformPropagationSystem`; Client adds render submission at construction and on `OnSystemsCleared`. | `RenderSystems.hpp` includes `Batcher2D` (Client). The engine's standard three (game-module spec §4.1) are unchanged for a Client-linked host; a Core-only host has the two it can execute. `RuntimeEngineSystemsTest` is re-pinned to this split. |
| P8 | **The system-factory table lives on `ProcessContext` (`SystemFactories()`), entries are `std::function<void(Astra::SystemScheduler&)>` + `RoleMask` + phase + name, and `PluginHost` clears the module's entries in `TeardownImage` BEFORE the image unmaps.** | Spec §4: registered "once per process at DLL load". A `std::function` allocated in the module is heap-shared under `/MD` and dies before the unmap — the same lifetime rule the `TypeMeta` thunks already live under (RuntimeApp.cpp:83-91). |
| P9 | **`PluginHost` becomes `PluginHost(ProcessContext&, path)` + `AttachRuntime(Runtime&)`/`DetachRuntime`. The FIRST attached Runtime is the PRIMARY: the module's `Init`/`SaveState`/`LoadState` bind to it (`ctx->engine`); every other attached Runtime is snapshotted/restored registry-only by the host and re-instantiated from the factory table.** | Spec §5 needs one transaction across all Runtimes and one module `Init` per load (§4, R1). Module extras (`OnSaveState`) are the primary's; a non-primary world's state is its registry. |
| P10 | **`ArcaneServer.exe` links Core only, but STAGES `ArcaneClient.dll` beside itself and reports whether it was loaded before/after the module load.** | Spec §1.2 has game modules link BOTH import libs (`ARCANE_GAME_MODULE`'s prologue calls `ImGui::SetCurrentContext`, exported from Client), so the loader needs `ArcaneClient.dll` resolvable to map `ReferenceGame.dll`. The construction gate is the exe's link line and is proven by the census; shedding the module's Client import is the §10 "server-only compile-out" follow-on, not this arc. |
| P11 | **`ProcessContext::Create(desc)` returns `nullptr` (+ `ARC_ERROR`) while one is alive — the spec's "refusal, not a warning" as a factory result, not an exception.** `ProcessContextDesc::externalTypeContext` adopts a caller-owned context (the test exe's `SharedTypeContext()`); an OWNED context is deliberately LEAKED on destruction. | The workspace throws nowhere; `Result`/optional is its refusal idiom. Leaking the owned context keeps the hosts' documented invariant (RuntimeApp.cpp:83-91: `TypeMeta` thunks into an unloaded plugin would run from `~TypeContext`). |
| P12 | **`ArcaneServer` gets its own `ServerConfig` over `Arcane::Cli`, and the Core-clean `HostBoot` helpers (`GameModule`, `PluginModules`, `BootSceneFile`, `BootScene`, `VerifySharedTypeContext`) move to a Core header `Arcane/Project/ProjectHost.hpp` that `Host/ProjectBoot.hpp` re-exports with `using`.** | `HostConfig.hpp` includes `Render/GraphicsBackend.hpp` and `ProjectBoot.hpp` includes `Render/GpuInstrumentation.hpp` — both Client. The five helpers use only Project/Scene/Serialization/Log. |
| P13 | **Out-of-process PIE spawns `ArcaneServer.exe --frames 0` (run until terminated) and the editor HOLDS the process handle (`ServerProcess`, new) so Stop terminates it; `RuntimeLaunch::SpawnDetached` stays fire-and-forget for the runtime.** | There is no transport yet (spec §4/§7: bring-up + project load is what this mode proves now); a child the editor cannot stop is a leak. |
| P14 | **The server witness is tagged `[witness][server]`, not `[gpu]`.** | It spawns a Core-only host with no device; `[gpu]` is the baseline-comparability convention for device cases. It therefore rises `~[gpu]` and is booked there. |

---

## File structure

| File | Change | Responsibility |
|---|---|---|
| `ArcaneClient/src/Arcane/Render/MeshBuilder.{hpp,cpp}` → `…/Mesh/MeshBuilder.{hpp,cpp}` | Move (T1) | CPU procedural geometry + `MeshData`/`MeshBounds` (P1). |
| `ArcaneClient/src/Arcane/Scene/{MeshSubmissionSystem,RenderSystems}.hpp` → `…/Render/` | Move (T1) | Presentation sweeps wearing a `Scene/` path. |
| `ArcaneClient/src/Arcane/Base/RuntimePresentation.hpp` | Create (T1) → moves to `Client/` (T4) | Audio device, input snapshot, ImGui handoff, 2D camera. |
| `ArcaneCore/src/Arcane/Core/Api.hpp` | Create (T2) | `ARCANE_CORE_API`. |
| `ArcaneCore/src/Arcane/{Base,Config,Jobs,Material,Mesh,Plugin,Project,Scene,Serialization,Sim,Sprite,Assets}/…` | Move from ArcaneClient (T2; `Base/Runtime.*`, `Plugin/PluginHost.*` at T4) | The headless engine layer. |
| `ArcaneCore/src/Arcane/{Guid,Cli/Cli,Build/Toolchain}.hpp` | Modify (T2) | Export today's Core symbols. |
| `premake5.lua`, `build/arcane.lua`, `ReferenceProject/premake5.lua` (no change), Gacha `Server/premake5.lua` | Modify (T2) | Core is a `SharedLib`; every host/tool/module links + stages it. |
| `ArcaneCore/src/Arcane/Base/ProcessContext.{hpp,cpp}` | Create (T3) | The one-per-process object: TypeContext, launch flag, system factories (T5). |
| `ArcaneTests/src/Helpers/TestTypeContext.hpp`, `ArcaneTests/src/test_main.cpp` | Modify (T3) | `Arcane::Test::Process()` — the test-wide `ProcessContext`. |
| `ArcaneCore/src/Arcane/Plugin/ClientHooks.hpp` | Create (T4) | `IClientHooks` (P6). |
| `ArcaneClient/src/Arcane/Client/ClientRuntime.{hpp,cpp}` | Create (T4) | Owns a `Runtime`; presentation surface; implements `IClientHooks`. |
| `ArcaneRuntime/src/RuntimeApp.*`, `ArcaneEditor/src/App/EditorApp*.{hpp,cpp}`, `ArcaneClient/src/Arcane/Host/ProjectBoot.*` | Modify (T4) | Hosts own a `ClientRuntime`. |
| `ArcaneCore/src/Arcane/Plugin/SystemFactory.hpp`, `ArcaneCore/src/Arcane/Sim/NetDriver.hpp` | Create (T5) | `NetMode`, `RoleMask`, `SystemFactoryTable`, `INetDriver`. |
| `ArcaneCore/src/Arcane/Plugin/{PluginABI.hpp,GameModule.hpp,PluginHost.*}` | Modify (T5) | ABI 30, `EngineContext` +3, `RegisterSystem`, N-Runtime host. |
| `ArcaneTests/plugins/HotReload{Plugin.cpp,Shared.hpp}` | Modify (T5) | Server-/Client-masked probe systems. |
| `ReferenceProject/ReferenceProject.arcproj`, Gacha `Game/Aphelyon.arcproj` | Modify (T5) | `"abi": 30`. |
| `ArcaneCore/src/Arcane/Project/ProjectHost.hpp` | Create (T6) | Core-clean boot helpers (P12). |
| `ArcaneServer/src/{main.cpp,ServerConfig.hpp,ServerConfig.cpp,ServerApp.hpp,ServerApp.cpp,ServerReport.hpp,ServerReport.cpp}` | Rewrite/Create (T6) | The dedicated-server host + census. |
| `ArcaneTests/src/{ServerConfigTest,ServerWitnessTest}.cpp` | Create (T6) | `[server]` units + `[witness][server]`. |
| `ArcaneEditor/src/App/PlayMode.{hpp,cpp}`, `ArcaneEditor/src/Panels/EditorPanels.cpp`, `ArcaneEditor/src/Project/ServerLaunch.{hpp,cpp}`, `ArcaneClient/src/Arcane/Host/HostConfig.{hpp,cpp}` | Modify/Create (T7) | Play topologies, the picker rows, the server child, `--play-as`. |
| `scripts/automation-baselines.json`, spec status line, this plan's Closeout | Modify (T8) | Booking + close. |

---

### Task 1: Prep — MeshBuilder re-homed, the six edges cut, the `Runtime` member lift

**Files:**
- Move: `ArcaneClient/src/Arcane/Render/MeshBuilder.hpp` → `ArcaneClient/src/Arcane/Mesh/MeshBuilder.hpp`; `…/Render/MeshBuilder.cpp` → `…/Mesh/MeshBuilder.cpp` (`git mv`, content unchanged except the header comment's first line gains "Lives in Mesh/ (Core-bound): CPU geometry, never GPU.")
- Move: `ArcaneClient/src/Arcane/Scene/MeshSubmissionSystem.hpp` → `…/Render/MeshSubmissionSystem.hpp`; `…/Scene/RenderSystems.hpp` → `…/Render/RenderSystems.hpp`
- Modify (include lines only, `Render/MeshBuilder.hpp` → `Mesh/MeshBuilder.hpp`): `ArcaneClient/src/Arcane/Scene/SceneResources.hpp:9`, `ArcaneClient/src/Arcane/Mesh/MeshAsset.hpp:29`, `ArcaneClient/src/Arcane/Host/SceneRenderResolver.hpp:55`, `ArcaneClient/src/Arcane/Render/Nri/NriMeshBufferCache.hpp:53`, `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp:107`, `ArcaneEditor/src/Project/MeshImportWave.hpp:19`, `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:13`, `ArcaneTests/src/{GoldenImageTest.cpp:11, MeshAssetTest.cpp:5, MeshBuilderTest.cpp:27, MeshNodeTest.cpp:31, MeshSubmissionTest.cpp:36, NriGraphPixelTest.cpp:67, NriMeshBufferCacheTest.cpp:20, RenderGraphTest.cpp:36}`, and the moved `Mesh/MeshBuilder.cpp:1`
- Modify (include lines only, `Scene/MeshSubmissionSystem.hpp` → `Render/MeshSubmissionSystem.hpp`): `ArcaneRuntime/src/RuntimeFrame.cpp:20`, `ArcaneEditor/src/App/EditorAppFrame.cpp:35`, `ArcaneTests/src/MeshSubmissionTest.cpp:30`
- Modify (include lines only, `Scene/RenderSystems.hpp` → `Render/RenderSystems.hpp`): `ArcaneClient/src/Arcane/Base/Runtime.cpp:16`, `ArcaneTests/src/{EntityIdentityTest.cpp:12, RuntimeEngineSystemsTest.cpp:12, RenderInterpolationTest.cpp:14, RuntimeTest.cpp:15, SpriteRotationTest.cpp:23}`
- Create: `ArcaneClient/src/Arcane/Base/RuntimePresentation.hpp`
- Modify: `ArcaneClient/src/Arcane/Base/Runtime.hpp` (drop `:10`'s `Input/InputSnapshot.hpp` include; forward-declare `struct InputSnapshot;`), `ArcaneClient/src/Arcane/Base/Runtime.cpp` (drop `:5`'s `Audio/AudioDevice.hpp`; `Impl` holds a `RuntimePresentation`)
- Test: existing suites are the net — `[mesh]`, `[runtime]`, `[audio]`, `[input]`, `[editor]`, plus `golden-gate.ps1` both configs.

**Interfaces:**
- Consumes: today's `Runtime::Impl` members `input`, `audioDesc`, `audio`, `cameraOffset`, `cameraZoom`, `imgui*` (`Runtime.cpp:93-113`) and `InitAudio`/`ResetAudio` (`:216-239`).
- Produces: `Arcane::RuntimePresentation` (below) — the exact member set `ClientRuntime` absorbs in Task 4. `Runtime`'s public API is byte-for-byte unchanged this task.

- [ ] **Step 1: RED — the include-graph facts before the move.** From the repo root:

```bash
grep -rn "Render/MeshBuilder.hpp\|Scene/MeshSubmissionSystem.hpp\|Scene/RenderSystems.hpp" --include=*.hpp --include=*.cpp ArcaneClient ArcaneEditor ArcaneRuntime ArcaneTests | grep "#include" | wc -l
grep -n "Audio/AudioDevice.hpp\|Input/InputSnapshot.hpp" ArcaneClient/src/Arcane/Base/Runtime.hpp ArcaneClient/src/Arcane/Base/Runtime.cpp
```
Expected: the first prints `25` (16 `MeshBuilder` + 3 `MeshSubmissionSystem` + 6 `RenderSystems` includers, the Files list above; if it prints more, add the extra sites to the Files list before moving anything); the second prints exactly two lines (`Runtime.cpp:5` and `Runtime.hpp:10`). These two greps are re-run at Step 7 and must print `0` and nothing.

- [ ] **Step 2: Move the four files and repoint every includer.**

```bash
git mv ArcaneClient/src/Arcane/Render/MeshBuilder.hpp ArcaneClient/src/Arcane/Mesh/MeshBuilder.hpp
git mv ArcaneClient/src/Arcane/Render/MeshBuilder.cpp ArcaneClient/src/Arcane/Mesh/MeshBuilder.cpp
git mv ArcaneClient/src/Arcane/Scene/MeshSubmissionSystem.hpp ArcaneClient/src/Arcane/Render/MeshSubmissionSystem.hpp
git mv ArcaneClient/src/Arcane/Scene/RenderSystems.hpp ArcaneClient/src/Arcane/Render/RenderSystems.hpp
```
Then edit each include line listed in **Files** (three patterns, 25 lines). One comment-only mention rides along: `ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactFormat.hpp:196` says "(Render/MeshBuilder.hpp)" — make it `Mesh/MeshBuilder.hpp`. Update the two moved headers' own self-references: `Render/MeshSubmissionSystem.hpp:6` ("sweep idiom (Scene/RenderSystems.hpp:47)" → `Render/RenderSystems.hpp`), and `Render/RenderSystems.hpp`/`SceneResources.hpp:175,265,288`/`PluginABI.hpp:294-296`/`MeshNode.hpp:248`/`SceneRenderResolver.{hpp:230,cpp:477}`/`MaterialPreviewHarvester.cpp:882` prose mentions of `Scene/MeshSubmissionSystem.hpp` → `Render/MeshSubmissionSystem.hpp` (comments; keep the sweep honest, `grep -rn "Scene/MeshSubmissionSystem\|Scene/RenderSystems\|Render/MeshBuilder"` must return nothing in source after this step).

- [ ] **Step 3: Regenerate + build Debug; expect 0 errors.**

```bash
ThirdParty/premake5/premake5.exe vs2026
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nologo -v:m > /tmp/t1a.log; grep -E " error |Error\(s\)|Warning\(s\)" /tmp/t1a.log
```
Expected: `0 Error(s)`. A `C1083 cannot open include file` names a site the Step 1 grep missed — fix it, do not add a forwarding header.

- [ ] **Step 4: Write `RuntimePresentation.hpp`** — the members leave `Runtime::Impl` verbatim:

```cpp
#pragma once

// RuntimePresentation: the presentation half of Runtime's substrate -- the OS audio
// device, the host's per-frame input snapshot, the ImGui cross-DLL handoff and the
// 2D camera the plugin drives. Lifted out of Runtime::Impl (Core-DLL split, plan 1
// Task 1; spec docs/specs/2026-09-15-core-dll-split-design.md s1.3/s2) so that the
// headless Runtime carries NO Audio/Input include: this struct is exactly what
// ClientRuntime absorbs at Task 4. Client-only; Core never includes it.

#include <Arcane/Audio/AudioDevice.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <glm/glm.hpp>

namespace Arcane
{
    class Assets;

    struct RuntimePresentation
    {
        InputSnapshot          input{};        // latest host-supplied snapshot; plugins read via Input()
        Audio::AudioDeviceDesc audioDesc{};
        Audio::AudioDevice     audio;
        glm::vec2              cameraOffset{0.0f, 0.0f};
        float                  cameraZoom = 1.0f;
        void* imguiContext  = nullptr;   // ImGuiContext*      -- all null in a headless host
        void* imguiAlloc    = nullptr;   // ImGuiMemAllocFunc
        void* imguiFree     = nullptr;   // ImGuiMemFreeFunc
        void* imguiUserData = nullptr;

        // enableAudioDevice: opt into a real OS device (interactive hosts); false = the
        // miniaudio null backend (tests/servers/tools/--frames N). The real->null fallback
        // applies either way. `assets` may be null (no audio at all).
        void InitAudio(Assets* assets, bool enableAudioDevice) noexcept
        {
            audioDesc.enableDevice = enableAudioDevice;
            if (!assets) return;
            if (audio.Init(assets, audioDesc)) return;
            if (audioDesc.enableDevice)
            {
                ARC_WARN("Runtime: audio device init failed; falling back to null backend");
                audioDesc.enableDevice = false;
                if (audio.Init(assets, audioDesc)) return;
            }
            ARC_WARN("Runtime: audio subsystem is unavailable");
        }
        void ResetAudio(Assets* assets) noexcept { audio.Shutdown(); InitAudio(assets, audioDesc.enableDevice); }
        ~RuntimePresentation() { audio.Shutdown(); }
    };
}
```

- [ ] **Step 5: Rewire `Runtime.hpp` / `Runtime.cpp` onto it.** In `Runtime.hpp`: delete line 10 (`#include <Arcane/Input/InputSnapshot.hpp>`) and add `struct InputSnapshot;` to the forward declarations at `:35-44` (the two `InputSnapshot` signatures at `:217-218` take/return references, so the forward declaration suffices). In `Runtime.cpp`: replace `#include <Arcane/Audio/AudioDevice.hpp>` (`:5`) with `#include <Arcane/Base/RuntimePresentation.hpp>`; add `#include <Arcane/Input/InputSnapshot.hpp>` (the .cpp still copies snapshots by value); in `Impl` delete the members `input`, `audioDesc`, `audio`, `cameraOffset`, `cameraZoom`, `imguiContext..imguiUserData` and the `InitAudio`/`ResetAudio` methods and the `~Impl` body, and add one member `RuntimePresentation presentation;` declared AFTER `assets` (it destructs before `assets`, as `audio` did); the ctor's tail becomes `presentation.InitAudio(assets.get(), enableAudioDevice);` in place of `audioDesc.enableDevice = …; … InitAudio();`. Every forwarding method reads `m_impl->presentation.X` (`AudioSystem`, `SetInputSnapshot`/`Input`, `SetImGui`/`ImGui*`, `SetCamera`/`CameraOffset`/`CameraZoom`, `SetRenderContext`'s camera reads, `ResetAudio` → `m_impl->presentation.ResetAudio(m_impl->assets.get())`).

- [ ] **Step 6: Build Debug, run the affected suites FOREGROUND from the exe dir.**

```bash
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nologo -v:m > /tmp/t1b.log; grep -E " error |Error\(s\)" /tmp/t1b.log
cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "[runtime],[audio],[input],[mesh],[editor],[hotreload]" | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED"
```
Expected: `0 Error(s)`; `All tests passed`. Then the full `~[gpu]` with `-r json::out=/tmp/t1.json` and `powershell -File scripts/check-baselines.ps1` — if it reports a RISE over 56963/1709 that this task did not create (the unbooked 2026-09-15 debts-arc figure), book it now in `scripts/automation-baselines.json` as a dated backfill entry (copy the 2026-09-11 backfill paragraph's shape: source commit `4ecfc6ec`, "measured, not derived here") so the guard reads `+0/+0` before Task 2.

- [ ] **Step 7: The edge facts after.** Re-run Step 1's two greps: expected `0` and no output. Then the gate, both configs (Release first, then flip the slot back):

```bash
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" ReferenceProject/ReferenceProject.slnx -t:Rebuild -p:Configuration=Release -m -nologo -v:m > /tmp/rp.log; grep -E "Error\(s\)" /tmp/rp.log
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Release -m -nologo -v:m > /tmp/t1r.log; grep -E "Error\(s\)" /tmp/t1r.log
powershell -ExecutionPolicy Bypass -File scripts/golden-gate.ps1 -Configuration Release | grep -E "diffCount|gatePassed|lanes"
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" ReferenceProject/ReferenceProject.slnx -t:Rebuild -p:Configuration=Debug -m -nologo -v:m > /tmp/rp.log
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nologo -v:m > /tmp/t1d.log
powershell -ExecutionPolicy Bypass -File scripts/golden-gate.ps1 | grep -E "diffCount|gatePassed|lanes"
```
Expected: 4/4 lanes `diffCount=0` in both configs. Anything else is a defect in Step 2/5 (the move changed a pixel path it must not have) — find it, do not bless.

- [ ] **Step 8: Commit.**

```bash
git add ArcaneClient ArcaneEditor ArcaneRuntime ArcaneTests scripts/automation-baselines.json
git commit -m "refactor(core-dll): prep -- MeshBuilder to Mesh/, MeshSubmissionSystem+RenderSystems to Render/, Runtime's presentation members lifted into RuntimePresentation (plan 1 Task 1)"
```

---

### Task 2: `ARCANE_CORE_API`, the physical move, `ArcaneCore.dll` builds and every host links it

**Files:**
- Create: `ArcaneCore/src/Arcane/Core/Api.hpp`
- Move (`git mv`, from `ArcaneClient/src/Arcane/` to `ArcaneCore/src/Arcane/`, same relative path): `Base/{Assert,DiagEnvelope,Diagnostics,Engine,Log,ServiceThread}.{hpp,cpp}`; `Config/*`; `Jobs/{ArcaneWorkScheduler.hpp,JobSystem.hpp,JobSystem.cpp}` (joins Core's existing `Jobs/TaskExecutor.hpp`); `Material/*`; `Mesh/*`; `Plugin/{GameComponents.hpp,GameModule.hpp,Module.hpp,Module.cpp,Plugin.hpp,Plugin.cpp,PluginABI.hpp}`; `Project/*`; `Scene/*` (the 7 files left after Task 1); `Serialization/*`; `Sim/*`; `Sprite/*`; `Assets/*`. **NOT moved:** `Base/Api.hpp`, `Base/Runtime.{hpp,cpp}`, `Base/RuntimePresentation.hpp`, `Plugin/PluginHost.{hpp,cpp}` (P3).
- Modify (macro audit, every moved file): `ARCANE_API` → `ARCANE_CORE_API`; `#include <Arcane/Base/Api.hpp>` → `#include <Arcane/Core/Api.hpp>`.
- Modify: `ArcaneCore/src/Arcane/Guid.hpp:21`, `ArcaneCore/src/Arcane/Cli/Cli.hpp:26,46`, `ArcaneCore/src/Arcane/Build/Toolchain.hpp:30-55` (export today's Core symbols)
- Modify: `ArcaneCore/src/Arcane/Base/Log.cpp:50-66` (`Init` installs Core's own Mosaic sink)
- Modify: `premake5.lua` (`ArcaneCore` `:116-172`, `arccook` `:251-312`, `arcbuild` `:327-392`, `ArcaneClient` `:402-514`, `ArcaneRuntime` `:534-651`, `ArcaneEditor` `:660-780`, `ArcaneTests` `:793-1280`, `test_plugin` `:1288-1325`), `build/arcane.lua:82-87`
- Modify (Gacha repo, its own commit, lands FIRST): `D:\dev\starworks\Gacha\Server\premake5.lua:92-95`
- Create: `ArcaneTests/src/CoreDllTest.cpp`

**Interfaces:**
- Consumes: Task 1's tree (every Core-bound folder is include-clean of `Render/Nri/Platform/ImGui/Input/Audio/Host`).
- Produces: `ARCANE_CORE_API` (dllexport under `ARCANE_CORE_BUILD_DLL`, dllimport otherwise); `ArcaneCore.dll` + `ArcaneCore.lib` at `bin/<cfg>-windows-x86_64-md/ArcaneCore/`; every exe stages `ArcaneCore.dll` beside itself; game modules link `ArcaneCore` + `ArcaneClient`.

- [ ] **Step 1: Gacha first — narrow the from-source glob (P4).** In `D:\dev\starworks\Gacha\Server\premake5.lua` replace lines 92-95 with the explicit list of what the server actually consumes today (the comment at `:76-79` names it):

```lua
    files {
        -- EXPLICIT, not a glob (Core-DLL split, Arcane plan 1 Task 2): the Arcane
        -- repo's ArcaneCore/src is about to absorb the headless engine layer
        -- (Scene/Plugin/Project/... -- Astra, Manifold2D, enkiTS), which this
        -- static-CRT source build has no include paths for. Plan 2 replaces this
        -- whole project with a link line against ArcaneCore.dll.
        IncludeDir["ArcaneCore"] .. "/Arcane/Guid.hpp",
        IncludeDir["ArcaneCore"] .. "/Arcane/Guid.cpp",
        IncludeDir["ArcaneCore"] .. "/Arcane/Version.hpp",
        IncludeDir["ArcaneCore"] .. "/Arcane/Build/**",
        IncludeDir["ArcaneCore"] .. "/Arcane/Cli/**",
        IncludeDir["ArcaneCore"] .. "/Arcane/Crypto/**",
        IncludeDir["ArcaneCore"] .. "/Arcane/Jobs/TaskExecutor.hpp",
        IncludeDir["ArcaneCore"] .. "/Arcane/Net/**",
        IncludeDir["ArcaneCore"] .. "/Arcane/Util/**",
    }
```
Verify against today's tree: `cd D:\dev\starworks\Gacha\Server && ..\ThirdParty\premake5\premake5.exe vs2026` then `msbuild Aphelyon.slnx -p:Configuration=Debug -m -v:m > log` → `0 Error(s)`. Commit in Gacha: `chore(server): ArcaneCore from-source build takes an explicit file list, not a glob (Arcane Core-DLL split, plan 1 Task 2)`.

- [ ] **Step 2: RED — the test that names the boundary.** Create `ArcaneTests/src/CoreDllTest.cpp`:

```cpp
// Core-DLL split (spec docs/specs/2026-09-15-core-dll-split-design.md s1, s8):
// which DLL DEFINES a symbol is the whole point of the export-macro audit, so pin
// it directly -- the address the exe resolves for a Core symbol lies inside
// ArcaneCore.dll's image, and a Client symbol's inside ArcaneClient.dll's.
// GetModuleHandleEx(FROM_ADDRESS) is the OS's own answer to "which module owns
// this address"; no psapi needed.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>   // RenderErrorCount -- a Client export

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{
    HMODULE OwnerOf(const void* addr)
    {
        HMODULE h = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(addr), &h);
        return h;
    }
}

TEST_CASE("ArcaneCore.dll is a loaded module and defines the Core surface", "[core-dll]")
{
    const HMODULE core   = ::GetModuleHandleW(L"ArcaneCore.dll");
    const HMODULE client = ::GetModuleHandleW(L"ArcaneClient.dll");
    REQUIRE(core != nullptr);
    REQUIRE(client != nullptr);
    CHECK(core != client);

    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::Log::Engine))       == core);
    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::Diagnostics::SetSink)) == core);
    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::BuildInfo))          == core);
    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::RenderErrorCount))   == client);
}

TEST_CASE("one engine logger: the exe and ArcaneClient.dll see the same spdlog instance", "[core-dll]")
{
    // Log::Engine() is Core's. A sink pushed from the exe must receive a line
    // logged from INSIDE ArcaneClient.dll -- Diagnostics::Publish with no sink
    // installed is silent, so use the plugin loader's own ARC_ERROR path:
    // PluginLoadDiagnosticsTest already proves the Diagnostics half; this pins the
    // LOGGER half, which the split moved.
    CHECK(Arcane::Log::Engine() != nullptr);
    CHECK(Arcane::Log::Engine() == Arcane::Log::Engine());
}
```
Build: expected RED at LINK — `unresolved external symbol` for nothing yet (the file compiles against today's tree) but the FIRST CHECK fails at run: `ArcaneCore.dll` is not a loaded module (`core == nullptr`). Run `./ArcaneTests.exe "[core-dll]"` after a Debug build: expected `FAILED` on `REQUIRE(core != nullptr)`.

- [ ] **Step 3: The macro.** Create `ArcaneCore/src/Arcane/Core/Api.hpp`:

```cpp
#pragma once

// ARCANE_CORE_API: dllexport when building ArcaneCore.dll (ARCANE_CORE_BUILD_DLL is
// defined ONLY in that project's premake block), dllimport for every consumer --
// ArcaneClient.dll, the hosts, arccook/arcbuild, ArcaneTests, ArcaneAssetPipeline's
// objects and every game module. Sibling of ArcaneClient's ARCANE_API
// (Arcane/Base/Api.hpp); a symbol is marked with exactly ONE of the two, by the DLL
// that defines it (spec docs/specs/2026-09-15-core-dll-split-design.md s8, R6).
#if defined(_WIN32)
    #if defined(ARCANE_CORE_BUILD_DLL)
        #define ARCANE_CORE_API __declspec(dllexport)
    #else
        #define ARCANE_CORE_API __declspec(dllimport)
    #endif
#else
    #define ARCANE_CORE_API __attribute__((visibility("default")))
#endif
```

- [ ] **Step 4: Move the folders and run the audit.** From the repo root (bash):

```bash
for p in Base/Assert Base/DiagEnvelope Base/Diagnostics Base/Engine Base/Log Base/ServiceThread; do git mv ArcaneClient/src/Arcane/$p.hpp ArcaneCore/src/Arcane/$p.hpp; [ -f ArcaneClient/src/Arcane/$p.cpp ] && git mv ArcaneClient/src/Arcane/$p.cpp ArcaneCore/src/Arcane/$p.cpp; done
for d in Config Material Mesh Project Scene Serialization Sim Sprite Assets; do mkdir -p ArcaneCore/src/Arcane/$d; git mv ArcaneClient/src/Arcane/$d/* ArcaneCore/src/Arcane/$d/; done
for f in ArcaneWorkScheduler.hpp JobSystem.hpp JobSystem.cpp; do git mv ArcaneClient/src/Arcane/Jobs/$f ArcaneCore/src/Arcane/Jobs/$f; done
for f in GameComponents.hpp GameModule.hpp Module.hpp Module.cpp Plugin.hpp Plugin.cpp PluginABI.hpp; do git mv ArcaneClient/src/Arcane/Plugin/$f ArcaneCore/src/Arcane/Plugin/$f; done
# the audit: every moved file, one macro and one include path
grep -rl "ARCANE_API\|Arcane/Base/Api.hpp" ArcaneCore/src | xargs sed -i 's/\bARCANE_API\b/ARCANE_CORE_API/g; s#<Arcane/Base/Api.hpp>#<Arcane/Core/Api.hpp>#'
grep -rn "ARCANE_API\b\|Base/Api.hpp" ArcaneCore/src   # expected: NO output
grep -rn "ARCANE_CORE_API" ArcaneClient/src ArcaneEditor/src ArcaneRuntime/src   # expected: NO output
```
Then the export markers on today's Core symbols: `struct ARCANE_CORE_API Guid` (`Guid.hpp:21`, add `#include <Arcane/Core/Api.hpp>`), `class ARCANE_CORE_API Cli` and `struct ARCANE_CORE_API Result` (`Cli.hpp:26,46` — a nested class does not inherit the outer export), and `ARCANE_CORE_API` on each of the five `Toolchain` functions (`Toolchain.hpp:30,37,44,50,55`). `Crypto`, `Net/*`, `Util/*`, `Jobs/TaskExecutor.hpp`, `Version.hpp` are header-only: no macro.

- [ ] **Step 5: Core's own Mosaic sink.** `Log.cpp` now compiles into ArcaneCore.dll, whose Mosaic storage is a THIRD per-module copy. In `Init()`'s `call_once` body, after `set_pattern`, add `Mosaic::SetLogSink(MosaicSink(), nullptr);   // THIS module's (ArcaneCore.dll's) Mosaic copy -- Astra/Manifold2D code running inside Core routes here; Client and the hosts keep installing into their own copies via InstallMosaicSink()`. The header's `InstallMosaicSink` stays `inline` (`Log.hpp:34`) for exactly the reason its comment states. Check whether `Assert::InstallMosaicHandler` (`Assert.hpp`) is `inline` too; if it is, call `Mosaic::SetAssertHandler(...)` the same way from `Init()` (mirror what the inline body does), so Core's copy has a handler.

- [ ] **Step 6: premake.** Edits, in order:
  1. `ArcaneCore` (`:116-172`): `kind "SharedLib"`; `defines` gains `"ARCANE_CORE_BUILD_DLL"`, `"NOMINMAX"`, `"WIN32_LEAN_AND_MEAN"`; `includedirs` gains `"%{IncludeDir.glm}"` (already), `"%{IncludeDir.stb}"`, `"%{IncludeDir.Astra}"`, `"%{IncludeDir.enkiTS}"`, `"%{IncludeDir.Manifold2D}"` (already), `"%{IncludeDir.Mosaic}"` (already), `"%{IncludeDir.picosha2}"` (already); `links { "enkiTS", "Manifold2D" }` (JobSystem.cpp instantiates enkiTS; `Scene/PhysicsSystem.hpp` is header-only but the link belongs here from the start); under `filter "system:windows"` add `links { "dbghelp" }` (Diagnostics.cpp) and drop the `"/utf-8"`-less assumption — the workspace `buildoptions` already carry it. Replace the header comment: "ArcaneCore: THE shared engine DLL (spec 2026-09-15). Presentation-free. Also consumed FROM SOURCE by the Server workspace with an explicit file list (Plan 2 retires that)."
  2. `ArcaneClient` (`:446`): keep `links { "ArcaneCore", … }` — it is now an import lib. Add to `postbuildcommands` nothing (the DLL is staged by the exes).
  3. `arccook` (`:283`): unchanged link line; add `postbuildcommands { '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ArcaneCore/ArcaneCore.dll" "%{cfg.buildtarget.directory}/ArcaneCore.dll"' }`.
  4. `arcbuild` (`:342-369`): `includedirs` drops `"%{wks.location}/ArcaneClient/src"`; `links { "ArcaneCore" }`; the postbuild copies `ArcaneCore.dll` instead of `ArcaneClient.dll`; the header comment's "It links ArcaneClient for Module::ScanFileCrtFlavor" becomes "It links ArcaneCore (Module/Project live there since the Core-DLL split)". Spec §1.2/§10: the small follow-on the split makes correct.
  5. `ArcaneRuntime` (`:575-643`), `ArcaneEditor` (`:708-762`), `ArcaneTests` (`:1169-1242`): each `postbuildcommands` gains, right before its `ArcaneClient.dll` copy line, `'{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ArcaneCore/ArcaneCore.dll" "%{cfg.buildtarget.directory}/ArcaneCore.dll"'`. Rewrite the "ArcaneCore links into exactly ONE module per process" comments (`:563-570`, `:784-786`, `:1284`) to: "ArcaneCore is a DLL since the Core-DLL split: exactly one copy per process BY CONSTRUCTION, and it is ArcaneCore.dll."
  6. `test_plugin` (`:1312`): `links { "ArcaneCore", "ArcaneClient" }`.
  7. `build/arcane.lua:82-87`: `libdirs { ARCANE_BIN .. "/ArcaneCore", ARCANE_BIN .. "/ArcaneClient" }`; `links { "ArcaneCore", "ArcaneClient" }`; the comment at `:69-72` ("include-only, no Core link (Core links into ONE module per process)") becomes "and its import lib: a game module links BOTH engine DLLs (spec 2026-09-15 s1.2); the host's own copies of both DLLs are what the loader binds".

- [ ] **Step 7: Regenerate, build, chase the link errors to zero.**

```bash
ThirdParty/premake5/premake5.exe vs2026
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nologo -v:m > /tmp/t2.log; grep -E " error |Error\(s\)" /tmp/t2.log | sort | uniq -c | sort -rn | head -40
```
The expected FIRST failure class is `LNK2019 unresolved external symbol` against `ArcaneCore.lib` from `ArcaneClient`/`ArcaneTests`/hosts: each names a Core symbol whose declaration lacks `ARCANE_CORE_API` (a free function in a header the sed did not touch because the file had no macro before — e.g. anything in `Scene/`, `Serialization/`, `Sim/` that is defined in a `.cpp`). Mark each, rebuild, repeat until `0 Error(s)`. Record the final list of NEWLY-marked symbols in the ledger (the spec's "92-site audit" is the sed; this loop is the rest of it). Second class: `C2491 definition of dllimport function not allowed` = a `.cpp` compiled into the wrong project (a stray un-moved file) — move it. Third: an `stb_image` link error from a Client TU means Client still has an `stb` user besides `Assets/` (`grep -rln "stb_image" ArcaneClient/src`); if so add `ArcaneClient/src/Arcane/Render/StbImpl.cpp` as a second implementation TU (two static copies in two modules — the workspace's established pattern for static libs) with a comment saying so.

- [ ] **Step 8: Suites, both configs; the gate, both configs.** Debug: `./ArcaneTests.exe "[core-dll]"` → `All tests passed`; then `~[gpu]` with `-r json`, `check-baselines.ps1` expected `+2 cases` (the two new `[core-dll]` cases; record the assertion delta). Then `ReferenceProject.slnx -t:Rebuild` Debug (its module now links both import libs) + `Arcane.slnx` Debug again (restage) + `~[gpu]` again including the `[witness][gpu]` scenarios by running the UNFILTERED suite once (`./ArcaneTests.exe | grep -E "^test cases|^assertions|passed|FAILED"`) — this is the run that proves a module linking both DLLs loads under a host that stages both. Release: the same build pair + `~[gpu]`. Gate: `golden-gate.ps1 -Configuration Release` then Debug (flip the slot back between, as in Task 1 Step 7) → 4/4 `diffCount=0` each. Gacha Game: `arcbuild.exe probe --project D:\dev\starworks\Gacha\Game --config Release --sdk D:\dev\starworks\Arcane` (note the verdict), then `build` for the config the slot held → `Game/Binaries/Aphelyon.dll` links; launch is not required here (Aphelyon's ABI is still 29 = current).

- [ ] **Step 9: Commit.**

```bash
git add ArcaneCore ArcaneClient ArcaneEditor ArcaneRuntime ArcaneTests arcbuild arccook premake5.lua build/arcane.lua
git commit -m "feat(core-dll): ArcaneCore.dll -- ARCANE_CORE_API, the headless engine layer moves to Core, every host/tool/module links it (plan 1 Task 2)"
```

---

### Task 3: `ProcessContext` — one per process, Core-owned, with the refusal test

**Files:**
- Create: `ArcaneCore/src/Arcane/Base/ProcessContext.hpp`, `ArcaneCore/src/Arcane/Base/ProcessContext.cpp`
- Create: `ArcaneTests/src/ProcessContextTest.cpp`
- Modify: `ArcaneTests/src/Helpers/TestTypeContext.hpp` (+ `Process()`), `ArcaneTests/src/test_main.cpp:19-33`
- Modify: `ArcaneClient/src/Arcane/Base/Runtime.hpp:53-69` (ctor), `ArcaneClient/src/Arcane/Base/Runtime.cpp:85-86,115-121,242-243`
- Modify: every test construction site `Arcane::Runtime X(&Arcane::Test::SharedTypeContext()…` (95 sites across 30 files, listed by the grep in Step 4)
- Modify: `ArcaneRuntime/src/RuntimeApp.hpp:144-146`, `ArcaneRuntime/src/RuntimeApp.cpp:81-127`; `ArcaneEditor/src/App/EditorApp.hpp:613-614`, `ArcaneEditor/src/App/EditorApp.cpp:257-294`

**Interfaces:**
- Consumes: `Astra::SetTypeContext(TypeContext*, ModuleResidency)` (`ThirdParty/Astra/include/Astra/Core/TypeContext.hpp:203`).
- Produces:
```cpp
namespace Arcane {
    struct ProcessContextDesc {
        bool isDedicatedServerProcess = false;        // the LAUNCH flag (spec s4 field 1) -- never a behaviour switch
        Astra::TypeContext* externalTypeContext = nullptr;   // adopt (not own) a caller's context; null = create + own
    };
    class ARCANE_CORE_API ProcessContext {
    public:
        // nullptr + ARC_ERROR while another instance is alive (P11). The live one:
        [[nodiscard]] static std::unique_ptr<ProcessContext> Create(ProcessContextDesc desc);
        [[nodiscard]] static ProcessContext* Current() noexcept;
        ~ProcessContext();   // frees the slot; an OWNED TypeContext is leaked on purpose
        Astra::TypeContext& TypeContext() noexcept;
        [[nodiscard]] bool  IsDedicatedServerProcess() const noexcept;
    };
    // Runtime: explicit Runtime(ProcessContext& process, bool enableAudioDevice = false);
    // ArcaneTests: Arcane::Test::Process() -> ProcessContext& (adopts SharedTypeContext())
}
```

- [ ] **Step 1: RED.** Create `ArcaneTests/src/ProcessContextTest.cpp`:

```cpp
// ProcessContext (spec docs/specs/2026-09-15-core-dll-split-design.md s3): the
// process-wide state Arcane pays for exactly once. N Runtimes share it; a second
// construction is a REFUSAL. The test exe's own instance (Helpers/TestTypeContext.hpp,
// created in test_main before Catch2 runs) is the live one every case here sees --
// which is the honest shape: the refusal is only observable against a live instance,
// and this process has one for its whole life, like every host.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>

#include "Helpers/TestTypeContext.hpp"

TEST_CASE("ProcessContext: exactly one per process -- a second Create is refused", "[process]")
{
    Arcane::ProcessContext& live = Arcane::Test::Process();
    REQUIRE(Arcane::ProcessContext::Current() == &live);
    CHECK(Arcane::ProcessContext::Create({}) == nullptr);                       // owned-context flavour
    Arcane::ProcessContextDesc adopt; adopt.externalTypeContext = &Arcane::Test::SharedTypeContext();
    CHECK(Arcane::ProcessContext::Create(adopt) == nullptr);                    // adopting flavour, same refusal
    CHECK(Arcane::ProcessContext::Current() == &live);                          // the refusal did not disturb the slot
}

TEST_CASE("ProcessContext: the test process is not a dedicated-server process and owns the shared TypeContext", "[process]")
{
    Arcane::ProcessContext& live = Arcane::Test::Process();
    CHECK_FALSE(live.IsDedicatedServerProcess());
    CHECK(&live.TypeContext() == &Arcane::Test::SharedTypeContext());
}

TEST_CASE("Runtime: every Runtime is built on the ProcessContext and reports its TypeContext", "[process][runtime]")
{
    Arcane::Runtime a(Arcane::Test::Process());
    Arcane::Runtime b(Arcane::Test::Process());
    CHECK(a.TypeContext() == &Arcane::Test::Process().TypeContext());
    CHECK(b.TypeContext() == a.TypeContext());
}
```
Build: expected RED at compile — `Arcane/Base/ProcessContext.hpp: No such file`.

- [ ] **Step 2: The class.** `ProcessContext.hpp`:

```cpp
#pragma once
// ProcessContext: the ONE per-process object (spec 2026-09-15 s3). Owns (or adopts)
// the Astra TypeContext every Runtime and every module imports, carries the
// launch flag (s4 field 1 -- systems NEVER branch on it; they branch on a
// Runtime's NetMode), and -- from Task 5 -- the system-factory table a game module
// registers into once per DLL load. Created exactly once by the host
// (ArcaneServer, RuntimeApp, EditorApp, ArcaneTests); a second Create() is a refusal.
#include <Arcane/Core/Api.hpp>
#include <memory>
namespace Astra { class TypeContext; }
namespace Arcane
{
    struct ProcessContextDesc
    {
        bool                isDedicatedServerProcess = false;
        Astra::TypeContext* externalTypeContext      = nullptr;
    };
    class ARCANE_CORE_API ProcessContext
    {
    public:
        [[nodiscard]] static std::unique_ptr<ProcessContext> Create(ProcessContextDesc desc);
        [[nodiscard]] static ProcessContext* Current() noexcept;
        ~ProcessContext();
        ProcessContext(const ProcessContext&) = delete;
        ProcessContext& operator=(const ProcessContext&) = delete;
        Astra::TypeContext& TypeContext() noexcept { return *m_context; }
        [[nodiscard]] bool  IsDedicatedServerProcess() const noexcept { return m_dedicated; }
    private:
        explicit ProcessContext(const ProcessContextDesc& desc);
        std::unique_ptr<Astra::TypeContext> m_owned;    // null when adopting
        Astra::TypeContext*                 m_context = nullptr;
        bool                                m_dedicated = false;
        bool                                m_slotHeld  = false;
    };
}
```
`ProcessContext.cpp`:

```cpp
#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Log.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <atomic>
namespace Arcane
{
    namespace { std::atomic<ProcessContext*> g_current{nullptr}; }

    ProcessContext::ProcessContext(const ProcessContextDesc& desc) : m_dedicated(desc.isDedicatedServerProcess)
    {
        if (desc.externalTypeContext) m_context = desc.externalTypeContext;
        else { m_owned = std::make_unique<Astra::TypeContext>(); m_context = m_owned.get(); }
    }

    std::unique_ptr<ProcessContext> ProcessContext::Create(ProcessContextDesc desc)
    {
        std::unique_ptr<ProcessContext> pc(new ProcessContext(desc));
        ProcessContext* expected = nullptr;
        if (!g_current.compare_exchange_strong(expected, pc.get()))
        {
            ARC_ERROR("ProcessContext: refused -- this process already has one (spec 2026-09-15 s3: exactly one per process; N Runtimes share it)");
            return nullptr;   // ~ProcessContext with m_slotHeld == false leaves the live slot alone
        }
        pc->m_slotHeld = true;
        // Install the context in THIS module's (ArcaneCore.dll's) per-module Astra slot,
        // Resident: Core never unmaps, so its binders are pinned (Runtime.cpp's residency note).
        Astra::SetTypeContext(pc->m_context, Astra::ModuleResidency::Resident);
        return pc;
    }

    ProcessContext* ProcessContext::Current() noexcept { return g_current.load(); }

    ProcessContext::~ProcessContext()
    {
        if (m_slotHeld) g_current.store(nullptr);
        // An OWNED TypeContext is LEAKED, deliberately: TypeMeta entries registered by a
        // game module hold std::function thunks compiled into that DLL, and after its
        // unload ~TypeContext would call into unmapped code (the heap-leak both hosts
        // documented at their old `new Astra::TypeContext()` sites). Adopted contexts
        // belong to their owner.
        (void)m_owned.release();
    }
}
```

- [ ] **Step 3: `Runtime` takes the process.** `Runtime.hpp:53-69`: replace the ctor and its comment with:

```cpp
        // Every Runtime is built on the process's ONE ProcessContext (spec 2026-09-15
        // s3): its TypeContext is the shared component-ID space; there is no
        // Runtime-owned context any more. The ctor installs that context in THIS
        // module's Astra slot (Resident) and registers the engine roster, so the
        // FIRST Runtime in a process still pins the numbering (the TypeContext-theft
        // note in ArcaneTests stands).
        // enableAudioDevice: as before -- false = the null backend; an interactive host passes true.
        explicit Runtime(ProcessContext& process, bool enableAudioDevice = false);
```
plus `class ProcessContext;` among the forward declarations and `#include <Arcane/Base/ProcessContext.hpp>` is NOT added (pointer/reference only). `Runtime.cpp`: `Impl(ProcessContext& process, bool enableAudioDevice)`; delete `ownedContext`; `context = &process.TypeContext();`; keep `Astra::SetTypeContext(context, Astra::ModuleResidency::Resident);` (this is ArcaneClient.dll's slot until Task 4 moves the file); ctor forwards `process`.

- [ ] **Step 4: The test exe.** `TestTypeContext.hpp` gains:

```cpp
#include <Arcane/Base/ProcessContext.hpp>
namespace Arcane::Test
{
    // The test process's ONE ProcessContext, ADOPTING SharedTypeContext() (so the exe's
    // own per-module install at test_main stays the same object). Created here, on first
    // use, which test_main forces before Catch2 runs.
    inline Arcane::ProcessContext& Process()
    {
        static std::unique_ptr<Arcane::ProcessContext> s_pc = [] {
            Arcane::ProcessContextDesc d; d.externalTypeContext = &SharedTypeContext();
            auto pc = Arcane::ProcessContext::Create(d);
            if (!pc) std::abort();   // a second Create in the test exe is a harness bug, not a test
            return pc;
        }();
        return *s_pc;
    }
}
```
`test_main.cpp:32`: `Arcane::Runtime pin(Arcane::Test::Process());` (the comment above it stays true). Then the sweep:

```bash
grep -rl "Arcane::Runtime [a-z]*(&Arcane::Test::SharedTypeContext()" ArcaneTests/src | xargs sed -i 's/Arcane::Runtime \([a-zA-Z_]*\)(&Arcane::Test::SharedTypeContext()/Arcane::Runtime \1(Arcane::Test::Process()/g'
grep -rn "SharedTypeContext()" ArcaneTests/src | grep -v "Helpers/TestTypeContext.hpp\|test_main.cpp\|ProcessContextTest.cpp\|RuntimeTest.cpp:45[2-3]\|Meta()"   # expected: only non-Runtime uses (SetTypeContext/Meta) remain
```

- [ ] **Step 5: The hosts.** `RuntimeApp.hpp:144`: replace `Astra::TypeContext* m_typeContext = nullptr;` with `std::unique_ptr<Arcane::ProcessContext> m_process;   // the process's ONE (spec s3); declared before m_runtime so it outlives it` (+ `#include <Arcane/Base/ProcessContext.hpp>`; the `namespace Astra { class TypeContext; }` line at `:30` can go). `RuntimeApp.cpp:95-120` becomes:

```cpp
    m_process = Arcane::ProcessContext::Create({});
    if (!m_process) { ARC_ERROR("ArcaneRuntime: ProcessContext refused -- a second host in this process?"); return false; }
    // This exe's OWN per-module Astra slot (unchanged reasoning: the slot is per module).
    Astra::SetTypeContext(&m_process->TypeContext());
    m_runtime.emplace(*m_process, m_config.maxFrames == 0);
```
Keep the comment block's substance (the leak reasoning now lives in `~ProcessContext`; say so in one line). Same edit in `EditorApp.hpp:613` / `EditorApp.cpp:268-283`.

- [ ] **Step 6: Build, suites, commit.** Debug build → `0 Error(s)`; `./ArcaneTests.exe "[process]"` → `All tests passed` (3 cases); `~[gpu]` `-r json` + `check-baselines.ps1` → `+3 cases`; the unfiltered suite once (the `[witness][gpu]` scenarios launch the real hosts through their new `runtime_create` bodies). Release build + `~[gpu]`. Gacha Game: no change (the module never constructed a Runtime).

```bash
git add ArcaneCore ArcaneClient ArcaneTests ArcaneRuntime ArcaneEditor
git commit -m "feat(core-dll): ProcessContext -- one per process, owns the TypeContext, refuses a second; Runtime builds on it; hosts and the test exe create theirs (plan 1 Task 3)"
```

---

### Task 4: `Runtime` (Core) / `ClientRuntime` (Client) — the split, `PluginHost` to Core, hosts rewired

**Files:**
- Create: `ArcaneCore/src/Arcane/Plugin/ClientHooks.hpp`
- Move: `ArcaneClient/src/Arcane/Base/Runtime.{hpp,cpp}` → `ArcaneCore/src/Arcane/Base/Runtime.{hpp,cpp}`; `ArcaneClient/src/Arcane/Plugin/PluginHost.{hpp,cpp}` → `ArcaneCore/src/Arcane/Plugin/PluginHost.{hpp,cpp}`; `ArcaneClient/src/Arcane/Base/RuntimePresentation.hpp` → `ArcaneClient/src/Arcane/Client/RuntimePresentation.hpp`
- Create: `ArcaneClient/src/Arcane/Client/ClientRuntime.hpp`, `ArcaneClient/src/Arcane/Client/ClientRuntime.cpp`
- Modify: the moved `Runtime.hpp` (drop `:93` `AudioSystem`, `:168-230` render/camera/input/ImGui bridges, `:250` `ResetAudio`; add the hooks surface), `Runtime.cpp` (drop the presentation forwarders, the `RuntimePresentation` member/include and `RenderSystems.hpp:16`; `InstallEngineSystems` installs two; `ClearSystems` calls the hook)
- Modify: the moved `PluginHost.cpp:12,71-107,195-202,244-249` (no `imgui.h`; `UiContextGuard` over hooks; teardown/refresh through hooks)
- Modify: `ArcaneRuntime/src/RuntimeApp.hpp:146`, `RuntimeApp.cpp` (all `Runtime&` passes), `ArcaneEditor/src/App/EditorApp.hpp:614`, `EditorApp*.cpp` (24 `*m_runtime` passes; see Step 6)
- Modify: `ArcaneTests/src/test_main.cpp:32`, `ArcaneTests/src/RuntimeEngineSystemsTest.cpp` (re-pin), the tests that use presentation members (Step 7's grep)
- Create: `ArcaneTests/src/ClientRuntimeTest.cpp`

**Interfaces:**
- Consumes: Task 3's `Runtime(ProcessContext&, bool)`; `RuntimePresentation` (Task 1); `IClientHooks` (below).
- Produces:
```cpp
// Arcane/Plugin/ClientHooks.hpp (Core)
namespace Arcane {
    struct EngineContext;
    struct IClientHooks {                       // implemented by ClientRuntime; null on a headless host
        virtual ~IClientHooks() = default;
        virtual void* SaveUiContext() noexcept = 0;               // ImGui::GetCurrentContext()
        virtual void  RestoreUiContext(void* saved) noexcept = 0; // ImGui::SetCurrentContext(saved)
        virtual void  OnModuleTeardown() noexcept = 0;            // drop plugin-created audio handles (ResetAudio)
        virtual void  OnSystemsCleared() noexcept = 0;            // reinstall the presentation-side engine systems
        virtual void  FillEngineContext(EngineContext& ctx) noexcept = 0;   // the four ImGui void*s
    };
}
// Arcane/Base/Runtime.hpp (Core) -- headless surface as today, plus:
//   void          AttachClient(ClientRuntime* client, IClientHooks* hooks) noexcept;
//   ClientRuntime* Client()      const noexcept;   // null on the server
//   IClientHooks*  ClientHooks() const noexcept;
// Arcane/Client/ClientRuntime.hpp (Client)
//   explicit ClientRuntime(ProcessContext& process, bool enableAudioDevice = false);
//   Runtime& Core() noexcept;
//   AudioSystem / SetInputSnapshot / Input / SetImGui / ImGuiContext / ImGuiAlloc / ImGuiFree / ImGuiUserData /
//   SetRenderContext / SetSpriteMaterials / SetSpriteTable / SetMeshTable / SetMeshMaterials /
//   SetCamera / CameraOffset / CameraZoom / ResetAudio   -- verbatim signatures from today's Runtime.hpp:93-230,250
//   + one-line inline aliases for every headless Runtime method (P5)
```

- [ ] **Step 1: RED.** Create `ArcaneTests/src/ClientRuntimeTest.cpp`:

```cpp
// The Runtime/ClientRuntime split (spec 2026-09-15 s2): a headless Runtime has NO
// presentation -- no client, no hooks, an EMPTY render scheduler; a ClientRuntime
// owns one, attaches itself as the client hooks, and keeps RenderSubmissionSystem
// installed across ClearSystems (which PluginHost -- now Core -- calls on every
// module unload/reload).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Render/RenderSystems.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include "Helpers/TestTypeContext.hpp"

TEST_CASE("a headless Runtime carries no client and installs only the two headless engine systems", "[runtime][client]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    CHECK(rt.Client() == nullptr);
    CHECK(rt.ClientHooks() == nullptr);
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem>());
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>());
    CHECK_FALSE(rt.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
    rt.ClearSystems();
    CHECK_FALSE(rt.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
}

TEST_CASE("ClientRuntime owns a Runtime, attaches as its client, and keeps render submission across ClearSystems", "[runtime][client]")
{
    Arcane::ClientRuntime crt(Arcane::Test::Process());
    Arcane::Runtime& core = crt.Core();
    CHECK(core.Client() == &crt);
    CHECK(core.ClientHooks() != nullptr);
    CHECK(core.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
    core.ClearSystems();                                   // the hook path PluginHost takes
    CHECK(core.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
    CHECK(core.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem>());
    // the aliases (P5) are the same objects
    CHECK(&crt.Registry() == &core.Registry());
    CHECK(&crt.Loop()     == &core.Loop());
}

TEST_CASE("ClientRuntime's ImGui handoff reaches an EngineContext only through the hooks", "[runtime][client]")
{
    Arcane::ClientRuntime crt(Arcane::Test::Process());
    int a = 0, b = 0, c = 0, d = 0;
    crt.SetImGui(&a, &b, &c, &d);
    Arcane::EngineContext ctx{};
    crt.Core().ClientHooks()->FillEngineContext(ctx);
    CHECK(ctx.imguiContext == &a); CHECK(ctx.imguiAlloc == &b); CHECK(ctx.imguiFree == &c); CHECK(ctx.imguiUserData == &d);
    Arcane::Runtime bare(Arcane::Test::Process());
    CHECK(bare.ClientHooks() == nullptr);                  // a headless host hands the module null ImGui, as before
}
```
(`EngineContext` comes through `Arcane/Plugin/PluginABI.hpp`, included by `Runtime.hpp`'s consumers — add the include explicitly.) Build: RED at compile — `Arcane/Client/ClientRuntime.hpp: No such file`.

- [ ] **Step 2: `ClientHooks.hpp`** (Core) — the interface exactly as in **Interfaces**, with this header comment: "The ONE Core→Client reach-back (plan 1 ruling P6). Defined here, implemented by ClientRuntime; null on a headless host, and every Core call site null-checks. Mosaic::IWorkScheduler is the precedent for an interface below the seam implemented above it."

- [ ] **Step 3: `Runtime` becomes headless and moves to Core.**

```bash
git mv ArcaneClient/src/Arcane/Base/Runtime.hpp ArcaneCore/src/Arcane/Base/Runtime.hpp
git mv ArcaneClient/src/Arcane/Base/Runtime.cpp ArcaneCore/src/Arcane/Base/Runtime.cpp
git mv ArcaneClient/src/Arcane/Plugin/PluginHost.hpp ArcaneCore/src/Arcane/Plugin/PluginHost.hpp
git mv ArcaneClient/src/Arcane/Plugin/PluginHost.cpp ArcaneCore/src/Arcane/Plugin/PluginHost.cpp
mkdir -p ArcaneClient/src/Arcane/Client
git mv ArcaneClient/src/Arcane/Base/RuntimePresentation.hpp ArcaneClient/src/Arcane/Client/RuntimePresentation.hpp
sed -i 's/\bARCANE_API\b/ARCANE_CORE_API/g; s#<Arcane/Base/Api.hpp>#<Arcane/Core/Api.hpp>#' ArcaneCore/src/Arcane/Base/Runtime.hpp ArcaneCore/src/Arcane/Plugin/PluginHost.hpp
```
In `Runtime.hpp`: delete `AudioSystem()` (`:93`), the render bridge + camera bridge + input bridge + ImGui handoff blocks (`:168-230`), `ResetAudio` (`:250`), the `Batcher2D`/`SpriteEntry`/`MeshEntry`/`ResolvedMeshMaterial`/`Audio::AudioDevice`/`InputSnapshot` forward declarations, `<glm/glm.hpp>` stays (`ResolvedGravity`). Add, after `Configuration()`:

```cpp
        // --- the client seam (Core-DLL split, spec s2 + plan 1 P6) ---
        // A ClientRuntime (ArcaneClient.dll) attaches itself here at construction. Core
        // never dereferences `client` (it is a forward-declared Client type) -- it hands
        // it to the module through EngineContext (Task 5) -- and reaches presentation
        // ONLY through `hooks`. Both null on a headless host (ArcaneServer, a bare
        // Runtime in a test, the editor's embedded server world).
        void           AttachClient(ClientRuntime* client, IClientHooks* hooks) noexcept;
        ClientRuntime* Client()      const noexcept;
        IClientHooks*  ClientHooks() const noexcept;
```
with `class ClientRuntime; struct IClientHooks;` forward-declared, and the `InstallEngineSystems` comment (`:252-261`) rewritten: "the engine's HEADLESS pair -- PhysicsSystem then TransformPropagationSystem into fixedUpdate. RenderSubmissionSystem is presentation and is ClientRuntime's to install (it does, at construction and on every OnSystemsCleared), so a Core-only host has exactly the systems it can execute." In `Runtime.cpp`: delete the forwarders and the `presentation` member; `Impl` gains `ClientRuntime* client = nullptr; IClientHooks* hooks = nullptr;`; drop includes `:5` (RuntimePresentation), `:16` (RenderSystems), `Input/InputSnapshot.hpp`; `InstallEngineSystems` loses the `render` block; `ClearSystems` ends with `if (m_impl->hooks) m_impl->hooks->OnSystemsCleared();`; the ctor keeps `Log::InstallMosaicSink(); Assert::InstallMosaicHandler();` (now Core's copies — harmless beside `Log::Init`'s own install) and `Astra::SetTypeContext(context, Resident)` (Core's slot; idempotent after `ProcessContext::Create`). Implement the three new methods on `m_impl`.

- [ ] **Step 4: `PluginHost` in Core.** `PluginHost.cpp`: delete `#include <imgui.h>` (`:12`); replace `ImGuiContextGuard` (`:100-107`) with

```cpp
        // Same contract as the old ImGuiContextGuard (comment above kept): restore
        // whatever UI context was current before a call into PluginHost. Core knows
        // no ImGui; the ClientRuntime's hooks do. Null hooks = headless = nothing to save.
        struct UiContextGuard
        {
            explicit UiContextGuard(IClientHooks* h) noexcept : hooks(h), saved(h ? h->SaveUiContext() : nullptr) {}
            ~UiContextGuard() noexcept { if (hooks) hooks->RestoreUiContext(saved); }
            UiContextGuard(const UiContextGuard&) = delete;
            UiContextGuard& operator=(const UiContextGuard&) = delete;
            IClientHooks* hooks;
            void*         saved;
        };
```
and every `const ImGuiContextGuard imguiGuard;` becomes `const UiContextGuard uiGuard(m_impl->runtime.ClientHooks());`. `RefreshContext()` (`:195-202`): `ctx.abiVersion = kGamePluginABIVersion; ctx.imguiContext = ctx.imguiAlloc = ctx.imguiFree = ctx.imguiUserData = nullptr; if (IClientHooks* h = runtime.ClientHooks()) h->FillEngineContext(ctx);`. `TeardownImage` (`:249`) and `Unload` (`:594`): `runtime.ResetAudio();` → `if (IClientHooks* h = runtime.ClientHooks()) h->OnModuleTeardown();`. Nothing else in the file names a presentation type (verify: `grep -n "ImGui\|Audio\|Batcher" ArcaneCore/src/Arcane/Plugin/PluginHost.cpp` → the comment lines only).

- [ ] **Step 5: `ClientRuntime`.** `ClientRuntime.hpp` per **Interfaces** (`class ARCANE_API ClientRuntime final : private IClientHooks`, members `Runtime m_core; RuntimePresentation m_pres;` in that order — the presentation destructs FIRST, before the substrate its audio handles came from; the pimpl warning-4251 push/pop as `Runtime.hpp:46-49` had). `ClientRuntime.cpp`:

```cpp
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Render/RenderSystems.hpp>       // RenderSubmissionSystem -- instantiated IN this module
#include <Arcane/Scene/SceneResources.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>
#include <imgui.h>
#include <tuple>
namespace Arcane
{
    ClientRuntime::ClientRuntime(ProcessContext& process, bool enableAudioDevice)
        : m_core(process)
    {
        // THIS module's (ArcaneClient.dll's) Astra slot + Mosaic routing -- the same
        // three installs Runtime's ctor performed when it lived here. Resident: this
        // DLL never unmaps either.
        Astra::SetTypeContext(&process.TypeContext(), Astra::ModuleResidency::Resident);
        Log::InstallMosaicSink();
        Assert::InstallMosaicHandler();
        m_core.AttachClient(this, this);
        InstallRenderSystems();
        m_pres.InitAudio(&m_core.AssetsFacade(), enableAudioDevice);
    }
    ClientRuntime::~ClientRuntime() { m_core.AttachClient(nullptr, nullptr); }

    void ClientRuntime::InstallRenderSystems()
    {
        auto& render = m_core.Schedulers().render;
        if (!render.HasSystem<RenderSubmissionSystem>())
            std::ignore = render.AddSystem<RenderSubmissionSystem>();
    }
    // IClientHooks
    void* ClientRuntime::SaveUiContext() noexcept              { return ImGui::GetCurrentContext(); }
    void  ClientRuntime::RestoreUiContext(void* s) noexcept    { ImGui::SetCurrentContext(static_cast<ImGuiContext*>(s)); }
    void  ClientRuntime::OnModuleTeardown() noexcept           { ResetAudio(); }
    void  ClientRuntime::OnSystemsCleared() noexcept           { InstallRenderSystems(); }
    void  ClientRuntime::FillEngineContext(EngineContext& ctx) noexcept
    { ctx.imguiContext = m_pres.imguiContext; ctx.imguiAlloc = m_pres.imguiAlloc; ctx.imguiFree = m_pres.imguiFree; ctx.imguiUserData = m_pres.imguiUserData; }
    // presentation surface: the bodies from the old Runtime.cpp:263-332,493-496, on m_pres / m_core
    Audio::AudioDevice& ClientRuntime::AudioSystem() noexcept { return m_pres.audio; }
    void ClientRuntime::ResetAudio() noexcept { m_pres.ResetAudio(&m_core.AssetsFacade()); }
    void ClientRuntime::SetInputSnapshot(const InputSnapshot& s) noexcept { m_pres.input = s; }
    const InputSnapshot& ClientRuntime::Input() const noexcept { return m_pres.input; }
    void ClientRuntime::SetImGui(void* c, void* a, void* f, void* u) noexcept { m_pres.imguiContext = c; m_pres.imguiAlloc = a; m_pres.imguiFree = f; m_pres.imguiUserData = u; }
    void* ClientRuntime::ImGuiContext() const noexcept { return m_pres.imguiContext; }
    void* ClientRuntime::ImGuiAlloc() const noexcept { return m_pres.imguiAlloc; }
    void* ClientRuntime::ImGuiFree() const noexcept { return m_pres.imguiFree; }
    void* ClientRuntime::ImGuiUserData() const noexcept { return m_pres.imguiUserData; }
    void ClientRuntime::SetCamera(glm::vec2 o, float z) noexcept { m_pres.cameraOffset = o; m_pres.cameraZoom = z; }
    glm::vec2 ClientRuntime::CameraOffset() const noexcept { return m_pres.cameraOffset; }
    float ClientRuntime::CameraZoom() const noexcept { return m_pres.cameraZoom; }
    void ClientRuntime::SetRenderContext(Batcher2D* batcher)
    {
        m_core.Registry().SetResource<RenderContext2D>(RenderContext2D{batcher, m_pres.cameraOffset, m_pres.cameraZoom, static_cast<float>(m_core.Loop().Alpha())});
    }
    void ClientRuntime::SetSpriteMaterials(const std::unordered_map<Guid, std::uint16_t>* m) { m_core.Registry().SetResource<SpriteMaterialTable>(SpriteMaterialTable{m}); }
    void ClientRuntime::SetSpriteTable(const std::unordered_map<Guid, SpriteEntry>* s)        { m_core.Registry().SetResource<SpriteTable>(SpriteTable{s}); }
    void ClientRuntime::SetMeshTable(const std::unordered_map<Guid, MeshEntry>* m)             { m_core.Registry().SetResource<MeshTable>(MeshTable{m}); }
    void ClientRuntime::SetMeshMaterials(const std::unordered_map<Guid, ResolvedMeshMaterial>* m) { m_core.Registry().SetResource<MeshMaterialTable>(MeshMaterialTable{m}); }
}
```
Keep each method's original doc comment beside its declaration in the header (they explain the module rule for the `SetResource` calls, which still holds: this TU is ArcaneClient.dll's and its slot is installed above).

- [ ] **Step 6: The hosts.** `RuntimeApp.hpp:146` / `EditorApp.hpp:614`: `std::optional<Arcane::ClientRuntime> m_runtime;` (+ `#include <Arcane/Client/ClientRuntime.hpp>`). Both `StageRuntimeCreate`: `ctx.runtime = &m_runtime->Core();`. Both `plugin_load` (`RuntimeApp.cpp:207`, `EditorApp.cpp:1006`, `EditorAppProject.cpp:3043`): `m_plugin.emplace(m_runtime->Core(), …)`. Then build and fix every `cannot convert from 'std::optional<Arcane::ClientRuntime>' / 'ClientRuntime' to 'Runtime &'` by writing `m_runtime->Core()` at that site — expected ~24 sites (`grep -c "\*m_runtime\b"` = 24 today: `HostBoot::BootScene(*m_runtime, …)`, `m_play.Play/Stop(*m_runtime, …)`, `EditModeSchedule`, `SceneRenderResolver`, `CommandStack` bindings, …). Presentation calls (`SetImGui` ×2, `SetInputSnapshot`, `SetCamera` ×4, `CameraOffset` ×4, `CameraZoom`, `AudioSystem`, `SetRenderContext`) compile unchanged on the alias-less native surface; headless calls compile through the P5 aliases.

- [ ] **Step 7: Tests.** `test_main.cpp:32`: `Arcane::ClientRuntime pin(Arcane::Test::Process());` (installs ArcaneClient.dll's slot; Core's is `Process()`'s). `RuntimeEngineSystemsTest.cpp`: the "installed set" case splits into the two cases Step 1 pins (delete or re-pin its render assertions against a `ClientRuntime`). Then:

```bash
grep -rln "SetRenderContext\|SetSpriteTable\|SetMeshTable\|SetSpriteMaterials\|SetMeshMaterials\|SetCamera\|AudioSystem\|SetInputSnapshot\|SetImGui\|ResetAudio" ArcaneTests/src ArcaneEditor/src
```
Every listed TEST file constructs `Arcane::ClientRuntime` instead of `Arcane::Runtime` and passes `.Core()` where a `Runtime&` is required (compiler-driven, as Step 6). Editor `.cpp` files in that list are already handled by Step 6.

- [ ] **Step 8: Regenerate, build both configs, suites, gate, commit.** `premake5.exe vs2026`; Debug build `0 Error(s)`; `./ArcaneTests.exe "[runtime],[client],[hotreload],[editor],[plugin]"` → passed; `~[gpu]` `-r json` → `check-baselines.ps1` (`+3` cases from `ClientRuntimeTest`, minus/plus the `RuntimeEngineSystemsTest` re-pin — record); `ReferenceProject.slnx -t:Rebuild` Debug + `Arcane.slnx` Debug + the UNFILTERED suite (the `[witness][gpu]` hosts boot through `ClientRuntime`); Release the same; `golden-gate.ps1` both configs → 4/4 `diffCount=0` (this task and Task 1 are the two the spec names as the gate's reason, §9). Gacha Game `arcbuild build` for the slot's config: Aphelyon.cpp names only `GameModule` + ImGui → still compiles (its `Engine()` is the Core `Runtime`).

```bash
git add ArcaneCore ArcaneClient ArcaneTests ArcaneRuntime ArcaneEditor
git commit -m "feat(core-dll): Runtime (Core, headless) / ClientRuntime (Client) split -- IClientHooks is the one reach-back, PluginHost moves to Core, hosts own a ClientRuntime (plan 1 Task 4)"
```

---

### Task 5: The module contract — role-masked system factories, `NetMode`, `HasAuthority()`, N-Runtime hot reload, ABI 30

**Files:**
- Create: `ArcaneCore/src/Arcane/Plugin/SystemFactory.hpp`, `ArcaneCore/src/Arcane/Plugin/SystemFactory.cpp`, `ArcaneCore/src/Arcane/Sim/NetDriver.hpp`
- Modify: `ArcaneCore/src/Arcane/Base/ProcessContext.{hpp,cpp}` (+ `SystemFactories()`), `ArcaneCore/src/Arcane/Base/Runtime.{hpp,cpp}` (ctor `NetMode`, `Mode`/`SetNetMode`/`HasAuthority`, `SetNetDriver`/`NetDriver`, `InstantiateModuleSystems`), `ArcaneCore/src/Arcane/Plugin/PluginHost.{hpp,cpp}` (P9), `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:832,846-864` (ledger + bump + `EngineContext`), `ArcaneCore/src/Arcane/Plugin/GameModule.hpp` (`RegisterSystem`, `Process()`, `Client()`)
- Modify: `ArcaneTests/plugins/HotReloadShared.hpp`, `ArcaneTests/plugins/HotReloadPlugin.cpp:42-50`
- Create: `ArcaneTests/src/RoleMaskTest.cpp`, `ArcaneTests/src/MultiRuntimeReloadTest.cpp`
- Modify: `ArcaneRuntime/src/RuntimeApp.cpp:207`, `ArcaneEditor/src/App/EditorApp.cpp:1006`, `ArcaneEditor/src/App/EditorAppProject.cpp:3043` (PluginHost's new ctor + attach), `ArcaneTests/src/PluginHostTest.cpp` + `PluginLoadDiagnosticsTest.cpp` (same)
- Modify: `ReferenceProject/ReferenceProject.arcproj:6`, Gacha `Game/Aphelyon.arcproj:6` (`"abi": 30`, Gacha commit)

**Interfaces:**
- Consumes: `Astra::SystemScheduler::{AddSystem<T>(args…), HasSystem<T>()}`, `SystemSchedulers` (Sim), Task 4's `Runtime`/`PluginHost`.
- Produces:
```cpp
// Arcane/Plugin/SystemFactory.hpp (Core)
namespace Arcane {
    enum class NetMode  : std::uint8_t { Standalone, DedicatedServer, ListenServer, Client };
    enum class RoleMask : std::uint8_t { Server = 1, Client = 2, Both = 3 };
    enum class SystemPhase : std::uint8_t { FixedUpdate, Update, Render };
    [[nodiscard]] constexpr RoleMask RolesOf(NetMode m) noexcept;       // Standalone->Both, DedicatedServer->Server, ListenServer->Both, Client->Client
    [[nodiscard]] constexpr bool     RoleMatches(RoleMask mask, NetMode m) noexcept;   // (mask & RolesOf(m)) != 0
    [[nodiscard]] ARCANE_CORE_API const char* ToString(NetMode m) noexcept;
    struct SystemFactoryEntry {
        std::string  name;      // the system type's name (log + census)
        RoleMask     mask;
        SystemPhase  phase;
        std::function<void(Astra::SystemScheduler&)> instantiate;   // AddSystem<T>(args...) -- lives in the MODULE; cleared before unmap (P8)
        const void*  owner;     // the registering image (PluginHost clears by owner)
    };
    class ARCANE_CORE_API SystemFactoryTable {
    public:
        void Add(SystemFactoryEntry e);
        void ClearOwner(const void* owner) noexcept;
        [[nodiscard]] std::size_t Size() const noexcept;
        [[nodiscard]] std::span<const SystemFactoryEntry> Entries() const noexcept;
        // Runs every entry whose mask matches `mode` against the right phase scheduler; returns how many.
        std::size_t InstantiateInto(SystemSchedulers& into, NetMode mode) const;
    };
}
// ProcessContext: SystemFactoryTable& SystemFactories() noexcept;
// Arcane/Sim/NetDriver.hpp (Core): struct INetDriver { virtual ~INetDriver() = default; virtual bool IsActive() const noexcept = 0; };
// Runtime:
//   explicit Runtime(ProcessContext& process, NetMode mode = NetMode::Standalone);
//   NetMode Mode() const noexcept;  bool HasAuthority() const noexcept;   // != Client
//   void SetNetMode(NetMode m);   // ClearSystems (engine pair + hooks reinstall) then re-instantiate the module factories for m
//   std::size_t InstantiateModuleSystems();   // from process.SystemFactories() for Mode(); idempotent (AlreadyRegistered ignored)
//   void SetNetDriver(INetDriver* d) noexcept;  INetDriver* NetDriver() const noexcept;   // test double until the replication arc
//   ProcessContext& Process() noexcept;
// PluginHost:
//   PluginHost(ProcessContext& process, std::filesystem::path sourceDllPath);
//   void AttachRuntime(Runtime& rt);   // FIRST attached = PRIMARY (ctx->engine); a later attach on a loaded host instantiates factories into it
//   void DetachRuntime(Runtime& rt) noexcept;
//   [[nodiscard]] std::span<Runtime* const> Runtimes() const noexcept;
// EngineContext (ABI 30): + ProcessContext* process; + ClientRuntime* client (null on the server); + NetMode netMode (the primary's)
// GameModule (SDK): ProcessContext& Process(); ClientRuntime* Client();
//   template <class System, class... Args> void RegisterSystem(RoleMask mask, SystemPhase phase, Args... args);   // explicit line in OnInit
```

- [ ] **Step 1: RED — the shared probe systems + the role-mask test.** `HotReloadShared.hpp` gains, after `Pulse`:

```cpp
    // Role-masked probe systems (spec 2026-09-15 s4, s9). Defined in the SHARED header
    // for the same reason Pulse is: Astra keys a system by a hash of its type NAME, so
    // the test exe's HasSystem<ServerOnlyTick>() resolves the system the PLUGIN
    // registered. Each stamps a distinct counter so behaviour, not just presence, is
    // observable per world.
    struct RoleCounters { int serverTicks = 0; int clientTicks = 0; };
    ASTRA_REFLECT_TYPE(RoleCounters)
        ASTRA_REFLECT_FIELD(RoleCounters, serverTicks)
        ASTRA_REFLECT_FIELD(RoleCounters, clientTicks)
    ASTRA_END_REFLECT_TYPE()
    struct ServerOnlyTick { void operator()(Astra::Registry& r) { r.CreateView<RoleCounters>().ForEach([](Astra::Entity, RoleCounters& c) { ++c.serverTicks; }); } };
    struct ClientOnlyTick { void operator()(Astra::Registry& r) { r.CreateView<RoleCounters>().ForEach([](Astra::Entity, RoleCounters& c) { ++c.clientTicks; }); } };
```
(+ `#include <Astra/Registry/Registry.hpp>`.) `HotReloadPlugin.cpp`: `ARCANE_COMPONENT(Arcane::HotReloadTest::RoleCounters)` beside `Pulse`'s, and in `OnInit`, before `return true;`:

```cpp
            // The s4 contract: factories registered ONCE per DLL load, with an explicit
            // mask; each Runtime instantiates what its NetMode matches.
            RegisterSystem<ServerOnlyTick>(Arcane::RoleMask::Server, Arcane::SystemPhase::FixedUpdate);
            RegisterSystem<ClientOnlyTick>(Arcane::RoleMask::Client, Arcane::SystemPhase::FixedUpdate);
```
Create `ArcaneTests/src/RoleMaskTest.cpp`:

```cpp
// Spec 2026-09-15 s4 + s9: two Runtimes, ONE loaded module, role-masked
// instantiation; ListenServer takes both; HasAuthority across the four modes; and
// the net-mode/launch-flag INDEPENDENCE the spec names as a bug class (UE's
// IsRunningDedicatedServer vs NetMode).
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>
#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"
#include <filesystem>
using namespace Arcane::HotReloadTest;

namespace
{
    void Step(Arcane::Runtime& rt, Arcane::PluginHost& host, int k)
    { for (int i = 0; i < k; ++i) rt.Loop().Advance(1.0 / 60.0, [&](double dt){ host.FixedUpdateAll(dt); }, [&](double,double){}); }
    RoleCounters Read(Arcane::Runtime& rt)
    { RoleCounters out; rt.Registry().CreateView<RoleCounters>().ForEach([&](Astra::Entity, RoleCounters& c){ out = c; }); return out; }
}

TEST_CASE("HasAuthority: every mode but Client", "[runtime][netmode]")
{
    using Arcane::NetMode;
    CHECK(Arcane::Runtime(Arcane::Test::Process(), NetMode::Standalone).HasAuthority());
    CHECK(Arcane::Runtime(Arcane::Test::Process(), NetMode::DedicatedServer).HasAuthority());
    CHECK(Arcane::Runtime(Arcane::Test::Process(), NetMode::ListenServer).HasAuthority());
    CHECK_FALSE(Arcane::Runtime(Arcane::Test::Process(), NetMode::Client).HasAuthority());
}

TEST_CASE("net mode and the launch flag are independent: a DedicatedServer Runtime in a non-server process has authority", "[runtime][netmode]")
{
    REQUIRE_FALSE(Arcane::Test::Process().IsDedicatedServerProcess());
    Arcane::Runtime rt(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    CHECK(rt.HasAuthority());
    CHECK(rt.Mode() == Arcane::NetMode::DedicatedServer);
}

TEST_CASE("two Runtimes, one module: the Server-masked system exists only in the server world and the Client-masked only in the client", "[runtime][netmode][hotreload]")
{
    Arcane::Runtime server(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    Arcane::Runtime client(Arcane::Test::Process(), Arcane::NetMode::Client);
    for (auto* rt : { &server, &client }) { rt->Components()->RegisterComponent<Pulse>(); rt->Components()->RegisterComponent<RoleCounters>(); }

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(server);
    host.AttachRuntime(client);
    REQUIRE(host.Load());
    REQUIRE(host.Runtimes().size() == 2);

    CHECK(server.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());
    CHECK_FALSE(server.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    CHECK(client.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    CHECK_FALSE(client.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());

    server.Registry().CreateEntityWith(RoleCounters{});
    client.Registry().CreateEntityWith(RoleCounters{});
    Step(server, host, 3); Step(client, host, 3);
    CHECK(Read(server).serverTicks == 3); CHECK(Read(server).clientTicks == 0);
    CHECK(Read(client).clientTicks == 3); CHECK(Read(client).serverTicks == 0);
    host.Unload();
}

TEST_CASE("ListenServer instantiates BOTH masks in its one Runtime", "[runtime][netmode][hotreload]")
{
    Arcane::Runtime listen(Arcane::Test::Process(), Arcane::NetMode::ListenServer);
    listen.Components()->RegisterComponent<Pulse>(); listen.Components()->RegisterComponent<RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(listen);
    REQUIRE(host.Load());
    CHECK(listen.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());
    CHECK(listen.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    host.Unload();
}

TEST_CASE("the factory table is the process's, cleared when the module unloads", "[runtime][netmode][hotreload]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>(); rt.Components()->RegisterComponent<RoleCounters>();
    const std::size_t before = Arcane::Test::Process().SystemFactories().Size();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    CHECK(Arcane::Test::Process().SystemFactories().Size() == before + 2);
    host.Unload();
    CHECK(Arcane::Test::Process().SystemFactories().Size() == before);   // std::functions into the image are gone BEFORE the unmap
}
```
Create `ArcaneTests/src/MultiRuntimeReloadTest.cpp`:

```cpp
// Spec 2026-09-15 s5: snapshot ALL -> reload the DLL ONCE -> re-run registration ->
// restore ALL. Both worlds' state survives one swap and both are repopulated from
// the re-registered factories; a reload is REFUSED while any attached Runtime
// reports an active net driver (a test double until the replication arc).
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Sim/NetDriver.hpp>
#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"
#include <filesystem>
using namespace Arcane::HotReloadTest;

namespace
{
    struct FakeDriver final : Arcane::INetDriver { bool active = false; bool IsActive() const noexcept override { return active; } };
    int ReadPulse(Arcane::Runtime& rt) { int v = 0; rt.Registry().CreateView<Pulse>().ForEach([&](Astra::Entity, Pulse& p){ v = p.ticks; }); return v; }
    // One sim step for EVERY attached world, with the module's FixedUpdate hook run
    // ONCE per step (it is bound to the primary's world; running it per Runtime would
    // double-count the primary's Pulse).
    void StepAll(Arcane::PluginHost& host, int k)
    {
        for (int i = 0; i < k; ++i)
        {
            bool first = true;
            for (Arcane::Runtime* rt : host.Runtimes())
            {
                rt->Loop().Advance(1.0/60.0, [&](double dt){ if (first) host.FixedUpdateAll(dt); }, [&](double,double){});
                first = false;
            }
        }
    }
}

TEST_CASE("snapshot-all / reload / restore-all across two live Runtimes", "[hotreload][netmode]")
{
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll", std::filesystem::copy_options::overwrite_existing);
    Arcane::Runtime server(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    Arcane::Runtime client(Arcane::Test::Process(), Arcane::NetMode::Client);
    for (auto* rt : { &server, &client }) { rt->Components()->RegisterComponent<Pulse>(); rt->Components()->RegisterComponent<RoleCounters>(); }
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(server); host.AttachRuntime(client);
    REQUIRE(host.Load());
    // The module's OnInit creates its Pulse entity in the PRIMARY (server) world only;
    // give the client world its own state to prove the registry-only restore path.
    client.Registry().CreateEntityWith(Pulse{100});
    server.Registry().CreateEntityWith(RoleCounters{}); client.Registry().CreateEntityWith(RoleCounters{});
    StepAll(host, 2);
    const int serverPulse = ReadPulse(server);   // V1: +1 per step on the primary's own Pulse -> 2
    REQUIRE(serverPulse == 2);

    std::filesystem::copy_file("../HotReloadPluginV2/HotReloadPluginV2.dll", "HotReloadPluginV1.dll", std::filesystem::copy_options::overwrite_existing);
    REQUIRE(host.ForceReload());
    CHECK(ReadPulse(server) == 2);          // primary: module SaveState/LoadState round-trip
    CHECK(ReadPulse(client) == 100);        // secondary: registry snapshot/restore, untouched by the module's OnInit
    CHECK(server.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());   // re-registered factories repopulated BOTH
    CHECK(client.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    StepAll(host, 1);
    CHECK(ReadPulse(server) == 12);         // V2's +10 ran on the restored primary world
    host.Unload();
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll", std::filesystem::copy_options::overwrite_existing);
}

TEST_CASE("hot reload is refused while any attached Runtime has an active net driver", "[hotreload][netmode]")
{
    Arcane::Runtime a(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    Arcane::Runtime b(Arcane::Test::Process(), Arcane::NetMode::Client);
    for (auto* rt : { &a, &b }) { rt->Components()->RegisterComponent<Pulse>(); rt->Components()->RegisterComponent<RoleCounters>(); }
    FakeDriver drv; b.SetNetDriver(&drv);
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(a); host.AttachRuntime(b);
    REQUIRE(host.Load());
    const std::uint32_t gen = host.Generation();
    drv.active = true;
    CHECK_FALSE(host.ForceReload());        // refused: the diagnostic names Runtime #2 (Client)
    CHECK(host.IsLoaded());                 // and the live module is untouched
    CHECK(host.Generation() == gen);
    drv.active = false;
    CHECK(host.ForceReload());
    host.Unload();
}
```
Build: RED at compile — `SystemFactory.hpp`/`NetDriver.hpp` missing, `Runtime(…, NetMode)` unknown, `PluginHost(ProcessContext&, …)` unknown, `RegisterSystem` unknown.

- [ ] **Step 2: `SystemFactory.hpp/.cpp` + `NetDriver.hpp` + `ProcessContext::SystemFactories()`.** Per **Interfaces**. `InstantiateInto`: for each entry, `if (!RoleMatches(e.mask, mode)) continue; e.instantiate(phase == FixedUpdate ? into.fixedUpdate : phase == Update ? into.update : into.render); ++n;`. `ClearOwner` erases by `owner`. `ProcessContext` holds `SystemFactoryTable m_factories;` (`ProcessContext.hpp` includes `SystemFactory.hpp`; the table is Core-owned and outlives every module).

- [ ] **Step 3: `Runtime`.** Ctor `Runtime(ProcessContext&, NetMode = Standalone)`; `Impl` gains `ProcessContext* process; NetMode mode; INetDriver* net = nullptr;`. `InstantiateModuleSystems()` = `process->SystemFactories().InstantiateInto(*schedulers, mode)` (call it at the END of the ctor, after `InstallEngineSystems`, so a Runtime constructed after the module loaded gets its systems). `SetNetMode(m)`: `mode = m; ClearSystems(); InstantiateModuleSystems();` (`ClearSystems` already reinstalls the engine pair and fires `OnSystemsCleared`). `HasAuthority() = mode != NetMode::Client`. `ToString(NetMode)` in `SystemFactory.cpp`.

- [ ] **Step 4: `PluginHost` serves N Runtimes (P9).** `Impl` loses `Runtime& runtime`, gains `ProcessContext& process; std::vector<Runtime*> runtimes;` with `Runtime& Primary()` (`runtimes.front()`, asserted non-empty at `Load`). `ctx.process = &process; ctx.engine = &Primary(); ctx.client = Primary().Client(); ctx.netMode = Primary().Mode();` in `RefreshContext`. Every `runtime.X()` site becomes a loop or a primary call:
  - `TeardownImage` / `Unload`'s shared reset: for every attached rt — `hooks->OnModuleTeardown()`, `rt->ClearSystems()`, `rt->ResetRegistry()`; then `process.SystemFactories().ClearOwner(imageBase)` BEFORE the image unmaps (the `Module::ImageSpan` the purge at `:275` already reads — use `image.base` as the owner key, and pass the same key to the module: `ctx.factoryOwner`? NO — `EngineContext` gains exactly three fields. Instead `SystemFactoryTable::Add` records the owner as the CURRENT image being initialised: `PluginHost` sets `process.SystemFactories().BeginOwner(image.base)` before `Init` and `EndOwner()` after; `Add` stamps the open owner. Document that on the table.)
  - `ReloadPrimary`: refusal FIRST — `for (auto* rt : runtimes) if (rt->NetDriver() && rt->NetDriver()->IsActive()) { ARC_ERROR("plugin: reload refused -- Runtime #{} ({}) has an active net driver (spec 2026-09-15 s5); stop it first", i+1, ToString(rt->Mode())); Diagnostics::Publish("plugin:" + name, {{Warning, Project, "plugin.reload.refused-net-active", …}}); return false; }`. Then snapshot: the primary through the module's `SaveState` (as today); every OTHER rt: `rt->SnapshotRegistry()` into a per-rt byte vector (a failure aborts the reload, keeping the live module, like `SaveState`'s). After `Init` + `LoadState` on the primary: `rt->RestoreRegistry(bytes)` for the others, then `rt->InstantiateModuleSystems()` for ALL attached (the primary included — `Init` registered the factories; nothing has instantiated them yet). Rollback path: same restore loop against the last-good image.
  - `Load`: after `Init` succeeds → `for (auto* rt : runtimes) rt->InstantiateModuleSystems();`.
  - `AttachRuntime` on an already-loaded host: `rt.InstantiateModuleSystems()` immediately. `DetachRuntime`: `ClearSystems` on it and erase; detaching the primary while loaded is refused (`ARC_ERROR`, no-op) — the primary is the module's world.
  Keep `PluginHost(Runtime&, path)` OUT: every caller goes through the new ctor + `AttachRuntime` (the hosts, `PluginHostTest.cpp`, `PluginLoadDiagnosticsTest.cpp`, `RuntimeModulePluginTest.cpp` — mechanical: `Arcane::PluginHost host(rt, p)` → `Arcane::PluginHost host(Arcane::Test::Process(), p); host.AttachRuntime(rt);`).

- [ ] **Step 5: ABI 30 + `EngineContext` + `GameModule`.** `PluginABI.hpp:846-864`: add after `engine`:

```cpp
        // ABI 30 (Core-DLL split, spec 2026-09-15 s2/s3/s4) -- the ONLY three additions:
        Arcane::ProcessContext*  process;        // the process's one (TypeContext, system factories)
        Arcane::ClientRuntime*   client;         // presentation extension; NULL on a headless host (ArcaneServer, an embedded server world)
        Arcane::NetMode          netMode;        // the PRIMARY Runtime's mode -- systems branch on THIS, never on process->IsDedicatedServerProcess()
```
(+ forward declarations, + `#include <Arcane/Plugin/SystemFactory.hpp>` for `NetMode`.) The ledger entry above `:832`:

```cpp
    // v30 (2026-09-15, Core-DLL split): headers a module compiles moved DLLs
    //     (Base/Scene/Plugin/Project/Serialization/Sim/... now export from
    //     ArcaneCore.dll, ARCANE_CORE_API; a module links BOTH import libs, build/
    //     arcane.lua); EngineContext gained process, client and netMode -- exactly
    //     three, tail-appended; and the module contract gained system-factory
    //     registration with role masks (GameModule::RegisterSystem, spec s4) --
    //     each Runtime instantiates what its NetMode matches. A v29 module under a
    //     v30 host would read EngineContext at the old size and register no
    //     factories. Reject the pairing. ReferenceProject.arcproj restamped with
    //     this change; Gacha's Game restamp (29 -> 30) is this plan's Task 5 Gacha
    //     commit, with the Aphelyon.dll rebuild -- not deferred.
    inline constexpr uint32_t kGamePluginABIVersion = 30;
```
`GameModule.hpp`: accessors `ProcessContext& Process() const noexcept { return *Context().process; }`, `ClientRuntime* Client() const noexcept { return Context().client; }`, and

```cpp
        // Register one of this module's systems ONCE per DLL load (spec s4: "systems
        // stay explicit, their order is a design act" -- an explicit line in OnInit,
        // with an explicit mask; Astra's Before/After traits still place it). Every
        // Runtime whose NetMode matches `mask` instantiates it: the primary right
        // after OnInit, any other attached Runtime at attach, and all of them again
        // after a hot reload. The std::function lives in THIS module and PluginHost
        // clears it before the image unmaps.
        template <class System, class... Args>
        void RegisterSystem(RoleMask mask, SystemPhase phase, Args... args)
        {
            Process().SystemFactories().Add(SystemFactoryEntry{
                Astra::TypeID<System>::Name(), mask, phase,
                [args...](Astra::SystemScheduler& s) { std::ignore = s.AddSystem<System>(args...); }, nullptr });
        }
```
`ClassTemplates.cpp`'s system template (`ArcaneEditor/src/Project/ClassTemplates.cpp`) grows its `OnInit` line to `RegisterSystem<{Name}>(Arcane::RoleMask::Both, Arcane::SystemPhase::FixedUpdate);` (was a direct `AddSystem`); pin it in `ClassTemplatesTest.cpp` (one CHECK on the rendered string).

- [ ] **Step 6: Hosts + restamps + rebuilds.** `RuntimeApp.cpp:207` / `EditorApp.cpp:1006` / `EditorAppProject.cpp:3043`: `m_plugin.emplace(*m_process, path); m_plugin->AttachRuntime(m_runtime->Core());` before `AddPlugin`. `ReferenceProject.arcproj:6` → `30`. Regenerate; Debug + Release builds of `Arcane.slnx`; `ReferenceProject.slnx -t:Rebuild` both configs (then Debug last); the three test plugins rebuild inside `Arcane.slnx`. Gacha: `Game/Aphelyon.arcproj:6` → `30`, `arcbuild build --project D:\dev\starworks\Gacha\Game --config <slot's>` → Aphelyon.dll; commit in Gacha: `chore(game): restamp Aphelyon.arcproj to engine ABI 30 (Core-DLL split) and rebuild Aphelyon.dll`.

- [ ] **Step 7: Suites, gate, commit.** `./ArcaneTests.exe "[netmode],[hotreload],[plugin],[runtime],[editor]"` → passed (7 new cases); `~[gpu]` `-r json` + guard (`+7` cases, record assertions); unfiltered once per config (the witnesses prove the restamped module loads under ABI 30); `golden-gate.ps1` both configs → 4/4 `diffCount=0`.

```bash
git add ArcaneCore ArcaneClient ArcaneTests ArcaneRuntime ArcaneEditor ReferenceProject/ReferenceProject.arcproj
git commit -m "feat(core-dll): role-masked system factories, NetMode/HasAuthority, N-Runtime hot reload with the net-driver refusal, EngineContext +3 -- ABI 30 (plan 1 Task 5)"
```

---

### Task 6: `ArcaneServer.exe`, real — Core-only host, fixed-step tick, `--report` census, the server witness

**Files:**
- Create: `ArcaneCore/src/Arcane/Project/ProjectHost.hpp` (P12); Modify: `ArcaneClient/src/Arcane/Host/ProjectBoot.hpp:59-100,168-346` (the five helpers become `using` re-exports)
- Rewrite: `ArcaneServer/src/main.cpp`; Create: `ArcaneServer/src/ServerConfig.hpp`, `ServerConfig.cpp`, `ServerApp.hpp`, `ServerApp.cpp`, `ServerReport.hpp`, `ServerReport.cpp`
- Modify: `premake5.lua:516-525` (the `ArcaneServer` block), `ArcaneTests` `files` list (source-compile `ServerConfig.cpp` + `ServerReport.cpp`, the arcbuild `Request.cpp` precedent at `:979-990`) and `includedirs` (`"%{wks.location}/ArcaneServer/src"`)
- Create: `ArcaneTests/src/ServerConfigTest.cpp`, `ArcaneTests/src/ServerWitnessTest.cpp`
- Modify: `Jenkinsfile` — nothing (ArcaneServer builds inside `Arcane.slnx`; the witness runs in the existing Tests stage)

**Interfaces:**
- Consumes: `Arcane::Cli` (Core), `ProcessContext`, `Runtime(ProcessContext&, NetMode)`, `PluginHost(ProcessContext&, path)` + `AttachRuntime`, `Project`, `HostWitness` (tests).
- Produces:
```cpp
// ArcaneServer/src/ServerConfig.hpp
namespace Arcane::Server {
    struct ServerConfig {
        std::string   projectPath;                 // --project (REQUIRED: a server with nothing to host refuses, like ArcaneRuntime)
        std::string   pluginPath;                  // --plugin override
        std::uint64_t frames        = 0;           // --frames N; 0 = run until terminated (P13)
        double        fixedDtSeconds = 1.0 / 60.0; // --fixed-dt
        std::string   reportPath;                  // --report <json>
        bool          printEngineInfo = false;     // --print-engine-info (same probe as the other hosts, Core-side)
        struct ParseOutcome { std::optional<ServerConfig> config; int exitCode = 0; };
        static ParseOutcome Parse(int argc, char** argv);
    };
}
// ArcaneServer/src/ServerReport.hpp -- the census (schemaVersion 1):
// { "schemaVersion":1, "host":"ArcaneServer", "netMode":"DedicatedServer", "isDedicatedServerProcess":true,
//   "project":{"opened":bool,"name":str,"engineAbi":int}, "module":{"path":str,"loaded":bool,"generation":uint},
//   "framesTicked":uint, "fixedDt":double, "systems":{"fixedUpdate":uint,"update":uint,"render":uint,
//   "hasPhysics":bool,"hasPropagation":bool,"hasRenderSubmission":false},
//   "presentation":{"clientAttached":false,"clientDllLoadedAtBoot":bool,"clientDllLoadedAfterModule":bool},
//   "exitReason":"frames-complete"|"project-open-failed"|"module-load-failed" }
// Arcane/Project/ProjectHost.hpp (Core): namespace Arcane::ProjectHost { VerifySharedTypeContext, GameModule, PluginModules, BootSceneFile (x2), BootSceneResult, BootScene (x2) } -- bodies moved verbatim from ProjectBoot.hpp
```

- [ ] **Step 1: RED — the config units and the witness.** `ArcaneTests/src/ServerConfigTest.cpp`:

```cpp
// ArcaneServer's CLI (spec 2026-09-15 s6: "mirrors the other hosts"). Core-side --
// HostConfig is Client (it names a GraphicsBackend), so the server owns its own
// vocabulary over the same Arcane::Cli. Source-compiled into the tests like
// arcbuild's Request.cpp.
#include <catch2/catch_test_macros.hpp>
#include "ServerConfig.hpp"
#include <vector>
namespace
{
    Arcane::Server::ServerConfig::ParseOutcome Parse(std::vector<const char*> args)
    { args.insert(args.begin(), "ArcaneServer"); return Arcane::Server::ServerConfig::Parse(static_cast<int>(args.size()), const_cast<char**>(args.data())); }
}
TEST_CASE("ServerConfig: --project is required; the defaults are the documented ones", "[server]")
{
    CHECK_FALSE(Parse({}).config.has_value());
    CHECK(Parse({}).exitCode == 2);
    auto ok = Parse({"--project", "ReferenceProject"});
    REQUIRE(ok.config);
    CHECK(ok.config->projectPath == "ReferenceProject");
    CHECK(ok.config->frames == 0);
    CHECK(ok.config->fixedDtSeconds == 1.0 / 60.0);
    CHECK(ok.config->reportPath.empty());
}
TEST_CASE("ServerConfig: --frames, --fixed-dt, --report, --plugin parse; a non-positive --fixed-dt is refused", "[server]")
{
    auto ok = Parse({"--project", "P", "--frames", "30", "--fixed-dt", "0.02", "--report", "r.json", "--plugin", "X.dll"});
    REQUIRE(ok.config);
    CHECK(ok.config->frames == 30); CHECK(ok.config->fixedDtSeconds == 0.02);
    CHECK(ok.config->reportPath == "r.json"); CHECK(ok.config->pluginPath == "X.dll");
    CHECK_FALSE(Parse({"--project", "P", "--fixed-dt", "0"}).config.has_value());
    CHECK_FALSE(Parse({"--project", "P", "--fixed-dt", "-1"}).config.has_value());
}
TEST_CASE("ServerConfig: --print-engine-info needs no --project; --help exits 0", "[server]")
{
    CHECK(Parse({"--print-engine-info"}).config->printEngineInfo);
    auto h = Parse({"--help"}); CHECK_FALSE(h.config); CHECK(h.exitCode == 0);
}
```
`ArcaneTests/src/ServerWitnessTest.cpp`:

```cpp
// The s6 witness: spawn the REAL staged ArcaneServer.exe and read its census. Same
// harness as the runtime witnesses (Helpers/HostWitness.hpp), same fresh-copy
// hygiene; [server], not [gpu] -- this host has no device (P14).
#include "Helpers/HostWitness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
using namespace Arcane::Test;
namespace
{
    std::filesystem::path StagedServerDir()
    {
        const std::filesystem::path p = std::filesystem::absolute("../ArcaneServer");
        INFO("staged ArcaneServer not found -- build Arcane.slnx first: " << p.string());
        REQUIRE(std::filesystem::exists(p / "ArcaneServer.exe"));
        return p;
    }
}
TEST_CASE("S1: ArcaneServer opens the project, loads the module, ticks N frames headless, and constructs no presentation", "[witness][server]")
{
    WitnessScratch scratch(StagedServerDir(), "s1-census");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneServer.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "server-report.json";
    inv.args = { "--project", "ReferenceProject", "--frames", "30", "--report", inv.reportPath.generic_string() };
    inv.hardCapMs = 60000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    CHECK(run.exitCode == 0);
    const auto& r = run.report;
    CHECK(r.at("schemaVersion") == 1);
    CHECK(r.at("host") == "ArcaneServer");
    CHECK(r.at("netMode") == "DedicatedServer");
    CHECK(r.at("isDedicatedServerProcess") == true);
    CHECK(r.at("project").at("opened") == true);
    CHECK(r.at("project").at("name") == "ReferenceProject");
    CHECK(r.at("module").at("loaded") == true);
    CHECK(r.at("framesTicked") == 30);
    CHECK(r.at("systems").at("hasPhysics") == true);
    CHECK(r.at("systems").at("hasPropagation") == true);
    CHECK(r.at("systems").at("hasRenderSubmission") == false);
    CHECK(r.at("systems").at("render") == 0);
    CHECK(r.at("presentation").at("clientAttached") == false);
    CHECK(r.at("presentation").at("clientDllLoadedAtBoot") == false);   // the exe's link line has no ArcaneClient (spec s6)
    CHECK(r.at("exitReason") == "frames-complete");
}
TEST_CASE("S2: ArcaneServer refuses a missing project with a report that says so", "[witness][server]")
{
    WitnessScratch scratch(StagedServerDir(), "s2-no-project");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneServer.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "server-report.json";
    inv.args = { "--project", "DoesNotExist", "--frames", "1", "--report", inv.reportPath.generic_string() };
    WitnessRun run = RunWitness(inv);
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    CHECK(run.exitCode != 0);
    CHECK(run.report.at("project").at("opened") == false);
    CHECK(run.report.at("exitReason") == "project-open-failed");
}
```
Build: RED at compile (`ServerConfig.hpp` missing); the witness would fail at `REQUIRE(exists(ArcaneServer.exe))` under the old stub's staging (there is none).

- [ ] **Step 2: `ProjectHost.hpp` (Core).** Move `VerifySharedTypeContext`, `GameModule`, `PluginModules`, both `BootSceneFile`s, `BootSceneResult`, `Detail::ApplySceneFile`, both `BootScene`s from `ProjectBoot.hpp` into `ArcaneCore/src/Arcane/Project/ProjectHost.hpp` under `namespace Arcane::ProjectHost`, bodies verbatim (they include `Project.hpp`, `AssetId.hpp`, `SceneAsset.hpp`, `Components.hpp`, `Log.hpp`, `Runtime.hpp` — all Core). In `ProjectBoot.hpp`, replace the moved bodies with `#include <Arcane/Project/ProjectHost.hpp>` and, inside `namespace Arcane::HostBoot`, `using ProjectHost::VerifySharedTypeContext; using ProjectHost::GameModule; using ProjectHost::PluginModules; using ProjectHost::BootSceneFile; using ProjectHost::BootSceneResult; using ProjectHost::BootScene;` — every Client/host/test caller compiles unchanged (`grep -rn "HostBoot::\(GameModule\|PluginModules\|BootScene\|VerifySharedTypeContext\)"` names them all; none needs an edit).

- [ ] **Step 3: The host.** `ServerConfig.cpp` — an `Arcane::Cli` with `Option("project", "", …)`, `Option("plugin", "", …)`, `Option("frames", "0", …).Type(CliType::Uint)`, `Option("fixed-dt", "0.016666666666666666", …).Type(CliType::Double)`, `Option("report", "", …)`, `Flag("print-engine-info", …)`; after `Parse`: refuse `fixed-dt <= 0` (print the reason + usage, exit 2), refuse an empty `--project` unless `--print-engine-info` (exit 2). `ServerReport.{hpp,cpp}`: a struct with the census fields + `ToJson()` (compact, `error_handler_t::replace`, as `VerifyReport::ToJson`) + `WriteTo(path)`. `ServerApp.{hpp,cpp}`:

```cpp
    int ServerApp::Run()
    {
        ServerReport rep;                          // every field defaulted honestly (opened=false, loaded=false, ...)
        rep.clientDllLoadedAtBoot = ::GetModuleHandleW(L"ArcaneClient.dll") != nullptr;   // the exe's own imports, before any module
        Arcane::ProcessContextDesc d; d.isDedicatedServerProcess = true;
        m_process = Arcane::ProcessContext::Create(d);
        if (!m_process) return Finish(rep, "process-context-refused", 1);
        Astra::SetTypeContext(&m_process->TypeContext());                 // this exe's own slot
        m_runtime.emplace(*m_process, Arcane::NetMode::DedicatedServer);   // the ONE authoritative world
        rep.netMode = Arcane::ToString(m_runtime->Mode());
        rep.isDedicatedServerProcess = m_process->IsDedicatedServerProcess();
        if (!m_runtime->OpenProject(m_cfg.projectPath)) return Finish(rep, "project-open-failed", 1);
        const Arcane::Project* proj = m_runtime->CurrentProject();
        rep.projectOpened = true; rep.projectName = proj->Manifest().name; rep.projectAbi = proj->Manifest().engineAbi;
        if (!Arcane::ProjectHost::VerifySharedTypeContext(m_runtime->Registry(), "ArcaneServer.exe")) return Finish(rep, "type-context-mismatch", 1);
        const std::string module = Arcane::ProjectHost::GameModule(proj, m_cfg.pluginPath);
        m_plugin.emplace(*m_process, module.empty() ? std::filesystem::path{} : std::filesystem::path(module));
        m_plugin->AttachRuntime(*m_runtime);
        for (const auto& dll : Arcane::ProjectHost::PluginModules(proj)) m_plugin->AddPlugin(dll);
        rep.modulePath = module;
        if (!m_plugin->Load()) return Finish(rep, "module-load-failed", 1);
        rep.moduleLoaded = true; rep.moduleGeneration = m_plugin->Generation();
        rep.clientDllLoadedAfterModule = ::GetModuleHandleW(L"ArcaneClient.dll") != nullptr;   // P10: the module's import, reported, not hidden
        (void)Arcane::ProjectHost::BootScene(*m_runtime, *proj);
        // Fixed-step tick, forever or --frames N. Wall-clock paced by sleeping the remainder
        // of each step (spec s6: tick rate is a RUNTIME value); the loop is unpaused (fresh).
        for (std::uint64_t f = 0; m_cfg.frames == 0 || f < m_cfg.frames; ++f)
        {
            const auto start = std::chrono::steady_clock::now();
            m_runtime->EnsurePhysics();
            m_runtime->Loop().Advance(m_cfg.fixedDtSeconds, [&](double dt){ m_plugin->FixedUpdateAll(dt); }, [&](double dt, double a){ m_plugin->UpdateAll(dt, a); });
            m_plugin->Poll();
            Arcane::Diagnostics::Heartbeat();
            ++rep.framesTicked;
            std::this_thread::sleep_until(start + std::chrono::duration<double>(m_cfg.fixedDtSeconds));
        }
        rep.fixedUpdate = m_runtime->Schedulers().fixedUpdate.Size(); /* update, render likewise */
        rep.hasPhysics = m_runtime->Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem>();
        rep.hasPropagation = m_runtime->Schedulers().fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>();
        rep.hasRenderSubmission = false;   // by construction: no ClientRuntime exists in this process
        rep.clientAttached = m_runtime->Client() != nullptr;
        return Finish(rep, "frames-complete", 0);
    }
    // Finish: rep.exitReason = reason; if (!m_cfg.reportPath.empty() && !rep.WriteTo(m_cfg.reportPath)) ARC_ERROR(...); return rc;
```
(If `Astra::SystemScheduler` has no `Size()`, count via `HasSystem` on the two engine types and the factory table's matching entries — the report's three integers are then derived, and the test's `render == 0` still holds.) `main.cpp` mirrors `ArcaneRuntime/src/main.cpp:24-110` minus the splash and the `--dump-layout` refusal: `Log::Init`, `InstallMosaicSink`, `InstallMosaicHandler`, `ServerConfig::Parse`, the `--print-engine-info` probe (`EngineInfoJson` lives in `ProjectBoot.hpp` — Client — so print `{"engineAbi":PluginABIVersion(),"build":BuildInfo(),"exePath":ExecutablePathUtf8()}` via nlohmann here, same three keys), `Diagnostics::Install` with `appName = "ArcaneServer"`, a scoped `ServerApp`, `Diagnostics::Shutdown()`. No `D3D12SDKVersion` exports (no D3D12 here).

- [ ] **Step 4: premake.** Replace the `ArcaneServer` stub block (`:516-525`) with a full project: `staticruntime "off"`; `includedirs { "%{prj.location}/src", "%{IncludeDir.ArcaneCore}", nlohmann, spdlog, glm, Astra, enkiTS, Mosaic, Manifold2D }` (headers only for the last three — every Manifold2D/enki object it can reach is created inside ArcaneCore.dll); `links { "ArcaneCore" }` — and NOT `ArcaneClient`, with the P10 comment; `dependson { "arccook" }` is NOT needed (no cooked content is read); `defines` as ArcaneRuntime's minus `IMGUI_API`; `postbuildcommands`: copy `ArcaneCore.dll` AND `ArcaneClient.dll` (P10, with its comment: "the game module links both import libs; the loader needs this beside the exe to MAP ReferenceGame.dll. Nothing in this exe references it; the census reports whether it was loaded before and after the module"), `{COPYDIR} data/EngineConfig`, and the ReferenceProject staging pair (`{MKDIR}`/`{RMDIR}` of `Content`/`Source`/`Verify` then the whole-tree `{COPYDIR}`, copied from ArcaneRuntime's block with the same comment reference). Standard Debug/Release/Dist filters; `fatalwarnings { "4715" }`. Add `ServerConfig.cpp` and `ServerReport.cpp` to `ArcaneTests`'s `files` with a comment in the `Request.cpp` shape, and `"%{wks.location}/ArcaneServer/src"` to its `includedirs`.

- [ ] **Step 5: Build, run, commit.** `premake5.exe vs2026`; Debug build → `0 Error(s)` (the server's link is the proof: an unresolved external here means a Core symbol still lacks `ARCANE_CORE_API` — mark it, do not link Client); `ReferenceProject.slnx -t:Rebuild` Debug + `Arcane.slnx` Debug (restage); from the exe dir: `./ArcaneTests.exe "[server]"` → passed (3 config cases + 2 witnesses); desk check once: `cd bin/Debug-windows-x86_64-md/ArcaneServer && ./ArcaneServer.exe --project ReferenceProject --frames 60 --report out.json; cat out.json` and read it (delete `out.json` after). `~[gpu]` `-r json` + guard (`+5` cases). Release: build + `[server]`.

```bash
git add ArcaneCore ArcaneClient ArcaneServer ArcaneTests premake5.lua
git commit -m "feat(server): ArcaneServer.exe is real -- Core-only dedicated-server host, fixed-step tick, --report census, [witness][server] (plan 1 Task 6)"
```

---

### Task 7: The editor's play-mode picker — listen server, client + embedded server (in-process), client + separate server process

**Files:**
- Modify: `ArcaneEditor/src/App/PlayMode.hpp:38-60`, `ArcaneEditor/src/App/PlayMode.cpp` (topologies, the embedded server world)
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp:163-170`, `ArcaneEditor/src/Panels/EditorPanels.cpp:505,611-662` (three new rows; `PluginHost*` instead of `const PluginVTable*`)
- Modify: `ArcaneEditor/src/App/EditorApp.hpp:614,781,857`, `EditorApp.cpp:159-166` (ini range), `:1053` (`StageFinalize`: `--play-as`), `:2751-2810` (report `worlds`), `EditorAppFrame.cpp:1223-1235` (`AdvanceSim` ticks the server world), `:2028,2969`, `EditorAppScene.cpp:131` (the `Play`/`Stop` call sites take `PluginHost*`)
- Create: `ArcaneEditor/src/Project/ServerLaunch.hpp`, `ArcaneEditor/src/Project/ServerLaunch.cpp` (P13)
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.hpp:218` (+ `playAs`), `HostConfig.cpp:30,113` (`--play-as`), `ArcaneRuntime/src/main.cpp:61-66` (refuse it), `ArcaneClient/src/Arcane/Host/VerifyReport.{hpp,cpp}` (`SetWorlds`, schema 6)
- Modify: `premake5.lua` `ArcaneTests` `files` (+ `ArcaneEditor/src/Project/ServerLaunch.cpp`, the `RuntimeLaunch.cpp` precedent at `:926-932`)
- Modify: `ArcaneTests/src/EditorPlayModeTest.cpp` (+3), `ArcaneTests/src/VerifyReportTest.cpp` (+1), `ArcaneTests/src/HostConfigTest.cpp` (+1); Create: `ArcaneTests/src/ServerLaunchTest.cpp`, `ArcaneTests/src/EditorWitnessTest.cpp`

**Interfaces:**
- Consumes: `Runtime::{SetNetMode, Mode, HasAuthority, Process, SnapshotRegistry, RestoreRegistry, ResetPhysics, EnsurePhysics}`, `PluginHost::{AttachRuntime, DetachRuntime, Vtable}` (Task 5), `RuntimeLaunch::QuoteArg`.
- Produces:
```cpp
namespace Arcane::Editor {
    enum class PlayLaunchMode { Viewport, SeparateWindow, ListenServer, EmbeddedServer, SeparateServerProcess };   // persisted as int (ini)
    enum class PlayTopology   { Standalone, ListenServer, EmbeddedServer, ClientOnly };   // ClientOnly = the viewport world under a separate server process
    class PlaySession {
        bool Play(Arcane::Runtime& runtime, Arcane::PluginHost* host = nullptr, PlayTopology topology = PlayTopology::Standalone);
        bool Stop(Arcane::Runtime& runtime, Arcane::PluginHost* host = nullptr);
        [[nodiscard]] Arcane::Runtime* ServerWorld() noexcept;   // the embedded DedicatedServer world while playing EmbeddedServer, else null
        void TickServer(double realDt);                          // EnsurePhysics + Loop().Advance on ServerWorld(); no-op when null
        [[nodiscard]] PlayTopology Topology() const noexcept;
    };
    namespace ServerLaunch {
        std::vector<std::filesystem::path> ExeCandidates(const std::filesystem::path& editorExeDir);   // ArcaneServer.exe beside, then ../ArcaneServer/
        std::vector<std::wstring> BuildArgs(const std::filesystem::path& projectRoot);                   // {"--project", root, "--frames", "0"}
        class ServerProcess { bool Spawn(const std::filesystem::path& exe, const std::vector<std::wstring>& args); bool IsRunning() const noexcept; void Stop() noexcept; ~ServerProcess(); };
    }
}
// HostConfig: std::string playAs; -- "" | "standalone" | "listen-server" | "embedded-server" | "client" (EDITOR ONLY; ArcaneRuntime refuses like --dump-layout)
// VerifyReport (schema 6): struct WorldFact { std::string role; bool hasAuthority; std::uint64_t entities; std::uint64_t fixedUpdateSystems; };
//   void SetWorlds(std::vector<WorldFact> worlds);   // emitted as "worlds":[...] ONLY when called
```

- [ ] **Step 1: RED — the session tests.** Append to `EditorPlayModeTest.cpp`:

```cpp
#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include "../plugins/HotReloadShared.hpp"

namespace
{
    std::size_t CountEntities(Arcane::Runtime& rt)
    { std::size_t n = 0; for (Astra::Entity e : rt.Registry().GetEntityManager()) { (void)e; ++n; } return n; }
}

TEST_CASE("Play as embedded server stands up a second DedicatedServer world on the same ProcessContext with the same scene; Stop tears it down", "[editor][netmode]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::RegisterSceneComponents(runtime.Registry());
    const Astra::Entity root = runtime.Registry().CreateEntityWith(Arcane::Transform{});
    runtime.Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    runtime.Registry().CreateEntityWith(Arcane::Transform{});
    const std::size_t authored = CountEntities(runtime);

    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(runtime, nullptr, Arcane::Editor::PlayTopology::EmbeddedServer));
    Arcane::Runtime* server = play.ServerWorld();
    REQUIRE(server != nullptr);
    CHECK(server->Mode() == Arcane::NetMode::DedicatedServer);
    CHECK(server->HasAuthority());
    CHECK(&server->Process() == &runtime.Process());          // shares the ProcessContext and nothing else
    CHECK(server != &runtime);
    CHECK(CountEntities(*server) == authored);                // the same scene, restored registry-only
    CHECK(runtime.Mode() == Arcane::NetMode::Client);
    CHECK_FALSE(runtime.HasAuthority());
    CHECK_FALSE(server->Loop().IsPaused());
    for (int i = 0; i < 5; ++i) play.TickServer(1.0 / 60.0);  // ticks independently of the editor's world
    CHECK(server->Loop().IsPaused() == false);

    REQUIRE(play.Stop(runtime));
    CHECK(play.ServerWorld() == nullptr);
    CHECK(runtime.Mode() == Arcane::NetMode::Standalone);
    CHECK(runtime.Loop().IsPaused());
    CHECK(CountEntities(runtime) == authored);
}

TEST_CASE("Play as listen server flips the ONE world to ListenServer; Stop restores Standalone", "[editor][netmode]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(runtime, nullptr, Arcane::Editor::PlayTopology::ListenServer));
    CHECK(runtime.Mode() == Arcane::NetMode::ListenServer);
    CHECK(runtime.HasAuthority());
    CHECK(play.ServerWorld() == nullptr);                     // one world, dual role (spec R3)
    REQUIRE(play.Stop(runtime));
    CHECK(runtime.Mode() == Arcane::NetMode::Standalone);
}

TEST_CASE("Play as embedded server with a loaded module: the server world gets the Server-masked system, the editor world the Client-masked", "[editor][netmode][hotreload]")
{
    using namespace Arcane::HotReloadTest;
    Arcane::Runtime runtime(Arcane::Test::Process());
    runtime.Components()->RegisterComponent<Pulse>(); runtime.Components()->RegisterComponent<RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(runtime);
    REQUIRE(host.Load());
    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(runtime, &host, Arcane::Editor::PlayTopology::EmbeddedServer));
    REQUIRE(play.ServerWorld() != nullptr);
    CHECK(host.Runtimes().size() == 2);
    CHECK(play.ServerWorld()->Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());
    CHECK_FALSE(play.ServerWorld()->Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    CHECK(runtime.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    CHECK_FALSE(runtime.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());
    REQUIRE(play.Stop(runtime, &host));
    CHECK(host.Runtimes().size() == 1);
    host.Unload();
}
```
`ArcaneTests/src/ServerLaunchTest.cpp` (`[editor]`): `ExeCandidates("C:/x/ArcaneEditor")` == `{"C:/x/ArcaneEditor/ArcaneServer.exe", "C:/x/ArcaneEditor/../ArcaneServer/ArcaneServer.exe"}` (compare `generic_string()`s); `BuildArgs("C:/p")` == `{L"--project", L"C:/p", L"--frames", L"0"}`; `ServerProcess` default `IsRunning()` false and `Stop()` a no-op. `VerifyReportTest.cpp` (+1): `SetWorlds({{"Client", false, 3, 2}, {"DedicatedServer", true, 3, 3}})` → `ToJson()` parses with `worlds.size()==2`, the fields verbatim, and a report WITHOUT the call has no `worlds` key. `HostConfigTest.cpp` (+1): `--play-as embedded-server` parses to `playAs == "embedded-server"`, `--play-as bogus` is refused (exit 2), absent = empty. Build → RED (`PlayTopology`, `ServerLaunch.hpp`, `SetWorlds`, `playAs` unknown).

- [ ] **Step 2: `PlaySession`.** `PlayMode.hpp`: the enums per **Interfaces**; members `PlayTopology m_topology = Standalone; std::optional<Arcane::Runtime> m_server; std::vector<std::byte> m_serverSeed;`. `PlayMode.cpp`:

```cpp
    bool PlaySession::Play(Arcane::Runtime& runtime, Arcane::PluginHost* host, PlayTopology topology)
    {
        if (m_mode == EditorMode::Play) return true;
        // (snapshot exactly as today: host ? host->Vtable() : nullptr in place of the old `plugin`)
        …
        runtime.ResetPhysics();
        m_topology = topology;
        switch (topology)
        {
            case PlayTopology::Standalone: break;
            case PlayTopology::ListenServer: runtime.SetNetMode(Arcane::NetMode::ListenServer); break;
            case PlayTopology::ClientOnly:   runtime.SetNetMode(Arcane::NetMode::Client); break;
            case PlayTopology::EmbeddedServer:
            {
                // The second world: same ProcessContext, same module (attached below), the
                // SAME scene -- a registry-only snapshot of the authored world, restored into
                // a fresh DedicatedServer Runtime. Nothing else is shared (spec s7).
                auto seed = runtime.SnapshotRegistry();
                if (!seed.IsOk()) return false;
                m_serverSeed = std::move(*seed.GetValue());
                m_server.emplace(runtime.Process(), Arcane::NetMode::DedicatedServer);
                if (!m_server->RestoreRegistry(m_serverSeed)) { m_server.reset(); return false; }
                if (const Arcane::SceneRoot* sr = runtime.Registry().GetResource<Arcane::SceneRoot>())
                    m_server->Registry().SetResource<Arcane::SceneRoot>(*sr);   // resources are not in the snapshot (GameModule.hpp's LoadState says the same)
                m_server->ResetPhysics();
                if (host) host->AttachRuntime(*m_server);                       // instantiates the Server-masked factories into it
                runtime.SetNetMode(Arcane::NetMode::Client);
                break;
            }
        }
        runtime.Loop().SetPaused(false);
        m_mode = EditorMode::Play;
        return true;
    }
    bool PlaySession::Stop(Arcane::Runtime& runtime, Arcane::PluginHost* host)
    {
        if (m_mode == EditorMode::Edit) return true;
        if (m_server) { if (host) host->DetachRuntime(*m_server); m_server.reset(); m_serverSeed.clear(); }
        if (runtime.Mode() != Arcane::NetMode::Standalone) runtime.SetNetMode(Arcane::NetMode::Standalone);
        // (restore exactly as today, then SetPaused(true), m_mode = Edit)
    }
    void PlaySession::TickServer(double realDt) { if (!m_server) return; m_server->EnsurePhysics(); m_server->Loop().Advance(realDt); }
```
`SetNetMode` clears + re-instantiates module systems from the factory table (Task 5); a module that still adds systems directly in `OnInit` (the pre-ABI-30 idiom) does not get them back on a mode flip until the next reload — say so in `GameModule.hpp`'s `RegisterSystem` comment.

- [ ] **Step 3: The picker + the hosts.** `EditorPanels.cpp:645-662`: three more `MenuItem` rows — "Listen server (in viewport)" → `ListenServer`, "Client + embedded server (in viewport)" → `EmbeddedServer`, "Client + separate server process" → `SeparateServerProcess` — each `MarkIniSettingsDirty()`. `:613-622`: the click maps `Viewport`→`Play(rt, host, Standalone)`, `ListenServer`→`Play(…, ListenServer)`, `EmbeddedServer`→`Play(…, EmbeddedServer)`, `SeparateServerProcess`→ sets a new out-param `launchServerRequested = true` and `Play(…, ClientOnly)`; `SeparateWindow` unchanged. `DrawSimTimeToolbar`'s `const PluginVTable* plugin` parameter becomes `Arcane::PluginHost* host` (`EditorPanels.hpp:170`; the three call sites `EditorAppFrame.cpp:2028,2969`, `EditorAppScene.cpp:131` pass `m_plugin ? &*m_plugin : nullptr`). `EditorApp.cpp:159-166`: the ini range check's upper bound becomes `SeparateServerProcess`. `EditorApp.hpp`: `Arcane::Editor::ServerLaunch::ServerProcess m_serverProcess;` beside `m_play`; a `void DoLaunchServer();` mirroring `DoLaunchStandalone` (`EditorAppScene.cpp:317-377`: `ExeCandidates(CurrentExeDir())` → first regular file → `m_serverProcess.Spawn(resolved, BuildArgs(proj->Root()))`, modal on failure). The toolbar's `launchServerRequested` routes to it from `EditorAppFrame.cpp:2028`'s block; every `m_play.Stop(...)` site is followed by `m_serverProcess.Stop();` (no-op unless running). `AdvanceSim` (`EditorAppFrame.cpp:1230-1233`): after the main `Advance`, `m_play.TickServer(simDt);`.

- [ ] **Step 4: `ServerLaunch`.** `ServerLaunch.cpp`: `ExeCandidates`/`BuildArgs` pure; `ServerProcess::Spawn` = `RuntimeLaunch.cpp:92-180`'s `CreateProcessW` body (reuse `RuntimeLaunch::QuoteArg`; `CREATE_NO_WINDOW`; stdout/stderr → `ArcaneServer.log` beside the exe; working dir = exe's parent) but KEEPING `hProcess` (closing only `hThread`); `IsRunning` = `WaitForSingleObject(h, 0) == WAIT_TIMEOUT`; `Stop` = `TerminateProcess(h, 0)` + `CloseHandle` + `ARC_INFO("ServerLaunch: stopped ArcaneServer.exe")`; the destructor calls `Stop`. Add the file to `ArcaneTests`'s `files` with the `RuntimeLaunch.cpp` comment shape (spawn is desk-verify; the pure halves are `[editor]`).

- [ ] **Step 5: `--play-as` + the report's `worlds`.** `HostConfig.cpp:30`: `cli.Option("play-as", "", "editor only: start playing at boot as standalone | listen-server | embedded-server | client")` (NO `Choices` — the default is empty and an empty choice is not a value); `:113`: `cfg.playAs = r.Get("play-as");` followed, in the post-parse refusal block beside `fixed-dt`'s, by `if (!cfg.playAs.empty() && cfg.playAs != "standalone" && cfg.playAs != "listen-server" && cfg.playAs != "embedded-server" && cfg.playAs != "client") { std::fprintf(stderr, "error: --play-as must be one of standalone | listen-server | embedded-server | client\n"); cli.PrintUsage(); return { std::nullopt, 2 }; }`. `ArcaneRuntime/src/main.cpp:61-66`: a second refusal in the same table — `if (!parsed.config->playAs.empty()) { fprintf(stderr, "error: --play-as is an EDITOR-only flag …"); return 2; }` (ahead of `Diagnostics::Install`, same reasoning). `EditorApp::StageFinalize` (`EditorApp.cpp:1053`): at its end, `if (!m_config.playAs.empty()) { const auto topo = …map…; if (!m_play.Play(m_runtime->Core(), m_plugin ? &*m_plugin : nullptr, topo)) ARC_ERROR("--play-as {}: Play refused", m_config.playAs); }` ("standalone" → `Standalone`, "listen-server" → `ListenServer`, "embedded-server" → `EmbeddedServer`, "client" → `ClientOnly`). `VerifyReport`: `kSchemaVersion = 6` (+ the header comment line "6 added worlds"), `SetWorlds`, emitted only when set. `EditorApp.cpp:2806` (after `AddCensus`): 

```cpp
            std::vector<Arcane::WorldFact> worlds;
            auto fact = [](Arcane::Runtime& rt) { Arcane::WorldFact w; w.role = Arcane::ToString(rt.Mode()); w.hasAuthority = rt.HasAuthority(); w.entities = CountEntities(rt.Registry()); w.fixedUpdateSystems = rt.Schedulers().fixedUpdate.Size(); return w; };
            worlds.push_back(fact(m_runtime->Core()));
            if (Arcane::Runtime* s = m_play.ServerWorld()) worlds.push_back(fact(*s));
            report.SetWorlds(std::move(worlds));
```

- [ ] **Step 6: The editor witness.** `ArcaneTests/src/EditorWitnessTest.cpp`, `[witness][gpu]` (it is a real editor host with a device — it belongs with the other `[witness][gpu]` scenarios and is INVISIBLE to `~[gpu]` by design):

```cpp
TEST_CASE("E1: the editor stands up client + embedded server through --play-as and reports both worlds", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir() /* ../ArcaneEditor, same shape as StagedRuntimeDir */, "e1-embedded-server");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneEditor.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "vulkan", "--frames", "60",
                 "--report", inv.reportPath.generic_string(), "--play-as", "embedded-server" };
    inv.hardCapMs = 120000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    const auto& worlds = run.report.at("worlds");
    REQUIRE(worlds.size() == 2);
    CHECK(worlds[0].at("role") == "Client");           CHECK(worlds[0].at("hasAuthority") == false);
    CHECK(worlds[1].at("role") == "DedicatedServer");  CHECK(worlds[1].at("hasAuthority") == true);
    CHECK(worlds[0].at("entities") == worlds[1].at("entities"));   // the same scene in both
}
```

- [ ] **Step 7: Build, suites, gate, desk note, commit.** `premake5.exe vs2026`; Debug build; `./ArcaneTests.exe "[editor],[netmode],[host],[verdict]"` → passed; `~[gpu]` `-r json` + guard (`+3` play-mode, `+3` ServerLaunch, `+1` VerifyReport, `+1` HostConfig = `+8` cases; record assertions); `ReferenceProject.slnx -t:Rebuild` + `Arcane.slnx` + the unfiltered suite (E1 + S1/S2 + the runtime witnesses); Release the same; `golden-gate.ps1` both configs → 4/4 `diffCount=0` (the picker's three new rows live inside a popup that the golden never opens; the transport button itself is unchanged). **Desk pass OWED, not performed here:** click each of the three new rows in the running editor once (Play/Stop), and "Client + separate server process" must show an `ArcaneServer.log` beside `ArcaneServer.exe` that ends on Stop.

```bash
git add ArcaneClient ArcaneEditor ArcaneRuntime ArcaneTests premake5.lua
git commit -m "feat(editor): play-mode picker grows listen server, client + embedded server (in-process) and client + separate server process; --play-as; report worlds (plan 1 Task 7)"
```

---

### Task 8: Closeout — baselines booked, spec status, docs, the memory note

**Files:**
- Modify: `scripts/automation-baselines.json` (the six rows + the `note`/`measured` paragraphs), `docs/specs/2026-09-15-core-dll-split-design.md:4-6` (status line), this plan (Closeout section below), `CLAUDE.md` + `premake5.lua:110-115` + `build/arcane.lua:1-25` (every sentence that still calls ArcaneCore a static lib)

- [ ] **Step 1: Two-config measurement, the file's own way.** `ReferenceProject.slnx -t:Rebuild` then `Arcane.slnx`, Debug then Release, `0 Error(s)` in all four; `ArcaneTests.exe "~[gpu]" -r json::out=…` FROM the exe dir in both configs (record both seeds); `check-baselines.ps1` reports the plan's whole rise (expected cases: Task 2 +2, Task 3 +3, Task 4 +3 minus the `RuntimeEngineSystemsTest` re-pin, Task 5 +7, Task 6 +5, Task 7 +8 — DERIVE the exact case and assertion sums from the per-case JSON, attribute per task as the 2026-09-12 entries do, and book the final numbers in the six `baselines` rows; the unfiltered Debug run once more for the `[gpu]` case count, which rose by exactly 1 (E1) — say so from `raw - ~[gpu]`). `golden-gate.ps1` both configs, 4/4 `diffCount=0`, quoted in the Closeout.
- [ ] **Step 2: Prose sweep.** `grep -rn -i "static lib\|StaticLib\|ONE module per process\|one ArcaneCore per process" CLAUDE.md premake5.lua build/arcane.lua ci docs/*.md | grep -i "arcanecore\|core"` — rewrite each surviving sentence to the DLL shape (the premake comments were done in Task 2; this catches `CLAUDE.md` and any README). Spec status line: `**Status:** Implemented -- plan 1 (Arcane) closed <date> at <sha>; plan 2 (Gacha /MD) pending.`
- [ ] **Step 3: Closeout section** appended to this plan: per-task shas, the measured counts + seeds per config, the gate figures, the ledger's newly-exported-symbol list from Task 2 Step 7, the P10 note (ArcaneClient.dll staged beside the server; `clientDllLoadedAfterModule: true` in S1's report, pinned as a FACT not a defect), the Task 7 desk pass as OWED, and the Plan 2 hand-off: Gacha `Server/premake5.lua` now carries the explicit file list (Task 2) and `Game/Aphelyon.arcproj` is at 30 (Task 5); Plan 2 deletes the from-source `ArcaneCore` project and links `ArcaneCore.dll` on the `-md` triplet.
- [ ] **Step 4: Commit** (`docs(core-dll): close plan 1 -- baselines booked, spec implemented, closeout`). Do not push. Then update the memory file `project_arcane_core_shared_dll_direction.md`: Plan 1 CLOSED at the sha, Plan 2 (Gacha) NEXT, desk pass OWED.

---

## Self-review (run at plan-writing time)

- **Spec coverage:** §1.1 two DLLs → T2; §1.2 linkage (arcbuild Core-only, modules both) → T2 Step 6; §1.3 prep 1–4 → T1 (1–3), T2 (4); §2 split + `EngineContext` client pointer → T4, T5; §3 ProcessContext, TypeContext ownership, Diagnostics/Log in Core, JobSystem per-Runtime (unchanged) → T2/T3; §4 NetMode, RoleMask, HasAuthority, factories, ListenServer one-world → T5 (+T7 flips); §5 snapshot-all/reload/restore-all + net-driver refusal → T5; §6 ArcaneServer + CLI + `--report` → T6; §7 picker in-process + out-of-process → T7; §8 macro, ABI 30 once, restamps, three plugin rebuilds → T2/T5 (Gacha `/MD` is Plan 2); §9 every device-less bullet → T3/T5 tests, the two host-level witnesses → T6 (S1) and T7 (E1), golden both lanes → every task; §10 non-goals untouched.
- **Placeholder scan:** no TBD/TODO; every code step carries the code or the exact edit; the only implementer-derived lists are compiler-driven (T2 Step 7's export loop, T4 Step 6's 24 `Runtime&` sites) and each names the error text that drives it.
- **Type consistency:** `Runtime(ProcessContext&, bool)` (T3) → `Runtime(ProcessContext&, NetMode = Standalone)` (T5) — the T4 `ClientRuntime(ProcessContext&, bool enableAudioDevice)` keeps the audio flag on the Client side and constructs `m_core(process)`; `Test::Process()` (T3) is used unchanged through T7; `PluginHost(ProcessContext&, path)` + `AttachRuntime` (T5) is what T6/T7 call; `IClientHooks`/`AttachClient` (T4) is what T5's `RefreshContext` reads; `ToString(NetMode)` (T5) is what T6's census and T7's `WorldFact` use; `Runtime::Process()` (T5) is what T7's `PlaySession` uses to build the embedded world.

<!-- CLOSEOUT -->

## Closeout (Task 8, 2026-09-15)

**Plan 1 (Arcane) is closed.** All seven implementation tasks landed on `main`
(no worktree), unpushed. Spec `docs/specs/2026-09-15-core-dll-split-design.md`
status line updated to `Implemented -- plan 1 (Arcane) closed 2026-09-15 at
c5abeb48; plan 2 (Gacha /MD) pending`.

### Per-task commits (both repos)

| Task | Arcane commit(s) | Gacha commit(s) |
|---|---|---|
| Plan | `06ba82e2` | — |
| T1 — prep (MeshBuilder/RenderSystems moves, `RuntimePresentation` lift) | `49cda5a1` | — |
| T2 — `ArcaneCore.dll`, `ARCANE_CORE_API`, the physical move | `882106f5` + fix `ddae6049` | `84b63f44` (explicit file list) + `a3351eee` (`ARCANE_CORE_STATIC`) |
| T3 — `ProcessContext` | `a783e81c` + fix `dc23a5e3` | — |
| T4 — `ClientRuntime` split, `IClientHooks` | `231fb719` | — |
| T5 — `NetMode`/`RoleMask`, N-Runtime `PluginHost`, ABI 30 | `7452555e` + fix `dc826b5a` | `8ab1be42` (ABI 29→30 restamp) |
| T6 — `ArcaneServer.exe`, `ProjectHost.hpp`, `--report` census | `34cccd6b` + fix `6561909c` | — |
| T7 — editor play-mode picker, `--play-as`, report `worlds` | `5c7726bc` + fix `c5abeb48` | — |
| T8 — this closeout (baselines, spec status, docs, Closeout) | *(this commit)* | — |

Base HEAD at Task 8 start: `c5abeb48` (last code commit). No Gacha change this task.

### Step 1 — the two-config measurement

Build ritual, both configs (`ReferenceProject.slnx -t:Rebuild` before
`Arcane.slnx` each time, per the single-slot trap): Debug pair, Release pair,
then flip back to Debug. All four builds: **0 Error(s), 0 Warning(s).**

| Run | Config | Seed | Assertions | Cases (passed/skipped) |
|---|---|---|---|---|
| `~[gpu]` `-r json` | Debug | `1046182859` | 57524 | 1786 / 4 |
| `~[gpu]` `-r json` | Release | `2201046918` | 57524 | 1786 / 4 |
| unfiltered `-r json` | Debug | `1519477275` | 120005 | 1833 / 4 |

Debug and Release `~[gpu]` are byte-identical (57524/1786), matching Task 7's
own last fix-round measurement at the same code head exactly (the ruling at
progress.md line 94 deferred Task 7's Release `~[gpu]` to this task's
mandatory two-config measurement; this run discharges it — no separate Release
run was needed since Task 8 changes only docs + the baselines JSON, not code).

`check-baselines.ps1 -Invocation "~[gpu]"` **before** this edit, both configs,
against the still-committed 57269/1749:
```
telemetry: arcanetests.assertions [Debug/~[gpu]]   = 57524, baseline 57269 (+255)
telemetry: arcanetests.cases      [Debug/~[gpu]]   = 1786,  baseline 1749  (+37)
telemetry: arcanetests.assertions [Release/~[gpu]] = 57524, baseline 57269 (+255)
telemetry: arcanetests.cases      [Release/~[gpu]] = 1786,  baseline 1749  (+37)
```
Booked below; guard re-run **after** the edit reports `+0/+0` exit 0 in both
configs (see "Baselines booked").

**Golden gate — 4/4 lanes, `diffCount=0`, both configs, no re-bless:**

| Lane | Debug | Release |
|---|---|---|
| ArcaneRuntime/dx12/runtime-scene | PassedOnFallback, `diffCount=0`, `resolvedLevel=shared (expected backend)` | same |
| ArcaneRuntime/vulkan/runtime-scene | Passed, `diffCount=0`, `resolvedLevel=backend` | same |
| ArcaneEditor/dx12/editor-ui | Passed, `diffCount=0`, `resolvedLevel=shared` | same |
| ArcaneEditor/vulkan/editor-ui | Passed, `diffCount=0`, `resolvedLevel=shared` | same |

Order run: Release gate first, then the slot flipped back to Debug
(`ReferenceProject.slnx -t:Rebuild -p:Configuration=Debug` + a Debug
`Arcane.slnx` restage), then the Debug gate. **The desk is left on Debug.**

### The plan's whole rise — per-task attribution, reconciled

Grand total: `57524 - 57269 = +255 assertions`, `1786 - 1749 = +37 cases`.
Reconciled two ways: (a) the ledger's own per-task deltas, each derived at
that task's own close from its own per-case `-r json` (progress.md line 97),
sum to exactly `+255/+37` with **zero residual**; (b) this close's own
per-case JSON independently confirms every wholly-new suite by direct
tag/file attribution (below) — the two methods agree everywhere they overlap.

| Task | Assertions | Cases | Source of the figure |
|---|---|---|---|
| T2 — `CoreDllTest.cpp` | +14 | +2 | **Confirmed directly** from this close's JSON: `CoreDllTest.cpp` = 2 cases / 14 assertions, both `[core-dll]`. Matches the fix-round-final delta in task-2-report.md exactly (the review round 1 fix rewrote case 2 from a tautology to a real cross-DLL logger pin, 9→14 assertions). |
| T3 — `ProcessContextTest.cpp` | +8 | +3 | **Confirmed directly**: `ProcessContextTest.cpp` = 3 cases / 8 assertions, all `[process]`. |
| T4 — `ClientRuntimeTest.cpp` net of two re-pins | +15 | +3 | `ClientRuntimeTest.cpp` = 3 cases / **18** assertions, confirmed directly (wholly new file, all `[client][runtime]`). Net task delta is `+15/+3` per the ledger (task-4-report.md): two pre-existing cases in `RuntimeEngineSystemsTest.cpp`/`RuntimeTest.cpp` were re-pinned to the engine-owned-systems split (P7), netting `-3` assertions against those cases with no case-count change — not independently re-derivable from a single post-hoc snapshot (it requires the case's PRIOR assertion count), so this half is cited from the task's own contemporaneous before/after JSON. `18 - 3 = 15`. |
| T5 — `RoleMaskTest.cpp` + `MultiRuntimeReloadTest.cpp` + `ClassTemplatesTest` pin | +84 | +11 | **Confirmed directly** for the bulk: `RoleMaskTest.cpp` = 7 cases / 45 assertions (all `[netmode][runtime]`), `MultiRuntimeReloadTest.cpp` = 4 cases / 38 assertions (all `[hotreload][netmode]`) — sums to 11 cases / 83 assertions. Plus `+1` assertion on a pre-existing `ClassTemplatesTest` case (a re-pin, no case-count change, cited from task-5-report.md). `83 + 1 = 84`. |
| T6 — `ServerConfigTest.cpp` + `ServerFixedRateTest.cpp` + `ServerWitnessTest.cpp` (S1/S2/S3) | +51 | +8 | **Confirmed directly**, all three wholly new: `ServerConfigTest.cpp` 3/17, `ServerFixedRateTest.cpp` 2/5, `ServerWitnessTest.cpp` 3/29 (S1/S2/S3 — `[server][witness]`, NOT `[gpu]`-tagged per ruling P14, so all three sit inside `~[gpu]`). `3+2+3=8` cases, `17+5+29=51` assertions. |
| T7 — `EditorPlayModeTest` +5, `ServerLaunchTest` +3, `VerifyReportTest` +1, `HostConfigTest` +1 | +83 | +10 | 10 cases directly identified: 5 `EditorPlayModeTest.cpp` cases carry the plan's `[netmode]` tag (the 3 from the initial round plus the fix round's I1 attach-refusal case and the C1 exit-order pin), summing to 50 assertions; `ServerLaunchTest.cpp` (wholly new) = 3 cases / 11 assertions; `VerifyReportTest.cpp`'s `worlds` case (`"schema 6: worlds carries one entry per live world, in host order"`) = 1 case / 12 assertions; `HostConfigTest.cpp`'s `--play-as` case = 1 case / 7 assertions. `50+11+12+7=80` of the task's `+83` reconciled directly; the remaining `+3` assertions are re-pins on pre-existing `VerifyReportTest.cpp`/`HostConfigTest.cpp` cases from the schema-5→6 bump (five sites per task-7-report.md item 3) and are not separable from a single post-hoc snapshot — cited from the task's own contemporaneous guard reading (`+83/+10` exactly, task-7-report.md fix-round section). |
| **Sum** | **+255** | **+37** | Reproduces the measured grand-total rise to the assertion, with **zero residual**. |

Case names were read from the JSON's `test-info.name` field (never a source
regex), matching the file's own 2026-09-12 convention.

### The `[gpu]` case identity at this close

Measured this close: unfiltered Debug totals **1837** cases attempted
(1833 passed + 4 skipped) against `~[gpu]`'s **1790** (1786 passed + 4
skipped) — **the same 4 SKIP cases appear identically in both runs** (they
are environment-conditional, e.g. `[ide-desk]`/`[build-desk]`, not
`[gpu]`-tagged), so they cancel out of the subtraction either way it is taken:
`1837 - 1790 = 47` using totals-including-skips, or `1833 - 1786 = 47` using
passed-only counts. Directly cross-checked by counting the `gpu` tag on every
case in the unfiltered JSON: **47** cases carry it, and zero `[gpu]`-tagged
cases leak into the `~[gpu]` run (both independently confirmed from this
close's own JSON, not derived by subtraction alone).

**This does not match the brief's "expect 35" premise, and the discrepancy is
resolved, not smoothed over.** The brief's expectation was `34 (prior) + 1
(E1) = 35`; the measured `[gpu]` count is 47. Task 2's own Step 8 measurement
— commit `882106f5`, base head after only T1+T2, **before any of T3–T7 ran**
— already recorded an unfiltered Debug run of 1797 passed cases against that
same close's `~[gpu]` of 1751 passed cases (task-2-report.md §10): `1797 -
1751 = 46`. Neither T1 nor T2 added any `[gpu]`-tagged case (T1 is a pure
refactor; T2's `CoreDllTest` cases are tagged `[core-dll]`), so **the `[gpu]`
count entering this plan was already 46, not the documented "34"** — some
unrelated, concurrent work (outside this plan) grew the raw `[gpu]`-tagged
case count by 12 between the automation-baselines.json note's last `[gpu]`-
identity entry (2026-09-12, "34") and this plan's own 2026-09-15 baseline
backfill (57269/1749), and that growth was never re-stated in the note's
`[gpu]`-invisibility argument. This plan's **own** contribution is exactly
what the brief predicted: **E1 adds exactly +1** (`46 → 47`), confirmed by
the same arithmetic that isolates T2's baseline (46) from this close's
measurement (47). The stale "34" is a pre-existing gap in the baselines
file's own documentation, not a defect introduced by this plan — flagged here
rather than corrected in the JSON note, since Step 2's prose-sweep scope is
the ArcaneCore-static-lib language, not the `[gpu]`-identity narrative, and
the note's `baselines` rows themselves (the only load-bearing numbers) were
never wrong.

### Task 2 Step 7 — the newly-exported-symbol list

The Step 7 link-error loop that chases `ArcaneCore.dll`'s export boundary to
zero found **zero** unresolved symbols on the first post-move build — every
symbol in the moved layer already carried `ARCANE_API` (it already lived
inside a DLL, `ArcaneClient.dll`), so the `sed` macro rename was the whole
audit for the 79 moved files. The only symbols genuinely NEW to
`ARCANE_CORE_API` are Core's own pre-existing (never-exported) headers, marked
by hand in Task 2 Step 4:

| # | file:line | symbol |
|---|---|---|
| 1 | `Guid.hpp:23` | `struct ARCANE_CORE_API Guid` |
| 2 | `Cli/Cli.hpp:30` | `class ARCANE_CORE_API Cli` |
| 3 | `Cli/Cli.hpp:52` | `struct ARCANE_CORE_API Cli::Result` (nested — does not inherit the outer export) |
| 4 | `Build/Toolchain.hpp:32` | `ARCANE_CORE_API Toolchain::DiscoverSolution(const std::filesystem::path&)` |
| 5 | `Build/Toolchain.hpp:39` | `ARCANE_CORE_API Toolchain::ResolvePremake(const std::filesystem::path&)` |
| 6 | `Build/Toolchain.hpp:46` | `ARCANE_CORE_API Toolchain::VsWhere(const std::string&)` |
| 7 | `Build/Toolchain.hpp:52` | `ARCANE_CORE_API Toolchain::ResolveMsBuild()` |
| 8 | `Build/Toolchain.hpp:57` | `ARCANE_CORE_API Toolchain::ResolveDevenv()` |
| 9 | `Core/ModuleContext.hpp:45` | `ARCANE_CORE_API void Arcane::Core::SetModuleTypeContext(Astra::TypeContext*)` — created by Task 2's own unbriefed fix (§5 below), later absorbed into `ProcessContext::Create` at Task 3 (`Core/ModuleContext.{hpp,cpp}` deleted). |

Header-only Core files (`Crypto/`, `Net/*`, `Util/*`, `Jobs/TaskExecutor.hpp`,
`Version.hpp`) got no macro, as briefed.

### P10 — `ArcaneServer.exe` stages `ArcaneClient.dll`, pinned as a FACT

Ruling P10: `ArcaneServer.exe` links `ArcaneCore` **only**, but its
`postbuildcommands` stage `ArcaneClient.dll` beside itself too, because a
game module links BOTH import libs (spec §1.2) and the loader needs
`ArcaneClient.dll` resolvable to map the module — the construction gate is
the exe's link line, proven by the S1 witness's `clientDllLoadedAtBoot ==
false`, not by whether `ArcaneClient.dll` is present on disk. Task 6's S1
desk-check census (task-6-report.md, `ArcaneServer.exe --project
ReferenceProject --frames 60 --report out.json`):
```json
"presentation":{"clientAttached":false,"clientDllLoadedAfterModule":true,"clientDllLoadedAtBoot":false}
```
`clientDllLoadedAfterModule: true` is **exactly what P10 predicts, and is a
FACT, not a defect**: `ReferenceGame.dll` (the module) links both import
libs, so loading it pulls `ArcaneClient.dll` in even though
`ArcaneServer.exe`'s own link line never references a single Client symbol.
Shedding the module's Client import (a server-only compile-out) is spec §10
follow-on work, explicitly out of this arc's scope.

### Task 7 desk pass — OWED

Per task-7-report.md, **not performed by any implementer, owed to the user**:
click each of the three new play-mode picker rows once in a live editor
(Play, then Stop) — "Listen server (in viewport)", "Client + embedded server
(in viewport)", "Client + separate server process" — and for the last one,
confirm an `ArcaneServer.log` appears beside `ArcaneServer.exe` and that it
**stops on Stop** (the `EditorAppFrame.cpp` `wasPlaying && !InPlayMode()`
observation is the line under test). This item remains open; Task 8 performed
no interactive desk verification (headless suites and scripted gates only).

### Outside-arc item — a pre-existing order-dependent SIGSEGV class (two repros, not this plan's)

Neither repro is caused by any commit in this plan; both are pre-existing
engine defects this plan's own tests made more likely to surface, because
Task 7's new cases are the first to hand a REAL loaded module through
`PlaySession`. **The mechanism in both cases**: a module-registered
`Astra::TypeContext` entry (a component or resource type first resolved
*inside* a plugin DLL) is not retracted when that DLL unmaps, so a later
resolution of the same type from a different module dereferences a dangling
descriptor.

1. **`SceneAssetTest` — "a v3 scene still loads after the v4 bump"** crashes
   when it runs immediately after `PluginLoadDiagnosticsTest`'s rollback case
   (Astra-side; reproduced at Task 5's base head `231fb719` with that task's
   whole diff stashed — see task-5-report.md concern 1). Pre-dates this plan
   entirely.
2. **`EntityOpsTest.cpp:496` — "CreateEntityInScene parents the top-level
   create under SceneRoot, surviving a save/load round trip"** crashes after
   any `PluginHost` Load + `ForceReload` + Unload sequence, with **no Task 7
   code on the crashing path** — bisected to a two-test deterministic repro
   and a probe matrix (task-7-report.md, fix-round "NEW FINDING" section):
   `ARCANE_GAME_MODULE`'s `SaveState` resolves `SceneRoot` from inside the
   module DLL; if that module is the FIRST registrar of `SceneRoot` in the
   shared `TypeContext`, the registration is never retracted at unload, and a
   later `GetResource<SceneRoot>()` from `ArcaneClient.dll`
   (`Edit::CreateEntityInScene`) dereferences the dangling entry. Resolving
   `SceneRoot` from the exe FIRST makes the suite deterministic (probe PB),
   which is evidence for the mechanism, not a fix — masking it that way was
   explicitly rejected (it would hide a real production bug from the suite).

Both are the same "raw pointers INTO a plugin module dangle on unload" bug
class as the pre-existing `ComponentModule` descriptor-retraction machinery
already partially covers ("Unloading a plugin restores the descriptors it
overrode" pin) — this class is TYPE-registration retraction, which that pin
does not reach. Recommended follow-up, outside this arc: module-registered
`TypeContext` entries need the same retraction-on-unload discipline
`ComponentModule` descriptors already have.

Task 8's own unfiltered Debug run (seed `1519477275`, 120005/1833) did **not**
hit either repro — both are order-dependent and did not trigger under this
run's random seed.

### Plan 2 hand-off (Gacha /MD)

- Gacha `Server/premake5.lua` now carries the explicit file list (Task 2,
  commits `84b63f44` + `a3351eee`) in place of the from-source `ArcaneCore`
  glob — narrowed BEFORE the Arcane-side move landed, so the Server never saw
  Scene/Plugin/Project (Astra/Manifold2D/enkiTS) sources it has no include
  paths for.
- `Game/Aphelyon.arcproj` is at ABI **30** (Task 5, Gacha commit `8ab1be42`).
- **Plan 2 deletes the from-source `ArcaneCore` project and links
  `ArcaneCore.dll` on the `-md` triplet** — the static-CRT source build
  (`ARCANE_CORE_STATIC`, Task 2's C1/I1 fix) is the bridge Plan 1 leaves in
  place; Plan 2 retires it outright rather than extending its file list
  further.

### Baselines booked

`scripts/automation-baselines.json`'s six `baselines` rows: Debug and Release
`arcanetests.assertions`/`arcanetests.cases` under `~[gpu]` updated
`57269→57524` / `1749→1786`. **Dist is untouched** (not built in this plan;
stays at its asset-manager Plan 3 row, `55226/1516`). `check-baselines.ps1`
after the edit reports `+0/+0` exit 0 in both Debug and Release.
