# Arcane Project Format — Design

**Date:** 2026-07-22
**Status:** Design (approved in brainstorm; pending written-spec review)
**Scope:** Define the on-disk format, identity, content-addressing, config, plugin, and
build model for an *Arcane project* — the unit the Arcane Editor opens and the runtime
launches. Implemented in slices; this document is the whole target.

---

## 1. Motivation

Arcane has no project concept today. The game is a DLL loaded by an explicit path
(`--plugin <path>`), assets are read from a `data/` folder next to the executable, and
there is no notion of "open this project." Concretely:

- **Game code** = a plugin DLL (`Sandbox.dll`, `PlaygroundGame.dll`) loaded by the
  ABI-versioned plugin host by path.
- **Assets** = the `Arcane/Assets/` facade (`Assets.{hpp,cpp}` + `AssetCache.hpp`), which
  loads textures/bytes/JSON keyed by the **resolved filesystem path** (`CacheKey(resolved)`);
  refcount + memoised failures + byte-budget LRU. References are physical paths.
- **Scenes** = reflection -> JSON (`Serialization/SceneSerializer.hpp`).
- **Config** = ad-hoc flat JSON next to the exe (`data/input_actions.json`, etc.).

This is fine for a single hard-wired demo host but cannot express a shippable game: no
engine/ABI binding, no content root, no portable references, no per-project settings, no
project discovery. This spec defines that missing layer.

**Directional constraint (user directive, 2026-07-22):** Arcane is moving *away* from a
text-first ethos toward a **fully ID/GUID-based** asset model, and eventually toward
binary, editor-authored content packages. The design below is ID-first: references resolve
to a stable GUID, not a path. (See `memory: project_arcane_asset_id_direction`.)

---

## 2. Research basis — Unity vs Unreal, and Arcane's position

Two reference models were studied (see the appendix for the full comparison and sources):

