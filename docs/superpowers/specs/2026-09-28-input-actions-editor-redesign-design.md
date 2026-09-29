# Input Actions editor redesign

**Status:** Approved 2026-09-28 (review decisions in section 6; arc sequence in section 7)
**Builds on:** `2026-09-28-inspector-ownership-design.md` (approved) -- this document is the
first non-scene Inspector source and the first `PropertyGrid` client.
**Reference:** the interactive mockup https://claude.ai/artifact/TUEutRAVgF9MmokP8kiuFE (its
"Player (Input Actions)" tab and Inspector pages are this spec's picture) and the existing
authoring spec `2026-09-28-project-input-actions-authoring-design.md` (asset model, evaluator,
boot path -- unchanged here).
**Replaces:** the current `InputActionsDocument` / `InputActionsDocumentWidgets` presentation
(the model, `InputActionsEditorModel`, stays; its API grows a little).

## 1. Why

The shipped document is raw ImGui: bare buttons (`Save`, `+ Map`, `Edit Scheme`, `Delete
Map`, `Duplicate Row`, `Move Up/Down`), `InputText`s with trailing lower-case labels, a
tree whose rows read `Move (Axis1D)  + 1D 2D`, bindings hidden by default with no device
or readable control name, a `Save / Saved` header, an `Actions | JSON` tab strip, a
`LIVE: Waiting scalar 0.00` printf, control schemes as collapsible headers in the sidebar
with text clipped at 200 px, and a PROPERTIES block drawn inside the document while the
editor's Inspector says "No selection". None of it uses the editor's vocabulary (the
Outliner's rows, the Asset Browser's `+ Create ▾`, search field, badges, lucide icons).

## 2. The document

Two columns inside the document window, split by a draggable separator (persisted in the
layout ini like the shell's splits); a toolbar above them.

### 2.1 Toolbar
- `+ Add ▾` : Action map · Action · Binding (to the selected action) · Composite (1D axis,
  2D vector) · Control scheme. Items are enabled by what is selected.
- Search field (Asset Browser style): filters actions by name and bindings by readable
  name or path; maps stay listed.
- Scheme filter combo: `All schemes` · one entry per scheme · `Edit schemes…` (opens the
  scheme popup, 2.5). Filtering hides bindings not in the chosen scheme's group; ungrouped
  bindings always show.
- `Preview` toggle: arms the live evaluator; while on, bindings glow amber as they fire
  and the Inspector's Live preview block reads live values. Off by default.
- No Save button and no JSON tab. Dirty state is the tab's dot and Ctrl+S (already
  routed to the focused document). JSON stays reachable as `Assets › Open as text` on the
  asset row: the external editor for now, and the in-editor text editor once that
  mini-arc lands (decided 2026-09-28: a fast, vim-capable, Sublime/VS Code-class text
  editor is the LAST mini-arc of this editor-upgrade arc; it gets its own spec).

### 2.2 Maps column (left, 180 px default)
- `ACTION MAPS` label row with a `+` icon button.
- One row per map: layers icon, name, a `blocks` badge when `blocking`, an action-count
  badge. Selected row uses the selection colour, hover the raised panel -- the Outliner's
  treatment. Right-click: Rename, Duplicate, Delete, Move up/down.
- Selecting a map shows its actions and pushes the map page to the Inspector.

### 2.3 Actions column (right)
- Header row `<Map> · actions` with a `+ Action` button.
- **Action row**: name (medium weight), type badge (`Button` / `Axis1D` / `Axis2D`), the
  interaction if any (`Hold 0.30 s`), dimmed. Right-click: Rename, Duplicate, Delete,
  Move up/down, Add binding, Add composite.
- **Binding rows, expanded by default** under their action, indented: device icon
  (lucide keyboard / gamepad / mouse), readable control name (`Space`, `Left Stick X`,
  `South Button` -- from `InputActions::BindingDisplayString`, never the raw path),
  composite part as a dimmed suffix (`1D Axis · negative`), scheme badge (KeyboardMouse
  tinted blue-grey, Gamepad violet-grey), and a `Rebind` button visible on hover and on
  the selected row. Selecting a binding pushes the binding page.
- **Composites** are one row per part under a composite header row (`1D Axis` / `2D
  Vector` with its own badge); parts are bindings with a part role.
- `+ Binding` ghost button after each action's bindings.
- **Rebind capture** (existing `InputRebindOperation`): the row's control text becomes
  `Press a control… Esc cancels · 10 s`, amber; the initiating click is not a capture; keys
  the UI has claimed are ignored (landed in the hygiene pass); completion writes the path
  through the model's edit command (undoable) and the row shows the new readable name.
