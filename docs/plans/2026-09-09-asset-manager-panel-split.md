# Asset Manager Panel Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the one-panel asset manager (Browse/Graph/Status lenses behind a lens strip) into three separate dockable panels — Asset Browser, Asset Graph, Asset Status — over the unchanged shared backend.

**Architecture:** Strangler sequence: registry/menu groundwork first, then the shared unit, then deep-links converted to host-routed actions while still one panel, then three pure code-motion extractions, then ONE cutover task that swaps the shell (registry rows, state split, wrappers, deletions), then chrome finish and the two-lane golden re-bless. Every task ends buildable and green.

**Tech Stack:** C++23, Dear ImGui (+ imgui-node-editor confined to the Graph unit), Catch2, msbuild `Arcane.slnx`, `scripts/golden-gate.ps1`.

**Spec:** `docs/specs/2026-09-09-asset-manager-panel-split-design.md` (rulings R1–R6 in its §3). The 2026-09-06 spec still governs panel *contents* (its §5 shell clause is superseded — split spec §11).

## Global Constraints

- **ABI stays 23.** Editor-only arc; no engine seam changes, no `Aphelyon.arcproj` restamp.
- **Repo:** `D:\dev\starworks\Arcane`, branch `main`. Baseline `cc14a609` + spec commit. **`out.txt` at repo root is the user's — never stage it.**
- All file:line anchors are against `cc14a609`; they drift as tasks land. **Re-locate by symbol name before editing; ranges are orientation, not gospel.**
- Build: `msbuild Arcane.slnx /p:Configuration=Debug /m` from the repo root (VS 2026 dev prompt).
- **Run tests FROM the exe dir:** `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "<filter>"`. Test order is randomized — capture the seed banner into any report that cites a run.
- Authored ImGui tables use `ImGuiTableFlags_NoSavedSettings` (imgui.ini vetoes authored changes otherwise).
- Spec §13 of the 2026-09-06 spec still binds: never render an unknown as a zero (unknown = em dash `\xE2\x80\x94`).
- `.superpowers/` is gitignored; the mockups/renders there stay on disk, never staged.
- **Do not push** until the user's desk pass at the end of the arc.
- Commit per task, conventional style (`feat(editor):` / `refactor(editor):` / `test(editor):` / `docs:`), ending with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc`

---

### Task 1: Panel sections + grouped Window menu (today's panels)

Registry gains the `section` field (spec §4.2); the Window menu becomes the grouped shape with **today's** panel set (single "Assets" row — the three-way swap is Task 7). Viewport leaves the menu (spec §4.3, ruling R5). Console/Problems rows swap so DIAGNOSTICS reads Problems-then-Console.

**Files:**
- Modify: `ArcaneEditor/src/Panels/PanelRegistry.hpp` (whole table region, `:18-41`)
- Modify: `ArcaneEditor/src/Panels/EditorPanels.cpp:270-297` (Window menu)
- Test: `ArcaneTests/src/PanelRegistryTest.cpp`

**Interfaces:**
- Produces: `enum class PanelSection : std::uint8_t { Assets, Diagnostics, Scene, Count }`; `PanelInfo::section`; `kSectionLabels[]`; `kSectionMenuOrder[]`. Consumed by the menu loop here and by Task 7's new rows.
- `PanelId` order becomes `Viewport, Outliner, Inspector, Assets, Problems, Console` (Problems/Console swap — code references are symbolic, ini keys name-based, so this is safe).

- [ ] **Step 1: Write the failing registry test**

Append to `PanelRegistryTest.cpp`:

```cpp
TEST_CASE("panel table: every row carries a valid section; section labels exist",
          "[editor]")
{
    for (const PanelInfo& p : kPanels)
        CHECK(static_cast<std::size_t>(p.section) <
              static_cast<std::size_t>(PanelSection::Count));
    // One label per section, and the menu order names each section once.
    CHECK(std::size(kSectionLabels) == static_cast<std::size_t>(PanelSection::Count));
    std::set<PanelSection> seen;
    for (PanelSection s : kSectionMenuOrder)
        CHECK(seen.insert(s).second);
    CHECK(seen.size() == static_cast<std::size_t>(PanelSection::Count));
    // The assets row sits in the ASSETS section (Task 7 adds two siblings).
    CHECK(kPanels[static_cast<std::size_t>(PanelId::Assets)].section == PanelSection::Assets);
    // DIAGNOSTICS order is Problems then Console (board order, spec s4.2).
    CHECK(static_cast<std::size_t>(PanelId::Problems) <
          static_cast<std::size_t>(PanelId::Console));
}
```

- [ ] **Step 2: Run it — expect FAIL (`PanelSection` undeclared)**

Run: `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "[editor]"` — expect a compile failure first, which for a header change counts as the red step.

- [ ] **Step 3: Implement the registry change**

In `PanelRegistry.hpp` replace the enum + table region (`:18-41`) with:

```cpp
    enum class PanelId : std::uint8_t
    {
        Viewport, Outliner, Inspector, Assets, Problems, Console,
        Count
    };

    // Window-menu grouping (panel-split spec s4.2). Menu order is
    // kSectionMenuOrder below, NOT enum order.
    enum class PanelSection : std::uint8_t { Assets, Diagnostics, Scene, Count };

    inline constexpr const char* kSectionLabels[] = { "ASSETS", "DIAGNOSTICS", "SCENE" };
    static_assert(std::size(kSectionLabels) == static_cast<std::size_t>(PanelSection::Count));

    inline constexpr PanelSection kSectionMenuOrder[] = {
        PanelSection::Assets, PanelSection::Diagnostics, PanelSection::Scene,
    };

    struct PanelInfo
    {
        PanelId      id;
        const char*  name;       // ImGui::Begin title AND menu label AND ini key
        bool         permanent;  // true = no X, cannot hide (Viewport only today)
        PanelSection section;    // Window-menu group (permanent rows are never listed)
    };

    // Names must match each panel's ImGui::Begin title exactly
    // (PanelRegistryTest pins the invariants).
    inline constexpr PanelInfo kPanels[] = {
        { PanelId::Viewport,  "Viewport",  true,  PanelSection::Scene       },
        { PanelId::Outliner,  "Outliner",  false, PanelSection::Scene       },
        { PanelId::Inspector, "Inspector", false, PanelSection::Scene       },
        { PanelId::Assets,    "Assets",    false, PanelSection::Assets      },
        { PanelId::Problems,  "Problems",  false, PanelSection::Diagnostics },
        { PanelId::Console,   "Console",   false, PanelSection::Diagnostics },
    };
    static_assert(std::size(kPanels) == static_cast<std::size_t>(PanelId::Count));