| Axis | Unity | Unreal | **Arcane** |
|---|---|---|---|
| Project identity | Folder-is-project (no project file) | Explicit `.uproject` manifest | **Explicit `.arcproj` manifest** |
| Code model | C# scripts in `Assets/`, editor-compiled | Native C++ modules -> DLLs | **Native game-as-DLL** (already so) |
| Asset identity | Stable **GUID** in sidecar `.meta` | **Path-as-identity** + redirectors | **GUID identity** (Unity's model) |
| Content addressing | Folder paths under `Assets/` | Virtual **mount points** (`/Game/`) | **Mount points** (`game://`) (Unreal's model) |
| Serialization | Text YAML (VCS concession) | Binary `.uasset` (+ LFS) | JSON now -> **binary/ID** later |
| Config | Per-category YAML, user split out | Layered `.ini` | **Layered JSON** |
| Source/derived split | Commit source; ignore caches | same | **same** |

**Arcane is a deliberate hybrid.** Its architecture already picks a side per axis and the
choice splits cleanly:

- **Code side -> Unreal.** Native game-as-DLL + ABI-versioned plugin host demand an
  *explicit manifest* read before load (engine binding, which DLL is the game, which
  plugins to mount). Folder-is-project cannot express that.
- **Content side -> Unity identity on Unreal addressing.** GUID identity (Unity) gives
  free rename/move with zero redirectors; virtual mount points (Unreal) decouple logical
  address from filesystem location. Arcane takes **Unreal's mounts for *addressing* and
  Unity's GUID for *identity*.**
- **Both agree -> adopt outright.** Commit the authored roots; gitignore every
  regenerable cache. Arcane already has this instinct (`shaders/generated/`, `bin/`).

---

## 3. Design principles

1. **Explicit over emergent.** A project is declared by a manifest, not inferred from a
   folder shape. Identity, engine binding, and module/plugin declarations are data read
   before anything loads.
2. **Source/derived split is the master pattern.** Commit `{manifest, Source, Content,
   Config, Plugins}`; gitignore everything regenerable (`Binaries, Intermediate, Saved`).
3. **ID-first, seam-early.** All asset references resolve through a seam whose `AssetId`
   is GUID-backed at the destination. Build the seam now; the GUID subsystem plugs in
   without touching call sites.
4. **Config stays text.** The one exception to "away from text-first": config is meant to
   be diffed, merged, and layered across contributors, so it stays JSON (even Unreal, fully
   binary for content, keeps config text).
5. **The game is the primary plugin.** The game DLL is loaded by the *same*
   ABI-versioned plugin host that already loads `Sandbox.dll`. Unifies game-as-DLL with
   plugins.
6. **One-way dependency.** project -> plugin -> engine, never reverse — the modular-stack
   / editor-free rule (`memory: project_modular_tool_stack`).

---

## 4. Decision #1 — Project identity: explicit `.arcproj` manifest

A project is a **self-contained folder** the editor opens and the runtime launches
against, anchored by a JSON manifest **`<Name>.arcproj`**. This replaces "`data/` next to
the exe."

Rationale: game-as-DLL demands declarative metadata read *before* load (engine/ABI binding,
which DLL is the game, which plugins to mount); ABI/version binding must be able to refuse
an incompatible engine; discovery is trivial (find the manifest), OS double-click can open
it, and a `formatVersion` lets the schema evolve. Unity's one counter-argument (avoid a
central merge-hotspot file) does not bite — this manifest is small and rarely edited.

### Manifest schema (`<Name>.arcproj`)

```jsonc
{
  "formatVersion": 1,                 // schema version -> migratable
  "name": "Aphelyon",
  "description": "",
  "engine": { "abi": 4 },             // engine/ABI binding; host refuses incompatible ABI
  "gameModule": "Aphelyon.dll",       // the project's primary module (game-as-DLL)
  "plugins": [                        // enabled plugins, .uproject-style
    { "name": "Sandbox", "enabled": false }
  ],
  "contentRoots": [                   // OPTIONAL extra mounts beyond the defaults (S4/S1)
    // { "mount": "shared", "path": "../Shared/Content" }
  ],
  "bootScene": "game://scenes/main.ascene"   // where the runtime starts
}
```

`formatVersion` and `engine.abi` are required. Everything else has sane defaults (a
content-only project may omit `gameModule`).

---

## 5. Decision #2 — On-disk folder skeleton

```
Aphelyon/                     <- project root (opened by the Arcane Editor)
├─ Aphelyon.arcproj           manifest / identity                    [commit]
├─ Source/                    C++ game module -> Aphelyon.dll         [commit]
├─ Content/                   assets: scenes, sprites, prefabs...     [commit]  -> game:// mount
├─ Config/                    project settings (layered JSON)         [commit]
├─ Plugins/                   project-local plugins (fractal)         [commit]
├─ Binaries/                  built DLL/exe output                    [ignore]
├─ Intermediate/              build byproducts, generated proj files  [ignore]
├─ Saved/                     logs, autosaves, flattened config, import/derived cache [ignore]
└─ .gitignore
```

- **`Content/`** (not `data/`) is the `game://` mount root — the name should read
  `game://...` and match the Unreal-style content model we are adopting.
- **`Source/`** holds the C++ game module *with* the project (self-contained, Unreal-style),
  built against the engine SDK (Decision #6).
- **`Binaries/ Intermediate/ Saved/`** are the disposable derived tree — the same set
  Unreal gitignores. `Saved/` also holds the import/derived-data cache (the Unity `Library/`
  <-> Unreal `DDC` analog) until it warrants its own `DerivedData/`.

### `.gitignore` (project template)

```gitignore
# Derived / generated — never commit
Binaries/
Intermediate/
Saved/

# Plugin generated output
Plugins/**/Binaries/
Plugins/**/Intermediate/

# Generated IDE project files
*.sln
*.vcxproj
*.vcxproj.filters
*.vcxproj.user
.vs/
```

Committed: `<Name>.arcproj`, `Source/`, `Content/` (incl. `.meta` sidecars), `Config/`,
`Plugins/**/{Source,Content,Config}`.

---

## 6. Decision #3 — Content addressing: Unreal mounts + Unity identity

### 6.1 Mount points (addressing layer)

Virtual roots decouple logical address from filesystem location:

- `game://...` -> the project's `Content/`
- `engine://...` -> the engine SDK's built-in content
- `plugin://<name>/...` -> that plugin's `Content/` (auto-registered on mount)
- extra mounts may be declared in the manifest's `contentRoots`

A scene is `game://scenes/main.ascene` regardless of where the project sits on disk;
plugin content stays portable when the plugin moves. **References never name a physical
path.**

### 6.2 Identity — GUID, not path

Every asset carries a stable **GUID** (`Arcane::Guid`, §7). Stored references *between*
assets are the GUID, never the mount path — so rename/move survives with **zero
redirectors** (the whole win of going ID-based; it is why we do not adopt Unreal's
path-identity + redirector machinery).

Where the GUID physically lives splits by asset type:
- **Embedded in the asset file** for engine-owned formats — a top-level `id` field in the
  JSON scene/prefab *now* (content stays JSON through these slices, §12), migrating into the
  binary package (`.ascene` / `.aprefab` / `.aasset`) when content goes binary later. The
  id always travels *with* the asset it identifies.
- **Sidecar `.meta`** for imported external originals (`.png` / `.wav` / `.ttf`, which
  cannot hold an embedded id). This is Unity's `.meta`, scoped only to imported source
  assets rather than to everything.

### 6.3 The AssetRegistry

The **AssetRegistry** owns the one map that matters: `GUID -> mount path -> physical file`,
built by scanning the content tree (project `Content/` + mounted plugins + `engine://`) at
project open, reading each asset's embedded id or its `.meta`.

### 6.4 The resolver seam (built now, GUID-shaped)

Every load routes through:

```
AssetId  ->  AssetRegistry::Resolve(AssetId)  ->  mount path  ->  physical file  ->  Assets::Load
```

`AssetId` is an **opaque handle that IS a `Guid` at the destination**. Call sites hold an
`AssetId` and never see a path or a raw GUID. Today's path-keyed `Assets` facade sits
underneath the seam unchanged, which is what makes the eventual path->GUID swap invisible
to every consumer.

**Sequencing:** the mount layer (6.1) + the seam (6.4) are foundation. The GUID + `.meta`
+ AssetRegistry (6.2/6.3) are the first subsystem slice *after* the foundation. Author
minimal content before that slice lands so there is nothing to migrate.

---

## 7. `Arcane::Guid` — Core primitive

Arcane has **no** GUID/UUID facility today (verified 2026-07-22). It has XoshiroCpp (fast
PRNG) and `Arcane/Core/src/Arcane/Crypto/` (SHA/HMAC). We build:

- **Type:** 128-bit (`uint64 hi, lo`), trivially copyable, hashable (AssetRegistry map
  key), comparable, canonical `8-4-4-4-12` string parse/format, `Nil()` / `IsValid()`.
- **Generation:** RFC-4122 **UUIDv4 (random)** as the default asset id — a thread-safe
  generator seeded from a strong entropy source (`std::random_device` mixed with a
  high-resolution clock + thread id to dodge weak `random_device` implementations;
  optionally backed by the Crypto CSPRNG). 122 random bits => negligible collision risk at
  any asset count.
- **Optional UUIDv5 (name-based, deterministic):** hash `namespace + name` -> a
  reproducible GUID, for stable engine-built-in-content ids and deterministic re-import.
- **Placement:** Arcane Core, beside Crypto (zero game deps; promotable to Mosaic later).
- **Determinism:** authoring/import-time only. It is **never** in the deterministic sim
  path, so its non-determinism cannot affect physics.

---

## 8. Decision #4 — Config: layered JSON

Config is the one place "away from text-first" stops at the door: it must stay diffable,
mergeable, and layerable. It keeps Arcane's **JSON**.

- **Layered, later overrides earlier:**
  `engine defaults (shipped, read-only)` -> `project Config/*.json (committed, the
  authoring layer)` -> `user overrides (local, gitignored)` -> flattened result written to
  `Saved/Config/...` at runtime (derived, gitignored).
- **Per-category files:** `Config/engine.json`, `Config/game.json`, `Config/input.json`,
  ... (mirrors Unity's `ProjectSettings/` split and Unreal's categories). Today's
  `input_actions.json` folds in as `Config/input.json`.
- **Team vs user split:** shared `Config/` is committed; per-user overrides live outside it
  and are gitignored — keeps personal prefs out of shared VCS.
- **Merge:** later layer overrides by key, deep-merges objects. Unreal's `+`/`-`
  array-merge operators are a later nicety, not foundation.

---

## 9. Decision #5 — Plugins as fractal mini-projects

A plugin is a self-contained mini-project mirroring the project itself.

- Descriptor **`<Name>.arcplugin`** (same shape as `.arcproj`), with optional `Source/`
  (-> its DLL), `Content/` (-> the `plugin://<name>/` mount), and `Config/`.
- **Two homes:** *engine plugins* (shipped with the SDK, shared) vs *project plugins*
  (`<project>/Plugins/<name>/`, versioned with the project) — Unreal's split.
- **Discovery:** scan `.arcplugin` descriptors (engine plugin dir + the project's
  `Plugins/`), gated by the manifest's `plugins: [{name, enabled}]`.
- **Dependency direction is one-way:** project -> plugin -> engine, never reverse. Plugins
  may depend on other plugins; load-ordering reuses the existing plugin host's ordering.

---

## 10. Decision #6 — Engine-as-SDK build model

The game DLL is the project's **primary module**, loaded through the *same* ABI-versioned
plugin host that already loads `Sandbox.dll`. The SDK is what lets that DLL be **built
outside** the engine workspace.

**A. What the Arcane SDK is** (the installed engine a project targets):
- `include/` — the public header surface (the `ARCANE_API` set: Core + engine public)
- `lib/` — `Arcane.lib` (import lib for `Arcane.dll`)
- `bin/` — `Arcane.dll`, `ArcaneEditor.exe`, the runtime host, compiled shaders, engine
  built-in content (the `engine://` mount)
- `build/arcane.lua` — a premake module a project consumes (include/lib dirs, links, `/MD`,
  ABI defines)
- versioned by the plugin ABI Arcane already versions

**B. Engine discovery / version binding:**
- Manifest `engine.abi` -> resolve the installed SDK via the **`ARCANE_SDK`** env var
  (later: a registered-versions file for multiple installs, Unreal-style).
- **ABI gate:** the host refuses to open a project/module built against an incompatible
  ABI — reusing the existing plugin-host ABI check.

**C. Build integration (build rules as committed code):**
- The project's committed build definition is its **premake script** (the
  `Build.cs`/`Target.cs` analog): it `include`s the SDK's `arcane.lua` and defines the game
  module linking `Arcane.lib`, `/MD`.
- Generate -> `.vcxproj`/`.sln` in `Intermediate/` (gitignored throwaway views).
- Build -> `Aphelyon.dll` into `Binaries/`; `gameModule` points at it.

**D. Run / host (the unification):**
- The runtime host (or the editor in play mode) opens `.arcproj` -> binds ABI -> mounts
  content roots -> loads `gameModule` + enabled plugins through the existing host -> boots
  `bootScene`. The game is the primary plugin.

**Pragmatic scope:** define the SDK *layout* + `arcane.lua` + env-var discovery. A packaged
installer / multi-version registry is a later nicety — initially the "SDK" is the engine's
own build output consumed in-place via `ARCANE_SDK`. This is the heaviest slice and lands
**last**; the migration is: carve the engine's public surface into the SDK layout, add
`arcane.lua`, and prove it by building the first external project (the Aphelyon client)
against it.

---

## 11. Implementation slices (for the plan)

Each slice is independently landable and testable.

1. **Foundation.** `.arcproj` manifest schema + loader; on-disk skeleton; project
   discovery/open (editor + runtime open a project instead of `data/`-next-to-exe); mount
   points (addressing); the `AssetId` + resolver-seam scaffold (path-backed for now);
   `.gitignore` template.
2. **Identity.** `Arcane::Guid` Core primitive; AssetRegistry (`GUID -> mount -> physical`);
   `.meta` sidecars for imported originals; embedded ids for native packages; swap
   `AssetId` to GUID-backed behind the seam.
3. **Config.** Layered JSON config (engine defaults -> project `Config/` -> user ->
   flattened `Saved/`); fold `input_actions.json` into `Config/input.json`.
4. **Plugin content.** `.arcplugin` descriptors; `plugin://` mounts; per-plugin
   `Content/Config/Source`.
5. **Engine-as-SDK.** SDK layout; `arcane.lua` premake module; `ARCANE_SDK` discovery;
   build the first external project against it.

---

## 12. Non-goals / deferred

- **Binary content packages** (the endpoint of "away from text-first"): the AssetRegistry
  + embedded-id design *anticipates* it, but the actual binary serializer/importer for
  `.ascene`/`.aprefab`/`.aasset` is a separate future spec. Content stays JSON through
  these slices.
- **Packaged/installed SDK with a multi-version registry** — env-var-in-place first.
- **Array-merge config operators** (`+`/`-`) — later nicety.
- **Redirectors** — explicitly not adopted; GUID identity makes them unnecessary.
- **Cooking / packaging a shippable build** — out of scope; a later milestone.

---

## 13. Open questions (resolve at plan time)

1. **`AssetId` shape in transition.** Slice 1 ships a path-backed `AssetId`; Slice 2 swaps
   it to GUID-backed. Confirm no serialized content is authored between the two that would
   need migration (mitigation: author minimal content pre-Slice-2).
2. **`engine://` content source.** Does the engine ship built-in content now, or is
   `engine://` reserved but empty until there is engine content to mount?
3. **First project location.** Where does the Aphelyon client project physically live
   (a sibling of the engine repo, or inside it) once it builds against the SDK?

---

## Appendix — Unity vs Unreal reference detail

Sources consulted (2026-07-22):

**Unity** — folder-is-project; `Assets/` + `Packages/` + `ProjectSettings/` committed,
`Library/`/`Temp/` derived; per-asset `.meta` sidecar carrying a GUID + import settings;
GUID-based references (`{fileID, guid, type}`) for free rename/move; UnityYAML text
serialization as a VCS concession.
- default-directories, SpecialFolders, AssetMetadata, ExternalVersionControlSystemSupport,
  upm-manifestPrj, UnityYAML (docs.unity3d.com).

**Unreal** — explicit `.uproject` (FileVersion, EngineAssociation, Modules, Plugins);
`Config/Content/Source/Plugins` committed, `Binaries/Intermediate/Saved/DDC` derived;
path-as-identity + redirectors; virtual mount points (`/Game/`, `/Engine/`, `/Plugin/`);
`Build.cs`/`Target.cs` as committed build rules with generated IDE project files; layered
`.ini` config; plugins as fractal sub-projects on a one-way dependency DAG.
- unreal-engine-directory-structure, unreal-engine-modules, referencing-assets,
  asset-redirectors, configuration-files, plugins (dev.epicgames.com).
