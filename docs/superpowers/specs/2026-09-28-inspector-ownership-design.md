# Inspector ownership: selection sources, pages, pin, instances

**Status:** Approved 2026-09-28 (review decisions in section 6)
**Mockup:** https://claude.ai/artifact/TUEutRAVgF9MmokP8kiuFE (interactive, in the editor's theme)
**Research:** `docs/research/2026-09-28-inspector-ownership-survey.md` (~30 products, cited)
**Supersedes:** the per-editor top-level panel pattern the shader editor's "Material" tab established (`ShaderEditorDocument.hpp:891`, docked beside the Inspector). That tab is migrated, not removed, by a later plan.

## 1. The problem

Arcane keeps every editor in ONE window: panels dock around a center node where asset
documents (shader graph, mesh preview, input actions) open as tabs. Today the Inspector
shows only the scene selection's components (`DrawInspectorPanel(registry, selection,
undo, ...)`, `EditorAppFrame.cpp:3598`). The shader document sidesteps it with its own
"Material" panel docked beside it; the new input-actions document draws a "PROPERTIES"
block inside its own pane. Each new document type has been inventing its own answer, and
the visible result is an Inspector that says "No selection" next to a second
inspector-shaped column.

Unreal's answer (one OS window per asset editor, each with its own Details) is ruled out
by design: everything stays in the one window.

## 2. What the field does (from the survey)

- Every single-window tool surveyed has ONE global, contextual properties panel. None
  shows two selection-following inspectors side by side on purpose; the embedded-panel
  exceptions (Unity Shader Graph's Graph Inspector, Android Studio's Attributes) are
  documented as problems (sticks to the cursor, size not saved, invisible when the
  editor is not).
- "The active editor owns the Inspector" is the mainstream model, and it works only with
  three mechanisms: (a) key it on the part that SUPPLIES a selection, not on focus
  (Eclipse's Properties view had a 17-year bug, #23862/#43085, for following focus;
  Visual Studio needed `SELCONTAINER_DONTPROPAGATE` for the same reason); (b) the source
  contributes a PAGE into the one panel (Eclipse page-book, Xcode's per-selection
  inspector bar, Visual Studio's per-designer toolbar modes); (c) PIN plus a second
  instance as the escape hatch, which nearly every mature tool added after resisting it
  (Unity, O3DE, GameMaker, Eclipse 3.0/3.4, Qt Design Studio, Blender, Houdini, Maya,
  C4D). Godot has no pin and carries open proposals with the predictable complaint.
- Blender's division of charge is the one to copy for WHAT goes where: the global panel
  holds data-block settings; each editor area's own sidebar holds that editor's view
  settings, active tool and transient item.

## 3. Decision

### 3.1 One Inspector, driven by selection sources

- A **selection source** is anything that can select: the scene (Outliner + Viewport,
  today's `m_selection`), the **Asset Browser** (its row selection; the page is the
  asset: path, GUID, kind, cook state, importer settings -- what its in-panel preview
  pane shows today, which that pane then stops duplicating), and any `EditorDocument`
  that opts in. The Inspector shows the page of the source that selected LAST.
- **Focus is not selection.** Activating a tab, clicking the Console, the Asset Browser,
  a document's empty background or the viewport background changes nothing. Only a
  selection event moves the Inspector. (This is the Eclipse `currentPropSourcePart`
  rule, stated up front instead of learned over 17 years.)
- Deselecting inside a source (click empty space in the input document) shows that
  source's container page (the map, or the asset) rather than "No selection" -- the
  IntelliJ-form / Rive-document fallback. "No selection" appears only for the scene with
  nothing selected and no document source active.
- A source that closes (document closed, project switched) releases the Inspector: it
  falls back to the scene page.

### 3.2 Pages

- A source contributes a **page**: a header (breadcrumb) and a body drawn with the
  shared widget layer (`Widgets/EditorWidgets.hpp`, `MaterialParamWidgets.hpp`, the
  Inspector's section/field rendering extracted into a `PropertyGrid` helper so every
  page uses the same rows, wells, labels and section headers as components do).
- The **breadcrumb** names the source and the path: `Scene › MeshCube`,
  `Player.arcinput › Player › Jump › Space`, `mesh_import_base.arcshader › Multiply`.
  Each crumb is clickable and selects that level in the source.
- Pages are contributed through a small interface on `EditorDocument`:

  ```cpp
  // A document that can select. Null = this document contributes no page and
  // never drives the Inspector (MeshDocument today).
  virtual InspectorPage* Page() { return nullptr; }
  // Fired by the document when its selection changes; the host's
  // InspectorHost routes it. Selection stays inside the document.
  ```

  `InspectorPage` = `Breadcrumb()` (vector of crumbs with select callbacks) +
  `Draw(PropertyGrid&)`. The scene's page is the current `DrawInspectorPanel`, wrapped.
- Document selection never enters the scene's `m_selection`, and vice versa (Unity's
  Animator/Timeline reuse of the global Selection is the documented failure: clicking a
  clip evicts the scene object). The two selection models stay separate; the Inspector
  merely asks the last source for its page.

### 3.3 Pin and instances

- The Inspector header gains a **pin** (pin, not lock: Godot renamed theirs because a
  lock reads as read-only). Pinned = keep this page whatever selects elsewhere; the page
  stays live for edits. A pinned page whose source goes away (document closed) shows a
  one-line "source closed" note and unpins on click.
- **Window › New Inspector** opens another instance in the right dock column (docked
  below or as a tab; the user rearranges). Each instance has its own pin. This is the
  answer to "I want the shader graph's node and the scene entity at once": pin one, let
  the other follow. At rest there is still one follower.
- The header also carries **back / forward** over selection history (Godot's arrows):
  cheap, and it turns "the scene page is gone" into "the scene page is one click back".

### 3.4 Documents keep their own toolbar (Blender's sidebar charge)

View settings, the active tool and transient state live in the document, never in the
Inspector: the input document's scheme filter, search, `Add ▾`, preview toggle; the
shader document's canvas zoom/grid; the mesh preview's camera. Data lives in the
Inspector page. When a document is used with the Inspector closed it still works; it
just has no properties view (the Blender rule: the sidebar duplicates the Properties
editor only where a standalone use needs it, and we start with none).

### 3.5 What this retires

- The Material tab as a category. `ShaderEditorDocument` becomes a source: node
  selection contributes a node page; nothing selected contributes the material page
  (blend, cull, parameters -- today's Material panel body). Migration is its own small
  plan after the input editor lands; until then the tab stays.
- Per-document properties blocks. `InputActionsDocumentWidgets`' "PROPERTIES" section is
  replaced by the document's page in the redesign spec that follows this one.
- The Asset Browser's preview pane as a properties surface. Its identity/cook/derived
  rows move to the asset page; the pane keeps only what is browser-specific (the
  thumbnail and the Open button), or goes entirely if that is all that is left.
  Decided 2026-09-28: part of this arc, not a later plan.
  **Amended 2026-09-29:** the pane goes ENTIRELY; the Asset Browser becomes a source and the
  default layout carries an "Assets only" Inspector beside it
  (`2026-09-29-inspector-filters-design.md` s6).

## 4. Automation

- `--open-asset <guid>` (landed `ca171951`) puts a document on screen headlessly; the
  input-editor redesign adds `--select-in-document <path>` (or reuses `--select-name`
  with a document-scoped syntax) so a golden can show a document page in the Inspector.
- `editor-ui` goldens do not change by this spec alone (the scene page is unchanged).
  New goldens: `editor-input-doc` (document open, an action selected, the Inspector on
  its page) once the redesign lands.
- Pin and New Inspector state are per-layout (imgui.ini via the existing layout seed
  mechanism), never per-project.

## 5. Verification

- Unit: an `InspectorHost` test double with two fake sources proves: last-selecting
  source wins; focus events do nothing; pin holds across selections; a closed source
  releases; history back/forward restores selection in the source; a second instance
  follows while the first is pinned.
- Witness: `E-lane` headless editor run: `--open-asset` + select-in-document, report
  field `inspector.source` = the Inspector source's `SourceName()` (for an input
  document its filename, e.g. `"Player.arcinput"` -- not a path; the scene reports
  `"Scene"`), and the breadcrumb text (e.g. `"Player.arcinput > Player > Jump"`) in
  the report.
- Desk: the mockup's six steps, on the real editor.

## 6. Decisions taken at review (2026-09-28)

1. **The Asset Browser is a source in this arc.** Outliner and Viewport are one source
   (both write `m_selection`). The Asset Browser's in-panel preview pane already shows
   the selected asset's details; with the unification that content becomes the asset
   page and the pane stops duplicating it (3.5).
2. **A pinned Inspector does not survive a project switch**; every source releases on
   switch. Per-user saved layouts (which could carry pins) are a later feature, not
   this arc.
3. **History depth 32, not persisted.** The cost that grows with depth is not memory
   (an entry is a source descriptor, ~100 bytes) but staleness: entries naming a
   deleted entity, a closed document or a removed binding must be skipped at
   navigation time and pruned when their source invalidates, and each invalidation
   walks the list. Depth is therefore a UX cap on how far "back" stays meaningful.
   Rule: prune on invalidate; skip anything unresolvable at navigation; never persist.

## 7. Owed by this spec

- The `PropertyGrid` extraction from `DrawInspectorPanel` (sections, rows, wells) is the
  only engine-wide code change; it is the first task of the input-editor plan because
  that document is its first non-scene client.
- The asset page + the Asset Browser preview-pane trim (this arc, after the input
  editor's page proves the seam).
- The Material tab migration plan (after the input editor).
- Per-user layouts (would carry pins and instances) -- later, its own spec.
- `[EditorInspector][Instances]` had no ClearAllFn -- the symptom of a bigger bug,
  fixed 2026-09-29 (arc-1 debt sweep Task 13). `ClearAllFn` ran only from
  `ImGui::ClearIniSettings`, which the editor never called, and the instance list is
  layout by design (`EditorInspectorHostTest.cpp:199`). The real defect:
  `RetargetLayoutIni` saved the outgoing layout and repointed `io.IniFilename` but
  never loaded the incoming file (ImGui reads the ini only while `!SettingsLoaded`),
  so a windowed project switch carried the outgoing project's docking, windows, panel
  visibility, camera, play mode, Material-panel preference and Inspector instances
  into the incoming project and then overwrote its file. A switch now clears and
  reloads, and every editor ini handler resets to defaults in its `ClearAllFn`.
