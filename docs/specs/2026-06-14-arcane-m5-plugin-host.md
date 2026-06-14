# Arcane M5 — Plugin Host + Game DLL + Hot Reload — Design

**Date:** 2026-06-14
**Status:** APPROVED — design contract for the engine's plugin module (`PluginHost`),
the game plugin ABI (`Game.dll` surface), the thin runner (`Loom.exe`), and the
hot-reload pipeline. The M4 Playground scene becomes the first live plugin
(`PlaygroundGame.dll`), gated by a small Astra serialization release (Phase 0).
**Upstream:**
- `2026-06-11-engine-architecture-design.md` — the Plugin ABI, hot-reload sequence,
  and host loop are specified there (originally labelled "M4"; renumbered to M5 when
  the simulation substrate landed). This spec implements that section and records the
  one mandated deviation (whole-registry snapshot, below).
- `2026-06-13-arcane-m4-sim-substrate-scene.md` — the substrate this milestone hosts:
  `Simulation` (engine Registry), per-phase `SystemSchedulers`, `RunLoop`, `JobSystem`
  (the one-per-process enkiTS `IWorkScheduler`), and the native-Astra-relations scene
  (`LocalTransform`/`WorldTransform`/`SpriteRenderer`, `TransformPropagationSystem`,
  `RenderSubmissionSystem`, `Registry::Save`/`Load` persistence).
- `2026-06-12-arcane-2d-renderer-architecture.md` — the Batcher2D submission path the
  hosted scene renders through (host-owned; the render graph is a later milestone).

## Milestone framing and numbering

The architecture spec's bring-up table originally labelled "M4 = Plugin module
(PluginHost + Game DLL + hot reload)". When the simulation substrate shipped as the
real M4 (Astra Registry + enkiTS + schedulers + RunLoop + scene slice), the plugin
module **renumbered to M5**; physics → M6; Grimoire/LevelEditor → M7. This spec is
**M5 = Plugin Host**.

M5 is the payoff of M4: M4 built an engine-owned Registry, schedulers, and a RunLoop
specifically so a `Game.dll` could register component types and systems *into* the
engine and survive hot reload via `registry.Save(buffer)`. M5 formalizes `Game.dll`
as a **second module** that registers into the **one** engine Registry through a
shared `Astra::TypeContext`, and stands up the watcher/versioned-load/rollback
machinery that makes the edit→rebuild→see-it-live loop real.

## The structural decision left open (resolved here)

The task left one structural call to the author: **is the `Arcane::Runtime` facade a
part of M5 or a tiny precursor milestone?** Resolved: **`Arcane::Runtime` is Phase 1
of M5**, not a separate milestone. Rationale:

- `EngineContext` (the plugin ABI) *requires* an `Arcane::Runtime*` to hand to the
  plugin. The facade and the ABI are the same design conversation; splitting them is
  artificial.
- Runtime is small (it composes already-built M4 pieces: `JobSystem`, a Registry, the
  `SystemSchedulers`, a `RunLoop`) plus the two M5-specific responsibilities that have
  no earlier home: **engine-side `TypeContext` ownership/install** and the
  **snapshot/restore + swap** used by hot reload.
- Building it standalone first (Phase 1, headless-testable) gives a clean checkpoint
  before the OS-level DLL machinery (Phase 2) — which is exactly the precursor value,
  kept inside M5.

`Arcane::Runtime` **supersedes M4's `Simulation`** as the substrate owner for *hosted*
execution. `Simulation` (by-value Registry, single-module) stays as-is for the
standalone `Playground.exe`; it cannot swap its Registry, which hot reload requires.
Folding `Simulation` into `Runtime` later is a non-goal here.

## Why one surgical Astra release (Phase 0 = Astra 3.3)

The engine-arch hot-reload sequence performs a **full state round-trip on every
reload** (`SaveState` → unload → load → `Init` → `LoadState`) so a reload is robust to
component-layout changes *by construction* — no "did the layout change?" detection.
With the whole-registry snapshot deviation (below), restore is implemented as
`Registry::Load(snapshot, componentRegistry)` producing a fresh Registry that the
engine swaps in.

`Registry::Load` (Astra 3.2, `Registry.hpp:1503`) constructs that Registry via
`std::make_unique<Registry>(componentRegistry)` — the `Registry(shared_ptr<Component
Registry>, Config{})` ctor with a **default `Config`**. The default `Config` has a
**null `workScheduler`**. So the restored Registry silently loses its work scheduler,
and every registry-level `Parallel*` call (`View::ParallelForEach`,
`Relations::ParallelForEachDescendant`) **silently degrades to sequential after the
first hot reload** — a latent, non-crashing correctness trap directly on the documented
hot-reload path Astra itself ships (`Astra/CLAUDE.md`, "Hot-reload").

There is no consumer-side workaround: `m_workScheduler` is private with no setter, and
`Load` is monolithic. The fix is one additive, backward-compatible overload, released
the M4 Phase-0 way (dev → github main → cut `3.3` → re-vendor, bump `Version.hpp`):

```cpp
// New overloads thread a Config (hence workScheduler) into the restored Registry.
static Result<std::unique_ptr<Registry>, SerializationError>
    Load(const std::filesystem::path& path, std::shared_ptr<ComponentRegistry> creg, const Config& config);
static Result<std::unique_ptr<Registry>, SerializationError>
    Load(std::span<const std::byte> data, std::shared_ptr<ComponentRegistry> creg, const Config& config);
```

