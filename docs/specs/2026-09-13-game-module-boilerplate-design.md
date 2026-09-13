# Game module boilerplate — `Arcane::GameModule` + engine-owned scene systems

**Status:** design, 2026-09-13. Follows the arcbuild driver arc
(`docs/specs/2026-09-13-arcbuild-driver-design.md`, implemented the same
day). Implementation plan to follow via `superpowers:writing-plans`. User
rulings folded in: the module's code is a **class deriving
`Arcane::GameModule`**; the engine's standard systems are **engine-owned**
and a game system **declares its placement** with Astra's `Before`/`After`
traits ("use what we've built with Astra and take the AAA pattern").

## 1. Why

`ReferenceProject/Source/ReferenceGame.cpp` (155 lines) and the Gacha
Game's `Source/Aphelyon.cpp` (190 lines) differ in two places: which
systems `Init` registers, and Aphelyon's `DrawUI` HUD. Everything else is
the same ~110 lines, copied: a per-project `GameApi.hpp` for the export
macro; the seven required `extern "C"` exports plus `DrawUI`; `Init`'s
prologue (pin the shared `TypeContext`, install the Mosaic log sink and
assert handler, adopt the host's ImGui context and allocators, open this
module's `Astra::ComponentModule` and drain the `ARCANE_COMPONENT`
registrar); `Shutdown`'s ordered teardown; and the whole `SaveState` /
`LoadState` registry round-trip with the `SceneRoot` re-registration
dance. The System class template's comment tells a new project to "add
this line to `GamePlugin_Init`, beside the engine systems" — so every
project also copies the engine's own system list.

