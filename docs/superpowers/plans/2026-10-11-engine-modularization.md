# Engine modularization: implementation plan

> Execution: subagent-driven per phase on branch `feat/modularization` (worktree), ledger `.superpowers/sdd/2026-10-11-modularization/`. Per R10: no tests or CI between phases; each phase ends with a local Windows Debug build and one commit; wholesale testing (s6) at the end.

**Spec:** `docs/superpowers/specs/2026-10-11-engine-modularization-design.md` (copied below in full, so this file is the single brief). The include-edge evidence for every move is `.superpowers/sdd/2026-10-11-modularization/sweep.md` (site lists) and `sweep-r2.md` (the corrected map, clean).

---

**Date:** 2026-10-11 · **Base:** `main` f91866c1 · **Status:** design approved in conversation; awaiting written-spec review
**Branch (to be cut):** `feat/modularization`

## 0. Intent

Arcane ships today as two engine DLLs (`ArcaneCore`, `ArcaneClient`) with every
vendor library baked into one of them. This arc splits the engine into **19
modules** with declared, enforced dependencies, and builds the engine in **two
link layouts** from one codebase:

- **modular** -- one shared library per module (dev, editor, hot reload; today's shape, finer);
- **monolithic** -- every module a static library, linked with the game into one
  executable per project (`<Game>.exe`, Unreal's shipping shape).

**Goals (user, 2026-10-11):** clear, enforced boundaries; ship only what a host
needs; faster iteration (smaller relinks). **Non-goal:** swapping a library
version without recompiling consumers (imgui and friends have no stable ABI).

**Prior art adopted from Unreal** (UE 5.8.2 source, `D:\dev\_reference`):

| UE mechanism | Source | Arcane adoption |
|---|---|---|
| Editor targets Modular, Game/Client/Server/Program Monolithic in every config | `TargetRules.cs:2688` | modular = dev/editor/hot reload; monolithic = packaged game |
| Per-module `<NAME>_API`: export / import / empty when monolithic | `UEBuildModule.cs:241,376,1520` | per-module `ARC_<MODULE>_API` (s2.4) |
| Monolithic links module object files directly, so static registrants survive | `UEBuildBinary.cs:786-799` | monolithic whole-archives each module's static lib (s3.3) |
| Forced reference to `IMPLEMENT_MODULE_<Name>` per module | `UEBuildBinary.cs:810`, `ModuleManager.h:946-950` | `ARC_MODULE(<Name>)` + forced reference (s2.5) |
| Module host types decide target inclusion | `ModuleDescriptor.cs` | `Runtime`/`Client`/`Developer`/`Editor`/`Game`/`Program` (s2.3) |
| `CircularlyReferencedDependentModules` escape hatch | `.Build.cs` | **not adopted**: cycles are a generate-time error |

Not adopted: UE's ~1000-module granularity, a separate build tool (premake +
arcbuild stay this arc), UHT codegen.

## 1. Module map

**Rules.** Cycles are forbidden *between modules*; folder cycles *inside* a
module are fine (Base and Config share `ArcaneCore`). A vendor library is a
**private static dependency** of the module that owns it; it becomes its own
module only if several modules need one shared instance or it is optional per
host -- today `ImGui`, `ImGuiNodeEditor` and `SDL3` (a *prebuilt* vendor module:
a descriptor with no project, R14) qualify (Tracy joins when it is actually
linked; nothing links it today).

### 1.1 Runtime (every host, servers included)

| Module | Contents (from today's tree) | Depends on (*private vendor*) |
|---|---|---|
| `ArcaneCore` (name kept) | `ArcaneCore.cpp`, `Core/`, `Base/` minus `Runtime.*`, `ProcessContext.*` (`Engine.*` STAYS: it is BuildInfo/ExecutablePathUtf8/PluginABIVersion, used by Diagnostics, Assets, Input, Render); `kGamePluginABIVersion` (moved out of `PluginABI.hpp` into a Core header PluginABI includes); `NetMode` + `ToString(NetMode)`; the OS CSPRNG (`SecureRandomBytes`, moved out of Crypto); `Config/`, `Platform/` (OS), `Util/`, `Math/`, `Guid.*`, `Jobs/`, `Reflection.hpp`, `Cli/`, `Build/`, `Version.hpp` | *enkiTS*; Astra, glm (header-only, **public**: Core headers include them) |
| `ArcaneNet` | `Net/` (+ the Net instantiation includes moved out of `ArcaneCore.cpp`) | Core |
| `ArcaneCrypto` | `Crypto/` (+ its instantiation include from `ArcaneCore.cpp`); calls Core's `SecureRandomBytes` | Core |
| `ArcaneAssets` | `Assets/`, `Material/`, `Mesh/`, `Sprite/`, plus `Project/AssetId.hpp` | Core |
| `ArcaneScene` | `Scene/` minus physics, `EngineRoster.hpp`, `EngineResourceTypes.*`; `Serialization/`, `Ecs.hpp`/`EcsFwd.hpp` (Astra facade) | Core, Assets, Sim (public: `Ecs.hpp` exposes `Arcane::Time`) |
| `ArcanePhysics2D` | `Scene/Physics2D*`, `Scene/PhysicsEvents2D*`, `Scene/PhysicsComponents.hpp`, `Scene/PhysicsSystem.hpp`, `Scene/PhysicsQuerySettings*`, `Physics2D.hpp`/`Physics2DFwd.hpp` | Core, Scene, *Manifold2D* |
| `ArcaneSim` | `Sim/` | Core (+ Astra headers via Core) |
| `ArcaneEngine` | `Base/Runtime.*`, `Base/ProcessContext.*`, `Plugin/`, `Project/`, `Scene/EngineRoster.hpp`, `Scene/EngineResourceTypes.*` (the roster composes Scene + Physics2D components; all includers are Engine) | Core, Scene, Physics2D, Sim, Assets, Net |

### 1.2 Client (windowed hosts only)

| Module | Contents | Depends on |
|---|---|---|
| `ArcaneWindow` | Client `Platform/` (Window, RenderWindowSettings) | Core, Assets (private: stb image decode for the window icon), SDL3 (private) |
| `ArcaneInput` | `Input/` | Core, Window (private), SDL3 (private) |
| `ArcaneAudio` | `Audio/` | Core, Assets |
| `ArcaneRender` | `Render/` incl. `Nri/` minus `nodes/ImGuiNriNode.*`; `PhysicsDebugDraw` keeps only its DRAW half | Core, Scene, Assets, Window, Physics2D (picking, debug draw), SDL3 (private), *NRI, freetype, msdfgen, DXC, Vulkan-Headers* |
| `SDL3` (vendor, prebuilt) | `ThirdParty/SDL3/SDL3.arcmodule`: no project; one shared SDL3 per process -- `SDL3.dll` on Windows (vcpkg per-port linkage in the overlay triplet), the system shared library on Linux/macOS; staged beside every program whose closure reaches it | -- |
| `ImGui` (vendor) | imgui core only; the one `GImGui` (the SDL3 platform backend compiles into ArcaneImGui) | -- |
| `ArcaneImGui` | `ImGui/` + `ImGuiNriNode.*` + imgui's SDL3 backend; implements ArcaneClient's overlay hook | Core, Client, Render, ImGui, Sim; Input, Window, SDL3 (private) |
| `ArcaneClient` | `Client/`, `Host/` minus the verify files; owns the overlay hook (no imgui include) | Core, Engine, Render, Input, Audio, Window, Assets, Scene, Sim |

**Known leak.** Manifold2D (a static vendor lib) is linked into every module that
depends on `ArcanePhysics2D`, because Physics2D's public headers call it inline.
It holds no mutable globals, so the copies are harmless; making it private to
Physics2D is a later physics cleanup.

### 1.3 Developer, Editor, Programs

| Module | Type | Contents |
|---|---|---|
| `ArcaneVerify` | Developer | `Host/Verdict.*`, `VerifyReport.*`, `ReferenceImages.*`, `OffscreenVehicle.*` (depends on Core, Assets, Client, Render) |
| `ArcaneAssetPipeline` | Developer | a module DLL (exports `ARC_ASSET_PIPELINE_API`, plus cgltf's `cgltf_parse`/`cgltf_validate`/`cgltf_free` for the tests); depends on Core (*bc7enc_rdo, meshoptimizer*; cgltf public) |
| `ArcaneEdit` | Editor | `Edit/` (only ArcaneEditor uses it; depends on Core, Scene) |
| `ImGuiNodeEditor` (vendor) | Editor | imgui-node-editor |

Programs (executables, never modules): `ArcaneEditor`, `ArcaneRuntime`,
`ArcaneServer`, `ArcaneCrashReporter`, `arcbuild`, `arccook`. The Hub stays
Tauri, outside this build.

### 1.4 What each host gets

- **Packaged game:** Runtime + Client modules + the game; `ImGui`/`ArcaneImGui` only if the game's descriptor names them.
- **Dedicated server:** Runtime modules only.
- **Aphelyon services:** `ArcaneCore`, `ArcaneNet`, `ArcaneCrypto`.

**ImGui's role (user, 2026-10-11).** ImGui is a developer/debug UI and a *temporary* game-UI stopgap, never Arcane's game-UI API. Games may keep shipping it for developer debug panels. Arcane's own game UI is roadmap item D7 (pre-1.0: a retained runtime UI with flex layout, themes, MSDF text, localization, gamepad navigation, data binding and a UI editor -- a Slate + UMG counterpart), and after 1.0 Arcane's own UI replaces imgui as the editor UI layer. This arc keeps that path open: `ImGui`/`ArcaneImGui` are optional per host behind ArcaneClient's `HostOverlay` seam, a future game-UI module (Client type) plugs into the same seam, and `ARC_GAME_HAS_IMGUI` / `OnDrawUI` are the imgui debug path only.

### 1.5 The untangle (measured 2026-10-11 by `#include` edges)

Verified exhaustively by the Codex sweep (`.superpowers/sdd/2026-10-11-modularization/sweep.py`, 415 engine files) and re-run with every row below applied (`recheck.py` -> `sweep-r2.md`): **0 unassigned files, 0 undeclared dependencies, 0 type-rule violations, 0 cycles.** Every module lists `ArcaneCore` explicitly when it includes it. Vendor headers a module's own public headers include are public `thirdParty` of that module (Core: Astra, glm, spdlog, nlohmann, Mosaic), and flow to consumers through public dependencies.

| Edge today | Fix |
|---|---|
| `Base/Runtime.*`, `Base/ProcessContext.*` include Scene/Plugin/Project/Sim/Assets/Serialization | move to `ArcaneEngine` |
| `Base/Engine.cpp:2` includes `Plugin/PluginABI.hpp` for `kGamePluginABIVersion` | the constant moves to a Core header that `PluginABI.hpp` includes; `Engine.*` stays in Core |
| `Guid.cpp:3` includes `Crypto/Crypto.hpp` for the CSPRNG | the OS CSPRNG moves to Core (`Arcane/Platform/SecureRandom.hpp`, `Arcane::SecureRandomBytes`); Guid and Crypto call it |
| `ArcaneCore.cpp:11-14` includes Crypto/Net headers (instantiation TU) | those includes move to `ArcaneCrypto`'s and `ArcaneNet`'s own `ARC_MODULE` TUs |
| `Scene/EngineRoster.hpp:23`, `Scene/EngineResourceTypes.cpp:3` include physics headers | both move to `ArcaneEngine` (their includers -- `Runtime.cpp`, `ProcessContext.cpp`, `Project/ProjectHost.hpp` -- are all Engine) |
| `Assets/*` includes `Project/AssetId.hpp` | `AssetId` moves to `ArcaneAssets` |
| `Assets/Assets.cpp` includes `Serialization/IdentityFieldRule.hpp` | the header (header-only, depends only on `Json.hpp`) moves to `ArcaneAssets` as `Arcane/Assets/IdentityFieldRule.hpp`; `Serialization/ReflectionJson.hpp` and the editor's `AssetPanelModel.hpp` include it from there |
| `Scene/PhysicsSystem.hpp` includes `Physics2D.hpp` | `PhysicsSystem` moves to `ArcanePhysics2D` |
| `Config/PlayerSettings.hpp` includes `Plugin/SystemFactory.hpp` (for `NetMode` only) | `NetMode` moves into `ArcaneCore` (a Core-level enum header); `SystemFactory` re-exports nothing |
| `Render/Nri/NriGraphContext.hpp` includes `Host/HostConfig.hpp` | the render graph takes the values it needs as a Render-owned struct; Host fills it |
| `Render/Nri/nodes/ImGuiNriNode.*`, `NriGraphContext.hpp:247`, `Host/GpuContext.hpp:27` (ImGuiLayer), `Client/ClientRuntime.cpp:15` (imgui.h) | **overlay hook**: ArcaneClient owns a small overlay interface (per-frame begin/draw + a render-graph node factory); ArcaneImGui implements it; hosts (Runtime, Editor, packages whose game names ImGui) install it. ImGui stays optional per host |
| `Plugin/GameModule.hpp:50` includes `imgui.h` (context adoption in Init) | gated by `ARC_GAME_HAS_IMGUI`, which `arcane.lua` defines when `ImGui` is in the game's closure |
| `Render/PhysicsDebugDraw.*` includes Manifold2D internals (`:28-35`) | split: the GATHER half (reads solver/broadphase, emits primitive lists) moves to `ArcanePhysics2D`; the DRAW half (Batcher2D, `PhysicsDebugSettings`) stays in Render. Manifold2D stays private to Physics2D |
| `Host/HostConfig.cpp:3-4` (VerifyReport, ReferenceImages) and the other Host users of the verify harness | `ArcaneClient` exposes hooks; `ArcaneVerify` plugs in. A Client module may not depend on a Developer module (s2.3) |
| `ArcaneClient/src/Arcane/Base/Api.hpp` (`ARC_API`) | retired in P2; each Client module includes its own `Api.hpp` |

## 2. Descriptors and build machinery

### 2.1 Disk layout

```
Arcane/
  Source/
    Runtime/<Module>/      <Module>.arcmodule, src/Arcane/<Folder>/...
    Client/<Module>/
    Developer/<Module>/
    Editor/<Module>/
    Programs/<Program>/
  ThirdParty/              unchanged path (vendor sync scripts depend on it);
                           ImGui + ImGuiNodeEditor gain .arcmodule files
  Tests/ArcaneTests/  Tests/Fixtures/<fixture>/
  ReferenceProject/  build/  scripts/  docs/
```

**Include paths do not change.** Each module keeps `src/Arcane/<Folder>/`, so
`#include <Arcane/Render/...>` is identical for the engine, games and Aphelyon.
Moves use `git mv` so history follows.

### 2.2 The descriptor (`<Module>.arcmodule`, JSON)

```json
{
  "name": "ArcaneRender",
  "type": "Client",
  "dependencies": {
    "public":  ["ArcaneScene", "ArcaneAssets", "ArcaneWindow"],
    "private": ["ArcaneCore"]
  },
  "thirdParty": { "public": [], "private": ["NRI", "freetype", "msdfgen"] },
  "platforms": {
    "windows": { "links": ["d3d12", "dxgi"] },
    "linux":   { "links": ["dl", "pthread"] }
  }
}
```

- **public** = my headers include it, so my consumers inherit its include path and link; **private** = my `.cpp` only.
- `thirdParty` has the same public/private split: a compiled vendor lib is private (`NRI` in Render); a header-only lib that the module's own headers include is public (Astra and glm in Core). Only `ImGui`/`ImGuiNodeEditor` are depended on by name as modules.
- Declarative data on purpose: premake reads it with `json.decode` now; a future Rust arcbuild reads the same files with serde (s7).
- Unknown keys are a generate-time error (a typo must not silently drop a dependency).

### 2.3 Types and the dependency rules

| Type | May depend on |
|---|---|
| `Runtime` | `Runtime` |
| `Client` | `Runtime`, `Client` |
| `Developer` | anything except `Editor` |
| `Editor` | anything |
| `Game` | `Runtime`, `Client` |
| `Program` | anything |

### 2.4 Generate-time checks (premake; failure aborts with a named error)

1. **cycle** -- the module graph is acyclic; the error prints the cycle.
2. **type rule** -- per s2.3; the error prints the offending edge.
3. **folder** -- a descriptor's `type` matches its `Source/<Type>/` folder (vendor modules under `ThirdParty/` exempt).
4. **declaration** -- every module has exactly one `ARC_MODULE(<name>)` in its sources.
5. **api header** -- every module's `Api.hpp` matches the template for its name.

### 2.5 Layouts, export macro, declaration

- `premake5 <action> --layout=modular|monolithic` (default `modular`). Module `kind`: `SharedLib` / `StaticLib`. Monolithic defines `ARC_MONOLITHIC` workspace-wide.
- Output dirs per layout: `bin/<cfg>-<sys>-<arch>-md/` (modular, today's) and `bin/<cfg>-<sys>-<arch>-mono/`. Both coexist; the SDK ships both.
- **Export macro.** Each module has a checked-in `src/Arcane/<Folder>/Api.hpp`:
  ```cpp
  #pragma once
  #include <Arcane/Core/ModuleApi.hpp>   // ARC_DLL_EXPORT / ARC_DLL_IMPORT per platform
  #if defined(ARC_MONOLITHIC)
      #define ARC_NET_API
  #elif defined(ARC_NET_EXPORTS)
      #define ARC_NET_API ARC_DLL_EXPORT
  #else
      #define ARC_NET_API ARC_DLL_IMPORT
  #endif
  ```
  A header (not command-line defines as UE does) so consumers outside our build -- Aphelyon's Server, IDE indexers, a future Rust arcbuild -- need only `ARC_MONOLITHIC` or nothing. Checked in (the preprocessor cannot mint macro names); check 5 keeps them honest. `ARC_CORE_API` keeps its name; today's `ARC_API` (ArcaneClient) is retired, each symbol taking its new module's macro. Hidden default visibility stays (Linux/macOS), as today.
- **Declaration.** `ARC_MODULE(ArcaneNet)` in one `.cpp` defines `extern "C" void ArcModule_ArcaneNet()` (the symbol is `ArcModule_<Name>`; `_ArcModule_<Name>` on ld64, which is what `-u` must spell). Every program and package links a forced reference for each module it uses (`/INCLUDE:` MSVC; `-u` GNU ld / ld64 with the leading underscore). A missing or undeclared module fails the link.
- **Staging.** Each program stages the transitive closure of module DLLs from the graph (plus vendor runtime DLLs, s3.4). The hand-written postbuild copy blocks are deleted.
- **Solution folders** from type: `Runtime`, `Client`, `Developer`, `Editor`, `Programs`, `ThirdParty`, `Tests`, `Tests/Fixtures`. A project cannot be filed by hand.

## 3. One-exe packaging

Scope: the **link half** of roadmap D15. Cooking, installers and Dist crash capture remain D15's.

### 3.1 One game interface, two ways to obtain it

`ARC_GAME_MODULE(Type)` additionally defines a `GameModuleTable` (the seven
entry points: ABIVersion, Init, Shutdown, FixedUpdate, Update, DrawUI,
SaveState/LoadState) and `ArcGameModule_Get()` returning it.

- **modular:** exports unchanged; `PluginHost` fills the same table from the DLL.
- **monolithic** (`ARC_MONOLITHIC`): nothing exported; the host calls `ArcGameModule_Get()` as a linked symbol.

Every call after obtaining the table is one code path in both layouts.

### 3.2 Off in monolithic

- ABI gate -> `static_assert` (engine and game build together).
- Hot reload, file watcher, Rebuild Game Module: never constructed.
- Developer and Editor modules: never linked; a game depending on one fails `package`, naming the dependency chain.

### 3.3 `arcbuild package --project <dir> --config <cfg>`

1. Generate + build the monolithic engine layout for `<cfg>`.
2. Generate a `<Game>` executable project (name from `.arcproj`): the Runtime host's code (moved from `ArcaneRuntime/src` into a small shared library both `ArcaneRuntime.exe` and packages link), a generated `main.cpp`, the game as a static library.
3. Link every module the game transitively needs, each whole-archived (`/WHOLEARCHIVE:` / `--whole-archive` / `-force_load`) so static registrars survive, with one forced reference per module. Dead-code stripping (`/OPT:REF`, `--gc-sections`, `-dead_strip`) still drops unreferenced functions.
4. Stage `Packages/<Game>-<platform>-<cfg>/`: the exe, the `.arcproj`, content **copied raw** (D15 replaces the copy with a cook).
5. The generated `main.cpp` carries the list of linked modules and logs it at boot (`[package] modules: ...`), so a packaged run states what it contains.

**Why registrars survive** (review ruling, 2026-10-11). Whole-archive puts every object in the link; a static registrar's dynamic initializer is then listed in the platform's init table (`.CRT$XCU` MSVC, `.init_array` ELF, `__mod_init_func` Mach-O), which each linker's dead-code stripping keeps as a root -- no reachability from `ArcModule_*` is needed. Only game code uses `ARC_COMPONENT`/`ARC_SYSTEM` (emitted by `ArcaneEditor/src/Project/ClassTemplates.cpp`); their list head is a function-local static private to each image (`Plugin/GameSystems.hpp:21`, `GameComponents.hpp:56`), which in a package is one list for one game: the intended semantics.

### 3.4 Vendor runtime libraries that stay separate files

All Arcane and game code lands in the exe. Platform-mandated vendor runtime
libraries are staged beside it: D3D12 Agility SDK (`D3D12Core.dll` in `D3D12/`),
DXC, MoltenVK. SDL3 goes static if its license and build allow (settled in planning).

### 3.5 Server packages: later

A packaged dedicated server needs the game built without its Client
dependencies -- game client/server module separation, which belongs to the
replication arc. This arc packages client games only.

## 4. Game and SDK contract

- **Game descriptor.** `<sourceDir>/<name>.arcmodule`, type `Game`, listing direct dependencies; the SDK computes the closure. Example: `ReferenceProject/Source/Game/ReferenceGame.arcmodule` -> `ArcaneEngine, ArcaneClient, ArcanePhysics2D, ArcaneImGui`.
- **No descriptor** -> default set = all Runtime + Client modules + `ImGui` + `ArcaneImGui` (exactly today's surface). The New Project template and the C++ Class wizard emit a descriptor.
- **Enforcement for games.** Including a header of an undeclared module fails with a missing header. The flat `Arcane::` names are untouched; only which module ships each header changes (`Physics2D.hpp` -> `ArcanePhysics2D`, `Ecs.hpp` -> `ArcaneScene`).
- **`build/arcane.lua`.**
  - `arcane_game_module(name)` reads the game descriptor and links/includes/stages only that closure, import libs or static libs by layout.
  - New `arcane_module_consumer { ... }` for non-game consumers.
  - `arcane_core_consumer()` is **removed** (no alias: after the split it would silently mean Core+Net+Crypto).
  - `arcane_core_stage_dll()` -> `arcane_stage_modules()`.
- **ABI.** `kGamePluginABIVersion` / `engine.abi` stay one SDK-wide number; **58 -> 59**. No per-module ABIs (modules always ship together). Engine modules never hot-reload (as today).

| Consumer | Change |
|---|---|
| ReferenceProject | add `ReferenceGame.arcmodule`; abi 59 |
| Aphelyon `Game/` | add `Aphelyon.arcmodule`; abi 59 (supersedes the user's uncommitted 57->58 restamp in their working copy) |
| Aphelyon `Server/` | 7x `arcane_core_consumer()` -> `arcane_module_consumer { "ArcaneCore", "ArcaneNet", "ArcaneCrypto" }`; stage the three DLLs |
| Test fixtures (hot-reload plugins, ReferenceGameUnderTest, TemplateSmokePlugin) | descriptors; HotReloadPluginBad keeps its deliberate ABI mismatch |

## 5. Phases

Ruling (user, 2026-10-11): **`main` does not stay green during the arc, and
there is no testing between phases.** The arc lands on `feat/modularization` and
is tested wholesale at the end; the shape is adjusted from those results. Two
cheap habits remain: each phase ends with one local Windows Debug **build** (no
tests, no CI), and **one commit per phase** so a late regression bisects by phase.

| # | Phase |
|---|---|
| P0 | Descriptor machinery: JSON reader, checks 1-5, `--layout`, `ModuleApi.hpp`, `ARC_MODULE`, solution folders from type |
| P1 | Untangle (s1.5) |
| P2 | Carve the 19 modules into `Source/<Type>/<Module>/` (`git mv`), descriptors, `ImGui` vendor module, every export re-tagged to its module's macro |
| P3 | Programs, tests, fixtures move; automatic staging; postbuild copy blocks deleted |
| P4 | Game contract: `GameModuleTable`, game descriptors, `arcane.lua` rewrite, ABI 59, ReferenceProject + fixtures |
| P5 | Monolithic layout + `arcbuild package` (shared Runtime-host lib, generated main, whole-archive + forced refs, staging). Includes one WSL2 link probe on this desk (GNU ld: whole-archive + `-u` + a static registrar survives `--gc-sections`); macOS is discovered in CI |
| P6 | Linux/macOS plumbing (rpath, visibility, GNU/ld64 whole-archive and forced refs) + CI workflow matrix |
| P7 | Aphelyon migration (Game descriptor + abi 59; Server -> `arcane_module_consumer`) |

## 6. Testing (wholesale, at the end)

**New tests.**
1. `[build-generator]`: a cycle, a type-rule violation, a folder mismatch, a missing `ARC_MODULE`, and a malformed descriptor (unknown key) each fail generation with their named error.
2. Link-fail fixture: a module without its declaration fails the link (the flat API compile-fail guard style).
3. Registrar survival: the packaged ReferenceProject boots with the **full** component and system roster -- names and count -- equal to the modular Runtime's.
4. Host isolation: the server program's binary imports no Client module; a packaged game contains no Developer/Editor module -- checked by inspecting the built binaries' dependencies/symbols.

**Full run.**
- Modular, 3 platforms x Debug/Release/Dist: full suite, golden gate, witness (today's 12 CI legs).
- Monolithic, 3 platforms: `arcbuild package` ReferenceProject Release + boot smoke of the packaged exe **run in place from `Packages/`, with no SDK or `bin/` on PATH** (3 new CI legs; WARP on Windows, lavapipe on Linux, no-GPU boot checks on macOS).
- Measurement (reported, not gated): frame time modular vs monolithic on the ReferenceProject physics scene, Release, same machine.
- Aphelyon: Game builds and loads in the editor; Server suites green.
- Then adjust the shape from the results, and merge.

## 7. Later arcs this one prepares

- **arcbuild in Rust** (user direction 2026-10-11): replaces premake; phase 1 generates ninja + IDE projects (VS, `compile_commands.json`, Xcode; never force tooling), phase 2 an own executor/cache only if a measured build-time breakdown justifies it; absorbs "arcbuild II". Reads the same `.arcmodule` files.
- **Live updates:** (1) data hotfixes, (2) server code hot reload, (3) client native-code hotfix (PC only; signed, anti-cheat aware). Tier 3 needs a game shipped in the modular layout -- the shipping layout is a per-project choice.
- **Server packages** with the replication arc (s3.5); **WASM/console** reuse the monolithic layout.
- **A further `ArcaneCore` split** (e.g. Math/Guid/Jobs or Cli/Build out) only if measured rebuild triggers point at it.

## 8. Costs, stated plainly

- Every exported declaration is re-tagged (P2): large, mechanical, review-heavy.
- Two layouts x three platforms: 3 more CI legs; monolithic link plumbing on three linkers.
- A cross-module call is an indirect call in the modular layout; seams are coarse and hot paths (math, Astra iteration, the flat facades) stay header-only/inline, so this is not expected to measure. The monolithic layout has no such cost.
- One SDK ABI bump (59); Aphelyon migrates in the same arc.

## 9. Rulings record (2026-10-11)

| # | Ruling |
|---|---|
| R1 | Goals: boundaries, ship-only-what-needed, iteration. Library swap dropped. |
| R2 | Both layouts built now; monolithic = full one-exe (engine + game), not engine-only. |
| R3 | Granularity: everything in one arc, including the Core untangle. |
| R4 | Approach: Unreal's model, refined (s0 table). Astra/registrar changes not needed. |
| R5 | Descriptors are declarative JSON `.arcmodule`; premake stays this arc; Rust arcbuild later. |
| R6 | Names: `ArcaneEngine` for the headless world layer; `ArcaneCore` keeps its name. |
| R7 | Disk: `Source/<Type>/<Module>/`, include paths unchanged, ThirdParty stays; solution folders from type. |
| R8 | Packaging = link half of D15; client games only. |
| R9 | Game descriptor of type `Game`; `arcane_core_consumer()` removed; ABI 59. |
| R10 | No green-throughout; build-only per phase, one commit per phase, test wholesale at the end. |
| R12 | Codex sweep (2026-10-11): map corrected per s1.5 (Engine.* stays in Core; roster to Engine; CSPRNG to Core; overlay hook keeps ImGui optional; PhysicsDebugDraw split); re-run clean. |
| R11 | Grok Bot review (2026-10-11): registrar-reachability concern rejected with the init-table reasoning (s3.3), test strengthened to full roster; mid-arc test runs declined per R10; accepted: in-place package boot (WARP), modular-vs-mono frame-time report, WSL2 link probe in P5, module list logged at boot, forced-ref symbol spelled, IdentityFieldRule -> ArcaneAssets. |
| R13 | Module attach (P2): Astra's current-`TypeContext` slot is per module image, so each `ARC_MODULE` TU registers an attach callback with Core (`Arcane/Core/ModuleAttach.hpp`: `RegisterModuleAttach`, `AttachAllModules`); every site that installs the process context calls `AttachAllModules`, and a module registered later attaches at once. Collapses to one image in monolithic. |
| R14 | P2 rulings (Step 0 of P3): SDL3 is a prebuilt vendor module (descriptor `prebuilt` field: include, link, libdir, runtime files, rpath, a Debug override), one instance per process -- `SDL3.dll` on Windows via the overlay triplet's per-port `VCPKG_LIBRARY_LINKAGE dynamic`, the system shared library elsewhere; every module that calls SDL depends on it (privately), and `arc_module_stage` stages its runtime file. Replaces ArcaneWindow's static SDL3 and its `/EXPORT` list. The monolithic package's SDL3 linkage stays P5's probe (s3.4). The s1 tables match the built graph. |
| R15 | P4: a modular (hot-reloadable) game DLL emits only its `ArcModule_<Name>` anchor -- `ARC_MODULE`'s attach registration would outlive the DLL in Core's process-lifetime list; the game pins its `TypeContext` in `GamePlugin_Init`. The monolithic game uses the full `ARC_MODULE`. |
| R16 | P5: `ArcaneRuntimeHost` (`Source/Programs/ArcaneRuntimeHost/`: RuntimeApp, RuntimeFrame, `RuntimeHost::Run`) is a static lib in both layouts; `ArcaneRuntime.exe` is its modular `main`. The verification vehicle (ArcaneVerify, Developer) compiles in only under `ARC_RUNTIME_HOST_VERIFY` (modular); a package refuses `--headless` and `HostConfig::Parse` already refuses the other verify flags without the hooks. |
| R17 | P5: the monolithic workspace is generated beside the modular one (`Arcane-mono.slnx`, `<Project>-mono.vcxproj`) and builds only Runtime + Client modules and `ArcaneRuntimeHost`. `Toolchain::DiscoverSolution` skips `*-mono.slnx` (it sorts first); `DiscoverPackageSolution` is its reader. |
| R18 | P5: static modules absorb no system/prebuilt libraries (premake merges a StaticLib's `links` into the `.lib`; whole-archiving several would duplicate SDL3's imports): the system links moved into the descriptors (`platforms.<os>.links`) and `arc_module_package` links them once. |
| R19 | P5: `arcbuild package` regenerates the PROJECT's own premake5.lua with `--layout=monolithic` (not a generated premake file): `arcane_game_module` builds the game static lib and `arcane_package_exe` declares `<Project>Package` (targetname `<Project>`), writing `Intermediate/Package/PackageMain.cpp` and `stage.json`. The project keeps its own workspace customisations. |
| R20 | P5: staging is data-driven -- `stage.json` (per configuration: exe, pdb, prebuilt runtime files, DXC, Agility SDK, engine data, `<Project>/` with `.arcproj`, `Content/`, `Config/`, `Intermediate/Artifacts/`) copied by arcbuild's `StagePackage` into a fresh `Packages/<Project>-<platform>-<cfg>/`. |
| R21 | P5: SDL3 stays a separate runtime file in Windows packages (`SDL3.dll`; the triplet builds it dynamic per R14). SDL3 discovery moved to `build/modules.lua` (`arc_discover_sdl3`, run lazily) so consumers that never link SDL3 need no vcpkg. |
| R22 | P5: the first package link found a dead stb implementation TU in ArcaneRender (a P2 leftover; ArcaneWindow imports ArcaneAssets' exported stb) -- LNK2005 under whole-archive; deleted. The crash reporter is not staged in packages (Dist crash capture is D15's). |

---

## Implementation detail: P0, P4, P5

The phases not listed here (P2, P3, P6, P7) are mechanical: the spec plus
the sweep's site lists (`.superpowers/sdd/2026-10-11-modularization/sweep.md`)
are their whole brief. Every phase ends with a local Windows Debug build of
`Arcane.slnx` (no tests, no CI) and one commit on `feat/modularization`.

### P0: Descriptor machinery

**Files**
- Create `build/modules.lua`: the ONE descriptor loader + checker, `include`d by both `premake5.lua` (engine workspace) and `build/arcane.lua` (game/consumer workspaces, rooted at `ARCANE_SDK`). Never two loaders.
- Create `ArcaneCore/src/Arcane/Core/ModuleApi.hpp` (moves with Core in P2): `ARC_DLL_EXPORT` / `ARC_DLL_IMPORT` per platform (`__declspec(dllexport/dllimport)` on Windows; `__attribute__((visibility("default")))` / empty on ELF/Mach-O) and the `ARC_MODULE(Name)` macro.
- Create `build/templates/Api.hpp.in`: the s2.5 header with `@MODULE@` (upper snake, `ArcaneNet` -> `NET`) placeholders; check 5 renders it and byte-compares (line endings normalized).
- Modify `premake5.lua`: `newoption { trigger = "layout", allowed = { {"modular"}, {"monolithic"} }, default = "modular" }`; `outputdir` suffix `-md` / `-mono` by layout; `defines { "ARC_MONOLITHIC" }` workspace-wide when monolithic.
- Bootstrap descriptors for TODAY's two DLLs (`ArcaneCore/ArcaneCore.arcmodule` type Runtime, `ArcaneClient/ArcaneClient.arcmodule` type Client) so the loader drives real projects from P0; P2 replaces them with the 19. The folder check (s2.4 #3) is skipped for paths not under `Source/` until P2 (one flag in modules.lua, deleted in P2).

**`build/modules.lua` API**
```lua
-- Loads every descriptor under the given roots; returns { byName = {...}, order = {...} } (topological order).
arc_modules_load(roots)            -- roots: { "Source", "ThirdParty", <bootstrap paths> }; errors on any check failure
arc_module_closure(mods, name, kind) -- kind "public"|"all": transitive dependency names
arc_module_project(mods, name)     -- emits the premake `project` for one module (kind by layout, files, includedirs,
                                   -- links, ARC_<M>_EXPORTS, group from type, rpath/visibility per platform)
arc_module_stage(mods, names)      -- postbuild copy of the transitive DLL closure (modular only; no-op in monolithic)
arc_module_forced_refs(names)      -- linkoptions: /INCLUDE:ArcModule_X | -Wl,-u,ArcModule_X | -Wl,-u,_ArcModule_X
```
- Descriptor schema (strict; unknown key -> error naming file + key): `name` (string, must equal file stem), `type` (enum), `dependencies.public|private` (string arrays), `thirdParty.public|private` (string arrays; names must exist in a `ARC_THIRDPARTY` table in modules.lua mapping name -> { include = ..., link = ... } built from today's `IncludeDir` entries), `platforms.<windows|linux|macosx>.links|defines` (string arrays).
- Checks, each `error("[arcmodule] <check>: ...")` so `[build-generator]` tests can match the prefix:
  1. `cycle` -- DFS with an on-stack set; message prints the cycle `A -> B -> C -> A`.
  2. `type-rule` -- s2.3 table; message prints `A (Client) -> B (Developer)` and the descriptor file.
  3. `folder` -- `type` vs `Source/<Type>/`.
  4. `declaration` -- scan the module's `.cpp` files with `io.readfile` for exactly one `ARC_MODULE(<name>)` (a `Game` module satisfies it through `ARC_GAME_MODULE`, P4).
  5. `api-header` -- `src/Arcane/<Folder>/Api.hpp` equals the rendered template. (`ArcaneCore` keeps `ARC_CORE_API`; its rendered name comes from a `apiMacro` override field allowed for ArcaneCore only.)
- `ARC_MODULE(Name)` expansion: `extern "C" ARC_DLL_EXPORT_IF_MODULAR void ArcModule_##Name() {}` -- exported in the modular layout too, so a probe (and future tooling) can `dlsym` it.

**Tests** (written in P0, run at the end): five `[build-generator]` cases in `ArcaneTests/src/BuildDriverTest.cpp` style, each generating a tiny fixture tree under a temp dir with the bundled premake (`ThirdParty/premake5/premake5.exe`, as the existing tests do) and asserting a non-zero exit plus the `[arcmodule] <check>` prefix on stderr: cycle, type-rule, folder, declaration, unknown key.

### P1: Untangle -- the inversions

Mechanical moves per s1.5 plus three small seams. Each seam is the smallest interface that removes the edge; no new behavior.
- **Verify hooks.** `ArcaneClient` declares a hook struct (function pointers or a tiny interface, in `Host/`) for what `HostConfig.cpp:3-4` and the other Host users call into today (VerifyReport, ReferenceImages, OffscreenVehicle, Verdict). `ArcaneVerify` provides the implementation; ArcaneRuntime, ArcaneEditor and ArcaneTests install it at startup. A packaged game installs nothing; verify flags then report "verify harness not linked" and exit non-zero.
- **Overlay hook.** `ArcaneClient` declares `HostOverlay` (BeginFrame, Draw, EndFrame, and a factory that returns the render-graph node to add, or null). `GpuContext` holds a `HostOverlay*` instead of `ImGuiLayer`; `NriGraphContext.hpp:247` asks the overlay for its node instead of including `ImGuiNriNode.hpp`; `ClientRuntime.cpp:15` stops including `imgui.h` (its imgui calls move into ArcaneImGui's implementation). `ArcaneImGui` implements `HostOverlay` with today's `ImGuiLayer` + `ImGuiNriNode`. The editor's and runtime's existing ImGui behavior must be byte-identical (goldens will prove it at the end).
- **HostConfig values for the render graph.** `NriGraphContext.hpp:226` takes a Render-owned struct (only the fields it reads from `HostConfig`); Host fills it.
- **PhysicsDebugDraw split.** `ArcanePhysics2D` gains `PhysicsDebugGeometry` (gather: walks Manifold2D broadphase/solver/narrowphase trace as `PhysicsDebugDraw.cpp:28-35` does today, emits lines/shapes/colors into a plain vector); Render's `PhysicsDebugDraw` keeps only the drawing through `Batcher2D` and `PhysicsDebugSettings`. Output must match pixel-for-pixel (the physics goldens prove it).
- **Small moves:** `kGamePluginABIVersion` -> a Core header; CSPRNG -> `Arcane/Platform/SecureRandom.hpp` (`SecureRandomBytes`), callers updated; `NetMode` + `ToString(NetMode)` -> Core; `IdentityFieldRule.hpp` -> Assets; `AssetId.hpp` -> Assets; `PhysicsSystem.hpp`, `EngineRoster.hpp`, `EngineResourceTypes.*` to their modules (folder moves happen in P2; P1 only fixes the includes/ownership so P2 is pure `git mv`).
- **Verification for P1 specifically** (build only, per R10): re-run `python .superpowers/sdd/2026-10-11-modularization/recheck.py` with the REMOVED-site list emptied -- it must report 0 missing / 0 type violations / 0 cycles against the real edited tree.

### P4: Game contract

**Reuse, do not add.** `Arcane::PluginVTable` (`ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:1226`) is already the host's table of the eight entry points, and `Plugin` (`Plugin/Plugin.hpp:108`) already wraps `Module` + vtable. The spec's `GameModuleTable` IS `PluginVTable`; no new type.

**Files**
- `Plugin/GameModule.hpp` (`ARC_GAME_MODULE_ABI`, line ~306): keep the eight `GamePlugin_*` functions; under `ARC_MONOLITHIC` drop `ARC_GAME_MODULE_EXPORT` from them and add
  ```cpp
  extern "C" const ::Arcane::PluginVTable* ArcGameModule_Get()
  { static const ::Arcane::PluginVTable t{ &GamePlugin_ABIVersion, &GamePlugin_Init, ... , &GamePlugin_DrawUI }; return &t; }
  ```
  plus `ARC_MODULE(<ARC_MODULE_NAME>)` (the define `build/arcane.lua` already passes) so check 4 and the forced reference cover games. In the modular layout `ArcGameModule_Get` is also emitted (exported), so both layouts can share one acquisition test.
  Gate the imgui context adoption (`GameModule.hpp:50` include + the `SetCurrentContext`/allocator calls in `State::Init`) on `ARC_GAME_HAS_IMGUI`, which `arcane.lua` defines when `ImGui` is in the game's closure.
  Monolithic ABI gate: `static_assert(Abi == ::Arcane::kGamePluginABIVersion)` inside the macro when `ARC_MONOLITHIC` (HotReloadPluginBad is never packaged).
- `Plugin/Plugin.hpp/.cpp`: add `static Plugin Plugin::FromStatic(const PluginVTable&, std::string cvarModule)` -- no `Module`, `m_module` empty (make it `std::optional<Module>` or a null-handle `Module`, whichever the existing Module type supports without new states). Cvar ownership: register the game's cvar module by NAME only (no image range; nothing is ever unloaded in a package).
- `Plugin/PluginHost.hpp/.cpp`: a static mode. `PluginHost::Load()` under `ARC_MONOLITHIC` adopts `Plugin::FromStatic(*ArcGameModule_Get(), ...)` and skips copy/load/ABI-probe; `Reload`, `ReloadFresh`, `Poll` compile to no-ops returning success/false as their callers expect (grep every caller: `PluginHost.cpp:504,758`, `ArcaneEditor/src/App/PlayMode.cpp:21,157`, `EditorAppFrame.cpp:2016`); the watcher is not constructed. `ArcGameModule_Get` is declared `extern "C"` in PluginHost.cpp only under `ARC_MONOLITHIC`, so the modular engine never references it.
- `PluginABI.hpp:1171`: `kGamePluginABIVersion = 59`.
- `build/arcane.lua`:
  - `include` `build/modules.lua`; `arc_modules_load({ ARCANE_SDK .. "/Source", ARCANE_SDK .. "/ThirdParty" })`.
  - `arcane_game_module(name)`: read `<sourceDir>/<name>.arcmodule` (absent -> the s4 default set); replace the hard-coded `includedirs` (lines ~170-182: `ArcaneClient/src`, `ArcaneCore/src`, glm, Astra, ...) with the closure's module `src` roots + public thirdParty includes; replace `libdirs`/`links { "ArcaneCore", "ArcaneClient" }` with the closure; `IMGUI_API` define only when `ImGui` is in the closure; `kind` stays `SharedLib` (modular) / becomes `StaticLib` (monolithic, consumed by `package`).
  - `arcane_module_consumer(names)`: the `arcane_core_consumer()` recipe (lines ~364-420: /MD, flavor defines, AVX2, /utf-8) parameterized by a module list.
  - delete `arcane_core_consumer()`; rename `arcane_core_stage_dll()` -> `arcane_stage_modules(names)`.
- Descriptors: `ReferenceProject/Source/Game/ReferenceGame.arcmodule`; one per fixture under `Tests/Fixtures/` (sweep s7 lists what each includes). `.arcproj` `engine.abi` 59 for ReferenceProject and every fixture project file.
- Editor emitters: the New Project template and `ArcaneEditor/src/Project/ClassTemplates.cpp` write a descriptor when they create a project (C++ Class on an existing project leaves an existing descriptor alone).

### P5: Monolithic layout + `arcbuild package`

**Runtime host library.** Create `Source/Programs/ArcaneRuntimeHost/` (premake `StaticLib` in BOTH layouts; type `Program`, not a module): move `ArcaneRuntime/src/RuntimeApp.*`, `RuntimeFrame.*` and main's body into it behind
```cpp
namespace Arcane::RuntimeHost { struct PackageInfo { const char* projectRelPath; }; int Run(int argc, char** argv, const PackageInfo* pkg); }
```
`pkg == nullptr` is today's ArcaneRuntime behavior. `ArcaneRuntime/src/main.cpp` becomes `ARC_AGILITY_SDK_EXPORTS();` + `return Arcane::RuntimeHost::Run(argc, argv, nullptr);`. The Agility export MUST stay in each EXE's own main (D3D12 reads it from the process image; `Render/AgilitySdk.hpp:13`).

**Generated package project.** `arcbuild package` writes `<project>/Intermediate/Package/<cfg>/` containing:
- `premake5.lua`: `include(ARCANE_SDK .. "/build/arcane.lua")`; `arcane_package("<Game>")` -- a new arcane.lua function: `kind "WindowedApp"` (ConsoleApp in Debug for logs), `--layout=monolithic` libdirs (`bin/<cfg>-<sys>-<arch>-mono/`), links the game static lib + every module in the game's closure + `ArcaneRuntimeHost`, each module whole-archived, `arc_module_forced_refs(closure)`, `/OPT:REF` / `-Wl,--gc-sections` / `-Wl,-dead_strip`.
  - Whole-archive spellings (already proven for imgui at `premake5.lua:916-938`): MSVC `linkoptions { "/WHOLEARCHIVE:<lib>" }`; GNU `-Wl,--whole-archive <path/lib.a> -Wl,--no-whole-archive` (path spelled out: the flag applies to FOLLOWING inputs); ld64 `-Wl,-force_load,<path/lib.a>`.
- `main.cpp` (generated):
  ```cpp
  #include <Arcane/Render/AgilitySdk.hpp>
  #include <Arcane/RuntimeHost/RuntimeHost.hpp>
  ARC_AGILITY_SDK_EXPORTS();
  static const char* const kModules[] = { "ArcaneCore", ... };   // the closure, for the boot log line
  int main(int argc, char** argv)
  { Arcane::RuntimeHost::PackageInfo p{ "<Game>.arcproj" }; return Arcane::RuntimeHost::Run(argc, argv, &p); }
  ```
  `Run` logs `[package] modules: ...` from a weak/registered list (pass `kModules` through `PackageInfo` -- add the field) and resolves the project as `<exe dir>/<projectRelPath>` when `pkg` is set and no `--project` was given.

**arcbuild changes** (`arcbuild/src/`):
- `Request.cpp:9-26`: add `package` to `ParseCommand`/`ToString` (the five-command test at `BuildDriverTest.cpp:302` becomes six).
- `Pipeline.cpp`/`Compose.cpp`: `package` = (1) engine monolithic generate+build for `<cfg>` if `bin/<cfg>-...-mono/` is missing or older than any module source (reuse the existing ProcessPlan steps with `--layout=monolithic` on the SDK workspace), (2) write the generated project (above), (3) premake + backend build of it (existing Compose path), (4) stage.
- `Stage.cpp`: the package stage: `Packages/<Game>-<platform>-<cfg>/` <- exe, `.arcproj`, `Content/` copied raw, vendor runtime files: Windows `dxcompiler.dll` (+ `dxil.dll` if present) and `D3D12/D3D12Core.dll` (+ `d3d12SDKLayers.dll` in Debug) from the same sources `premake5.lua:1230-1245` copies today; Linux `libdxcompiler.so`, `libdxil.so` (`premake5.lua:90`); macOS `libdxcompiler.dylib` (`:88`) and MoltenVK. SDL3: link static on Linux/macOS if the vcpkg/pkg-config build provides `SDL3-static`; otherwise stage the shared lib -- decide by probing in P5 and record the ruling in the ledger.

**WSL2 link probe** (R11): before the full package works, a two-file fixture under `Tests/Fixtures/LinkProbe/` (a static lib with one `ARC_MODULE` + one static registrar that sets a global, and an exe that reads it) built under WSL2 with GNU ld, `--whole-archive`, `-u ArcModule_LinkProbe`, `--gc-sections`; exit 0 iff the registrar ran. Kept as a test fixture so CI's Linux leg runs it too.
