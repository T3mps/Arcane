# Arcane Engine Architecture — Design

**Date:** 2026-06-11
**Status:** APPROVED — design contract for the engine's module architecture, plugin ABI,
hot reload, and server integration.
**Upstream:** `2026-06-10-engine-thirdparty-stack-design.md` (library stack; this spec is
the "next design conversation" it deferred), `2026-06-10-cpp-client-port-scoping.md`
(strangler-migration rationale). Gating prerequisite COMPLETE: Astra v3.1 hardening
(IWorkScheduler seam + TypeContext/ReRegisterComponent) merged and tagged.

## Names

| Thing | Name |
|---|---|
| Engine | **Arcane** — `Arcane.dll`, namespace `Arcane::`, export macro `ARCANE_API` |
| Runner | **Loom** — `Loom.exe`, thin host that runs the game plugin |
| Editor | **Grimoire** — `Grimoire.exe`, engine-native editor shell |
| Shared lib | **Core** — `Arcane.Core` static lib (types live in `Arcane::`) |
| Workspace | `Arcane/` top-level dir in the Gacha repo → `Arcane.slnx` |

## Decisions (all confirmed 2026-06-11)

1. **Location:** top-level `Arcane/` directory in the Gacha repo, own premake workspace.
   Shares `ThirdParty/` and `Server/data` (protocol/ui_screens/maps stay single-source);
   the LOVE client sits in-repo as the migration oracle. Extraction to its own repo is a
   future option once boundaries are proven (the Astra precedent).
2. **Linkage model:** **Arcane.dll**. Loom/Grimoire/Playground are hosts linking it;
   `Game.dll` links the same import lib and uses the C++ engine API directly. One engine
   instance per process. The only C ABI is the plugin entry-point set. Same-toolchain
   contract: host and game DLL are always built together from one workspace.
3. **CRT rule:** the entire Arcane workspace builds **/MD (dynamic CRT)** — memory
   crosses the Arcane.dll/Game.dll boundary, so all modules must share one heap.
   Vendored static libs get /MD flavors inside this workspace. Server and Tools keep
   their existing static-runtime conventions, unaffected.
4. **Language:** C++23 (`cppdialect "C++23"`). Astra remains a C++20-compatible
   header-only dependency. Core's server flavor builds C++20 to match Server.
5. **Engine granularity:** ONE `Arcane.dll`, modular inside by folder/namespace
   (premature DLL splits rejected; split later only if link times or boundaries demand).
6. **Hot reload:** automatic file-watch + swap with versioned DLL/PDB copies, full
   state round-trip via Astra versioned serialization, last-good rollback on failure.
7. **Editor path:** Tools/AphelyonTools coexists untouched; Grimoire absorbs editors
   only when strictly superior (LevelEditor first). Old tool retires editor-by-editor.
8. **Playground:** standalone exe first (fastest path to pixels), converts to the first
   game plugin (`PlaygroundGame.dll` hosted by Loom) once PluginHost exists — becoming
   the live test of the ABI + hot-reload pipeline.
9. **Server is a first-class member at the Core layer:** shared code converges in
   `Arcane.Core` (extracted from `Server/Common` strangler-style); shared sim lands
   there with Combat Sphere; full workspace merge deliberately deferred.

## Workspace layout

```
Arcane/                          (top-level, beside Server/ Client/ Tools/)
├── premake5.lua                 workspace "Arcane": /MD, /utf-8, C++23, Debug/Release/Dist
├── GenerateProjects.bat
├── Core/                        -> Arcane.Core (static lib, presentation-FREE)
│   └── src/{Net, Protocol, Crypto, Types, Serialization, Math}/
├── Arcane/                      -> Arcane.dll (links Core)
│   └── src/{Base, Platform, Render, Audio, Text, Assets, UI, Jobs, Plugin}/
├── Loom/                        -> Loom.exe        (~50 lines: Engine boot + RunLoop + plugin path)
├── Grimoire/                    -> Grimoire.exe    (editor shell: ImGui on NVRHI; sim-time control)
├── Playground/                  -> Playground.exe  (phase 1); scene later moves to PlaygroundGame.dll
├── Game/                        -> Game.dll        (Aphelyon gameplay; Combat Sphere lands here)
└── Tests/                       -> ArcaneTests.exe (Catch2 + rapidcheck, Server conventions)
```