```

Add `#include <set>` is already in the test file. Keep `PanelVisibility` and `ParsePanelVisibilityLine` untouched.

- [ ] **Step 4: Rebuild + run `[editor]` — expect PASS** (existing three cases + the new one; the Problems/Console enum swap is invisible to them because every check is symbolic).

- [ ] **Step 5: Regroup the Window menu**

Replace the loop at `EditorPanels.cpp:270-297` (the Window `BeginMenu` body) with:

```cpp
            if (ImGui::BeginMenu("Window"))
            {
                // Grouped sections over the registry (panel-split spec s4.2):
                // dim section label, that section's panels, separator. The
                // Viewport (permanent) is deliberately NOT listed -- its tab is
                // always physically present in the central tab bar, so a menu
                // row would duplicate it (spec s4.3; do not re-add "for
                // completeness").
                for (PanelSection section : kSectionMenuOrder)
                {
                    ImGui::TextDisabled("%s", kSectionLabels[static_cast<std::size_t>(section)]);
                    for (const PanelInfo& p : kPanels)
                    {
                        if (p.section != section || p.permanent)
                            continue;
                        ImGui::MenuItem(p.name, nullptr,
                                        &panels.visible[static_cast<std::size_t>(p.id)]);
                    }
                    ImGui::Separator();
                }
                // Rebuild the stock dock layout (the first-run path, on
                // demand) and re-show everything -- also the standing cure for
                // an old imgui.ini hiding newly shipped panels.
                if (ImGui::MenuItem("Reset Layout"))
                    requests.resetLayout = true;
                ImGui::EndMenu();
            }
```

- [ ] **Step 6: Full build + full `[editor]` suite green; launch the editor once** (`bin/Debug-windows-x86_64-md/ArcaneEditor/ArcaneEditor.exe`) and eyeball the Window menu: three labeled groups, no Viewport row, Reset Layout after the final separator. (Menu drawing has no unit seam; the golden lane in Task 9 is its regression net.)

- [ ] **Step 7: Commit** — `feat(editor): grouped Window menu with panel sections; Viewport row retired`

---

### Task 2: AssetPanelCommon + the Actions/Services rename (zero behavior)

Create the shared unit and move the cross-panel contracts into it, renaming `AssetsPanelActions`→`AssetPanelActions` and `AssetsPanelServices`→`AssetPanelServices` (spec §5). Pure motion + rename; no behavior delta.