Unreal's answer is `IMPLEMENT_PRIMARY_GAME_MODULE`: one macro emits the
entry points, and the engine ticks the world — a game module registers
nothing about transforms or rendering; gameplay declares *where* it runs
(tick groups). Unity DOTS is the same shape (`TransformSystemGroup` is
the engine's; `[UpdateBefore]` places yours). This spec brings both halves
to Arcane. The pieces already exist: `Runtime::InstallEngineSystems()`
installs `PhysicsSystem` engine-side and reinstalls it after a module
unload; Astra `9607fb6` added `SystemTraits<Before<…>, After<…>>` with a
stable topological reorder, keyed by `TypeID<T>::Hash()` — a compile-time
hash of the type *name*, so a game module can name an engine system across
the DLL boundary by construction.

## 2. Goals / non-goals

**Goals**
- `ArcaneClient/src/Arcane/Plugin/GameModule.hpp`: `class GameModule`
  (virtual hooks, every one defaulted) + `ARCANE_GAME_MODULE(Type)`, which
  emits the eight exports and the prologue. A module is its own class plus
  one macro line.
- **All three existing module sources adopt it as the arc's ending part**
  (user ruling 2026-09-13): `ArcaneTests/plugins/HotReloadPlugin.cpp` (the
  V1/V2/Bad DLLs — the macro's test vehicle, see §7), `ReferenceGame.cpp`,
  and the Gacha Game's `Aphelyon.cpp`. After the arc no module in either
  repo spells the exports by hand.
- `Runtime::InstallEngineSystems()` owns `TransformPropagationSystem` and
  `RenderSubmissionSystem` beside `PhysicsSystem`. Modules stop registering
  them. A game system places itself with `Before`/`After` against the
  engine's system types.
- `GameApi.hpp` disappears from both projects (the macro carries the
  export attribute). The class wizard's System template documents the
  placement idiom instead of the old "add this line" instruction.

**Non-goals**
- Engine phase *markers* (`Phase::PrePhysics` …). The engine systems are
  the anchors; `AddSyncPoint` fences exist in Astra for the day a hard
  barrier is needed. Not built until a second need appears.
- Any change to the plugin ABI's *shape*: the export names, signatures and
  `EngineContext` stay exactly as they are. The ABI *number* bumps (§6)
  because the contract about who registers the scene systems changed.
- Auto-build after the class wizard's Create (still the future Settings
  opt-in). Live Coding. Multiple game modules per project.
- A raw-ABI test plugin kept "for reference": what the ABI pins is the
  export set and its semantics, and `ArcaneTests/src/HotReloadTest.cpp`
  pins those from the host side regardless of how the plugin spells them.

## 3. `Arcane/Plugin/GameModule.hpp`

### 3.1 The class

```cpp
namespace Arcane
{
    class GameModule
    {
    public:
        virtual ~GameModule() = default;

        // ---- hooks (every one defaulted; override what the module needs) ----
        virtual bool OnInit(EngineContext& ctx) { (void)ctx; return true; }   // after the prologue; false aborts the load
        virtual void OnShutdown() {}                                          // before the ComponentModule closes
        virtual void OnFixedUpdate(double dt) { (void)dt; }
        virtual void OnUpdate(double dt, double alpha) { (void)dt; (void)alpha; }
        virtual void OnDrawUI() {}                                            // between the host's ImGui BeginFrame and Render
        virtual void OnSaveState(Astra::BinaryWriter& w) { (void)w; }         // module extras, AFTER the registry blob
        virtual bool OnLoadState(Astra::BinaryReader& r) { (void)r; return true; }   // module extras, AFTER the registry restore

        // ---- what the prologue established (valid from OnInit to OnShutdown) ----
        [[nodiscard]] EngineContext&          Context()    const noexcept;
        [[nodiscard]] Runtime&                Engine()     const noexcept;   // *Context().engine
        [[nodiscard]] Astra::Registry&        Registry()   const noexcept;   // Engine().Registry()
        [[nodiscard]] Astra::ComponentModule& Components() const noexcept;   // this module's open handle
        // The scene's root entity, read from the registry ON DEMAND -- never cached:
        // Init runs before the host loads the boot scene, and File > Open Scene swaps
        // the whole registry. Invalid when no scene is loaded.
        [[nodiscard]] Astra::Entity           SceneRootEntity() const;
    };
}
```

`Context()` / `Components()` are bound by the macro's `Init` before
`OnInit` runs; calling them outside the `OnInit` → `OnShutdown` window is
a programming error (asserted in Debug).

### 3.2 The macro

`ARCANE_GAME_MODULE(Type)` expands, in the module's own TU, to exactly the
eight exports the host resolves (`PluginEntry::k*` in `PluginABI.hpp`),
with the export attribute inline (`__declspec(dllexport)` on Windows,
`visibility("default")` elsewhere) — no `GameApi.hpp`, no `GAME_API`.
The bodies are today's ReferenceGame bodies, generalised:

- `GamePlugin_ABIVersion` → `kGamePluginABIVersion`. The macro is the
  one-argument face of `ARCANE_GAME_MODULE_ABI(Type, abi)`, whose second
  argument is the value this export reports; the only intended caller of
  the two-argument form is the `HotReloadPluginBad` build (`kGamePluginABIVersion
  + HOTRELOAD_ABI_OFFSET`), which exists to be refused by the host's gate.
- `GamePlugin_Init(ctx)`: the prologue in today's order — (1)
  `Astra::SetTypeContext(ctx->typeContext)`; `Log::InstallMosaicSink()`;
  `Assert::InstallMosaicHandler()`; (2) ImGui adoption when
  `ctx->imguiContext` is non-null (`SetCurrentContext` +
  `SetAllocatorFunctions`; null in a headless host → skipped); (3) open
  `Astra::ComponentModule::Open(ctx->engine->Components(), #Type)` into a
  heap-held handle and drain `Arcane::Game::RegisterComponents` into it,
  logging the count as today; (4) `new Type` into a heap-held pointer,
  bind `Context()`/`Components()`, call `OnInit(*ctx)`. A false from
  `OnInit`, or an empty handle, tears down what was built and returns
  false (the host reports `initial load failed` as it does now).
- `GamePlugin_Shutdown`: `OnShutdown()`; `delete` the instance; `delete`
  the `ComponentModule` handle (releases this module's descriptors + meta
  BEFORE the image unmaps); null both. Same order as today's manual code,
  with the instance going before the handle so a module's destructor may
  still touch its components.
- `GamePlugin_FixedUpdate` / `Update` / `DrawUI` → the hooks.
- `GamePlugin_SaveState(w)`: today's body verbatim (root entity id, then
  the registry snapshot blob, zero-length on snapshot failure), then
  `OnSaveState(w)`.
- `GamePlugin_LoadState(r)`: today's body verbatim (root id, blob,
  `RestoreRegistry`, re-set `SceneRoot`), then `return OnLoadState(r)`.

Both heap-held pointers are RAW on purpose — the ComponentModule
contract's "NEVER a plugin-side static/global object": a static whose
destructor does live cleanup would run during `FreeLibrary` under the
loader lock. A raw pointer has no destructor; a skipped `Shutdown`
degrades to the contract's forget semantics. The macro carries this
comment once so the modules no longer have to.

