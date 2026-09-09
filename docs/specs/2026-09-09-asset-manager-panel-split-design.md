# Asset Manager Panel Split — Design

**Date:** 2026-09-09
**Status:** Approved (brainstorm run same day; every ruling below was decided by the user in-session)
**Amends:** `docs/specs/2026-09-06-asset-manager-redesign-design.md` (supersedes its §5 shell; leaves its lens bodies, value tables, and invariants binding — see §11)
**Baseline:** Arcane `main` @ `cc14a609` (all file:line anchors below are against this commit)

---

## 1. What this is

The one-panel asset manager (one "Assets" window, three lenses — Browse / Graph /
Status — switched by a toolbar lens strip) becomes **three separate dockable
panels** sharing the same backend: one `AssetPanelModel`, one selection, one
reference index, one activity ring. This is the user-decided **Option G**
direction (canvas exploration boards, 2026-09-09): the lens strip retires, the
Window menu grows grouped sections, and each lens body carries over wholesale
into its own window.

This is a **shell restructure, not a rebuild**. The lens bodies
(`DrawBrowseLens` `AssetsPanel.cpp:1998`, `DrawStatusLens` `:2633`,
`DrawGraphLens` `:3760`) already take an identical argument tuple and already
communicate exclusively through the shared model — they are near-drop-in as
three panel entry points. What actually changes is: the panel registry, the
Window menu, the toolbar/bottom-bar chrome, the state struct, and the three
places that programmatically switched lenses.

### Why (recorded verbatim intent)

The user's selection rule, stated 2026-09-09: *"if more than one asset view is
open and one gets a selection, select it in the others; otherwise don't open a
window that isn't open."* The one-panel shell forces one lens at a time; the
split lets Browse and Graph sit side by side, and the shared-selection behavior
the lenses already have becomes visible instead of latent. The backend keeps
working *"behind the scenes as it does now."*

---

## 2. Scope and non-goals

**In scope:** panel registry rows + Window-menu grouping; the file split of
`AssetsPanel.*` into three panel units + one shared unit; the state split; the
cross-panel command routing (with the focus-if-open rule); per-panel toolbars
and bottom bars incl. the Status recency line; default dock layout; both golden
lanes (layout seed + editor-ui image); test rework; canvas restamp.

**Non-goals (each deliberately ruled out, do not improvise):**

- **No new persistence.** Panel *view* state stays session-only (today's rule,
  `AssetsPanel.hpp:60`). Open/closed visibility keeps persisting exactly as
  today (§10). The Graph canvas continues to refuse imgui-node-editor's own ini
  (`AssetsPanel.cpp:3795`).
- **No new filtering.** Search stays a Browse-only behavior. Graph and Status
  gain no search boxes (toolbar ruling, §9).
- **No per-count digest deep-links.** The health-digest chip stays ONE hit
  target aimed at Asset Status; refused/cooking/unused do not become three
  separately-clickable links.
- **Backend untouched.** `AssetPanelModel`, `AssetReferenceIndex`,
  `AssetActivityLog`, the create flow (`CreateAssetRequest` invariant), and
  every engine seam are unchanged. **ABI stays 23** — this arc is editor-only.
- **No Problems+Console merge.** They stay two panels over one owner; the only
  change they see is a menu-section label (§4).
- **F4 scope (editor camera/viewport) is untouched**, as always for this arc.

---

## 3. Rulings ledger (user-decided 2026-09-09, in-session)

