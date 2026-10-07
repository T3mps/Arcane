# Settings and cvar completion -- design

**Status:** Implemented 2026-10-07 on branch `feat/settings-s7` (tip `79c3f14a`; it contains `feat/settings` through merge `f5b40c29`; S1-S7 gates green, plugin ABI 54). Written 2026-10-03 from the brainstorm with the user; decisions in s16. What shipped: s14.5.
**Supersedes for scope:**
- the cvar plan's "Hygiene pass 2026-09-28: what the review found owed" list (`docs/plans/2026-09-24-cvar-system-plan.md`);
- the never-built v1 items of `docs/specs/2026-09-02-cvar-system-design.md` (the server surface, `ARC_CVAR_ALIAS`);
- the "configuration pass" memory note.

**Keeps:** the 2026-09-02 cvar spec's model. That means typed values, `SetBy` provenance, the publish barrier, default-deny, names over GUIDs, and Config file stems as categories. This spec extends that model; it does not replace it.

---

## 1. What the user asked for (2026-10-03)

> "Finish the cvar system, alongside a full settings UI for the editor. These would obviously control the same values... We want to also ensure we're exposing literally everything to the end user. We want this to be a highly customizable engine."

The user's decisions, in order:
1. **Audience = developers AND players.**
   - There are two editor windows, like Unreal: **Editor Preferences** (personal) and **Project Settings** (shared, committed).
   - Games can also expose any player-safe setting in their own runtime menus.
   - "Also the Source 2 model, where the cvars access all settings, too": every setting is reachable as a cvar from the console, `--set` and the config files.
   - The settings players can reach must exclude "pointless editor things, or ones we don't want/need players messing with".
2. **Sweep scope = full audited sweep.** Every hard-coded tunable in the engine and editor is inventoried and classified, and every real setting becomes a cvar in this arc. Defaults stay identical.
3. **Rich settings:**
   - the theme editor, keyboard shortcuts, fonts and scale, and layouts and panels;
   - "everything that makes sense": Astra memory settings, Manifold2D physics settings, network settings, "the whole shabang";
   - "making it trivial to extend in the future".
4. **Architecture = C, the hybrid.**
   - cvars are the one store.
   - A setting is declared either as a one-off `ARC_CVAR` or as part of a reflected **settings struct**, Unreal `UDeveloperSettings` style. The struct's fields become cvars, and code reads them as a typed snapshot.
   - Both windows are generated from metadata, plus custom pages.

## 2. Where it stands (verified in code, 2026-10-03, main 2ea5bc9e)

**Built:**
- the typed registry (8 value types), the `SetBy` ladder with history, `Publish()` once per frame, and default-deny;
- `--set`, and the config layers (engine -> plugin -> project -> user; `Runtime.cpp:546-551`);
- `cvar_explain` and `cvarlist`;
- `ARC_CVAR_RANGED` (min/max + declaring module; `CVarDecl.hpp:28`), and Archive write-back to `<project>/Saved/Config` (3df4360d);
- the editor Console (completion, history) and the runtime overlay;
- about 15 cvars: `editor.undo.*`, `editor.graph.*`, `editor.inspector.*`, `render.meshCull`, `diagnostics.drawMarkers`, `log.level` and `console.historySize`.

**Still open** (the 2026-09-28 owed list, each re-verified):
- **O1.** `ARC_CVAR` and `ARC_COMMAND` hard-code module `"engine"` (`CVarDecl.hpp:21`, `:44`). `CVarRegistry::UnregisterModule` (`CVarRegistry.cpp:300`) has no caller. As a result, a rebuilt game module's cvars collide as duplicates, and stale function pointers stay live.
- **O2.** Config layers are applied in `OpenProject` (`Runtime.cpp:546-551`), BEFORE the game module's statics register. Game-module cvars therefore never receive their config values, and nothing re-applies them on a reload.
- **O3.** `Set` appends a history record on every call (`CVarRegistry.cpp:294`), so equal-rung repeats and every project open grow the history.
- **O4.** Four smaller defects:
  - `Register` does not check that `defaultValue.type == type`;
  - the cheat revert updates values without firing callbacks (`:395-400`);
  - callback dispatch iterates a vector that a callback can grow;
  - command success is inferred from the reply text (`:462`).
- **O5.** The spec's "workers read an immutable snapshot" has no implementation.
- **O6.** Missing pieces:
  - handles cannot be named in code (reads go by string);
  - there is no `PublishImmediate()`;
  - the eight-type round-trip test and the `GpuDrawMarkersEnabled` test are missing;
  - `Color` and `Vec2/3/4` are reserved without storage (`CVarTypes.hpp:22-25`).
- **O7.** Never built: the server surface (spec s7.3) and `ARC_CVAR_ALIAS` (spec s10).

**Settings that live outside the registry today:**
- `ViewportSettings` (camera mode, speed, grid, gizmo size), stored in `imgui.ini`;
- `UndoSettings` and `TextureImportSettings`;
- 31 theme colour tokens (`EditorTheme.hpp`, `inline constexpr ImVec4`);
- about 49 ImGui-routed editor key checks across 11 files, plus 22 raw SDL-scancode bindings (viewport/camera, `EditorAppFrame.cpp`) and 7 keys inside the vendored imgui-node-editor (corrected by the 2026-10-03 inventory);
- about 575 numeric `constexpr`s across ArcaneCore, ArcaneClient, ArcaneEditor, ArcaneRuntime and ArcaneServer. Many are true constants and many are tunables.

**Library configs Arcane fills today with defaults:**
- Astra `Registry::Config` (`EntityManager::Config`, `ArchetypeChunkPool::Config`: chunkSize, chunksPerBlock, maxChunks, initialBlocks, useHugePages, min/maxChunkBytes; `ResourceStorage::Config`);
- Manifold2D `PhysicsWorld` world definition (broadphase kind, hashCellSize, gravity, substepCount, contactHertz, contactDampingRatio, restitutionThreshold, ...).

---

## 3. The model

### 3.1 One store, three ways in
Every setting is a cvar in the one `CVarRegistry`. The windows, the console, `--set`, the config files and game code all read and write the same values through the same permission checks. No setting has a second store.

### 3.2 Who may touch what: audience x context, and the game's policy

**The audience** (what the setting is) is declared once per setting:
- `Editor`: the editor's own settings. Declared in ArcaneEditor or an editor plugin, so a shipped game never contains them.
- `Game`: engine and game-module settings that run in every build.
- `PlayerSafe`: a `Game` refinement, safe for any player to change (graphics, audio, controls).
- `Server`: settings of the authoritative simulation and session, Source's `sv_*` (gravity, tick rate, timeouts, match rules).

**The context** (who is asking right now) comes from the session role, not from which console was used:

| Context | Who | Example |
|---|---|---|
| `Editor` | the editor, its console, `--set` in the editor | everything |
| `LocalHost` | the local player of a single-player game or the HOST of a listen server: their console IS the server's console | Source's local `sv_cheats 1` + `sv_gravity 200` |
| `ServerAdmin` | a dedicated server's own console, or an authenticated remote admin (s9) | rcon |
| `Client` | a player connected to someone else's server | |

**The default rules**, before any game policy:

| Audience \ Context | Editor | LocalHost | ServerAdmin | Client |
|---|---|---|---|---|
| `Editor` | read/write | absent | absent | absent |
| `Game` | read/write | read; write only if `Cheat` and cheats on | read; write only if `Cheat` and cheats on | read |
| `PlayerSafe` | read/write | read/write | read/write | read/write (their own client) |
| `Server` | read/write | read/write (`Cheat` ones need cheats on) | read/write (`Cheat` ones need cheats on; audited) | read the replicated value; write never (asks the server through a game command, if the game offers one) |

