# Arcane Editor: Scene Authoring

Date: 2026-07-26
Status: design approved, no implementation plan yet
Scope: scene session core + project integration (Sandbox migration and the
entity clipboard are explicitly OUT)

> **This document records the design as approved, not the code as shipped.**
> Where the two differ, the shipped code and the implementation plan are
> authoritative. The API names below have been corrected against the code;
> anything else here may still lag it.

---

## 1. Problem

The Arcane Editor cannot save what you build.

`Arcane/ArcaneEditor/src/EditorPanels.cpp:63-70` carries four dead menu items --
New Scene, Open Scene..., Save Scene, Save Scene As... -- that draw and do
nothing. Meanwhile every piece of plumbing they would need already exists and is
wired to nothing:

- `Arcane::Scene::SaveJson` / `LoadJson`
  (`Arcane/Arcane/src/Arcane/Serialization/SceneSerializer.hpp`) is versioned
  (`kSceneJsonVersion = 2`), reflection-driven over an arbitrary component
  roster, and persists both hierarchy and non-hierarchical links. Its only
  consumer is `Arcane/Tests/src/SceneJsonTest.cpp`.
- `ProjectManifest::bootScene` (`ProjectManifest.hpp:32`) is parsed
  (`ProjectManifest.cpp:44`), written as `""` by `Project::Create`
  (`Project.cpp:143`), and consumed by nothing.
- `AssetKind` (`ArcaneEditor/src/AssetBrowser.hpp`) has Material, Texture,
  Audio, Font, Data, Other. No Scene.

The consequence: an editing session is throwaway. You edit whatever the loaded
plugin constructed in `GamePlugin_Init` (e.g. `Arcane/Sandbox/src/Scenes.cpp`
spawns 39 entities in code), and it dies with the process. Nothing authored
persists, so no level design, no game content, and no Aphelyon rebuild can
start.

This is the "addressable gap" shape: the capability is present, only the wiring
is missing.

## 2. Decisions taken

**Data wins.** Opening or creating a scene replaces the registry wholesale.
Plugin `Init` is for registering components and systems; entity spawning from
code is legacy test scaffolding (Sandbox and the other code-only projects are
early tests, to be removed and eventually remade as authored examples once the
editor is ready). No merge semantics, no marker-scoped clearing, no
authored-vs-code distinction to maintain.

Critically, this breaks nothing today: *no scene loaded* means nothing clears.
A project with an empty `bootScene` and a spawning plugin behaves exactly as it
does now. Only New/Open Scene takes ownership of the registry.

**Extension: `.arcscene`**, consistent with `.arcmat`. The
`game://scenes/main.ascene` example in `ProjectManifest.hpp:32` is stale and
gets corrected.