| # | Question | Ruling |
|---|---|---|
| R1 | May explicit cross-panel commands (Focus in Graph, Reveal in Browse, digest chip) open a closed target panel? | **Focus if open, else disabled.** A command whose target panel is closed renders greyed with a tooltip. **The Window menu is the only thing that ever opens a panel.** Passive selection sync never opens anything (that was already settled). |
| R2 | Per-panel toolbar contract | **Minimal: Browse owns the tools.** Browse = `+ Create` · search. Graph = focus combo only. Status = no toolbar. The menubar Assets ▸ Create submenu (`EditorPanels.cpp:228-259`) is the already-existing global create route when Browse is closed. |
| R3 | Digest chip placement | **Browse + Graph.** Not on Status (redundant on the panel that shows those facts at full size). Status's bottom-bar right slot instead carries a recency line from the activity ring (§9.3). |
| R4 | Panel naming (window title = menu label = ini key) | **Asset Browser / Asset Graph / Asset Status.** Fully qualified — a tab reads correctly wherever it docks. `[Window][Assets]` retires with the rename. |
| R5 | Window-menu final layout | **Board order** — ASSETS: Asset Browser · Asset Graph · Asset Status │ DIAGNOSTICS: Problems · Console │ SCENE: Outliner · Inspector │ separator │ Reset Layout. **Viewport is dropped from the menu** (§4.3). |
| R6 | Code shape | **True file split.** `AssetsPanel.*` retires (the way `AssetBrowser.*` did in Plan 1 Task 15); three panel units + `AssetPanelCommon.*` (§5). |

---

## 4. Panel registry and Window menu

### 4.1 Registry rows

`PanelId::Assets` (`PanelRegistry.hpp:18-22`) is **replaced** by three ids:
`AssetBrowser`, `AssetGraph`, `AssetStatus`. `kPanels`
(`PanelRegistry.hpp:33-40`) carries their rows with names **"Asset Browser"**,
**"Asset Graph"**, **"Asset Status"** — each name is simultaneously the
`ImGui::Begin` title, the Window-menu label, and the visibility ini key, per the
registry's existing contract (`PanelRegistry.hpp:24-29`). All three are
non-permanent (closable; `OpenFlag` returns a real pointer, tabs get an X).

The registry's header contract ("adding a panel = one enum value + one table
row + gating its draw site", `PanelRegistry.hpp:3-8`) holds: id==index order is
preserved (the enum and the table change together), and
`ParsePanelVisibilityLine` needs no change — it is name-keyed.

### 4.2 Menu grouping

`PanelInfo` gains a `section` field:

```cpp
enum class PanelSection : std::uint8_t { Assets, Diagnostics, Scene };
```

The Window menu (`EditorPanels.cpp:270-297`) becomes a two-level loop: for each
section in declaration order (Assets, Diagnostics, Scene), draw a dim uppercase
section label, then every `kPanels` row whose `section` matches, then a
separator before the next section. Final shape:

```
Window
  ASSETS
    ✓ Asset Browser
    ✓ Asset Graph
    ✓ Asset Status
  ──────────────
  DIAGNOSTICS
    ✓ Problems
    ✓ Console
  ──────────────
  SCENE
    ✓ Outliner
    ✓ Inspector
  ──────────────
  Reset Layout
```

Within DIAGNOSTICS the order is **Problems then Console**, matching the G
board. Today's `kPanels` order has Console first; the menu order follows
section iteration + table order, so the Console/Problems rows swap in the
table (with their `PanelId` enum values, preserving id==index) — harmless,
code references are symbolic and ini keys are name-based.

### 4.3 Viewport leaves the menu

Today's Viewport menu item is not a toggle — it is a tab-focuser
(`SelectDockTab("Viewport")`, `EditorPanels.cpp:281-282`). It is **dropped
entirely**, not perma-greyed. Rationale, recorded so nobody re-adds it "for
completeness": the Viewport is permanent and its tab is always physically
present in the central tab bar, even when a shader-editor document tab covers
the view — the tab itself is the recovery route, and the menu item duplicated
it. Mechanically: **the menu loop skips permanent rows** (Viewport is the only
one, and a permanent panel has no toggle to offer anyway), which also retires
the loop's `SelectDockTab` branch (`EditorPanels.cpp:281-282`). Viewport keeps
a valid `section` value (`Scene`) so the registry check in §4.4 stays total;
the `permanent` field and the registry invariants ("exactly one permanent",
permanent-always-visible) are unchanged.

