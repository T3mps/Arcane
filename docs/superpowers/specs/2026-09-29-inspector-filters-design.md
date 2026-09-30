# Inspector filters: which source kinds an instance follows

**Status:** Implemented 2026-09-30, branch feat/inspector-filters (approved 2026-09-29 after a
brainstorm with the user; decisions in section 8). A full shader NODE page is the next phase,
right after this one.
**Amends:** `docs/superpowers/specs/2026-09-28-inspector-ownership-design.md` s3.3 (pin and
instances) and s3.5 (the Asset Browser's preview pane goes ENTIRELY; the "keeps the thumbnail
and the Open button" option is dropped).
**Scheduled:** editor mini-arc 2, as ONE piece with the asset page and the preview-pane removal
(s6 explains why they cannot land apart). The Material tab retirement (formerly mini-arc 3) and
the Sprite/Mesh property forms are FOLDED INTO THIS PHASE (s6a, user 2026-09-29).

## 1. The problem

An Inspector instance today does one of two things: it FOLLOWS (shows the page of whichever
source selected last, `InspectorHost::Current()`) or it is PINNED (holds one source + key).
There is no middle: "keep showing whatever is selected in the scene while I work in an action
map" needs a pin that the user re-pins on every scene click.

The request: each instance carries a FILTER -- a set of source kinds it follows, chosen from a
dropdown of checkboxes. All kinds ticked (the default) is today's behaviour; "Scene" alone is a
scene inspector; "everything but Scene" is equally expressible.

Example: click entity A, then entity B, then open an action map and select a binding. An
instance filtered to Scene shows B; an instance on All moves to the binding.

Prior art: Houdini's pane link groups (panes follow a group or are pinned). Unreal's Details
panels only follow or lock.

## 2. Kinds

- `InspectorSource` gains `[[nodiscard]] virtual std::string_view Kind() const = 0;` -- a
  stable id. The catalog, in dropdown order: `"scene"` Scene, `"assets"` Assets,
  `"input-actions"` Input Actions, `"material"` Materials (the shader editor's documents),
  `"sprite"` Sprites, `"mesh"` Meshes. A document with nothing to edit (the crash-report
  viewer) has an empty kind.
- The kinds form a fixed editor-side CATALOG (id + display name, e.g. `"input-actions"` ->
  "Input Actions"), independent of which sources are open: a filter must be choosable and
  restorable before any source of a kind exists.

## 3. Model (`InspectorHost`)

- **One selection rule for every source (user, 2026-09-29: "the same functionality for the
  inspector throughout all editors/documents").** Opening a document selects its default page;
  a selection gesture inside a source (including a click anywhere in a document's content that
  re-selects its document-level page) is a selection event; switching tabs, focus and window
  activation are NEVER events; a clear is never an event. Every source -- the scene, the Asset
  Browser, every document kind -- follows it; no source gets a focus-follow exception.
  Scoped exception: the Asset Graph lens mirrors its persistent canvas selection into the model
  only on change, so re-clicking the already-selected Graph node is not a selection event; every
  other asset click site (Browser, Status, feed, the Browser row context menu on its first frame)
  is.

- **Per-instance filter.** `Instance` gains `std::vector<std::string> excluded;` -- the
  UNTICKED kinds. Empty = All (the default for every instance). The filter is stored as
  exclusions on purpose: a kind added to the catalog later is SHOWN by every existing filter
  ("everything but Scene" keeps meaning that when Shader arrives); see decision 8.3.
- **At least one kind stays ticked.** The dropdown refuses to untick the last ticked kind
  (the checkbox draws disabled with a tooltip). An instance that can show nothing reads as
  broken.
- **Selection order.** The host keeps a monotonic counter; every selection EVENT
  (`NotifySelected`, behind the existing `SelectionEdge` filter -- a clear is never an event)
  and every history landing stamps its source with the next value. `RemoveSource` drops the
  stamp; `ReleaseAll` clears them all.
- **Permanent sources.** The Asset Browser's source lives as long as the app, like the scene:
  `ReleaseAll` (project switch) keeps it registered and invalidates its keys (history entries,
  pins) instead of dropping it.
- **Routing.** `SourceFor(id)`:
  1. pinned -> the pinned source (unchanged);
  2. no exclusions -> `Current()` (unchanged);
  3. `Current()` admitted by the filter -> `Current()` (a filtered instance agrees with an All
     instance whenever it can: closing a document falls back to the scene for both);
  4. otherwise -> among registered sources the filter admits, the one with the highest stamp;
     if none has a stamp yet, the fallback (scene) when admitted, else the most recently added
     admitted source; if none is admitted, null.
  A source whose `Kind()` is empty (a document that never selects: mesh, sprite) is admitted
  only by All.
- **No matching source.** The instance draws one line naming what it waits for: "No Input
  Actions document open" for a single kind, "Nothing to show for this filter" otherwise.
- **Deselect is per source.** What an instance shows is always its routed source's LIVE
  selection. A clear in source X changes only the instances routed to X: clicking empty space
  in the viewport blanks a Scene inspector ("No selection") and leaves an Input Actions
  inspector on its binding; an All inspector is unaffected unless it is currently routed to X
  (a clear is not an event, so it moves no stamp and never moves `Current()`). A filtered
  instance never keeps a stale page after its own source clears -- keeping a page is what pin
  is for.
- **Pin is per instance.** `CanPin` and `SetPinned` take the instance id and capture what THAT
  instance shows (`SourceFor(id)` + its key + name), not `Current()`. Without this, pinning a
  Scene inspector just after clicking a binding would pin the binding. Unpinning returns the
  instance to following its filter.

## 4. History

- One shared history (depth 32, prune-on-invalidate, never persisted) -- unchanged.
- A filtered instance's back/forward step only through entries whose source's kind passes its
  filter. `CanGoBack/CanGoForward/GoBack/GoForward/BackEntry/ForwardEntry` and the history
  dropdown take the instance id; no exclusions = today's exact behaviour.
- An instance's position is anchored on what it SHOWS (final fix wave, item N): the history
  entry whose (source, key) equals its routed `SourceFor(id)` and that source's live
  `SelectionKey()`, nearest the cursor. Only when no entry matches does it fall back to the
  last admitted entry at or before the cursor. Back/Forward are the nearest admitted entries
  before/after that position, so one instance navigating the shared cursor never makes
  another instance's arrows lie. `Push`'s forward truncation stays global (browser
  semantics; a per-kind truncation is parked).
- Landing on an entry restores it in its source (unchanged `TryLand`), so it IS a selection:
  the cursor moves in the shared list and every instance whose filter admits that source
  follows.

## 5. UI

- **Header:** a filter dropdown between the back/forward arrows and the breadcrumb:
  `<- -> [All but Assets v]  Scene > Player  (pin)`. It opens a list of checkboxes, one per
  catalog kind. Available on every instance, including id 0.
- **Label** (dropdown face and window-title suffix):
  - every kind ticked: "All" on the dropdown, no title suffix;
  - one kind ticked: its name ("Scene");
  - one kind unticked: "All but <name>" ("All but Assets");
  - otherwise: the ticked names joined with ", ", ellipsized to the dropdown width (full list
    in the tooltip).
- **Title:** "Inspector - All but Assets", "Inspector 2 - Assets"; the `###` window id is
  unchanged so the dock slot and `[Window]` entry survive a filter change.
- A filtered window must be obviously filtered: an inspector that ignores some clicks
  otherwise reads as broken.

## 6. The Asset Browser's preview pane goes; two inspectors by default

- The Asset Browser's in-panel preview pane (`kAssetsPreviewPaneDefaultWidth`, the
  table<->preview splitter) is REMOVED entirely. The Asset Browser becomes an ordinary
  `InspectorSource` (kind `"assets"`); its selection contributes the asset page (identity,
  cook, derived rows, the thumbnail and the Open button -- everything the pane showed).
- **Default layout (`BuildDefaultLayout`) gets two inspectors:**
  - "Inspector" (id 0) in the right node, filter **All but Assets**;
  - "Inspector 2" filter **Assets only**, docked in a split to the RIGHT of the Asset
    Browser's node (where the pane was), so it sits where users already look.
  This keeps today's behaviour -- browsing assets never replaces the entity page -- while the
  asset details become a full Inspector (history, pin, its own filter).
- **Why this cannot land apart from the filters:** once the Asset Browser is a source, an
  unfiltered Inspector jumps to every asset click and throws away the page the user was
  editing. The pane is what prevents that today; the filtered default layout is its
  replacement. Asset source + pane removal + filters + default layout ship together.
- **Layouts saved before this feature are upgraded ONCE.** A `[EditorInspector][Instances]`
  section with no `Filters=` line was written before this feature. On that first load the
  host applies the new default: instance 0 gets "All but Assets", and an "Assets only"
  instance (lowest free id) is docked into a split to the right of the Asset Browser's dock
  node (if the Asset Browser is not docked, the instance takes New Inspector's placement: a tab
  in the main Inspector's dock node). The next save writes `Filters=`,
  so the upgrade never repeats. A user who deliberately set everything back to All keeps it:
  an empty `Filters=` line is a real answer, not a missing one.

## 6a. The Material tab retires; Sprite and Mesh forms move into the Inspector

- **Material page.** `ShaderEditorDocument` becomes a source (kind `"material"`). Its page is the
  whole document's -- NOT a node's (node pages are the next phase): the document title (plus
  "(Instance)"), the live preview, the draggable preview/params split (still the one
  editor-wide `previewSplit` preference, persisted as today), and the params editor (blend,
  cull, parameters). Selection key `"material"`: opening the document selects it; a click
  anywhere in the document's content (canvas background, a node, the preview, the snippet
  text) re-selects it. Selecting a node in the canvas does not change the page this phase.
- **Retired:** the "Material" window (`DrawMaterialPanel`/`DrawMaterialWindow`), its docking in
  `BuildDefaultLayout`, and the app's tab-follow logic (`SelectDockTab("Material")` /
  `SelectDockTab("Inspector")` on a material tab becoming visible) -- in fact all of
  `EditorApp::SyncCenterTabFocus`, including its viewport-appearing -> `SelectDockTab("Inspector")`
  branch and `ViewportPanelResult::appearing`. That branch existed only because Material shared
  the Inspector's dock node. Accepted loss: a window the user docks into the Inspector's node no
  longer gets the Inspector tab raised when the viewport tab appears. A stale `[Window][Material]`
  ini entry is harmless (never submitted again).
- **Sprite page / Mesh page.** `SpriteDocument` (kind `"sprite"`) and `MeshDocument` (kind
  `"mesh"`) become sources with one document-level key each (`"sprite"`, `"mesh"`), same open +
  click rule. Each document's FORM (sprite: texture, sub-rect, pivot and the rest of
  SpriteAssetData; mesh: source + topology parameters) moves into its page; the document window
  keeps its toolbar and, for the mesh, its preview (interactive where it is today); the sprite
  window keeps its "(no texture)" placeholder (it never had a preview). Edits keep riding each document's
  own undo steps (its EditGesture bracket moves with the form).