**The scene is the session, not a document.** The Viewport *is* the scene view,
and `PlayMode.hpp` already declares the Edit-mode registry to be the authored
state ("edit-mode edits (the snapshot content) are the authored state that
survives a Play/Stop cycle"). A scene tab beside the Viewport would be two views
of one registry. Materials are `DocumentHost` documents because they are side
artifacts; the scene is the session. This is the UE/Unity shape.

## 3. Architecture

Scene load/save is not editor-only -- Loom and a future game runtime must boot a
scene too -- so the capability lands in the engine and the editor owns only the
session and its UI.

### 3.1 Engine (`Arcane.dll`)

**`Arcane/Scene/SceneAsset.hpp` / `.cpp`** -- the *file* layer over the existing
in-memory `SaveJson`/`LoadJson`:

- `bool SaveSceneFile(const std::filesystem::path&, const Astra::Registry&, const Guid& id, std::string* error)`
- `std::optional<SceneDocument> ReadSceneFile(const std::filesystem::path&, std::string* error)`
  and `bool ApplySceneDocument(const SceneDocument&, Astra::Registry&)`

(As shipped: the single `LoadSceneFile` this section originally listed was
replaced by the read/apply split described just below, which is what makes the
ordering requirement enforceable rather than merely documented.)

Validates the top-level `id` and `version`. Exception-free (engine rule):
returns `false` with a logged reason on IO failure, parse failure, version
mismatch, or a malformed id. Never partially populates a registry -- it parses
and validates fully before mutating, so a failed load leaves the caller's
registry untouched.

**Ordering requirement, and it is load-bearing:** every caller must read and
validate the file BEFORE destroying the current scene. A failed Open Scene must
leave the session exactly as it was -- current scene intact, dirty state intact
-- not drop the user into an empty registry with an error. Concretely: parse and
validate to a staged document, and only then `ResetRegistry()` and populate.
`HostBoot::BootScene` follows the same order; it just has nothing to preserve on
failure.

**`HostBoot::BootScene(Runtime&, const Project&)`** -- resolve the manifest's
boot scene Guid through `Project::ResolveAsset` to a physical file, then
`ReadSceneFile` -> `Runtime::ResetRegistry()` -> `ApplySceneDocument`. Must be called *after* the plugin
loads, so the plugin's reflected component types are registered and its
components deserialize rather than being silently dropped.

**`AssetRegistry.cpp:155`** -- add `.arcscene` to the native-JSON extension
list, which currently reads `if (ext == ".json" || ext == ".arcmat")`. Scenes
then get a minted, written-back Guid on scan and appear in the browser with no
further work.

**`Project::SetBootScene(const Guid&)`** -- new plumbing. Today `Manifest()`
returns a const reference and only `Project::Create` ever writes a `.arcproj`;
this adds a manifest mutate + rewrite path. Writes are atomic (temp file +
rename) so an interrupted write cannot corrupt the project file.

### 3.2 Editor (`ArcaneEditor`)

**`SceneSession`** (new) -- pure and headless-testable, following the
`ConsoleBuffer` / `DocumentHost` pattern where all state and transitions are
free of ImGui and only the draw call touches it:

- current file path, Guid, and display name (`Untitled` when never saved)
- `IsDirty()` / `MarkSaved()`
- an unsaved-changes confirm state machine -- one pending request at a time,
  resolved by Save / Discard / Cancel -- mirroring `DocumentHost`'s close flow
  rather than inventing a second vocabulary

(As shipped: `SceneSession` is pure STATE and performs no IO. The New / Open /
Save / Save As effects this section originally put on it live in the host, as
`EditorApp::DoNewScene` / `DoOpenScene` / `DoSaveScene`, which return `bool` and
leave the human-readable message in `EditorApp::m_sceneError`.)

ImGui touches only the menu wiring and the confirm/error modals.

## 4. File format and identity

```json
{ "id": "<guid>", "version": 2, "entities": [ ... ] }
```

`SaveJson` already emits `version` + `entities`; the file layer adds `id` at the
top level. That is exactly `AssetRegistry`'s native-JSON contract (top-level
`"id"`, minted and written back when missing or invalid), so a hand-dropped or
externally-authored `.arcscene` is adopted on the next scan for free.

Scenes live under `<project>/Content/scenes/` by default, and Save As may put
them anywhere the user picks. A scene saved *outside* the project's content root
is written to disk but cannot be registered -- `AssetRegistry::AddFile` rejects
files outside the content root -- so it gets no Guid and cannot become a boot
scene until it moves inside. The session reports this rather than failing the
save.

`bootScene` becomes **the Guid text**, not a mount path. This follows the
ID/GUID-first directive, and `AssetId` + `AssetRegistry::Resolve` +
`MountTable` already implement exactly this resolution -- the asset can move on
disk and the reference survives. The `ProjectManifest.hpp:32` comment is stale
on both the extension and the representation and gets fixed. Nothing consumes
the field today, so redefining it costs no compatibility.

## 5. Dirty tracking

Add to `CommandStack`:

```cpp
[[nodiscard]] std::uint64_t StateId() const noexcept;
```

A stamp identifying the *current* state: the id of the transaction on top of the
undo stack, `0` when the stack is empty. `CommandStack::Transaction`
(`CommandStack.hpp:103`) has no id field today and gains one; `m_nextId`
(`CommandStack.hpp:124`) is the existing monotonic generator and is reused, so
a committed stamp can never be re-minted.

- `SceneSession::MarkSaved()` records the current `StateId()`.
- `IsDirty()` is `stack.StateId() != saved`.
- **Undoing back to the save point goes clean again**, which a plain
  change-counter cannot express.
- If the saved transaction falls off the bottom of the stack's depth cap
  (`maxDepth`, default 100 -- `CommandStack.hpp:42`), its stamp becomes
  unreachable and the scene stays dirty. That is the safe direction, and is the
  same caveat Qt documents for `QUndoStack`'s clean state.
- `Clear()` (called on New and Open) yields `0`, and `MarkSaved()` then records
  `0`, so a freshly loaded scene is clean.