**Files:**
- Create: `ArcaneEditor/src/Panels/AssetPanelCommon.hpp`, `ArcaneEditor/src/Panels/AssetPanelCommon.cpp`
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.hpp` (structs move out, `:230-296`), `ArcaneEditor/src/Panels/AssetsPanel.cpp` (`DrawCreateMenuEntries`/`DrawCreateMenu` move out, `:448-471`; includes), `ArcaneEditor/src/App/EditorApp.hpp`, `ArcaneEditor/src/App/EditorAppFrame.cpp` (type-name rename at the seam sites), `ArcaneTests/src/AssetsGraphCanvasTest.cpp` (type names)
- Test: existing suites (rename ripple only)

**Interfaces:**
- Produces: `AssetPanelCommon.hpp` declaring — `struct AssetPanelActions` (field-for-field today's `AssetsPanelActions`, `AssetsPanel.hpp:230-264`, comments carried), `struct AssetPanelServices` (today's `AssetsPanelServices`, `:276-296`), `void DrawCreateMenuEntries(AssetPanelActions&, bool enabled)`, `void DrawCreateMenu(AssetPanelActions&)`, and the shared constants `kAssetPanelBottomBarHeight = 24.0f`, `kAssetPanelToolbarFramePadY = 4.0f`, `kAssetPanelToolbarBodyGapPx = 7.0f` (today's file-local `kBottomBarHeight`/`kToolbarFramePadY`/`kToolbarBodyGapPx`, `AssetsPanel.cpp:52-62`; AssetsPanel.cpp switches to the new names).
- Consumes: `CreateAssetKind` (`Panels/CreateAssetDialog.hpp`), icon defines, `Arcane::Guid`.

- [ ] **Step 1: Create `AssetPanelCommon.hpp`** — header comment: "Shared contracts and chrome for the three asset panels (panel-split spec s5). One actions type, one services type, one create menu — every panel returns/consumes the same shapes so the host consumes them identically." Move the two structs verbatim (keep every field comment; only the type names change). Declare the two create-menu functions and the three constants.
- [ ] **Step 2: Create `AssetPanelCommon.cpp`** — move `DrawCreateMenuEntries` + `DrawCreateMenu` bodies verbatim from `AssetsPanel.cpp:448-471` (they were file-local; now exported — keep their doc comments).
- [ ] **Step 3: Repoint consumers.** `AssetsPanel.hpp` includes `Panels/AssetPanelCommon.hpp` and drops its own copies; add `using AssetsPanelActions = AssetPanelActions;` / `using AssetsPanelServices = AssetPanelServices;` **temporarily?** NO — do the honest rename now: update the ~dozen spellings in `AssetsPanel.cpp`, `EditorApp.hpp`, `EditorAppFrame.cpp` (`:2040`, `:2058`, `ConsumeBrowserActions` signature), `AssetsGraphCanvasTest.cpp`. No aliases left behind.
- [ ] **Step 4: Regenerate the solution if the new files need it** (`GenerateProjects.bat` — premake globs `src/**`, so new files under `src/Panels/` are picked up; run it anyway to be safe), full rebuild, full suite from the exe dir — counts must match the pre-task run exactly (zero behavior delta).
- [ ] **Step 5: Commit** — `refactor(editor): extract AssetPanelCommon; rename asset panel actions/services types`

---

### Task 3: Deep-links become host-routed actions (still one panel)

Convert the three `state.lens` writers into `AssetPanelActions` fields the host consumes (spec §7.1), add the visibility bools + disabled gates (spec §7.3, ruling R1), and extract `RevealAssetInBrowser` (spec §7.2). The host still consumes them by writing `state.lens` — behavior identical; the routing machinery is what lands. The Problems button comes under the R1 rule here too.

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetPanelCommon.hpp/.cpp` (new fields + helper), `ArcaneEditor/src/Panels/AssetsPanel.cpp` (digest `:695-704`, Reveal `:2467-2525`, Focus-in-Graph `:2601-2618`, Problems button `:2178-2179`), `ArcaneEditor/src/App/EditorAppFrame.cpp` (services fill `:2058-2072`, `ConsumeBrowserActions` tail `:2447-2451`)
- Test: Create `ArcaneTests/src/AssetPanelCommonTest.cpp`; extend `ArcaneTests/src/AssetsGraphCanvasTest.cpp`

**Interfaces:**
- Produces on `AssetPanelActions`: `bool showStatus = false;`, `Arcane::Guid revealInBrowse;`, `Arcane::Guid focusInGraph;` (beside the existing `showProblems`/`recook`).
- Produces on `AssetPanelServices`: `bool browserOpen = false, graphOpen = false, statusOpen = false, problemsOpen = false;` — host-filled from `m_panelVis` every frame.
- Produces in Common: `void RevealAssetInBrowser(AssetsPanelState& state, AssetPanelModel& model, const Arcane::Guid& guid);` (Task 7 retargets the first parameter to `AssetBrowserPanelState&`).

- [ ] **Step 1: Write the failing helper test** (`AssetPanelCommonTest.cpp` — headless, no ImGui, modeled on `AssetPanelModelTest.cpp`'s model-building):

```cpp
// RevealAssetInBrowser (panel-split spec s7.2): today's Reveal sequence
// (AssetsPanel.cpp Unreferenced card) as a host-callable helper -- clears
// search + kind filter BOTH places, walks the folder ancestry open BOTH
// places, forces the derived fold, selects.
TEST_CASE("RevealAssetInBrowser clears filters, opens ancestry, selects",
          "[editor]")
{
    AssetPanelModel model;
    /* build a model with a nested entry the way AssetPanelModelTest's
       fixtures do: an entry whose folder is "Content/props/crates", plus a
       derived child folded under a texture. */
    AssetsPanelState state;
    std::snprintf(state.search, sizeof(state.search), "zzz-no-match");
    state.railKind = 2;
    model.SetSearch(state.search);
    model.SetKindFilter(state.railKind);
    state.groupOpen["Content/props"] = false;
    model.SetGroupOpen("Content/props", false);

    RevealAssetInBrowser(state, model, targetGuid);

    CHECK(state.search[0] == '\0');
    CHECK(state.railKind == -1);
    CHECK(!model.Filtered());
    CHECK(state.groupOpen.at("Content/props"));
    CHECK(state.groupOpen.at("Content/props/crates"));
    CHECK(model.selected == targetGuid);
}
```

(Executor: lift the exact fixture-building calls from `AssetPanelModelTest.cpp` — the assertions above are the contract; the fold-open leg needs a `foldedUnder` fixture too, assert `state.childrenOpen.at(textureGuid)`.)

- [ ] **Step 2: Run — expect FAIL** (`RevealAssetInBrowser` undeclared).
- [ ] **Step 3: Implement `RevealAssetInBrowser`** in `AssetPanelCommon.cpp` by MOVING the body of the Unreferenced card's reveal (`AssetsPanel.cpp:2467-2525`, minus the trailing `state.lens = Browse` line and the button chrome): search clear both places, kind clear both places, ancestry walk `SetGroupOpen` both places, fold-open when `foldedUnder` valid, `model.Select(guid)`. The card's click handler shrinks to `actions.revealInBrowse = e->guid;` under a `BeginDisabled(!services.browserOpen)` gate with the tooltip `"Asset Browser is closed \xE2\x80\x94 open it from Window \xE2\x96\xB8"` on `ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)` (mirror the Build menu's disabled-tooltip idiom, `EditorPanels.cpp:312-315`).
- [ ] **Step 4: Run — expect PASS.**
- [ ] **Step 5: Convert the other three raise sites** (same disabled-gate + tooltip pattern, target-named text):
  - Digest click (`AssetsPanel.cpp:702-704`): `if (ImGui::InvisibleButton(...) && services.statusOpen) actions.showStatus = true;` — counts always render; only the click gates (spec §7.3). Tooltip on hover when `!services.statusOpen`.
  - Focus in Graph (`:2601-2618`): raise `actions.focusInGraph = e.guid;` — the direct `state.graphFocus`/`state.lens`/`model.Select` writes move to the host consumer. Gate on `services.graphOpen`.
  - Problems button (`:2178-2179`): gate on `services.problemsOpen` (raise unchanged).
- [ ] **Step 6: Host side.** Fill the four bools in the services block (`EditorAppFrame.cpp:2058-2072`) from `m_panelVis.IsVisible(...)` (all `PanelId::Assets` for the first three until Task 7; `PanelId::Problems` for the fourth). In `ConsumeBrowserActions`: consume `showStatus` → `m_assetsPanel.lens = AssetLens::Status`; `focusInGraph` → `m_assetsPanel.graphFocus = g; m_assetModel.Select(g); m_assetsPanel.lens = AssetLens::Graph;`; `revealInBrowse` → `RevealAssetInBrowser(m_assetsPanel, m_assetModel, g); m_assetsPanel.lens = AssetLens::Browse;`. **Change `showProblems` consumption to R1 semantics now:** delete the un-hide line (`:2449`), keep `SelectDockTab("Problems")` — and update its comment to cite the split spec's R1 (the old "un-hide then select" prose is now false).
- [ ] **Step 7: Click-path test** (append to `AssetsGraphCanvasTest.cpp`, reusing `GraphMouseHarness` + `MaterialHubFixture`):

```cpp
TEST_CASE("digest chip raises showStatus only while the Status target is open",
          "[editor][graphcanvas]")
{
    /* fixture + harness setup exactly as the pin-drag case (:918-952) */
    hw.services.statusOpen = true;
    for (int i = 0; i < 3; ++i) hw.Frame();
    // The digest ends flush at the bottom bar's right edge; click just
    // inside it. POSITIVE CONTROL FIRST -- if this leg misses the chip, the
    // test fails loudly here instead of passing vacuously below.
    const ImVec2 chip(hw.origin.x + hw.size.x - 20.0f,
                      hw.origin.y + hw.size.y - kAssetPanelBottomBarHeight * 0.5f);
    hw.MoveTo(chip); hw.Frame(); hw.Button(true); hw.Frame(); hw.Button(false); hw.Frame();
    REQUIRE(hw.lastActions.showStatus);          // control: the click DOES land

    hw.lastActions = {};
    hw.services.statusOpen = false;              // target closed -> R1 disables
    hw.MoveTo(chip); hw.Frame(); hw.Button(true); hw.Frame(); hw.Button(false); hw.Frame();
    CHECK_FALSE(hw.lastActions.showStatus);
}
```

(Executor: `GraphMouseHarness` must capture the panel's returned actions into a `lastActions` member if it doesn't already — a two-line harness addition.)

- [ ] **Step 8: Full suite from the exe dir — green; desk-check the three deep-links still behave identically in the live editor** (digest→Status, Reveal→Browse row revealed, Focus-in-Graph→Graph centered).
- [ ] **Step 9: Commit** — `feat(editor): asset deep-links routed as host actions with focus-if-open gates`

---

### Task 4: Status body → AssetStatusPanel.* (pure motion)

**Files:**
- Create: `ArcaneEditor/src/Panels/AssetStatusPanel.hpp`, `ArcaneEditor/src/Panels/AssetStatusPanel.cpp`
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp` (bodies move out; dispatch `:4969` calls the exported body)