### 4.4 Registry test

`PanelRegistryTest.cpp:15-31` (id==index, unique names, exactly one permanent)
stays green by construction. New checks: every row's `section` is a valid
enum value, and the three asset rows carry `PanelSection::Assets`.

---

## 5. Files and entry points

`AssetsPanel.hpp/.cpp` (349 / 4,979 lines) retires. Four units replace it in
`ArcaneEditor/src/Panels/`:

| Unit | Contents |
|---|---|
| `AssetBrowserPanel.hpp/.cpp` | `DrawAssetBrowserPanel(...)`: toolbar (`+ Create` · search), kind rail, grouped folder table, preview pane, bottom bar. Today's `DrawBrowseLens` body (`AssetsPanel.cpp:1998-2062`) plus the Browse halves of `DrawToolbar`/`DrawBottomBar`, carried over wholesale. |
| `AssetGraphPanel.hpp/.cpp` | `DrawAssetGraphPanel(...)`: focus-combo toolbar, node canvas, legend, bottom bar. The 1,078-line `DrawGraphLens` body (`:3760-4838`) carries over, with the canvas lifecycle: `DestroyAssetsPanelCanvas` renames to `DestroyAssetGraphPanelCanvas`, same two call sites (project switch `EditorApp.cpp:1249`, shutdown `:2950`). The `#include <imgui_node_editor.h>` stays confined to this one .cpp (Plan 3 ruling 1 preserved). The once-per-project boot-scene `graphFocus` seed (`AssetsPanel.cpp:4923-4927`) moves into this panel's preamble. |
| `AssetStatusPanel.hpp/.cpp` | `DrawAssetStatusPanel(...)`: the dashboard body (`DrawStatusLens`, `:2633-3031`) plus its bottom bar. |
| `AssetPanelCommon.hpp/.cpp` | What all three share: `AssetPanelActions` / `AssetPanelServices` (renamed from `AssetsPanel*`, §7), the bottom-bar skeleton + health-digest chip, `DrawCreateMenuEntries` (`:448`), the peek-tooltip anatomy, `RevealAssetInBrowser` (§7.2), and the shared band/spec constants (`kBottomBarHeight`, `kToolbarFramePadY`, `kToolbarBodyGapPx`, kind hues — wherever these are not already in EditorWidgets/EditorTheme). |

**Entry-point signatures** keep today's argument shape minus the lens:

```cpp
AssetPanelActions DrawAssetBrowserPanel(AssetBrowserPanelState&, AssetPanelModel&,
                                        const Arcane::Project*, DocumentHost&,
                                        const AssetPanelServices&, bool* open);
AssetPanelActions DrawAssetGraphPanel(AssetGraphPanelState&, AssetPanelModel&,
                                      const Arcane::Project*, DocumentHost&,
                                      const AssetPanelServices&, bool* open);
AssetPanelActions DrawAssetStatusPanel(AssetPanelModel&,
                                       const Arcane::Project*, DocumentHost&,
                                       const AssetPanelServices&, bool* open);
```

(Status takes no state struct — see §6.)

**Host side:** the single gated draw site (`EditorAppFrame.cpp:2043-2075`)
becomes three, each gated on its own `PanelId` exactly like Console/Problems
(`:2081-2095`). The model is still rebuilt once per frame **before any panel
draws, whether or not any is visible** (`EditorAppFrame.cpp:2042`, rationale at
`:2030-2039` — unchanged, and now load-bearing for three windows instead of
three lenses). The host consumes three returned `AssetPanelActions` values;
consumption is identical per value, so the existing handler code runs over each
in turn (draw order: Browser, Graph, Status — and see §7.4 for why ordering is
not semantically load-bearing).

The precedent for all of this is Console+Problems over `ConsoleDiagnostics`
(`EditorApp.hpp:753-764`): two free-function panels, per-panel UI state structs,
one shared owner, docked as sibling tabs.

---

## 6. State split