`LoadInternal` gains a `const Config&` parameter and constructs the registry as
`make_unique<Registry>(creg, config)` (the existing two-arg `Load`s delegate with a
default `Config`, preserving behavior). This is the entire Astra change for M5. If, in
the course of execution, a *second* genuine Astra gap surfaces, it folds into the same
3.3 release; otherwise Astra ships at 3.3 with exactly this overload + its test, and is
left there.

## Goals

1. **Astra 3.3:** `Registry::Load` accepts a `Config` so a restored registry keeps its
   work scheduler (the only Astra change). Re-vendor at 3.3.
2. **`Arcane::Runtime` facade** (Arcane.dll, `ARCANE_API`): owns the engine-side
   `TypeContext` (created + installed in the engine module), a persistent
   `shared_ptr<ComponentRegistry>`, a swappable `unique_ptr<Registry>`, the
   `SystemSchedulers`, a `RunLoop`, and the `JobSystem`. Exposes the accessors the host
   and plugin need, plus `SnapshotRegistry()` / `RestoreRegistry()` / `ClearSystems()` /
   `SetRenderContext()`. This is the object `EngineContext::engine` points at.
3. **Plugin ABI** (`Arcane/Plugin/PluginABI.hpp`, shared by engine + game): the seven
   `extern "C"` entry points, `EngineContext`, the ABI version constant, and the
   host-side function-pointer table type. The only unmangled surface in the engine.
4. **`PluginHost`** (Arcane.dll Plugin module, `ARCANE_API`): debounced mtime watch on
   the game DLL build output; versioned DLL/PDB copy-and-load (`PlaygroundGame_0042.dll`)
   to dodge the MSVC PDB lock so msbuild is never blocked; ABI-version check;
   **last-good rollback** on any failure (one error logged, never a lost session). Manual
   controls: force-reload (with state restore) and reload-fresh (without).
5. **`PlaygroundGame.dll`** (new `PlaygroundGame/` project): the M4 scene content moved
   behind the plugin ABI — the first live ABI + hot-reload consumer. `Init` does the
   contractual `SetTypeContext` → inject scheduler → `ReRegisterComponent<T>` dance,
   builds the parent/child sprite scene, registers the engine scene systems into the
   engine schedulers, and caches the orbiter. `FixedUpdate` orbits the parent.
   `SaveState`/`LoadState` wrap the whole-registry snapshot.
6. **`Loom.exe`** (new `Loom/` project): the thin host — engine boot + render plumbing
   (the M4 host loop, scene content removed) + `RunLoop` interleaving plugin callbacks
   with the engine schedulers + the `PluginHost`. `--frames N`, `--backend`, `--no-vsync`,
   `--plugin <path>` for the dev reload loop.
7. **Scripted hot-reload test** (per the engine-arch Testing section): build variant B
   of a tiny test plugin, swap it in over variant A while state is non-default, assert
   the ECS state survived AND the new code path is live. Plus a headless rollback test
   (bad DLL → session survives on last-good) and a `[gpu]` Loom `--frames` live proof
   (`RenderErrorCount() == 0` through the real host + plugin + render path, both backends).

**Design rule (carried from M4, non-negotiable):** every engine/game component type is
`ASTRA_REFLECT`-annotated. M5 adds: **every module that touches a `TypeID`/`Registry`
installs the shared `TypeContext` first** (engine module at Runtime construction; game
module as the first line of `GamePlugin_Init`).

## Non-goals (deferred; captured in Roadmap)

- Physics port / oracle parity (M6) — Loom's "engine pre-step" stays empty.
- Grimoire editor, sim-time control (pause/step/time-scale), LevelEditor (M7). Loom and
  Grimoire are specified to share `RunLoop`; M5 ships Loom only.
- Multi-plugin hosting, plugin dependency graphs, plugin-provided render passes — one
  `Game.dll` per host for M5.
- Engine-owned render submission end-to-end (needs the render graph; renderer-spec
  later milestone). Render plumbing stays host-owned, exactly as M4 left it.
