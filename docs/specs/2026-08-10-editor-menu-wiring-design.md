# Editor Menu Wiring Pass: Window Menu + Menu Item Wiring

**Date:** 2026-08-10
**Status:** Approved
**Scope:** Arcane Editor (`Arcane/ArcaneEditor/`), plus new engine-side entity
clipboard ops (`Arcane/Edit`)

## Goal

Two folds of the same pass:

1. Add a **Window** menu (after Assets, before Build) where users show and
   hide the editor's dockable panels, backed by a scalable panel registry,
   plus a **Reset Layout** item.
2. Wire the placeholder menu items shipped by the 2026-08-10 menu
   restructure: Edit's clipboard + selection ops, File's Exit / Save All /
   both recents, and Assets' Show in Explorer / Copy Path.

Reference model: the vendored UE menus (`Arcane/.example/.../MainMenu.cpp`;
Window menu at `RegisterWindowMenu`, :294-366 -- a flat list of panel
spawners followed by a Layout section).

## Ratified scope decisions

- Every panel EXCEPT the Viewport gets a close X on its docked tab.
- The Viewport is **permanent**: no X, cannot be hidden; its menu entry is an
  always-checked item whose click focuses the Viewport tab.
- The Window menu closes with a separator + **Reset Layout** (no named-layout
  Load/Save/Remove management).
- The Material panel is **excluded** from the Window menu: it is
  document-driven (`ShaderEditorDocument::DrawMaterialWindow` submits nothing
  without an active shader document, ShaderEditorDocument.cpp:1914-1916).
  Floating documents likewise own their close buttons.
- **Preferences... / Project Settings... are DEFERRED to their own arc** --
  they stay enabled no-op placeholders this pass.
- Entity clipboard ships the **full set** (Cut / Copy / Paste / Duplicate)
  over the OS clipboard.
- "Save Project" is **renamed "Save All"** (UE's File-menu item) and saves
  the dirty scene + every dirty open document.
- **Both recents** ship: Open Recent Project (parked plumbing resurfaces) and
  Open Recent Scene (new per-project tracking).

---

# Part I -- Window menu + panel registry

## I.1 Panel registry (`EditorPanels.hpp`)

```cpp
enum class PanelId : uint8_t { Viewport, Outliner, Inspector, Assets, Console, Problems, Count };

struct PanelInfo {
    PanelId     id;
    const char* name;       // ImGui::Begin title AND menu label AND ini key
    bool        permanent;  // true = no X, cannot hide (Viewport only today)
};
inline constexpr PanelInfo kPanels[] = { /* Window-menu order, Viewport first */ };
```

Table invariants: entry count == `PanelId::Count`, entry `i` has id `i`, names
unique and exactly matching each panel's `ImGui::Begin` title, exactly one
permanent entry today. Adding a future panel = one enum value + one table row
+ a gate at its draw site; menu, X buttons, persistence, and Reset all
iterate the table and never change.

Runtime state:

```cpp
struct PanelVisibility {
    std::array<bool, static_cast<size_t>(PanelId::Count)> visible;  // default all true

    bool  IsVisible(PanelId id) const;  // permanent panels always report true
    bool* OpenFlag(PanelId id);         // &visible[id], or nullptr when permanent (no X)
};
```

Owned by `EditorApp` (`m_panelVis`), passed by reference into
`BeginDockSpace`. The menu toggles the bools in place -- visibility is pure
UI state, so it does NOT go through `MenuRequests` (requests are for actions
the app must run). Only Reset Layout is a request, because the rebuild must
run at a specific point in the frame.

## I.2 Window menu (in `BeginDockSpace`)

Position: after Assets, before Build. Contents, in `kPanels` order:

- Permanent entries: `MenuItem(name, nullptr, true)`; click calls the
  existing `SelectDockTab(name)` directly -- safe immediately, the Viewport
  is always submitted, so its window/node/tab-bar exist.
- Non-permanent entries: `MenuItem(name, nullptr, &vis.visible[i])` -- a
  checkmark toggle bound to the visibility array.
- Separator, then **Reset Layout** -> `requests.resetLayout = true`.

## I.3 Hiding and the tab X

The five non-permanent `Draw*Panel` functions (`DrawOutlinerPanel`,
`DrawInspectorPanel`, `DrawConsolePanel`, `DrawProblemsPanel`,
`DrawAssetBrowserPanel`) gain a `bool* open` parameter forwarded to
`ImGui::Begin(name, open)`. The docked tab's native close X writes the same
bool the menu checkmark reads -- one source of truth, the two cannot drift.

`EditorApp` skips a panel's draw call entirely when `!IsVisible(id)`
(call sites: EditorAppFrame.cpp ~1210 Assets, ~1264 Console, ~1273 Problems,
~1650 Outliner, ~1653 Inspector). All five sites already tolerate the panel
producing nothing: ConsoleBuffer accumulates independently of the panel, the
DiagnosticStore is app-owned, the browser/outliner/inspector return values
are consumed only when present.

Re-show behavior: ImGui restores a re-submitted window to its remembered dock
node (`window->DockId = settings->DockId`, the restore path already cited at
ShaderEditorDocument.cpp:1913); if the node was garbage-collected while
empty, ImGui rebuilds it from its persisted DockNodeSettings. The dock
system's newly-added-tab rule (imgui.cpp:19617-19620 -- the rule
`SelectDockTab` deliberately loses to) selects the returning tab, so
show-and-focus comes free. Desk-verify confirms this; the fallback, if it
does not hold, is a one-frame-deferred `SelectDockTab(name)` after the
panel's first re-Begin.