`AssetsPanelState` (`AssetsPanel.hpp:66-219`) dissolves. **`AssetLens` and
`state.lens` cease to exist.**

```cpp
struct AssetBrowserPanelState {
    char search[128] = {};
    int  railKind = -1;
    std::uint32_t seenSelectionStamp = 0;
    float previewPaneWidth = kAssetsPreviewPaneDefaultWidth;
    std::unordered_map<std::string, bool>  groupOpen;    // mirrors, as today
    std::unordered_map<Arcane::Guid, bool> childrenOpen; // (absent == default)
};

struct AssetGraphPanelState {
    void* graphCanvas = nullptr;      // opaque ax::NodeEditor context, as today
    GraphGridPhase graphGrid;
    Arcane::Guid graphFocus;
    bool graphFocusSeeded = false;
    std::uint32_t seenSelectionStampGraph = 0;
    Arcane::Guid graphMenuGuid, graphHoverGuid, graphWireGuid, graphDragGuid;
    float graphHoverSeconds = 0.0f;
    bool graphWireDerivable = false, graphDragRight = false;
    AssetGraphViewModel graph;
    std::uint32_t graphBuiltStamp = 0; Arcane::Guid graphBuiltFocus;
    bool graphBuilt = false, graphLayoutDirty = false;
};
// Status: NO state struct — the dashboard is entirely derived from the model
// and services; its only state writes today are the deep-links, which become
// actions (§7).
```

Field-for-field these are today's `AssetsPanelState` partitioned along the
grouping its own comments already draw (`AssetsPanel.hpp:66-218`); every field
comment carries over. Notes:

- **The every-frame `SetSearch`/`SetKindFilter` push** (`AssetsPanel.cpp:514-515`)
  moves into the Browser panel's toolbar and nowhere else. This is correct by
  construction: `Rows()`/`Rail()`/`ShownAssetCount()` are the **only** filtered
  views of the model, and only Browse reads them. Graph builds from unfiltered
  `Entries()` + `RefIndex()` (`AssetPanelModel.hpp:601-614`: "a search keystroke
  rebuilds Rows() and nothing else, and the Graph lens does not read Rows()");
  Status reads unfiltered aggregates (`Health()`, `Entries()`, `UnusedGuids()`,
  `RefIndex()`). Browse's filter never leaks into the other panels.
- **`seenSelectionStamp` / `seenSelectionStampGraph`** stay two independent
  stamp consumers — the pattern was built for exactly this (Plan 3 ruling 4)
  and generalizes unchanged.
- **Ownership:** `EditorApp.hpp:1184`'s `m_assetsPanel` becomes
  `m_assetBrowserUi` + `m_assetGraphUi`.
- **Project switch is preserved, not extended:** only the graph teardown runs
  there (`DestroyAssetGraphPanelCanvas` beside `ResetForProjectSwitch` and the
  activity `Clear()`, `EditorApp.cpp:1237-1253`). The Browser mirrors tolerate
  stale keys across projects exactly as today — absent == default, stale keys
  are never queried.
- **Session-only remains the rule** for all of the above (old spec §5's "panel
  state is session-only in v1" survives the shell it was written for).

---

## 7. Cross-panel commands

### 7.1 The routing change

`state.lens` had three writers, all writing it directly from inside a lens body
(digest chip `AssetsPanel.cpp:704`, Reveal `:2523`, Focus in Graph `:2612`) —
deliberately not routed through actions, with the rationale recorded at
`:2550-2557`: switching lenses inside one panel was not a host effect. **The
split invalidates that rationale**: these are now cross-window commands, and
they move into `AssetPanelActions`, joining `showProblems` (`AssetsPanel.hpp:263`)
which already has exactly this shape (raised `:2178-2179`, consumed
`EditorAppFrame.cpp:2447-2451`).

```cpp
// added to AssetPanelActions:
bool         showStatus = false;   // digest chip (Browser/Graph → Status)
Arcane::Guid revealInBrowse;       // Status's Unreferenced card → Browser
Arcane::Guid focusInGraph;         // Status's scene card → Graph
```

Host consumption, all under R1 (focus if open, else the panel already disabled
the control; the host still re-checks visibility and no-ops when closed — it
never opens):

- **`showStatus`** → `SelectDockTab("Asset Status")`. Nothing else — no
  selection or filter change, today's semantics (`AssetsPanel.cpp:695-704`).
- **`focusInGraph`** → `m_assetGraphUi.graphFocus = guid`;
  `m_assetModel.Select(guid)`; `SelectDockTab("Asset Graph")`. The old
  focus-before-lens-flip ordering dance (`AssetsPanel.cpp:2603-2610`) dissolves:
  the host writes both before any next-frame draw, and the Graph panel's own
  rebuild trigger (`graphBuiltStamp`/`graphBuiltFocus` vs the model) does the
  rest.
- **`revealInBrowse`** → `RevealAssetInBrowser(m_assetBrowserUi, m_assetModel,
  guid)` (shared helper in `AssetPanelCommon`), then
  `SelectDockTab("Asset Browser")`.

### 7.2 `RevealAssetInBrowser`

Today's reveal sequence (`AssetsPanel.cpp:2467-2525`) runs from inside the
Status body and writes Browse's state mirrors directly — a layering wart the
split would turn into one panel reaching into another panel's state. It becomes
a free function in `AssetPanelCommon`, performing today's exact sequence:
clear search both places (`model.SetSearch("")` + `state.search[0]='\0'`),
clear the kind filter both places, walk the entry's folder ancestry forcing
every group open both places, force the derived fold open when `foldedUnder`
is valid, then `model.Select(guid)`. Browse's scroll-to-selection machinery
(`:1481-1495`) finishes the job on its next draw, unchanged. Called only by the
host (action consumption); the Status panel itself never touches Browser state
again.

### 7.3 Disabled-state mechanics

`AssetPanelServices` gains three host-filled bools:

```cpp
bool browserOpen = false, graphOpen = false, statusOpen = false;
```

filled each frame from `m_panelVis` (`IsVisible(...)`) at the same site the
services struct is populated today. Controls disable + explain when their
target is closed:

- **Focus in Graph** (scene card) — greyed when `!graphOpen`; tooltip
  "Asset Graph is closed — open it from Window ▸".
- **Reveal** (Unreferenced card) — greyed when `!browserOpen`; same tooltip
  form.
- **Digest chip** (Browser + Graph bottom bars) — the counts **always render**
  (it is information first); only the click affordance goes inert when
  `!statusOpen`, with the tooltip on hover. The chip never disappears.
- **Problems button** (Status's refused card) — comes under the same rule:
  greyed when Problems is closed, and the host's consumer **drops the un-hide**
  (`EditorAppFrame.cpp:2447-2449`'s `visible[...] = true` line goes away; the
  `SelectDockTab` stays). This is a deliberate behavior change to a shipped
  control, user-approved: under R1, *nothing* opens a panel except the Window
  menu.

**Known inconsistency, recorded and accepted:** Edit ▸ Rename still surfaces
the Outliner by un-hiding it (`EditorAppFrame.cpp:2192-2193`). That is outside
this arc's scope and is left as-is; if R1 is ever promoted to an editor-wide
rule, that site is the one to revisit.

### 7.4 The retired hazard

`AssetsGraphProjectionIsCurrent` (`AssetsPanel.hpp:311-330`,
`AssetsPanel.cpp:4840-4860`, consumed by the bottom bar at `:646`) existed
because a same-frame lens flip could put the Graph bottom bar on screen in a
frame whose body was another lens's. Post-split the Graph bar draws only inside
the Graph window, after the Graph body, in the same `Begin`/`End` — the
stale-projection frame is unrepresentable. **The predicate is deleted.** Its
pinning test is replaced, not ported (§12). Action consumption happens after
all draws (today's pattern), so a `focusInGraph` raised this frame takes effect
in the next frame's Graph draw — body before bar, projection current, no
ordering hazard among the three panels.

---

## 8. Selection

**Zero work, by design.** The selection already lives on the shared model
(`AssetPanelModel.hpp:598-615` — "THE shared selection", stamp bumped on real
changes only), and every consumer already reads it from there: Browser rows,
preview pane, Status cards, Graph's three-part bridge (8a/8b/8c,
`AssetsPanel.cpp:4552-4637`), the Inspector fallback
(`EditorAppFrame.cpp:3091`), and the menubar's `hasAssetSelection` gate
(`:1992`). The Graph bridge's documented invariant — "deselecting inside one
lens does not clear the selection every other lens and the Inspector are
pointing at" (`AssetsPanel.cpp:4603-4608`) — carries over verbatim with lens →
panel.

The user's sync rule ("one open view selects → the others follow") is exactly
this existing behavior, now visible across simultaneous windows. The
never-auto-open half holds trivially: no selection path touches panel
visibility, and after this spec no path but the Window menu does at all.

---

## 9. Per-panel chrome

### 9.1 Toolbars (R2 — minimal)

- **Asset Browser:** `+ Create` · search (flex width, 80px floor — today's
  math minus the lens-strip and per-lens-slot subtraction,
  `AssetsPanel.cpp:491-509`). `kToolbarFramePadY` and the 7px toolbar-body gap
  carry over.
- **Asset Graph:** the focus combo only (`kGraphFocusComboWidth = 230.0f`),
  left-anchored. No Create, no search.
- **Asset Status:** **no toolbar** — the stat tiles start at the top of the
  body. No toolbar-body gap (there is no toolbar to gap from).
- The lens strip (`SegmentedStrip`, `AssetsPanel.cpp:560-564`) has no remaining
  caller in these panels. `kLensLabels` / `kLensCount` / `kLensEnabledMask`
  die with `AssetLens`. (`SegmentedStrip` itself stays in EditorWidgets — it is
  a general widget.)
- **Global create fallback:** the menubar Assets ▸ Create submenu
  (`EditorPanels.cpp:228-259`) already raises the identical
  `CreateAssetKind` → `BeginCreateAsset` route, so create remains reachable
  when only Graph/Status are open. No new work; recorded here because R2's
  slimming depends on it.

### 9.2 Bottom bars

The Polish-8 skeleton survives per-panel: LEFT = context, RIGHT = one glanceable
fact; hairline divider painted by drawlist as today (`AssetsPanel.cpp:599-604`).

| Panel | Left (context) | Right |
|---|---|---|
| Asset Browser | "N assets · M selected" (filtered form when filtering, as today `:664-669`) | health-digest chip |
| Asset Graph | "X of N · focus: `<scene>`" (`:650-660`, no longer predicate-gated — §7.4) | health-digest chip |
| Asset Status | "N assets · M need attention" (`:661-663`) | **recency line** (§9.3) |

The digest chip is unchanged in composition (amber `⚠ N refused` + dim
`· N cooking · N unused`, `:672-693`), reads `model.Health()` (always current —
the model rebuilds before any panel draws), and keeps its single-hit-target
click-through, now raising `showStatus` (§7.1) instead of writing a lens.
`DrawBottomBar`'s non-const-state justification (`:577-584`) dies with the lens
write; the shared skeleton takes the panel's own state const (or not at all).

### 9.3 Status's recency line (R3)

Status's bottom-bar right slot carries the newest activity-ring entry:

```
last change 2m ago · uv_marker.png
```

sourced from `services.activity` (which Status already consumes for the feed;
null-guarded as today — the slot is empty when the log is null or empty).
Dim text, no click-through. Chosen over a cooked-ratio or live-queue line
because it is the one glanceable fact not already carried by the tiles/meter,
and it stays useful while the feed is scrolled away. The relative-time
formatting reuses the feed's own.

### 9.4 Preview pane and peek tooltip

The preview pane stays in Asset Browser — it is purely selection-driven and
takes no state (`AssetsPanel.cpp:1754-1763`). The peek-tooltip anatomy (one
shared shape across all rows/nodes/cards, per the interaction contract) moves
to `AssetPanelCommon` and serves all three panels.

---

## 10. Default layout and persistence

**Defaults.** Visibility defaults stay all-true (`PanelRegistry.hpp:47`).
`BuildDefaultLayout` (`EditorPanels.cpp:361-385`) docks all three into the
existing bottom node as tabs:

```
[ Asset Browser | Asset Graph | Asset Status | Console | Problems ]
```

with Asset Browser selected. The G board's side-by-side arrangement (Browser +
Status tabbed left, Graph right) is a *user* arrangement the split makes
possible, not the shipped default. Reset Layout re-shows and re-tabs
everything, as today (`EditorAppFrame.cpp:2024-2026`).

**Persistence.** Mechanism unchanged: open/closed per panel via the
`[EditorPanels][Visibility]` custom handler (`EditorApp.cpp:199-246`) — now
three name-keyed lines where `Assets=` was one — and dock geometry in the same
per-project imgui.ini (`RetargetLayoutIni`, `EditorApp.cpp:1328-1346`). Panel
view state remains session-only (§2). Nothing new is persisted in this arc.

---

## 11. Relationship to the 2026-09-06 spec

`docs/specs/2026-09-06-asset-manager-redesign-design.md` remains the governing
document for everything **inside** the panels. This spec supersedes exactly one
clause of it:

- **Superseded — §5's shell:** the one-panel three-band anatomy with the lens
  strip ("layout pinned from day one, later plans enable, nothing shifts") is
  replaced by the three-panel shell defined here. The lens strip retires.
- **Still binding:** the lens bodies and their rulings (§9.2 Status dashboard,
  §10 Graph scoping, the whole of Plan 3's §19 record), the §11/§11.2 value and
  kind-hue tables, the band constants, the create-flow invariant ("no creation
  path may bypass CreateAssetRequest"), the interaction contract (peek tooltip,
  shared selection, double-click opens), and the fidelity requirement for
  everything the boards still govern.

Where the two documents disagree on the shell, **this document wins**; where
this document is silent, the 2026-09-06 spec (as amended by its §17/§19
records) stands.

---

## 12. Testing

- **`PanelRegistryTest.cpp`** — existing invariants (`:15-31` id==index /
  unique names / exactly one permanent; `:33-52` visibility; `:54-76` ini
  parse) stay green by construction. New checks: every row's `section` is
  valid; the three asset rows are `PanelSection::Assets`; parse round-trip for
  the three new names.
- **`AssetsGraphCanvasTest.cpp`** — its four cases re-target
  `DrawAssetGraphPanel` + `AssetGraphPanelState`; the 8 direct `state.lens`
  writes disappear. Three cases carry over nearly verbatim (device-less frames
  `:117`, id-space `:774`, pin-drag derive `:918`, quiet no-op `:1043` — all
  were Graph-body tests). The **Focus-in-Graph transition case (`:425-480`) is
  replaced, not ported**: its scenario ("lens is Graph, focus moved, no rebuild
  ran because another lens's body drew") is unrepresentable post-split. The
  replacement pins the new mechanism end-to-end at the panel level: draw Status
  device-less, click the scene card's Focus in Graph (with `graphOpen = true`),
  assert `actions.focusInGraph` carries the guid and nothing wrote Graph state
  directly; then apply the host's consumption by hand (focus + select) and
  assert the next `DrawAssetGraphPanel` frame rebuilds the projection.
- **New: disabled-rule coverage** (device-less): with `statusOpen = false`, a
  digest-chip click raises no `showStatus`; with `graphOpen = false`, the scene
  card's button is disabled and raises nothing; same for Reveal vs
  `browserOpen` and the Problems button vs a closed Problems panel.
- **`GoldenImageTest.cpp:32-58`** (seed sanity) — closes a found gap: assert
  the committed seed carries `[Window][Asset Browser]`,
  `[Window][Asset Graph]`, `[Window][Asset Status]` and does **not** carry a
  line-anchored `[Window][Assets]`, so a stale seed after the rename fails
  loudly instead of silently re-ghosting.
- **`AssetPanelModelTest.cpp`** — untouched; the model does not change.
- Suites run **from the exe dir**, as always.

## 13. Goldens and the layout seed

Both lanes re-bless, Arc 2 discipline throughout (assert `gatePassed` + per-lane
`verdict` from the JSON only; a bless restages BOTH hosts).

- **`ReferenceProject/Saved/verify-layout.ini`** — regenerated via
  `--dump-layout` per the seed's own documented procedure (`:58-64`), after the
  split builds and the default layout is up. **Move the old seed aside first**
  — an already-tainted seed re-emits its ghost `[Window]` entries verbatim.
  Expected shape of the new seed: three asset `[Window]` sections, the
  visibility block gains `Asset Browser=1` / `Asset Graph=1` /
  `Asset Status=1` (and loses `Assets=1`), the bottom dock node's `Selected=`
  hash becomes the Asset Browser tab's.
- **`ReferenceProject/Verify/References/editor-ui.png`** — full re-bless on
  **both** backends (dx12 + vulkan, `scripts/golden-gate.ps1:305-306`): bless
  SOURCE, restage both hosts, rerun green. The diff will be large (the bottom
  node's tab bar and chrome change wholesale); read the diff artifact before
  blessing and copy it into the SDD workspace first (the gate deletes stale
  diff PNGs on each run).

## 14. Canvas restamp

The design canvas (artifact `de210519-110a-47a1-b5f8-9d82b4ec421b`; local
working set `.superpowers/design/asset-manager-mockups/extract-20260909/`)
restamps with this spec:

- **Option G → FINAL**, with this spec's amendments recorded on the board:
  minimal toolbars (R2), digest on Browser+Graph only with Status's recency
  line (R3), qualified names (R4), Viewport off the Window menu (R5),
  focus-if-open rule (R1).
- **Option F → not chosen** (multi-instance/UE pattern; recorded, not adopted).
- **The one-panel FINAL boards → "superseded (shell) / bodies survive"** — the
  lens bodies, values, and interaction contract they pin remain the redline
  for panel *contents*.
- The top-left decision-record note points at this spec as the shell authority.

## 15. Hazards ledger (found in the pre-spec code survey, each addressed)

| Hazard | Where addressed |
|---|---|
| `state.lens` had 4 readers / 3 writers | §7.1 — writers become actions; readers die with the enum |
| Digest chip is one hit target; per-count links would be new UI | §2 non-goals — stays one target |
| `AssetsGraphProjectionIsCurrent` papered over the same-frame flip | §7.4 — deleted; test replaced §12 |
| Shared toolbar pushed `search`/`railKind` into the shared model every frame | §6 — Browser-only by construction (filtered views are Browse-only) |
| Reveal wrote Browse mirrors from the Status body | §7.2 — `RevealAssetInBrowser`, host-called |
| `DestroyAssetsPanelCanvas` keyed to one state | §5 — renamed, takes `AssetGraphPanelState`, same two sites |
| Both golden lanes invalidated | §13 |
| Window menu would hit 8 flat entries | §4.2 — sections; §4.3 — Viewport dropped |
| Multi-window writes to one model in one frame (Material-panel precedent's gesture hazard) | §7.4 — all cross-panel effects routed through host action consumption after draws; in-panel writes remain single-writer per field (§6, §8) |

---

*Sequencing note: the foundation (Plans 1–3 + the pin-drag fix wave) is pushed
at `cc14a609`; this spec starts the split arc proper. Implementation follows
via the house flow — plan(s) written from this spec, then SDD. ABI stays 23
throughout; the Gacha Game-DLL rebuild debt is unchanged by this arc.*