- Aphelyon `Game.dll` (the real target — the **Aphelyon client itself**, ported onto
  Arcane: the engine's "game" plugin IS the client) — M5 ships `PlaygroundGame.dll` as
  the demo consumer; the reserved `Game/` slot stays empty until the client port lands.
- Reflection→JSON in the hot-reload path — restore is binary (`Registry::Save/Load`).
  The M4 JSON bridge is unrelated to M5 and untouched.
- Linux: `dlopen`/`.so` parity and the gmake2 plugin path (the engine-arch Linux port
  is a later milestone; M5 is Windows-first, with the platform seam isolated, below).

---

## Phase 0 — Astra 3.3 (`Registry::Load` keeps the work scheduler)

Worked in `D:\dev\starworks\Astra` on branch `dev`; released via `D:\dev\github\Astra`
(merge `dev` → `main`, cut `3.3`); re-vendored into `ThirdParty/Astra`. Header-only,
C++20, GoogleTest. **Hard constraints:** purely additive (the two-arg `Load` overloads
keep identical behavior by delegating with a default `Config`); binary format byte-for-
byte unchanged; no new dependency. Encoding: UTF-8 without BOM, ASCII in comments.

### 0.1 The change (`include/Astra/Registry/Registry.hpp`)

- Add the two `Load(..., const Config&)` overloads shown above.
- `LoadInternal` takes `const Config& config` and constructs
  `auto registry = std::make_unique<Registry>(componentRegistry, config);` instead of
  the one-arg form. Every subsequent line (entity-manager move, archetype deserialize,
  relationship-graph deserialize, checksum verify) is unchanged.
- The existing two-arg `Load(path, creg)` / `Load(data, creg)` delegate:
  `return LoadInternal(reader, std::move(creg), Config{});`.

### 0.2 AstraTest coverage (`tests/Registry/RegistrySerializationTest.cpp`, GoogleTest)

- **Scheduler survives Load:** build a Registry with a `Config.workScheduler` set to a
  `Testing::TestWorkerPool`; `Save()` to bytes; `Load(bytes, creg, cfg)`; assert the
  restored registry runs a `View::ParallelForEach` across more than one worker
  (observe `WorkerCount() > 1` via the pool, or a multi-thread-id set captured in the
  callback). Then `Load(bytes, creg)` (two-arg) and assert it runs sequential
  (workScheduler null) — proving the default path is unchanged.
- **Round-trip parity:** existing serialization suites stay green (binary unchanged).

### 0.3 Phase 0 workflow (release + re-vendor)

Identical to the M4 Astra 3.2 procedure:
1. dev + test on `D:\dev\starworks\Astra` branch `dev`; `AstraTest` green (validate in
   **Release** — Debug full-suite aborts on pre-existing `RelationshipGraph` asserts,
   per the M4 gotcha); push `dev`.
2. In `D:\dev\github\Astra`: merge `dev` → `main`, cut branch `3.3`, push both.
3. Bump `include/Astra/Core/Version.hpp` to `ASTRA_VERSION_MINOR 3`.
4. Re-vendor the headers into `ThirdParty/Astra/include/Astra/` (plain copy);
   verify `Version.hpp` reports 3.3 and the new `Load` overloads are present.

Phase 0 is DONE when `ThirdParty/Astra` is at 3.3 with the overload and `AstraTest`
green (Release).

---

## Phase 1 — `Arcane::Runtime` facade + plugin ABI header

### 1.1 `Arcane/Plugin/PluginABI.hpp` (shared by engine + game; the ONLY C surface)

```cpp
#pragma once
#include <Arcane/Base/Api.hpp>
#include <cstdint>

namespace Astra { class TypeContext; class IWorkScheduler; class BinaryWriter; class BinaryReader; }

namespace Arcane
{
    class Runtime;  // engine facade (defined in Arcane.dll); the plugin holds it opaquely

    // Bump on ANY change to EngineContext layout or the entry-point set/signatures.
    inline constexpr uint32_t kGamePluginABIVersion = 1;

    struct EngineContext
    {
        uint32_t               abiVersion;     // == kGamePluginABIVersion at the host
        Astra::TypeContext*    typeContext;    // plugin calls Astra::SetTypeContext(this) FIRST
        Astra::IWorkScheduler* workScheduler;  // the one engine enkiTS adapter (shared instance)
        Arcane::Runtime*       engine;         // registry, schedulers, snapshot/restore, render ctx
    };

    // C entry points every game DLL exports. Names the host resolves by GetProcAddress.
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

    // Host-side resolved function-pointer table (one loaded plugin instance).
    struct PluginVTable
    {
        uint32_t (*ABIVersion)();
        bool     (*Init)(EngineContext*);
        void     (*Shutdown)();
        void     (*FixedUpdate)(double dt);
        void     (*Update)(double dt, double alpha);
        void     (*SaveState)(Astra::BinaryWriter&);
        bool     (*LoadState)(Astra::BinaryReader&);
    };
}
```

The game declares the matching `extern "C"` functions (export macro game-local, §3.1).
The host never links the game's import lib — it `LoadLibrary` + `GetProcAddress` the
seven names into a `PluginVTable`.

### 1.2 `Arcane/Base/Runtime.hpp` / `.cpp` (`ARCANE_API`)

```cpp
namespace Arcane
{
    // The engine facade handed to plugins via EngineContext. Owns the substrate that
    // must OUTLIVE plugin reloads: the shared TypeContext (installed in THIS module),
    // the persistent ComponentRegistry, the (swappable) Registry, the per-phase
    // schedulers, the RunLoop, and the JobSystem (the one enkiTS scheduler).
    class ARCANE_API Runtime
    {
    public:
        // externalContext == null: Runtime creates+owns a TypeContext (production: Loom).
        // externalContext != null: Runtime installs+uses the caller's context, so multiple
        // modules (e.g. a test exe + Arcane.dll + the plugin) share ONE identity space.
        explicit Runtime(Astra::TypeContext* externalContext = nullptr);
        ~Runtime();

        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        // --- substrate the plugin registers into / the host drives ---
        Astra::Registry&        Registry()        noexcept;   // current (post-swap) registry
        SystemSchedulers&       Schedulers()       noexcept;
        RunLoop&                Loop()             noexcept;
        Astra::TypeContext*     TypeContext()      noexcept;   // handed to the plugin
        Astra::IWorkScheduler*  WorkScheduler()    noexcept;   // handed to the plugin
        std::shared_ptr<Astra::ComponentRegistry> Components() noexcept; // persistent; ReRegister + Load target

        // --- render bridge (host sets the live batcher each frame, IN this module) ---
        void SetRenderContext(class Batcher2D* batcher, glm::vec2 cameraOffset);

        // --- hot-reload support (called by the plugin Save/LoadState + the host) ---
        std::vector<std::byte> SnapshotRegistry() const;        // Registry::Save() -> CRC'd versioned bytes
        bool RestoreRegistry(std::span<const std::byte> bytes); // Registry::Load(.., Config{sched}); swap; rebind RunLoop
        void ResetRegistry();                                   // swap in a fresh empty registry (fresh-boot reload)
        void ClearSystems();                                    // Clear() all three phase schedulers (drop stale plugin functors)

    private:
        std::unique_ptr<struct Impl> m_impl;  // JobSystem + TypeContext + ComponentRegistry + Registry + Schedulers + RunLoop
    };
}
```

**Construction order (the contract that makes cross-module IDs agree):**
1. Create the `JobSystem` → obtain the shared `IWorkScheduler`.
2. Create the engine-owned `Astra::TypeContext` and call `Astra::SetTypeContext(&ctx)`
   **inside Arcane.dll** (this sets *this module's* per-module context slot — the host
   exe and the game DLL each install it in their own module separately). This MUST
   precede any `TypeID`/`Registry` use in the engine module.
3. Create the persistent `shared_ptr<ComponentRegistry>`.
4. Build the `Registry` sharing that ComponentRegistry, with `Config.workScheduler` set.
5. Build `SystemSchedulers(workScheduler)` and `RunLoop(Registry(), Schedulers())`.

**`RestoreRegistry`** uses the Phase-0 overload: `Registry::Load(bytes, Components(),
Config{ .workScheduler = sched })` → new `unique_ptr<Registry>`; on success it swaps the
old registry out, rebinds the `RunLoop` to the new registry (RunLoop holds a `Registry*`),
and returns true. On a corrupt/checksum failure it returns false WITHOUT swapping (the
caller keeps the live registry — the rollback path needs the old one intact).

**`SetRenderContext`** runs `Registry().SetResource<RenderContext2D>({batcher, offset})`
*inside Arcane.dll*, so `TypeID<RenderContext2D>` resolves in the engine module against
the shared context — the host (Loom) never instantiates a scene `TypeID` itself and so
needs no `SetTypeContext` call. The game's `RenderSubmissionSystem` reads the same
resource (game module, same context). (RenderContext2D holds a transient `Batcher2D*`;
it is re-set every frame, so a stale pointer captured in a snapshot is overwritten before
the next `SubmitRender` — harmless.)

### 1.3 Test (`Tests/src/RuntimeTest.cpp`, headless, Catch2)

- **Boot + substrate:** construct a `Runtime`; assert `WorkScheduler()->WorkerCount() >= 1`,
  `TypeContext() != nullptr`; register a reflected component into `Components()`, create
  entities, run a trivial `fixedUpdate` system via `Loop().Advance(...)`, assert it ran.
- **Snapshot/restore preserves state AND the scheduler:** register `Counter`, create N
  entities with distinct values, `SnapshotRegistry()`; mutate the live registry; then
  `RestoreRegistry(snapshot)`; assert the entities/values match the snapshot AND a
  `View<Counter>().ParallelForEach` on the restored registry visits each exactly once
  (proves the Phase-0 scheduler-survival end-to-end through Runtime).
- **`ClearSystems`:** add a system to each phase, `ClearSystems()`, assert all three
  schedulers are empty (`IsEmpty()`).

---

## Phase 2 — `PluginHost`: watch, versioned load, ABI check, rollback

`Arcane/Plugin/PluginHost.hpp` / `.cpp` (`ARCANE_API`). Windows-first; the OS calls
(`LoadLibraryW`/`GetProcAddress`/`FreeLibrary`) are isolated behind a 3-function internal
seam (`Plugin/DynamicLibrary.hpp`) so the Linux `dlopen` port (later milestone) is a
single-file swap.

### 2.1 Responsibilities

```cpp
namespace Arcane
{
    class ARCANE_API PluginHost
    {
    public:
        PluginHost(Runtime& runtime, std::filesystem::path sourceDllPath);
        ~PluginHost();                          // Unload()

        bool Load();                            // initial load: copy+load+ABI+Init  (NO state restore)
        void Unload();                          // Shutdown + FreeLibrary + delete versioned copy
        bool Reload(bool restoreState);         // the full sequence (below); rollback on any failure
        void Poll();                            // debounced mtime check; triggers Reload(true) on change

        bool ForceReload() { return Reload(true);  }  // manual hotkey: reload WITH state restore
        bool ReloadFresh() { return Reload(false); }  // manual hotkey: reload WITHOUT restore (fresh boot)

        bool        IsLoaded() const noexcept;
        const PluginVTable* Vtable() const noexcept;  // null until loaded; the host calls FixedUpdate/Update through it
        uint32_t    Generation() const noexcept;      // versioned-copy counter (for the versioned name + logs)

    private:
        // current + last-good loaded images, the host-owned snapshot buffer, debounce state
    };
}
```

### 2.2 The hot-reload sequence (matches engine-arch §"Hot-reload sequence", with the
whole-registry snapshot deviation)