## I.4 Reset Layout

`EndDockSpace()` gains `bool resetLayout = false`. When set, call
`BuildDefaultLayout(dockspaceId)` unconditionally -- the exact code path the
first-run (no saved node) case already exercises at the same safe point
(DockBuilder mutations before the `DockSpace()` submission,
EditorPanels.cpp:239-259). The app forwards `requests.resetLayout` at the
call site the same frame and, when set, also resets every `visible` entry to
true so hidden panels come back with the default layout.

`DockBuilderDockWindow` operates on window settings, so it re-docks currently
hidden windows too. This item doubles as the standing cure for "an old
imgui.ini hides newly shipped panels" (the Problems-panel case).

## I.5 Persistence

New ini section `[EditorPanels][Visibility]`, mirroring the
`[EditorPlayMode][State]` handler exactly (EditorApp.cpp:92-158): registered
at the same Init site, idempotency-guarded, `UserData` pointing at the app's
`PanelVisibility`.

Format: one name-keyed line per non-permanent panel, e.g. `Console=1`.
Name-keyed so reordering or adding panels never breaks an old ini. Read
rules: unknown names ignored, malformed lines ignored, missing lines keep
the default (visible). WriteAll emits every non-permanent panel.

---

# Part II -- Wiring the placeholder menu items

## II.A Edit: selection ops + rename/delete (pure routing)

`MenuRequests` grows `selectAll` / `deselectAll` / `invertSelection` /
`renameSelected` / `deleteSelected`. App-side consumption:

- **Deselect All** = `SelectionContext::Clear()`.
- **Select All / Invert Selection** collect the SceneRoot subtree in scene
  walk order (BFS pre-order -- NOT the Outliner's current sort, which is
  panel-local state) and rebuild the selection; primary = last collected.
  Registry access stays app-side (`SelectionContext` deliberately has none);
  the collectors land as small free functions so they are headless-testable.
- **Rename / Delete** route to the EXACT code the Outliner's F2/Del bindings
  use: `BeginRename` / `DeleteSelection` are promoted from EditorPanels.cpp
  file-local statics (:892/:905) to declared panel functions, called with the
  app-owned `OutlinerState`. Rename enabled when a selection exists (renames
  the primary, same as F2); Delete likewise. REVISED at the final review
  (user-ratified): the menu items grey during Play -- Cut/Copy/Duplicate/
  Rename/Delete on `hasSelection && !playing`, Paste on `!playing` -- so the
  refusal is visible before the click.

## II.B Entity clipboard (engine-side, `Arcane/Edit`)

Two new ops built on the reflection walk `SceneSerializer` already uses:

- `SerializeSubtrees(reg, roots) -> nlohmann::json` -- runs `SelectionRoots`
  first so a nested selection copies once; parent links are internal payload
  indices; each root additionally records its original parent's **GUID**.
- `InstantiateSubtrees(reg, json, ...) -> vector<Entity>` -- fresh Identity
  GUIDs, names uniquified per the creation-time policy (the UE split
  EntityOps.hpp:86-92 already cites: spawn/paste/duplicate unique-ify,
  interactive rename does not). Each root re-parents to its recorded parent
  GUID when that entity exists in the target registry, else under SceneRoot
  -- one rule that makes Duplicate a sibling (UE behavior) and cross-instance
  Paste sane. Refuses (creates nothing) when the registry has no SceneRoot,
  matching `CreateEntityInScene`'s contract.

Menu/app behavior:

- **Copy** puts a versioned JSON envelope on the OS clipboard
  (`ImGui::SetClipboardText`) -- UE puts T3D text there the same way.
- **Paste** reads the clipboard, no-ops with a console INFO on foreign
  content. Always enabled -- parsing the clipboard every frame for
  enablement is not worth it.