- **Save.** Ctrl+S in an Inspector showing any document page saves that document (the existing
  `saveRequested` route).

## 7. Persistence

- Filters are LAYOUT (like the instance list), persisted in `[EditorInspector][Instances]`
  as one more line: `Filters=<id>:<kind>+<kind>,<id>:<kind>` listing each instance's
  EXCLUDED kinds (instances absent from the line are All; the line itself is always written).
  Kinds not in the catalog at load are dropped from the exclusion set.
- The existing ClearAllFn resets filters with the list (a windowed project switch reads the
  incoming project's layout; a missing `Filters=` there triggers the one-time upgrade too).
  `ReleaseAll` (project switch) keeps filters and releases sources, pins, history and the
  selection stamps exactly as today.
- Pins stay unpersisted.
- **Window > Reset Layout** restores the default: exactly instances {0, 1}, 0 = All but Assets,
  1 = Assets only, docked as in the default layout.

## 8. Decisions taken in the brainstorm (2026-09-29)

1. **Filter by source KIND**, not by a specific source (a specific document dies on close and
   cannot persist) and not Scene-vs-Documents buckets (cannot tell a shader inspector from an
   input-actions one).
2. **A multi-select filter (checkbox dropdown)**, not a single target: "everything but X" is a
   first-class filter. (Supersedes the single-target draft of this spec, same day.)
3. **Persist the unticked kinds**, so a new kind appears in existing filters. The user's rule:
   the tool stays honest -- a filter that silently narrows when the catalog grows is the kind
   of surprise that makes a tool infuriating.
4. **Back/forward = the shared history, filtered**; landing re-selects in the source. Not a
   per-instance history (a second list to keep in sync), not hidden arrows.
5. **An emptied source shows "No selection"**, never the last page (an unmarked auto-pin
   whose edits land on an entity selected nowhere). Deselect stays per source/editor: a scene
   clear never blanks an instance filtered to another kind (s3).
6. **Header dropdown + title suffix**; any instance, including the main one.
7. **The preview pane is removed entirely**; the default layout carries two inspectors (All but
   Assets / Assets only). Filters move into mini-arc 2 with the asset page, because the pane
   cannot go before the filters exist.
8. **Old layouts are upgraded once** to the two-inspector default (s6), rather than left on a
   single All inspector with a "Reset Layout" hint.
9. **The Material tab retires in this phase**, material page only; a full node page is the
   next phase (user: "ensure we do a full node page as a soon follow up").
10. **One selection rule for every source**: open + click select, focus/tab switches never do
    (the user kept the rule over a follow-the-tab exception for materials).
11. **Sprite and Mesh forms move into Inspector pages in this phase** (the parent spec's
    s3.5 "per-document properties blocks" retirement, completed).

## 9. Verification

- Unit (`EditorInspectorHostTest`; the existing fake sources gain a kind):
  - routing by filter; no exclusions unchanged (every existing test passes untouched);
  - `Current()` wins when admitted; otherwise the higher stamp among admitted sources; closing
    it falls back to the other; "All but X" follows Y and Z but never X; an empty-kind source
    is admitted only by All;
  - `ReleaseAll` keeps a permanent source registered and drops its history entries and pins;
  - no admitted source -> `SourceFor` null;
  - **a clear in source X never changes what an instance filtered away from X shows** (and a
    clear never moves an All instance off a different source);
  - the last ticked kind cannot be unticked (the setter refuses an all-excluded set);
  - `SetPinned` on a filtered instance captures that instance's source/key, not `Current()`;
  - filtered back/forward skip non-matching entries; landing makes the source current;
  - ini: `Filters=` round-trip; an unknown kind dropped; a kind added to the catalog after the
    save is admitted; a missing `Filters=` line triggers the one-time upgrade and an empty
    one does not; ClearAllFn resets filters.
- Goldens: `editor-ui` changes (two inspectors, no preview pane) -- re-bless per the golden
  procedure; a new asset-selected golden (`--select` of an asset in the Browser, the Assets
  inspector on the asset page, the main Inspector unchanged).
- Witness: the report's inspector block gains `instances[].excluded` (kind ids; `[]` = All);
  `--open-asset` of a material, a sprite and a mesh each route the main Inspector to that
  document's page (`instances[0].source` = the document's name).