`Arcane.dll` internal module responsibilities (one folder = one responsibility):
- **Base**: logging (spdlog), assertions, ids, config. **Platform**: SDL3 window/events/
  input (input_actions.json semantics), app lifecycle. **Render**: NVRHI device/swapchain
  (D3D12+Vulkan), 2D batcher, canvases, shader system (HLSL/DXC artifacts), render graph
  seam. **Audio**: miniaudio engine. **Text**: FreeType + skyline atlas + layout.
  **Assets**: loaders (stb images, fonts, shaders, JSON), lifetime/refcounts — semantics
  ported from `services.Assets`. **UI**: the JSON UI runtime (21 element types,
  constraints, bindings, BehaviorGraph interpreter). **Jobs**: enkiTS wrapper + the
  `Astra::IWorkScheduler` adapter. **Plugin**: PluginHost (watcher, loader, snapshot,
  rollback), EngineContext assembly.

### Core rules (the server tie-in)

- **Presentation-free, enforced:** Core never includes SDL3/NVRHI/audio/ImGui headers.
  It must always link into a headless server binary.
- **No mutable global state** that would duplicate across modules. Core links into
  exactly ONE module per process: the server exe on one side, Arcane.dll on the other.
  `Game.dll` consumes Core *types* via headers; stateful services (e.g. protocol ID
  registry) are reached through Arcane exports.
- **Two build flavors** of the same sources: Arcane workspace (/MD, C++23) and Server
  workspace (static CRT, C++20). The flavors never meet in one process.
- **Extraction order** (Server/Common shrinks; services keep shipping, no flag-day):
  wire framing, `ProtocolLoader`, `Types`, `Crypto`, `RateLimiter` → Core.
  `SessionCache`, `ServiceEndpoint`, `ServiceClient`, `AccountData` stay in Common,
  which links Core. The `threading_harness` framing assertions port to ArcaneTests as
  Core's wire oracle.