`Reload(restoreState)`:
1. If `restoreState` and a plugin is loaded: build an `Astra::BinaryWriter` over a
   host-owned `std::vector<std::byte>` and call `vtable.SaveState(writer)` → the snapshot
   (the plugin writes `runtime.SnapshotRegistry()` blob; §3). Keep the bytes.
2. `vtable.Shutdown()`; `runtime.ClearSystems()` (drop stale system functors that point
   into the module about to be unloaded); `FreeLibrary` the current image.
3. Copy the source `PlaygroundGame.dll` + `.pdb` to versioned names in a host-owned temp
   dir (`PlaygroundGame_<gen>.dll/.pdb`, `gen` increments) and `LoadLibraryW` the **copy**.
   The copy's purpose is the **lock dodge**: the source DLL/PDB are never opened by the
   host, so msbuild can rewrite them while a session runs. The PDB travels alongside so
   the debugger keeps working off the versioned image; exact CodeView symbol-path patching
   under the versioned name is best-effort and **not required** for the reload to succeed
   (deferred — the lock dodge is the load-bearing part).
4. Resolve the seven entry points → `PluginVTable`. Any missing symbol → failure.
5. `vtable.ABIVersion() == kGamePluginABIVersion`? Mismatch → failure.
6. `vtable.Init(&ctx)` (the engine fills `EngineContext`). `false` → failure.
7. If `restoreState`: `Astra::BinaryReader` over the saved bytes → `vtable.LoadState(reader)`.
   `false` → failure.