- **The cheats gate** is itself a `Server` setting, `server.cheats` (Source's `sv_cheats`). It is replicated, so a client's own `Cheat` settings are gated by the server it is connected to. Turning it off reverts every `Cheat` setting (s4.5).
- **Unchanged flags:**
  - `Dev`: compiled out of Dist, whatever the audience.
  - `Hidden`: never listed or completed; still settable by exact name in the `Editor` context.
  - `Protected`: never readable outside `Editor` and `ServerAdmin` (secrets, passwords).
- **Archive and player-settable split.** `Archive` means only "persist me"; it no longer implies player-settable. Today's `UserSettable` bit is derived from the audience, so default-deny holds by construction: a setting nobody marked `PlayerSafe` or `Server` cannot be set from a `Client`.
- **Editor-audience settings never exist in a shipped game**, because they are declared in the editor DLL. They are absent, not merely hidden.

**The game's policy: not restrictive by design.** Game developers decide who may change their own settings in their game. A game module can install a `CVarPolicy`:
```cpp
// Called for any non-Editor context. Return Allow / Deny, or Default to keep the table above.
Arcane::SetCVarPolicy([](const CVarInfo& cvar, const CVarRequest& req) -> PolicyVerdict {
    if (cvar.name.starts_with("match.") && req.caller.IsLobbyOwner()) return PolicyVerdict::Allow;   // private-lobby rules
    if (req.context == Context::Client && cvar.name == "server.gravity" && req.caller.HasRole("moderator")) return PolicyVerdict::Allow;
    return PolicyVerdict::Default;
});
```
- **What a policy can do:**
  - widen access (let lobby owners, moderators or a single-player "mods" menu change any `Game` or `Server` setting);
  - narrow access (lock settings in ranked play).
- **What a policy cannot do:**
  - reach `Editor` settings, which do not exist in a game;
  - make `Protected` values readable by clients;
  - bypass `Dev` compile-out.
- **Engine-level knobs** let a game that wants no policy code still choose its defaults:
  - `server.allowClientSetServer` (default off): `Client` may set `Server` settings when the server allows it;
  - `server.cheatsAllowed` (default on for LocalHost, off for dedicated): whether `server.cheats` may be turned on at all.
- **Audit and replication:** every non-Editor set of a `Server` setting goes to the audit sink (s9) with who, old, new and the verdict source (table or policy). A changed `Server` setting that is `Replicated` is sent to clients by the replication arc.

### 3.3 Where a value is saved: the home scope
Every setting has a **home scope**, which picks the window it appears in and the layer its edits write to:
- `Preferences`: per user. Edits write the user's own rungs (s11.1):
  - machine-wide preferences (theme, fonts, shortcuts, layouts) go to the EditorUser rung;
  - per-project ones (camera feel, undo budget) go to the User rung under the project's `Saved/Config`.

  **Per-project override is one click (user, 2026-10-03).** Every Preferences row has a scope switch, "All projects" or "This project", with an icon showing which one is set:
  - "This project" writes the User rung for this project, so it beats the machine-wide value.
  - Switching back to "All projects" clears the project value.
  - The Modified filter has a "Project overrides" option that lists them all.
- `Project`: shared and committed. Edits write the Project rung, the project's `Config/<category>.json`. This covers rendering, physics, Astra memory, networking, input, assets and cook, and game settings.

**Scope is where the DEFAULT is edited. The override rung follows the audience** (inventory Reconciliation R2). A `PlayerSafe` setting's player override always writes the per-machine User rung (`GameUserDir` in Dist; s8.2), even when its default lives in Project. Examples: `render.backend`, `render.vsync`, window size, input deadzones. The inventory writes scope as `Pref-M` (machine-wide, the EditorUser rung), `Pref-P` (per-project User rung) or `Project`.

The layer system is unchanged: engine -> plugin -> project -> user -> command line -> code -> console. So a project default can still be overridden locally (the user rung wins), and both windows show that (s6.4).

### 3.4 When a change applies
Every setting declares one apply mode:
- `Live`: takes effect at the next `Publish()`. Callbacks and snapshots update.
- `NextWorld`: read when a world, registry or physics world is created. Shows the badge "applies on next world load" (Play-in-editor restart, scene reopen).
- `Restart`: read once at boot (device, backend, thread counts). Shows the badge "restart required", and the window offers "Restart editor".

These are statements about the consumer, so the binding that reads the value declares them. Tests pin each binding's mode (s13).

### 3.5 Determinism and replication
Settings that change a simulation's outcome carry `Deterministic`: physics solver settings, the fixed step, and anything the bit-exact trajectory fixture depends on. `Replicated` and `ServerOnly` stay reserved until the replication arc enforces them. The windows show a "simulation" badge on `Deterministic` rows, because changing one changes replays and goldens.

---

## 4. The cvar core: completion

### 4.1 Value types
- `Color` (RGBA float; JSON `"#RRGGBBAA"`, or `[r,g,b,a]` on read) and `Vec2`, `Vec3`, `Vec4` (JSON arrays) gain storage, accessors, clamping (per component, against `Vec` min/max) and console parsing (`1 2 3`, `#ff8800`).
- **New `Enum` type:**
  - storage is an `Int32` value plus the declared ordered list of names, taken from a reflected enum or an explicit list;
  - JSON and the console accept the name (`"Perspective"`); the number is accepted on read with a warning;
  - the widget is a combo.
- Strings with a widget hint cover the rest:
  - `asset:<kind>` (an asset GUID string; picker);
  - `path:file` and `path:dir`;
  - `keychord` (s7.2);
  - `font` (s7.3).

The CVarValue variant grows. That is an ABI change (s14.3).

### 4.2 Metadata
`CVarDesc` gains:
- `displayName`;
- `keywords`;
- `widget` (the hint string);
- `audience`;
- `scope`;
- `apply`;
- `order` (a stable sort within a category);
- `categoryPath`. The default is derived from the dotted name (`physics.solver.substeps` -> Engine / Physics / Solver); it can be overridden for display only, and the name stays the address.

Help text is required for every non-`Hidden` setting. Registration refuses an empty help string, which a test pins.

### 4.3 Declaring settings
**One-off:**
```cpp
ARC_CVAR(cvar_graphFitMinZoom, "editor.graph.fitMinZoom", Float32, 0.5f,
         Arcane::Range(0.1f, 2.0f), Audience::Editor, Scope::Preferences, Apply::Live,
         "Smallest zoom a graph's frame-to-fit may pick.");
```
The first argument is the handle's C++ identifier: the preprocessor cannot derive an identifier from a dotted string.
- The macro gets an overload set: the old positional forms keep compiling and map to `Audience::Game`, `Scope::Project`, `Apply::Live`, with the flags as given. `ARC_CVAR_RANGED` folds into it.
- The declaring module is captured automatically. Each module's statics run under a "current module" scope that the plugin host sets, the same pattern `Astra::ModuleScope` uses for components. The `"engine"` literal disappears (fixes O1).
- The macro defines a nameable handle: `ARC_CVAR(...)` expands to `inline const CVarRef<float> cvar_graphFitMinZoom = ...`, so code reads `cvar_graphFitMinZoom.Get()` with no string lookup (fixes O6, unnameable handles). A header can declare it with `ARC_CVAR_EXTERN` for use from other translation units.

**A system's group, as a settings struct:**
```cpp
struct PhysicsSettings
{
    std::uint32_t substeps = 4;
    float contactHertz = 30.0f;
    PhysicsBroadphase broadphase = PhysicsBroadphase::Tree;
};
ARCANE_REFLECT_TYPE(PhysicsSettings)
    ARCANE_REFLECT_TYPE_ATTR(Settings, "physics", Scope::Project, Apply::NextWorld, Audience::Game)
    ARCANE_REFLECT_FIELD(PhysicsSettings, substeps)     ARCANE_REFLECT_ATTR(Range, 1, 16)  ARCANE_REFLECT_ATTR(Tooltip, "Solver substeps per fixed step") ARCANE_REFLECT_ATTR(Deterministic)
    ARCANE_REFLECT_FIELD(PhysicsSettings, contactHertz) ARCANE_REFLECT_ATTR(Range, 1.0f, 240.0f) ARCANE_REFLECT_ATTR(Tooltip, "...")
    ARCANE_REFLECT_FIELD(PhysicsSettings, broadphase)   ARCANE_REFLECT_ATTR(Tooltip, "...")
ARCANE_REFLECT_TYPE_END(PhysicsSettings)
ARC_SETTINGS(PhysicsSettings);   // registers one cvar per reflected field: physics.substeps, ...

const PhysicsSettings& s = Arcane::Settings<PhysicsSettings>();   // typed snapshot (s4.6)
```
- It reuses the reflection Arcane already has (`Reflection.hpp`: `Range`, `Tooltip`, `DisplayName`, `Category`, `ColorFormat`, `FilePath`, `AliasName`, `Precision`, `Deprecated`, `Hidden`).
- New Arcane-side attributes: `Settings` (type), `Keywords`, `Widget`, `Deterministic`, `PlayerSafe`, `Apply`, `Scope`. Any of them on a field overrides the type default.
- Field types map onto cvar types, with enums becoming `Enum` and `Color`/`Vec*` mapping directly. An unmappable field fails the build with a static_assert that names it.
- The struct's default member initializers ARE the defaults, so there is one source of truth.
- "Trivial to extend" means this: an engine system, a plugin or a game module adds a field (or a struct plus `ARC_SETTINGS`), and the setting appears in the console, the config files and the right window with no other edit.

### 4.4 Module lifetime and reload (fixes O1, O2)
- Every registration records the declaring module, captured as described in s4.3.
- Unloading a plugin or game module calls `CVarRegistry::UnregisterModule(module)` from `PluginHost`'s unload path, next to `UnregisterModuleRange`. That drops the module's cvars, settings structs, commands and callbacks, and every history record the module sourced. Handles into it go stale (the generation bump), and the registry never calls into the unloaded module again.
- When a module (re)loads and its statics have registered, the host calls `ApplyLayersFor(module)`. That re-applies every config rung (engine, plugin, project, user, command line) to just the cvars that module declared, then publishes. A reloaded module therefore gets exactly the values a cold boot would give it.
- A game module's Archive values written by the user survive a hot reload: the User rung is applied again from disk, and unsaved edits are flushed to disk before the unload.

### 4.5 History and correctness (fixes O3, O4)
- **History holds one record per (rung, source).** A `Set` at a rung that already has a record from the same source replaces it. Re-opening a project replaces its rungs. `CloseProject` pops the project and user rungs it pushed.
- `Register` refuses a default of the wrong type, a min > max range, empty help (non-`Hidden`), and an `Enum` with no names.
- The cheat revert routes through the normal publish path, so callbacks fire and snapshots refresh.
- Callback dispatch takes a copy of each slot's callback list before invoking it. A callback added during dispatch first fires on the next change.
- **Commands** return `CommandResult { bool ok; std::string text; }`. `ARC_COMMAND` gains that signature; the old text-returning form is wrapped and treated as ok.

### 4.6 Threading: the published snapshot (fixes O5)
- The main thread writes. `Publish()` builds an immutable `CVarSnapshot`: a flat value array indexed by slot, plus one block per settings struct holding its typed copy.
- `Publish()` swaps the snapshot in through an atomic shared pointer (`std::atomic<std::shared_ptr<const CVarSnapshot>>`).
- Worker threads (jobs, render, physics substeps) read only `CVarRegistry::Snapshot()` or `Settings<T>()`. Both are wait-free reads of the current snapshot, and a reader keeps its copy alive for as long as it holds it.
- `Settings<T>()` on the main thread returns the same snapshot block, so there is no second code path.
- `PublishImmediate()` exists for tools and tests: it publishes and swaps outside the frame boundary. It asserts that it runs on the main thread.

### 4.7 Renames (fixes O7, alias)
- `ARC_CVAR_ALIAS("old.name", "new.name")`, or the `AliasName` attribute on a settings field, resolves the old name in config files, `--set` and the console. Each use logs a one-time warning naming the new name.
- `WriteCVarArchive` writes only new names, so the next save migrates a user's file.

### 4.8 Persistence formats
- One JSON file per category (unchanged). `Enum` values are written as names, `Color` as `"#RRGGBBAA"`, and `Vec*` as arrays.
- An unknown key is reported, not applied (unchanged). It now also surfaces in the Problems panel, with the file and key as a locator, instead of only being returned in `CVarApplyReport`.

---

## 5. Settings for systems and libraries (bindings)

### 5.1 The binding rule
- Vendored libraries (Astra, Manifold2D, Mosaic) never include Arcane headers. Arcane owns a settings struct per library, and **binds** it to the library's own config struct at the point where Arcane creates the library object.
- A binding is a small, pure function such as `ToAstraConfig(const AstraMemorySettings&) -> Astra::Registry::Config`. Tests pin it field by field, so a library upgrade that renames a field breaks the build, not the behaviour.
- Live-capable fields (a library setter exists, e.g. Manifold2D gravity) are applied through a cvar callback.
- Everything else is `NextWorld` or `Restart`, as s3.4 describes.

### 5.2 The initial binding set
Each category below gets a settings struct and appears in Project Settings unless marked otherwise. Field lists are illustrative; the s10 audit decides the final fields.

| Category | Library / system | Examples | Apply |
|---|---|---|---|
| `astra.memory` | Astra `Registry::Config` | chunkSize, chunksPerBlock, maxChunks, initialBlocks, useHugePages, min/maxChunkBytes; ResourceStorage limits | NextWorld |
| `physics` (2D) | Manifold2D world definition | broadphase, hashCellSize, gravity, substepCount, contactHertz, contactDampingRatio, restitutionThreshold, sleep thresholds; the fixed step (`sim.fixedHz`) | all NextWorld (Manifold2D has no gravity setter; PhysicsSystem captures dt at creation); all `Deterministic`. `server.tickHz` is separate from `sim.fixedHz` |
| `render` | the renderer and NRI | backend (D3D12/Vulkan), vsync, frames in flight, mesh cull, selection-outline width, MSAA/AA, gamma, debug markers | mixed; backend Restart |
| `jobs` | enkiTS / job system | worker threads, pin main thread | Restart |
| `net` | Arcane `Net` (TcpSocket, RateLimiter, Protocol), host ports | timeouts, rate-limit buckets, buffer sizes, server port defaults | Restart / Live per field |
| `assets` / `cook` | asset manager and cook | watch interval, cook threads, thumbnail size, mesh light triple | mixed |
| `input` | input system (non-document parts) | dead zones, repeat delay; action maps stay document-shaped in `input.json` | Live |
| `log` / `diagnostics` | logging and crash capture | log level, sinks, capture-on-hang timeout, minidump kind | Live / Restart |
| `editor.*` | ArcaneEditor (Preferences) | undo, graph, inspector, viewport camera and grid, autosave, theme, fonts, shortcuts, layouts | mostly Live |
| `audio`, `app` (splash, window), `boot`, `console`, `runtime`, `sim`, `server`, `debug.physics`, `editor.thumbnail` | the systems found by the 2026-10-03 inventory | see the inventory's canonical names (Reconciliation R1) | per row |

- Two existing defaults move the thumbnail golden set if they ever change: thumbnail size and the mesh light triple (the cvar plan's exclusion). They become cvars with IDENTICAL defaults, and only a deliberate change re-blesses.
- The replication arc adds `net.replication.*` through the same declarations. There is no separate design.

---

## 6. The two windows

### 6.1 Placement
- `Edit > Preferences...` opens **Editor Preferences**. That is today's disabled placeholder (`EditorPanels.cpp:318`).
- `Edit > Project Settings...` opens **Project Settings**, replacing today's read-out (`DrawProjectSettings`, `EditorPanels.cpp:2975`). The read-out's content (name, GUID, ABI, game module, mounts) becomes the "Project" page (s6.6).
- Both are dockable editor windows, so they persist in the layout like other panels and can float or dock.

### 6.2 Layout
- **Left:** a category tree, built from `categoryPath`, with settings structs as nodes. Plugin and game-module categories sit under "Plugins › <name>" and "Game › <module>".
- **Top:**
  - a search field that matches names, display names, help and keywords, and filters the tree to the categories with hits;
  - a **Modified** filter (value != default) and an **Overridden** filter (a higher rung wins);
  - a **Show advanced** toggle that reveals `Dev` and `Hidden` rows.
- **Right:** the selected category's rows, grouped by `Category` attribute sub-headers, using the existing property-grid widgets (`PropertyGrid`, `SliderRow`/`FloatRow`, colour picker, asset picker, enum combo).
- **Each row** shows:
  - a label (display name), with the help text as a tooltip;
  - the widget;
  - a reset arrow when the value differs from the default;
  - badges for `NextWorld`, `Restart` and `Deterministic`;
  - a provenance marker when a higher rung wins (s6.4);
  - the cvar name (dim, copyable) on hover, so the console spelling is always discoverable.

### 6.3 Editing
- An edit is `Set(value, rung = the window's scope)` plus `Publish()`.
- The archive for that rung is written debounced (500 ms; `editor.settings.saveDebounceMs`) and always on window close and editor exit. The write is atomic, as today.
- Edits are undoable inside the window: a window-local undo stack, separate from the scene undo, because settings are not scene state. Ctrl+Z works when the window is focused.
- Dragging a slider is one undo step, using the existing `EditGesture`.
- Project Settings edits mark the project's `Config/` files dirty in the source-control sense only. There is no save prompt.

### 6.4 Provenance and overrides
- When a rung above the window's scope wins (a user override shown in Project Settings, or `--set`, Code or Console), the row shows the winning value with a marker: "Overridden by User / Command line / Console". It offers **Clear override**, which pops that rung for this cvar.
- The row's context menu offers **Explain**, which shows the `cvar_explain` history inline.

### 6.5 Restart and next-world flow
- After any edit to a `Restart` setting, the window shows a bar: "N settings need a restart -- Restart editor". The restart reopens the same project.
- `NextWorld` settings show "Applies on next Play or scene reopen". There is no forced action.

### 6.6 Custom pages
- `RegisterSettingsPage(scope, categoryPath, DrawFn)` lets any module add a page.
- The rich pages (s7) and the "Project" identity page use it.
- Custom pages still read and write cvars, with the same provenance and undo helpers. A page that edits non-cvar data must say so in its header, as the Project identity page and the layout files do.

---

## 7. The rich pages

### 7.1 Theme (Preferences › Appearance › Theme)
- Each of the 31 theme tokens (`EditorTheme.hpp`) becomes a `Color` cvar in an `EditorThemeSettings` struct (`editor.theme.accent`, ...). `ApplyEditorTheme` reads the snapshot, and a change re-applies the ImGui style at the next frame (Live).
- The page shows:
  - grouped swatches;
  - a live preview pane with sample widgets (a button, a toggle, a selected tab with its overline, a text field, a warning/error row);
  - the contrast ratio, from the existing `Theme::ContrastRatio`, next to each text/background pair, with a warning under 4.5:1 (3:1 for large text and icons).
- **Presets:**
  - Dark (today's values, the default);
  - Light;
  - High Contrast.

  Presets are JSON theme files shipped in `data/EditorThemes/`. Applying a preset sets the User rung for every theme cvar.
- **Import/Export** read and write a `.arctheme` JSON with the same keys.
- The contrast tests in `EditorThemeContrastTest.cpp` keep pinning the Dark preset's values.

### 7.2 Keyboard shortcuts (Preferences › Keyboard)
- **An editor action registry.** Every editor command becomes a named action (`editor.view.frameAll`, `edit.undo`, `graph.delete`, ...), with:
  - a display name and context (Global, Viewport, Graph, Asset Browser, Text, Inspector, ...);
  - a default chord;
  - a callback.

  The ~40 hard-coded `IsKeyPressed`/`Shortcut` checks (8 files) become action lookups. Each action's chord is a `keychord` String cvar: `editor.keys.<action>`, Preferences, Live.
- **The page:** a searchable table with columns Action, Context, Shortcut and Default. Clicking a shortcut cell listens for the next chord; Esc cancels and Backspace clears.
- **Conflict detection (user, 2026-10-03):** two actions in overlapping contexts with the same chord are allowed and saved, but:
  - EVERY row involved shows red, not just the newer binding;
  - hovering any of them shows a tooltip naming the exact conflict: "Ctrl+D is also bound to Duplicate Node (Graph) and Delete Line (Text)". It lists each other action and its context;
  - while the conflict stands, the more specific context wins (Graph over Global); between equal contexts, the newer binding wins. The tooltip says which one fires.
  - "Reset all" restores the defaults.
- **Menus** show the current chord next to each item, so the menus and the page cannot drift.
- **Key type: labelled by default, physical for positional clusters** (user, 2026-10-03, after research). Every action declares how its key is matched:
  - **Labelled (logical, the default):** for keys chosen for their letter or mnemonic (Ctrl+Z, Ctrl+S, F to frame, Del). This matches the OS and every other app on the user's layout.
  - **Physical (scancode):** only for keys chosen for their POSITION, used like a joystick: fly-camera W/A/S/D/Q/E, and a positional gizmo tool row (W/E/R) if kept. On AZERTY these become Z/Q/S/D in the same places.
  - **Precedent:**
    - SDL's keyboard guidance (scancodes for "a joystick with a lot of buttons", keycodes for "press I for inventory");
    - the W3C UI Events `code` vs `key` split;
    - Godot (`physical_keycode` for movement; its editor freelook and the QWER tool row are physical since PR #73651, everything else logical);
    - Unity's Input System (keys named by position; `#(a)` binds by character);
    - VS Code (`ctrl+z` by produced character; `[KeyZ]` for a position).

    Unreal and Blender use labels everywhere, and their AZERTY users must rebind the viewport keys by hand.
  - **Cvar format, following VS Code:** `"Ctrl+Z"` is labelled and `"[KeyW]"` is physical. The shortcuts page has a Type column, so the user can flip any binding.
  - **Display** always uses the current layout's labels. Physical keys go through the layout map (SDL `GetKeyFromScancode` + `GetKeyName`), so an AZERTY camera row reads "Forward: Z".
  - **Non-Latin layouts** (Cyrillic, Greek): a labelled letter chord falls back to the key's QWERTY position when the layout produces no Latin letter, so Ctrl+C works on a Russian layout as it does across Windows.
  - **Both input routes resolve through the one action map:** the ImGui-key route (the 49 checks) and the SDL-scancode route (the 22 viewport/camera bindings).
- Game input actions are a different thing: the project's `input.json`, edited in the Input Actions document. Project Settings › Input links to that document.

### 7.3 Fonts and scale (Preferences › Appearance)
- `editor.ui.fontFamily` (`font` hint; the bundled families plus any `.ttf` under the user's fonts folder);
- `editor.ui.fontSize` (default 16, today's `InstallEditorFonts(16)`);
- `editor.ui.monoFontFamily`;
- `editor.ui.scale` (0.75-2.0, default 1.0; multiplies the style metrics and the font size, and follows the monitor DPI when `editor.ui.followDpi` is on).

A change rebuilds the font atlas at the next frame boundary (Live, deferred one frame). The NRI ImGui backend already re-uploads atlas textures.

### 7.4 Layouts and panels (Preferences › Layout)
- **Named layouts** are saved as files under the user's `Saved/Layouts/<name>.ini`: an ImGui dock layout plus panel visibility. The page offers Save Current As, Load, Delete, Set As Default and Reset To Factory.
- **Settings:**
  - `editor.layout.default` (String, the name);
  - `editor.layout.openPanelsAtStart` (a list in a String, using the panel-visibility vocabulary).
- Layout files are document-shaped, like `input.json`. They are not cvar values, so the page header says "Layouts are files" (s6.6).
- The rule "imgui.ini vetoes authored UI changes" stays true for the CURRENT layout. Factory reset and named layouts give the user an explicit way out.

---

## 8. Players and runtime

### 8.1 The game-facing settings API
- `Arcane::PlayerSettings::List(categoryPrefix)` returns descriptors for `PlayerSafe` settings only: name, display name, help, type, range, enum names and apply mode.
- `Arcane::PlayerSettings::Set(name, value)` goes through `Permission::Player` and the same default-deny check.
- A game's own settings menu (graphics, audio, controls) is built from these lists, so adding a `PlayerSafe` field to a settings struct adds it to the game's menu with no UI code.

### 8.2 Where a shipped game saves the player's settings
- In the editor and dev builds: unchanged (`<project>/Saved/Config`, the User rung).
- In a **Dist** build, the User rung lives under the per-user OS location, NOT the install folder (which may be read-only): `%LOCALAPPDATA%/<Company>/<Game>/Config` on Windows, and `$XDG_CONFIG_HOME/<company>/<game>` on Linux. The company and game names come from the project.

### 8.3 The runtime console
The overlay's context is the session role (s3.2):
- `LocalHost` in single-player and on a listen-server host, so `Server` settings are settable there (and `Cheat` ones once `server.cheats` is on), as in Source;
- `Client` when connected to someone else's server.

The game's `CVarPolicy` applies on top. `Dev` settings are compiled out of Dist.

---

## 9. The server surface (O7)
- **Engine side**, transport-agnostic: `RemoteCVarService::Handle(request) -> response`, for get/set/list/explain in the `ServerAdmin` context (s3.2).
  - It honours `Protected` (never readable remotely) and `ServerCanExecute`.
  - Every set is passed to an injected audit sink with who, old, new and when.
- **ArcaneServer** wires it to its local admin console (stdin) for development.
- **Aphelyon's services** wire it to the existing HMAC-signed internal RPC (`ServiceEndpoint`), with the audit sink writing the existing `audit_log`. That is an Aphelyon-side task in the plan's last tranche.
- There is no remote UI. A remote UI is out of scope, as in the cvar spec's s10.

---

## 10. The audit sweep

### 10.1 The inventory
The audit's first deliverable is `docs/superpowers/audits/2026-10-xx-settings-inventory.md`: every hard-coded tunable candidate, with:
- `file:line`;
- the current value and unit;
- a verdict: **SETTING**, **CONSTANT** (a true invariant, format constant, protocol number or math identity) or **DERIVED** (computed from other settings);
- for SETTING rows: the proposed name, category, struct, audience, scope, apply mode, range, and whether it is `Deterministic`.

**Sources:**
- all ~575 numeric `constexpr`s in ArcaneCore, ArcaneClient, ArcaneEditor, ArcaneRuntime and ArcaneServer;
- the 31 theme tokens;
- the ~40 key checks;
- `ViewportSettings`, `UndoSettings` and `TextureImportSettings`;
- `HostConfig` fields;
- library config structs (Astra, Manifold2D, Mosaic, NRI device creation, enkiTS).

**Classification rules** (so the audit is mechanical, not taste):
- **CONSTANT:**
  - a value fixed by a file format, wire protocol, shader contract, ABI or the hardware;
  - a mathematical identity;
  - a test-only value;
  - a value whose change would be a bug, not a preference.
- **SETTING:** a value a reasonable developer or player might want different, including budgets, caps, timeouts, sizes, speeds, colours, thresholds, counts and toggles.
- **When in doubt: SETTING with `Dev`**, so it is reachable in development and not shown to players.
- **OTHER-STORE:** a real preference that is not a cvar, because it lives in another process or store (the Hub's `settings.archub`), in shipped template assets, or in an environment rung.
- **Capacity hints** (reserve sizes, I/O chunk sizes): CONSTANT with an `ARC_CONSTANT` marker. They have no observable preference.

**Diagnostics policy (inventory R2):** evidence capture (DRED, the crash handler, the hang watchdog) cannot be turned off in a shipped build. Its switches are `Dev`, never archived, and command-line only. The reporter UI (`diagnostics.spawnReporter`) is an ordinary Game setting.

### 10.2 Conversion rules
- **Defaults are byte-identical**, so a converted value has the same type and value. The whole suite, the goldens and the trajectory fixture must be unchanged by the sweep. A golden diff during the sweep is a conversion bug, not a re-bless.
- **Consumers read from snapshots:** a hot path reads `Settings<T>()` or a `CVarRef` once per frame or step, never a string lookup.
- **The stray stores migrate:**
  - `ViewportSettings` moves out of `imgui.ini` into `editor.viewport.*` cvars. On first run it imports the old `[EditorViewport][Camera]` block once and then stops writing it. The camera pose stays layout-like state in the ini; the preferences move.
  - `UndoSettings` folds into the existing `editor.undo.*`.
  - `TextureImportSettings` defaults become `assets.import.texture.*`. Per-asset import overrides stay in the asset's `.meta`.
- **Every converted constant leaves no shadow copy.** The `constexpr` is deleted, and a grep sweep proves it (the zero-legacy grep method).

### 10.3 After the arc (form (a), decided 2026-10-03)
The project rule stands: a new tunable is a setting. How it is enforced is decided with the user once they have read the inventory's "Should NOT be exposed" findings. Two candidates:
- **(a) Classification, not conversion.** A new numeric constant must be either a setting or carry an `ARC_CONSTANT("<reason>")` marker (a comment macro), and the guard fails only on unmarked ones. Real constants stay constants, with their reason written down.
- **(b) Report, not a failure.** The guard lists new unmarked constants in the gate report for review.

---

## 11. Persistence details

### 11.0 Well-known locations: the start of Arcane's file-system model
The user (2026-10-03): "we need to start thinking about the full file system".

Today every subsystem computes its own paths. Examples:
- `UserCVarDir` (`Runtime.cpp:480`);
- the diagnostics capture dir;
- Hub/editor recents;
- `Saved/UndoCache`, `Saved/Config` and thumbnails;
- the exe-dir `imgui.ini`.

This arc introduces ONE place that names every location: `Arcane::Paths`, in ArcaneCore. Settings, logs, caches, crash capture and layouts resolve through it, and a path cvar can override any user-writable location.

| Location | Editor / dev | Dist game | Writable |
|---|---|---|---|
| `EngineDir` / `EngineData` / `EngineConfig` | the engine checkout or SDK `data/` | the install folder | no |
| `ProjectDir` / `ProjectConfig` / `ProjectContent` | the project root, `Config/`, `Content/` | packaged with the game | Config: editor only |
| `ProjectSaved` | `<project>/Saved/` (UndoCache, Config, Layouts, Logs, Thumbnails) | not used | yes |
| `ProjectIntermediate` / `Cache` | `<project>/Intermediate/`, `Saved/Cache/` (cooked artifacts, DDC-like) | n/a | yes |
| `EditorUserDir` | `%LOCALAPPDATA%/Arcane/Editor/` (Config = the EditorUser rung, Themes, Layouts, Fonts, recents) | n/a | yes |
| `GameUserDir` | `<project>/Saved/` (dev runs) | `%LOCALAPPDATA%/<Company>/<Game>/` or `$XDG_CONFIG_HOME/<company>/<game>/` (Config, Saves, Logs, Screenshots) | yes |
| `DiagnosticsDir` | `<exe dir>/diagnostics` today; moves under `ProjectSaved/Diagnostics` or `GameUserDir/Diagnostics` | `GameUserDir/Diagnostics` | yes |
| `TempDir` | the OS temp `/Arcane/<pid>` | the same | yes |

- **Linux paths** follow XDG (`$XDG_CONFIG_HOME`, `$XDG_DATA_HOME`, `$XDG_CACHE_HOME`) and are named now, so the Linux port inherits them.
- **Migration:** each subsystem that computes a path today moves to `Paths` in the S1/S2 tranches, with tests pinning every location per build type.
- **Packaging stays out of scope:** a full virtual file system (mount points, pak archives, content streaming) needs its own spec. `Paths` is the foundation it will build on, and the asset system's existing mounts stay as they are.

### 11.1 Rung locations
| Rung | Editor / dev | Dist game |
|---|---|---|
| Engine | `data/EngineConfig/*.json` | the same, shipped |
| Plugin | `<plugin>/Config/*.json` | the same |
| Project | `<project>/Config/*.json` (committed) | shipped with the game |
| User | `<project>/Saved/Config/*.json` | per-user OS dir (s8.2) |
| Command line / Code / Console | not persisted | not persisted |

Editor Preferences values are per user and per project (`<project>/Saved/Config`). Machine-wide preferences, shared by every project on the machine, live under a new **EditorUser** rung between Project and User: `%LOCALAPPDATA%/Arcane/Editor/Config`. Theme, fonts, shortcuts and layouts default to EditorUser, so they follow the user across projects. A project-specific user value still overrides them through the User rung.

### 11.2 Migration
- Old names resolve through aliases (s4.7).
- An `Enum` file value given as a number is accepted and rewritten as its name on the next save.
- A corrupt file is kept aside as `.bad`. That behaviour is unchanged (5b6bc440).

---

## 12. Errors and edge cases
- **An out-of-range value** from a file, the console or the window is clamped, and a warning names the setting, the value and the range. The window cannot produce one.
- **A type mismatch** (`"substeps": "four"`) is refused for that key, and a Problems entry names the file:key.
- **A Restart setting edited, then reverted before restarting:** the bar disappears, because the window compares against the value captured at boot, not against "was edited".
- **A game module unloads while its category is open in the window:** the page shows "Module unloaded", then repopulates on reload (s4.4).
- **Two windows editing the same cvar** (Preferences and Project) is impossible, because a cvar has one home scope. Overrides are shown, not edited, in the other window (s6.4).
- **A theme preset leaves text below 4.5:1 contrast:** allowed (the user's choice), with a warning on the theme page.
- **A shortcut chord that the OS or ImGui reserves** (Alt+F4, Ctrl+Tab for window switching) is refused with a reason.
- **A Dist build** sees a `Dev` setting in a user file: it is ignored silently, because Dev is compiled out, and nothing is written back.

---

## 13. Testing
- **Core:**
  - a round-trip of all types, including `Enum`, `Color` and `Vec*`, through JSON, the console and `--set`;
  - history replacement per (rung, source);
  - `Register` refusals;
  - the cheat revert firing callbacks;
  - re-entrant callback dispatch;
  - `CommandResult`;
  - aliases (an old name resolves and warns once, and a save writes the new name);
  - snapshot reads from N worker threads during publish (TSan-style stress under the jobs system, with no torn values).
- **Module lifetime:** with the HotReloadPlugin fixtures, a module's cvars are registered, then unregistered on unload (handles stale, callbacks gone), then re-registered on reload with config values re-applied, and a user value survives a hot reload.
- **Settings structs:**
  - field -> cvar mapping and attribute -> metadata;
  - a static_assert on unmappable fields (a compile-fail test in the build guard);
  - `Settings<T>()` returns the published values;
  - the bindings: Astra and Manifold2D field-by-field `ToXConfig` tests.
- **Audience, context and policy:**
  - a matrix test of every audience x context (Editor/LocalHost/ServerAdmin/Client) x cheats on/off x Dist/Dev (Dist simulated through the existing `devCvars` switch);
  - policy tests: Allow, Deny and Default; a policy can widen `Game`/`Server` but never reach `Protected` reads or `Dev` in Dist;
  - the `server.cheats` revert;
  - audit-sink records for every non-Editor `Server` set.
- **Windows (headless ImGui harness, as with the existing panel harnesses):**
  - the tree from category paths;
  - search hits and the Modified/Overridden filters;
  - the reset arrow;
  - the provenance marker plus Clear override;
  - window undo;
  - the Restart bar.
- **Rich pages:**
  - theme preset apply and export/import round-trip, with the contrast warning;
  - shortcut listen/cancel/clear, conflict detection, and menu chord text following the binding;
  - the font/scale change rebuilding the atlas once;
  - named layout save/load/default.
- **Sweep:**
  - the full suite, the goldens and the `[trajectory]` fixture are unchanged after every sweep tranche;
  - a grep guard shows no deleted constant reappears;
  - the new-constexpr guard (s10.3).
- **Desk by automation** (screenshots plus a PASS/FIXED/OPEN table, no SendInput): both windows at 1920x1080, the theme presets, and the shortcuts page with a conflict.

---

## 14. How it lands

### 14.1 Tranches (gated, subagent-driven, as in the node-page phase)
1. **S1 Core:** types (`Enum`, `Color`, `Vec*`), metadata, audience/scope/apply, declaration overloads plus nameable handles, module capture, `UnregisterModule` wiring plus `ApplyLayersFor`, history per (rung, source), the O4 fixes, `CommandResult`, the snapshot plus `PublishImmediate`, aliases, and the Problems surfacing of unknown keys. Gate.
2. **S2 Settings structs and bindings:** `ARC_SETTINGS` over reflection, the new attributes, `Settings<T>()`, the EditorUser rung (needed by every Pref-M row, so it lands before S4), the Astra and Manifold2D bindings (fixtures unchanged), and the jobs/render/log bindings for what already exists. Gate.
3. **S3 The windows:** the tree, search and filters, rows, provenance, window undo, the Restart flow, custom-page registration, Preferences wired to the menu, and Project Settings replacing the read-out. Gate (desk by automation).
4. **S4 The rich pages:** theme (presets, preview, contrast), the editor action registry plus the shortcuts page (the 40 key checks routed), fonts and scale, and layouts. Gate.
5. **S5 The audit:** finalize the inventory (first pass read-only on 2026-10-03, reconciled the same day). Steps, in order:
   1. apply the Reconciliation (one name per value, the scope vocabulary, the R2 policies) mechanically;
   2. **user review point:** the user's row-by-row corrections (s16.3);
   3. freeze the names. S6 converts only frozen rows.
6. **S6 The sweep:** conversions by subsystem (render, physics/sim, assets/cook, net, input, editor UX, host and diagnostics), each a task with "defaults identical" proven; the stray stores migrated; and the guard test. Gate (full suite, goldens and trajectory unchanged).
7. **S7 Players and server:** `PlayerSettings` List/Set, the Dist user directory, `RemoteCVarService` plus the ArcaneServer console, and the Aphelyon services wiring plus the audit sink (Aphelyon commit). Gate. Close.

### 14.2 Order and parallelism
S1 -> S2 -> (S3 || S5) -> S4 -> S6 -> S7. The audit (S5) needs only S1-S2's vocabulary, so it can run beside the window work.

### 14.3 ABI
- S1 changes `CVarValue`, `CVarDesc`, the macros and exported registry signatures. That is one bump at the S1 gate (51 -> 52).
- S2 adds the settings exports, which is a bump at the S2 gate unless it folds into S1's.
- Later tranches bump only if a gate finds a layout change. ReferenceProject and Aphelyon are restamped at each bump.

### 14.4 Never
Push; `git add -A`; touch the user's untracked files; SendInput or focus stealing during desk checks; re-bless a golden during the sweep.

### 14.5 What shipped (S7-CLOSE, 2026-10-07)
Every fact below was read from git, the gate commits and the gate reports. Nothing is pushed. The branch is `feat/settings-s7` (worktree `Arcane-settings-c`). It forked from `feat/settings` at the S1 gate (`79b1e1b4`) and merged `feat/settings` back at `f5b40c29`. `feat/settings` was cut from main `8926eecb`. The branch holds 242 commits that are not on main (231 not counting merges). Main has 10 commits the branch lacks, all docs.

**Plugin ABI 54.** There were three bumps, as ruling I1 set out:
- 51 -> 52 at `e2b476b8` (S1 gate);
- 52 -> 53 at `f17bcfe7` (S2-13);
- 53 -> 54 at `93d14080` (S7-9: `ProjectManifest` gains `company`).

S7-SEC folded its exported-layout changes into the unreleased 54 instead of bumping again: `CVarApplyReport`, `CVarConfigIssue` and `LayerSources` grew, and new exports were added. At the close, ReferenceProject and Aphelyon were both rebuilt (`arcbuild rebuild`) against the final headers in Release, Dist and Debug. ArcaneServer loads each one with `engineAbi` 54 and no mismatch.

Every gate ran its suites from the exe directory, with a JSON report and the seed recorded. "Goldens 14/14" means `golden-gate.ps1` passed every lane at diffCount 0.

- **S0, the prelude.**
  - `4c71f05c`: one `ARC_` macro prefix.
  - `edfa164b`: drops the rename script.
  - Aphelyon `f155236`.
- **S1, the core.**
  - Commits: `83c3f7c7`..`7f654da0` on `feat/settings`. The Paths lane (`feat/settings-paths`) was cherry-picked as `3b845be9`..`95e53615`. The gate is `e2b476b8`..`79b1e1b4`.
  - ABI 52.
  - Seeds: Debug `~[gpu]` 355381039 (witness 185712660); Release 1817843884 (witness 1727486448). 2793 cases, 77763 assertions.
  - Goldens 14/14 in both configs, with no re-bless.
  - Aphelyon `3ca3bcc` (ABI 52).
- **S2, settings structs and bindings.**
  - Commits:
    - `bf606f25`..`4469f2b4`: `ARC_SETTINGS`, the typed snapshot blocks, the EditorUser rung and the All projects / This project API;
    - `4998f81b`..`ded67192`: the astra, physics, sim, jobs and log bindings, and `ApplyEarlyConfigRungs` (`c84be557`, ruling I2);
    - the gate, `ba06152a`, `dbcce566` and `56c04105`;
    - S2-H, `9eca053a`..`8b0ecdd8`: one engine-config folder and one project owner.
  - ABI 53.
  - Seeds: Release 3758890402, Debug 307039439; after the fix round, Release 1406201 and Debug 1406202 (2901 cases, 78950 assertions).
  - Goldens 14/14 in both configs.
  - Aphelyon `c9fa80d` (ABI 53).
- **S3, the windows.**
  - Commits: `40683350`..`80531aea`, plus the side lanes `s3q`, `s3r` and `s3s`, merged. The gate is `aab220d0` and `7e811c2f`.
  - Seeds: Release 1854519789, Debug 4249257802 (2876 cases, 78708 assertions).
  - Witness E11 added.
  - Goldens 14/14 in both configs.
  - The one re-bless in the whole arc is here: `304e4e57`. It re-blessed five editor goldens for main's WindowPadding 8 -> 4, which came in with the main merge `ff04b00f`. It is not a settings pixel change.
- **S4, the rich pages.**
  - Commits: the lane `3e6fb70e`..`0c4c7d7e`, merged at `04b5f442`. The gate is `c1afa75f`, `c29dc902`, `9781ab36` and `a7a79f41`.
  - Seeds: Debug 3951359785, Release 2423765022 (2994 cases, 80868 assertions). Witnesses: Debug 3317819103, Release 3369414156.
  - Witness E12 added. The S6 gate strengthened it with the verify report schema 14 `cvarSets` (`8f9b5173`).
  - Goldens 14/14 in both configs, with no re-bless.
- **S5, the audit.**
  - S5-1: `7335a4c8` and `3bab7620`.
  - S5-2: `d99a5db0`, frozen after the user's review, and `f1261acf`, 531 frozen names.
  - The S6 sweep's names bring `scripts/settings-frozen-names.txt` to 573 at the tip.
- **S6, the sweep.**
  - Commits: the lanes `s6` (`d99a5db0`..`34ff9ff0`), `s6b` (`dc354ce7`..`0db67819`) and `s6c` (`1accaeed`..`bad65fca`), merged at `30914dce`, `e81e8742` and `7129c062`. The follow-ups are `27741384`..`63499750`. The gate is `3c4a49bc`..`19716cf2`.
  - Seeds: Release 3173736889, Debug 2799223410 (3204 cases, 84602 assertions). `[trajectory]` is unchanged and `[sweep]` passed 196/196.
  - `fe34c93f` fixed a 186 px editor-asset-page diff in code, not by re-blessing.
  - The GPU half of this gate ran at the S7 gate.
- **S7, players and server.** Lane C ran these commits:
  - `73adf9d2`..`254aefde`, the S7 tasks;
  - the merge `f5b40c29`;
  - `93d14080`, ABI 54;
  - `d5911806` and `a9191c72`, the Dist path fixes;
  - `53fdb674`, the stdin BOM fix;
  - the gate, `ac8b222d` and `8dc46e12`;
  - S7-DIST, `2cb7f71f` and `f2914d95`;
  - S7-SEC, `40eaf8f5` and `512c9adc`;
  - the close verification, `79c3f14a`.

  Gate results:
  - Seeds: Release `~[gpu]` 3600637913 (witness 2563675766); Debug 215659823 (witness 745888366). 3240 cases, 85066 assertions.
  - The GPU half: Release `[gpu]` 1796971354 (witness 2796278738) and Debug 40181528 (witness 3524994855), each 71 cases and 65933 assertions. Each witness pass is 25 cases with 1 desk-only skip (G1). Goldens 14/14 in both configs.
  - The S6-19 R11G11B10F/D24S8 runs passed on dx12 and Vulkan.
  - The S6-8 server fixedDt census passed.

  Dist and later runs:
  - Full Dist `~[gpu]` was made green and hermetic by S7-DIST: seeds 3084954678 and 4276287535.
  - S7-SEC: Debug 914769167 (fix round 4097496357), Release 3269652200, Dist 2693378780.
  - The close verification at `512c9adc`:
    - Release 3794975143: 3261 cases + 4 skips, 85636 assertions;
    - Dist 2599648587: 3219 cases + 36 skips, 84400 assertions;
    - Debug 1754483335: 3261 cases + 4 skips, 85636 assertions.
    - `%LOCALAPPDATA%` was identical before and after each run.

  Witnesses added: PS-W1 (`PlayerSettingsWitnessTest`) and server S4 (`ServerWitnessTest`, the stdin admin console).
- **S7 details the text above does not name:**
  - `PlayerSettings` follows `EngineContext::netMode` through `PluginHost::RefreshContext`, using the primary world's mode. A dedicated server's `Set` is Denied.
  - `CVarListEntryEx` / `CVarRegistry::ListEx` give the whole descriptor per listed cvar.
  - `.arcproj` gained an optional identity field, `company`.
  - `Paths::ResolveGameUserDir` / `SanitizePathSegment` put the Dist user dir at `%LOCALAPPDATA%/<company>/<game>`, or the XDG equivalent.
  - `RemoteCVarService` audits every set and command attempt through its own sink. Protected cvars are unreadable on every transport.
  - ArcaneServer's stdin admin console:
    - `--no-admin-console` turns it off;
    - the registry publishes once per tick;
    - a dedicated host sets `server.cheatsAllowed=false` at the Project rung;
    - a leading UTF-8 BOM is ignored.
  - ReferenceGame's mods policy: `game.mods.enabled` (PlayerSafe) lets the local host change non-cheat Game and Server settings. The policy is cleared at module unload.
  - Aphelyon's `CVarRpc` serves Auth, Account and Combat with an operator caller allow-list. `audit_log.account_id` is nullable, for system rows.
  - S7-SEC's honoured sources are a deliberate deviation from its brief. A `LaunchesProgram` value is honoured from the Default, EditorUser, CommandLine and Code rungs, and from a Console in the Editor context. Consoles in every other context are Denied (s18). **The invariant that must hold:** no data-driven path may ever call `Set` or `Execute` at `SetBy::Code`, or at `SetBy::Console` in the Editor context. That covers project files, scripts, graphs and replication.
- **Aphelyon (`main`, not pushed).** These commits, oldest first:
  - `f155236` refactor(game): ARC_ macro prefix;
  - `3ca3bcc` chore(game): engine.abi 52;
  - `16de714` feat(services): expose audited CVar RPC on signed endpoint;
  - `f2100af` fix(services): authorize trimmed CVar set operations;
  - `f1ab400` feat(account): audit_log accepts system rows;
  - `59f0d06` feat(services): expose admin CVar RPC and audit Account changes;
  - `e0c3271` fix(services): document CVar gate caller configuration;
  - `c9fa80d` chore(project): restamp Aphelyon to engine ABI 53;
  - `1744466` fix(services): build against the settings-arc SDK (net.* caps);
  - `f953ad1` docs(services): the PBKDF2 round count is `crypto.pbkdf2Iterations`;
  - `01b873a` chore(game): engine.abi 54;
  - `13bf247` test(services): the hidden `[live-cvar-rpc]` case starts Winsock itself.

  Aphelyon's main builds only against this branch's SDK until the branch merges. Results at the S7 gate:
  - Services: CommonTests, AuthTests and CombatTests passed in both configs.
  - AccountTests passed in full on the ephemeral CI DB, `-p aphelyon_ci`: 197 cases, seed 479914898.
  - The `[live-cvar-rpc]` live smoke passed (seed 1784810102), and it wrote two `rpc:s7-gate` audit rows with `account_id` NULL.
  - The S7-A3 "hang" was a long, silent rapidcheck property, not a hang.
- **The dev-DB `ALTER`** (`audit_log.account_id DROP NOT NULL`) was applied on 2026-10-05 (S7-A2), on the user's "Yes, run it". It is non-destructive, and the 21 accounts are intact.
- **Owed.** These are copied from the gate reports and the controller's close notes:
  - **Dev test root.** Dev-build test runs still use the real `%LOCALAPPDATA%\Arcane` (EditorUserDir, UserRoot). The hermetic root is Dist-only.
  - **arcbuild configs.** arcbuild cannot tell Release from Dist, because they share a CRT. Flip between them with `rebuild`.
  - **Dist `--compare`.** It writes no diff artifact: DiffArtifactPath is refused, because Dist has no ProjectSaved. Where it should write is undecided.
  - **Crypto fallback.** `Crypto.hpp`'s Windows branch falls back to `std::random_device` when `BCryptGenRandom` fails. Make it fail-closed, like the POSIX branch, and find the cause. The close runs log it in the case "crypto: GenerateRandomBytes returns the requested count with variation", probably from its count-0 draw.
  - **Per-service credentials.** Aphelyon needs per-service HMAC keys, or a key-to-identity map, before any non-loopback deployment. Today the CVar RPC caller name is attribution, not authentication.
  - **Unknown-key noise.** A non-editor host reports the editor's Pref-P keys from `Saved/Config/editor.json` as `config.cvar.unknown-key`. The possible fix is for a non-editor host to skip that file.
  - **Desk checks.** The S6 gate's desk checks 1-5 (headless HUD, clearColor, a Project-rung window size, the viewport-ini import, the legacy `.arcproj` migration) are not recorded as run in any gate report. S4's runtime font-reinstall pixel check is still open.
  - **Descriptor consolidation.** `CVarListEntryEx`, `CVarDescInfo` and `CVarMetadata` share one descriptor tail and should be consolidated.
  - **Astra settings pass.** This includes `astra.snapshot.compression`, which is Dev and behind Show advanced; making it user-facing is the user's call.
  - **Two controller items:** `IMGUI_USE_WCHAR32`, and `console.enabled` for Dist builds.
  - **Linux.** `Module::MappedModules` / `PinMapped` are Windows-only.
  - **Known GPU-log noise.** The Vulkan "vertex attribute at location 1 not consumed" warning and the D3D12 debug-layer and refcount lines are pre-existing.
  - **PowerShell capture noise.** PowerShell-captured logs carry NativeCommandError wrappers.
  - **Deferred minors.** The 136 `minor (deferred` lines in the arc's ledger.

---

## 15. Risks
- **The sweep's size.** Hundreds of conversions risk behaviour drift. *Mitigation:* identical defaults, per-subsystem tasks, and goldens plus trajectory as the oracle after each task.
- **Hot-path cost.** A careless conversion could do a string lookup per entity. *Mitigation:* `CVarRef`/`Settings<T>()` only, and a review-checklist item; a micro-benchmark on the physics step and render submission.
- **Reflection coupling.** Settings structs depend on Astra reflection. *Mitigation:* Astra stays the reflection provider (the binding direction is unchanged); Arcane-only attributes live in Arcane.
- **Shortcut migration.** Moving 40 key checks risks regressions in focus and context handling (text fields must swallow keys). *Mitigation:* each context has a `WantsKeyboard` gate, as today, plus a test per migrated action.
- **The EditorUser rung** is a new rung in the ladder. *Mitigation:* numbered between Project (30) and User (40), at 35; the ladder had gaps for exactly this.

## 16. Decisions (the user, 2026-10-03, at the spec review)
1. **Machine-wide editor preferences:** yes, via the EditorUser rung. Per-project overrides must be EASY: a one-click "All projects / This project" switch on every Preferences row (s3.3).
2. **Dist save location:** yes, the per-user OS dir. "We need to start thinking about the full file system", so `Arcane::Paths` was added (s11.0).
3. **The audit review:** yes, the user reviews the SETTING list. The inventory was run early, read-only, on 2026-10-03 (`docs/superpowers/audits/2026-10-03-settings-inventory.md`), and S5 becomes "finalize the inventory with the user's corrections".
4. **Theme presets:** yes: Dark, Light and High Contrast.
5. **Shortcut conflicts:** allowed and shown red on BOTH (every) conflicting row, with a tooltip naming the exact conflict (s7.2).
6. **Server surface:** yes, both: the engine seam plus the ArcaneServer console, and Aphelyon's services wiring in S7.
7. **Project identity fields** stay in `.arcproj` (the Project page).
8. **The new-constant guard (s10.3): form (a), the `ARC_CONSTANT("why")` marker.** The user asked: "Is there anything today that shouldn't be exposed?" The inventory answered yes (12 categories), so the guard does not fail on every constant. It fails only on a new numeric constant that is neither a setting nor marked with its reason.
9. **Server settings in player contexts (user question, 2026-10-03):** the original draft was too restrictive (`Server` was absent from every player surface). It is replaced by audience x context with a game-installable `CVarPolicy` (s3.2): a single-player game or listen-server host sets `Server` settings like Source's local `sv_*`, cheat ones behind `server.cheats`, and games widen or narrow access themselves.

10. **The inventory's proposals (Reconciliation R1-R4): all as proposed** (user, 2026-10-03).
11. **One UI-scale setting:** yes. About 60 hard pixel sizes become DERIVED from `editor.ui.scale` x their base size (and the font size where text-relative), not about 100 individual Dev cvars.
12. **Shortcut key type:** labelled by default, physical for positional clusters, after research (s7.2).

## 17. Out of scope
- A remote console UI.
- `exec`.
- Scalability groups and device profiles. These are a later layer: named bundles of `PlayerSafe` render settings, built on this system, with no new store.
- Per-platform overrides. The rung ladder makes them a later insertion, not a redesign.
- A settings asset referenced from content, the spec's GUID revisit trigger. It is not triggered here: windows bind by name.

## 18. Amendments

| Date | Task | Amends | Change |
|---|---|---|---|
| 2026-10-07 | S7-SEC (controller, security) | s3.3, s4.8, s11.1, s12 | **Project config never chooses a program to run.** New flag `CVarFlags::LaunchesProgram` for a setting whose value names a program or command line Arcane executes (`build.premakePath`, `build.msbuildPath`, `build.makePath`, `build.ninjaPath`, `build.ideExecutable`, `diagnostics.reporterPath`). Its value is honoured only from the default, the machine-wide EditorUser rung, `--set`, code, and a console in the Editor context. The EngineConfig, Plugin, Project and User rungs refuse it, and so does any rung folder inside the project or a plugin root, whatever its rung. A `PreferencesMachine` setting is refused from the Project and Plugin rungs (it is the user's; its per-project override stays the User rung). `PreferencesProject` is unchanged: the project may suggest it. A refused key is not applied and is reported once per key per load as the Problems row and log warning `config.cvar.refused`, naming the file, the key and the reason ("names a program; set it in Preferences (machine) or with --set"). The registry's `Set` and `SetRung` refuse the same rungs as a backstop. The Preferences window writes a `LaunchesProgram` row to EditorUser only and does not offer "This project" (`SetPreferenceTarget` returns Denied). Every launch site checks the program it is handed (`CheckLaunchPath`: it exists, is not a directory, and holds no quote or line break; an app-execution alias counts); each `CreateProcessW` site passes it as `lpApplicationName` and quotes arguments with the one Windows quoting helper (`QuoteWindowsArg`; arcbuild keeps its own equivalent), and the editor's module-build `cmd.exe` line (`_wpopen`) refuses a path holding a quote or a line break, and a command or configuration token that is not one word. A sweep test fails on any path setting not classified by the launch-site audit. Fix round 1: the legacy `.arcproj` block's warning names the manifest file; arccook's project Config read warns once per refused key, naming the folder; the crash monitor's line quotes the exe and the session path with `QuoteWindowsArg`, and because the crash thread may not allocate, a `diagnostics.dumpDir` holding a quote or a line break is refused for the default report dir at Install/`RetargetDumpDir` (with a warning) and the crash thread skips the reporter spawn for a report stem holding one. |