**Interfaces:**
- Produces: `void DrawAssetStatusBody(AssetPanelModel& model, const Arcane::Project* project, DocumentHost& docs, const AssetPanelServices& services, AssetPanelActions& actions);` — the Status dashboard, no `Begin`, no bottom bar (Task 7 wraps it). Status takes **no state struct** (spec §6).
- Consumes: `AssetPanelCommon.hpp`, model read surface (`Health()`, `Entries()`, `UnusedGuids()`, `RefIndex()`), `services.activity`/`cookDetailFor`.

- [ ] **Step 1: Move** `DrawStatusLens` (`AssetsPanel.cpp:2633-3031`) and its private helpers — the attention/queued cards, `DrawUnreferencedCard` (`:2389`), `DrawSceneCard` (`:2558`), the activity feed, and any Status-only statics they reach (relocate by symbol; the compiler is the completeness check) — into `AssetStatusPanel.cpp`, exporting only `DrawAssetStatusBody` (same six-arg body minus `state`: Status's former `state` uses were the deep-link writes, already actions since Task 3; if any residual `state` read remains, STOP — that's an undocumented coupling to surface in the SDD ledger, not to paper over).
- [ ] **Step 2:** `AssetsPanel.cpp`'s dispatch branch (`:4969`) becomes `DrawAssetStatusBody(model, project, docs, services, actions);` plus `#include "Panels/AssetStatusPanel.hpp"`.
- [ ] **Step 3:** Regenerate solution, full rebuild, full suite — identical counts (pure motion). Launch once; Status lens pixel-identical.
- [ ] **Step 4: Commit** — `refactor(editor): move the Status dashboard body into AssetStatusPanel`