- **Shared sim (future, with Combat Sphere):** ported physics + combat rules land in a
  presentation-free sim module so the server can run authoritative validation with the
  same code (the port audit's T3 trigger, by design instead of by accident).

## Plugin ABI

The only unmangled C surface (everything else is direct C++ linkage):

```cpp
extern "C" {
    GAME_API uint32_t GamePlugin_ABIVersion();                    // host refuses on mismatch
    GAME_API bool     GamePlugin_Init(EngineContext* ctx);        // after every (re)load
    GAME_API void     GamePlugin_Shutdown();                      // before every unload
    GAME_API void     GamePlugin_FixedUpdate(double dt);          // 60 Hz, host-driven
    GAME_API void     GamePlugin_Update(double dt, double alpha); // per-frame; alpha = render lerp
    GAME_API void     GamePlugin_SaveState(Astra::BinaryWriter&); // hot reload: serialize world
    GAME_API bool     GamePlugin_LoadState(Astra::BinaryReader&); // hot reload: restore (migration-aware)
}

struct EngineContext {
    uint32_t               abiVersion;
    Astra::TypeContext*    typeContext;     // plugin calls Astra::SetTypeContext(...) FIRST
    Astra::IWorkScheduler* workScheduler;   // Arcane's enkiTS adapter
    Arcane::Runtime*       engine;          // C++ facade: registry, assets, render, audio, input, UI
};
```

Contractual first lines of every `GamePlugin_Init`:
`SetTypeContext` → inject scheduler into registry config → (re)register component types.
The Astra `Registry` is owned engine-side; game component types register into it from
the game module (TypeContext + `ReRegisterComponent` exist precisely for this).
Plugin code must not call `TypeID<T>::Value()` from its own static initializers
(documented Astra contract).

## Hot-reload sequence

Host watches the game DLL build output (debounced; waits until the file opens cleanly):

1. `GamePlugin_SaveState(writer)` → snapshot into a host-owned buffer (Astra versioned
   serialization — survives component layout changes between reloads).
2. `GamePlugin_Shutdown()` → `FreeLibrary`.
3. Copy `Game.dll` + `Game.pdb` to versioned names (`Game_0042.dll/.pdb`) and load the
   copy — originals never locked, msbuild never blocked (MSVC PDB-lock dodge); PDB path
   patched so debugging keeps working.
4. `GamePlugin_ABIVersion()` check — mismatch → rollback (step 6).
5. `GamePlugin_Init(ctx)` → `GamePlugin_LoadState(reader)`.
6. **Failure = one error overlay, never a lost session:** on any failure the host
   reloads the previous versioned copy and restores the same snapshot.

Manual controls: hotkey forcing the sequence; a second chord reloads WITHOUT state
restore (fresh boot) for init-logic changes. Swap time + snapshot size reported via
Tracy zone + log.

## Host loop (Loom and Grimoire share it)

```
while running:
    pump SDL events -> Arcane input
    accumulate fixed steps (60 Hz):
        Arcane pre-step (physics world, once ported)
        GamePlugin_FixedUpdate(dt)
    GamePlugin_Update(dt, alpha)
    Arcane render: UI runtime + scene submissions -> render graph -> NVRHI -> present
    plugin watcher check
```

Mirrors the proven Lua Application design (fixed 60 UPS, alpha on draw). Main thread
owns the SDL pump and render submission; sim parallelism via the injected IWorkScheduler;
structural changes from jobs via Astra CommandBuffer. Exported as `Arcane::RunLoop` so
hosts stay thin. **Grimoire** wraps the accumulator with sim-time control (pause /
single-step / time-scale) — the structural basis for play-in-editor and the LevelEditor
authoring against the live sim. Grimoire UI = Dear ImGui via first-party
`imgui_impl_nvrhi` + upstream `imgui_impl_sdl3`.

## Migration & coexistence

- **Tools/AphelyonTools:** builds and runs untouched (incl. UDP live-preview to the LOVE
  client). Grimoire absorbs an editor only when strictly superior; LevelEditor is the
  first engine-native panel (needs the real sim). Old tool retires editor-by-editor.
- **LOVE client:** stays the reference implementation + oracle. Physics port is verified
  against its determinism hashes; screens migrate as editor-authored JSON onto Arcane's
  UI runtime — hand-coded screens are NEVER ported (standing rule).
- **Data:** `Server/data` remains canonical; Arcane reads the same JSON families
  (protocol, ui_screens, maps, input_actions, characters/weapons/banners/quests).

## Bring-up order (the implementation plan derives tasks from this)

1. **M0** — `Arcane/` workspace + premake + Core extraction from Server/Common +
   ThirdParty vendoring per the stack spec (NVRHI + headers, SDL3 via vcpkg, enkiTS,
   miniaudio, FreeType, glm, stb, DXC/ShaderMake tools, Tracy).
2. **M1** — window + NVRHI device on BOTH backends + clear + present (Playground.exe).
3. **M2** — 2D renderer (batcher/canvas/shaders), text, asset loaders, ImGui backends.
4. **M3** — Playground full demo: Astra ball scene on enkiTS, collision tones, FreeType
   HUD, Tracy zones, runtime D3D12<->Vulkan swap (the stack spec's cohesive sample).
5. **M4** — Plugin module: PluginHost + Game DLL + hot reload; Playground scene becomes
   `PlaygroundGame.dll` under Loom — first live ABI/hot-reload consumer.
6. **M5** — physics port (oracle-gated, hash parity with the Lua reference) into the
   presentation-free sim layer; Playground swaps its toy collision for the real engine.
7. **M6** — Grimoire shell + sim-time control; LevelEditor work begins; Combat Sphere
   designs on top of the now-real engine.

## Testing

ArcaneTests (Catch2 + rapidcheck, Server conventions). Oracles where they exist: wire
framing (from threading_harness), physics determinism hashes (from physics_harness),
iso/world math (from world_harness). The Playground is the living integration test.
Hot-reload gets a scripted test (build variant B of PlaygroundGame, swap, assert state
survived). Engine CI (Windows-first) once the workspace is stable.

## Constraints carried forward

- Encoding: UTF-8 without BOM, ASCII in comments (Astra hardening lesson).
- Full rebuild after header changes when results look impossible (stale-object ODR
  lesson).
- `/fp:precise`, no `-ffast-math`; contraction off in the physics module (stack spec).
- Determinism gate: per-platform self-consistency first.

## Out of scope (deferred to their own designs)

- Asset pipeline (pack format, cooking, compression) — loose files + std::filesystem now.
- Render-graph internals beyond the seam (how `systems/render`'s Pipeline re-hosts).
- Server workspace merge into Arcane.slnx (future option).
- macOS (MoltenVK) and mobile platform bring-up.
- Combat Sphere and LevelEditor feature designs (they build ON this).