8. On success: promote the new image to "last-good"; delete the previous versioned copy
   (best-effort; a still-mapped/locked file is retried on the next reload). Log swap time
   + snapshot size (Tracy zone + `ARC_INFO`).

**Failure at any step 3–7 = rollback, never a lost session:** restore the previous
last-good versioned image (`LoadLibrary` it again — it was never deleted on failure),
`Init` it, and `LoadState` the SAME snapshot buffer. Surface one error (`ARC_ERROR` +
a host error-overlay flag the host can render). If even rollback fails (should not
happen — it loaded moments ago), the host stays on the now-unloaded state and logs a
fatal; the process is not torn down by the PluginHost.

**Fresh reload (`ReloadFresh`, `restoreState=false`):** skips the snapshot (step 1) and,
after teardown, calls `Runtime::ResetRegistry()` to swap in an empty registry before
`Init`, so the plugin's `Init` rebuilds its scene from scratch (its `if (!SceneRoot)`
guard would otherwise see the old scene and skip). This is the "fresh boot for init-logic
changes" chord.

### 2.3 The watcher

`Poll()` is called once per frame by the host. It stats the source DLL's
`last_write_time`; on change it starts a debounce timer (default 250 ms) and only fires
`Reload(true)` once the timestamp has been stable for the debounce window **and** the
file opens cleanly for read (an exclusive-open probe — msbuild still writing → skip this
frame, retry next). No background thread: polling on the main thread keeps the swap on
the render/sim thread, which is where `LoadState`/`ClearSystems`/RunLoop rebind must
happen anyway (no cross-thread registry mutation).

### 2.4 Test (`Tests/src/PluginHostRollbackTest.cpp`, headless, Catch2)

Uses the dedicated test plugins from §4.3 (no GPU). Asserts:
- **Rollback on ABI mismatch:** load `HotReloadPluginV1`; advance to a non-default state;
  point the host at a deliberately bad image (a plugin built with a mismatched
  `kGamePluginABIVersion`, or a non-plugin DLL); `Reload(true)` returns false, the live
  vtable is still V1, and the V1 ECS state is intact (the session survived).
- **Rollback on corrupt LoadState:** a plugin whose `LoadState` returns false →
  `Reload(true)` false, state intact.

---

## Phase 3 — `PlaygroundGame.dll`: the first live plugin

New project `PlaygroundGame/` → `PlaygroundGame.dll` (SharedLib, /MD). It includes the
engine's header-only `Scene/` module (the M4 components/systems live in Arcane.dll's
source tree but are header-only and instantiate in the **game** module) and links the
Arcane import lib + Core. The reserved `Game/` slot is left empty: the engine's eventual
`Game.dll` is the **Aphelyon client** ported onto Arcane, not this demo plugin.

### 3.1 `PlaygroundGame/src/GameApi.hpp`

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

### 3.2 `PlaygroundGame/src/PlaygroundGame.cpp`

A single TU implementing the seven entry points. It holds a file-static `EngineContext*`
and cached `Astra::Entity` handles (root/orbiter/moon) — *not* in static initializers
(the static-init `TypeID` prohibition); they are assigned in `Init`.

```cpp
extern "C" {

GAME_API uint32_t GamePlugin_ABIVersion() { return Arcane::kGamePluginABIVersion; }

GAME_API bool GamePlugin_Init(Arcane::EngineContext* ctx)
{
    // CONTRACTUAL FIRST LINES (game module):
    Astra::SetTypeContext(ctx->typeContext);        // 1. install the shared context in THIS module
    g_ctx = ctx;
    auto& reg = ctx->engine->Registry();
    // 2. inject the scheduler into the registry config: already done engine-side; the
    //    game re-registers component types so descriptors point into THIS (current) module:
    auto creg = ctx->engine->Components();
    creg->ReRegisterComponent<Arcane::LocalTransform>();   // 3. ReRegisterComponent per game component
    creg->ReRegisterComponent<Arcane::WorldTransform>();
    creg->ReRegisterComponent<Arcane::SpriteRenderer>();

    // Register engine scene systems into the engine schedulers (functors live in THIS module;
    // the host ClearSystems() before every reload drops the prior module's stale functors).
    auto& sch = ctx->engine->Schedulers();
    sch.fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>();
    sch.render.AddSystem<Arcane::RenderSubmissionSystem>();

    // Build the scene ONLY on a fresh boot. On a state-restoring reload, LoadState
    // repopulates the registry; Init must not double-create. Distinguish via a flag the
    // host sets, OR (chosen) check whether the SceneRoot resource already exists.
    if (!reg.GetResource<Arcane::SceneRoot>())
        BuildScene(reg);          // root + orbiter(parent) + moon(child); SetResource<SceneRoot>
    CacheHandles(reg);            // re-acquire root/orbiter/moon from SceneRoot (ID-stable across Load)
    return true;
}

GAME_API void GamePlugin_Shutdown() { /* drop cached handles; systems are cleared by the host */ }

GAME_API void GamePlugin_FixedUpdate(double dt)
{
    // Gameplay tick: orbit the parent's LocalTransform. (Engine systems -- propagation --
    // run AFTER this each fixed step, driven by the host RunLoop.)
    g_sceneTime += dt;
    if (auto* lt = g_ctx->engine->Registry().GetComponent<Arcane::LocalTransform>(g_orbiter))
        lt->rotation = (float)g_sceneTime;
}

GAME_API void GamePlugin_Update(double /*dt*/, double /*alpha*/) { /* slice: nothing */ }

GAME_API void GamePlugin_SaveState(Astra::BinaryWriter& w)
{
    // Whole-registry snapshot (engine-arch spec deviation): one integrated CRC'd
    // versioned stream, NOT per-component writes. The plugin owns only the framing.
    // Astra archives serialize PODs via operator() and raw bytes via WriteBytes.
    const std::vector<std::byte> blob = g_ctx->engine->SnapshotRegistry();
    w(static_cast<uint64_t>(blob.size()));
    w.WriteBytes(blob.data(), blob.size());
}

GAME_API bool GamePlugin_LoadState(Astra::BinaryReader& r)
{
    uint64_t n = 0; r(n);
    std::vector<std::byte> blob(n);
    r.ReadBytes(blob.data(), n);
    if (r.HasError()) return false;
    if (!g_ctx->engine->RestoreRegistry(blob)) return false;  // Load(Config{sched}) + swap + rebind RunLoop
    CacheHandles(g_ctx->engine->Registry());                  // re-acquire handles from the restored SceneRoot
    return true;
}

} // extern "C"
```