**Stated assumption: the CommandStack is a faithful proxy for authored change.**
This holds today -- the RunLoop is paused in Edit mode, and every Outliner,
Inspector and gizmo edit is bracketed through the stack. Anything that mutates
the registry outside the stack will not mark the scene dirty. This gets a
comment at the seam so the next person to add an unbracketed mutation path sees
the obligation.

**Save is disabled during Play.** The authored state is the pre-Play snapshot;
the live registry during Play is play-time mutation that `PlaySession::Stop`
exists to discard. Saving it would persist garbage. The menu item greys out with
a tooltip, matching UE greying out Save during PIE.

## 6. Project integration

- **Open Project**: after the plugin loads, resolve `bootScene` and load it. On
  any failure -- unresolvable Guid, missing file, bad version -- log the reason
  and continue with an empty scene. A broken boot scene must never block opening
  a project, because the editor is how you would fix it.
- **Asset Browser**: `AssetKind::Scene` with `.arcscene` classification and an
  icon. Double-click routes to *load into the session*, not through a
  `DocumentHost` factory -- the one asset kind that is not a document. The
  routing therefore goes through the session's unsaved-changes guard.
- **Context menu**: "Set as Boot Scene" -> `Project::SetBootScene`.
- **`SampleProject`** ships a real authored `Content/scenes/main.arcscene` and
  points `bootScene` at its Guid, so the whole loop is demonstrable from a clean
  checkout.

## 7. Guards and teardown

Unsaved-changes guards on: New Scene, Open Scene, Open Project, Asset Browser
double-click, and application exit. The Open Project path extends the existing
dirty-document refusal at `EditorApp.cpp:617` rather than adding a parallel one.

Scene load reuses `SwitchProject`'s existing teardown sequence -- clear
selection, reset outliner state, clear the undo stack -- because no entity
handle survives a `Runtime::ResetRegistry()` (`Runtime.hpp:164`), which swaps in
a fresh empty registry while keeping the shared `ComponentRegistry` and the
system schedulers. Systems belong to the plugin and must survive; entities
belong to the scene and must not.

The `CommandStack`'s `std::function<Astra::Registry&()>` resolver already
tolerates the registry swap -- that was the point of the UAF fix -- so no change
is needed there.

## 8. Testing

All headless, no `[gpu]` tag:

- **`SceneAssetTest.cpp`** -- round-trip through a real file; id preserved
  across save/load; version mismatch rejected; corrupt JSON rejected; missing
  file rejected; a failed load leaves the target registry untouched.
- **`EditorSceneSessionTest.cpp`** -- dirty transitions across edit/save/undo;
  undo-back-to-clean; the confirm state machine including a second request while
  one is pending; Save As retargeting the session's path and Guid. (As shipped:
  "Save refuses while in Play" is NOT a `SceneSession` test -- that gate lives in
  `EditorApp::DoSaveScene`, which is host wiring and is desk-verified like every
  other ImGui-facing surface in this arc.)
- **`AssetBrowserTest.cpp`** -- a case for `.arcscene` classifying as
  `AssetKind::Scene`.
- **`ProjectManifestTest.cpp`** -- update for `bootScene` as a Guid; add a
  `SetBootScene` round-trip through a rewritten `.arcproj`.

Note that `ArcaneTests` runs in random order under a time-based seed, so a
single green run is a sample and not a proof: capture the "Randomness seeded to"
banner and re-run the suite under at least two fixed seeds before calling the
gate green. Never construct a bare `Arcane::Runtime` in a test -- it steals
Arcane.dll's TypeContext slot and `Edit::` operations then silently report zero
changes.

## 9. Known gaps, flagged not fixed

- **Plugins that cache entity handles in `Init`** hold dangling handles after a
  scene load. The data-wins direction says plugins should not do this; Sandbox
  does, and is scaffolding scheduled for removal. Not worked around here.
- **Multi-scene / sub-levels** are out of scope. One open scene at a time.
- **Scene diffing, merge, or a text-friendly stable entity ordering** beyond the
  existing root-first BFS is out of scope.
- **Every ImGui surface remains without automated coverage.** The menu wiring,
  the confirm modal and the Asset Browser routing need desk-verify by a human,
  as with every editor panel shipped to date.

## 10. Out of scope

Deliberately excluded from this arc, in descending order of likely next step:

- **Entity clipboard** (Cut/Copy/Paste/Duplicate) -- would ride the same subtree
  serialization and kill three more dead menu items, but is its own arc.
- **Sandbox migration** to authored scenes -- Sandbox is being removed and
  remade, so migrating it now is wasted work.
- **Editor Preferences window** (`EditorPanels.cpp:94` TODO).
