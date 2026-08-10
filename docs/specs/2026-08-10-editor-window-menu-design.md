# Editor Window Menu + Panel Visibility Registry

**Date:** 2026-08-10
**Status:** Approved
**Scope:** Arcane Editor (`Arcane/ArcaneEditor/`)

## Goal

Add a **Window** menu to the editor menu bar (after Assets, before Build) where
users show and hide the editor's dockable panels, plus a **Reset Layout** item.
Panel visibility is backed by a scalable registry -- adding a future panel is
one enum value, one table row, and a gate at its draw site; the menu, the tab
X buttons, persistence, and Reset all iterate the table and never change.

Reference model: the vendored UE Window menu (`Arcane/.example/.../MainMenu.cpp`,
`RegisterWindowMenu`, :294-366) -- a flat list of panel spawners followed by a
Layout section. We ship the flat list + Reset Layout; Load/Save/Remove named
layouts are out of scope.

## Decisions (ratified in brainstorming)

- Every panel EXCEPT the Viewport gets a close X on its docked tab.
- The Viewport is **permanent**: no X, cannot be hidden; its menu entry is an
  always-checked item whose click focuses the Viewport tab.
- The Window menu closes with a separator + **Reset Layout** (one item; no
  named-layout management).
- The Material panel is **excluded** from the registry: it is document-driven
  (`ShaderEditorDocument::DrawMaterialWindow` submits nothing without an active
  shader document, ShaderEditorDocument.cpp:1914-1916), so its lifecycle
  belongs to the document host, not this menu. Floating documents (shader
  graph, sprite) likewise stay out -- they own their close buttons.

## 1. Panel registry (`EditorPanels.hpp`)

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
permanent entry today.

Runtime state:

```cpp
struct PanelVisibility {
    std::array<bool, static_cast<size_t>(PanelId::Count)> visible;  // default all true

    bool  IsVisible(PanelId id) const;  // permanent panels always report true
    bool* OpenFlag(PanelId id);         // &visible[id], or nullptr when permanent (no X)
};
```

Owned by `EditorApp` as a member (`m_panelVis`), passed by reference into
`BeginDockSpace`. The menu toggles the bools in place -- visibility is pure UI
state, so it does NOT go through `MenuRequests` (the file's convention reserves
requests for actions the app must run, e.g. dialogs). Only Reset Layout is a
request, because the rebuild must run at a specific point in the frame.

## 2. Window menu (in `BeginDockSpace`)

Position: after Assets, before Build. Contents, in `kPanels` order:

- Permanent entries: `MenuItem(name, nullptr, true)`; on click call the
  existing `SelectDockTab(name)` directly -- safe immediately, the Viewport is
  always submitted, so its window/node/tab-bar exist.
- Non-permanent entries: `MenuItem(name, nullptr, &vis.visible[i])` -- a
  checkmark toggle bound to the visibility array.
- Separator, then **Reset Layout** -> `requests.resetLayout = true`.

Signature changes:

- `BeginDockSpace(...)` gains `PanelVisibility& panels` (after
  `hasGameModule`, before `recents`).
- `MenuRequests` grows `bool resetLayout = false;`.

## 3. Hiding and the tab X

The five non-permanent `Draw*Panel` functions (`DrawOutlinerPanel`,
`DrawInspectorPanel`, `DrawConsolePanel`, `DrawProblemsPanel`,
`DrawAssetBrowserPanel`) gain a `bool* open` parameter forwarded to
`ImGui::Begin(name, open)`. The docked tab's native close X writes the same
bool the menu checkmark reads -- one source of truth, the two cannot drift.

`EditorApp` skips a panel's draw call entirely when `!IsVisible(id)`
(call sites: EditorAppFrame.cpp ~1210 Assets, ~1264 Console, ~1273 Problems,
~1650 Outliner, ~1653 Inspector). All five sites already tolerate the panel
producing nothing: ConsoleBuffer accumulates independently of the panel,
the DiagnosticStore is app-owned, the browser/outliner/inspector return
values are consumed only when present.

Re-show behavior: ImGui restores a re-submitted window to its remembered dock
node (`window->DockId = settings->DockId`, the restore path already cited at
ShaderEditorDocument.cpp:1913); if the node was garbage-collected while empty,
ImGui rebuilds it from its persisted DockNodeSettings. The dock system's
newly-added-tab rule (imgui.cpp:19617-19620 -- the rule `SelectDockTab`
deliberately loses to) selects the returning tab, so show-and-focus comes
free. Desk-verify confirms this; the fallback, if it does not hold, is a
one-frame-deferred `SelectDockTab(name)` after the panel's first re-Begin.

## 4. Reset Layout

`EndDockSpace()` gains `bool resetLayout = false`. When set, call
`BuildDefaultLayout(dockspaceId)` unconditionally -- the exact code path the
first-run (no saved node) case already exercises at the same safe point
(DockBuilder mutations before the `DockSpace()` submission,
EditorPanels.cpp:239-259). The app forwards `requests.resetLayout` at the
call site the same frame and, when set, also resets every `visible` entry to
true so hidden panels come back with the default layout.

`DockBuilderDockWindow` operates on window settings, so it re-docks currently
hidden windows too; they land in their default nodes when re-shown. This item
doubles as the standing cure for "an old imgui.ini hides newly shipped
panels" (the Problems-panel case).

## 5. Persistence

New ini section `[EditorPanels][Visibility]`, mirroring the
`[EditorPlayMode][State]` handler exactly (EditorApp.cpp:92-158): registered
at the same Init site (after the ImGui context exists, before the first
NewFrame reads the ini), idempotency-guarded, `UserData` pointing at the
app's `PanelVisibility`.

Format: one name-keyed line per non-permanent panel, e.g. `Console=1`.
Name-keyed so reordering or adding panels never breaks an old ini. Read rules:
unknown names ignored, malformed lines ignored, missing lines keep the
default (visible) -- never trust a hand-edited ini. WriteAll emits every
non-permanent panel.

## 6. Testing

Pure-logic Catch2 tests (tag `[editor]`, headless):

- Registry integrity: entry count == `PanelId::Count`; entry `i` has id `i`;
  names unique; exactly one permanent entry.
- `PanelVisibility` defaults all-visible; `IsVisible` true for the permanent
  panel regardless of its array slot; `OpenFlag` null for permanent, non-null
  otherwise.
- Ini line parsing (extracted as a free function so it is testable): valid
  lines round-trip, unknown names and malformed lines are ignored.

Desk-verify (visual, at the desk):

- X on each non-viewport tab hides the panel; the Window menu item unchecks.
- Re-show returns the panel to its previous dock spot with its tab selected.
- Visibility survives an editor restart.
- Reset Layout rebuilds the default layout and re-shows everything.
- The Viewport tab has no X; its menu entry stays checked and focuses it.
- An open center document (shader graph) survives Reset Layout -- the
  DocumentHost re-docks it (DocumentHost.cpp:156).

## Out of scope

- Load/Save/Remove named layouts (UE's full Layout section).
- Multi-instance panels (UE's Details 1-4 style submenus) -- the registry's
  one-window-per-entry shape does not preclude them later.
- A Window-menu entry for the Material panel or floating documents.