(`Astra::BinaryWriter`/`BinaryReader` expose `operator()(POD&)` for scalars and public
`WriteBytes`/`ReadBytes(void*, size_t)` for raw blobs; the reader latches an error state
read via `HasError()`. The contract: one whole-registry blob, length-framed by the plugin.)

### 3.3 `Loom`/`Playground` coexistence with the scene module

The M4 `Playground.exe` keeps its inline scene (unchanged) so the standalone path stays
alive ("keep standalone Playground.exe working too if cheap"). `PlaygroundGame.dll`'s
`BuildScene` is the same construction lifted out of `Playground/main.cpp` (root, orbiter
at canvas centre, moon parented to orbiter, all `textureId == 0`). The shared scene
*components/systems* are the single source in `Arcane/src/Arcane/Scene/`; both consumers
include them.

### 3.4 Test (`Tests/src/PlaygroundGamePluginTest.cpp`, headless, Catch2)

Load `PlaygroundGame.dll` through `PluginHost` into a headless `Runtime` (no window/
device): `Load()`; drive `Loop().Advance(dt, [&]{vtable.FixedUpdate(fixedDt);}, ...)` for
K fixed steps; assert the moon's `WorldTransform` translation changed (orbit + propagation
ran across the ABI). `ForceReload()`; assert the orbiter's `rotation`/position survived
the real `FreeLibrary`/`LoadLibrary` cycle (snapshot round-trip), and `RenderErrorCount()
== 0` (no GPU touched, so trivially 0 — the assertion is the foundation rule's habit).

---

## Phase 4 — `Loom.exe`: the thin host + live integration + scripted hot-reload test

### 4.1 `Loom/src/main.cpp` (~the M4 Playground loop, scene content removed)

Boot order mirrors `Playground/main.cpp`: parse args (`--backend`, `--frames N`,
`--no-vsync`, `--plugin <path>`); `Window` → `RenderDevice` → `Swapchain` → `ShaderLibrary`
→ `Canvas` → `Batcher2D` → `TonemapPass` → `TextSystem` (HUD) → `ImGuiLayer` →
`InputDevices`/`InputActions`. Then:

```cpp
Arcane::Runtime runtime;                                   // JobSystem + TypeContext + Registry + schedulers + RunLoop
Arcane::PluginHost plugin(runtime, pluginPath);            // default "./PlaygroundGame.dll"
if (!plugin.Load()) return 1;                              // copy+load+ABI+Init (builds the scene)

while (running) {
    auto events = window.PumpEvents();  ...resize/minimize as M4...
    input->Update(frameDt, devices->Sample(...));
    if (input->Pressed("quit")) break;
    if (input->Pressed("reload_plugin"))       plugin.ForceReload();   // hotkey: reload WITH state
    if (input->Pressed("reload_plugin_fresh")) plugin.ReloadFresh();   // hotkey: reload WITHOUT state

    const auto* vt = plugin.Vtable();
    // RunLoop interleaves plugin callbacks with engine schedulers (see 4.2):
    double alpha = runtime.Loop().Advance(
        simDt,
        [&](double fdt){ vt->FixedUpdate(fdt); },          // gameplay tick, BEFORE engine fixedUpdate scheduler
        [&](double dt,double a){ vt->Update(dt,a); });     // once per frame

    // --- render (host-owned; M4 plumbing) ---
    swapchain.BeginFrame(); commandList.open(); clear canvas;
    batcher.Begin(commandList, canvas.Framebuffer(), w, h);
    ... optional HUD text ...
    runtime.SetRenderContext(batcher.get(), {0,0});        // sets RenderContext2D resource IN Arcane.dll
    runtime.Loop().SubmitRender();                          // RenderSubmissionSystem (game module) -> batcher
    batcher.End(); tonemap.Run(...); imgui.Render(...); commandList.close(); execute; Present();

    plugin.Poll();                                          // debounced watcher -> Reload(true) on rebuild
    if (--frames hits 0) running = false;
}
runtime.Loop()... ; device->waitForIdle();
```

`Loom` adds `reload_plugin` / `reload_plugin_fresh` actions to its
`data/input_actions.json` (copied post-build).

### 4.2 `RunLoop` interleaving overload (`Arcane/Sim/RunLoop.hpp`)

The M4 `RunLoop::Advance(realDt)` runs only the engine schedulers. M5 adds an overload so
the host stays thin and the accumulator stays single-owned:

```cpp
// Host-driven: pluginFixed(fixedDt) runs each fixed step BEFORE the engine fixedUpdate
// scheduler (gameplay moves transforms; propagation reads them after). pluginUpdate(dt,
// alpha) runs once after the Update scheduler. Same clamp/spiral-of-death guard as Advance.
double Advance(double realDt,
               const std::function<void(double)>& pluginFixed,
               const std::function<void(double,double)>& pluginUpdate);
```

Order per fixed step: `pluginFixed(fixedDt)` → `schedulers.fixedUpdate.Execute(...)`. The
existing no-callback `Advance` is retained (Playground.exe, tests). The engine-arch host
loop's "Arcane pre-step (physics)" slot is empty for M5; the engine's fixedUpdate
scheduler (transform propagation) is the post-plugin step.

### 4.3 The scripted hot-reload test (engine-arch Testing section)

Two tiny test-plugin projects share one source
(`Tests/plugins/HotReloadPlugin.cpp`) compiled with a `HOTRELOAD_VARIANT` define into
`HotReloadPluginV1.dll` and `HotReloadPluginV2.dll`. The plugin owns one reflected
component (`struct Pulse { int ticks; int variant; }`); `FixedUpdate` does
`ticks += VARIANT` (V1 increments by 1, V2 by 10 — observably different *code* in a
recompiled binary); `SaveState`/`LoadState` wrap the whole-registry snapshot like
PlaygroundGame; `Init` stamps `variant = HOTRELOAD_VARIANT` on a singleton and creates
the `Pulse` entity on fresh boot only.

`Tests/src/HotReloadSwapTest.cpp` (headless, Catch2, `[hotreload]`):
1. `PluginHost` loads `HotReloadPluginV1.dll` (copied to the watched path) into a headless
   `Runtime`. Advance K fixed steps → `ticks == K` (V1 step = 1), `variant == 1`.
2. Copy `HotReloadPluginV2.dll` over the watched path; `plugin.ForceReload()` (or `Poll()`
   after touching mtime). Assert: `Reload` succeeded; `ticks == K` **survived the swap**
   (state round-tripped through `SaveState`→`FreeLibrary`→`LoadLibrary`→`LoadState`); and
   advancing one more fixed step now adds **10** (`ticks == K + 10`) — **the new code is
   live**. `RenderErrorCount() == 0`.

This is the literal engine-arch deliverable: *build variant B, swap, assert state
survived* — plus the stronger assertion that B's code actually runs.

### 4.4 `[gpu]` live integration (`Loom --frames`, both backends)

The Loom `--frames N` scripted run is the living integration test: a real host + real
`PlaygroundGame.dll` + real render path. CI (`windows-1`) runs
`Loom.exe --backend dx12 --frames 180` and `--backend vulkan --frames 180`; both must
exit 0. A `[gpu]` Catch2 case (`Tests/src/LoomSliceTest.cpp`) optionally drives the same
path offscreen and asserts `Batcher2D::Stats().quads >= 2` (orbiter + moon submitted
through the plugin's registered `RenderSubmissionSystem`) and `RenderErrorCount() == 0`.

---

## Binding cross-module contracts (carry into the plan; review-enforced)

1. **One `TypeContext`, installed per module.** The engine owns it (created in
   `Runtime`, `SetTypeContext` called inside Arcane.dll before the Registry is built).
   The game installs the SAME context as the first line of `GamePlugin_Init`. The host
   exe (`Loom`) never instantiates a scene `TypeID` directly — every scene-typed
   `SetResource`/`GetComponent` it appears to need is routed through an Arcane.dll method
   (`Runtime::SetRenderContext`), so Loom needs no `SetTypeContext`. This is the
   formalization of M4's single-module-ownership rule: two modules now touch one
   Registry, kept consistent by the shared context.
2. **`ReRegisterComponent<T>` per game component, every `Init`.** Component *data* lives
   in the engine-owned Registry and survives reload; the *descriptors* (serialize /
   visitFields / type-erased ctors) point into whichever module last registered them, so
   the game rebuilds them into the freshly loaded module each `Init`. Never call
   `TypeID<T>::Value()` from a game static initializer (Astra contract).
3. **Stale system functors → `ClearSystems()` before every reload.** Systems registered
   via `AddSystem<T>()`/lambda capture code in the plugin module; after `FreeLibrary`
   they dangle. The host clears all three phase schedulers between `Shutdown` and the next
   `Init`; the plugin re-registers them in `Init`.
4. **Whole-registry snapshot, not per-component.** `SaveState`/`LoadState` frame exactly
   one `Registry::Save()` blob (CRC'd, versioned, LZ4). This is the recorded engine-arch
   deviation; it is what makes reload robust to component-layout changes without
   detection logic. Restore is `Registry::Load(blob, Components(), Config{sched})` (the
   Phase-0 overload) → swap → rebind RunLoop.
5. **Transient resources are re-set, not relied upon post-restore.** `RenderContext2D`
   carries a live `Batcher2D*`; the host re-sets it every frame via
   `Runtime::SetRenderContext` before `SubmitRender`, so the stale pointer a snapshot
   captured is overwritten and never dereferenced.
6. **Same-toolchain, /MD across the boundary.** Host (Loom) + Arcane.dll + the game DLL
   are always built together from the Arcane workspace, all /MD, so allocations and
   `std::function`/`shared_ptr` cross the boundary on one heap. The game links the Arcane
   import lib and uses the C++ API directly; only the seven `extern "C"` entry points are
   unmangled.
8. **Test harness shares one `TypeContext` process-wide.** `TypeID<T>::Value()` caches
   per-module on first touch, so a test exe that reads scene/game components in its own
   module must agree with the engine + plugin. `ArcaneTests` installs ONE shared context in
   `main()` (before any test runs, so the first component-TypeID touch resolves under it),
   and every `Runtime` a test constructs is given that same context (`Runtime(&shared)`).
   M4 tests that use bare registries stay correct (each is internally self-consistent under
   whatever context is installed). Production hosts (Loom) pass no context — `Runtime` owns
   one — and never touch a component `TypeID` themselves, so they need no install.
9. **Build order + plugin placement.** `Arcane` → `PlaygroundGame` → `Loom`. `Loom`
   `dependson { "PlaygroundGame" }` and post-build-copies `PlaygroundGame.dll(+.pdb)`
   beside `Loom.exe` (the default watched path). The dev live-reload loop rebuilds
   `PlaygroundGame` in VS; Loom's versioned-copy load never locks the source, so msbuild
   succeeds and the watcher fires. `--plugin <path>` points Loom at the build-output DLL
   for in-place reloads without a Loom rebuild.

---

## Testing strategy (summary)

| Test | Project | Tag | Asserts |
|---|---|---|---|
| `RegistrySerializationTest` (scheduler-survives-Load) | AstraTest | — | restored registry keeps workScheduler (Config overload); two-arg Load still sequential |
| binary regression | AstraTest | — | existing Serialization/Registry suites stay green |
| `RuntimeTest` | ArcaneTests | — | boot substrate; snapshot/restore preserves state + scheduler; ClearSystems empties phases |
| `PluginHostRollbackTest` | ArcaneTests | `[hotreload]` | ABI-mismatch + corrupt-LoadState → rollback, session + state intact |
| `PlaygroundGamePluginTest` | ArcaneTests | `[hotreload]` | scene runs across the ABI; ForceReload preserves state |
| `HotReloadSwapTest` | ArcaneTests | `[hotreload]` | V1→V2 swap: state survived AND new code live (`+10`) |
| `LoomSliceTest` | ArcaneTests | `[gpu]` | quads>=2 via plugin-registered render system; `RenderErrorCount()==0` |
| `Loom --frames N` | Loom | — | real host+plugin+render renders N frames, exits 0, both backends |

Build verification each phase: `msbuild Arcane.slnx -p:Configuration=Debug -m` and
`Release` (both backends where relevant); run `ArcaneTests.exe` **from its output dir**
(the `Arcane.dll`, `PlaygroundGame.dll`, test plugins, `shaders/`, and font are copied
there post-build; `MsdfgenSmokeTest` + the plugin load paths are relative).

## Build-env gotchas (carried from M4 + new)

- **Run `ArcaneTests.exe` from its output dir** (relative font + plugin paths).
- **AstraTest validates in Release** (Debug full-suite aborts on pre-existing
  `RelationshipGraph` asserts — not an M5 issue).
- **Regenerate by running `../ThirdParty/premake5/premake5.exe vs2026` directly**
  (`GenerateProjects.bat` hangs on a `pause`); Astra via `premake5 vs2022` directly.
- **MSBuild full path, DASH flags under Git Bash**
  (`-p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo`); MSBuild at
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`.
- **New:** the host loads plugins by versioned copy — never delete/lock the source DLL;
  the watcher must probe for an exclusive open before reloading (msbuild may still be
  writing). Build order is `Arcane → PlaygroundGame → Loom`; a stale `PlaygroundGame.dll`
  beside `Loom.exe` is the usual cause of "old behavior after edit" — confirm Loom's
  post-build copy ran or pass `--plugin`.
- **/MD everywhere; no `/fp:fast`; UTF-8 without BOM; ASCII comments; C++23 (Arcane),
  C++20 (Astra).** Never `db-reset` / `clean --deep` / `docker compose down -v`; never
  `tauri dev`. CI self-provisions libpq (M4 commit `fedd338`) — no action needed.
  Work on a new branch off `main`. Commit trailer:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## Roadmap notes (capture, do NOT build in M5)

- **M6 = physics port** (oracle-gated, hash parity with the Lua reference) into a
  presentation-free sim layer; Loom's empty "engine pre-step" gains the physics world;
  Playground/PlaygroundGame swap toy motion for the real engine.
- **M7 = Grimoire shell + sim-time control** (pause / single-step / time-scale wrapping
  the same `RunLoop`) → play-in-editor → LevelEditor. Grimoire and Loom share `RunLoop`;
  M5's interleaving overload + `Runtime` facade are what Grimoire wraps.
- **Aphelyon client port** — the engine's eventual `Game.dll` (the reserved `Game/` slot)
  IS the Aphelyon client running on Arcane; it lands on top of the proven ABI (Combat
  Sphere builds inside it).
- **Linux plugin path** (`dlopen`/`.so`, gmake2 game/host targets) is isolated behind
  `Plugin/DynamicLibrary.hpp` so it is a single-file port in the Linux milestone.
- **Multi-plugin / plugin render passes / async PSO-aware reload** are future; M5 is one
  plugin, host-owned render.

## Constraints (carried forward)

- Every engine/game component `ASTRA_REFLECT`-annotated from day one.
- Each module installs the shared `TypeContext` before its first `TypeID`/`Registry`
  use; no `TypeID<T>::Value()` from static initializers in any plugin module.
- `/MD` across the whole Arcane workspace; no `/fp:fast` (determinism); UTF-8 without
  BOM; ASCII in comments. C++23 in Arcane; Astra stays C++20-compatible.
- Full rebuild after header changes when results look impossible (stale-object ODR
  lesson).

## Out of scope (deferred to their own designs)

Physics port (M6); Grimoire/LevelEditor + sim-time control (M7); Aphelyon `Game.dll`;
multi-plugin hosting and plugin-provided render passes; engine-owned end-to-end render
submission (needs the render graph); Linux `.so` plugin parity; reflection→JSON in the
reload path (restore is binary); macOS/mobile.