---

### Task 5: Graph body + canvas lifecycle → AssetGraphPanel.* (pure motion)

**Files:**
- Create: `ArcaneEditor/src/Panels/AssetGraphPanel.hpp`, `ArcaneEditor/src/Panels/AssetGraphPanel.cpp`
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp` / `AssetsPanel.hpp`, `ArcaneEditor/src/App/EditorApp.cpp:1249`, `:2950` (teardown call sites), `ArcaneTests/src/AssetsGraphCanvasTest.cpp` (includes only)

**Interfaces:**
- Produces: `void DrawAssetGraphBody(AssetsPanelState& state, AssetPanelModel& model, const Arcane::Project* project, DocumentHost& docs, const AssetPanelServices& services, AssetPanelActions& actions);` (Task 7 retargets `state` to `AssetGraphPanelState&`); `void DestroyAssetGraphPanelCanvas(AssetsPanelState& state);` (renamed from `DestroyAssetsPanelCanvas`, same body); `const char* GraphFocusLabel(const AssetPanelModel&, const Arcane::Guid&)` and `ScenesByName(...)` exported (the toolbar's focus combo still lives in AssetsPanel.cpp until Task 7).
- The `#include <imgui_node_editor.h>` moves with the body — **AssetGraphPanel.cpp becomes its only editor include site** (Plan 3 ruling 1 preserved; grep-verify).