- Golden: `editor-material-page` (a material document open, its page in the main Inspector).
- Desk: the s1 example with a Scene and an All instance; an empty-space viewport click with a
  Scene + an Input Actions instance; an asset click on the default layout (the entity page
  stays); a pre-feature ReferenceProject/Aphelyon layout upgrading once and not again; change
  a docked instance's filter and confirm the dock slot holds.

### What shipped (witnesses and lanes actually added)

- Unit: `EditorInspectorHostTest` (routing, stamps, permanent sources, per-source deselect,
  per-instance pin, filtered history, `Filters=` ini round-trip and the one-time legacy upgrade),
  `AssetInspectorSourceTest`, and the sprite, mesh and material page tests (kind, open = selected,
  content click re-selects, tab switches never do). `VerifyReportTest` pins schema 12 and the new
  `inspector.instances` case; `HostConfigTest` covers `--select-asset`.
- Automation: the report's inspector block carries `instances[]` = `{id, excluded, source}`
  (`VerifyReport` schema 12; `golden-gate.ps1` `ReportSchemaMax` = 12). The `--select` of the
  asset golden is spelled `--select-asset <guid>` (editor-only; ArcaneRuntime refuses it with
  exit 2; the guid resolves through the project). Plugin ABI 45 -> 46 (`HostConfig::selectAsset`,
  the report's instances); `ReferenceProject` is restamped, the Aphelyon restamp is owed.
- Witnesses (`EditorWitnessTest`): E3 asserts `instances` on the input-actions run; E4 =
  `--select-asset` against the `editor-asset-page` golden; E5 = `--open-asset` of a material (with
  the `editor-material-page` compare), a sprite and a mesh, each routing the main Inspector to
  that document's page.
- Golden lanes (`golden-gate.ps1`, fourteen combinations): `editor-asset-page` and
  `editor-material-page`, each on dx12 and vulkan; `editor-ui`, `editor-ui-perspective` and
  `editor-input-doc` re-blessed for the two-inspector layout. The verify seed
  (`ReferenceProject/Saved/verify-layout.ini`) was re-authored with `[Window][Inspector]` +
  `[Window][inspector_1]`.
- Known limits of the goldens: the Assets-only pane is about 182x180 px in the default layout,
  so `editor-asset-page` shows the thumbnail only (the rows and import settings sit below the
  fold); for a mesh material the document's preview panel shows "compiling..." (an existing
  defect), and `editor-material-page` records that state.