- **Conflicts**: a binding whose control is bound elsewhere in the same map and scheme
  gets an amber dot before its badge and a tooltip naming the other action; the
  Problems panel lists the same set for the document (`Warnings()` extended to
  ungrouped-vs-grouped overlap and to unknown control paths, both owed by the review).
- Reorder: drag rows within their parent (ImGui drag-drop payload = id), plus the
  context menu's Move up/down for keyboard users.
- Keyboard: Up/Down move selection across visible rows, Right/Left expand/collapse an
  action, Enter rebinds the selected binding, Delete removes (with the undo command),
  F2 renames.

### 2.4 Inspector page (the document is a selection source)
Breadcrumb `Player.arcinput › <Map> › <Action> › <Binding>`; each crumb selects that
level. Pages, all drawn with `PropertyGrid`:
- **Asset** (nothing selected in the document): name, path, schemes list, default map
  combo, counts.
- **Map**: Name, Blocking, Priority; Contents (actions, bindings).
- **Action**: Name, Type combo, Interactions combo (`—`, Press, Hold with seconds, Tap),
  Processors; Bindings summary (count per scheme); **Live preview** block: Phase, Device,
  Value with a meter, live while the toolbar's Preview is on -- the designed replacement
  for the printf.
- **Binding**: Control (readable), Path (mono, editable), `Rebind…` and `Pick…` (the
  picker becomes a searchable list generated from the evaluator's own control tables, so
  it can never offer a path the evaluator cannot compile), composite part role,
  Control schemes (a checkbox per scheme = group membership), Processors (Scale,
  Invert), Live preview.
- Field edits go through `ApplyEdit` so undo/redo stays on the shared `CommandStack`;
  the JSON-tab keystroke-granularity undo goes away with the tab.

### 2.5 Control schemes popup
From the toolbar combo's `Edit schemes…`: a small modal with the scheme list (name,
group, delete) and `+ Scheme`. Renaming a scheme renames its group references (the model
already does this). Nothing about schemes lives in the maps column any more.

### 2.6 Save and publish
Ctrl+S saves atomically (existing). On a successful save the host re-runs
`ConfigureGameInput` for the designated asset so a running preview/play session picks up
the new definition at the next frame boundary, preserving active maps and compatible
overrides -- the republish the authoring spec promised and the review found missing.

## 3. Engine-side pieces (in dependency order)
1. **`PropertyGrid`** (`Widgets/PropertyGrid.hpp`): the section header, label/field row,
   numeric triple, checkbox, combo, mono text and "button row" primitives extracted from
   `DrawInspectorPanel`, so the scene page and every document page share one look.
2. **`InspectorHost`** (spec 2026-09-28-inspector-ownership): sources, last-selected
   routing, pin, instances, history; the scene page wraps `DrawInspectorPanel`; the
   Inspector panel's body becomes `host.Draw()`.
3. `EditorDocument::Page()` + selection-changed signalling; `InputActionsDocument`
   implements it.
4. The document presentation above.
5. `--select-in-document <map>[/<action>[/<binding>]]` (editor only) beside `--open-asset`,
   so a golden can show a chosen page.

## 4. Verification
- Model tests (existing `InputActionsEditorModelTest`): unchanged behaviours plus the new
  `Warnings()` cases and the picker-from-evaluator-tables property (every offered path
  compiles to a resolved binding).
- `InspectorHost` unit tests (per the ownership spec).
- Witness lane `E-input`: `--headless --open-asset <Player> --select-in-document
  Player/Jump`, report carries `inspector.source == "Player.arcinput"` and the breadcrumb
  text; golden `editor-input-doc` on dx12 + vulkan, blessed once the layout settles.
- Desk: the mockup's flow on the real editor, plus the rebind capture with a real keyboard
  and a gamepad.

## 5. Out of scope
Runtime rebind conflict reporting to the game (`InputRebindResult` conflicts), device
display glyphs (icons for individual keys), the unversioned legacy asset translation --
all remain on the authoring spec's owed list.

## 6. Decisions taken at review (2026-09-28)
- **JSON view**: the tab is dropped; `Open as text` opens the external editor until the
  in-editor text editor mini-arc (last in this editor-upgrade arc) replaces it.
- **Composites**: header row + one row per part (mirrors Unity; parts stay individually
  rebindable).
- **Reorder**: drag-drop within the parent plus the context menu's Move up/down.

## 7. This arc's sequence (as agreed)
1. Inspector ownership + this redesign (one plan; `PropertyGrid` first).
2. The asset page + Asset Browser preview-pane trim.
3. The Material tab migration into the shader document's page.
4. The in-editor text editor (own spec: buffer model, vim mode, search/replace, multi-
   cursor, syntax highlighting, large-file performance; candidates to study before
   designing: Zep, ImGuiColorTextEdit, and the editors named above).