- **Cut** = Copy + `DeleteEntities`, ONE undo step ("Cut").
- **Duplicate** = serialize + instantiate fused, no clipboard involved.
- Every op wraps in `RegistryStateCommand` like the outliner's existing
  structural edits; pasted/duplicated roots become the new selection.
- Menu hints Ctrl+X/C/V/D; app-input-loop bindings gated on
  `!io.WantTextInput` (text fields keep their own clipboard).

## II.C File: Exit, Save All, both recents

- **Exit**: `requests.exitEditor`; the app routes it through the SAME
  `SceneIntent::Exit` flow the OS quit uses (EditorAppFrame.cpp:160-184) --
  a dirty scene parks behind the existing confirm modal; a clean/confirmed
  exit sets `m_requestExit`.
- **Save All** (renamed from "Save Project"): `requests.saveAll`; saves the
  scene when dirty (existing path; never-saved routes to the Save As dialog)
  plus every dirty open document via a new `DocumentHost::SaveAllDirty()`.
  Disabled during Play with the same tooltip as Save/Save As.
- **Open Recent Project**: submenu under Open Project, feeding the intact
  `MenuRequests::openRecentPath` consumer chain from the Hub-shared
  `recents` list already passed into `BeginDockSpace`.
- **Open Recent Scene**: new per-project tracking at
  `<project root>/Saved/recent-scenes.json` (`Saved/` already exists for
  editor.lock; imgui.ini is editor-global, so recents there would leak
  across projects). Pushed on successful scene open/save-as; deduped;
  capped at 10; missing files pruned at menu build. No project or empty
  list = submenu stays greyed. Selecting an entry routes through the same
  unsaved-changes-guarded open path the Assets panel's scene double-click
  uses (`AssetBrowserActions::openScene` precedent).

## II.D Assets: selection plumbing

`AssetBrowserState` gains a tracked last-clicked entry (Guid + resolved
path; cleared on project switch or when the entry vanishes). The menu learns
of it via a small readonly param into `BeginDockSpace`.

- **Show in Explorer**: enabled when the tracked entry has an on-disk path;
  shells `explorer /select,<path>` (win32-only is fine; the editor is).
- **Copy Path**: puts the absolute path on the clipboard.

## II.E MenuRequests summary

Grows: `resetLayout`, `selectAll`, `deselectAll`, `invertSelection`,
`renameSelected`, `deleteSelected`, `cutSelection`, `copySelection`,
`paste`, `duplicateSelection`, `exitEditor`, `saveAll`,
`openRecentScenePath` (string; empty = none), `showInExplorer`,
`copyAssetPath`. Reuses: `openRecentPath` (project recents, chain intact).

---

# Testing

Engine-side (ArcaneTests, headless):

- Clipboard round-trip: fresh GUIDs, uniquified names, internal parent
  remap, missing-parent-GUID -> SceneRoot fallback, nested-selection
  collapse, no-SceneRoot refusal, cut + undo restores.

Editor-side pure logic (tag `[editor]`, headless):

- Registry integrity: entry count == `PanelId::Count`; entry `i` has id `i`;
  names unique; exactly one permanent entry.
- `PanelVisibility`: defaults all-visible; `IsVisible` true for permanent;
  `OpenFlag` null for permanent, non-null otherwise.
- Visibility ini line parsing (extracted as a free function): valid lines
  round-trip, unknown names and malformed lines ignored.
- Scene-recents list ops: dedup, cap, prune with an injected exists-fn,
  JSON round-trip.
- Select All / Invert collectors over a scratch registry.

Desk-verify (visual, at the desk):

- X on each non-viewport tab hides the panel; the Window menu item unchecks.
- Re-show returns the panel to its previous dock spot with its tab selected.
- Visibility survives an editor restart.
- Reset Layout rebuilds the default layout and re-shows everything; an open
  center document (shader graph) survives it (DocumentHost re-docks,
  DocumentHost.cpp:156).
- The Viewport tab has no X; its menu entry stays checked and focuses it.
- Exit with a dirty scene shows the confirm modal.
- Both recents submenus populate and open their targets.
- Show in Explorer lands on the file; Copy Path pastes the absolute path.
- Paste works across two editor instances (OS clipboard).

# Out of scope

- Preferences... / Project Settings... windows (own arc; items stay
  placeholders).
- Load/Save/Remove named layouts (UE's full Layout section).
- Multi-instance panels (UE's Details 1-4 submenus) -- the registry's
  one-window-per-entry shape does not preclude them later.
- A Window-menu entry for the Material panel or floating documents.
- New Assets -> Create asset types beyond Material (they land as they exist).