### 3.3 What a module looks like

```cpp
// Aphelyon.cpp -- the whole file
#include <Arcane/Plugin/GameModule.hpp>
#include <imgui.h>

namespace Aphelyon
{
    struct Module final : Arcane::GameModule
    {
        void OnUpdate(double dt, double) override { m_time += dt; }
        void OnDrawUI() override { /* the HUD, as today */ }
        double m_time = 0.0;
    };
}
ARCANE_GAME_MODULE(Aphelyon::Module)
```

ReferenceGame becomes the class with no overrides at all — the proof that
the defaults are the whole common case.

## 4. Engine-owned scene systems

### 4.1 `Runtime::InstallEngineSystems()`

Grows from one system to the standard three, each behind its own
`HasSystem<T>()` guard (the single early-return today becomes per-system):

| Scheduler | Order | System |
|---|---|---|
| `fixedUpdate` | 1 | `PhysicsSystem` (as today) |
| `fixedUpdate` | 2 | `TransformPropagationSystem` |
| `render` | 1 | `RenderSubmissionSystem` |

`ClearSystems()` (module unload / hot reload) already calls
`InstallEngineSystems()` afterwards, so the standard three come back
without the module's help. They are instantiated inside `ArcaneClient.dll`
— exactly as `PhysicsSystem` and the editor's `EditModeSchedule` already
instantiate them — so they never live in a game image and the
dangling-function-pointer class on unload no longer applies to them.

Relative order among the engine's own systems is insertion order (Astra's
stable topological reorder keeps insertion order for unconstrained
systems). No traits are added to the engine systems in this spec.

**Behaviour change, intended:** a content-only project (no `gameModule`)
now renders in `ArcaneRuntime`/play mode — today nothing registers
`RenderSubmissionSystem` for it. The editor's edit-mode render is its own
`SubmitSceneToBatcher` path and is untouched; `EditModeSchedule`'s own
`TransformPropagationSystem` is untouched.

### 4.2 Placement of game systems

A game system declares where it runs relative to the engine's:

```cpp
struct Movement : Astra::SystemTraits<Astra::Writes<Transform2D>,
                                      Astra::Before<Arcane::TransformPropagationSystem>>
{
    void operator()(Astra::SystemContext& ctx) { /* ... */ }
};
// in OnInit:  std::ignore = ctx.engine->Schedulers().fixedUpdate.AddSystem<Movement>();
```

Verified against the vendored Astra (identical to Astra `dev`):
- Systems are keyed by `TypeID<T>::Hash()`, a compile-time XXHash64 of
  the type name (`__FUNCSIG__`), so the game's `Before<Arcane::X>` matches
  the engine's registration of `Arcane::X` across the DLL boundary.
  Consequence: system types must be **namespaced** (an anonymous-namespace
  type hashes per TU) — the wizard's templates already are.
- An anchor that is not registered resolves to *no edge* — a headless
  host without `RenderSubmissionSystem` imposes no constraint. The flip
  side, a misspelled anchor being silent, is recorded as an Astra
  follow-up ("dangling ordering constraint" diagnostic at plan build), not
  a blocker.
- Edges resolve within a scheduler segment; nothing in Arcane calls
  `AddSyncPoint`, so engine and module systems share segment 0.
- A system with no traits lands after the engine's, in insertion order —
  what today's modules get.

### 4.3 The user's ruling, restated

"Systems stay explicit — their order is a design act" holds: the engine's
list is explicit in one place (`InstallEngineSystems`), a game's placement
is an explicit declaration on the type, and nothing self-registers a
system through static initialisation.

## 5. Consumers — the three module sources, all adopted