- [ ] **Step 1: Move** `DrawGraphLens` (`AssetsPanel.cpp:3760-4838`), the graph tooltip/edge-summary helpers (`:709` onward), the canvas create path (`:3788-3815`), `DestroyAssetsPanelCanvas` (`:4862-4903`, renamed), `AssetsGraphProjectionIsCurrent` (`:4840-4860` — moves as-is; deleted in Task 7), and the boot-scene focus seed (`:4923-4927` relocates into `DrawAssetGraphBody`'s preamble) into `AssetGraphPanel.cpp`.
- [ ] **Step 2:** Repoint the two teardown call sites (`EditorApp.cpp:1249`, `:2950`) to `DestroyAssetGraphPanelCanvas`; dispatch branch (`:4961`) calls `DrawAssetGraphBody(...)`. `AssetsPanel.hpp` drops the `AssetsGraphProjectionIsCurrent`/`DestroyAssetsPanelCanvas` declarations (they move to `AssetGraphPanel.hpp`); `AssetsGraphCanvasTest.cpp` adds the new include.
- [ ] **Step 3:** Grep gate: `grep -rn "imgui_node_editor.h" ArcaneEditor/src` → exactly `AssetGraphPanel.cpp` (the test's own include is exempt and documented there).
- [ ] **Step 4:** Regenerate, rebuild, full suite (the four `[graphcanvas]` cases are the sharp edge — they drive the real panel); identical counts. Commit — `refactor(editor): move the Graph lens body and canvas lifecycle into AssetGraphPanel`

---

### Task 6: Browse body + toolbar + preview → AssetBrowserPanel.* (pure motion)

**Files:**
- Create: `ArcaneEditor/src/Panels/AssetBrowserPanel.hpp`, `ArcaneEditor/src/Panels/AssetBrowserPanel.cpp`
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp` (shrinks to shell: dispatch + toolbar + bottom bar + shared glue)

**Interfaces:**
- Produces: `void DrawAssetBrowserBody(AssetsPanelState& state, AssetPanelModel& model, const Arcane::Project* project, DocumentHost& docs, const AssetPanelServices& services, AssetPanelActions& actions);` — rail + grouped table + preview pane (`DrawBrowseLens` `:1998-2062` plus its row/table/preview/rail helpers: `DrawAssetRow` `:1304` region, `DrawChildRow`, `DrawDerivedRow`, `DrawTable` (scroll-to-selection `:1481-1495`), `PreviewPaneSplitter` `:1708`, `DrawPreviewPane` `:1754`, `AttachRowInteractions`/`DrawRowContextMenu` `:921` region, peek-tooltip callers).
- The peek-tooltip **anatomy** (shared shape) goes to `AssetPanelCommon` if it isn't panel-specific after the move — executor judges by what Graph/Status callers still reference; record the call in the ledger.

- [ ] **Step 1: Move** the Browse bodies + helpers into `AssetBrowserPanel.cpp`; dispatch branch (`:4959`) calls `DrawAssetBrowserBody(...)`.
- [ ] **Step 2:** Regenerate, rebuild, full suite — identical counts; launch once, Browse pixel-identical (rail, table, preview, splitter drag).
- [ ] **Step 3: Commit** — `refactor(editor): move the Browse lens body into AssetBrowserPanel`

After this task `AssetsPanel.cpp` holds only: state struct header side, `DrawToolbar`, `DrawBottomBar`, `DrawAssetsPanel`, and includes — the shell Task 7 deletes.

---

### Task 7: THE CUTOVER — three windows, state split, shell retired

The one non-incremental task (spec §4.1, §5, §6, §7.4, §10). Everything here lands in ONE commit because the intermediate states don't build.

**Files:**
- Modify: `ArcaneEditor/src/Panels/PanelRegistry.hpp` (row swap), `ArcaneEditor/src/Panels/EditorPanels.cpp` (`BuildDefaultLayout` `:361-385`), `ArcaneEditor/src/Panels/AssetBrowserPanel.hpp/.cpp`, `AssetGraphPanel.hpp/.cpp`, `AssetStatusPanel.hpp/.cpp` (wrappers + state retarget), `ArcaneEditor/src/App/EditorApp.hpp:1184` (members), `ArcaneEditor/src/App/EditorApp.cpp` (teardown sites), `ArcaneEditor/src/App/EditorAppFrame.cpp:2040-2077` (three draw sites), `:2447+` (consumption → `SelectDockTab`)
- Delete: `ArcaneEditor/src/Panels/AssetsPanel.hpp`, `ArcaneEditor/src/Panels/AssetsPanel.cpp`
- Test: `ArcaneTests/src/PanelRegistryTest.cpp`, `ArcaneTests/src/AssetsGraphCanvasTest.cpp`, `ArcaneTests/src/AssetPanelCommonTest.cpp`, `ArcaneTests/src/AssetBrowserTest.cpp:7` (comment)

**Interfaces (final, from spec §5/§6):**

```cpp
// AssetBrowserPanel.hpp
struct AssetBrowserPanelState {
    char search[128] = {};
    int  railKind = -1;
    std::uint32_t seenSelectionStamp = 0;
    float previewPaneWidth = kAssetsPreviewPaneDefaultWidth;
    std::unordered_map<std::string, bool>  groupOpen;
    std::unordered_map<Arcane::Guid, bool> childrenOpen;
};   // field comments carried verbatim from AssetsPanelState
AssetPanelActions DrawAssetBrowserPanel(AssetBrowserPanelState&, AssetPanelModel&,
                                        const Arcane::Project*, DocumentHost&,
                                        const AssetPanelServices&, bool* open);

// AssetGraphPanel.hpp
struct AssetGraphPanelState {
    void* graphCanvas = nullptr;
    GraphGridPhase graphGrid;
    Arcane::Guid graphFocus;
    bool graphFocusSeeded = false;
    std::uint32_t seenSelectionStampGraph = 0;
    Arcane::Guid graphMenuGuid;
    Arcane::Guid graphHoverGuid;  float graphHoverSeconds = 0.0f;
    Arcane::Guid graphWireGuid;   bool graphWireDerivable = false;
    Arcane::Guid graphDragGuid;   bool graphDragRight = false;
    AssetGraphViewModel graph;
    std::uint32_t graphBuiltStamp = 0;
    Arcane::Guid graphBuiltFocus;
    bool graphBuilt = false;
    bool graphLayoutDirty = false;
};   // field comments carried verbatim
AssetPanelActions DrawAssetGraphPanel(AssetGraphPanelState&, AssetPanelModel&,
                                      const Arcane::Project*, DocumentHost&,
                                      const AssetPanelServices&, bool* open);
void DestroyAssetGraphPanelCanvas(AssetGraphPanelState&);

// AssetStatusPanel.hpp
AssetPanelActions DrawAssetStatusPanel(AssetPanelModel&, const Arcane::Project*,
                                       DocumentHost&, const AssetPanelServices&,
                                       bool* open);

// AssetPanelCommon.hpp — RevealAssetInBrowser retargets:
void RevealAssetInBrowser(AssetBrowserPanelState&, AssetPanelModel&, const Arcane::Guid&);
```

- [ ] **Step 1: Registry swap.** `PanelId`: `Viewport, Outliner, Inspector, AssetBrowser, AssetGraph, AssetStatus, Problems, Console, Count`. Table rows: `{ AssetBrowser, "Asset Browser", false, Assets }`, `{ AssetGraph, "Asset Graph", false, Assets }`, `{ AssetStatus, "Asset Status", false, Assets }` in place of the Assets row. Extend `PanelRegistryTest`: the three asset rows carry `PanelSection::Assets`; parse round-trip `"Asset Browser=0"` → `{AssetBrowser,false}`; `"Assets=1"` now → `nullopt` (unknown name).
- [ ] **Step 2: Wrappers.** Each panel file gains its `Draw*Panel` wrapper (final signatures above): `Begin(<name>, open)` → early-out on collapse → toolbar (Browser: `+ Create` + search, the width math dropping `stripWidth`/`focusSlotWidth` entirely; Graph: focus combo only — the combo code moves here from the old `DrawToolbar` `:523-558`; Status: none) → `BeginChild("##<name>body", ImVec2(0, -kAssetPanelBottomBarHeight))` → body → bottom bar per spec §9.2. Bottom bars: each panel keeps its own left-context form (Browse `:664-669` verbatim, Graph `:650-660` **without** the `graphCurrent` gate — the predicate's frame is unrepresentable now, print `state.graph.realNodeCount` when `graphBuilt` else the em dash — Status `:661-663`); Browse + Graph call the digest chip, Status's right slot is EMPTY this task (Task 8 fills it). The chip + bar skeleton (divider paint `:599-604`, `rightEdgeX`/`padY` math, refused/rest two-segment draw `:672-693`, click overlay) move into `AssetPanelCommon` as `void DrawAssetPanelBottomBarChrome(...)` helpers so the three bars share one implementation.
- [ ] **Step 3: State split.** Introduce the two structs (fields + comments moved verbatim from `AssetsPanelState`); `boot-scene focus seed` and every `state.` reference in the two moved bodies retarget mechanically (`state.graphFocus` etc. keep their spellings — only the struct name changed). `EditorApp.hpp:1184` → `AssetBrowserPanelState m_assetBrowserUi;` + `AssetGraphPanelState m_assetGraphUi;`. Teardown sites take `m_assetGraphUi`.
- [ ] **Step 4: Host draw sites.** Replace the single gated site (`EditorAppFrame.cpp:2043-2076`) with three, Console/Problems-shaped: build `services` ONCE (bools from the three real `PanelId`s now + `PanelId::Problems`), then per panel `if (m_panelVis.IsVisible(id)) actions = Draw*Panel(...)` collecting three `AssetPanelActions`; run `ConsumeBrowserActions` (rename to `ConsumeAssetPanelActions`) over each in order Browser, Graph, Status. Model rebuild stays ONCE, before all three (`:2042`, comment updated to say "three panels").
- [ ] **Step 5: Consumption switches to R1.** `showStatus` → `if (m_panelVis.IsVisible(PanelId::AssetStatus)) SelectDockTab("Asset Status");`. `focusInGraph` → visibility-checked: `m_assetGraphUi.graphFocus = g; m_assetModel.Select(g); SelectDockTab("Asset Graph");`. `revealInBrowse` → `RevealAssetInBrowser(m_assetBrowserUi, m_assetModel, g); SelectDockTab("Asset Browser");`. (Panels already grey when closed — the host check is defense in depth, spec §7.1.)
- [ ] **Step 6: Default layout.** `BuildDefaultLayout` `:380-382` becomes five `bottomId` rows: `"Asset Browser"`, `"Asset Graph"`, `"Asset Status"`, `"Console"`, `"Problems"` — Browser first so a fresh layout opens on it.
- [ ] **Step 7: Delete the shell.** Remove `AssetsPanel.hpp/.cpp` (git rm), `AssetLens`, `kLensLabels`/`kLensCount`/`kLensEnabledMask`, `DrawToolbar`, `DrawBottomBar`, `DrawAssetsPanel`, `AssetsGraphProjectionIsCurrent` (spec §7.4 — delete, don't move). `kAssetsPreviewPaneDefaultWidth` relocates to `AssetBrowserPanel.hpp`. Sweep: `grep -rniw "AssetsPanel\|AssetLens\|DrawAssetsPanel\|AssetsPanelState" ArcaneEditor ArcaneTests docs/plans/2026-09-09*` → only historical docs/spec mentions remain.
- [ ] **Step 8: Test rework.** `AssetsGraphCanvasTest.cpp`: harness targets `DrawAssetGraphPanel` + `AssetGraphPanelState` (the 8 `state.lens` writes delete; `state.graphFocusSeeded = true` etc. keep their spellings). The four carried cases (`:117`, `:774`, `:918`, `:1043`) port near-verbatim. The transition case (`:425-490`) is **replaced** by the new-mechanism test (spec §12):

```cpp
TEST_CASE("focusInGraph consumption refocuses and the next Graph frame rebuilds",
          "[editor][graphcanvas]")
{
    /* fixture + device-less frames as the survives-frames case */
    // Steady state: built for sceneId.
    state.graphFocus = sceneId;
    drawFrame();                       // DrawAssetGraphPanel via the harness
    const std::uint32_t epoch = state.graph.buildEpoch;

    // The host's focusInGraph consumption, applied by hand (panel-split spec
    // s7.1): focus + select. No direct graph-state write beyond focus.
    state.graphFocus = materialId;
    model.Select(materialId);

    drawFrame();                       // next Graph frame rebuilds for it
    CHECK(state.graph.buildEpoch == epoch + 1u);
    CHECK(state.graphBuiltFocus == materialId);
}
```

  `AssetPanelCommonTest.cpp`'s Reveal test retargets `AssetBrowserPanelState`. `AssetBrowserTest.cpp:7`'s comment updates to name `DrawAssetBrowserPanel`.
- [ ] **Step 9:** Regenerate, full rebuild **Debug AND Release**, full suite both configs from the exe dir. Derive the new counts and attribute every delta to a named test add/remove (derive, never recall). Launch the editor: Window menu lists the three; open/close each; selection syncs across open panels; deep-links focus (open target) / grey (closed target); Reset Layout lands five tabs with Asset Browser selected.
- [ ] **Step 10: Commit** — `feat(editor): split the Assets panel into Asset Browser / Asset Graph / Asset Status`

---

### Task 8: Chrome finish — Status recency line + disabled-tooltip pass

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetStatusPanel.cpp` (bottom-bar right slot), `AssetBrowserPanel.cpp`/`AssetGraphPanel.cpp` (tooltip copy check)

**Interfaces:**
- Consumes: `services.activity` (`AssetActivityLog`) — the feed's own newest-entry access + relative-time formatting, reused not duplicated.

- [ ] **Step 1:** Status bottom-bar right slot (spec §9.3): newest activity entry as `last change <rel> \xC2\xB7 <name>` in `TextDisabled`, right-aligned with the same `rightEdgeX` math the digest uses; slot empty when `services.activity` is null or the ring is empty (never a fabricated "just now" — §13 discipline). Reuse the feed's relative-time formatter by exporting it from wherever it landed in Task 4 (it is Status-local, so this is a file-internal helper call).
- [ ] **Step 2:** Tooltip copy audit: every disabled deep-link names its target — `"Asset Status is closed \xE2\x80\x94 open it from Window \xE2\x96\xB8"` / `"Asset Graph is closed \xE2\x80\x94 ..."` / `"Asset Browser is closed \xE2\x80\x94 ..."` / `"Problems is closed \xE2\x80\x94 ..."`.
- [ ] **Step 3:** Build, suite, desk-glance (close Status → Browse/Graph chips grey with tooltip; Status bar shows the recency line after any cook/create). Commit — `feat(editor): status recency line + disabled deep-link tooltips`

---

### Task 9: Goldens — layout seed + editor-ui, both lanes

Arc 2 discipline throughout: assert `gatePassed` + per-lane `verdict` from the gate JSON only; a bless restages BOTH hosts; copy diff artifacts into the SDD workspace BEFORE any re-run (the gate deletes stale diff PNGs).

**Files:**
- Modify: `ArcaneTests/src/GoldenImageTest.cpp:32-58`, `ReferenceProject/Saved/verify-layout.ini` (regenerated), `ReferenceProject/Verify/References/editor-ui.png` (re-blessed)

- [ ] **Step 1: Write the failing seed assertions** — extend the `[golden]` seed case (`GoldenImageTest.cpp:47-57`), line-anchored per its own Finding-D lesson:

```cpp
    // Panel-split spec s13: the three asset panels replaced [Window][Assets].
    CHECK(text.find("\n[Window][Asset Browser]") != std::string::npos);
    CHECK(text.find("\n[Window][Asset Graph]")   != std::string::npos);
    CHECK(text.find("\n[Window][Asset Status]")  != std::string::npos);
    CHECK(text.find("\n[Window][Assets]")        == std::string::npos);
```

- [ ] **Step 2: Run — expect FAIL** (old seed still names `[Window][Assets]`).
- [ ] **Step 3: Regenerate the seed** per its own documented procedure (`verify-layout.ini:58-64`): **move the old seed aside first** (`git mv` to a scratch name or delete after copying — a tainted seed re-emits ghost entries), launch the editor headed against ReferenceProject, Reset Layout, quit, run `--dump-layout`, review the diff (three asset `[Window]` sections; visibility block gains `Asset Browser=1`/`Asset Graph=1`/`Asset Status=1`, loses `Assets=1`; bottom node `Selected=` is the Asset Browser tab hash), restore the header prose block updating its window inventory (`:19-21`, `:47-48`).
- [ ] **Step 4: Run — expect PASS.**
- [ ] **Step 5: editor-ui re-bless.** Run `scripts/golden-gate.ps1` (editor lane, dx12 + vulkan) → it FAILS on the layout change; read both diff artifacts (copy into the SDD workspace), confirm the diff is exactly the expected shell change (tab bar + chrome across the bottom node, no unexplained pixels elsewhere), bless SOURCE, restage BOTH hosts, rerun → green on both backends, `gatePassed` + per-lane `verdict` asserted from the JSON.
- [ ] **Step 6: Commit** — `test(editor): regenerate layout seed and re-bless editor-ui for the panel split`

---

### Task 10: Closeout sweep + suites + canvas restamp

- [ ] **Step 1: Zero-legacy sweep** (path-exclude + `-riw` method): `AssetsPanel`, `AssetLens`, `AssetsPanelState`, `AssetsPanelActions`, `AssetsPanelServices`, `DrawAssetsPanel`, `AssetsGraphProjectionIsCurrent`, `DestroyAssetsPanelCanvas` across `ArcaneEditor ArcaneTests ArcaneRuntime scripts` — hits only in docs/specs history.
- [ ] **Step 2: Full suites, Debug AND Release**, from the exe dir; derive final counts, attribute every delta vs the pre-arc baseline (55294/1522 ~[gpu], Debug=Release; Dist gap 68/6) to named adds/removes/replacements. Ledger the runs (seed banners included).
- [ ] **Step 3: Canvas restamp (spec §14)** — session-level action for the controller, not a subagent: restamp the design canvas boards (Option G → FINAL with R1–R6 amendments noted; Option F → not chosen; one-panel FINALs → "superseded (shell) / bodies survive"; decision-record note points at the split spec). Local working set `.superpowers/design/asset-manager-mockups/extract-20260909/` is the seed; republish via the design canvas skill.
- [ ] **Step 4:** Hold for the user's desk pass (side-by-sides vs the G board where it still binds; the R2/R3 slimmings are DELIBERATE divergences from the board — the spec wins). **Push only after the pass.**

---

## Self-review record (run at authoring time)

- **Spec coverage:** §4→T1/T7 · §5→T2/T4-T7 · §6→T7 · §7.1-7.3→T3/T7 · §7.4→T7(step 2,7) · §8→no task (zero work, asserted by T7 step 9 desk check) · §9.1-9.2→T7 step 2 · §9.3→T8 · §9.4→T6 · §10→T7 steps 1,6 · §12→T1/T3/T7/T9 · §13→T9 · §14→T10 · §15 hazards each land where §15's table says.
- **Type consistency:** `AssetPanelActions`/`AssetPanelServices` named identically T2→T7; `RevealAssetInBrowser` first param `AssetsPanelState&` (T3) retargeted `AssetBrowserPanelState&` (T7, declared in both places); constants renamed once in T2 and used by that name after.
- **Known intentional gaps:** menu drawing and wrapper chrome have no unit seam — their net is the T9 golden lane + desk checks, stated in-task.