1. **`ArcaneTests/plugins/HotReloadPlugin.cpp`** (one source → V1/V2/Bad
   DLLs) → a `GameModule` subclass: `OnInit` creates the `Pulse` entity and
   registers its stepping system (`HOTRELOAD_STEP`), `OnSaveState` /
   `OnLoadState` carry the plugin's own extras (the pulse entity id) after
   the base's registry round-trip; `ARCANE_GAME_MODULE_ABI(Module,
   kGamePluginABIVersion + HOTRELOAD_ABI_OFFSET)`; `PluginExport.hpp`
   deleted. Its `HotReloadTest.cpp` expectations (V1 → V2 reload with state
   carried, Bad refused and rolled back) do not change — that is the point.
2. **`ReferenceProject/Source/ReferenceGame.cpp`** → the class with no
   overrides + the macro; `GameApi.hpp` deleted. The file's header comment
   keeps its role as the minimal end-to-end proof.
3. **Gacha `Game/Source/Aphelyon.cpp`** → `OnUpdate` + `OnDrawUI` (the
   HUD) + `m_time`; `GameApi.hpp` deleted; `SceneRootOf` replaced by
   `SceneRootEntity()`.
4. **`ArcaneEditor/src/Project/ClassTemplates.cpp`** — the System
   template's comment (`:104-120`) shows the `Before`/`After` idiom and
   "add `AddSystem<…>()` in your module's `OnInit`"; `ClassTemplates.hpp:19`
   likewise. `ClassTemplatesTest` pins the new wording.
5. **`build/arcane.lua:90`** — `GAME_BUILD_DLL` stays defined (harmless,
   and an external module may still use it); its comment stops naming
   `GameApi.hpp`.
6. **`PluginABI.hpp:734`**'s comment about modules registering
   `TransformPropagationSystem/RenderSubmission` is rewritten to point at
   `InstallEngineSystems` and `GameModule.hpp`.
7. **`GameComponents.hpp`**'s header comment references "this module's
   `GamePlugin_Init`" — now "the `ARCANE_GAME_MODULE` prologue".

## 6. ABI

`kGamePluginABIVersion` 28 → **29**. The export shape is unchanged, but
the contract about who registers the scene systems changed: a module
built against 28 that still calls `AddSystem<TransformPropagationSystem>`
would get `AlreadyRegistered` (ignored via `std::ignore`, harmless) — the
bump exists so the host's gate makes the cross-build visible rather than
letting it pass by accident, per the standing "bumps are cheap" policy.
`ReferenceProject.arcproj` is restamped in the same commit; the Gacha
manifest is restamped by the driver on its next rebuild and committed
there (`chore(game): restamp …` as usual).

## 7. Testing

- **`[plugin]` — the hot-reload plugins, now built with the macro.**
  `HotReloadTest.cpp`'s existing cases are the macro's plugin test: V1 →
  V2 reload through `PluginHost` against a real `Runtime` proves the
  prologue (the `Pulse` component registers through the drained handle),
  the base `SaveState`/`LoadState` round-trip plus the module's
  `OnSaveState`/`OnLoadState` extras (the pulse entity survives the swap
  with its stepped value), and the `Shutdown` order; the Bad build proves
  the ABI-override seam is what the host's gate refuses. One case is added:
  a hook-order probe (the plugin records `OnInit`/`OnShutdown` into a
  block the test reads through `Module::Symbol`, asserting `OnShutdown` saw
  its component type still registered — the instance is torn down before
  the handle).
- **`[runtime]` — engine-owned systems.** After `Runtime` construction:
  `fixedUpdate.HasSystem<PhysicsSystem>() && HasSystem<TransformPropagationSystem>()`,
  `render.HasSystem<RenderSubmissionSystem>()`; after `ClearSystems()`,
  all three again; the fixed order is Physics then Propagation (observable
  via a probe system with `After<TransformPropagationSystem>` recording
  execution order).
- **`[runtime]` — placement.** A namespaced test system with
  `Before<TransformPropagationSystem>` added *after* the engine's install
  runs before it; one with no traits runs after; one naming an
  unregistered anchor is placed as if untraited (no error).
- **`[editor]`** — `ClassTemplatesTest` pins the System template's new
  comment text.
- **Product:** both real modules rebuilt on the macro; the Gacha Game via
  `arcbuild build` (one compile + link for the module TU); ReferenceProject
  rebuilt; `golden-gate.ps1` **4/4 in both configs** — the runtime-scene
  lanes render the same systems in the same order, so `diffCount=0` is
  the expected verdict, and the gate is run rather than assumed.

## 8. Rollout

1. `GameModule.hpp` + `Runtime::InstallEngineSystems` + the `[runtime]`
   tests (RED first); ABI 29 + `ReferenceProject.arcproj`.
2. **The ending part — the three module sources, in this order:**
   `HotReloadPlugin.cpp` (the `[plugin]` suite must stay green across the
   conversion, plus the new hook-order case), then `ReferenceGame.cpp`
   (`GameApi.hpp` gone; gate), then Aphelyon (Gacha commit + restamp).
3. Wizard/template/ABI-header comments; `ClassTemplatesTest`.
4. Memory + the arc's close: a repo-wide grep proves no hand-spelled
   `GamePlugin_` export definition remains outside `GameModule.hpp`.
