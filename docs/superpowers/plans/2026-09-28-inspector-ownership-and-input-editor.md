# Inspector Ownership + Input Actions Editor Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One Inspector driven by selection sources (scene + documents) with pages, pin, instances and history; and the Input Actions document rebuilt in the editor's vocabulary as the first non-scene Inspector source.

**Architecture:** A pure `InspectorHost` (no ImGui) routes "which source selected last" to N Inspector windows; every source contributes an `InspectorPage` (breadcrumb + body) drawn through a shared `PropertyGrid` extracted from the scene Inspector. The Input Actions document keeps its `InputActionsEditorModel`; a pure row builder (`InputActionsRows`) feeds a two-column presentation (maps | actions with expanded binding rows), and the document's page draws the asset/map/action/binding properties in the Inspector. `--select-in-document` plus a `inspector` block in the witness report make the result capturable headlessly.

**Tech Stack:** C++23 (MSVC v143, `/MD`), Dear ImGui 1.92.9 (docking), nlohmann/json, Catch2, premake5, the Arcane witness harness (`--headless --report`), PowerShell golden gate.

**Spec:** `docs/superpowers/specs/2026-09-28-inspector-ownership-design.md` (spec A) and `docs/superpowers/specs/2026-09-28-input-actions-editor-redesign-design.md` (spec B). Both approved 2026-09-28. Mockup: https://claude.ai/artifact/TUEutRAVgF9MmokP8kiuFE.

## Global Constraints

- Repo: `D:\dev\starworks\Arcane`, branch `main` (clean, = origin/main `decb4e41` at plan time). Work on a branch `feat/inspector-ownership-input-editor`; fix-forward commits with conventional subjects; every commit ends with the two trailer lines `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi`. Push only when the user asks.
- Build (from the repo root): `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nr:false -v:m -nologo`. `ArcaneEditor` globs its own `src/**.cpp`; **`ArcaneTests` lists editor sources one by one** (`premake5.lua`, the `files {` block of `project "ArcaneTests"`, :1176-~1400, one commented line per file). A new editor `.cpp` that a test or an already-listed file depends on goes into that list IN THE STEP THAT WRITES THE .CPP (an entry for a file that does not exist yet makes MSBuild fail on the missing source), then `./ThirdParty/premake5/premake5.exe vs2026`, then the build. `SceneInspectorSource.cpp` (T3) and `InspectorWindows.cpp` (T4) are NEVER listed: both need `EditorPanels.cpp`, which the test exe deliberately excludes (:1222, :1329). `LNK1104 cannot open <exe>` means the user's editor is running: stop and report, never kill it.
- Tests run FROM `bin/Debug-windows-x86_64-md/ArcaneTests/`: `./ArcaneTests.exe "[editor][inspector]"` etc. Baseline `~[gpu]`: 2093 cases / 2089 pass / 4 skipped. Every build and suite command runs in the FOREGROUND (no `run_in_background`, 10-minute timeout).
- Golden gate: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/golden-gate.ps1 -Configuration Debug` (8 lanes, all green on main). Re-bless procedure: bless the STAGED slot (`bin/Debug-windows-x86_64-md/ArcaneEditor/ReferenceProject/Verify/References/<name>.png`, run the exe from that dir with `--bless`, delete that dir's `imgui.ini` first), then copy the PNG to `ReferenceProject/Verify/References/` in the source tree IMMEDIATELY.
- Layout ini: the desk editor writes `%LOCALAPPDATA%\Arcane\editor\layouts\<project-guid>.ini` (`EditorApp.cpp:1585-1592`; ReferenceProject's guid is `cfafaf09-86bb-4b4b-a99f-9ecb0771bc15`), NEVER the exe-dir `imgui.ini`; a `--headless` run pins `io.IniFilename = nullptr` and reads only the committed seed `ReferenceProject/Saved/verify-layout.ini` (`EditorApp.cpp:1562-1581`).
- Spec A 3.1: focus is NEVER selection. Only a selection event moves the Inspector. Document selection never enters the scene's `m_selection`, and vice versa.
- Spec A 6: history depth 32, prune on invalidate, skip unresolvable at navigation, never persisted. Pins release on project switch.
- Spec A 3.3: pin, not lock. `Window > New Inspector` adds an instance; each has its own pin.
- Spec B 2.1: NO Save button, NO JSON tab in the document. Dirty state = the tab's dot + Ctrl+S.
- Spec B 2.3: rows show READABLE control names from the evaluator (`InputActions::DisplayForPath`), never the raw path; the picker offers ONLY paths the evaluator compiles (`InputActions::KnownControls()`).
- Spec B 2.4: every field edit goes through `InputActionsEditorModel::ApplyEdit` (via `SetField` and friends) so undo/redo stays on the shared `CommandStack`.
- Spec A 3.5 / B 7: the shader document's "Material" tab and the Asset Browser's preview pane are UNTOUCHED here (mini-arcs 2 and 3). The Asset Browser is not a source in this plan.
- Theme: only `Widgets/EditorTheme.hpp` tokens (`Theme::kSelection`, `kPanelRaised`, `kAmber`, `kTextDim`, ...) and lucide glyphs from `Widgets/IconsLucide.h`. No new hex literals outside `EditorWidgets.cpp`'s pill palette.
- Engine-side generic capability (control-path tables, readable names, rebind timer) is built IN the engine (`ArcaneClient/src/Arcane/Input/`), never forked into the editor.
- Every engine LAYOUT change in this plan lands in **Task 5** -- the `InputActions` virtual (`BindingValue`), the `InputRebindOperation` member (`heldModifiers_`), the `LocalInputUser` members (`mapStack_`, `scheme_`) -- so there is ONE bump: `kGamePluginABIVersion` 44 -> 45 at `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:972` with the neighbours' changelog line, `ReferenceProject/ReferenceProject.arcproj:6` `"abi": 44` -> `45` (as 04891dcd did), and the sample module REBUILT before the root build so the postbuild stages a matching DLL (Task 5 Step 4c). `Plugin.cpp:57-66` refuses a 44 DLL in a 45 host outright and the editor then shows the Open Project Failed modal in every capture. Aphelyon's restamp is already owed and absorbs it.
- Subagent report files: `.superpowers/sdd/2026-09-28-inspector-ownership-and-input-editor/task-N-report.md`, appended with `echo >>` (the Write tool is refused to subagents).

## Review Focus

1. **A history entry that names a deleted entity or a removed binding**: back/forward must skip it and prune it, never select garbage or crash. Pinned to Task 2's `GoBack skips and prunes` test. Pinned also to Task 2's `PruneStale` / `InvalidateSource` tests: a stale entry is pruned BEFORE a click, and the arrows are never enabled for one.
2. **A pinned instance whose document closes, then the user opens another document**: the pinned instance keeps showing "closed" with an unpin affordance; the follower shows the new document. Pinned to Task 2's `closed source releases` test and Task 4's desk check.
3. **A malformed `.arcinput` draft (parse failure or `actionMaps` missing)**: the document must show its repair banner and draw an empty maps column, never index into a non-array. Pinned to Task 7's `non-object draft yields no rows` test and Task 8's banner.
4. **A rebind capture that times out or is cancelled with Escape while the row is drawn**: the row returns to its readable name and the capture target clears; a completed capture writes ONE undoable `SetField`. Pinned to Task 9's document-level capture handling and Task 5's `Remaining()` test. The key that completes the capture is CONSUMED (same-frame Delete/Enter/F2/arrow/click must not act -- Task 8's `InputSwallowed`), a hidden/unfocused document CANCELS the capture, and a held modifier prefixes a chord (Task 5).
5. **`--select-in-document` naming a map or action that does not exist, or given without `--open-asset`**: refused at parse time when `--open-asset` is absent; a loud boot `ARC_ERROR` when the path is unresolvable, with the run still completing and the report carrying the document as the source. Pinned to Task 12's HostConfig tests and E3.

## File Structure

Create:
- `ArcaneEditor/src/Widgets/PropertyGrid.hpp` / `.cpp` -- `PropertyGridState`, `PropertyGrid` (Section/SubSection/Rows + the row primitives), built on `FieldGrid`, `FieldLabelCell`, `HeaderBand`, `InputTextString`.
- `ArcaneEditor/src/Panels/InspectorSource.hpp` -- `InspectorCrumb`, `InspectorPage`, `InspectorSource` interfaces (no ImGui).
- `ArcaneEditor/src/Panels/InspectorHost.hpp` / `.cpp` -- `InspectorHost` (sources, current, instances, pin, history), `SelectionEdge` (the "is this a selection EVENT" filter). Pure.
- `ArcaneEditor/src/Panels/SceneInspectorSource.hpp` / `.cpp` -- the scene as a source; its page wraps `DrawInspectorBody`.
- `ArcaneEditor/src/Panels/SceneSelectionKey.hpp` -- header-only: the scene's whole-set selection key encode/decode/alive-filter, testable without `EditorPanels.cpp`.
- `ArcaneEditor/src/Panels/InspectorWindows.hpp` / `.cpp` -- draws every instance window: header (back/forward, breadcrumb, pin) + the page body.
- `ArcaneEditor/src/Documents/InputActionsRows.hpp` / `.cpp` -- pure row builder for the actions column, keyboard stepping, sibling index, interaction text.
- `ArcaneEditor/src/Documents/InputActionsInspectorPage.hpp` / `.cpp` -- the document's Inspector pages (asset/map/action/binding + Live preview + picker).
- `ArcaneTests/src/PropertyGridTest.cpp`, `ArcaneTests/src/EditorInspectorHostTest.cpp`, `ArcaneTests/src/SceneSelectionKeyTest.cpp`, `ArcaneTests/src/InputActionsRowsTest.cpp`.

Modify:
- `ArcaneEditor/src/Panels/EditorPanels.hpp` / `.cpp` -- `DrawInspectorPanel` becomes `DrawInspectorBody` (no Begin/End) using `PropertyGrid`; `InspectorState::labelColWidth` becomes `InspectorState::grid`; `MenuRequests::newInspector` + the Window menu item; `MenuRequests::openAssetAsText` + the Assets > Open as text item (Task 8 Step 0).
- `ArcaneEditor/src/Scene/SelectionContext.hpp` -- `Epoch()` (bumped by Select/Toggle/AddRange/Clear, never by Prune; Task 2 Step 4b).
- `ArcaneEditor/src/Panels/AssetPanelCommon.hpp` / `.cpp`, `ArcaneEditor/src/Panels/AssetBrowserPanel.cpp` -- `AssetPanelActions::openAsText`: the context-menu + preview-pane "Open as text" entries (Task 8 Step 0).
- `ArcaneEditor/src/Documents/EditorDocument.hpp` -- derives `InspectorSource` with defaults; gains `SelectionEpoch()`, `SelectByPath()`.
- `ArcaneEditor/src/Documents/DocumentHost.hpp` / `.cpp` -- open/close observer.
- `ArcaneEditor/src/Documents/InputActionsDocument.hpp` / `.cpp` -- toolbar state, capture + preview ownership, page, `SourceName`, save epoch, Problems publish.
- `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp` / `.cpp` -- REWRITTEN presentation.
- `ArcaneEditor/src/Documents/InputActionsEditorModel.hpp` / `.cpp` -- selection epoch/key/restore/by-path, `MoveRowTo`, `Conflicts`, extended `Warnings`.
- `ArcaneClient/src/Arcane/Input/InputActions.hpp` / `.cpp` -- `KnownControls`, `IsKnownControlPath`, `DisplayForPath`, `BindingValue`.
- `ArcaneClient/src/Arcane/Input/InputRebindOperation.hpp` / `.cpp` -- `Remaining()`; `heldModifiers_` + modifier chords (a held modifier prefixes the captured key).
- `ArcaneClient/src/Arcane/Input/LocalInputUser.hpp` / `.cpp` -- `mapStack_`, `scheme_`, `ActiveMap()`; same-project re-Configure (map stack, scheme and dirty overrides survive a save's republish).
- `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp` + `ReferenceProject/ReferenceProject.arcproj` -- ABI 45 (the ONE bump, Task 5 Step 4c).
- `ArcaneClient/src/Arcane/Host/HostConfig.hpp` / `.cpp`, `ArcaneRuntime/src/main.cpp`, `ArcaneEditor/src/main.cpp` -- `--select-in-document`.
- `ArcaneClient/src/Arcane/Host/VerifyReport.hpp` / `.cpp` -- `SetInspector`, schema 11.
- `ArcaneEditor/src/App/EditorApp.hpp`, `EditorApp.cpp`, `EditorAppFrame.cpp`, `EditorAppProject.cpp`, `EditorAppScene.cpp` -- host member, per-frame sync, registration, project switch, `ClearSceneReferences` (scene-source invalidation + edge re-arm), ini, republish, boot flag.
- `ArcaneEditor/src/Widgets/EditorWidgets.hpp` / `.cpp` -- `AssetPill` variants 2 (blue-grey) and 3 (violet-grey); the two `labelColWidth` authority comments (Task 1 Step 5).
- Comment-only renames of `DrawInspectorPanel` -> `DrawInspectorBody` (Task 1 Step 5): `ArcaneEditor/src/Panels/InspectorView.hpp` / `.cpp`, `ArcaneEditor/src/Scene/ComponentCatalog.hpp`, `ArcaneTests/src/EditorComponentCatalogTest.cpp`.
- Golden references: `ReferenceProject/Verify/References/editor-ui.png` and `editor-ui-perspective.png` re-blessed (Task 4 Step 7, the new header row); `editor-input-doc.png` NEW (Task 12 Step 6). The gate holds eight lanes today (`$combos.Count`) and ten after Task 12.
- `scripts/golden-gate.ps1` -- `ReportSchemaMax = 11`, the `editor-input-doc` lanes.
- `premake5.lua` -- the `ArcaneTests` `files` block (:1176-~1400): four new entries (T1 `PropertyGrid.cpp`, T2 `InspectorHost.cpp`, T7 `InputActionsRows.cpp`, T10 `InputActionsInspectorPage.cpp`), each added in the step that writes the `.cpp`.
- Tests: `EditorInspectorVectorTest.cpp`, `EditorDocumentHostTest.cpp`, `SelectionOpsTest.cpp`, `InputActionsEditorModelTest.cpp`, `InputActionsTest.cpp`, `InputRebindOperationTest.cpp`, `ClientRuntimeTest.cpp`, `HostConfigTest.cpp`, `VerifyReportTest.cpp`, `EditorWitnessTest.cpp`.

---

### Task 1: PropertyGrid extracted from the scene Inspector

**Files:**
- Create: `ArcaneEditor/src/Widgets/PropertyGrid.hpp`, `ArcaneEditor/src/Widgets/PropertyGrid.cpp`
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp:504-567 (InspectorState; labelColWidth at :553) and :577-582 (DrawInspectorPanel declaration)`, `ArcaneEditor/src/Panels/EditorPanels.cpp:2367-2846` (DrawInspectorPanel), `ArcaneEditor/src/App/EditorAppFrame.cpp:3598` (call site), `ArcaneTests/src/EditorInspectorVectorTest.cpp:324`, `premake5.lua` (the `ArcaneTests` `files` block: one entry for PropertyGrid.cpp), comment mentions of `DrawInspectorPanel` in `ArcaneEditor/src/App/EditorApp.hpp`, `ArcaneEditor/src/Panels/InspectorView.hpp` / `.cpp`, `ArcaneEditor/src/Scene/ComponentCatalog.hpp`, `ArcaneEditor/src/Widgets/EditorWidgets.cpp`, `ArcaneTests/src/EditorComponentCatalogTest.cpp`
- Test: `ArcaneTests/src/PropertyGridTest.cpp`

**Interfaces:**
- Consumes: `FieldGrid`, `FieldLabelCell`, `HeaderBand`, `InputTextString` (`Widgets/EditorWidgets.hpp`); `ImGui::ClearActiveID` (`imgui_internal.h`, the numeric-row Escape cancel).
- Produces:
  ```cpp
  struct PropertyGridState {
      float labelColWidth = 0.0f;
      struct TextDraft { std::string text; std::string seed; bool active = false; int lastFrame = 0; std::function<void(std::string)> commit; };   // commit is bound to the TARGET the row was drawn for
      std::unordered_map<unsigned int, TextDraft> textDrafts;      // keyed by ImGui id
      struct NumericDraft { double value = 0.0; double seed = 0.0; bool active = false; };   // the ROW owns the in-flight number
      std::unordered_map<unsigned int, NumericDraft> numericDrafts; // keyed by ImGui id
      std::unordered_map<std::string, ImVec2>* probe = nullptr;
  };
  class PropertyGrid {
  public:
      explicit PropertyGrid(PropertyGridState& state);
      [[nodiscard]] bool Section(const char* label, bool defaultOpen = true);
      [[nodiscard]] bool SubSection(std::string_view label, bool defaultOpen = true);   // when true, caller ends with EndSubSection()
      void EndSubSection();
      struct [[nodiscard]] Rows { Rows(PropertyGrid& grid, const char* id); ~Rows(); explicit operator bool() const noexcept; private: FieldGrid m_grid; };
      bool TextRow(const char* label, std::string_view current, std::function<void(std::string)> commit, bool dimmed = false);   // commit is STORED in the draft (see TextDraft)
      bool CheckboxRow(const char* label, bool& value);
      bool IntRow(const char* label, int& value);                          // true once per gesture, on deactivate-after-edit AND value != seed; value follows the gesture every frame
      bool FloatRow(const char* label, float& value, float speed = 0.01f); // same rule; Escape mid-drag = cancel, no commit
      int  ComboRow(const char* label, const char* const* items, int count, int current);   // new index or -1
      void ReadOnlyRow(const char* label, std::string_view text);
      int  ButtonRow(const char* label, const char* const* buttons, int count, unsigned enabledMask = ~0u);   // clicked index or -1
      void MeterRow(const char* label, float fraction01, const char* overlay);
      void CommitOrphans();   // flush drafts deactivated while their row was not drawn; ONCE per frame per state, BEFORE any window that draws this state Begins
      PropertyGridState& State() noexcept;
  };
  ```
  A row's ImGui id must include the TARGET's id (the caller pushes it around the Rows scope); the label alone is not an identity.
  `InspectorState::grid` (a `PropertyGridState`) replaces `InspectorState::labelColWidth`. `DrawInspectorBody(...)` replaces `DrawInspectorPanel(...)`: same parameters minus `bool* open`; no `ImGui::Begin/End` inside.

- [ ] **Step 1: Write the failing test** (`ArcaneTests/src/PropertyGridTest.cpp`)

```cpp
// PropertyGrid (inspector-ownership arc, Task 1): the shared page primitives,
// driven device-less through the REAL ImGui path (EditorInspectorVectorTest's
// harness shape: software atlas, window pinned at the origin, probe centres).
#include <catch2/catch_test_macros.hpp>
#include <Widgets/PropertyGrid.hpp>
#include <imgui.h>
#include <string>
#include <unordered_map>

namespace
{
    struct GridHarness
    {
        Arcane::Editor::PropertyGridState state;
        std::unordered_map<std::string, ImVec2> probe;
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        std::string name = "Alpha";
        bool flag = false;
        int commits = 0;
        bool sectionOpen = false;
        bool drawName = true;            // false = the A "Name" row vanishes; a B row draws under another id
        int commitsB = 0;
        std::string nameB = "Beta";
        float storedSeconds = 0.40f;     // the "model": Seconds is re-derived from it EVERY frame
        int floatCommits = 0;
        float scale = 1.0f;
        int scaleCommits = 0;

        GridHarness()
        {
            IMGUI_CHECKVERSION();
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            state.probe = &probe;
        }
        ~GridHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }

        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            Arcane::Editor::PropertyGrid(state).CommitOrphans();   // once per frame, BEFORE any window that draws this state Begins
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(640, 1000), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            Arcane::Editor::PropertyGrid grid(state);
            sectionOpen = grid.Section("Action");
            if (sectionOpen)
            {
                Arcane::Editor::PropertyGrid::Rows rows(grid, "##fields");
                if (rows)
                {
                    if (drawName) grid.TextRow("Name", name, [&](std::string v) { name = std::move(v); ++commits; });
                    else { ImGui::PushID("B"); grid.TextRow("Name", nameB, [&](std::string v) { nameB = std::move(v); ++commitsB; }); ImGui::PopID(); }
                    grid.CheckboxRow("Blocking", flag);
                    grid.ReadOnlyRow("Path", "<Keyboard>/space");
                    float seconds = storedSeconds;   // re-derived from the "model" EVERY frame, as the input pages do
                    if (grid.FloatRow("Seconds", seconds, 0.01f)) { storedSeconds = seconds; ++floatCommits; }
                    if (grid.FloatRow("Scale", scale, 0.01f)) ++scaleCommits;
                }
            }
            ImGui::End();
            ImGui::Render();
        }
        ImVec2 Centre(const std::string& key) { INFO(key); REQUIRE(probe.count(key) == 1); return probe.at(key); }
        void Click(ImVec2 at)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(at.x, at.y); Frame();
            io.AddMouseButtonEvent(0, true); Frame();
            io.AddMouseButtonEvent(0, false); Frame();
        }
        void Press(ImVec2 at) { ImGuiIO& io = ImGui::GetIO(); io.AddMousePosEvent(at.x, at.y); Frame(); io.AddMouseButtonEvent(0, true); Frame(); }
    };
}

TEST_CASE("PropertyGrid: rows draw into one shared grid, TextRow commits once, CheckboxRow toggles", "[editor][inspector]")
{
    GridHarness h;
    h.Frame();
    CHECK(h.sectionOpen);                       // DefaultOpen
    CHECK(h.state.labelColWidth > 0.0f);        // the first grid seeded the shared split
    REQUIRE(h.probe.count("Name") == 1);
    REQUIRE(h.probe.count("Blocking") == 1);

    h.Click(h.Centre("Blocking"));
    CHECK(h.flag);

    h.Click(h.Centre("Name"));                  // focus the text field
    ImGui::GetIO().AddInputCharacter('X'); h.Frame();
    CHECK(h.commits == 0);                      // typing alone never commits
    h.Click(ImVec2(600.0f, 900.0f));            // click empty window space: deactivate
    CHECK(h.commits == 1);
    CHECK(h.name.size() == 6);
    CHECK(h.name.find('X') != std::string::npos);
    h.Frame();
    CHECK(h.commits == 1);                      // exactly once
}

TEST_CASE("PropertyGrid: a draft deactivated while its row is not drawn still commits to the row's own target", "[editor][inspector]")
{
    GridHarness h; h.Frame();
    h.Click(h.Centre("Name"));
    ImGui::GetIO().AddInputCharacter('X'); h.Frame();
    CHECK(h.commits == 0);
    h.drawName = false;                                  // the A row vanishes (page switched / tab hidden); the B row draws under another id
    h.Frame(); h.Frame(); h.Frame();
    CHECK(h.commits == 1);                               // flushed by CommitOrphans, through A's stored commit
    CHECK(h.name.find('X') != std::string::npos);
    CHECK(h.commitsB == 0);
    CHECK(h.nameB == "Beta");
    h.Frame();
    CHECK(h.commits == 1);
}

TEST_CASE("PropertyGrid: FloatRow accumulates a multi-frame drag against a per-frame re-seeded value and commits once", "[editor][inspector]")
{
    GridHarness h; h.Frame();
    const ImVec2 c = h.Centre("Seconds");
    ImGuiIO& io = ImGui::GetIO();
    h.Press(c);
    for (int i = 1; i <= 3; ++i) { io.AddMousePosEvent(c.x + 15.0f * i, c.y); h.Frame(); }   // each step beats the 3 px drag threshold
    CHECK(h.floatCommits == 0);                          // in flight: nothing committed
    io.AddMouseButtonEvent(0, false); h.Frame();
    CHECK(h.floatCommits == 1);                          // exactly once, on release
    CHECK(h.storedSeconds > 0.40f + 0.25f);              // the whole gesture, not one frame's delta, not the original
    h.Frame();
    CHECK(h.floatCommits == 1);
    CHECK(h.state.numericDrafts.empty());
    h.Click(h.Centre("Seconds"));                        // press + release without moving
    CHECK(h.floatCommits == 1);                          // no MarkItemEdited: no commit
}

TEST_CASE("PropertyGrid: Escape during a numeric drag restores the seed and commits nothing", "[editor][inspector]")
{
    GridHarness h; h.Frame();
    const ImVec2 c = h.Centre("Scale");
    ImGuiIO& io = ImGui::GetIO();
    h.Press(c);
    io.AddMousePosEvent(c.x + 40.0f, c.y); h.Frame(); h.Frame();
    CHECK(h.scale != 1.0f);                              // the drag moved it (write-through)
    io.AddKeyEvent(ImGuiKey_Escape, true); h.Frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    io.AddMouseButtonEvent(0, false); h.Frame(); h.Frame();
    CHECK(h.scale == 1.0f);
    CHECK(h.scaleCommits == 0);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./ThirdParty/premake5/premake5.exe vs2026` then the MSBuild line. Expected: compile error, `Widgets/PropertyGrid.hpp` not found.

- [ ] **Step 3: Write PropertyGrid.hpp**

```cpp
#pragma once

// PropertyGrid (inspector-ownership spec s3.2 / input-editor spec s3.1): the
// section header, the two-column row region and the row primitives the scene
// Inspector draws components with, extracted so every Inspector PAGE (a
// document's map/action/binding pages, later the asset page) uses the same
// rows, wells, labels and bands. Built on the reflection-free widget layer
// (EditorWidgets.hpp: FieldGrid / FieldLabelCell / HeaderBand /
// InputTextString); knows nothing about Astra reflection.
//
// Row primitives return "an edit was COMMITTED this frame" (deactivate-after-
// edit for text/number rows, a click for checkbox/combo/button rows) so a
// page can route the write through its own undoable command. Drag/typing in
// flight commits nothing. While a drag or typed edit is in flight the row
// keeps its own draft and writes the in-flight number back into `value`
// every frame (a page may preview it) but commits nothing; on commit `value`
// holds the gesture's final number. Escape during a numeric drag cancels (no
// commit). TextRow selects all its text on activation (single-line rows).
//
// A row's ImGui id must include the TARGET's id -- the caller pushes it
// around the Rows scope (Task 10 does; the scene body's component rows
// already sit under the entity's id); the label alone is not an identity.

#include "Widgets/EditorWidgets.hpp"   // FieldGrid (Rows holds one)
#include <imgui.h>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Arcane::Editor
{
    // Persistent, per-panel-instance: the shared label split (UE's one split
    // per Details panel) and the in-flight TextRow / numeric drafts. Owned by
    // whoever owns the window (InspectorState::grid for the scene panel; one
    // per Inspector instance in InspectorWindowsState).
    struct PropertyGridState
    {
        float labelColWidth = 0.0f;   // 0 = not seeded yet; the first grid seeds it
        // One in-flight TextRow edit. A draft outlives the row that made it:
        // `commit` is bound to the TARGET the row was drawn for (UE commits
        // through the property handle the widget was built with), so an edit
        // deactivated while the row was not drawn can still land (CommitOrphans).
        struct TextDraft
        {
            std::string text;
            std::string seed;                             // value at activation
            bool active = false;                          // active on its last draw
            int lastFrame = 0;                            // ImGui frame it was last drawn
            std::function<void(std::string)> commit;      // bound to the row's target
        };
        std::unordered_map<unsigned int, TextDraft> textDrafts;   // keyed by ImGui id
        // One in-flight numeric gesture per IntRow/FloatRow (drag, held step
        // button, Ctrl+click text). The ROW owns the number while the widget
        // is active (UE SSpinBox InternalValue): pages re-derive their locals
        // from the draft every frame, and ImGui's drag accumulator is consumed
        // by what it applied last frame, so without this the gesture restarts
        // every frame and the release commits the untouched original. `seed`
        // is the value at activation; a commit is reported only when the
        // released value differs from it (UE: LastSliderCommittedValue != New).
        struct NumericDraft { double value = 0.0; double seed = 0.0; bool active = false; };
        std::unordered_map<unsigned int, NumericDraft> numericDrafts;   // keyed by ImGui id
        // TEST SEAM (PropertyGridTest): when non-null every row records the
        // centre of its VALUE widget under its label. Production: nullptr.
        std::unordered_map<std::string, ImVec2>* probe = nullptr;
    };

    class PropertyGrid
    {
    public:
        explicit PropertyGrid(PropertyGridState& state) : m_state(state) {}

        // Full-width headers -- draw these OUTSIDE a Rows scope.
        [[nodiscard]] bool Section(const char* label, bool defaultOpen = true);
        // Tree-style sub-header (the scene Inspector's category band). When it
        // returns true the caller draws its content and calls EndSubSection().
        [[nodiscard]] bool SubSection(std::string_view label, bool defaultOpen = true);
        void EndSubSection();

        // The two-column region rows go into. Bool-convertible like FieldGrid:
        // false = the host window is culled; submit NO rows.
        struct [[nodiscard]] Rows
        {
            Rows(PropertyGrid& grid, const char* id);
            ~Rows();
            explicit operator bool() const noexcept { return static_cast<bool>(m_grid); }
            Rows(const Rows&) = delete;
            Rows& operator=(const Rows&) = delete;
        private:
            FieldGrid m_grid;
        };

        bool TextRow(const char* label, std::string_view current,
                     std::function<void(std::string)> commit, bool dimmed = false);   // commit is STORED in the draft (see TextDraft)
        bool CheckboxRow(const char* label, bool& value);
        bool IntRow(const char* label, int& value);                          // true once per gesture, on deactivate-after-edit AND value != seed; value follows the gesture every frame
        bool FloatRow(const char* label, float& value, float speed = 0.01f); // same rule; Escape mid-drag = cancel, no commit
        int  ComboRow(const char* label, const char* const* items, int count, int current);
        void ReadOnlyRow(const char* label, std::string_view text);
        int  ButtonRow(const char* label, const char* const* buttons, int count,
                       unsigned enabledMask = ~0u);
        void MeterRow(const char* label, float fraction01, const char* overlay);
        // Flush drafts whose box was deactivated while its row was not drawn
        // (window hidden/closed, section collapsed, selection moved): commit
        // the text if it changed, then drop the draft. Call ONCE per frame per
        // state BEFORE any window that draws this state Begins.
        void CommitOrphans();

        PropertyGridState& State() noexcept { return m_state; }

    private:
        void Probe(const char* label);
        PropertyGridState& m_state;
    };
}
```

- [ ] **Step 4: Write PropertyGrid.cpp**

```cpp
#include "Widgets/PropertyGrid.hpp"
#include "Widgets/EditorTheme.hpp"
#include <imgui_internal.h>   // ClearActiveID (numeric-row Escape cancel)

#include <cfloat>

namespace Arcane::Editor
{
    void PropertyGrid::Probe(const char* label)
    {
        if (!m_state.probe) return;
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        (*m_state.probe)[label] = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
    }

    bool PropertyGrid::Section(const char* label, bool defaultOpen)
    {
        // The band pops at return, so a tooltip/popup drawn after the header
        // reads the theme's own Header colours (EditorPanels' rule).
        HeaderBand band;
        return ImGui::CollapsingHeader(label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    }

    bool PropertyGrid::SubSection(std::string_view label, bool defaultOpen)
    {
        ImGui::PushID(label.data(), label.data() + label.size());
        bool open = false;
        {
            HeaderBand band;
            open = ImGui::TreeNodeEx("##subsection", defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0,
                                     "%.*s", static_cast<int>(label.size()), label.data());
        }
        if (!open) ImGui::PopID();
        return open;
    }

    void PropertyGrid::EndSubSection()
    {
        ImGui::TreePop();
        ImGui::PopID();
    }

    PropertyGrid::Rows::Rows(PropertyGrid& grid, const char* id)
        : m_grid(id, grid.m_state.labelColWidth) {}
    PropertyGrid::Rows::~Rows() = default;

    bool PropertyGrid::TextRow(const char* label, std::string_view current,
                               std::function<void(std::string)> commit, bool dimmed)
    {
        using TextDraft = PropertyGridState::TextDraft;
        (void)FieldLabelCell(label, dimmed);
        ImGui::PushID(label);
        const unsigned int key = ImGui::GetID("##value");
        const int now = ImGui::GetFrameCount();
        auto [it, inserted] = m_state.textDrafts.try_emplace(
            key, TextDraft{ std::string(current), std::string(current), false, now, {} });
        TextDraft& draft = it->second;
        // Re-seed from live data whenever THIS widget was not active on its
        // last draw, OR was not drawn last frame at all (page/source switched
        // while the box was active: CommitOrphans owns that edit, never this
        // row). Gated on the draft's own flags, not IsAnyItemActive(): on the
        // frame a click elsewhere deactivates the box, ImGui may already have
        // moved ActiveId, and a global check would re-seed BEFORE InputText
        // reports IsItemDeactivatedAfterEdit -- wiping the edit it commits.
        if (!inserted && (!draft.active || draft.lastFrame + 1 < now) && draft.text != current)
        {
            draft.text.assign(current);
            draft.seed.assign(current);
            draft.active = false;
        }
        draft.lastFrame = now;
        draft.commit = commit;   // re-bound to THIS target every draw
        ImGui::BeginDisabled(dimmed);
        // Single-line property text selects all on activation (click or Tab/nav
        // into the box), the same rule as the document's inline rename box.
        InputTextString("##value", &draft.text, ImGuiInputTextFlags_AutoSelectAll);
        ImGui::EndDisabled();
        Probe(label);
        draft.active = ImGui::IsItemActive();
        bool committed = false;
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            if (inserted)
            {
                // A deactivation reported on a draft created THIS frame belongs
                // to the draft CommitOrphans already flushed (ImGui re-applies its
                // deactivated buffer for up to one extra frame): ignore it.
                draft.text.assign(current);
            }
            else
            {
                std::string edited = draft.text;
                auto fn = std::move(draft.commit);
                m_state.textDrafts.erase(it);
                if (edited != current) { fn(std::move(edited)); committed = true; }
            }
        }
        ImGui::PopID();
        return committed;
    }

    void PropertyGrid::CommitOrphans()
    {
        using TextDraft = PropertyGridState::TextDraft;
        // ImGui clears an ActiveId that was not submitted last frame (NewFrame)
        // and reports the deactivation for at most one further frame; a row not
        // drawn in that window never sees it. Erase before calling: the commit
        // may re-enter the grid.
        const int now = ImGui::GetFrameCount();
        for (auto it = m_state.textDrafts.begin(); it != m_state.textDrafts.end();)
        {
            TextDraft& d = it->second;
            if (d.active && d.lastFrame < now - 1)
            {
                auto fn = std::move(d.commit);
                std::string text = std::move(d.text);
                const bool changed = text != d.seed;
                it = m_state.textDrafts.erase(it);
                if (changed && fn) fn(std::move(text));
            }
            else ++it;
        }
    }

    bool PropertyGrid::CheckboxRow(const char* label, bool& value)
    {
        (void)FieldLabelCell(label, false);
        ImGui::PushID(label);
        const bool changed = ImGui::Checkbox("##value", &value);
        Probe(label);
        ImGui::PopID();
        return changed;
    }

    namespace
    {
        // Shared drag/commit path for both numeric rows. Runs right after
        // PushID(label); the caller draws the label cell first and Probe()s
        // after (the widget is still LastItemData -- Probe adds no item).
        template <typename T, typename DrawFn>
        bool NumericRow(PropertyGridState& state, T& value, DrawFn&& draw)
        {
            const unsigned int key = ImGui::GetID("##value");
            auto [it, inserted] = state.numericDrafts.try_emplace(key, PropertyGridState::NumericDraft{});
            PropertyGridState::NumericDraft& draft = it->second;
            // Re-seed whenever THIS widget was not active on its last draw (the
            // draft's own flag, never IsAnyItemActive() -- see TextRow). While
            // active the external value is never consulted: pages mutate the
            // document only on commit, so there is no mid-gesture change to adopt.
            if (!draft.active) { draft.value = static_cast<double>(value); draft.seed = draft.value; }
            T local = static_cast<T>(draft.value);
            draw(local);
            draft.value = static_cast<double>(local);
            value = local;                                   // write-through: the caller's reference follows the gesture
            draft.active = ImGui::IsItemActive();
            // Escape mid-DRAG (button held; text mode has no button down and
            // InputText reverts on its own): restore the seed, end the gesture.
            if (draft.active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                value = static_cast<T>(draft.seed);
                ImGui::ClearActiveID();
                state.numericDrafts.erase(it);
                return false;
            }
            bool committed = false;
            if (ImGui::IsItemDeactivatedAfterEdit())
                committed = draft.value != draft.seed;       // released ON the seed = no undo step
            if (ImGui::IsItemDeactivated())
                state.numericDrafts.erase(it);               // gesture over (edited or not)
            return committed;
        }
    }

    bool PropertyGrid::IntRow(const char* label, int& value)
    {
        (void)FieldLabelCell(label, false);
        ImGui::PushID(label);
        const bool committed = NumericRow(m_state, value, [](int& v) { ImGui::InputInt("##value", &v); });
        Probe(label);
        ImGui::PopID();
        return committed;
    }

    bool PropertyGrid::FloatRow(const char* label, float& value, float speed)
    {
        (void)FieldLabelCell(label, false);
        ImGui::PushID(label);
        const bool committed = NumericRow(m_state, value,
            [speed](float& v) { ImGui::DragFloat("##value", &v, speed, 0.0f, 0.0f, "%.2f"); });
        Probe(label);
        ImGui::PopID();
        return committed;
    }

    int PropertyGrid::ComboRow(const char* label, const char* const* items, int count, int current)
    {
        (void)FieldLabelCell(label, false);
        ImGui::PushID(label);
        int picked = -1;
        const char* preview = (current >= 0 && current < count) ? items[current] : "";
        if (ImGui::BeginCombo("##value", preview))
        {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(items[i], i == current) && i != current)
                    picked = i;
            ImGui::EndCombo();
        }
        Probe(label);
        ImGui::PopID();
        return picked;
    }

    void PropertyGrid::ReadOnlyRow(const char* label, std::string_view text)
    {
        (void)FieldLabelCell(label, true);
        ImGui::PushID(label);
        ImGui::TextDisabled("%.*s", static_cast<int>(text.size()), text.data());
        Probe(label);
        ImGui::PopID();
    }

    int PropertyGrid::ButtonRow(const char* label, const char* const* buttons, int count,
                                unsigned enabledMask)
    {
        (void)FieldLabelCell(label, false);
        ImGui::PushID(label);
        int clicked = -1;
        for (int i = 0; i < count; ++i)
        {
            if (i > 0) ImGui::SameLine();
            ImGui::BeginDisabled(((enabledMask >> i) & 1u) == 0);
            if (ImGui::SmallButton(buttons[i])) clicked = i;
            ImGui::EndDisabled();
        }
        Probe(label);
        ImGui::PopID();
        return clicked;
    }

    void PropertyGrid::MeterRow(const char* label, float fraction01, const char* overlay)
    {
        (void)FieldLabelCell(label, true);
        ImGui::PushID(label);
        ImGui::ProgressBar(fraction01 < 0.0f ? 0.0f : fraction01 > 1.0f ? 1.0f : fraction01,
                           ImVec2(-FLT_MIN, 0.0f), overlay);   // PlotHistogram = Theme::kAmber
        Probe(label);
        ImGui::PopID();
    }
}
```

Then register the new .cpp with the test exe. `ArcaneTests` lists editor sources one by one (nothing globs them): in `premake5.lua`, the `files {` block of `project "ArcaneTests"`, insert after `"%{wks.location}/ArcaneEditor/src/Widgets/ColorPickerPopup.cpp",` (:1281):

```lua
        -- Inspector ownership T1: PropertyGrid (the shared page rows) source-
        -- compiles into the test exe: PropertyGridTest drives it under a real
        -- ImGui context, and InputActionsInspectorPage.cpp (T10) links against it.
        "%{wks.location}/ArcaneEditor/src/Widgets/PropertyGrid.cpp",
```

(The entry lands in the same step that writes the file: an entry for a file that does not exist yet makes MSBuild fail on the missing source. Without it Step 6 fails with LNK2019 from PropertyGridTest.)

- [ ] **Step 5: Move the scene Inspector onto it**

Line numbers are advisory; the quoted text and the grep sweep govern. In `EditorPanels.hpp`: replace `float labelColWidth = 0.0f;` (InspectorState, :553) with `PropertyGridState grid;` (add `#include "Widgets/PropertyGrid.hpp"`); keep the comment, reworded: "the shared split now lives on the PropertyGridState every page draws through". Rename the declaration `DrawInspectorPanel(...)` (:577-582; drop the `bool* open = nullptr` parameter at :581 -- `selectedAsset` stays the last, defaulted parameter, the 8-argument shape the Task 1 call site and Task 3's Draw use) to:

```cpp
    // The scene page's BODY -- everything the Inspector shows for the scene
    // selection, drawn into the CURRENT window (the caller Begins it; since
    // the inspector-ownership arc that caller is the SceneInspectorSource
    // page inside an Inspector instance window, never a panel of its own).
    void DrawInspectorBody(Astra::Registry& registry, const SelectionContext& sel,
                           Arcane::CommandStack& undo, const SceneEditBinding& binding,
                           const Arcane::Project* project, InspectorState& state,
                           const InspectorServices* services = nullptr,
                           const Arcane::Guid& selectedAsset = Arcane::Guid{});
```

In `EditorPanels.cpp:2367-2846`: rename; delete the `ImGui::Begin("Inspector", open);` line and every `ImGui::End();` inside the function (three: the two early returns and the tail); construct `PropertyGrid pg(state.grid);` after the ScopeGuard; replace the HeaderBand+CollapsingHeader block (:2616-2621) with `open = pg.Section(headerLabel.c_str());`; replace both `FieldGrid grid{ "##fields", state.labelColWidth }; if (grid) DrawReflectedComponent(fieldArgs);` blocks (:2766-2769, :2808-2811) with `PropertyGrid::Rows rows(pg, "##fields"); if (rows) DrawReflectedComponent(fieldArgs);`; replace the category block (:2778-2815) with:

```cpp
                    for (const std::string_view cat : categories)
                    {
                        if (!pg.SubSection(cat))
                            continue;
                        fieldArgs.activeCategory = cat;
                        {
                            PropertyGrid::Rows rows(pg, "##fields");
                            if (rows)
                                DrawReflectedComponent(fieldArgs);
                        }
                        pg.EndSubSection();
                    }
```

Keep the existing comments that explain the id scope and the indent. In `EditorAppFrame.cpp:3591-3600` the call temporarily becomes (Task 4 replaces it again):

```cpp
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Inspector))
        {
            ImGui::Begin("Inspector", m_panelVis.OpenFlag(Arcane::Editor::PanelId::Inspector));
            Arcane::Editor::DrawInspectorBody(m_runtime->Registry(), m_selection, *m_undo,
                                              m_editBinding, m_runtime->CurrentProject(),
                                              m_inspector, &m_inspectorServices, m_assetModel.selected);
            ImGui::End();
        }
```

`EditorInspectorVectorTest.cpp:324`: `state.labelColWidth` -> `state.grid.labelColWidth`. Then `grep -rn "labelColWidth\|DrawInspectorPanel" ArcaneEditor/src ArcaneTests/src`. Rename the DrawInspectorPanel comment mentions to DrawInspectorBody in EditorApp.hpp:1044, InspectorView.hpp:9, InspectorView.cpp:101,106, ComponentCatalog.hpp:27,75, EditorPanels.hpp:420,424, EditorPanels.cpp:2257, EditorAppFrame.cpp:3595 and EditorComponentCatalogTest.cpp:373; reword the two EditorWidgets.cpp:84,92 comments so the width authority is `PropertyGridState::labelColWidth`. Leave FieldGrid's own `labelColWidth` parameter (EditorWidgets.hpp:97,112; EditorWidgets.cpp:159-558) as is -- PropertyGrid::Rows wraps it.

- [ ] **Step 6: Build, run the tests**

Run: premake, MSBuild, then from the tests dir `./ArcaneTests.exe "[editor][inspector]"` and `./ArcaneTests.exe "[editor]"`. Expected: the four new cases pass -- the drag case is the one a re-seeding-blind FloatRow fails, the hidden-row case the one a label-keyed draft without CommitOrphans fails; the `[editor]` suite is green (EditorInspectorVectorTest included).

- [ ] **Step 7: Golden check** -- `scripts/golden-gate.ps1 -Configuration Debug`. Expected: 8/8 green (the scene page's pixels are unchanged: same band, same grid, same rows).

- [ ] **Step 8: Commit**

`git status --short` must show nothing unstaged under ArcaneEditor/ or ArcaneTests/.

```bash
git add premake5.lua ArcaneEditor/src/Widgets/PropertyGrid.hpp ArcaneEditor/src/Widgets/PropertyGrid.cpp ArcaneEditor/src/Panels/EditorPanels.hpp ArcaneEditor/src/Panels/EditorPanels.cpp ArcaneEditor/src/Panels/InspectorView.hpp ArcaneEditor/src/Panels/InspectorView.cpp ArcaneEditor/src/Scene/ComponentCatalog.hpp ArcaneEditor/src/Widgets/EditorWidgets.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/PropertyGridTest.cpp ArcaneTests/src/EditorInspectorVectorTest.cpp ArcaneTests/src/EditorComponentCatalogTest.cpp
git commit -m "feat(editor): PropertyGrid -- the Inspector's section/row primitives extracted for every page; the scene panel body becomes DrawInspectorBody (inspector ownership plan T1)"
```

---

### Task 2: InspectorSource, InspectorPage, InspectorHost (pure) + EditorDocument defaults

**Files:**
- Create: `ArcaneEditor/src/Panels/InspectorSource.hpp`, `ArcaneEditor/src/Panels/InspectorHost.hpp`, `ArcaneEditor/src/Panels/InspectorHost.cpp`
- Modify: `ArcaneEditor/src/Documents/EditorDocument.hpp`, `ArcaneEditor/src/Scene/SelectionContext.hpp` (`Epoch()`), `premake5.lua` (the `ArcaneTests` `files` block: one entry for `InspectorHost.cpp`)
- Test: `ArcaneTests/src/EditorInspectorHostTest.cpp`, `ArcaneTests/src/SelectionOpsTest.cpp` (one case)

**Interfaces:**
- Consumes: `PropertyGrid` (forward-declared; pages draw through it -- Task 1's `bool TextRow(const char* label, std::string_view current, std::function<void(std::string)> commit, bool dimmed = false)`, the commit STORED in the row's draft so an edit deactivated while the row is hidden still lands through `CommitOrphans`).
- Produces:
  ```cpp
  struct InspectorCrumb { std::string label; std::function<void()> select; std::optional<std::string> key; };   // select re-selects that level in the SOURCE (an unpinned instance); key = that level's selection key, a PINNED instance re-targets its own pin to it (nullopt = no page at that level: inert while pinned)
  class InspectorPage { virtual std::vector<InspectorCrumb> Breadcrumb() const = 0; virtual void Draw(PropertyGrid&) = 0; };
  class InspectorSource {
      virtual std::string SourceName() const = 0;                  // "Scene", "Player.arcinput"
      virtual InspectorPage* Page() = 0;                           // the page for the CURRENT selection (null = none)
      virtual InspectorPage* PageFor(std::string_view key) = 0;    // the page for a specific selection key (null = unresolvable)
      virtual std::string SelectionKey() const = 0;                // opaque; "" = nothing selected
      virtual bool RestoreSelection(std::string_view key) = 0;     // false = unresolvable now
      virtual bool Resolves(std::string_view key) const = 0;       // PURE: would RestoreSelection(key) succeed right now? never selects, never bumps an epoch
  };
  std::string InspectorCrumbText(const InspectorSource&, const InspectorPage*);   // the crumb labels joined with " > ", or SourceName() without a page: history labels, header tooltips, Task 12's report
  struct SelectionEdge { std::uint64_t lastEpoch = 0; bool Observe(std::uint64_t epoch, std::string_view key); };   // true = a selection GESTURE (epoch moved) that left a non-empty key
  class InspectorHost {  // see Step 4 for the full declaration
      static constexpr std::size_t kHistoryDepth = 32;
      static constexpr int kMaxInstances = 8;                      // ids 0..7: a FIXED pool of dock slots, lowest free reused, never minted
      struct Instance { int id; bool pinned; InspectorSource* pinnedSource; std::string pinnedKey; std::string pinnedName; bool sourceClosed; };
      struct HistoryEntry { InspectorSource* source; std::string key; std::string label; };   // label = InspectorCrumbText at push time
      explicit InspectorHost(InspectorSource& fallback);
      void AddSource(InspectorSource&); void RemoveSource(InspectorSource&); void InvalidateSource(InspectorSource&); void NotifySelected(InspectorSource&);
      InspectorSource& Current() const; InspectorSource* SourceFor(int instanceId) const;
      const std::vector<Instance>& Instances() const; Instance* Find(int id); int AddInstance(); void RemoveInstance(int id); void SetInstanceIds(std::span<const int> extras);
      bool CanPin(); void SetPinned(int id, bool pinned); void RepinKey(int id, std::string key);
      bool CanGoBack() const; bool CanGoForward() const; bool GoBack(); bool GoForward(); bool JumpTo(std::size_t index);
      const HistoryEntry* BackEntry() const; const HistoryEntry* ForwardEntry() const;
      const std::vector<HistoryEntry>& History() const; std::size_t HistoryCursor() const; void PruneStale();
      void ReleaseAll();
  };
  ```
  `SelectionContext` gains `[[nodiscard]] std::uint64_t Epoch() const noexcept` -- monotonic, bumped by `Select`/`Toggle`/`AddRange`/`Clear` only; `Prune()` does NOT bump it (a prune is not a gesture).
  `EditorDocument : public InspectorSource` with defaults: `SourceName()` = `Title()`, `Page()`/`PageFor()` = nullptr, `SelectionKey()` = "", `RestoreSelection()` = false, `Resolves()` = false, plus `virtual std::uint64_t SelectionEpoch() const { return 0; }` and `virtual bool SelectByPath(std::string_view) { return false; }`.

- [ ] **Step 1: Write the failing tests** (`ArcaneTests/src/EditorInspectorHostTest.cpp`)

```cpp
// InspectorHost (inspector-ownership spec s5): the PURE routing -- which
// source's page an Inspector instance shows -- driven with fake sources, no
// ImGui. Every rule in spec s3.1/s3.3/s6 has a case here.
#include <catch2/catch_test_macros.hpp>
#include <Panels/InspectorHost.hpp>
#include <string>
#include <vector>

using namespace Arcane::Editor;

namespace
{
    struct FakePage final : InspectorPage
    {
        std::vector<InspectorCrumb> crumbs;
        std::vector<InspectorCrumb> Breadcrumb() const override { return crumbs; }
        void Draw(PropertyGrid&) override {}
    };
    struct FakeSource final : InspectorSource
    {
        std::string name;
        std::string key;
        bool restoreOk = true;
        std::string normalizeTo;            // non-empty: RestoreSelection lands on THIS key, not the asked one (a member died in between)
        std::vector<std::string> restored;
        FakePage page;
        explicit FakeSource(std::string n) : name(std::move(n)) {}
        std::string SourceName() const override { return name; }
        InspectorPage* Page() override { return &page; }
        InspectorPage* PageFor(std::string_view) override { return restoreOk ? &page : nullptr; }
        std::string SelectionKey() const override { return key; }
        bool RestoreSelection(std::string_view k) override
        {
            if (!restoreOk) return false;
            key = normalizeTo.empty() ? std::string(k) : normalizeTo;
            restored.push_back(key);
            return true;
        }
        bool Resolves(std::string_view) const override { return restoreOk; }
    };
    struct World
    {
        FakeSource scene{ "Scene" }, doc{ "Player.arcinput" }, other{ "mat.arcshader" };
        InspectorHost host{ scene };
        World() { host.AddSource(doc); host.AddSource(other); }
        void Select(FakeSource& s, std::string k) { s.key = std::move(k); host.NotifySelected(s); }
    };
}

TEST_CASE("InspectorHost: the last-selecting source wins; nothing else moves it", "[editor][inspector]")
{
    World w;
    CHECK(&w.host.Current() == &w.scene);            // fallback at rest
    w.Select(w.doc, "m/a//");
    CHECK(&w.host.Current() == &w.doc);
    w.Select(w.scene, "7");
    CHECK(&w.host.Current() == &w.scene);
    // Focus is not selection: touching the other source's page, adding an
    // instance, reading keys -- none of it is an event.
    (void)w.other.Page(); (void)w.host.AddInstance(); (void)w.host.SourceFor(0);
    CHECK(&w.host.Current() == &w.scene);
    // A deselect (empty key) is not a selection event either.
    w.Select(w.doc, "");
    CHECK(&w.host.Current() == &w.scene);
    // A re-select of the scene's UNCHANGED key is still a selection event
    // (UE: SelectActor re-notifies the Details views even when the set did
    // not change): doc selects, scene re-selects "7" -> scene wins again, and
    // history gains a scene entry so Back returns to the document.
    w.Select(w.doc, "m/a//");
    CHECK(&w.host.Current() == &w.doc);
    w.Select(w.scene, "7");
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.host.CanGoBack());
    REQUIRE(w.host.GoBack());
    CHECK(&w.host.Current() == &w.doc);
}

TEST_CASE("SelectionEdge: a selection event is a gesture (epoch moved) with a non-empty key", "[editor][inspector]")
{
    SelectionEdge edge;
    CHECK(edge.Observe(1, "7"));        // first gesture
    CHECK_FALSE(edge.Observe(1, "7"));  // nothing happened (focus, idle)
    CHECK_FALSE(edge.Observe(1, "9"));  // key moved WITHOUT a gesture (a prune re-primaried): not an event
    CHECK(edge.Observe(2, "7"));        // re-clicking the already-selected entity IS an event
    CHECK_FALSE(edge.Observe(3, ""));   // a clear moves the epoch but is never an event
    CHECK(edge.Observe(4, "7"));        // re-selecting after a clear is one
}

TEST_CASE("InspectorHost: pin holds a page while others select; a second instance follows", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "m/a//");
    w.host.SetPinned(0, true);
    const int second = w.host.AddInstance();
    w.Select(w.scene, "7");
    CHECK(w.host.SourceFor(0) == &w.doc);
    CHECK(w.host.Find(0)->pinnedKey == "m/a//");
    CHECK(w.host.SourceFor(second) == &w.scene);
    w.host.SetPinned(0, false);
    CHECK(w.host.SourceFor(0) == &w.scene);
    w.scene.restoreOk = false;                       // PageFor(key) is null: nothing to hold
    w.host.SetPinned(second, true);
    CHECK_FALSE(w.host.Find(second)->pinned);
    w.scene.restoreOk = true; w.scene.key.clear();   // an EMPTY key whose page resolves (a document's asset page) IS pinnable
    w.host.SetPinned(second, true);
    CHECK(w.host.Find(second)->pinned);
    CHECK(w.host.Find(second)->pinnedKey.empty());
    w.host.SetPinned(second, false);
    w.host.RemoveInstance(0);                        // refused: instance 0 is permanent
    CHECK(w.host.Instances().size() == 2);
    w.host.RemoveInstance(second);
    CHECK(w.host.Instances().size() == 1);
}

TEST_CASE("InspectorHost: a closed source releases the follower and flags the pin", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "m/a//");
    w.host.SetPinned(0, true);
    const int second = w.host.AddInstance();
    w.Select(w.doc, "m/b//");
    w.host.RemoveSource(w.doc);
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.host.SourceFor(second) == &w.scene);
    REQUIRE(w.host.Find(0)->pinned);
    CHECK(w.host.Find(0)->sourceClosed);
    CHECK(w.host.SourceFor(0) == nullptr);
    CHECK(w.host.Find(0)->pinnedName == "Player.arcinput");
    CHECK(w.host.History().empty());                 // every entry named the closed source
    w.host.SetPinned(0, false);
    CHECK(w.host.SourceFor(0) == &w.scene);
}

TEST_CASE("InspectorHost: history back/forward restores the selection in its source", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a");
    w.Select(w.doc, "b");
    w.Select(w.scene, "7");
    REQUIRE(w.host.History().size() == 3);
    CHECK(w.host.History()[2].label == "Scene");   // FakePage has no crumbs: the label is the source name
    CHECK(w.host.HistoryCursor() == 2);
    CHECK(w.host.CanGoBack());
    CHECK_FALSE(w.host.CanGoForward());
    REQUIRE(w.host.GoBack());
    CHECK(&w.host.Current() == &w.doc);
    CHECK(w.doc.restored.back() == "b");
    // The document re-reports the restored key next frame: the same entry
    // as the cursor, so nothing is pushed and forward history survives.
    w.host.NotifySelected(w.doc);
    CHECK(w.host.History().size() == 3);
    CHECK(w.host.CanGoForward());
    REQUIRE(w.host.GoBack());
    CHECK(w.doc.restored.back() == "a");
    REQUIRE(w.host.GoForward());
    CHECK(w.doc.restored.back() == "b");
    // A NEW selection at cursor 1 truncates the forward entries.
    w.Select(w.other, "n1");
    CHECK(w.host.History().size() == 3);
    CHECK_FALSE(w.host.CanGoForward());
}

TEST_CASE("InspectorHost: GoBack skips and prunes entries that no longer resolve", "[editor][inspector]")
{
    World w;
    w.Select(w.scene, "1");
    w.Select(w.doc, "a");
    w.Select(w.doc, "b");
    w.Select(w.scene, "7");
    w.doc.restoreOk = false;                         // the bindings were deleted
    REQUIRE(w.host.GoBack());
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.scene.restored.back() == "1");
    CHECK(w.host.History().size() == 2);             // both doc entries pruned
    w.scene.restoreOk = false;
    CHECK_FALSE(w.host.GoForward());                 // "7" unresolvable: pruned, nothing to land on
    CHECK(w.host.History().size() == 1);
}

TEST_CASE("InspectorHost: history is capped at 32 and never grows past it", "[editor][inspector]")
{
    World w;
    for (int i = 0; i < 40; ++i) w.Select(w.doc, "k" + std::to_string(i));
    CHECK(w.host.History().size() == InspectorHost::kHistoryDepth);
    CHECK(w.host.History().front().key == "k8");
    CHECK(w.host.HistoryCursor() == InspectorHost::kHistoryDepth - 1);
}

TEST_CASE("InspectorHost: ReleaseAll drops every non-fallback source, the history and the pins", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a");
    w.host.SetPinned(0, true);
    const int ids[] = { 2, 3, 3, 0, 99 }; w.host.SetInstanceIds(ids);   // {0,2,3}: 0 implicit, dup + out-of-range dropped
    CHECK(w.host.Instances().size() == 3);
    w.host.ReleaseAll();
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.host.History().empty());
    for (const auto& inst : w.host.Instances()) CHECK_FALSE(inst.pinned);
    CHECK(w.host.Instances().size() == 3);           // instances are layout, not project state
    w.host.NotifySelected(w.doc);                    // an unregistered source is ignored
    CHECK(&w.host.Current() == &w.scene);
}

TEST_CASE("InspectorHost: InvalidateSource prunes history and releases pins but keeps the source registered", "[editor][inspector]")
{
    World w;
    w.Select(w.scene, "1");
    w.host.SetPinned(0, true);
    w.Select(w.doc, "a");
    REQUIRE(w.host.History().size() == 2);
    w.host.InvalidateSource(w.scene);                // the registry was replaced
    CHECK(&w.host.Current() == &w.doc);              // current is untouched
    REQUIRE(w.host.History().size() == 1);
    CHECK(w.host.History()[0].key == "a");
    CHECK(w.host.HistoryCursor() == 0);
    REQUIRE(w.host.Find(0)->pinned);
    CHECK(w.host.SourceFor(0) == nullptr);           // "Pinned selection is gone"
    CHECK_FALSE(w.host.Find(0)->sourceClosed);
    w.host.NotifySelected(w.scene);                  // still registered: the next scene selection lands
    CHECK(&w.host.Current() == &w.scene);
    w.host.SetPinned(0, false);
    CHECK(w.host.SourceFor(0) == &w.scene);
}

TEST_CASE("InspectorHost: PruneStale drops entries that no longer resolve and CanGoBack turns false without a click", "[editor][inspector]")
{
    World w;
    w.Select(w.scene, "1"); w.Select(w.doc, "a"); w.Select(w.doc, "b"); w.Select(w.scene, "7");
    w.scene.restoreOk = false; w.doc.restoreOk = false;
    w.host.PruneStale();
    CHECK(w.host.History().empty());
    CHECK_FALSE(w.host.CanGoBack());
    CHECK_FALSE(w.host.CanGoForward());
    CHECK(w.scene.restored.empty());                 // nothing was selected by the prune
}

TEST_CASE("InspectorHost: pruning an earlier duplicate keeps the cursor on the same entry", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a"); w.Select(w.other, "x"); w.Select(w.doc, "b"); w.Select(w.doc, "a");
    REQUIRE(w.host.History().size() == 4);
    w.host.RemoveSource(w.other);
    REQUIRE(w.host.History().size() == 3);
    CHECK(w.host.HistoryCursor() == 2);
    CHECK(w.host.History()[2].key == "a");
    REQUIRE(w.host.GoBack());
    CHECK(w.doc.restored.back() == "b");             // a key search would have landed on index 0 and Back done nothing
}

TEST_CASE("InspectorHost: a restore that lands on a normalized key keeps forward history", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a,b,c"); w.Select(w.scene, "7");
    w.doc.normalizeTo = "a,c";                       // b died between push and restore
    REQUIRE(w.host.GoBack());
    CHECK(w.host.History()[0].key == "a,c");         // re-snapshotted
    w.host.NotifySelected(w.doc);                    // the next-frame re-report
    CHECK(w.host.History().size() == 2);             // an echo, not a new entry
    CHECK(w.host.CanGoForward());
    REQUIRE(w.host.GoForward());
    CHECK(w.scene.restored.back() == "7");
}

TEST_CASE("InspectorHost: JumpTo lands on the chosen entry or prunes only it; entries carry labels", "[editor][inspector]")
{
    World w;
    w.doc.page.crumbs = { { "Player.arcinput", {}, {} }, { "Player", {}, {} }, { "Jump", {}, {} } };
    w.Select(w.scene, "1"); w.Select(w.doc, "a"); w.Select(w.doc, "b"); w.Select(w.scene, "7");
    CHECK(w.host.History()[1].label == "Player.arcinput > Player > Jump");
    REQUIRE(w.host.JumpTo(1));
    CHECK(w.doc.restored.back() == "a");
    CHECK(w.host.HistoryCursor() == 1);
    CHECK(w.host.CanGoForward());
    CHECK(w.host.BackEntry() != nullptr); CHECK(w.host.BackEntry()->key == "1");
    w.doc.restoreOk = false;
    CHECK_FALSE(w.host.JumpTo(2));                   // that one entry is pruned, nothing else moves
    CHECK(w.host.History().size() == 3);
    CHECK(w.host.HistoryCursor() == 1);
    CHECK_FALSE(w.host.JumpTo(1));                   // index == cursor: no-op
    CHECK_FALSE(w.host.JumpTo(99));
}

TEST_CASE("InspectorHost: instance ids are a reusable pool of 8 slots, never minted", "[editor][inspector]")
{
    World w;
    CHECK(w.host.AddInstance() == 1);
    CHECK(w.host.AddInstance() == 2);
    w.host.RemoveInstance(1);
    CHECK(w.host.AddInstance() == 1);                // the freed slot, not 3
    for (int i = 0; i < 8; ++i) (void)w.host.AddInstance();
    CHECK(w.host.Instances().size() == static_cast<std::size_t>(InspectorHost::kMaxInstances));
    CHECK(w.host.AddInstance() == -1);
    w.host.RemoveInstance(0);                        // still refused
    CHECK(w.host.Instances().size() == static_cast<std::size_t>(InspectorHost::kMaxInstances));
    const int ids[] = { 0, 3, 3, 42, -1 };
    w.host.SetInstanceIds(ids);
    REQUIRE(w.host.Instances().size() == 2);
    CHECK(w.host.Instances()[1].id == 3);
}

TEST_CASE("InspectorHost: RepinKey moves only the pinned instance's key", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "m/a//");
    w.host.SetPinned(0, true);
    const auto historyBefore = w.host.History().size();
    w.host.RepinKey(0, "m///");
    CHECK(w.host.Find(0)->pinnedKey == "m///");
    CHECK(w.host.SourceFor(0) == &w.doc);
    CHECK(w.doc.key == "m/a//");                       // the source's selection did not move
    CHECK(w.host.History().size() == historyBefore);   // no history push
    w.host.SetPinned(0, false);
    w.host.RepinKey(0, "x");                           // unpinned: ignored
    CHECK(w.host.Find(0)->pinnedKey.empty());
}
```

And the SelectionContext epoch case, appended to `ArcaneTests/src/SelectionOpsTest.cpp` (add `#include "Scene/SelectionContext.hpp"` beside its `Scene/SelectionOps.hpp` include; the file already has `using namespace Arcane;`):

```cpp
TEST_CASE("SelectionContext: every selection action bumps Epoch, including a re-select; Prune does not", "[editor][selection]")
{
    using Astra::Entity;
    Arcane::Editor::SelectionContext sel;
    const auto e0 = sel.Epoch();
    const Entity a(static_cast<Entity::StorageType>(7)), b(static_cast<Entity::StorageType>(9));
    sel.Select(a);                       CHECK(sel.Epoch() == e0 + 1);
    sel.Select(a);                       CHECK(sel.Epoch() == e0 + 2);   // re-click on the selected entity IS an action
    sel.Toggle(b);                       CHECK(sel.Epoch() == e0 + 3);
    sel.Prune([&](Entity e) { return e != b; });                          // the primary (b) dies: primary falls back to a
    CHECK(sel.Primary() == a);
    CHECK(sel.Epoch() == e0 + 3);                                         // a sweep is not a gesture
    sel.Clear();                         CHECK(sel.Epoch() == e0 + 4);
}
```

- [ ] **Step 2: Run to verify it fails** -- premake + build. Expected: `Panels/InspectorHost.hpp` not found (and, in SelectionOpsTest.cpp, `Epoch` is not a member of `SelectionContext`).

- [ ] **Step 3: Write InspectorSource.hpp**

```cpp
#pragma once

// The Inspector's contribution interfaces (inspector-ownership spec s3.1/s3.2).
// A SOURCE is anything that can select (the scene, an opted-in document); it
// contributes a PAGE (breadcrumb + body drawn through PropertyGrid). No ImGui
// here: InspectorHost routes these headlessly, InspectorWindows draws them.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    class PropertyGrid;

    // One breadcrumb segment. `select` re-selects that level in the SOURCE
    // (an unpinned instance). `key` is that level's selection key: a PINNED
    // instance re-targets its own pin to it (InspectorHost::RepinKey) and
    // never touches the source; nullopt = no page exists at that level (the
    // crumb draws inert while pinned).
    struct InspectorCrumb
    {
        std::string label;
        std::function<void()> select;
        std::optional<std::string> key;
    };

    class InspectorPage
    {
    public:
        virtual ~InspectorPage() = default;
        [[nodiscard]] virtual std::vector<InspectorCrumb> Breadcrumb() const = 0;
        virtual void Draw(PropertyGrid& grid) = 0;
    };

    class InspectorSource
    {
    public:
        virtual ~InspectorSource() = default;
        // The first crumb: "Scene", "Player.arcinput".
        [[nodiscard]] virtual std::string SourceName() const = 0;
        // The page for the CURRENT selection. Null = nothing to show.
        [[nodiscard]] virtual InspectorPage* Page() = 0;
        // The page for a SPECIFIC selection key (a pinned instance). Null when
        // the key no longer resolves. Pages are keyed on purpose: "pinned =
        // keep THIS page" must survive the source selecting something else.
        [[nodiscard]] virtual InspectorPage* PageFor(std::string_view key) = 0;
        // Opaque, stable across frames; "" = nothing selected. The history
        // stores these, so a key must survive being handed back later.
        [[nodiscard]] virtual std::string SelectionKey() const = 0;
        // Re-select `key` inside the source. False = unresolvable now (the
        // entity died, the binding was removed): the host prunes the entry.
        virtual bool RestoreSelection(std::string_view key) = 0;
        // PURE: would RestoreSelection(key) succeed right now? Never selects,
        // never bumps an epoch. The host prunes stale history with it once
        // per frame so the back/forward arrows are truthful (spec s6 rule 3).
        [[nodiscard]] virtual bool Resolves(std::string_view key) const = 0;
    };

    // The page's crumb labels joined with " > ", or the source's name when
    // there is no page / an empty breadcrumb. History labels, the header
    // tooltips and the witness report's `inspector.breadcrumb` all use it.
    [[nodiscard]] inline std::string InspectorCrumbText(const InspectorSource& src, const InspectorPage* page)
    {
        std::string out;
        if (page)
            for (const InspectorCrumb& c : page->Breadcrumb()) { if (!out.empty()) out += " > "; out += c.label; }
        return out.empty() ? src.SourceName() : out;
    }
}
```

- [ ] **Step 4: Write InspectorHost.hpp**

```cpp
#pragma once

// InspectorHost (inspector-ownership spec s3): which source's page each
// Inspector INSTANCE shows. Pure state -- no ImGui -- so the rules are unit-
// tested (EditorInspectorHostTest): the last-selecting source wins; focus is
// never an event (there is no focus API here at all); pin holds a keyed page;
// a closed source releases; history depth 32, prune-on-invalidate (PruneStale,
// once per frame, through the sources' PURE Resolves), skip at navigation,
// never persisted; every source but the fallback releases on project switch;
// the fallback is INVALIDATED (history + pins) on every scene swap.

#include "Panels/InspectorSource.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    // The app's per-source "was that a selection EVENT" filter (spec s3.1):
    // a selection GESTURE (the source's epoch moved) that left a non-empty
    // key. A clear is never an event; a key that changed without a gesture
    // (a prune re-primaried a multi-selection after a deletion or a
    // structural undo) is never an event; re-selecting the already-selected
    // thing IS one -- it re-asserts the Inspector on that source without
    // moving history (Push's echo compare swallows the duplicate).
    struct SelectionEdge
    {
        std::uint64_t lastEpoch = 0;
        bool Observe(std::uint64_t epoch, std::string_view key)
        {
            const bool event = epoch != lastEpoch && !key.empty();
            lastEpoch = epoch;
            return event;
        }
    };

    class InspectorHost
    {
    public:
        static constexpr std::size_t kHistoryDepth = 32;
        // ids 0..7: a FIXED pool of stable window keys (UE keeps Details 1..4),
        // so a closed slot's imgui.ini dock entry is the one its next opener
        // inherits. Never minted, always the lowest free slot.
        static constexpr int kMaxInstances = 8;

        struct Instance
        {
            int  id = 0;                    // 0 = the permanent "Inspector" window
            bool pinned = false;
            InspectorSource* pinnedSource = nullptr;   // null once the key died or the source closed
            std::string pinnedKey;
            std::string pinnedName;         // survives the source's death (the "closed" note)
            bool sourceClosed = false;      // true only for a CLOSED source; a dead key on a live one reads "Pinned selection is gone"
        };
        struct HistoryEntry
        {
            InspectorSource* source;
            std::string key;
            std::string label;              // InspectorCrumbText at push time, refreshed when the cursor leaves it
        };

        // `fallback` (the scene) is registered for the host's whole life.
        explicit InspectorHost(InspectorSource& fallback);

        void AddSource(InspectorSource& source);
        // Mark pins on it closed, invalidate, erase. No-op for the fallback.
        void RemoveSource(InspectorSource& source);
        // The source's KEYS just died (the scene's registry was replaced by
        // New/Open Scene or a project switch) but the source lives on: prune
        // every history entry naming it, release every pin naming it
        // ("Pinned selection is gone", not "<name> closed"), keep it
        // registered and keep Current() as it is. Works on the fallback --
        // that is its main caller.
        void InvalidateSource(InspectorSource& source);
        // A selection EVENT in `source` (never focus). Ignored for a source
        // that was never added. Makes it current; pushes {source, key, label}
        // unless the key is empty or equals the cursor entry (a navigation echo).
        void NotifySelected(InspectorSource& source);
        [[nodiscard]] InspectorSource& Current() const noexcept { return *m_current; }
        // What instance `id` shows: its pinned source (null while dead/closed) or Current().
        [[nodiscard]] InspectorSource* SourceFor(int instanceId) const;

        [[nodiscard]] const std::vector<Instance>& Instances() const noexcept { return m_instances; }
        [[nodiscard]] Instance* Find(int id);
        int  AddInstance();                              // lowest free id in [1, kMaxInstances); -1 when the pool is full
        void RemoveInstance(int id);                     // id 0 is refused
        void SetInstanceIds(std::span<const int> extras); // ini restore: exactly {0} + the valid, deduplicated ids exist afterwards
        // True when Current() has a page for its current key: the pin is only
        // offered for a resolvable page (UE's details lock exists only while
        // objects are viewed). Non-const: PageFor is.
        [[nodiscard]] bool CanPin();
        void SetPinned(int id, bool pinned);   // pin captures Current() + key + name; REFUSED (no-op) when !CanPin()
        // In-window navigation of a PINNED instance (a breadcrumb click): only
        // that instance's pinnedKey changes -- never Current(), the source's
        // selection or the history. No-op for an unknown, unpinned or closed instance.
        void RepinKey(int id, std::string key);

        [[nodiscard]] bool CanGoBack() const noexcept { return !m_history.empty() && m_cursor > 0; }
        [[nodiscard]] bool CanGoForward() const noexcept { return !m_history.empty() && m_cursor + 1 < m_history.size(); }
        bool GoBack();
        bool GoForward();
        // Land on ONE chosen entry: no walking. On failure that single entry is pruned.
        bool JumpTo(std::size_t index);
        [[nodiscard]] const HistoryEntry* BackEntry() const noexcept { return CanGoBack() ? &m_history[m_cursor - 1] : nullptr; }
        [[nodiscard]] const HistoryEntry* ForwardEntry() const noexcept { return CanGoForward() ? &m_history[m_cursor + 1] : nullptr; }
        [[nodiscard]] const std::vector<HistoryEntry>& History() const noexcept { return m_history; }
        [[nodiscard]] std::size_t HistoryCursor() const noexcept { return m_cursor; }
        // Drop every entry whose source no longer resolves its key; the cursor
        // keeps pointing at the same surviving entry (index arithmetic, never a
        // key search). Once per frame from the draw: <= kHistoryDepth pure lookups.
        void PruneStale();

        // Project switch: every non-fallback source is dropped, the history
        // cleared, every instance unpinned. Instances themselves stay (they
        // are layout, per-ini, not project state).
        void ReleaseAll();

    private:
        [[nodiscard]] bool Registered(const InspectorSource* s) const;
        void Push(InspectorSource& source, std::string key);
        void RefreshCursorLabel();
        void EraseHistoryIf(const std::function<bool(const HistoryEntry&)>& pred);

        InspectorSource* m_fallback;
        std::vector<InspectorSource*> m_sources;
        InspectorSource* m_current;
        std::vector<HistoryEntry> m_history;
        std::size_t m_cursor = 0;
        std::vector<Instance> m_instances;
    };
}
```

- [ ] **Step 4b: SelectionContext epoch** (`ArcaneEditor/src/Scene/SelectionContext.hpp`)

The scene's selection gets the same epoch a document already carries, so the app's `SelectionEdge` (Task 4) can tell a GESTURE from a per-frame `Prune` that re-primaried a multi-selection. Add `#include <cstdint>` beside the existing includes, the member and accessor, and `++m_epoch;` as the LAST statement of `Select`, `Toggle`, `AddRange` and `Clear`. `Prune` is left untouched (its comment gains one sentence):

```cpp
#include <Astra/Entity/Entity.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace Arcane::Editor
{
    struct SelectionContext
    {
        using EntityT = Astra::Entity;

        [[nodiscard]] bool HasSelection() const noexcept { return !m_entities.empty(); }
        [[nodiscard]] std::size_t Count() const noexcept { return m_entities.size(); }
        [[nodiscard]] Astra::Entity Primary() const noexcept { return m_primary; }
        [[nodiscard]] const std::vector<Astra::Entity>& Entities() const noexcept { return m_entities; }
        // Bumped by every user selection ACTION (Select, Toggle, AddRange,
        // Clear), including a re-select of the already-selected entity; NOT
        // by Prune(), which is a sweep after a registry swap and must not
        // read as the user re-selecting the scene (it would steal the
        // Inspector from a document during undo/redo).
        [[nodiscard]] std::uint64_t Epoch() const noexcept { return m_epoch; }
        [[nodiscard]] bool Contains(Astra::Entity e) const noexcept
        {
            return std::find(m_entities.begin(), m_entities.end(), e) != m_entities.end();
        }

        // Plain click: selection becomes exactly { e }.
        void Select(Astra::Entity e)
        {
            m_entities.assign(1, e);
            m_primary = e;
            ++m_epoch;
        }

        // Ctrl-click: add (becomes primary) or remove (primary falls back to
        // the most recently selected remaining entry).
        void Toggle(Astra::Entity e)
        {
            auto it = std::find(m_entities.begin(), m_entities.end(), e);
            if (it == m_entities.end())
            {
                m_entities.push_back(e);
                m_primary = e;
            }
            else
            {
                m_entities.erase(it);
                m_primary = m_entities.empty() ? Astra::Entity::Invalid()
                                               : m_entities.back();
            }
            ++m_epoch;
        }

        // Shift-range: append `range` in visible-row order, skipping entries
        // already selected; `primary` becomes the primary (the clicked row).
        void AddRange(std::span<const Astra::Entity> range, Astra::Entity primary)
        {
            for (Astra::Entity e : range)
                if (!Contains(e))
                    m_entities.push_back(e);
            if (primary.IsValid())
                m_primary = primary;
            ++m_epoch;
        }

        void Clear() noexcept
        {
            m_entities.clear();
            m_primary = Astra::Entity::Invalid();
            ++m_epoch;
        }

        // Sweep entries the registry no longer recognizes (after a structural
        // undo/redo swapped the registry object). Primary falls back like
        // Toggle-removal. `alive` is injected so this header stays free of
        // registry includes: sel.Prune([&](Astra::Entity e){ return reg.IsValid(e); });
        // Does NOT bump Epoch(): a sweep is not a selection gesture
        // (inspector-ownership plan T2).
        template<typename IsAliveFn>
        void Prune(IsAliveFn&& alive)
        {
            std::erase_if(m_entities,
                          [&](Astra::Entity e) { return !alive(e); });
            if (!m_entities.empty() && !Contains(m_primary))
                m_primary = m_entities.back();
            else if (m_entities.empty())
                m_primary = Astra::Entity::Invalid();
        }

    private:
        std::vector<Astra::Entity> m_entities;
        Astra::Entity m_primary = Astra::Entity::Invalid();
        std::uint64_t m_epoch = 0;
    };
}
```

(The file's header comment and everything not shown stay as they are.)

- [ ] **Step 5: Write InspectorHost.cpp**

```cpp
#include "Panels/InspectorHost.hpp"

#include <algorithm>

namespace Arcane::Editor
{
    InspectorHost::InspectorHost(InspectorSource& fallback)
        : m_fallback(&fallback), m_sources{ &fallback }, m_current(&fallback)
    {
        m_instances.push_back(Instance{});   // id 0
    }

    bool InspectorHost::Registered(const InspectorSource* s) const
    {
        return std::find(m_sources.begin(), m_sources.end(), s) != m_sources.end();
    }

    void InspectorHost::AddSource(InspectorSource& source)
    {
        if (!Registered(&source)) m_sources.push_back(&source);
    }

    void InspectorHost::EraseHistoryIf(const std::function<bool(const HistoryEntry&)>& pred)
    {
        // UE FHistoryManager::RemoveHistoryData: for every removed index at or
        // before the cursor the cursor steps back one, so it keeps pointing at
        // the same surviving entry (or the previous one when its own went).
        for (std::size_t i = 0; i < m_history.size();)
        {
            if (!pred(m_history[i])) { ++i; continue; }
            m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(i));
            if (m_cursor >= i && m_cursor > 0) --m_cursor;
        }
        if (m_history.empty()) m_cursor = 0;
        else m_cursor = std::min(m_cursor, m_history.size() - 1);
    }

    void InspectorHost::InvalidateSource(InspectorSource& source)
    {
        EraseHistoryIf([&](const HistoryEntry& e) { return e.source == &source; });
        for (Instance& inst : m_instances)
            if (inst.pinned && inst.pinnedSource == &source)
                inst.pinnedSource = nullptr;      // sourceClosed untouched: "Pinned selection is gone"
    }

    void InspectorHost::RemoveSource(InspectorSource& source)
    {
        if (&source == m_fallback) return;
        for (Instance& inst : m_instances)         // mark BEFORE Invalidate nulls the pointer
            if (inst.pinned && inst.pinnedSource == &source)
                inst.sourceClosed = true;
        InvalidateSource(source);
        std::erase(m_sources, &source);
        if (m_current == &source) m_current = m_fallback;
    }

    void InspectorHost::PruneStale()
    {
        EraseHistoryIf([](const HistoryEntry& e) { return !e.source->Resolves(e.key); });
    }

    void InspectorHost::Push(InspectorSource& source, std::string key)
    {
        if (!m_history.empty())
        {
            const HistoryEntry& at = m_history[m_cursor];
            // Navigation echo: the cursor entry IS (source, key) -- it is re-
            // snapshotted on every restore, so the next-frame re-report matches.
            if (at.source == &source && at.key == key) return;
            m_history.resize(m_cursor + 1);                        // drop forward entries
        }
        m_history.push_back(HistoryEntry{ &source, std::move(key), InspectorCrumbText(source, source.Page()) });
        if (m_history.size() > kHistoryDepth)
            m_history.erase(m_history.begin(), m_history.begin() + static_cast<std::ptrdiff_t>(m_history.size() - kHistoryDepth));
        m_cursor = m_history.size() - 1;
    }

    void InspectorHost::NotifySelected(InspectorSource& source)
    {
        if (!Registered(&source)) return;
        std::string key = source.SelectionKey();
        if (key.empty()) return;   // a deselect is not a selection event
        m_current = &source;
        Push(source, std::move(key));
    }

    InspectorSource* InspectorHost::SourceFor(int instanceId) const
    {
        for (const Instance& inst : m_instances)
            if (inst.id == instanceId)
                return inst.pinned ? inst.pinnedSource : m_current;
        return nullptr;
    }

    InspectorHost::Instance* InspectorHost::Find(int id)
    {
        for (Instance& inst : m_instances)
            if (inst.id == id) return &inst;
        return nullptr;
    }

    int InspectorHost::AddInstance()
    {
        for (int id = 1; id < kMaxInstances; ++id)
            if (!Find(id)) { Instance inst; inst.id = id; m_instances.push_back(inst); return id; }
        return -1;
    }

    void InspectorHost::RemoveInstance(int id)
    {
        if (id == 0) return;
        std::erase_if(m_instances, [id](const Instance& i) { return i.id == id; });
    }

    void InspectorHost::SetInstanceIds(std::span<const int> extras)
    {
        std::erase_if(m_instances, [](const Instance& i) { return i.id != 0; });
        for (const int id : extras)
            if (id >= 1 && id < kMaxInstances && !Find(id)) { Instance inst; inst.id = id; m_instances.push_back(inst); }
        std::sort(m_instances.begin(), m_instances.end(), [](const Instance& a, const Instance& b) { return a.id < b.id; });
    }

    bool InspectorHost::CanPin()
    {
        return m_current->PageFor(m_current->SelectionKey()) != nullptr;
    }

    void InspectorHost::SetPinned(int id, bool pinned)
    {
        Instance* inst = Find(id);
        if (!inst) return;
        if (pinned && !CanPin()) return;   // nothing to hold: a pin never holds emptiness
        inst->pinned = pinned;
        inst->sourceClosed = false;
        inst->pinnedSource = pinned ? m_current : nullptr;
        inst->pinnedKey = pinned ? m_current->SelectionKey() : std::string{};
        inst->pinnedName = pinned ? m_current->SourceName() : std::string{};
    }

    void InspectorHost::RepinKey(int id, std::string key)
    {
        Instance* inst = Find(id);
        if (!inst || !inst->pinned || inst->sourceClosed) return;
        inst->pinnedKey = std::move(key);
    }

    void InspectorHost::RefreshCursorLabel()
    {
        // UE refreshes the entry it is LEAVING before navigating, so a rename
        // made after the selection does not leave a stale label behind.
        if (m_history.empty()) return;
        HistoryEntry& at = m_history[m_cursor];
        if (at.source == m_current) at.label = InspectorCrumbText(*at.source, at.source->Page());
    }

    bool InspectorHost::GoBack()
    {
        RefreshCursorLabel();
        while (CanGoBack())
        {
            const std::size_t i = m_cursor - 1;
            HistoryEntry e = m_history[i];
            if (e.source->RestoreSelection(e.key))
            {
                m_cursor = i;
                m_current = e.source;
                m_history[i].key = e.source->SelectionKey();   // re-snapshot from live state: members may be gone, the source may normalize
                return true;
            }
            m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(i));   // stale: prune (same-frame safety net under PruneStale)
            --m_cursor;
        }
        return false;
    }

    bool InspectorHost::GoForward()
    {
        RefreshCursorLabel();
        while (CanGoForward())
        {
            const std::size_t i = m_cursor + 1;
            HistoryEntry e = m_history[i];
            if (e.source->RestoreSelection(e.key))
            {
                m_cursor = i;
                m_current = e.source;
                m_history[i].key = e.source->SelectionKey();
                return true;
            }
            m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return false;
    }

    bool InspectorHost::JumpTo(std::size_t index)
    {
        if (index >= m_history.size() || index == m_cursor) return false;
        RefreshCursorLabel();
        HistoryEntry e = m_history[index];
        if (e.source->RestoreSelection(e.key))
        {
            m_cursor = index;
            m_current = e.source;
            m_history[index].key = e.source->SelectionKey();
            return true;
        }
        m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(index));
        if (index < m_cursor) --m_cursor;
        return false;
    }

    void InspectorHost::ReleaseAll()
    {
        m_sources.assign(1, m_fallback);
        m_current = m_fallback;
        m_history.clear();
        m_cursor = 0;
        for (Instance& inst : m_instances)
        {
            inst.pinned = false;
            inst.pinnedSource = nullptr;
            inst.pinnedKey.clear();
            inst.pinnedName.clear();
            inst.sourceClosed = false;
        }
    }
}
```

Do NOT auto-release a live source's dead key: `RegistryStateCommand.hpp:4-8` -- binary registry restore resurrects EXACT entity ids, so delete->undo and Play->Stop bring the pinned page back by themselves (the draw re-evaluates `PageFor(pinnedKey)` every frame; only a click or `RemoveSource` releases).

Then list the new .cpp in the test exe: `ArcaneTests` links editor sources one by one (`premake5.lua`, the `files {` block of `project "ArcaneTests"`). Insert after `"%{wks.location}/ArcaneEditor/src/Panels/InspectorMeta.cpp",` (:1224), in the neighbours' comment style:

```lua
        -- Inspector ownership T2: InspectorHost (pure routing, no ImGui) source-
        -- compiles into the test exe so EditorInspectorHostTest drives it with
        -- fake sources. SceneInspectorSource.cpp / InspectorWindows.cpp stay OUT
        -- (they need EditorPanels.cpp).
        "%{wks.location}/ArcaneEditor/src/Panels/InspectorHost.cpp",
```

- [ ] **Step 6: EditorDocument becomes a source with defaults** (`Documents/EditorDocument.hpp`)

Add `#include "Panels/InspectorSource.hpp"` and `<cstdint>`, `<string_view>`; change the class line to `class EditorDocument : public InspectorSource` and add, after `Draw`:

```cpp
        // ---- Inspector source (inspector-ownership spec s3.2) -----------
        // A document that can SELECT opts in by overriding Page()/PageFor()/
        // SelectionKey()/RestoreSelection()/Resolves() and bumping
        // SelectionEpoch() on every selection change (the host's per-frame
        // poll turns the bump into InspectorHost::NotifySelected). The
        // defaults are "this document contributes no page and never drives
        // the Inspector" (MeshDocument today).
        std::string SourceName() const override { return Title(); }
        InspectorPage* Page() override { return nullptr; }
        InspectorPage* PageFor(std::string_view) override { return nullptr; }
        std::string SelectionKey() const override { return {}; }
        bool RestoreSelection(std::string_view) override { return false; }
        bool Resolves(std::string_view) const override { return false; }
        // Monotonic; changes whenever the document's own selection changes.
        [[nodiscard]] virtual std::uint64_t SelectionEpoch() const { return 0; }
        // --select-in-document: select by a human path ("Player/Jump"). False
        // = the document has no such notion or the path did not resolve.
        virtual bool SelectByPath(std::string_view) { return false; }
```

- [ ] **Step 7: Build and run** -- premake, MSBuild, `./ArcaneTests.exe "[editor][inspector]"` then `./ArcaneTests.exe "[editor]"`. Expected: all cases pass (the SelectionContext epoch case in `[editor][selection]` included); `[editor]` green (FakeDoc in EditorDocumentHostTest compiles unchanged because every added member has a default).

- [ ] **Step 8: Commit**

```bash
git add premake5.lua ArcaneEditor/src/Panels/InspectorSource.hpp ArcaneEditor/src/Panels/InspectorHost.hpp ArcaneEditor/src/Panels/InspectorHost.cpp ArcaneEditor/src/Documents/EditorDocument.hpp ArcaneEditor/src/Scene/SelectionContext.hpp ArcaneTests/src/EditorInspectorHostTest.cpp ArcaneTests/src/SelectionOpsTest.cpp
git commit -m "feat(editor): InspectorHost -- selection sources, keyed pages, pin, instances, history 32 with prune-on-invalidate (pure Resolves, PruneStale, InvalidateSource), instance pool of 8, labelled entries + JumpTo, RepinKey; SelectionContext epoch; EditorDocument is a source with opt-out defaults (T2)"
```

---
### Task 3: DocumentHost observer + the scene as a source

**Files:**
- Modify: `ArcaneEditor/src/Documents/DocumentHost.hpp`, `ArcaneEditor/src/Documents/DocumentHost.cpp` (Add/Close/CloseAll)
- Create: `ArcaneEditor/src/Panels/SceneInspectorSource.hpp`, `ArcaneEditor/src/Panels/SceneInspectorSource.cpp`
- Create: `ArcaneEditor/src/Panels/SceneSelectionKey.hpp` (header-only: the scene's whole-set selection key encode/decode/alive-filter, testable without EditorPanels.cpp)
- Test: `ArcaneTests/src/EditorDocumentHostTest.cpp` (one new case)
- Test: `ArcaneTests/src/SceneSelectionKeyTest.cpp` (new file, one case; ArcaneTests globs `ArcaneTests/src/**.cpp`, so no premake entry -- and `SceneInspectorSource.cpp` is NEVER listed in the ArcaneTests `files` block: it needs `EditorPanels.cpp`, which the test exe deliberately excludes)

**Interfaces:**
- Consumes: `InspectorSource`/`InspectorPage` (Task 2), `DrawInspectorBody` + `InspectorState` (Task 1), `SelectionContext`, `Arcane::Edit::DisplayName(registry, entity)` (`Arcane/Edit/EntityOps.hpp`, already used by EditorPanels.cpp).
- Produces:
  ```cpp
  struct DocumentObserver { std::function<void(EditorDocument&)> opened; std::function<void(EditorDocument&)> closing; };
  void DocumentHost::SetObserver(DocumentObserver observer);   // opened fires from Add(); closing fires from Close() and once per document in CloseAll(), BEFORE destruction
  class SceneInspectorSource final : public InspectorSource, public InspectorPage {
      struct Deps { Astra::Registry* registry; SelectionContext* selection; Arcane::CommandStack* undo; const SceneEditBinding* binding; const Arcane::Project* project; InspectorState* state; const InspectorServices* services; const Arcane::Guid* selectedAsset; };
      void Bind(const Deps& deps);   // every frame, before the host draws
      // InspectorSource: SourceName()=="Scene"; SelectionKey()= the whole selection SET "<primary>;<e1>,<e2>,..." (decimal entity values, Entities() order) or ""; RestoreSelection(key) re-selects the ALIVE subset (false only when none survive); Resolves(key) = some member alive; Page()/PageFor(key) return this, PageFor pinning the alive subset of the keyed set; crumbs carry keys for a pinned instance (scene root: none; entity crumb: the drawn set's key)
      // InspectorPage: Breadcrumb()= {"Scene", <primary name>}; Draw()= DrawInspectorBody over the live or the pinned SelectionContext
  };
  ```

- [ ] **Step 1: Write the failing test** (append to `EditorDocumentHostTest.cpp`)

```cpp
TEST_CASE("DocumentHost tells its observer about every open and every close, before destruction", "[editor]")
{
    DocumentHost host;
    std::vector<std::string> events;
    host.SetObserver({
        [&](EditorDocument& d) { events.push_back("open:" + d.Title()); },
        [&](EditorDocument& d) { events.push_back("close:" + d.Title()); } });
    auto* a = static_cast<FakeDoc*>(host.Add(std::make_unique<FakeDoc>("a", Arcane::Guid::Generate(), false)));
    host.Add(std::make_unique<FakeDoc>("b", Arcane::Guid::Generate(), false));
    host.RequestClose(a);                            // clean: closes now
    host.CloseAll();                                 // b
    REQUIRE(events.size() == 4);
    CHECK(events[0] == "open:a");
    CHECK(events[1] == "open:b");
    CHECK(events[2] == "close:a");
    CHECK(events[3] == "close:b");
}
```

And a second, new test file `ArcaneTests/src/SceneSelectionKeyTest.cpp` (`[editor][inspector]`; picked up by the `ArcaneTests/src/**.cpp` glob, no premake entry) for the pure key helpers Step 3b writes:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "Panels/SceneSelectionKey.hpp"
#include <set>

using namespace Arcane::Editor;
namespace K = SceneSelectionKey;

TEST_CASE("SceneSelectionKey: the key is the whole set; the alive subset keeps the live members and drops the dead ones", "[editor][inspector]")
{
    using Astra::Entity;
    const Entity a(static_cast<Entity::StorageType>(7)), b(static_cast<Entity::StorageType>(9)), c(static_cast<Entity::StorageType>(11));
    SelectionContext sel;
    sel.Select(a); sel.Toggle(b); sel.Toggle(c);        // primary = c (last toggled)
    const std::string key = K::Encode(sel);
    CHECK(key == "11;7,9,11");
    K::KeySet set = K::Decode(key);
    REQUIRE(set.members.size() == 3);
    CHECK(set.primary == c);
    // Round trip through Clear + AddRange re-reports the identical key.
    SelectionContext again; again.Clear(); again.AddRange(set.members, set.primary);
    CHECK(K::Encode(again) == key);
    // The PRIMARY dies: the survivors stay, the last survivor becomes primary.
    std::set<Entity> dead{ c };
    K::KeySet alive = K::AliveSubset(set, [&](Entity e) { return !dead.count(e); });
    REQUIRE(alive.members.size() == 2);
    CHECK(alive.primary == b);
    dead = { a, b, c };
    CHECK(K::AliveSubset(set, [&](Entity e) { return !dead.count(e); }).members.empty());
    // Malformed keys decode to nothing.
    CHECK(K::Decode("").members.empty());
    CHECK(K::Decode("7").members.empty());
    CHECK(K::Decode("bogus;1,2").members.empty());
    CHECK(K::Decode("5;1,2").members.empty());          // primary not in the list
    CHECK(K::Decode("1;1,x").members.empty());
    CHECK(K::Encode(SelectionContext{}).empty());
}
```

- [ ] **Step 2: Run to verify it fails** -- build. Expected: `SetObserver` undeclared; `Panels/SceneSelectionKey.hpp` not found.

- [ ] **Step 3: Implement the observer**

`DocumentHost.hpp`: add after the `PeekGuid` alias:

```cpp
        // Lifecycle observer (inspector-ownership arc): the app registers each
        // opened document as an Inspector source and releases it on close.
        // `closing` fires BEFORE the document is destroyed (Close and CloseAll
        // both), so the observer may still read its identity.
        struct DocumentObserver
        {
            std::function<void(EditorDocument&)> opened;
            std::function<void(EditorDocument&)> closing;
        };
        void SetObserver(DocumentObserver observer) { m_observer = std::move(observer); }
```

and a private `DocumentObserver m_observer;`. `DocumentHost.cpp`: in `Add`, before `return`: `if (m_observer.opened) m_observer.opened(*m_docs.back());`. In `Close`, before the erase: `if (m_observer.closing) m_observer.closing(*doc);`. In `CloseAll`, before `m_docs.clear()`: `if (m_observer.closing) for (const auto& d : m_docs) m_observer.closing(*d);`.

- [ ] **Step 3b: Write SceneSelectionKey.hpp** (header-only, no ImGui, no EditorPanels)

```cpp
#pragma once

// The scene's Inspector selection key (inspector-ownership T3): the WHOLE
// ordered selection set, primary first -- "<primary>;<e1>,<e2>,...,<eN>" --
// so a pin or a history entry names what the user saw (the body fans out
// over every member) and a member's death does not kill the page while
// others live (UE's locked Details view purges dead members and empties
// only when none remain). Pure and header-only so ArcaneTests can drive it
// without SceneInspectorSource.cpp (which needs EditorPanels.cpp).

#include "Scene/SelectionContext.hpp"
#include <Astra/Entity/Entity.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor::SceneSelectionKey
{
    struct KeySet
    {
        Astra::Entity primary = Astra::Entity::Invalid();
        std::vector<Astra::Entity> members;
    };

    [[nodiscard]] inline std::string Encode(const SelectionContext& sel)
    {
        if (!sel.HasSelection()) return {};
        std::string out = std::to_string(static_cast<std::uint64_t>(sel.Primary().GetValue())) + ";";
        bool first = true;
        for (const Astra::Entity e : sel.Entities())
        {
            if (!first) out += ',';
            first = false;
            out += std::to_string(static_cast<std::uint64_t>(e.GetValue()));
        }
        return out;
    }

    [[nodiscard]] inline Astra::Entity ParseOne(std::string_view tok)
    {
        std::uint64_t value = 0;
        const auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), value);
        if (ec != std::errc{} || p != tok.data() + tok.size()) return Astra::Entity::Invalid();
        return Astra::Entity(static_cast<Astra::Entity::StorageType>(value));
    }

    // Empty members on any malformed segment, or a primary absent from the list.
    [[nodiscard]] inline KeySet Decode(std::string_view key)
    {
        KeySet set;
        const std::size_t semi = key.find(';');
        if (semi == std::string_view::npos) return {};
        const Astra::Entity primary = ParseOne(key.substr(0, semi));
        if (!primary.IsValid()) return {};
        std::string_view rest = key.substr(semi + 1);
        while (!rest.empty())
        {
            const std::size_t comma = rest.find(',');
            const Astra::Entity e = ParseOne(rest.substr(0, comma));
            if (!e.IsValid()) return {};
            set.members.push_back(e);
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
        if (std::find(set.members.begin(), set.members.end(), primary) == set.members.end()) return {};
        set.primary = primary;
        return set;
    }

    // Members filtered by `alive`; primary = the keyed primary when alive, else
    // the LAST alive member (SelectionContext::Prune's fallback), Invalid when none.
    template <typename IsAliveFn>
    [[nodiscard]] KeySet AliveSubset(const KeySet& set, IsAliveFn&& alive)
    {
        KeySet out;
        for (const Astra::Entity e : set.members)
            if (alive(e)) out.members.push_back(e);
        if (out.members.empty()) return out;
        out.primary = alive(set.primary) ? set.primary : out.members.back();
        return out;
    }
}
```

`SelectionContext::AddRange` takes `std::span<const Astra::Entity>` (`SelectionContext.hpp`), so a `std::vector<Astra::Entity>` converts in place; `Astra::Entity` has a defaulted `operator==` and `operator<` (`Entity.hpp:67,71`), which `std::find` and the test's `std::set<Entity>` rely on.

- [ ] **Step 4: Write SceneInspectorSource.hpp**

```cpp
#pragma once

// The scene as an Inspector source (inspector-ownership spec s3.1, decision
// 1): the Outliner and the Viewport both write SelectionContext, so they are
// ONE source. Its page is the scene Inspector body (DrawInspectorBody) --
// unchanged pixels, now reached through the host like every other page.
// Owned by EditorApp; rebound every frame because the registry, undo stack
// and project it draws through can all be replaced by a project switch.

#include "Panels/EditorPanels.hpp"     // InspectorState, InspectorServices, SceneEditBinding
#include "Panels/InspectorSource.hpp"
#include "Scene/SelectionContext.hpp"

namespace Arcane { class CommandStack; class Project; }
namespace Astra { class Registry; }

namespace Arcane::Editor
{
    class SceneInspectorSource final : public InspectorSource, public InspectorPage
    {
    public:
        struct Deps
        {
            Astra::Registry*          registry = nullptr;
            SelectionContext*         selection = nullptr;
            Arcane::CommandStack*     undo = nullptr;
            const SceneEditBinding*   binding = nullptr;
            const Arcane::Project*    project = nullptr;
            InspectorState*           state = nullptr;
            const InspectorServices*  services = nullptr;
            const Arcane::Guid*       selectedAsset = nullptr;
        };
        void Bind(const Deps& deps) { m_deps = deps; }

        // InspectorSource
        std::string SourceName() const override { return "Scene"; }
        InspectorPage* Page() override;
        InspectorPage* PageFor(std::string_view key) override;
        std::string SelectionKey() const override;
        bool RestoreSelection(std::string_view key) override;
        bool Resolves(std::string_view key) const override;

        // InspectorPage
        std::vector<InspectorCrumb> Breadcrumb() const override;
        void Draw(PropertyGrid& grid) override;

    private:
        [[nodiscard]] bool Alive(Astra::Entity e) const;

        Deps m_deps{};
        SelectionContext m_pinnedSel;                 // PageFor's pinned selection: the alive subset of the pinned key, rebuilt every call
        const SelectionContext* m_drawSel = nullptr;  // what Draw/Breadcrumb read this frame
    };
}
```

- [ ] **Step 5: Write SceneInspectorSource.cpp**

```cpp
#include "Panels/SceneInspectorSource.hpp"
#include "Panels/SceneSelectionKey.hpp"

#include <Arcane/Edit/EntityOps.hpp>   // Arcane::Edit::DisplayName
#include <Astra/Registry/Registry.hpp>

namespace Arcane::Editor
{
    bool SceneInspectorSource::Alive(Astra::Entity e) const
    {
        return m_deps.registry && e.IsValid() && m_deps.registry->IsValid(e);
    }

    InspectorPage* SceneInspectorSource::Page()
    {
        m_drawSel = m_deps.selection;
        return m_deps.selection ? this : nullptr;
    }

    InspectorPage* SceneInspectorSource::PageFor(std::string_view key)
    {
        using namespace SceneSelectionKey;
        const KeySet alive = AliveSubset(Decode(key), [&](Astra::Entity e) { return Alive(e); });
        if (alive.members.empty()) return nullptr;
        m_pinnedSel.Clear();
        m_pinnedSel.AddRange(alive.members, alive.primary);   // Clear+AddRange keeps order and the named primary; Select would collapse the set
        m_drawSel = &m_pinnedSel;
        return this;
    }

    std::string SceneInspectorSource::SelectionKey() const
    {
        return m_deps.selection ? SceneSelectionKey::Encode(*m_deps.selection) : std::string{};
    }

    bool SceneInspectorSource::Resolves(std::string_view key) const
    {
        using namespace SceneSelectionKey;
        return !AliveSubset(Decode(key), [&](Astra::Entity e) { return Alive(e); }).members.empty();
    }

    bool SceneInspectorSource::RestoreSelection(std::string_view key)
    {
        using namespace SceneSelectionKey;
        if (!m_deps.selection) return false;
        const KeySet alive = AliveSubset(Decode(key), [&](Astra::Entity e) { return Alive(e); });
        if (alive.members.empty()) return false;
        m_deps.selection->Clear();
        m_deps.selection->AddRange(alive.members, alive.primary);
        return true;
    }

    std::vector<InspectorCrumb> SceneInspectorSource::Breadcrumb() const
    {
        std::vector<InspectorCrumb> crumbs;
        crumbs.push_back({ "Scene", [sel = m_deps.selection] { if (sel) sel->Clear(); }, std::nullopt });   // no scene-level page: inert when pinned
        if (m_drawSel && m_drawSel->HasSelection() && m_deps.registry)
        {
            const Astra::Entity primary = m_drawSel->Primary();
            std::string name = Arcane::Edit::DisplayName(*m_deps.registry, primary);
            if (m_drawSel->Count() > 1) name += " (+" + std::to_string(m_drawSel->Count() - 1) + ")";
            crumbs.push_back({ std::move(name),
                               [sel = m_deps.selection, primary] { if (sel) sel->Select(primary); },
                               SceneSelectionKey::Encode(*m_drawSel) });
        }
        return crumbs;
    }

    void SceneInspectorSource::Draw(PropertyGrid&)
    {
        // The scene body draws through InspectorState::grid (Task 1), which
        // IS a PropertyGridState -- the parameter is the page contract's, and
        // the scene page keeps its own persistent state on purpose (gesture
        // token, quat views, search) rather than the instance's.
        if (!m_deps.registry || !m_drawSel || !m_deps.undo || !m_deps.binding || !m_deps.state) return;
        const Arcane::Guid nil;
        DrawInspectorBody(*m_deps.registry, *m_drawSel, *m_deps.undo, *m_deps.binding,
                          m_deps.project, *m_deps.state, m_deps.services,
                          m_deps.selectedAsset ? *m_deps.selectedAsset : nil);
    }
}
```

The key is opaque to InspectorHost; any change to the SET (add, Ctrl-remove, Prune) changes the key, and the epoch decides whether it was a gesture (Task 2 Step 4b: `SelectionContext::Epoch()` + `SelectionEdge::Observe(epoch, key)`). The pinned key is a snapshot: a member deleted then restored by undo reappears in the pinned page on the next PageFor. `SelectionContext::Toggle` must NOT be used to rebuild a set -- it re-assigns the primary. The crumb `select` lambdas call `Clear()`/`Select()` on the LIVE selection, which bump its epoch: a crumb click in an unpinned instance is a gesture on purpose (a pinned instance never reaches them -- Task 4 routes its crumb clicks to `InspectorHost::RepinKey` with the crumb's `key`).

Check `Astra::Entity::StorageType` exists (`ThirdParty/Astra/include/Astra/Entity/Entity.hpp:50`: `using StorageType = typename Traits::StorageType`; `explicit BasicEntity(StorageType)` at :58). Check the include for `DisplayName` by grepping `DisplayName` in `EditorPanels.cpp`'s include list and use the same header.

- [ ] **Step 6: Build and run** -- premake, MSBuild, `./ArcaneTests.exe "[editor]"`. Expected: green, the observer case and the SceneSelectionKey case included.

- [ ] **Step 7: Commit**

```bash
git add ArcaneEditor/src/Documents/DocumentHost.hpp ArcaneEditor/src/Documents/DocumentHost.cpp ArcaneEditor/src/Panels/SceneInspectorSource.hpp ArcaneEditor/src/Panels/SceneInspectorSource.cpp ArcaneEditor/src/Panels/SceneSelectionKey.hpp ArcaneTests/src/EditorDocumentHostTest.cpp ArcaneTests/src/SceneSelectionKeyTest.cpp
git commit -m "feat(editor): DocumentHost open/close observer; the scene is an Inspector source whose page is DrawInspectorBody and whose key is the whole selection set (inspector ownership plan T3)"
```

---

### Task 4: Inspector instance windows + app wiring (pin, back/forward, breadcrumb, New Inspector, ini, project switch)

**Files:**
- Create: `ArcaneEditor/src/Panels/InspectorWindows.hpp`, `ArcaneEditor/src/Panels/InspectorWindows.cpp`
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp` (MenuRequests), `ArcaneEditor/src/Panels/EditorPanels.cpp:327-331` (Window menu), `ArcaneEditor/src/App/EditorApp.hpp` (members), `ArcaneEditor/src/App/EditorApp.cpp` (observer registration near :826; ini handler near :277-318), `ArcaneEditor/src/App/EditorAppFrame.cpp:3591-3600` (draw), `:2640-2641` (the raw-scancode Ctrl+S gate), `:2445+` (ConsumeMenuRequests), `ArcaneEditor/src/App/EditorAppProject.cpp:2164` (project switch), `ArcaneEditor/src/App/EditorAppScene.cpp:150` (ClearSceneReferences: the scene half of the release)
- Golden: re-bless `editor-ui` and `editor-ui-perspective` (the header row is new pixels -- a recorded deviation from spec A s4, see 99-tail).

**Interfaces:**
- Consumes: `InspectorHost` (`PruneStale`, `CanPin`, `RepinKey`, `JumpTo`, `BackEntry`/`ForwardEntry`, `History`/`HistoryCursor`, `SetInstanceIds`, `InvalidateSource`, `kMaxInstances`), `InspectorCrumb::key`, `SceneInspectorSource`, `PropertyGrid` (`CommitOrphans`), `SelectionContext::Epoch()` (Task 2 Step 4b), `PanelVisibility::OpenFlag`, lucide `ICON_LC_CHEVRON_LEFT/RIGHT`, `ICON_LC_PIN`, `ICON_LC_PIN_OFF`.
- Produces:
  ```cpp
  struct InspectorWindowsState { std::unordered_map<int, PropertyGridState> grids; };   // per instance id (pool slots)
  struct InspectorWindowsResult
  {
      std::vector<int> closed;                        // extra instances whose X was clicked this frame
      std::vector<InspectorSource*> saveRequested;    // Ctrl+S in a focused instance: the source whose page it showed
      InspectorSource* focusedSource = nullptr;       // the source shown by the focused instance (null = none focused)
  };
  InspectorWindowsResult DrawInspectorWindows(InspectorHost& host, InspectorWindowsState& state, bool* primaryOpen);
  bool MenuRequests::newInspector;   // Window -> New Inspector (reopens a hidden primary before minting; -1 from AddInstance = no-op)
  // EditorApp: Arcane::Editor::InspectorSource* m_inspectorFocusedSource (latched per draw);
  //            EditorDocument* InspectorSaveTarget(InspectorSource*) const (dynamic_cast; null for the scene source / null)
  // Layout ini section [EditorInspector][Instances] with ONE line `Ids=<extra ids, comma-separated>` (empty = {0})
  ```

- [ ] **Step 1: Write InspectorWindows.hpp**

```cpp
#pragma once

// The Inspector windows (inspector-ownership spec s3.2/s3.3): one window per
// InspectorHost instance -- "Inspector" (id 0, the registry panel) and
// "Inspector N" for Window -> New Inspector. Each draws the header (back /
// forward, the clickable breadcrumb, the pin) and then the page body of the
// source the host routes to it, through that instance's own PropertyGrid
// state (so two instances showing the same page keep separate text drafts).
// Instance ids are pool slots (InspectorHost::kMaxInstances), so a closed
// instance's `[Window]` entry is the one its next opener inherits.

#include "Panels/InspectorHost.hpp"
#include "Widgets/PropertyGrid.hpp"

#include <unordered_map>
#include <vector>

namespace Arcane::Editor
{
    struct InspectorWindowsState
    {
        std::unordered_map<int, PropertyGridState> grids;   // per instance id (ids are pool slots, InspectorHost::kMaxInstances)
    };

    struct InspectorWindowsResult
    {
        std::vector<int> closed;   // ids whose X was clicked: the app removes them after the draw
        // Ctrl+S pressed while an instance window was focused: the source whose
        // page that instance showed (null when pinned-and-closed). The app maps
        // it to a document and saves it -- UE's asset-editor host resolves
        // Ctrl+S to the toolkit that OWNS the focused panel, never the panel.
        std::vector<InspectorSource*> saveRequested;
        // The source shown by the instance that held focus at this draw (null =
        // no instance focused). The app's raw-scancode scene Ctrl+S stands down
        // when this names a document, as it does for a focused document window.
        InspectorSource* focusedSource = nullptr;
    };

    // `primaryOpen` is PanelVisibility's flag for instance 0 (null = no X).
    // Instance 0 is skipped when the flag is false; extra instances always
    // draw (they carry their own X).
    InspectorWindowsResult DrawInspectorWindows(InspectorHost& host, InspectorWindowsState& state,
                                                bool* primaryOpen);
}
```

- [ ] **Step 2: Write InspectorWindows.cpp**

```cpp
#include "Panels/InspectorWindows.hpp"

#include "Widgets/EditorTheme.hpp"
#include "Widgets/IconsLucide.h"

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName (the primary's dock node for a new instance)

#include <algorithm>
#include <string>

namespace Arcane::Editor
{
    namespace
    {
        void DrawHeader(InspectorHost& host, const InspectorHost::Instance& inst, InspectorSource* src,
                        InspectorPage* page, bool canPin)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            const float pinWidth = ImGui::CalcTextSize(ICON_LC_PIN).x + style.FramePadding.x * 2.0f;

            // Back / forward over the selection history: each arrow names its
            // target, and a right-click lists that side's entries nearest-first
            // (UE's Content Browser history). The disabled state suppresses both.
            ImGui::BeginDisabled(!host.CanGoBack());
            if (ImGui::SmallButton(ICON_LC_CHEVRON_LEFT "##back")) (void)host.GoBack();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                if (const auto* e = host.BackEntry()) ImGui::SetTooltip("Back to %s", e->label.c_str());
            if (ImGui::BeginPopupContextItem("##back_history"))
            {
                const std::size_t cur = host.HistoryCursor();
                for (std::size_t i = cur; i-- > 0;)
                {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(host.History()[i].label.c_str())) { (void)host.JumpTo(i); ImGui::PopID(); break; }   // JumpTo may erase: stop iterating
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!host.CanGoForward());
            if (ImGui::SmallButton(ICON_LC_CHEVRON_RIGHT "##forward")) (void)host.GoForward();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                if (const auto* e = host.ForwardEntry()) ImGui::SetTooltip("Forward to %s", e->label.c_str());
            if (ImGui::BeginPopupContextItem("##forward_history"))
            {
                for (std::size_t i = host.HistoryCursor() + 1; i < host.History().size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(host.History()[i].label.c_str())) { (void)host.JumpTo(i); ImGui::PopID(); break; }
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();

            // Breadcrumb: a horizontal strip clipped to the width left of the pin
            // and scrolled to its END, so the LEAF stays visible and the head
            // scrolls off (UE's SBreadcrumbTrail); the pin can never overdraw the
            // leaf in a narrow docked Inspector. Every crumb is a link-styled
            // button; a dimmed chevron sits only BETWEEN crumbs.
            std::vector<InspectorCrumb> crumbs;
            if (page) crumbs = page->Breadcrumb();
            else if (src) crumbs.push_back({ src->SourceName(), {}, std::nullopt });
            const float crumbWidth = ImGui::GetContentRegionAvail().x - pinWidth - style.ItemSpacing.x;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::BeginChild("##crumbs", ImVec2(std::max(crumbWidth, 1.0f), ImGui::GetFrameHeight()), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::kNone);
            for (std::size_t i = 0; i < crumbs.size(); ++i)
            {
                if (i > 0) { ImGui::SameLine(); ImGui::TextDisabled(ICON_LC_CHEVRON_RIGHT); ImGui::SameLine(); }
                ImGui::PushID(static_cast<int>(i));
                if (inst.pinned)
                {
                    // A pinned instance navigates ITSELF: re-target the pin, never the source.
                    ImGui::BeginDisabled(!crumbs[i].key.has_value());
                    if (ImGui::SmallButton(crumbs[i].label.c_str()) && crumbs[i].key)
                        host.RepinKey(inst.id, *crumbs[i].key);
                    ImGui::EndDisabled();
                }
                else if (ImGui::SmallButton(crumbs[i].label.c_str()) && crumbs[i].select)
                    crumbs[i].select();
                ImGui::PopID();
            }
            ImGui::PopStyleColor();
            if (ImGui::GetScrollMaxX() > 0.0f) ImGui::SetScrollX(ImGui::GetScrollMaxX());   // last frame's width: one-frame lag, invisible at 90 frames + settle
            ImGui::EndChild();   // always, whatever BeginChild returned
            ImGui::PopStyleVar();
            ImGui::SameLine();   // the child's fixed width right-aligns the pin

            // The pin. Amber = pinned (the editor's acting-on hue). Offered only
            // for a resolvable page (UE's details lock exists only while objects
            // are viewed); the "closed"/"gone" notes stay for the page-less state.
            ImGui::BeginDisabled(!canPin);
            if (inst.pinned) ImGui::PushStyleColor(ImGuiCol_Text, Theme::kAmber);
            if (ImGui::SmallButton(inst.pinned ? ICON_LC_PIN "##pin" : ICON_LC_PIN_OFF "##pin"))
                host.SetPinned(inst.id, !inst.pinned);
            if (inst.pinned) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(inst.pinned ? "Pinned: this page stays while other things select. Click to follow."
                                  : canPin    ? "Pin this page"
                                              : "Nothing to pin");
            ImGui::EndDisabled();
            ImGui::Separator();
        }
    }

    InspectorWindowsResult DrawInspectorWindows(InspectorHost& host, InspectorWindowsState& state,
                                                bool* primaryOpen)
    {
        InspectorWindowsResult result;
        host.PruneStale();   // once per frame, <= kHistoryDepth pure lookups: the arrows below are truthful (spec s6 rule 3)
        // Snapshot the instances: SetPinned/GoBack/RepinKey inside the loop mutate host state.
        const std::vector<InspectorHost::Instance> instances = host.Instances();
        for (const InspectorHost::Instance& inst : instances)
        {
            // Flush text drafts deactivated while this instance was hidden or its
            // page switched -- BEFORE Begin, whether or not the window shows.
            PropertyGrid(state.grids[inst.id]).CommitOrphans();
            if (inst.id == 0 && primaryOpen && !*primaryOpen) continue;
            const std::string title = inst.id == 0
                ? std::string("Inspector")
                : "Inspector " + std::to_string(inst.id + 1) + "###inspector_" + std::to_string(inst.id);
            bool open = true;
            if (inst.id != 0)
                if (ImGuiWindow* primary = ImGui::FindWindowByName("Inspector"))
                    if (primary->DockId != 0)
                        ImGui::SetNextWindowDockID(primary->DockId, ImGuiCond_FirstUseEver);
            // BEFORE resolving the page: CanPin calls PageFor on the current
            // source, which re-targets the scene source's draw selection; the
            // Page()/PageFor() call below restores it.
            const bool canPin = inst.pinned || host.CanPin();
            // NO `if (Begin)` on purpose: the scene page's EditGesture::ScopeGuard
            // must run on collapsed/background-tab frames too (EditGesture.hpp:
            // 263-274) or an abandoned drag transaction stays open for the next
            // consumer to JOIN. Every widget bails on SkipItems; Section/Rows/
            // RowWithThumb already guard it.
            (void)ImGui::Begin(title.c_str(), inst.id == 0 ? primaryOpen : &open);
            InspectorSource* src = host.SourceFor(inst.id);
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) result.focusedSource = src;
            // Shortcut(), not IsKeyChordPressed(): RouteFocused means only the
            // focused instance fires, and an active InputText does not swallow
            // Ctrl+S (SpriteDocument.cpp:178-183 relies on the same fact).
            if (src && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) result.saveRequested.push_back(src);
            InspectorPage* page = nullptr;
            if (inst.pinned) page = src ? src->PageFor(inst.pinnedKey) : nullptr;
            else page = src ? src->Page() : nullptr;
            DrawHeader(host, inst, src, page, canPin);
            if (inst.pinned && !page)
            {
                // The pinned source closed or its selection went away: one line,
                // click to follow again (spec s3.3). NOT auto-released: registry
                // restore resurrects exact entity ids (delete->undo, Play->Stop),
                // so the page comes back by itself on the next PageFor.
                const std::string note = (inst.sourceClosed ? inst.pinnedName + " closed"
                                                            : "Pinned selection is gone")
                                         + " -- click to follow the selection";
                if (ImGui::Selectable(note.c_str())) host.SetPinned(inst.id, false);
            }
            else if (page)
            {
                PropertyGrid grid(state.grids[inst.id]);
                page->Draw(grid);
            }
            ImGui::End();
            if (inst.id != 0 && !open) result.closed.push_back(inst.id);
        }
        return result;
    }
}
```

`GetWindowContentRegionMax` (obsolete in ImGui 1.92) is not used anywhere in this file: the crumb child's fixed width right-aligns the pin. The body is drawn on every path after `Begin` -- see the comment in the loop -- because the scene page's `EditGesture::ScopeGuard` must destruct on hidden-tab frames too.

- [ ] **Step 3: The menu** -- `EditorPanels.hpp` MenuRequests: add `bool newInspector = false;   // Window -> New Inspector (another Inspector instance, own pin)` after `resetLayout`. `EditorPanels.cpp:327`, before the Reset Layout item:

```cpp
                // Another Inspector instance (inspector-ownership spec s3.3):
                // pin one, let the other follow.
                // Stays enabled: InspectorHost::AddInstance() returns -1 when the
                // pool of 8 is full and the app treats that as a no-op; the menu
                // has no host access.
                if (ImGui::MenuItem("New Inspector"))
                    requests.newInspector = true;
                ImGui::Separator();
```

- [ ] **Step 4: App members** (`EditorApp.hpp`, next to `m_inspector` at :1042)

```cpp
        // Inspector ownership (spec 2026-09-28): the scene source, the host
        // that routes the last-selecting source to every Inspector instance,
        // the per-instance draw state, and the two epoch watermarks (the
        // scene's SelectionEdge over SelectionContext::Epoch(), the per-
        // document map over SelectionEpoch()) that turn a selection GESTURE
        // into a host event (never focus, never a prune). Declared in
        // dependency order: the host holds a reference to the scene source.
        Arcane::Editor::SceneInspectorSource  m_sceneSource;
        Arcane::Editor::InspectorHost         m_inspectorHost{ m_sceneSource };
        Arcane::Editor::InspectorWindowsState m_inspectorWindows;
        Arcane::Editor::SelectionEdge         m_sceneSelectionEdge;   // epoch-based (Task 2): Observe(m_selection.Epoch(), key)
        std::unordered_map<const Arcane::Editor::EditorDocument*, std::uint64_t> m_docSelectionEpochs;
        Arcane::Editor::InspectorSource* m_inspectorFocusedSource = nullptr;   // latched each Inspector draw; read by the Ctrl+S gate
        // The document an Inspector source IS, for the Ctrl+S routes: null for
        // the scene source and for null (EditorDocument derives from
        // InspectorSource, Task 2).
        [[nodiscard]] Arcane::Editor::EditorDocument* InspectorSaveTarget(Arcane::Editor::InspectorSource* src) const
        {
            return dynamic_cast<Arcane::Editor::EditorDocument*>(src);
        }
        static void* InspectorSettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name);
        static void  InspectorSettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line);
        static void  InspectorSettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler*, ImGuiTextBuffer* buf);
        void RegisterInspectorSettingsHandler();
```

Add the includes (`Panels/SceneInspectorSource.hpp`, `Panels/InspectorHost.hpp`, `Panels/InspectorWindows.hpp`). Because `m_inspectorHost` stores raw `EditorDocument*` sources and `m_documents` is declared later (:1363, destructs FIRST), the host must never touch its sources in its destructor -- it does not (verify: `~InspectorHost` is defaulted).

- [ ] **Step 5: Registration + ini** (`EditorApp.cpp`)

After the `.arcinput` factory registration (:826-830):

```cpp
        // Every opened document is an Inspector source until it closes
        // (inspector-ownership spec s3.1: "a source that closes releases").
        m_documents.SetObserver({
            [this](Arcane::Editor::EditorDocument& d) { m_inspectorHost.AddSource(d); },
            [this](Arcane::Editor::EditorDocument& d)
            {
                m_inspectorHost.RemoveSource(d);
                m_docSelectionEpochs.erase(&d);
                if (m_inspectorFocusedSource == &d) m_inspectorFocusedSource = nullptr;   // the latch is a raw pointer into m_documents
            } });
```

The ini handler, modelled line-for-line on `PanelVisibilitySettings*` (:277-318) with type name `"EditorInspector"`, section `"Instances"`, one line `Ids=<extra ids, comma-separated>` (extra ids only -- instance 0 is implicit; an empty `Ids=` line restores {0}; add `#include <cstdlib>` for `std::strtol`, `<cstring>` is already there at :78):

```cpp
    void* EditorApp::InspectorSettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name)
    {
        return std::strcmp(name, "Instances") == 0 ? handler->UserData : nullptr;
    }
    void EditorApp::InspectorSettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
    {
        auto* app = static_cast<EditorApp*>(entry);
        if (std::strncmp(line, "Ids=", 4) != 0) return;
        std::vector<int> ids;
        for (const char* p = line + 4; *p;)
        {
            char* end = nullptr;
            const long v = std::strtol(p, &end, 10);
            if (end == p) break;
            ids.push_back(static_cast<int>(v));
            p = (*end == ',') ? end + 1 : end;
        }
        app->m_inspectorHost.SetInstanceIds(ids);   // 0, duplicates and out-of-range ids are dropped inside
    }
    void EditorApp::InspectorSettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
    {
        auto* app = static_cast<EditorApp*>(handler->UserData);
        buf->appendf("[%s][Instances]\nIds=", handler->TypeName);
        bool first = true;
        for (const auto& inst : app->m_inspectorHost.Instances())
            if (inst.id != 0) { buf->appendf(first ? "%d" : ",%d", inst.id); first = false; }
        buf->append("\n\n");
    }
    void EditorApp::RegisterInspectorSettingsHandler()
    {
        if (ImGui::GetCurrentContext() == nullptr ||
            ImGui::FindSettingsHandler("EditorInspector") != nullptr)
            return;   // same idempotence guard as RegisterPanelVisibilitySettings (:306-310)

        ImGuiSettingsHandler handler;
        handler.TypeName = "EditorInspector";
        handler.TypeHash = ImHashStr("EditorInspector");
        handler.UserData = this;
        handler.ReadOpenFn = &EditorApp::InspectorSettingsReadOpen;
        handler.ReadLineFn = &EditorApp::InspectorSettingsReadLine;
        handler.WriteAllFn = &EditorApp::InspectorSettingsWriteAll;
        ImGui::AddSettingsHandler(&handler);
    }
```

Call `RegisterInspectorSettingsHandler()` right after the `RegisterPanelVisibilitySettings();` call at `EditorApp.cpp:520` (the function :312 belongs to is `RegisterPanelVisibilitySettings`, declared at `EditorApp.hpp:960` -- declare `RegisterInspectorSettingsHandler` beside it). Pins are deliberately NOT persisted: a pin names a selection, and a selection does not survive a restart (the same reason history is never persisted, spec s6.3); the instance ID LIST is layout and is -- ids are dock slots (ImGui keys `###inspector_<id>` settings on the id, imgui.cpp:2523), so a closed slot stays closed across a restart and Window > New Inspector reopens the lowest free slot (UE's Details 1..4 rule). The section lands in `%LOCALAPPDATA%\Arcane\editor\layouts\<project-guid>.ini` (EditorApp.cpp:1585-1592) -- never the exe-dir imgui.ini -- and in --headless runs is read only from `ReferenceProject/Saved/verify-layout.ini`, where its absence means {0}.

- [ ] **Step 6: Per-frame sync + draw** (`EditorAppFrame.cpp` `DrawSelectionPanels`, replacing the Task 1 block)

```cpp
        // ---- Inspector ownership: sources -> host -> instances ----------
        // Selection EVENTS only (spec s3.1): a scene selection GESTURE (the
        // SelectionContext epoch moved -- Select/Toggle/AddRange/Clear, never
        // the per-frame Prune above) that left a non-empty key, or a document
        // whose SelectionEpoch moved AND now has a selection. Focus, tab
        // activation, background clicks, and a prune that re-primaried after a
        // deletion or a structural undo: none of these reach the host. Re-
        // clicking the already-selected entity IS an event (it brings the
        // Inspector back from a document); Push's echo compare keeps it out of
        // history.
        m_sceneSource.Bind({ &m_runtime->Registry(), &m_selection, m_undo ? &*m_undo : nullptr,
                             &m_editBinding, m_runtime->CurrentProject(), &m_inspector,
                             &m_inspectorServices, &m_assetModel.selected });
        if (m_sceneSelectionEdge.Observe(m_selection.Epoch(), m_sceneSource.SelectionKey()))
            m_inspectorHost.NotifySelected(m_sceneSource);
        m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
        {
            std::uint64_t& last = m_docSelectionEpochs[&d];
            const std::uint64_t epoch = d.SelectionEpoch();
            if (epoch == last) return;
            last = epoch;
            if (!d.SelectionKey().empty())
                m_inspectorHost.NotifySelected(d);
        });
        if (m_undo)   // the scene body needs the stack; DrawInspectorBody takes it by reference
        {
            const Arcane::Editor::InspectorWindowsResult res = Arcane::Editor::DrawInspectorWindows(
                m_inspectorHost, m_inspectorWindows,
                m_panelVis.OpenFlag(Arcane::Editor::PanelId::Inspector));
            for (const int id : res.closed)
            {
                m_inspectorHost.RemoveInstance(id);
                m_inspectorWindows.grids.erase(id);
            }
            m_inspectorFocusedSource = res.focusedSource;
            for (Arcane::Editor::InspectorSource* src : res.saveRequested)
                if (auto* doc = InspectorSaveTarget(src); doc && !doc->Save())
                    ARC_WARN("Inspector: save refused for '{}'", doc->Title());
        }
```

The `m_selection.Prune(...)` at the top of `DrawSelectionPanels` (EditorAppFrame.cpp:3581) stays where it is, BEFORE the edge: it never bumps the epoch, so it never reads as a gesture.

The raw-scancode scene Ctrl+S gate (EditorAppFrame.cpp:2640-2641) becomes:

```cpp
        const bool docOwnsSave =
            fs.scSaveScene && (m_documents.FocusedDoc() != nullptr ||
                               InspectorSaveTarget(m_inspectorFocusedSource) != nullptr);
```

and its comment gains: "...or an Inspector instance showing a DOCUMENT's page (the document's Ctrl+S then routes through that instance, InspectorWindows.cpp). An instance on the scene page leaves the scene keybind alone."

`ConsumeMenuRequests` (EditorAppFrame.cpp:2445+) -- reopen before create (UE's summon-details rule: reuse an open view, else the first CLOSED slot; never a copy beside a closed one). Instance 0 is the standing follower (spec s3.3) and its visibility is the Window > Inspector checkbox:

```cpp
        if (menuReq.newInspector)
        {
            bool* primary = m_panelVis.OpenFlag(Arcane::Editor::PanelId::Inspector);
            if (primary && !*primary) *primary = true;       // hidden follower: bring it back, no new instance
            else (void)m_inspectorHost.AddInstance();        // visible: another instance with its own pin (-1 = pool full, no-op)
        }
```

Selection events never open an Inspector; only the explicit menu path reopens.

Project switch (`EditorAppProject.cpp:2164`, `ResetPerProjectState`, BEFORE `m_documents.CloseAll();`) is ONLY:

```cpp
        // Inspector ownership (spec decision 2): every non-fallback source
        // releases on a project switch, pins included. The scene half (fallback
        // invalidation + its watermark) lives in ClearSceneReferences, which
        // this function calls below (:2168).
        m_inspectorHost.ReleaseAll();
        m_docSelectionEpochs.clear();
```

and in `EditorApp::ClearSceneReferences()` (`EditorAppScene.cpp:150`, right after `m_inspector = {};` and before `if (m_undo) m_undo->Clear();`) add:

```cpp
        // Inspector ownership: the pinned keys and history entries naming
        // entities of the OUTGOING registry die here too. A fresh registry
        // (Runtime::ResetRegistry -> a new Astra::Registry, EntityIDStack from
        // id 0 / version 1) re-mints the SAME (id,version) values, so
        // SceneInspectorSource::Alive cannot tell an old key from a new entity
        // -- the owner of the swap says so explicitly (UE pushes
        // RemoveDeletedObjects into every details view for the same reason).
        // Play > Stop is NOT such a swap: it restores the pre-Play snapshot, so
        // the same keys name the same entities and pins/history survive it.
        m_inspectorHost.InvalidateSource(m_sceneSource);
        m_sceneSelectionEdge.lastEpoch = m_selection.Epoch();   // re-arm on the Clear() above: the first GESTURE in the new scene is the first event
```

(ClearSceneReferences runs on New Scene :156, Open Scene :184 and from ResetPerProjectState :2168, so one hook covers all three registry swaps.)

The instance loop draws the body UNCONDITIONALLY after Begin (see the comment in DrawInspectorWindows): DrawInspectorBody's `EditGesture::ScopeGuard` therefore still destructs on every path, the same frame position as before (Phase 18), so its "last guard to run" ordering with `ShaderEditorDocument::Draw` (EditorAppFrame.cpp:2380) is unchanged.

- [ ] **Step 7: Build, tests, desk, goldens**

Build; `./ArcaneTests.exe "[editor]"` green. Desk (the user's editor is not running -- check `Get-Process ArcaneEditor` first): launch `ArcaneEditor.exe --project ReferenceProject`, verify and record each: (1) selecting MeshCube in the Outliner shows `Scene > MeshCube` with the same component sections; (1b, mockup step 3, focus is not selection) with `Scene > MeshCube` showing, click the Console tab, the Problems tab, the Asset Browser tab and an Asset Browser row, the Outliner's empty space below the last entity, activate then leave the Viewport tab: the breadcrumb still reads `Scene > MeshCube`; (2) clicking the viewport background clears the selection: the page says "No selection" and the pin button is disabled with the tooltip "Nothing to pin"; (3) Window > New Inspector docks a second tab beside the first; pin it, select another entity: the pinned one holds; pin an instance on a binding-less multi-selection (ctrl-click three entities), delete the primary: the pinned page shows the two survivors with "(+1)"; (4) back/forward walks the selections; hovering Back reads "Back to Scene > <name>"; right-click Back lists prior entries nearest-first and clicking one lands there in one step; (5) close the second instance with its X; (5b) uncheck Window > Inspector (the primary hides), then Window > New Inspector: the primary reappears and no "Inspector 2" is created; with the primary visible, New Inspector still adds Inspector 2; (6) open a third instance, close the SECOND, drag the third somewhere distinct, restart: `Inspector 3` returns in that spot (`[EditorInspector][Instances]` in `%LOCALAPPDATA%\Arcane\editor\layouts\cfafaf09-86bb-4b4b-a99f-9ecb0771bc15.ini` shows `Ids=2`; no `Inspector 2` window), then Window > New Inspector reopens `Inspector 2` in its old slot; open extras until New Inspector stops adding (8 total); (7) create an entity (Outliner +), ctrl-click it as a second member, Ctrl+Z (the creation): the Inspector and history do not move (a prune is not a gesture); (8) pin the primary on MeshCube, File > Open Scene a different scene: the pinned instance reads "Pinned selection is gone -- click to follow the selection", never another entity, and Back is disabled (history pruned); (9) Play then Stop with an instance pinned on MeshCube: the pin still shows MeshCube; (10) drag a Position field in Inspector 1 and, mid-drag, click the Inspector 2 tab: Ctrl+Z reverts exactly that drag and a following gizmo drag is its own undo step; (11) `git status` shows no `imgui.ini` in the exe dir touched by the desk (the layout lives in %LOCALAPPDATA%). Record each in the report.

Then the goldens (unchanged procedure): run `scripts/golden-gate.ps1 -Configuration Debug` -- expect the FOUR editor lanes red (new header pixels) and the four runtime lanes green. Re-bless per the Global Constraints procedure: `editor-ui` (dx12 blesses the shared slot) and `editor-ui-perspective` (dx12 with `--view-mode perspective`), copy both PNGs from the staged `ReferenceProject/Verify/References/` into the source tree's, re-run the gate: 8/8 green. Commit the PNGs with the code.

- [ ] **Step 8: Commit**

```bash
git add ArcaneEditor/src/Panels/InspectorWindows.hpp ArcaneEditor/src/Panels/InspectorWindows.cpp ArcaneEditor/src/Panels/EditorPanels.hpp ArcaneEditor/src/Panels/EditorPanels.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneEditor/src/App/EditorApp.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneEditor/src/App/EditorAppProject.cpp ArcaneEditor/src/App/EditorAppScene.cpp ReferenceProject/Verify/References/editor-ui.png ReferenceProject/Verify/References/editor-ui-perspective.png
git commit -m "feat(editor): Inspector instances -- pin (only for a resolvable page, RepinKey crumbs), back/forward with labelled targets + right-click jump, end-scrolled breadcrumb, Window > New Inspector (reopens a hidden primary, pool of 8), instance ID LIST in the layout ini, Ctrl+S routed to the shown document; sources release on project switch, the scene invalidates on every registry swap; editor goldens re-blessed for the header (inspector ownership plan T4)"
```

---
### Task 5: Engine growth -- control tables, readable names, canonical keys, per-binding value, rebind timer + modifier chords, LocalInputUser same-project re-Configure, ABI 45 + ReferenceProject rebuild

**Files:**
- Modify: `ArcaneClient/src/Arcane/Input/InputActions.hpp`, `ArcaneClient/src/Arcane/Input/InputActions.cpp` (tables at :84-160, `CompileSinglePath` :171-262, `CompilePath` :284-302, `ResolveControl` :308, `BindingDisplayString` :1001, `Update` :1057), `ArcaneClient/src/Arcane/Input/InputRebindOperation.hpp`, `ArcaneClient/src/Arcane/Input/InputRebindOperation.cpp` (keyboard branch :48-59, mouse :60-70), `ArcaneClient/src/Arcane/Input/LocalInputUser.hpp`, `ArcaneClient/src/Arcane/Input/LocalInputUser.cpp` (`Configure` :15-39), `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:972` (`kGamePluginABIVersion` 44 -> 45 with the neighbours' changelog line), `ReferenceProject/ReferenceProject.arcproj:6` (`"abi": 44` -> `45`)
- Test: `ArcaneTests/src/InputActionsTest.cpp`, `ArcaneTests/src/InputRebindOperationTest.cpp`, `ArcaneTests/src/ClientRuntimeTest.cpp`

Every engine LAYOUT change of the plan lands HERE (the `InputActions` vtable, the `InputRebindOperation` member, the `LocalInputUser` members held by value inside `ARCANE_API ClientRuntime`), so there is exactly ONE ABI bump and ONE ReferenceProject rebuild (Step 4c). Line numbers are advisory; the quoted text anchors govern.

**Interfaces:**
- Produces (all in `namespace Arcane`, `InputActions.hpp`):
  ```cpp
  struct InputControlDisplay { std::string device; std::string control; };      // {"Gamepad","South Button"}
  struct InputControlChoice  { std::string path; InputControlDisplay display; };
  // static, on InputActions:
  [[nodiscard]] static std::vector<InputControlChoice> KnownControls();          // every simple path the compiler accepts, from ITS OWN tables
  [[nodiscard]] static bool IsKnownControlPath(std::string_view path);          // every '+' part compiles, silently
  [[nodiscard]] static std::string CanonicalControlKey(std::string_view path);  // the COMPILED identity, spelling-independent: "<Mouse>/button/1" == "<Mouse>/leftButton", "<Keyboard>/scancode/space" == "<Keyboard>/space" on the running layout, "a+b" == "b+a"; "" when any part fails to compile
  [[nodiscard]] static InputControlDisplay DisplayForPath(std::string_view path);
  // virtual, on InputActions:
  [[nodiscard]] virtual float BindingValue(const Guid& binding) const = 0;      // the binding's raw value from the last Update's snapshot; composites = max over parts; 0 unknown/before Update
  // InputRebindOperation:
  [[nodiscard]] float Remaining() const noexcept;                              // seconds left in the capture
  // Observe: modifier keys accumulate; the first non-modifier key completes with the held modifiers prefixed ("<Keyboard>/scancode/lshift+<Keyboard>/scancode/a"); a modifier's release completes with the bare modifier only when no other modifier is still held.
  // LocalInputUser:
  [[nodiscard]] std::optional<Guid> ActiveMap() const noexcept;                // top of the map stack (the base map when nothing is pushed); nullopt before Configure
  ```
  `BindingDisplayString(binding)` now returns `DisplayForPath(path).device + " " + .control` for simple bindings (composites unchanged: "1D Axis"/"2D Vector").
  `LocalInputUser::Configure(asset, projectId)` called for the project it is ALREADY configured for (the editor's save republish, input-editor spec B 2.6) is a RE-ENTRY: it swaps the evaluator in place, replays the map stack, the control scheme and a dirty profile's overrides, and primes the new evaluator with the last snapshot so a control held across the swap does not re-fire. A different project is the cold start it always was. `Conflicts()` in Task 6 compares `CanonicalControlKey`s, never path strings.

- [ ] **Step 1: Write the failing tests** (append to `InputActionsTest.cpp`)

```cpp
TEST_CASE("input: KnownControls is generated from the compiler's own tables and every entry compiles", "[input]")
{
    const auto known = Arcane::InputActions::KnownControls();
    REQUIRE(known.size() > 40);
    for (const auto& c : known)
    {
        INFO(c.path);
        CHECK(Arcane::InputActions::IsKnownControlPath(c.path));
        CHECK_FALSE(c.display.device.empty());
        CHECK_FALSE(c.display.control.empty());
    }
    auto has = [&](const char* p) { return std::any_of(known.begin(), known.end(), [&](const auto& c) { return c.path == p; }); };
    CHECK(has("<Keyboard>/space"));
    CHECK(has("<Keyboard>/a"));
    CHECK(has("<Keyboard>/f12"));
    CHECK(has("<Mouse>/leftButton"));
    CHECK(has("<Gamepad>/buttonSouth"));
    CHECK(has("<Gamepad>/leftStick/x"));
    CHECK(has("<Gamepad>/leftStick"));
    CHECK(has("<Gamepad>/rightTrigger"));
}

TEST_CASE("input: IsKnownControlPath refuses what the compiler would zero-compile", "[input]")
{
    using Arcane::InputActions;
    CHECK(InputActions::IsKnownControlPath("<Keyboard>/scancode/a"));
    CHECK(InputActions::IsKnownControlPath("<Keyboard>/lshift+<Keyboard>/a"));
    CHECK(InputActions::IsKnownControlPath("<Mouse>/button/4"));
    CHECK_FALSE(InputActions::IsKnownControlPath("<Keyboard>/spaec"));
    CHECK_FALSE(InputActions::IsKnownControlPath("<Wheel>/up"));
    CHECK_FALSE(InputActions::IsKnownControlPath("space"));
    CHECK_FALSE(InputActions::IsKnownControlPath(""));
    CHECK_FALSE(InputActions::IsKnownControlPath("<Keyboard>/a+<Wheel>/up"));
}

TEST_CASE("input: DisplayForPath splits the device from a readable control name", "[input]")
{
    using Arcane::InputActions;
    auto d = InputActions::DisplayForPath("<Keyboard>/space");
    CHECK(d.device == "Keyboard"); CHECK(d.control == "Space");
    d = InputActions::DisplayForPath("<Keyboard>/scancode/a");
    CHECK(d.device == "Keyboard"); CHECK(d.control == "A");
    d = InputActions::DisplayForPath("<Keyboard>/lshift");
    CHECK(d.control == "Left Shift");
    d = InputActions::DisplayForPath("<Gamepad>/buttonSouth");
    CHECK(d.device == "Gamepad"); CHECK(d.control == "South Button");
    d = InputActions::DisplayForPath("<Gamepad>/leftStick/x");
    CHECK(d.control == "Left Stick X");
    d = InputActions::DisplayForPath("<Gamepad>/dpadLeft");
    CHECK(d.control == "D-Pad Left");
    d = InputActions::DisplayForPath("<Mouse>/leftButton");
    CHECK(d.device == "Mouse"); CHECK(d.control == "Left Button");
    d = InputActions::DisplayForPath("<Keyboard>/lshift+<Keyboard>/a");
    CHECK(d.control == "Left Shift + A");
    d = InputActions::DisplayForPath("garbage");
    CHECK(d.device.empty()); CHECK(d.control == "garbage");
    // Captured paths carry SDL's own lower-cased scancode name: they display
    // with SDL's canonical spelling, resolved by the compiler's lookup.
    d = InputActions::DisplayForPath("<Keyboard>/scancode/left shift");
    CHECK(d.control == "Left Shift");
    d = InputActions::DisplayForPath("<Keyboard>/scancode/page down");
    CHECK(d.control == "Page Down");
    d = InputActions::DisplayForPath("<Keyboard>/scancode/keypad 1");
    CHECK(d.control == "Keypad 1");
}

TEST_CASE("input: CanonicalControlKey is spelling-independent", "[input]")
{
    using Arcane::InputActions;
    CHECK(InputActions::CanonicalControlKey("<Mouse>/button/1") == InputActions::CanonicalControlKey("<Mouse>/leftButton"));
    CHECK(InputActions::CanonicalControlKey("<Keyboard>/scancode/space") == InputActions::CanonicalControlKey("<Keyboard>/space"));
    CHECK(InputActions::CanonicalControlKey("<Keyboard>/lshift+<Keyboard>/a") == InputActions::CanonicalControlKey("<Keyboard>/a+<Keyboard>/lshift"));
    CHECK(InputActions::CanonicalControlKey("<Keyboard>/a") != InputActions::CanonicalControlKey("<Keyboard>/b"));
    CHECK(InputActions::CanonicalControlKey("<Keyboard>/spaec").empty());
    CHECK(InputActions::CanonicalControlKey("").empty());
}

TEST_CASE("input: BindingValue reports one binding's raw value from the last snapshot", "[input]")
{
    auto input = Arcane::InputActions::Create();
    const auto asset = Arcane::InputActionAsset::FromJson(nlohmann::json::parse(R"({
      "version": 1, "id": "11111111-1111-4111-8111-111111111111", "defaultMap": "22222222-2222-4222-8222-222222222222",
      "controlSchemes": [],
      "actionMaps": [{ "id": "22222222-2222-4222-8222-222222222222", "name": "demo", "actions": [
        { "id": "33333333-3333-4333-8333-333333333333", "name": "jump", "type": "Button", "bindings": [
          { "id": "44444444-4444-4444-8444-444444444444", "path": "<Keyboard>/space" },
          { "id": "55555555-5555-4555-8555-555555555555", "path": "<Gamepad>/buttonSouth" } ] },
        { "id": "66666666-6666-4666-8666-666666666666", "name": "move", "type": "Axis1D", "bindings": [
          { "id": "77777777-7777-4777-8777-777777777777", "composite": "1DAxis", "parts": [
            { "id": "88888888-8888-4888-8888-888888888888", "name": "negative", "path": "<Keyboard>/a" },
            { "id": "99999999-9999-4999-8999-999999999999", "name": "positive", "path": "<Keyboard>/d" } ] } ] }
      ] }] })")));
    REQUIRE(asset);
    REQUIRE(input->LoadAsset(*asset));
    const auto space = *Arcane::Guid::FromString("44444444-4444-4444-8444-444444444444");
    const auto south = *Arcane::Guid::FromString("55555555-5555-4555-8555-555555555555");
    const auto axis  = *Arcane::Guid::FromString("77777777-7777-4777-8777-777777777777");
    CHECK(input->BindingValue(space) == 0.0f);          // before any Update
    Arcane::InputSnapshot snap;
    snap.AddKeycode(kKeycodeSpace);
    input->Update(1.0 / 60.0, snap);
    CHECK(input->BindingValue(space) == 1.0f);
    CHECK(input->BindingValue(south) == 0.0f);
    CHECK(input->BindingValue(axis) == 0.0f);
    Arcane::InputSnapshot d; d.AddKeycode('d');
    input->Update(1.0 / 60.0, d);
    CHECK(input->BindingValue(axis) == 1.0f);           // max over parts
    CHECK(input->BindingValue(Arcane::Guid::Generate()) == 0.0f);
}
```

`kKeycodeSpace` is the constant the file already defines near its top; `'d'` is SDLK_d (SDL keycodes for letters are their lowercase ASCII, the same convention `kKeycodeGrave = 96` relies on at :82). `defaultMap` is REQUIRED by `FromJson` when `actionMaps` is non-empty (InputActionAsset.cpp:426-429): without it `REQUIRE(asset)` fails on every run. Append to `InputRebindOperationTest.cpp`:

```cpp
TEST_CASE("input profile: Remaining counts the capture timeout down", "[input][profile]")
{
    Arcane::InputRebindOperation capture;
    capture.Begin(Binding(), std::nullopt, 10.0f, {});
    CHECK(capture.Remaining() == Catch::Approx(10.0f));
    capture.Observe({}, 1.0f);
    CHECK(capture.Remaining() == Catch::Approx(9.0f));
    capture.Cancel();
    CHECK(capture.Remaining() == 0.0f);
}

// Modifier chords (SDL scancodes: LCTRL = 224, LSHIFT = 225, A = 4). '+' chords
// are first-class in the path compiler; a capture must be able to write one.
TEST_CASE("input profile: a held modifier prefixes the captured key as a chord", "[input][profile]")
{
    Arcane::InputRebindOperation capture;
    capture.Begin(Binding(), Arcane::InputDevice::Kbm, 5.0f, {});
    Arcane::InputSnapshot shift; shift.SetScancode(225);
    capture.Observe(shift, 0.1f);
    CHECK(capture.Result().state == Arcane::InputRebindState::Waiting);
    Arcane::InputSnapshot chord = shift; chord.SetScancode(4);
    capture.Observe(chord, 0.1f);
    CHECK(capture.Result().state == Arcane::InputRebindState::Completed);
    CHECK(capture.Result().replacementPath == "<Keyboard>/scancode/lshift+<Keyboard>/scancode/a");
}
TEST_CASE("input profile: a modifier pressed and released alone is captured bare", "[input][profile]")
{
    Arcane::InputRebindOperation capture;
    capture.Begin(Binding(), Arcane::InputDevice::Kbm, 5.0f, {});
    Arcane::InputSnapshot shift; shift.SetScancode(225);
    capture.Observe(shift, 0.1f);
    capture.Observe({}, 0.1f);
    CHECK(capture.Result().state == Arcane::InputRebindState::Completed);
    CHECK(capture.Result().replacementPath == "<Keyboard>/scancode/lshift");
}
TEST_CASE("input profile: releasing one modifier while another is held keeps waiting", "[input][profile]")
{
    Arcane::InputRebindOperation capture;
    capture.Begin(Binding(), Arcane::InputDevice::Kbm, 5.0f, {});
    Arcane::InputSnapshot shift; shift.SetScancode(225);
    capture.Observe(shift, 0.1f);
    Arcane::InputSnapshot both = shift; both.SetScancode(224);
    capture.Observe(both, 0.1f);
    capture.Observe(shift, 0.1f);                     // ctrl up, shift still down
    CHECK(capture.Result().state == Arcane::InputRebindState::Waiting);
    capture.Observe({}, 0.1f);                        // shift up, nothing else held
    CHECK(capture.Result().state == Arcane::InputRebindState::Completed);
    CHECK(capture.Result().replacementPath == "<Keyboard>/scancode/lshift");
}
```

(The file includes only `<catch2/catch_test_macros.hpp>` today: add `#include <catch2/catch_approx.hpp>` beside it -- the cases above spell `Catch::Approx` in full, so no using-declaration is needed. `Binding()` is the file's existing fixture at :7.) The existing cases that press a bare letter or mouse button are unaffected (a non-modifier still completes on its first newly-down frame); run the file and fix any case that pressed a modifier expecting immediate completion (none known).

Append to `ArcaneTests/src/ClientRuntimeTest.cpp` (it already includes `<Arcane/Input/LocalInputUser.hpp>` and parses JSON through `InputActionAsset.hpp`; put the helper in the file's existing anonymous namespace beside `RuntimeInputAsset()`, :67-79):

```cpp
namespace
{
    // Three revisions of one asset, as the editor's Save republishes them:
    // A = the session's original (Jump = Space, maps gameplay + menu),
    // B = Jump re-authored to K, C = B with the "menu" map removed.
    Arcane::InputActionAsset SessionAsset(const char* jumpPath, bool withMenu)
    {
        nlohmann::json doc = nlohmann::json::parse(R"JSON({
            "version":1,"id":"11111111-1111-4111-8111-111111111111",
            "defaultMap":"22222222-2222-4222-8222-222222222222",
            "controlSchemes":[{"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","name":"Gamepad","bindingGroup":"Gamepad"}],
            "actionMaps":[
              {"id":"22222222-2222-4222-8222-222222222222","name":"gameplay","actions":[
                {"id":"44444444-4444-4444-8444-444444444444","name":"Jump","type":"Button","bindings":[
                  {"id":"55555555-5555-4555-8555-555555555555","path":"<Keyboard>/space"}]}]},
              {"id":"33333333-3333-4333-8333-333333333333","name":"menu","actions":[
                {"id":"66666666-6666-4666-8666-666666666666","name":"Back","type":"Button","bindings":[
                  {"id":"77777777-7777-4777-8777-777777777777","path":"<Keyboard>/escape"}]}]}
            ]})JSON");
        doc["actionMaps"][0]["actions"][0]["bindings"][0]["path"] = jumpPath;
        if (!withMenu) doc["actionMaps"].erase(1);
        return *Arcane::InputActionAsset::FromJson(doc);
    }
}

TEST_CASE("LocalInputUser re-Configure for the same project replays the map stack, scheme and dirty overrides", "[client][input]")
{
    // Fresh project ids: SDL's pref path may hold a Default.json from an earlier
    // run under a fixed id, and a loaded profile would mask the dirty one.
    const auto projectId      = Arcane::Guid::Generate();
    const auto otherProjectId = Arcane::Guid::Generate();
    const auto gameplayId     = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto menuId         = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    const auto jumpActionId   = *Arcane::Guid::FromString("44444444-4444-4444-8444-444444444444");
    const auto jumpBindingId  = *Arcane::Guid::FromString("55555555-5555-4555-8555-555555555555");
    Arcane::LocalInputUser user;
    CHECK_FALSE(user.ActiveMap().has_value());
    REQUIRE(user.Configure(SessionAsset("<Keyboard>/space", true), projectId));
    CHECK(user.ActiveMap() == gameplayId);                          // the base map
    REQUIRE(user.PushMap(menuId));
    CHECK(user.ActiveMap() == menuId);
    REQUIRE(user.SetControlScheme("Gamepad"));
    REQUIRE(user.SetOverride(jumpBindingId, "<Keyboard>/j"));
    REQUIRE(user.ProfileDirty());
    REQUIRE(user.Configure(SessionAsset("<Keyboard>/k", true), projectId));     // the save's republish
    CHECK(user.ActiveMap() == menuId);                              // pushed map survived
    REQUIRE(user.Bindings(jumpActionId).size() == 1);
    CHECK(user.Bindings(jumpActionId)[0].authoredPath == "<Keyboard>/k");   // the edit is live
    CHECK(user.Bindings(jumpActionId)[0].effectivePath == "<Keyboard>/j");  // the unsaved override still sits on it
    CHECK(user.ProfileDirty());                                     // unsaved override kept
    CHECK(user.BindingDisplayString(jumpBindingId) == "Keyboard J");
    REQUIRE(user.Configure(SessionAsset("<Keyboard>/k", false), projectId));    // "menu" removed
    CHECK(user.ActiveMap() == gameplayId);                          // collapsed to the surviving base
    CHECK(user.ProfileDirty());
    REQUIRE(user.Configure(SessionAsset("<Keyboard>/k", false), otherProjectId)); // a different project: cold start
    CHECK_FALSE(user.ProfileDirty());
    CHECK(user.Bindings(jumpActionId)[0].effectivePath == "<Keyboard>/k");
    CHECK(user.ProjectId() == otherProjectId);
}

TEST_CASE("ClientRuntime: a re-Configure does not re-fire a control held across it", "[client][input]")
{
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    const auto project = Arcane::Guid::Generate();
    REQUIRE(runtime.ConfigureGameInput(RuntimeInputAsset(), project));
    const auto jump = runtime.GameInput().FindAction("Player", "Jump");
    REQUIRE(jump);
    Arcane::InputSnapshot raw; raw.AddKeycode(32);                  // space held
    runtime.UpdateGameInput(1.0 / 60.0, raw);
    CHECK(runtime.GameInput().Pressed(*jump));                      // the real press
    REQUIRE(runtime.ConfigureGameInput(RuntimeInputAsset(), project));
    runtime.UpdateGameInput(1.0 / 60.0, raw);                       // first live tick, still held
    CHECK(runtime.GameInput().Down(*jump));
    CHECK_FALSE(runtime.GameInput().Pressed(*jump));
    CHECK_FALSE(runtime.GameInput().Started(*jump));
    runtime.BeginGameInputFixedStep();
    CHECK_FALSE(runtime.GameInput().PressedThisFixedStep(*jump));
    runtime.UpdateGameInput(1.0 / 60.0, {});
    CHECK(runtime.GameInput().Released(*jump));                     // the held state was carried, not zeroed
}
```

(`RuntimeInputAsset()` names its map "Player" and its action "Jump", ClientRuntimeTest.cpp:67-79. `LocalInputUser::AuthoredPath` is PRIVATE, so the test reads the authored path through the public `Bindings(action)[0].authoredPath`, the same struct the runtime's `Bindings()` fills.)

- [ ] **Step 2: Run to verify they fail** -- build. Expected: `KnownControls` / `CanonicalControlKey` / `BindingValue` / `Remaining` / `ActiveMap` undeclared; the three chord cases and the two ClientRuntimeTest cases cannot compile until they are.

- [ ] **Step 3: Declare** (`InputActions.hpp`, before `class ARCANE_API InputActions`)

```cpp
    // A control path's readable name with its device split out, for a UI that
    // draws the device as an icon: "<Gamepad>/buttonSouth" -> {"Gamepad",
    // "South Button"}, "<Keyboard>/scancode/a" -> {"Keyboard", "A"}. A path
    // the compiler cannot parse comes back as {"", path}. Captured scancode
    // paths display with SDL's canonical name, resolved by the compiler's own
    // lookup ("<Keyboard>/scancode/left shift" -> "Left Shift").
    struct InputControlDisplay
    {
        std::string device;
        std::string control;
    };

    // One entry of KnownControls(): a simple path the compiler accepts.
    struct InputControlChoice
    {
        std::string path;
        InputControlDisplay display;
    };
```

and inside the class, after `BindingDisplayString`:

```cpp
        // ---- control-path vocabulary (input-editor redesign spec s2.4) ----
        // Generated from the path compiler's OWN tables (LoveToSdlName, the
        // gamepad token tables, the mouse branch), never a hand-kept list, so
        // an editor picker built from it can never offer a path that compiles
        // to a constant-zero binding (hygiene pass 2026-09-28: five did).
        [[nodiscard]] static std::vector<InputControlChoice> KnownControls();
        // True when every '+'-separated part compiles -- the exact test
        // LoadAsset applies, minus its warning.
        [[nodiscard]] static bool IsKnownControlPath(std::string_view path);
        // The compiled identity of a control path, spelling-independent: two
        // paths that drive the same physical control on the running layout
        // yield the same key ("<Mouse>/button/1" and "<Mouse>/leftButton",
        // "<Keyboard>/scancode/space" and "<Keyboard>/space", "a+b" and
        // "b+a"). Empty when any part fails to compile. The editor's conflict
        // detection compares THESE, never path strings: the rebind capture
        // writes the scancode form while assets author the keycode form.
        [[nodiscard]] static std::string CanonicalControlKey(std::string_view path);
        [[nodiscard]] static InputControlDisplay DisplayForPath(std::string_view path);
        // The binding's RAW value from the last Update's snapshot: 1/0 for a
        // button chord, the signed axis value, the max over a composite's
        // parts. 0 for an unknown id or before any Update. Feeds the editor's
        // live-preview glow; the runtime asks actions, not bindings.
        [[nodiscard]] virtual float BindingValue(const Guid& binding) const = 0;
```

`InputRebindOperation.hpp` (add `#include <vector>`): public `[[nodiscard]] float Remaining() const noexcept { return result_.state == InputRebindState::Waiting ? remaining_ : 0.0f; }`; private, after `remaining_`:

```cpp
        // Modifier scancodes newly pressed during THIS capture, in press order.
        // The first non-modifier completes with them prefixed as a '+' chord;
        // a modifier released while it is the last one held completes bare.
        // Modifiers already down at Begin are never seeded (the initiating
        // control must be released and re-pressed -- the existing rule).
        std::vector<uint32_t> heldModifiers_;
```

`Begin` clears it: add `heldModifiers_.clear();` beside `previous_ = currentSnapshot;`.

- [ ] **Step 4: Implement** (`InputActions.cpp`)

Refactor `CompileSinglePath` into a quiet core + the warning wrapper. Add, right above it (:171):

```cpp
        // The quiet core: nullopt for anything the compiler does not know,
        // with NO warning -- IsKnownControlPath and the editor's picker ask
        // this question thousands of times a session.
        std::optional<ControlId> TryCompileSinglePath(const std::string& path)
        {
            ... the body of CompileSinglePath with every `ARC_WARN(...); return {};` replaced by `return std::nullopt;`, every `return { Source, code };` rewritten as `return ControlId{ Source, code };` (a braced pair cannot copy-list-initialise a `std::optional<ControlId>`), and the leading `if (path.empty()) return {};` (:180, no warning today) rewritten as `if (path.empty()) return ControlId{};` so an empty path stays a silent None as today; `<optional>` is already reachable (InputActions.hpp:21) ...
        }
        ControlId CompileSinglePath(const std::string& path, const std::string& mapName,
                                    const std::string& actionName)
        {
            if (const auto id = TryCompileSinglePath(path)) return *id;
            ARC_WARN("input: unknown control path '{}' in {}/{}", path, mapName, actionName);
            return {};
        }
```

(The distinct warn texts "unknown scancode name", "unknown key name", "unknown mouse control" collapse into the one line above; `InputActionsTest`'s existing "unknown path token" case only checks the zero-compiled behaviour, not the text -- confirm with `grep -n "unknown" ArcaneTests/src/InputActionsTest.cpp`.) Then the statics, placed after the anonymous namespace closes (they need `LoveToSdlName`, the token tables and `TryCompileSinglePath`):

```cpp
    namespace
    {
        const char* GamepadDisplayName(const std::string& token)
        {
            struct Entry { const char* token; const char* name; };
            static constexpr Entry kTable[] = {
                { "buttonSouth", "South Button" }, { "buttonEast", "East Button" },
                { "buttonWest", "West Button" },   { "buttonNorth", "North Button" },
                { "dpadUp", "D-Pad Up" }, { "dpadDown", "D-Pad Down" },
                { "dpadLeft", "D-Pad Left" }, { "dpadRight", "D-Pad Right" },
                { "leftShoulder", "Left Shoulder" }, { "rightShoulder", "Right Shoulder" },
                { "start", "Start" }, { "back", "Back" }, { "guide", "Guide" },
                { "leftStickPress", "Left Stick Press" }, { "rightStickPress", "Right Stick Press" },
                { "leftStick/x", "Left Stick X" }, { "leftStick/y", "Left Stick Y" },
                { "rightStick/x", "Right Stick X" }, { "rightStick/y", "Right Stick Y" },
                { "leftTrigger", "Left Trigger" }, { "rightTrigger", "Right Trigger" },
                { "leftStick", "Left Stick" }, { "rightStick", "Right Stick" },
            };
            for (const auto& e : kTable) if (token == e.token) return e.name;
            return nullptr;
        }

        std::string ReadableControl(const std::string& device, std::string control)
        {
            if (device == "Gamepad")
                if (const char* n = GamepadDisplayName(control)) return n;
            if (device == "Keyboard")
            {
                constexpr std::string_view scan = "scancode/";
                const bool isScancode = control.starts_with(scan);
                if (isScancode) control.erase(0, scan.size());
                if (const char* sdl = LoveToSdlName(control)) return sdl;   // "Left Shift", "Space"
                if (control.size() == 1) return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(control[0]))));
                if (control.size() >= 2 && control[0] == 'f' && std::isdigit(static_cast<unsigned char>(control[1]))) { control[0] = 'F'; return control; }
                // A captured path carries SDL's own (lower-cased) name: resolve it
                // through the same lookup the compiler uses so it displays with
                // SDL's canonical spelling ("left shift" -> "Left Shift"), never
                // through string surgery. Empty means SDL does not know the text.
                if (isScancode)
                {
                    if (const char* n = SDL_GetScancodeName(SDL_GetScancodeFromName(control.c_str())); n && *n) return n;
                }
                else
                {
                    if (const char* n = SDL_GetKeyName(SDL_GetKeyFromName(control.c_str())); n && *n) return n;
                }
            }
            if (device == "Mouse")
            {
                if (control == "leftButton") return "Left Button";
                if (control == "rightButton") return "Right Button";
                if (control == "middleButton") return "Middle Button";
                if (control.starts_with("button/")) return "Button " + control.substr(7);
            }
            // Fallback: the camel-case split BindingDisplayString always did.
            std::string readable;
            for (char c : control)
            {
                if (c == '/') readable += ' ';
                else if (std::isupper(static_cast<unsigned char>(c)) && !readable.empty() &&
                         std::islower(static_cast<unsigned char>(readable.back()))) { readable += ' '; readable += c; }
                else readable += c;
            }
            if (!readable.empty()) readable.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(readable.front())));
            return readable;
        }
    }

    InputControlDisplay InputActions::DisplayForPath(std::string_view pathView)
    {
        const std::string path(pathView);
        InputControlDisplay out;
        std::size_t start = 0;
        while (start <= path.size())
        {
            const std::size_t plus = path.find('+', start);
            const std::string part = path.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            const std::size_t close = part.find(">/");
            if (part.empty() || part.front() != '<' || close == std::string::npos)
                return { {}, path };   // unparseable: hand the raw text back whole
            const std::string device = part.substr(1, close - 1);
            if (out.device.empty()) out.device = device;
            if (!out.control.empty()) out.control += " + ";
            out.control += ReadableControl(device, part.substr(close + 2));
            if (plus == std::string::npos) break;
            start = plus + 1;
        }
        return out;
    }

    bool InputActions::IsKnownControlPath(std::string_view path)
    {
        if (path.empty()) return false;
        for (const std::string& part : SplitChordParts(path))
            if (part.empty() || !TryCompileSinglePath(part)) return false;
        return true;
    }

    std::string InputActions::CanonicalControlKey(std::string_view path)
    {
        if (path.empty()) return {};
        std::vector<std::string> keys;
        for (const std::string& part : SplitChordParts(path))
        {
            std::optional<ControlId> id;
            if (!part.empty()) id = TryCompileSinglePath(part);
            if (!id) return {};
            if (id->source == ControlSource::Scancode)
            {
                // The running layout's translation -- the exact call
                // InputDevices.cpp:143-144 makes when it fills a snapshot -- so
                // "<Keyboard>/scancode/space" and "<Keyboard>/space" meet at one
                // keycode. An untranslatable scancode stays a Scancode part.
                const SDL_Keycode kc = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(id->code), SDL_KMOD_NONE, false);
                if (kc != SDLK_UNKNOWN) id = ControlId{ ControlSource::Keycode, static_cast<uint32_t>(kc) };
            }
            keys.push_back(std::to_string(static_cast<int>(id->source)) + ':' + std::to_string(id->code));
        }
        std::sort(keys.begin(), keys.end());   // "a+b" == "b+a": ResolveChord needs every part down, order is spelling
        std::string out;
        for (const std::string& k : keys) { if (!out.empty()) out += '+'; out += k; }
        return out;
    }

    std::vector<InputControlChoice> InputActions::KnownControls()
    {
        std::vector<InputControlChoice> out;
        auto add = [&](std::string path) { out.push_back({ path, DisplayForPath(path) }); };
        // Keyboard: the LOVE-named table, letters, digits, F-keys -- exactly
        // what the Keyboard branch of TryCompileSinglePath accepts by name.
        for (const char* love : { "lshift", "rshift", "lctrl", "rctrl", "lalt", "ralt", "lgui", "rgui",
                                  "return", "escape", "grave", "space", "tab", "backspace",
                                  "up", "down", "left", "right" })
            add(std::string("<Keyboard>/") + love);
        for (char c = 'a'; c <= 'z'; ++c) add(std::string("<Keyboard>/") + c);
        for (char c = '0'; c <= '9'; ++c) add(std::string("<Keyboard>/") + c);
        for (int f = 1; f <= 12; ++f) add("<Keyboard>/f" + std::to_string(f));
        for (const char* m : { "leftButton", "rightButton", "middleButton", "button/4", "button/5" })
            add(std::string("<Mouse>/") + m);
        for (const char* b : { "buttonSouth", "buttonEast", "buttonWest", "buttonNorth", "dpadUp", "dpadDown",
                               "dpadLeft", "dpadRight", "leftShoulder", "rightShoulder", "start", "back", "guide",
                               "leftStickPress", "rightStickPress" })
            add(std::string("<Gamepad>/") + b);
        for (const char* a : { "leftStick/x", "leftStick/y", "rightStick/x", "rightStick/y", "leftTrigger", "rightTrigger" })
            add(std::string("<Gamepad>/") + a);
        add("<Gamepad>/leftStick");
        add("<Gamepad>/rightStick");
        return out;
    }
```

`ReadableControl`'s Keyboard branch resolves in this order: the LOVE table, single letters / F-keys, the SDL name lookup (`SDL_GetScancodeName(SDL_GetScancodeFromName(...))` for a captured scancode path, `SDL_GetKeyName(SDL_GetKeyFromName(...))` otherwise -- SDL's lookups are case-insensitive, so "left shift" resolves), then the camel-split fallback. InputActions.cpp already includes `<SDL3/SDL_keyboard.h>` / `<SDL3/SDL_keycode.h>` / `<SDL3/SDL_scancode.h>` (:7-9); `CanonicalControlKey`'s `SDL_GetKeyFromScancode` + `SDL_KMOD_NONE` need nothing more. The keyboard/gamepad name lists above MUST be the same spellings as `LoveToSdlName`'s `kTable`, `GamepadButtonToken`'s and `GamepadAxisToken`'s -- the test proves every entry compiles; to keep them from drifting, make those three tables `static constexpr` at namespace scope (they are function-local today) and iterate them here instead of repeating the literals (preferred; the literal form above is the fallback if the tables cannot be hoisted cleanly).

`BindingDisplayString` (:1001): replace the body after the composite branch with `const auto d = DisplayForPath(binding.path); return d.device.empty() ? d.control : d.device + " " + d.control;`.

`BindingValue`: add `InputSnapshot m_lastSnap{};` to the implementation class; at the top of `Update` (:1057) `m_lastSnap = snap;`; implement:

```cpp
            float BindingValue(const Guid& bindingId) const override
            {
                const auto it = m_bindingById.find(bindingId);
                if (it == m_bindingById.end()) return 0.0f;
                const CompiledBinding& binding = *it->second.binding;
                if (!binding.isComposite) return ResolveChord(binding.chord, m_lastSnap);
                float best = 0.0f;
                for (const auto& [role, parts] : binding.parts)
                    for (const CompiledBinding& part : parts)
                        best = std::max(best, std::abs(ResolveChord(part.chord, m_lastSnap)));
                return best;
            }
```

Read `m_bindingById`'s value type first (`:1001`'s `it->second.binding` shows it holds a pointer to the CompiledBinding); a composite PART may also be registered there under its own id (check `Bindings()`'s part enumeration at ~:990) -- if parts are registered, the simple branch already serves them.

`SplitChordParts` -- the '+' rule `CompilePath` hand-rolls at :284-302, hoisted. It goes in the FIRST anonymous namespace (the one holding `TryCompileSinglePath`), directly ABOVE `CompilePath`, so `CompilePath`, `IsKnownControlPath` and `CanonicalControlKey` all split through it -- one rule, never three:

```cpp
        // CompilePath's '+' rule: a '+' INSIDE '<...>' is not a separator. The
        // last part is always emitted ("" for an empty path or a trailing '+'),
        // so a caller that refuses empty parts refuses those.
        std::vector<std::string> SplitChordParts(std::string_view path)
        {
            std::vector<std::string> parts;
            std::string part;
            bool inAngle = false;
            for (char c : path)
            {
                if (c == '<') inAngle = true;
                else if (c == '>') inAngle = false;
                else if (c == '+' && !inAngle) { parts.push_back(part); part.clear(); continue; }
                part += c;
            }
            parts.push_back(part);
            return parts;
        }
```

`CompilePath` (:284-302) then becomes:

```cpp
        std::vector<ControlId> CompilePath(const std::string& path,
                                           const std::string& mapName,
                                           const std::string& actionName)
        {
            std::vector<ControlId> chord;
            for (const std::string& part : SplitChordParts(path))
                if (!part.empty())   // as today: an empty part is skipped, never zero-compiled
                    chord.push_back(CompileSinglePath(part, mapName, actionName));
            return chord;
        }
```

- [ ] **Step 4a: Rebind capture -- modifier chords** (`InputRebindOperation.cpp`)

`Observe` completes on the FIRST newly-down scancode today, so Shift-then-A writes `left shift` the instant Shift goes down although '+' chords are first-class in the compiler and in this task's own tests. Modifiers accumulate; the first non-modifier completes with them prefixed; a lone modifier binds on its release only when no other modifier is still held (UE SInputKeySelector's rule, written fresh). Prefix order = press order (display only: `ResolveChord` requires every part down). `Begin` clears `heldModifiers_` (Step 3), so modifiers already down at `Begin` are NOT seeded -- the initiating control must be released and re-pressed, the existing rule. Add the file-local helpers above `Begin` and replace the whole `Observe`:

```cpp
    namespace
    {
        // SDL_SCANCODE_LCTRL..SDL_SCANCODE_RGUI (224..231) -> the LOVE token the
        // path compiler's LoveToSdlName table round-trips ("<Keyboard>/scancode/
        // lshift" compiles to SDL_SCANCODE_LSHIFT). nullptr = not a modifier.
        const char* ModifierToken(uint32_t scancode)
        {
            switch (scancode)
            {
            case SDL_SCANCODE_LCTRL:  return "lctrl";
            case SDL_SCANCODE_LSHIFT: return "lshift";
            case SDL_SCANCODE_LALT:   return "lalt";
            case SDL_SCANCODE_LGUI:   return "lgui";
            case SDL_SCANCODE_RCTRL:  return "rctrl";
            case SDL_SCANCODE_RSHIFT: return "rshift";
            case SDL_SCANCODE_RALT:   return "ralt";
            case SDL_SCANCODE_RGUI:   return "rgui";
            default:                  return nullptr;
            }
        }
        bool IsModifierScancode(uint32_t scancode) { return ModifierToken(scancode) != nullptr; }
        bool AnyModifierDown(const InputSnapshot& snap)
        {
            for (uint32_t sc = SDL_SCANCODE_LCTRL; sc <= SDL_SCANCODE_RGUI; ++sc)
                if (snap.ScancodeDown(sc)) return true;
            return false;
        }
    }

    void InputRebindOperation::Observe(const InputSnapshot& snapshot, float dt)
    {
        if (result_.state != InputRebindState::Waiting) return;
        remaining_ -= std::max(0.0f, dt);
        if (remaining_ <= 0.0f)
        {
            result_.state = InputRebindState::TimedOut;
            return;
        }

        auto complete = [&](std::string path)
        {
            result_.replacementPath = std::move(path);
            result_.state = InputRebindState::Completed;
        };
        // The held modifiers as a chord prefix, press order, "" when none.
        auto heldPrefix = [&]
        {
            std::string prefix;
            for (uint32_t sc : heldModifiers_)
                prefix += std::string("<Keyboard>/scancode/") + ModifierToken(sc) + "+";
            return prefix;
        };

        if (!eligibleDevice_ || *eligibleDevice_ == InputDevice::Kbm)
        {
            // A key or button the UI has claimed this frame (an ImGui text
            // field, a hovered widget) is not a capture -- the same rule the
            // evaluator applies to wantCaptureKeyboard/Mouse.
            if (!snapshot.wantCaptureKeyboard)
            {
                // First pass, modifiers: every one newly down joins the held
                // list; every held one now up leaves it and, when it was the
                // last held and no other modifier is down, completes bare.
                for (uint32_t sc = SDL_SCANCODE_LCTRL; sc <= SDL_SCANCODE_RGUI; ++sc)
                    if (snapshot.ScancodeDown(sc) && !previous_.ScancodeDown(sc))
                        heldModifiers_.push_back(sc);
                for (auto it = heldModifiers_.begin(); it != heldModifiers_.end();)
                {
                    if (snapshot.ScancodeDown(*it)) { ++it; continue; }
                    const uint32_t released = *it;
                    it = heldModifiers_.erase(it);
                    if (heldModifiers_.empty() && !AnyModifierDown(snapshot))
                    {
                        complete(std::string("<Keyboard>/scancode/") + ModifierToken(released));
                        return;
                    }
                }
                // Second pass: the first NON-modifier newly down completes,
                // prefixed by whatever modifiers are held.
                for (uint32_t scancode = 1; scancode < 512; ++scancode)
                {
                    if (IsModifierScancode(scancode)) continue;
                    if (!snapshot.ScancodeDown(scancode) || previous_.ScancodeDown(scancode)) continue;
                    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
                    if (!name || !*name) continue;
                    std::string token(name);
                    std::transform(token.begin(), token.end(), token.begin(),
                        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    complete(heldPrefix() + "<Keyboard>/scancode/" + token);
                    return;
                }
            }
            static constexpr const char* mouseNames[] = {
                "leftButton", "rightButton", "middleButton", "button/4", "button/5" };
            for (uint8_t bit = 0; bit < 5; ++bit)
            {
                if (!snapshot.wantCaptureMouse
                    && (snapshot.mouseButtons & (1u << bit)) && !(previous_.mouseButtons & (1u << bit)))
                {
                    // "<Keyboard>/scancode/lshift+<Mouse>/leftButton" compiles:
                    // CompilePath splits on '+' and each part is a simple path.
                    complete(heldPrefix() + "<Mouse>/" + mouseNames[bit]);
                    return;
                }
            }
        }
        if ((!eligibleDevice_ || *eligibleDevice_ == InputDevice::Gamepad) && snapshot.gamepadConnected)
        {
            // Gamepad completions ignore held keyboard modifiers.
            static constexpr const char* buttonNames[] = {
                "buttonSouth", "buttonEast", "buttonWest", "buttonNorth",
                "dpadUp", "dpadDown", "dpadLeft", "dpadRight",
                "leftShoulder", "rightShoulder", "start", "back", "guide",
                "leftStickPress", "rightStickPress" };
            for (uint8_t bit = 0; bit < 15; ++bit)
            {
                if ((snapshot.gamepadButtons & (1u << bit)) && !(previous_.gamepadButtons & (1u << bit)))
                {
                    complete(std::string("<Gamepad>/") + buttonNames[bit]);
                    return;
                }
            }
            static constexpr const char* axisNames[] = {
                "leftStick/x", "leftStick/y", "rightStick/x", "rightStick/y",
                "leftTrigger", "rightTrigger" };
            for (uint8_t axis = 0; axis < 6; ++axis)
            {
                if (std::abs(snapshot.gamepadAxes[axis]) > 0.5f &&
                    std::abs(previous_.gamepadAxes[axis]) <= 0.5f)
                {
                    complete(std::string("<Gamepad>/") + axisNames[axis]);
                    return;
                }
            }
        }
        previous_ = snapshot;
    }
```

(`Begin` and `Cancel` are unchanged apart from Step 3's `heldModifiers_.clear();`. The file already includes `<SDL3/SDL_scancode.h>`, `<algorithm>`, `<cctype>`, `<cmath>`.)

- [ ] **Step 4b: LocalInputUser same-project re-Configure** (`LocalInputUser.hpp` / `.cpp`)

`Configure` (LocalInputUser.cpp:15-39) is the cold-start path: `Clear()` then `SetBaseMap(default)` + `LoadProfile` from disk, so the editor's save-triggered republish (Task 11) would collapse the pushed map stack, drop the scheme, cancel an in-flight capture, discard dirty overrides, and the fresh evaluator would re-fire a held control -- the opposite of spec B 2.6. The only callers today are `ProjectBoot.cpp:43` (boot: the first Configure, always cold) and `EditorAppFrame.cpp:2424` (the republish Task 11 rewrites), so Play > Stop > Play never re-enters this path and the cold branch keeps its exact behaviour.

Header: add `#include <vector>`; public, after `SetControlScheme`:

```cpp
        // The map on top of the stack: the base map when nothing is pushed,
        // nullopt before Configure. A same-project re-Configure replays it.
        [[nodiscard]] std::optional<Guid> ActiveMap() const noexcept
        { return mapStack_.empty() ? std::nullopt : std::optional<Guid>{ mapStack_.back() }; }
```

private, after `lastSnapshot_`:

```cpp
        std::vector<Guid> mapStack_;   // SetBaseMap = {map}; PushMap appends on success; PopMap pops. Replayed by a same-project re-Configure.
        std::string scheme_;           // the last SetControlScheme that succeeded ("" = none); re-applied by a same-project re-Configure
```

`.cpp` -- the four context wrappers keep the mirrors, `Clear` resets them (a cold start has no scheme and no stack), and `Configure` gains the re-entry branch BEFORE `Clear()`; the cold branch is byte-identical to today:

```cpp
    bool LocalInputUser::Configure(const InputActionAsset& asset, const Guid& projectId)
    {
        if (projectId.IsNil()) return false;
        auto next = InputActions::Create();
        if (!next->LoadAsset(asset)) return false;   // failure leaves the live session untouched
        if (asset_.has_value() && projectId == projectId_)
        {
            // RE-ENTRY (input-editor spec B 2.6): the editor saved THIS project's
            // asset while the session runs. Swap the evaluator in place and
            // carry the session across it.
            // Prime: prev/cur both hold the held state, so the first live tick
            // sees no edge; dt = 0 adds no hold time; no map is on the stack
            // yet, so no fixed-step transition is queued (UE: a key held across
            // a mapping rebuild is ignored until release).
            next->Update(0.0, lastSnapshot_);
            actions_ = std::move(next);
            asset_ = asset;
            // Replay the map stack by id: the first id that still resolves is
            // the base, later ones are pushed, vanished ids are skipped; none
            // left -> the asset's default map.
            const std::vector<Guid> stack = mapStack_;
            mapStack_.clear();
            for (const Guid& map : stack)
            {
                if (mapStack_.empty()) (void)SetBaseMap(map);
                else (void)PushMap(map);
            }
            if (mapStack_.empty() && asset.defaultMap) (void)SetBaseMap(*asset.defaultMap);
            // The scheme, while it still exists.
            if (!scheme_.empty() && !actions_->SetControlScheme(scheme_)) scheme_.clear();
            // Overrides: a dirty profile is the user's unsaved work -- keep it
            // and re-apply ("compatible overrides": one whose binding id
            // vanished fails SetBindingPath and drops with ApplyProfile's WARN).
            // A clean profile is re-read against the NEW asset (Load drops
            // stale ids itself). Not LoadProfile(): that recompiles the
            // evaluator and resets the base context, undoing the prime and the
            // replay above.
            if (!profile_.Dirty())
            {
                InputBindingProfile fresh;
                ProfileLoadResult loaded;
                if (!profileRoot_.empty())
                    loaded = fresh.Load(profileRoot_ / (profileName_ + ".json"), *asset_);
                if (loaded.status == ProfileLoadStatus::Invalid)
                    ARC_WARN("input: invalid {} binding profile for project {}", profileName_, projectId.ToString());
                else
                    profile_ = std::move(fresh);
            }
            ApplyProfile();
            rebind_ = {};   // a capture armed against the old evaluator is void
            return true;
        }
        Clear();
        actions_ = std::move(next);
        asset_ = asset;
        projectId_ = projectId;
        if (asset.defaultMap) (void)SetBaseMap(*asset.defaultMap);
        char* pref = SDL_GetPrefPath("Arcane", "Arcane");
        if (pref)
        {
            profileRoot_ = std::filesystem::path(reinterpret_cast<const char8_t*>(pref)) / "InputProfiles" / projectId.ToString();
            SDL_free(pref);
        }
        else
        {
            ARC_WARN("input: SDL_GetPrefPath failed; binding profiles cannot be saved");
        }
        const auto loaded = LoadProfile("Default");
        if (loaded.status == ProfileLoadStatus::Invalid)
            ARC_WARN("input: invalid Default binding profile for project {}", projectId.ToString());
        return true;
    }

    void LocalInputUser::Clear()
    {
        actions_ = InputActions::Create();
        asset_.reset();
        profile_ = {};
        rebind_ = {};
        lastSnapshot_ = {};
        projectId_ = Guid::Nil();
        profileRoot_.clear();
        profileName_ = "Default";
        reportedQueries_.clear();
        mapStack_.clear();
        scheme_.clear();
    }

    bool LocalInputUser::SetBaseMap(const Guid& map)
    {
        const auto name = MapName(map);
        if (!name) return false;
        actions_->SetBaseContext(*name);
        mapStack_.assign(1, map);
        return true;
    }
    bool LocalInputUser::PushMap(const Guid& map)
    {
        const auto name = MapName(map);
        if (!name) return false;
        actions_->PushContext(*name);
        mapStack_.push_back(map);
        return true;
    }
    void LocalInputUser::PopMap()
    {
        actions_->PopContext();                          // pops whatever is on top, the base included (InputActions.cpp:1133-1137)
        if (!mapStack_.empty()) mapStack_.pop_back();
    }
    bool LocalInputUser::SetControlScheme(std::string_view name)
    {
        if (!actions_->SetControlScheme(name)) return false;
        scheme_.assign(name);                            // "" clears the scheme in both
        return true;
    }
```

`LoadProfile`, `ImportProfile` and `ResetOverrides` still call `SetBaseMap(*asset_->defaultMap)` after their recompile, which now also resets `mapStack_` to the base -- the behaviour they had (their `SetBaseContext` already cleared the evaluator's stack), only the mirror follows. Owed (99-tail): the prime leaves `lastPressFrame`/`bufConsumed` residue so `Buffered(action, 6)` can report a held control for six live frames, and `heldTime` restarts for an in-progress Hold -- both need a small `InputActionsImpl` seam, not this task.

- [ ] **Step 4c: ABI 45 + the sample module**

(1) `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:972`: `kGamePluginABIVersion = 44` -> `45`, with the neighbours' changelog line above it: "v45 (2026-09-28, inspector-ownership/input-editor arc): `InputActions` gained the pure virtual `BindingValue`; `InputRebindOperation` gained `heldModifiers_`; `LocalInputUser` (held by value inside `ARCANE_API ClientRuntime`) gained `mapStack_`/`scheme_`. A v44 module was compiled against the old vtable and layouts; reject the pairing. ReferenceProject.arcproj restamped." (2) `ReferenceProject/ReferenceProject.arcproj:6`: `"abi": 44` -> `45` (as 04891dcd did). (3) BEFORE the root MSBuild of Step 5, rebuild the sample module against the new headers so the root postbuild stages a matching DLL -- `ReferenceGame.dll` bakes the ABI at ITS build, and the root premake's `{COPYDIR} ReferenceProject` postbuild then stages `Binaries/` beside each host; `Plugin.cpp:57-66` refuses a 44 DLL in a 45 host outright and the editor shows the Open Project Failed modal in every capture until the sample is rebuilt:

```bash
cd ReferenceProject && ../ThirdParty/premake5/premake5.exe vs2026 && "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" ReferenceProject.slnx -t:Rebuild -p:Configuration=Debug -m -nr:false -v:m -nologo && cd ..
```

(4) Then the root premake + MSBuild (Step 5). Verify: `ArcaneEditor.exe --project ReferenceProject --headless --frames 5` boots with no "Open Project Failed" modal, and `ArcaneEditor.exe --print-engine-info` (or the boot log) reports ABI 45. Aphelyon's restamp is already owed and absorbs this bump.

- [ ] **Step 5: Build and run** -- root MSBuild (after Step 4c's ReferenceProject rebuild); `./ArcaneTests.exe "[input]"` and `./ArcaneTests.exe "[client][input]"`. Expected: green including the eleven new cases (InputActionsTest: KnownControls, IsKnownControlPath, DisplayForPath, CanonicalControlKey, BindingValue; InputRebindOperationTest: Remaining + the three chord cases; ClientRuntimeTest: the two re-Configure cases) and the existing `BindingDisplayString` non-empty check; every pre-existing `[input]`, `[input][profile]` and `[client][input]` case still green (the cold Configure path is byte-identical).

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Input/InputActions.hpp ArcaneClient/src/Arcane/Input/InputActions.cpp ArcaneClient/src/Arcane/Input/InputRebindOperation.hpp ArcaneClient/src/Arcane/Input/InputRebindOperation.cpp ArcaneClient/src/Arcane/Input/LocalInputUser.hpp ArcaneClient/src/Arcane/Input/LocalInputUser.cpp ArcaneCore/src/Arcane/Plugin/PluginABI.hpp ReferenceProject/ReferenceProject.arcproj ArcaneTests/src/InputActionsTest.cpp ArcaneTests/src/InputRebindOperationTest.cpp ArcaneTests/src/ClientRuntimeTest.cpp
git commit -m "feat(input): KnownControls/IsKnownControlPath/DisplayForPath from the compiler's own tables, BindingValue for the editor's live glow, rebind Remaining(); LocalInputUser re-Configure for the same project replays the map stack, scheme and dirty overrides and primes the new evaluator (spec B 2.6); rebind capture accumulates modifiers into a '+' chord; CanonicalControlKey; ABI 45 + ReferenceProject restamped and rebuilt (input editor plan T5)"
```

---

### Task 6: Model growth -- selection epoch/key/restore/by-path, MoveRowTo, Conflicts, extended Warnings

**Files:**
- Modify: `ArcaneEditor/src/Documents/InputActionsEditorModel.hpp`, `ArcaneEditor/src/Documents/InputActionsEditorModel.cpp`
- Test: `ArcaneTests/src/InputActionsEditorModelTest.cpp`

**Interfaces:**
- Consumes: `InputActions::IsKnownControlPath`, `InputActions::CanonicalControlKey` (Task 5).
- Produces (on `InputActionsEditorModel`):
  ```cpp
  [[nodiscard]] std::uint64_t SelectionEpoch() const noexcept;      // bumps on EVERY Select* call with a valid id (a re-select is a gesture) and on a clear that changes something
  [[nodiscard]] std::string SelectionKey() const;                    // "<map>/<action>/<binding>/<part>" guid strings, empty segments for unset levels; "" when no map
  [[nodiscard]] bool RestoreSelection(std::string_view key);         // every non-empty segment must exist in the draft
  [[nodiscard]] bool Resolves(std::string_view key) const;           // PURE: would RestoreSelection(key) succeed? no selection, no bump
  [[nodiscard]] const nlohmann::json* FindNode(const Guid& id) const; // nullptr when no node carries that id (Task 10 PageFor uses it)
  [[nodiscard]] bool SelectByPath(std::string_view namePath);        // "<map name>[/<action name>[/<binding index>[/<part index>]]]"; false when a name segment matches more than one sibling (the runtime's ambiguity rule)
  [[nodiscard]] bool MoveRowTo(const Guid& id, std::size_t index);   // reorder within the row's own parent array (undoable)
  struct BindingConflict { Guid binding; Guid otherBinding; Guid otherAction; std::string otherActionName; std::string path; std::string group; };
  [[nodiscard]] std::vector<BindingConflict> Conflicts() const;      // one entry PER DIRECTION (a and b each get one); compares InputActions::CanonicalControlKey, never the spelling
  [[nodiscard]] std::vector<std::string> Warnings() const;           // invalid names + unknown paths (via the evaluator) + one line per conflicting PAIR
  // Name rules mirror the runtime's LoadAsset keys (InputActions.cpp:757-775): map names unique across the document, action names unique within their map; trimmed, case-sensitive. nullopt = acceptable; the unchanged name is acceptable.
  [[nodiscard]] static std::optional<std::string> ValidateName(const nlohmann::json& draft, const Guid& id, std::string_view proposed);
      // "Names cannot be blank" | "A map named 'X' already exists" | "Another action in this map is already named 'X'"
  [[nodiscard]] bool SiblingNameTaken(const Guid& id, std::string_view name) const;   // = ValidateName(draft_, id, name).has_value() && !blank
  // Called by an undo command after its weak anchor is checked (public beside RestoreDraft, the same seam):
  void RestoreSelectionOrAncestor(std::string_view key);              // undo/redo: SILENT (no epoch bump), trimmed to the deepest surviving ancestor
  // private: bool ParseKey(std::string_view, std::array<Guid,4>&) const; void SetSelectionSilently(const std::array<Guid,4>&);
  // file-local (the .cpp's anonymous namespace, beside DuplicateInArray which needs it): std::string UniqueSiblingName(const nlohmann::json& siblings, std::string base);  // base, "base 2", "base 3"... until no sibling's "name" equals it
  ```
  The four `Select*` setters move from the header to the `.cpp` (they bump the epoch). `SetField(id, "name", value)` trims the string and returns false (no edit, no undo entry) when `ValidateName` reports a reason. `AddMap(name)` / `AddAction(map, name)` pass their name through `UniqueSiblingName` (siblings = `actionMaps` / the map's `actions`), so "Action Map"/"Action" never collide; `DuplicateAction` and `DuplicateInArray`'s `" Copy"` branch use `UniqueSiblingName(siblings, name + " Copy")`. `DraftEditCommand` gains `undoKey_`/`redoKey_` and restores the selection live on undo/redo.

- [ ] **Step 1: Write the failing tests** (append to `InputActionsEditorModelTest.cpp`; `DocumentJson()` is the file's fixture; add `#include <algorithm>` for `std::count_if`/`std::any_of`)

First, one edit to the existing case "stable selection and one-step undo redo" (:30-49): insert `model.SelectMap(*Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222"));` directly before `model.SelectAction(selected);`. The undo restore below is key-based and a key needs a map; an action selected without its map is not a selection any producer makes (every document/row path selects the map first). Every other line of that case stays.

```cpp
TEST_CASE("input editor: selection epoch, key and restore", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto action = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    const auto binding = *Arcane::Guid::FromString("44444444-4444-4444-8444-444444444444");
    CHECK(model.SelectionKey().empty());
    const auto e0 = model.SelectionEpoch();
    model.SelectMap(map);
    CHECK(model.SelectionEpoch() == e0 + 1);
    model.SelectMap(map);                                  // re-select: a gesture, bumps
    CHECK(model.SelectionEpoch() == e0 + 2);
    model.SelectAction(action);
    model.SelectBinding(binding);
    CHECK(model.SelectionEpoch() == e0 + 4);
    const std::string key = model.SelectionKey();
    CHECK(key == map.ToString() + "/" + action.ToString() + "/" + binding.ToString() + "/");
    model.SelectMap({});
    CHECK(model.SelectionKey().empty());
    REQUIRE(model.RestoreSelection(key));
    CHECK(model.SelectedBinding() == binding);
    CHECK(model.SelectedAction() == action);
    REQUIRE(model.RemoveBinding(map, action, binding));
    CHECK_FALSE(model.Resolves(key));
    const auto eR = model.SelectionEpoch();
    CHECK_FALSE(model.RestoreSelection(key));              // the binding is gone: unresolvable
    CHECK(model.SelectionEpoch() == eR);                   // Resolves/RestoreSelection failure: pure
    CHECK(model.FindNode(action) != nullptr);
    CHECK(model.FindNode(binding) == nullptr);
    CHECK_FALSE(model.RestoreSelection("not-a-key"));
}

TEST_CASE("input editor: undo of a structural edit restores a live selection, silently", "[editor][input]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::CommandStack commands([&]() -> Astra::Registry& { return runtime.Registry(); });
    Arcane::Editor::InputActionsEditorModel model(DocumentJson(), &commands);
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto action = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    model.SelectMap(map); model.SelectAction(action);
    REQUIRE(model.AddBinding(map, action, "<Keyboard>/w"));      // selects the new binding
    const auto added = model.SelectedBinding();
    REQUIRE(added.IsValid());
    const auto e0 = model.SelectionEpoch();
    commands.Undo();                                              // the STACK, as EditorAppFrame.cpp:885 drives it
    CHECK_FALSE(model.SelectedBinding().IsValid());
    CHECK(model.SelectedAction() == action);
    CHECK(model.SelectedMap() == map);
    CHECK(model.SelectionEpoch() == e0);                          // silent: a mechanical change is not a gesture
    commands.Redo();
    CHECK(model.SelectedBinding() == added);
    REQUIRE(model.RemoveBinding(map, action, added));
    commands.Undo();
    CHECK(model.SelectedBinding() == added);                      // undo of a delete reselects what was live when it was deleted
}

TEST_CASE("input editor: SelectByPath resolves names and binding indices", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    REQUIRE(model.SelectByPath("Player"));
    CHECK(model.SelectedMap().ToString() == "22222222-2222-4222-8222-222222222222");
    CHECK_FALSE(model.SelectedAction().IsValid());
    REQUIRE(model.SelectByPath("Player/Jump"));
    CHECK(model.SelectedAction().ToString() == "33333333-3333-4333-8333-333333333333");
    REQUIRE(model.SelectByPath("Player/Jump/0"));
    CHECK(model.SelectedBinding().ToString() == "44444444-4444-4444-8444-444444444444");
    CHECK_FALSE(model.SelectByPath("Player/Crouch"));
    CHECK_FALSE(model.SelectByPath("Enemy"));
    CHECK_FALSE(model.SelectByPath("Player/Jump/7"));
    CHECK_FALSE(model.SelectByPath(""));
}

TEST_CASE("input editor: MoveRowTo reorders within the parent and is one undo step", "[editor][input]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::CommandStack commands([&]() -> Astra::Registry& { return runtime.Registry(); });
    Arcane::Editor::InputActionsEditorModel model(DocumentJson(), &commands);
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto action = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    REQUIRE(model.AddBinding(map, action, "<Keyboard>/w"));
    REQUIRE(model.AddBinding(map, action, "<Gamepad>/buttonSouth"));
    const auto& bindings = model.Draft()["actionMaps"][0]["actions"][0]["bindings"];
    const auto south = *Arcane::Guid::FromString(bindings[2]["id"].get<std::string>());
    REQUIRE(model.MoveRowTo(south, 0));
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["bindings"][0]["path"] == "<Gamepad>/buttonSouth");
    CHECK_FALSE(model.MoveRowTo(south, 0));                // already there: not an edit
    REQUIRE(model.MoveRowTo(south, 99));                   // clamps to the last slot
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["bindings"][2]["path"] == "<Gamepad>/buttonSouth");
    REQUIRE(model.Undo());
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["bindings"][0]["path"] == "<Gamepad>/buttonSouth");
    CHECK_FALSE(model.MoveRowTo(Arcane::Guid::Generate(), 0));
}

TEST_CASE("input editor: names are validated and kept unique among siblings", "[editor][input]")
{
    using M = Arcane::Editor::InputActionsEditorModel;
    M model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto jump = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    CHECK(M::ValidateName(model.Draft(), jump, "") == "Names cannot be blank");
    CHECK(M::ValidateName(model.Draft(), jump, "   ").has_value());
    CHECK_FALSE(M::ValidateName(model.Draft(), jump, "Jump").has_value());       // unchanged
    CHECK_FALSE(M::ValidateName(model.Draft(), jump, " Jump ").has_value());     // trimmed, unchanged
    REQUIRE(model.AddAction(map));  const auto a1 = model.SelectedAction();     // AddAction selects the new row (:274-286)
    REQUIRE(model.AddAction(map));  const auto a2 = model.SelectedAction();
    CHECK(model.FindNode(a1)->at("name") == "Action");
    CHECK(model.FindNode(a2)->at("name") == "Action 2");
    CHECK(M::ValidateName(model.Draft(), a2, "Jump") == "Another action in this map is already named 'Jump'");
    CHECK(model.SiblingNameTaken(a2, "Jump"));
    CHECK_FALSE(model.SiblingNameTaken(jump, "Jump"));                           // self
    const auto before = model.Draft();
    CHECK_FALSE(model.SetField(a2, "name", "Jump"));                             // refused: no edit
    CHECK(model.Draft() == before);
    REQUIRE(model.SetField(a2, "name", "  Crouch  "));
    CHECK(model.FindNode(a2)->at("name") == "Crouch");                           // trimmed
    // DuplicateAction inserts the copy right after the original (:230-233) and
    // does NOT select it: read the copy's name from the draft at index 1.
    REQUIRE(model.DuplicateAction(map, jump)); CHECK(model.Draft()["actionMaps"][0]["actions"][1]["name"] == "Jump Copy");
    REQUIRE(model.DuplicateAction(map, jump)); CHECK(model.Draft()["actionMaps"][0]["actions"][1]["name"] == "Jump Copy 2");
    REQUIRE(model.AddMap()); CHECK(model.FindNode(model.SelectedMap())->at("name") == "Action Map");   // AddMap selects the new map (:240-251)
    REQUIRE(model.AddMap()); CHECK(model.FindNode(model.SelectedMap())->at("name") == "Action Map 2");
    CHECK(M::ValidateName(model.Draft(), model.SelectedMap(), "Player") == "A map named 'Player' already exists");
    // A legacy file with two "Jump"s: Problems line + ambiguous SelectByPath refused.
    auto dup = DocumentJson();
    dup["actionMaps"][0]["actions"].push_back(nlohmann::json::parse(R"({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","name":"Jump","type":"Button","bindings":[]})"));
    M legacy(std::move(dup));
    CHECK(std::any_of(legacy.Warnings().begin(), legacy.Warnings().end(), [](const std::string& w) { return w.starts_with("Invalid name in Player/Jump"); }));
    CHECK_FALSE(legacy.SelectByPath("Player/Jump"));
}

TEST_CASE("input editor: Conflicts and Warnings -- same scheme, ungrouped-vs-grouped, spelling-independent, unknown paths", "[editor][input]")
{
    auto json = DocumentJson();
    json["controlSchemes"] = nlohmann::json::array({
        { {"id", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"}, {"name", "KeyboardMouse"}, {"bindingGroup", "KeyboardMouse"} },
        { {"id", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"}, {"name", "Gamepad"}, {"bindingGroup", "Gamepad"} } });
    auto& actions = json["actionMaps"][0]["actions"];
    actions[0]["bindings"][0]["groups"] = { "KeyboardMouse" };                        // Jump: space (KBM)
    // Parsed from text, not brace-initialised: nlohmann's initializer-list
    // rule turns a one-element list of pairs into an OBJECT, so a single-
    // binding "bindings" array written with braces would not be an array.
    actions.push_back(nlohmann::json::parse(R"JSON({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","name":"Crouch","type":"Button",
        "bindings":[{"id":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","path":"<Keyboard>/scancode/space"}]})JSON"));   // ungrouped space, in the CAPTURE's spelling
    actions.push_back(nlohmann::json::parse(R"JSON({"id":"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee","name":"Fire","type":"Button",
        "bindings":[{"id":"ffffffff-ffff-4fff-8fff-ffffffffffff","path":"<Keyboard>/space","groups":["Gamepad"]},
                    {"id":"12121212-1212-4121-8121-121212121212","path":"<Keyboard>/spaec"}]})JSON"));
    actions.push_back(nlohmann::json::parse(R"JSON({"id":"13131313-1313-4131-8131-131313131313","name":"Aim","type":"Button",
        "bindings":[{"id":"14141414-1414-4141-8141-141414141414","path":"<Mouse>/button/1"},
                    {"id":"15151515-1515-4151-8151-151515151515","path":"<Keyboard>/lshift+<Keyboard>/a"}]})JSON"));   // ungrouped
    actions.push_back(nlohmann::json::parse(R"JSON({"id":"16161616-1616-4161-8161-161616161616","name":"Block","type":"Button",
        "bindings":[{"id":"17171717-1717-4171-8171-171717171717","path":"<Mouse>/leftButton"},
                    {"id":"18181818-1818-4181-8181-181818181818","path":"<Keyboard>/a+<Keyboard>/lshift"}]})JSON"));   // ungrouped
    Arcane::Editor::InputActionsEditorModel model(std::move(json));
    REQUIRE(model.LastValidPreview());
    const auto conflicts = model.Conflicts();
    auto conflictsOf = [&](const char* id) {
        std::vector<std::string> others;
        for (const auto& c : conflicts) if (c.binding.ToString() == id) others.push_back(c.otherActionName);
        return others; };
    // Jump/space (KBM) vs Crouch/scancode/space (ungrouped): the same control, a conflict. Jump vs Fire (Gamepad group): NOT a conflict.
    CHECK(conflictsOf("44444444-4444-4444-8444-444444444444") == std::vector<std::string>{ "Crouch" });
    // Crouch (ungrouped) conflicts with BOTH grouped ones.
    const auto crouch = conflictsOf("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    CHECK(crouch.size() == 2);
    CHECK(conflictsOf("ffffffff-ffff-4fff-8fff-ffffffffffff") == std::vector<std::string>{ "Crouch" });
    CHECK(conflictsOf("14141414-1414-4141-8141-141414141414") == std::vector<std::string>{ "Block" });   // <Mouse>/button/1 IS <Mouse>/leftButton
    CHECK(conflictsOf("15151515-1515-4151-8151-151515151515") == std::vector<std::string>{ "Block" });   // chord parts in either order
    CHECK(conflictsOf("12121212-1212-4121-8121-121212121212").empty());                                  // 'spaec' compiles to nothing: conflicts with nothing
    const auto warnings = model.Warnings();
    CHECK(std::count_if(warnings.begin(), warnings.end(), [](const std::string& w) { return w.starts_with("Unknown control path"); }) == 1);
    CHECK(std::count_if(warnings.begin(), warnings.end(), [](const std::string& w) { return w.starts_with("Conflicting"); }) == 4);   // one per PAIR: Jump/Crouch, Fire/Crouch, Aim/Block mouse, Aim/Block chord
}
```

- [ ] **Step 2: Run to verify they fail** -- build. Expected: `SelectionEpoch` undeclared.

- [ ] **Step 3: Header** (`InputActionsEditorModel.hpp`) -- add `#include <array>`, `<cstdint>`, `<string_view>`. Replace the four inline `Select*` setters (:37, :39-41) with declarations and add the new members; the rest of the class is unchanged:

```cpp
        // Selection. Every Select* call with a VALID id bumps SelectionEpoch()
        // (a re-click on the selected row is a gesture: it re-asserts the
        // Inspector on this document); a clear bumps only when it changes
        // something. Undo/redo restore the selection SILENTLY (no bump).
        void SelectAction(const Guid& action) noexcept;
        void SelectMap(const Guid& map) noexcept;
        void SelectBinding(const Guid& binding) noexcept;
        void SelectPart(const Guid& part) noexcept;
        [[nodiscard]] std::uint64_t SelectionEpoch() const noexcept { return selectionEpoch_; }
        [[nodiscard]] std::string SelectionKey() const;                    // "<map>/<action>/<binding>/<part>"; "" when no map
        [[nodiscard]] bool RestoreSelection(std::string_view key);         // every non-empty segment must exist
        [[nodiscard]] bool Resolves(std::string_view key) const;           // PURE: would RestoreSelection succeed? no selection, no bump
        [[nodiscard]] const nlohmann::json* FindNode(const Guid& id) const; // nullptr when no node carries that id
        [[nodiscard]] bool SelectByPath(std::string_view namePath);        // "<map>[/<action>[/<binding index>[/<part index>]]]"
        [[nodiscard]] bool MoveRowTo(const Guid& id, std::size_t index);
        struct BindingConflict { Guid binding; Guid otherBinding; Guid otherAction; std::string otherActionName; std::string path; std::string group; };
        [[nodiscard]] std::vector<BindingConflict> Conflicts() const;
        // Name rules mirror the runtime's LoadAsset keys (InputActions.cpp:757-775):
        // map names unique across the document, action names unique within
        // their map; trimmed, case-sensitive. nullopt = acceptable (the
        // unchanged name always is).
        [[nodiscard]] static std::optional<std::string> ValidateName(const nlohmann::json& draft, const Guid& id, std::string_view proposed);
        [[nodiscard]] bool SiblingNameTaken(const Guid& id, std::string_view name) const;

        // Called by an undo command after its weak document anchor is checked.
        void RestoreDraft(const nlohmann::json& draft);
        void RestoreSelectionOrAncestor(std::string_view key);   // silent; trimmed to the deepest surviving ancestor

    private:
        void Validate();
        [[nodiscard]] bool ParseKey(std::string_view key, std::array<Guid, 4>& ids) const;
        void SetSelectionSilently(const std::array<Guid, 4>& ids);
        nlohmann::json draft_;
        nlohmann::json saved_;
        std::optional<InputActionAsset> preview_;
        std::vector<std::string> diagnostics_;
        Arcane::CommandStack* commands_ = nullptr;
        std::shared_ptr<InputActionsEditorModel*> anchor_;
        Guid selectedAction_;
        Guid selectedMap_;
        Guid selectedBinding_;
        Guid selectedPart_;
        std::uint64_t selectionEpoch_ = 0;
    };
```

(`Warnings()` keeps its existing declaration at :66; the existing `RestoreDraft` comment and declaration at :68-69 are the ones shown above; the private section is the existing one at :71-82 plus the two helpers and the epoch.)

- [ ] **Step 4: Implement** (`InputActionsEditorModel.cpp`; add `#include <Arcane/Input/InputActions.hpp>` for `IsKnownControlPath`/`CanonicalControlKey`, and `#include <array>`, `<cctype>`, `<optional>`, `<string_view>`)

`DraftEditCommand` (replace the class at :17-38) and `ApplyEdit` (:156-165):

```cpp
        class DraftEditCommand final : public Arcane::ICommand
        {
        public:
            DraftEditCommand(std::weak_ptr<InputActionsEditorModel*> anchor,
                             std::string label, nlohmann::json before,
                             nlohmann::json after, std::string undoKey)
                : anchor_(std::move(anchor)), label_(std::move(label)),
                  before_(std::move(before)), after_(std::move(after)),
                  undoKey_(std::move(undoKey)) {}
            // The selection is restored LIVE, trimmed to the deepest surviving
            // ancestor (a deleted binding falls back to its action), and
            // SILENTLY: undo/redo is a mechanical change, not a gesture, so
            // Ctrl+Z never steals the Inspector from the scene. The redo-side
            // key is captured AT UNDO TIME because callers such as AddBinding
            // select the new row only after ApplyEdit returns.
            void Undo() override
            {
                if (auto* m = Model()) { redoKey_ = m->SelectionKey(); m->RestoreDraft(before_); m->RestoreSelectionOrAncestor(undoKey_); }
            }
            void Redo() override
            {
                if (auto* m = Model()) { undoKey_ = m->SelectionKey(); m->RestoreDraft(after_); m->RestoreSelectionOrAncestor(redoKey_); }
            }
            const char* Label() const override { return label_.c_str(); }
        private:
            [[nodiscard]] InputActionsEditorModel* Model() const
            {
                const auto alive = anchor_.lock();
                return alive ? *alive : nullptr;
            }
            std::weak_ptr<InputActionsEditorModel*> anchor_;
            std::string label_;
            nlohmann::json before_;
            nlohmann::json after_;
            std::string undoKey_;
            std::string redoKey_;
        };
```

```cpp
    bool InputActionsEditorModel::ApplyEdit(std::string label, nlohmann::json before,
                                             nlohmann::json after)
    {
        if (before != draft_ || before == after) return false;
        std::string undoKey = SelectionKey();   // what was selected BEFORE the edit; ApplyEdit itself never touches the selection
        RestoreDraft(after);
        if (commands_)
            commands_->Push(std::make_unique<DraftEditCommand>(anchor_, std::move(label),
                                                                std::move(before), std::move(after),
                                                                std::move(undoKey)));
        return true;
    }
```

Setters, keys and lookup:

```cpp
    void InputActionsEditorModel::SelectMap(const Guid& map) noexcept
    {
        const bool changed = selectedMap_ != map || selectedAction_.IsValid() ||
                             selectedBinding_.IsValid() || selectedPart_.IsValid();
        selectedMap_ = map; selectedAction_ = {}; selectedBinding_ = {}; selectedPart_ = {};
        if (map.IsValid() || changed) ++selectionEpoch_;   // a re-select IS a gesture; a clear bumps only when it changes something
    }
    void InputActionsEditorModel::SelectAction(const Guid& action) noexcept
    { const bool changed = selectedAction_ != action; selectedAction_ = action; if (action.IsValid() || changed) ++selectionEpoch_; }
    void InputActionsEditorModel::SelectBinding(const Guid& binding) noexcept
    { const bool changed = selectedBinding_ != binding || selectedPart_.IsValid(); selectedBinding_ = binding; selectedPart_ = {}; if (binding.IsValid() || changed) ++selectionEpoch_; }
    void InputActionsEditorModel::SelectPart(const Guid& part) noexcept
    { const bool changed = selectedPart_ != part; selectedPart_ = part; if (part.IsValid() || changed) ++selectionEpoch_; }

    std::string InputActionsEditorModel::SelectionKey() const
    {
        if (!selectedMap_.IsValid()) return {};
        auto seg = [](const Guid& g) { return g.IsValid() ? g.ToString() : std::string{}; };
        return seg(selectedMap_) + "/" + seg(selectedAction_) + "/" + seg(selectedBinding_) + "/" + seg(selectedPart_);
    }

    bool InputActionsEditorModel::ParseKey(std::string_view key, std::array<Guid, 4>& ids) const
    {
        ids = {};
        std::size_t start = 0;
        for (std::size_t level = 0; level < 4; ++level)
        {
            const std::size_t slash = key.find('/', start);
            if (level < 3 && slash == std::string_view::npos) return false;
            const std::string_view seg = key.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
            if (!seg.empty())
            {
                const auto id = Guid::FromString(std::string(seg));
                if (!id || !id->IsValid() || !FindNode(*id)) return false;
                ids[level] = *id;
            }
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
        return ids[0].IsValid();
    }
    bool InputActionsEditorModel::Resolves(std::string_view key) const { std::array<Guid, 4> ids{}; return ParseKey(key, ids); }
    bool InputActionsEditorModel::RestoreSelection(std::string_view key)
    {
        std::array<Guid, 4> ids{};
        if (!ParseKey(key, ids)) return false;
        SelectMap(ids[0]);
        if (ids[1].IsValid()) SelectAction(ids[1]);
        if (ids[2].IsValid()) SelectBinding(ids[2]);
        if (ids[3].IsValid()) SelectPart(ids[3]);
        return true;
    }
    void InputActionsEditorModel::SetSelectionSilently(const std::array<Guid, 4>& ids)
    { selectedMap_ = ids[0]; selectedAction_ = ids[1]; selectedBinding_ = ids[2]; selectedPart_ = ids[3]; }   // NO epoch bump: undo/redo is not a gesture
    void InputActionsEditorModel::RestoreSelectionOrAncestor(std::string_view key)
    {
        // Lenient split (no existence check), then keep the deepest chain of
        // levels that still exist: a deleted binding falls back to its action.
        std::array<Guid, 4> raw{};
        std::size_t start = 0;
        for (std::size_t level = 0; level < 4 && start <= key.size(); ++level)
        {
            const std::size_t slash = key.find('/', start);
            const std::string_view seg = key.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
            if (!seg.empty()) raw[level] = Guid::FromString(std::string(seg)).value_or(Guid{});
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
        std::array<Guid, 4> keep{};
        for (std::size_t level = 0; level < 4; ++level)
        {
            if (!raw[level].IsValid() || !FindNode(raw[level])) break;
            keep[level] = raw[level];
        }
        SetSelectionSilently(keep);
    }
    const nlohmann::json* InputActionsEditorModel::FindNode(const Guid& id) const { return FindId(draft_, id); }
```

`FindId(json&, const Guid&)` is the file's existing non-const helper at :51 (used by `SetField`); add a const overload beside it, same body over const refs -- `FindNode` and `ParseKey` need it:

```cpp
        const nlohmann::json* FindId(const nlohmann::json& node, const Guid& id)
        {
            if (node.is_object())
            {
                if (node.contains("id") && node["id"].is_string() &&
                    node["id"].get<std::string>() == id.ToString()) return &node;
                for (const auto& [key, value] : node.items())
                    if (const auto* found = FindId(value, id)) return found;
            }
            else if (node.is_array())
                for (const auto& value : node)
                    if (const auto* found = FindId(value, id)) return found;
            return nullptr;
        }
```

`SelectByPath` (a name segment that matches more than one sibling is refused -- the runtime keys maps and actions by name and refuses such a file, InputActions.cpp:757-775):

```cpp
    bool InputActionsEditorModel::SelectByPath(std::string_view namePath)
    {
        std::vector<std::string> segs;
        for (std::size_t start = 0;;)
        {
            const std::size_t slash = namePath.find('/', start);
            segs.emplace_back(namePath.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
        if (segs.empty() || segs[0].empty() || !draft_.is_object() || !draft_.contains("actionMaps") || !draft_["actionMaps"].is_array())
            return false;
        auto idOf = [](const nlohmann::json& row) { return Guid::FromString(row.value("id", std::string{})).value_or(Guid{}); };
        // The ONE sibling named `name`; nullptr when none or more than one match.
        auto unique = [](const nlohmann::json& siblings, const std::string& name) -> const nlohmann::json* {
            const nlohmann::json* hit = nullptr;
            for (const auto& s : siblings)
            {
                if (!s.is_object() || s.value("name", std::string{}) != name) continue;
                if (hit) return nullptr;   // ambiguous
                hit = &s;
            }
            return hit; };
        const nlohmann::json* map = unique(draft_["actionMaps"], segs[0]);
        if (!map || !idOf(*map).IsValid()) return false;
        const nlohmann::json* action = nullptr;
        if (segs.size() > 1)
        {
            if (!map->contains("actions") || !(*map)["actions"].is_array()) return false;
            action = unique((*map)["actions"], segs[1]);
            if (!action || !idOf(*action).IsValid()) return false;
        }
        auto index = [](const std::string& s, const nlohmann::json& arr) -> const nlohmann::json* {
            if (s.empty() || !arr.is_array() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) return nullptr;
            const std::size_t i = std::stoul(s);
            return i < arr.size() ? &arr[i] : nullptr; };
        const nlohmann::json* binding = nullptr;
        if (segs.size() > 2)
        {
            binding = action->contains("bindings") ? index(segs[2], (*action)["bindings"]) : nullptr;
            if (!binding || !idOf(*binding).IsValid()) return false;
        }
        const nlohmann::json* part = nullptr;
        if (segs.size() > 3)
        {
            part = binding->contains("parts") ? index(segs[3], (*binding)["parts"]) : nullptr;
            if (!part || !idOf(*part).IsValid()) return false;
        }
        SelectMap(idOf(*map));
        if (action) SelectAction(idOf(*action));
        if (binding) SelectBinding(idOf(*binding));
        if (part) SelectPart(idOf(*part));
        return true;
    }

    bool InputActionsEditorModel::MoveRowTo(const Guid& id, std::size_t index)
    {
        auto next = draft_;
        return MoveToInArray(next, id, index) && ApplyEdit("Reorder input row", draft_, next);
    }
```

with, in the anonymous namespace beside `MoveInArray`:

```cpp
        bool MoveToInArray(nlohmann::json& node, const Guid& id, std::size_t index)
        {
            if (node.is_array())
            {
                for (std::size_t i = 0; i < node.size(); ++i)
                    if (node[i].is_object() && node[i].value("id", std::string{}) == id.ToString())
                    {
                        if (index >= node.size()) index = node.size() - 1;
                        if (index == i) return false;
                        nlohmann::json row = std::move(node[i]);
                        node.erase(node.begin() + static_cast<std::ptrdiff_t>(i));
                        node.insert(node.begin() + static_cast<std::ptrdiff_t>(index), std::move(row));
                        return true;
                    }
                for (auto& child : node) if (MoveToInArray(child, id, index)) return true;
            }
            else if (node.is_object())
                for (auto& [key, child] : node.items()) if (MoveToInArray(child, id, index)) return true;
            return false;
        }
```

Names. Two file-local helpers in the anonymous namespace, placed ABOVE `DuplicateInArray` (which uses the second):

```cpp
        std::string Trim(std::string_view s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
            return std::string(s);
        }

        // base, "base 2", "base 3"... until no sibling's "name" equals it: the
        // runtime keys maps and actions by name (InputActions.cpp:757-775), so
        // a default or a copy must never collide with a sibling.
        std::string UniqueSiblingName(const nlohmann::json& siblings, std::string base)
        {
            auto used = [&](const std::string& candidate) {
                if (!siblings.is_array()) return false;
                for (const auto& s : siblings)
                    if (s.is_object() && s.value("name", std::string{}) == candidate) return true;
                return false; };
            if (!used(base)) return base;
            for (int n = 2;; ++n)
            {
                const std::string candidate = base + " " + std::to_string(n);
                if (!used(candidate)) return candidate;
            }
        }
```

`DuplicateInArray` (:109-130): the `" Copy"` line becomes `copy["name"] = UniqueSiblingName(node, copy["name"].get<std::string>() + " Copy");` (`node` is the sibling array at that point). `DuplicateAction` (:232): `duplicate["name"] = UniqueSiblingName(map["actions"], duplicate.value("name", std::string("Action")) + " Copy");`. `AddMap` (:240-251): first line `name = Trim(name);`, then the existing empty/shape guard, and the pushed row's name becomes `UniqueSiblingName(next["actionMaps"], std::move(name))`. `AddAction` (:274-286): `name = Trim(name);` first, the pushed row's name becomes `UniqueSiblingName((*owner)["actions"], std::move(name))`. The validator, its query and the guarded `SetField`:

```cpp
    std::optional<std::string> InputActionsEditorModel::ValidateName(const nlohmann::json& draft, const Guid& id, std::string_view proposed)
    {
        const std::string name = Trim(proposed);
        if (name.empty()) return "Names cannot be blank";
        if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return std::nullopt;
        const std::string self = id.ToString();
        auto taken = [&](const nlohmann::json& siblings) {
            for (const auto& s : siblings)
                if (s.is_object() && s.value("id", std::string{}) != self && Trim(s.value("name", std::string{})) == name) return true;
            return false; };
        for (const auto& map : draft["actionMaps"])
        {
            if (!map.is_object()) continue;
            if (map.value("id", std::string{}) == self)
                return taken(draft["actionMaps"]) ? std::optional<std::string>("A map named '" + name + "' already exists") : std::nullopt;
            if (!map.contains("actions") || !map["actions"].is_array()) continue;
            for (const auto& action : map["actions"])
                if (action.is_object() && action.value("id", std::string{}) == self)
                    return taken(map["actions"]) ? std::optional<std::string>("Another action in this map is already named '" + name + "'") : std::nullopt;
        }
        return std::nullopt;   // a part role or a scheme: only the blank rule applies
    }

    bool InputActionsEditorModel::SiblingNameTaken(const Guid& id, std::string_view name) const
    {
        return !Trim(name).empty() && ValidateName(draft_, id, name).has_value();
    }

    bool InputActionsEditorModel::SetField(const Guid& id, std::string key, nlohmann::json value)
    {
        if (key == "id" || key.empty()) return false;
        if (key == "name")
        {
            // A name commits trimmed and validated: a refused name is no edit
            // and no undo entry (the rename box keeps it live, Task 8).
            if (!value.is_string()) return false;
            const std::string name = Trim(value.get<std::string>());
            if (ValidateName(draft_, id, name)) return false;
            value = name;
        }
        auto next = draft_;
        auto* node = FindId(next, id);
        if (!node) return false;
        (*node)[key] = std::move(value);
        return ApplyEdit("Edit " + key, draft_, next);
    }
```

Conflicts + Warnings (replacing the current `Warnings()` at :478-503). `Conflicts` compares the COMPILED control (`InputActions::CanonicalControlKey`, Task 5), never the spelling: the rebind capture writes `<Keyboard>/scancode/space` while assets author `<Keyboard>/space`, `<Mouse>/button/1` is `<Mouse>/leftButton`, and a chord's parts may come in either order. `BindingConflict::path` keeps the authored `entries[i].path` for the message/tooltip.

```cpp
    std::vector<InputActionsEditorModel::BindingConflict> InputActionsEditorModel::Conflicts() const
    {
        std::vector<BindingConflict> out;
        if (!preview_) return out;
        struct Entry { Guid id; Guid action; std::string actionName; std::string path; std::string key; std::vector<std::string> groups; };
        for (const auto& map : preview_->actionMaps)
        {
            std::vector<Entry> entries;
            for (const auto& action : map.actions)
                for (const auto& binding : action.bindings)
                {
                    if (binding.composite.empty())
                        entries.push_back({ binding.id, action.id, action.name, binding.path,
                                            InputActions::CanonicalControlKey(binding.path), binding.groups });
                    else
                        for (const auto& part : binding.parts)
                            entries.push_back({ part.id, action.id, action.name, part.path,
                                                InputActions::CanonicalControlKey(part.path),
                                                part.groups.empty() ? binding.groups : part.groups });
                }
            auto overlap = [](const Entry& a, const Entry& b) -> std::string {
                if (a.groups.empty() || b.groups.empty()) return "*";   // ungrouped = every scheme
                for (const auto& g : a.groups)
                    if (std::find(b.groups.begin(), b.groups.end(), g) != b.groups.end()) return g;
                return {}; };
            for (std::size_t i = 0; i < entries.size(); ++i)
                for (std::size_t j = i + 1; j < entries.size(); ++j)
                {
                    if (entries[i].key.empty() || entries[i].key != entries[j].key) continue;   // compare the COMPILED control, not the spelling: the rebind capture writes the scancode form while assets author the keycode form
                    const std::string group = overlap(entries[i], entries[j]);
                    if (group.empty()) continue;
                    out.push_back({ entries[i].id, entries[j].id, entries[j].action, entries[j].actionName, entries[i].path, group });
                    out.push_back({ entries[j].id, entries[i].id, entries[i].action, entries[i].actionName, entries[i].path, group });
                }
        }
        return out;
    }

    std::vector<std::string> InputActionsEditorModel::Warnings() const
    {
        std::vector<std::string> warnings;
        // Names first, from the DRAFT: a file loaded from disk with duplicate
        // names parses (FromJson keys ids, not names) but the runtime's
        // LoadAsset refuses it (InputActions.cpp:757-775) -- surface it in
        // Problems (Task 11 maps this prefix to `input.name.invalid`).
        if (draft_.is_object() && draft_.contains("actionMaps") && draft_["actionMaps"].is_array())
            for (const auto& map : draft_["actionMaps"])
            {
                if (!map.is_object()) continue;
                const Guid mapId = Guid::FromString(map.value("id", std::string{})).value_or(Guid{});
                const std::string mapName = map.value("name", std::string{});
                if (mapId.IsValid())
                    if (const auto why = ValidateName(draft_, mapId, mapName))
                        warnings.push_back("Invalid name in " + mapName + ": " + *why);
                if (!map.contains("actions") || !map["actions"].is_array()) continue;
                for (const auto& action : map["actions"])
                {
                    if (!action.is_object()) continue;
                    const Guid actionId = Guid::FromString(action.value("id", std::string{})).value_or(Guid{});
                    const std::string actionName = action.value("name", std::string{});
                    if (!actionId.IsValid()) continue;
                    if (const auto why = ValidateName(draft_, actionId, actionName))
                        warnings.push_back("Invalid name in " + mapName + "/" + actionName + ": " + *why);
                }
            }
        if (!preview_) return warnings;
        for (const auto& map : preview_->actionMaps)
            for (const auto& action : map.actions)
                for (const auto& binding : action.bindings)
                {
                    auto check = [&](const std::string& path) {
                        if (!InputActions::IsKnownControlPath(path))
                            warnings.push_back("Unknown control path '" + path + "' in " + map.name + "/" + action.name); };
                    if (binding.composite.empty()) check(binding.path);
                    else for (const auto& part : binding.parts) check(part.path);
                }
        std::set<std::pair<std::string, std::string>> seenPairs;
        for (const auto& c : Conflicts())
        {
            const auto a = c.binding.ToString(), b = c.otherBinding.ToString();
            if (!seenPairs.emplace(std::min(a, b), std::max(a, b)).second) continue;   // one line per pair
            warnings.push_back("Conflicting '" + c.path + "': " + c.otherActionName + " shares it" +
                               (c.group == "*" ? std::string(" in every scheme") : " in " + c.group));
        }
        return warnings;
    }
```

The old `Warnings()` reported "Conflicting" lines per (group, path) collision and "Unrecognized control path" on a missing '>'; the existing test at :180 only checks non-empty -- keep it green. The existing "map lifecycle" case (:138) adds "Player" then "Menus" to `CreateDefault()` (no maps), so unique naming leaves its names untouched; "action binding composite and scheme edits" (:156) sets `type`, never `name`.

- [ ] **Step 5: Build and run** -- `./ArcaneTests.exe "[editor][input]"`. Expected: green (six new cases; the existing "stable selection" case passes with its added `SelectMap`). Then `grep -n "SelectMap\|SelectAction\|SelectBinding\|SelectPart" ArcaneEditor/src/Documents/InputActionsEditorModel.hpp` shows declarations only (no inline bodies left).

- [ ] **Step 6: Commit**

```bash
git add ArcaneEditor/src/Documents/InputActionsEditorModel.hpp ArcaneEditor/src/Documents/InputActionsEditorModel.cpp ArcaneTests/src/InputActionsEditorModelTest.cpp
git commit -m "feat(editor): input model -- selection epoch (a re-select is a gesture), key/Resolves/restore/by-path, silent live selection restore on undo/redo, FindNode, MoveRowTo, unique + validated names, per-binding Conflicts on the compiled control, Warnings from the evaluator's own path check (input editor plan T6)"
```

---

### Task 7: Pure row builder for the actions column

**Files:**
- Create: `ArcaneEditor/src/Documents/InputActionsRows.hpp`, `ArcaneEditor/src/Documents/InputActionsRows.cpp`
- Modify: `premake5.lua` (the `ArcaneTests` `files` block: one new entry for `InputActionsRows.cpp`, added in Step 4)
- Test: `ArcaneTests/src/InputActionsRowsTest.cpp`

**Interfaces:**
- Consumes: `InputActions::DisplayForPath` (Task 5), `InputActionsEditorModel::BindingConflict` (Task 6).
- Produces:
  ```cpp
  enum class InputRowKind : std::uint8_t { Action, Binding, CompositeHeader, Part, AddBinding };
  struct InputRow { InputRowKind kind; Guid id; Guid actionId; Guid bindingId; int depth; std::string name; std::string badge; std::vector<std::string> groups; std::string detail; std::string device; std::string path; bool conflict; };
  struct InputRowFilter { std::string search; std::string schemeGroup; };
  [[nodiscard]] std::vector<InputRow> BuildInputRows(const nlohmann::json& draft, const Guid& map, const InputRowFilter& filter, const std::vector<InputActionsEditorModel::BindingConflict>& conflicts, const std::unordered_set<std::string>& collapsedActions);
  [[nodiscard]] std::optional<Guid> StepSelection(const std::vector<InputRow>& rows, const Guid& current, int direction);
  [[nodiscard]] std::string InteractionText(const nlohmann::json& interactions);   // "hold(duration=0.3)" -> "Hold 0.30 s"
  struct SiblingPosition { Guid parent; std::size_t index; std::size_t count; };
  [[nodiscard]] std::optional<SiblingPosition> SiblingIndex(const nlohmann::json& draft, const Guid& id);   // position of a row within its parent array
  ```

- [ ] **Step 1: Write the failing tests** (`InputActionsRowsTest.cpp`)

```cpp
// InputActionsRows (input-editor redesign spec s2.3): the PURE row list the
// actions column draws -- expanded bindings, composite header + parts,
// readable names, scheme badges, the search and scheme filters, collapse,
// keyboard stepping. No ImGui.
#include <catch2/catch_test_macros.hpp>
#include <Documents/InputActionsRows.hpp>
#include <Documents/InputActionsEditorModel.hpp>
#include <unordered_set>

using namespace Arcane::Editor;

namespace
{
    nlohmann::json Fixture()
    {
        return nlohmann::json::parse(R"JSON({
          "version":1,"id":"11111111-1111-4111-8111-111111111111",
          "controlSchemes":[{"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","name":"KeyboardMouse","bindingGroup":"KeyboardMouse"},
                            {"id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","name":"Gamepad","bindingGroup":"Gamepad"}],
          "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
            {"id":"33333333-3333-4333-8333-333333333333","name":"Move","type":"Axis1D","bindings":[
              {"id":"44444444-4444-4444-8444-444444444444","composite":"1DAxis","groups":["KeyboardMouse"],"parts":[
                {"id":"55555555-5555-4555-8555-555555555555","name":"negative","path":"<Keyboard>/scancode/a"},
                {"id":"66666666-6666-4666-8666-666666666666","name":"positive","path":"<Keyboard>/scancode/d"}]},
              {"id":"77777777-7777-4777-8777-777777777777","path":"<Gamepad>/leftStick/x","groups":["Gamepad"]}]},
            {"id":"88888888-8888-4888-8888-888888888888","name":"Jump","type":"Button","interactions":["hold(duration=0.3)"],"bindings":[
              {"id":"99999999-9999-4999-8999-999999999999","path":"<Keyboard>/space","groups":["KeyboardMouse"]},
              {"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","path":"<Gamepad>/buttonSouth"}]}
          ]}]})JSON");
    }
    const Arcane::Guid kMap = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
}

TEST_CASE("input rows: expanded bindings, composite header + parts, readable names, badges", "[editor][input]")
{
    const auto rows = BuildInputRows(Fixture(), kMap, {}, {}, {});
    REQUIRE(rows.size() == 10);
    CHECK(rows[0].kind == InputRowKind::Action);          CHECK(rows[0].name == "Move");   CHECK(rows[0].badge == "Axis1D"); CHECK(rows[0].detail.empty());
    CHECK(rows[1].kind == InputRowKind::CompositeHeader); CHECK(rows[1].name == "1D Axis"); CHECK(rows[1].badge == "KeyboardMouse"); CHECK(rows[1].depth == 1);
    CHECK(rows[2].kind == InputRowKind::Part);            CHECK(rows[2].name == "A");      CHECK(rows[2].detail == "1D Axis · negative"); CHECK(rows[2].device == "Keyboard"); CHECK(rows[2].depth == 2);
    CHECK(rows[2].bindingId.ToString() == "44444444-4444-4444-8444-444444444444");
    CHECK(rows[3].kind == InputRowKind::Part);            CHECK(rows[3].name == "D");
    CHECK(rows[4].kind == InputRowKind::Binding);         CHECK(rows[4].name == "Left Stick X"); CHECK(rows[4].device == "Gamepad"); CHECK(rows[4].badge == "Gamepad");
    CHECK(rows[5].kind == InputRowKind::AddBinding);      CHECK(rows[5].actionId.ToString() == "33333333-3333-4333-8333-333333333333");
    CHECK(rows[6].kind == InputRowKind::Action);          CHECK(rows[6].name == "Jump");   CHECK(rows[6].badge == "Button"); CHECK(rows[6].detail == "Hold 0.30 s");
    CHECK(rows[7].name == "Space");                       CHECK(rows[7].path == "<Keyboard>/space");
    CHECK(rows[8].name == "South Button");                CHECK(rows[8].badge.empty());   // ungrouped: no scheme badge
    CHECK(rows[9].kind == InputRowKind::AddBinding);
}

TEST_CASE("input rows: scheme filter hides other groups, ungrouped always shows; search narrows", "[editor][input]")
{
    InputRowFilter f; f.schemeGroup = "Gamepad";
    auto rows = BuildInputRows(Fixture(), kMap, f, {}, {});
    std::vector<std::string> names; for (const auto& r : rows) names.push_back(r.name);
    CHECK(names == std::vector<std::string>{ "Move", "Left Stick X", "", "Jump", "South Button", "" });
    f = {}; f.search = "SPACE";
    rows = BuildInputRows(Fixture(), kMap, f, {}, {});
    names.clear(); for (const auto& r : rows) names.push_back(r.name);
    CHECK(names == std::vector<std::string>{ "Jump", "Space", "" });
    f = {}; f.search = "mov";                                          // action name hit: every binding shows
    rows = BuildInputRows(Fixture(), kMap, f, {}, {});
    CHECK(rows.size() == 6);
    CHECK(rows[0].name == "Move");
}

TEST_CASE("input rows: collapse, conflicts, malformed drafts", "[editor][input]")
{
    std::unordered_set<std::string> collapsed{ "33333333-3333-4333-8333-333333333333" };
    auto rows = BuildInputRows(Fixture(), kMap, {}, {}, collapsed);
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].name == "Move");
    CHECK(rows[1].name == "Jump");
    InputRowFilter f; f.search = "stick";                                  // search overrides collapse
    rows = BuildInputRows(Fixture(), kMap, f, {}, collapsed);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].name == "Move"); CHECK(rows[1].name == "Left Stick X"); CHECK(rows[2].kind == InputRowKind::AddBinding);
    rows = BuildInputRows(Fixture(), kMap, {}, {}, collapsed);             // search cleared: the collapse returns
    CHECK(rows.size() == 5);
    std::vector<InputActionsEditorModel::BindingConflict> conflicts;
    conflicts.push_back({ *Arcane::Guid::FromString("99999999-9999-4999-8999-999999999999"), {}, {}, "Crouch", "<Keyboard>/space", "KeyboardMouse" });
    rows = BuildInputRows(Fixture(), kMap, {}, conflicts, {});
    CHECK(rows[7].conflict);
    CHECK_FALSE(rows[8].conflict);
    CHECK(BuildInputRows(nlohmann::json("not an object"), kMap, {}, {}, {}).empty());
    CHECK(BuildInputRows(nlohmann::json::object(), kMap, {}, {}, {}).empty());
    CHECK(BuildInputRows(Fixture(), Arcane::Guid::Generate(), {}, {}, {}).empty());
}

TEST_CASE("input rows: StepSelection walks selectable rows and skips the ghost rows", "[editor][input]")
{
    const auto rows = BuildInputRows(Fixture(), kMap, {}, {}, {});
    const auto stick = *Arcane::Guid::FromString("77777777-7777-4777-8777-777777777777");
    auto next = StepSelection(rows, stick, +1);
    REQUIRE(next); CHECK(next->ToString() == "88888888-8888-4888-8888-888888888888");   // Jump, not the AddBinding ghost
    auto prev = StepSelection(rows, stick, -1);
    REQUIRE(prev); CHECK(prev->ToString() == "66666666-6666-4666-8666-666666666666");
    CHECK(StepSelection(rows, *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333"), -1)->ToString() == "33333333-3333-4333-8333-333333333333");   // clamps at the top
    CHECK(StepSelection(rows, Arcane::Guid::Generate(), +1)->ToString() == "33333333-3333-4333-8333-333333333333");   // unknown: the first row
    CHECK_FALSE(StepSelection({}, stick, +1));
}

TEST_CASE("input rows: InteractionText and SiblingIndex", "[editor][input]")
{
    CHECK(InteractionText(nlohmann::json::array()) == "");
    CHECK(InteractionText(nlohmann::json::array({ "press" })) == "Press");
    CHECK(InteractionText(nlohmann::json::array({ "hold" })) == "Hold 0.40 s");
    CHECK(InteractionText(nlohmann::json::array({ "hold(duration=0.3)" })) == "Hold 0.30 s");
    CHECK(InteractionText(nlohmann::json::array({ "tap(duration=0.15)", "press" })) == "Tap 0.15 s · Press");
    CHECK(InteractionText(nlohmann::json("nope")) == "");
    const auto pos = SiblingIndex(Fixture(), *Arcane::Guid::FromString("cccccccc-cccc-4ccc-8ccc-cccccccccccc"));
    REQUIRE(pos);
    CHECK(pos->parent.ToString() == "88888888-8888-4888-8888-888888888888");
    CHECK(pos->index == 1); CHECK(pos->count == 2);
    const auto part = SiblingIndex(Fixture(), *Arcane::Guid::FromString("66666666-6666-4666-8666-666666666666"));
    REQUIRE(part); CHECK(part->parent.ToString() == "44444444-4444-4444-8444-444444444444"); CHECK(part->index == 1);
    CHECK_FALSE(SiblingIndex(Fixture(), Arcane::Guid::Generate()));
}

TEST_CASE("input rows: a binding in several schemes carries every group", "[editor][input]")
{
    const auto draft = nlohmann::json::parse(R"JSON({"version":1,"id":"11111111-1111-4111-8111-111111111111","controlSchemes":[],
      "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"P","actions":[
        {"id":"33333333-3333-4333-8333-333333333333","name":"Fire","type":"Button","bindings":[
          {"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/f","groups":["KeyboardMouse","Gamepad"]}]}]}]})JSON");
    const auto rows = BuildInputRows(draft, kMap, {}, {}, {});
    REQUIRE(rows.size() == 3);
    CHECK(rows[1].badge == "KeyboardMouse, Gamepad");
    REQUIRE(rows[1].groups.size() == 2);
    CHECK(rows[1].groups[1] == "Gamepad");
}
```

- [ ] **Step 2: Run to verify it fails** -- premake + build. Expected: header not found.

- [ ] **Step 3: Write InputActionsRows.hpp**

```cpp
#pragma once

// InputActionsRows (input-editor redesign spec s2.3): the actions column as
// PURE DATA -- the EntityList/BuildOutlinerRows pattern. The widgets draw
// what this returns; the tests pin what it returns. Readable control names
// come from the evaluator (InputActions::DisplayForPath), never from a
// second table here.

#include "Documents/InputActionsEditorModel.hpp"   // BindingConflict
#include <Arcane/Guid.hpp>
#include <Json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Arcane::Editor
{
    enum class InputRowKind : std::uint8_t { Action, Binding, CompositeHeader, Part, AddBinding };

    struct InputRow
    {
        InputRowKind kind = InputRowKind::Action;
        Guid id;          // the row's own id (AddBinding: the action's)
        Guid actionId;    // owning action
        Guid bindingId;   // Part: the composite it belongs to; Binding/CompositeHeader: == id
        int  depth = 0;   // 0 action, 1 binding / composite header / add-binding ghost, 2 part
        std::string name;    // action name | readable control | composite type
        std::string badge;   // action: type; binding/part/composite: scheme groups joined ", " ("" = ungrouped)
        std::vector<std::string> groups;   // one entry per scheme group (badge is the joined form, for search/tests); the widgets draw one tinted pill PER entry
        std::string detail;  // action: interaction text; part: "<composite type> · <role>" ("1D Axis · negative")
        std::string device;  // "Keyboard" | "Mouse" | "Gamepad" | ""
        std::string path;    // raw control path (binding/part), for the tooltip and the picker
        bool conflict = false;
    };

    struct InputRowFilter
    {
        std::string search;        // case-insensitive; actions by name, bindings by readable name or path
        std::string schemeGroup;   // "" = all schemes; otherwise hides bindings not in this group (ungrouped always show)
    };

    [[nodiscard]] std::vector<InputRow> BuildInputRows(
        const nlohmann::json& draft, const Guid& map, const InputRowFilter& filter,
        const std::vector<InputActionsEditorModel::BindingConflict>& conflicts,
        const std::unordered_set<std::string>& collapsedActions);   // action ids as strings

    // Up/Down over the selectable rows (every kind but AddBinding). Unknown
    // `current` lands on the first row; the ends clamp; empty rows -> nullopt.
    [[nodiscard]] std::optional<Guid> StepSelection(const std::vector<InputRow>& rows,
                                                    const Guid& current, int direction);

    // "hold(duration=0.3)" -> "Hold 0.30 s"; "press" -> "Press"; several
    // joined with " · ". Anything but an array of strings -> "".
    [[nodiscard]] std::string InteractionText(const nlohmann::json& interactions);

    struct SiblingPosition
    {
        Guid parent;        // the id of the object whose array holds the row (map for actions, action for bindings, composite for parts)
        std::size_t index;
        std::size_t count;
    };
    [[nodiscard]] std::optional<SiblingPosition> SiblingIndex(const nlohmann::json& draft, const Guid& id);
}
```

- [ ] **Step 4: Write InputActionsRows.cpp**

```cpp
#include "Documents/InputActionsRows.hpp"

#include <Arcane/Input/InputActions.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>   // std::strtof (InteractionText)

namespace Arcane::Editor
{
    namespace
    {
        Guid IdOf(const nlohmann::json& row)
        {
            if (!row.is_object() || !row.contains("id") || !row["id"].is_string()) return {};
            return Guid::FromString(row["id"].get<std::string>()).value_or(Guid{});
        }
        std::string Str(const nlohmann::json& row, const char* key)
        {
            return row.is_object() && row.contains(key) && row[key].is_string() ? row[key].get<std::string>() : std::string{};
        }
        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }
        bool Contains(const std::string& haystack, const std::string& needleLower)
        {
            return needleLower.empty() || Lower(haystack).find(needleLower) != std::string::npos;
        }
        std::vector<std::string> Groups(const nlohmann::json& row)
        {
            std::vector<std::string> out;
            if (row.is_object() && row.contains("groups") && row["groups"].is_array())
                for (const auto& g : row["groups"]) if (g.is_string()) out.push_back(g.get<std::string>());
            return out;
        }
        std::string Joined(const std::vector<std::string>& groups)
        {
            std::string s;
            for (const auto& g : groups) { if (!s.empty()) s += ", "; s += g; }
            return s;
        }
        bool InScheme(const std::vector<std::string>& groups, const std::string& scheme)
        {
            return scheme.empty() || groups.empty() || std::find(groups.begin(), groups.end(), scheme) != groups.end();
        }
        bool Conflicted(const std::vector<InputActionsEditorModel::BindingConflict>& conflicts, const Guid& id)
        {
            return std::any_of(conflicts.begin(), conflicts.end(), [&](const auto& c) { return c.binding == id; });
        }
        const nlohmann::json* FindMap(const nlohmann::json& draft, const Guid& map)
        {
            if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return nullptr;
            for (const auto& m : draft["actionMaps"]) if (IdOf(m) == map) return &m;
            return nullptr;
        }
    }

    std::string InteractionText(const nlohmann::json& interactions)
    {
        if (!interactions.is_array()) return {};
        std::string out;
        for (const auto& entry : interactions)
        {
            if (!entry.is_string()) continue;
            const std::string token = entry.get<std::string>();
            const std::size_t paren = token.find('(');
            const std::string name = token.substr(0, paren);
            float seconds = name == "hold" ? 0.4f : name == "tap" ? 0.2f : 0.0f;
            if (paren != std::string::npos)
                if (const std::size_t d = token.find("duration=", paren); d != std::string::npos)
                    seconds = std::strtof(token.c_str() + d + 9, nullptr);
            std::string text;
            if (name == "press") text = "Press";
            else if (name == "hold" || name == "tap")
            {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%s %.2f s", name == "hold" ? "Hold" : "Tap", seconds);
                text = buf;
            }
            else continue;
            if (!out.empty()) out += " · ";
            out += text;
        }
        return out;
    }

    std::vector<InputRow> BuildInputRows(const nlohmann::json& draft, const Guid& map, const InputRowFilter& filter,
                                         const std::vector<InputActionsEditorModel::BindingConflict>& conflicts,
                                         const std::unordered_set<std::string>& collapsedActions)
    {
        std::vector<InputRow> rows;
        const nlohmann::json* m = FindMap(draft, map);
        if (!m || !m->contains("actions") || !(*m)["actions"].is_array()) return rows;
        const std::string search = Lower(filter.search);

        auto bindingRow = [&](InputRowKind kind, const nlohmann::json& b, const Guid& actionId, const Guid& compositeId,
                              const std::vector<std::string>& groups, int depth)
        {
            InputRow r;
            r.kind = kind; r.id = IdOf(b); r.actionId = actionId;
            r.bindingId = kind == InputRowKind::Part ? compositeId : r.id;
            r.depth = depth; r.path = Str(b, "path");
            const InputControlDisplay d = InputActions::DisplayForPath(r.path);
            r.name = d.control; r.device = d.device;
            r.groups = groups; r.badge = Joined(groups);
            if (kind == InputRowKind::Part) r.detail = Str(b, "name");
            r.conflict = Conflicted(conflicts, r.id);
            return r;
        };

        for (const auto& action : (*m)["actions"])
        {
            const Guid actionId = IdOf(action);
            if (!actionId.IsValid()) continue;
            const std::string actionName = Str(action, "name");
            const bool actionHit = Contains(actionName, search);

            // Collect this action's rows first, so the search can decide
            // whether the action shows at all (any binding hit).
            std::vector<InputRow> children;
            bool anyBindingHit = false;
            if (action.contains("bindings") && action["bindings"].is_array())
                for (const auto& b : action["bindings"])
                {
                    if (!b.is_object()) continue;
                    const std::vector<std::string> groups = Groups(b);
                    if (!InScheme(groups, filter.schemeGroup)) continue;
                    if (b.contains("composite"))
                    {
                        InputRow header;
                        header.kind = InputRowKind::CompositeHeader; header.id = IdOf(b); header.actionId = actionId;
                        header.bindingId = header.id; header.depth = 1;
                        header.name = Str(b, "composite") == "1DAxis" ? "1D Axis" : "2D Vector";
                        header.groups = groups; header.badge = Joined(groups);
                        std::vector<InputRow> parts;
                        bool partHit = false;
                        if (b.contains("parts") && b["parts"].is_array())
                            for (const auto& p : b["parts"])
                            {
                                const auto pg = Groups(p);
                                if (!InScheme(pg.empty() ? groups : pg, filter.schemeGroup)) continue;
                                InputRow pr = bindingRow(InputRowKind::Part, p, actionId, header.id, pg.empty() ? groups : pg, 2);
                                pr.detail = header.name + " · " + pr.detail;   // "1D Axis · negative" (spec B 2.3); UTF-8 middle dot, the same literal as the "· actions" header
                                partHit |= Contains(pr.name, search) || Contains(pr.path, search);
                                parts.push_back(std::move(pr));
                            }
                        if (!actionHit && !search.empty() && !partHit) continue;
                        anyBindingHit |= partHit;
                        children.push_back(std::move(header));
                        for (auto& pr : parts) if (actionHit || search.empty() || Contains(pr.name, search) || Contains(pr.path, search)) children.push_back(std::move(pr));
                    }
                    else
                    {
                        InputRow br = bindingRow(InputRowKind::Binding, b, actionId, {}, groups, 1);
                        const bool hit = Contains(br.name, search) || Contains(br.path, search);
                        if (!actionHit && !search.empty() && !hit) continue;
                        anyBindingHit |= hit;
                        children.push_back(std::move(br));
                    }
                }
            if (!search.empty() && !actionHit && !anyBindingHit) continue;

            InputRow ar;
            ar.kind = InputRowKind::Action; ar.id = actionId; ar.actionId = actionId; ar.depth = 0;
            ar.name = actionName; ar.badge = Str(action, "type");
            ar.detail = action.contains("interactions") ? InteractionText(action["interactions"]) : std::string{};
            rows.push_back(std::move(ar));
            // A non-empty search overrides collapse: every surviving action draws
            // expanded so the matching binding is visible. collapsedActions itself
            // is untouched and returns when the search clears.
            if (search.empty() && collapsedActions.count(actionId.ToString())) continue;
            for (auto& c : children) rows.push_back(std::move(c));
            InputRow ghost;
            ghost.kind = InputRowKind::AddBinding; ghost.id = actionId; ghost.actionId = actionId; ghost.depth = 1;
            rows.push_back(std::move(ghost));
        }
        return rows;
    }

    std::optional<Guid> StepSelection(const std::vector<InputRow>& rows, const Guid& current, int direction)
    {
        std::vector<const InputRow*> selectable;
        for (const auto& r : rows) if (r.kind != InputRowKind::AddBinding) selectable.push_back(&r);
        if (selectable.empty()) return std::nullopt;
        std::ptrdiff_t at = -1;
        for (std::size_t i = 0; i < selectable.size(); ++i) if (selectable[i]->id == current) { at = static_cast<std::ptrdiff_t>(i); break; }
        if (at < 0) return selectable.front()->id;
        at = std::clamp<std::ptrdiff_t>(at + (direction < 0 ? -1 : 1), 0, static_cast<std::ptrdiff_t>(selectable.size()) - 1);
        return selectable[static_cast<std::size_t>(at)]->id;
    }

    std::optional<SiblingPosition> SiblingIndex(const nlohmann::json& draft, const Guid& id)
    {
        std::optional<SiblingPosition> found;
        auto visit = [&](auto&& self, const nlohmann::json& node, const Guid& parent) -> void
        {
            if (found) return;
            if (node.is_array())
            {
                for (std::size_t i = 0; i < node.size(); ++i)
                    if (IdOf(node[i]) == id) { found = SiblingPosition{ parent, i, node.size() }; return; }
                for (const auto& child : node) self(self, child, parent);
            }
            else if (node.is_object())
            {
                const Guid own = IdOf(node);
                for (const auto& [key, child] : node.items()) self(self, child, own.IsValid() ? own : parent);
            }
        };
        visit(visit, draft, Guid{});
        return found;
    }
}
```

Then register the new .cpp with the test exe. `ArcaneTests` lists editor sources one by one (`premake5.lua`, the `files {` block of `project "ArcaneTests"`, :1176-~1400 -- nothing globs them, so without this entry `InputActionsRowsTest.cpp` fails to link with LNK2019). Insert directly after `"%{wks.location}/ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp",` (:1208):

```lua
        -- Input editor T7: InputActionsRows (the actions column as pure rows)
        -- source-compiles here for InputActionsRowsTest, and the rewritten
        -- InputActionsDocumentWidgets.cpp above links against it.
        "%{wks.location}/ArcaneEditor/src/Documents/InputActionsRows.cpp",
```

Then `./ThirdParty/premake5/premake5.exe vs2026` before the build.

- [ ] **Step 5: Build and run** -- `./ArcaneTests.exe "[editor][input]"`. Expected: green. If `rows[1].badge` disagrees (composite badge), the fixture's composite carries `groups:["KeyboardMouse"]` -- the header's badge comes from the composite's own groups.

- [ ] **Step 6: Commit**

```bash
git add premake5.lua ArcaneEditor/src/Documents/InputActionsRows.hpp ArcaneEditor/src/Documents/InputActionsRows.cpp ArcaneTests/src/InputActionsRowsTest.cpp
git commit -m "feat(editor): InputActionsRows -- the actions column as pure rows (expanded bindings, composites as header + parts, readable names, scheme/search filters, stepping) (input editor plan T7)"
```

---
### Task 8: The document presentation -- toolbar, maps column, actions column (rows, context menus, scheme popup)

**Files:**
- Modify: `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp` (rewrite), `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp` (rewrite), `ArcaneEditor/src/Documents/InputActionsDocument.hpp`, `ArcaneEditor/src/Documents/InputActionsDocument.cpp`, `ArcaneEditor/src/Widgets/EditorWidgets.hpp:182` + `.cpp` (AssetPill variants 2/3)
- Modify (Step 0, Assets > Open as text): `ArcaneEditor/src/Panels/AssetPanelCommon.hpp:55-56`, `ArcaneEditor/src/Panels/AssetPanelCommon.cpp:334-335`, `ArcaneEditor/src/Panels/AssetBrowserPanel.cpp:1290-1291`, `ArcaneEditor/src/Panels/EditorPanels.hpp:94`, `ArcaneEditor/src/Panels/EditorPanels.cpp:292-294`, `ArcaneEditor/src/App/EditorAppFrame.cpp:174-193, :2589-2593, :2752-2755`
- Test: `ArcaneTests/src/InputActionsEditorModelTest.cpp` (the `SnapshotForCapture` case, Step 2)
- Desk: the headless capture (`--open-asset`) after each build.

**Interfaces:**
- Consumes: `BuildInputRows`, `StepSelection`, `SiblingIndex`, `InputRow::groups` (Task 7); the model (Task 6) including `ValidateName` and `FindNode`; `RowWithThumb`, `AssetPill`, `InputTextString` (EditorWidgets); `InputActions::BindingValue` (Task 5) through the document's preview; `InputRebindOperation` modifier chords (Task 5) as one `replacementPath`.
- Produces:
  ```cpp
  struct InputActionsDocumentState {   // the document's OWN view/tool state (spec A s3.4: never in the Inspector)
      char search[128] = {}; std::string schemeFilter; bool previewArmed = false;
      std::unordered_set<std::string> collapsedActions;   // a non-empty search overrides it without editing it
      Guid renameTarget; std::string renameBuf; bool renameFocusPending = false;   // swept every frame when the target's row is not drawable
      bool scrollToSelection = false;                     // consumed by the row that draws as selected (keyboard step, F2, a new row)
      enum class DragVerdict : std::uint8_t { None, Legal, Illegal };
      DragVerdict dragVerdict = DragVerdict::None, dragVerdictPrev = DragVerdict::None;   // hovered target writes; the source's preview reads last frame's
      bool schemePopupPending = false; char newSchemeName[64] = "Gamepad"; char newSchemeGroup[64] = "Gamepad";
  };
  struct InputActionsPreview {   // owned by the document; the widgets and the page borrow it
      std::unique_ptr<InputActions> evaluator; nlohmann::json source;
      void Sync(const InputActionsEditorModel& model);          // rebuild when LastValidPreview changed
      void Update(const InputSnapshot& raw);                    // raw = wantCapture cleared
      [[nodiscard]] InputActionValue Value(const Guid& action) const;
      [[nodiscard]] float BindingValue(const Guid& binding) const;
      [[nodiscard]] InputDevice ActiveDevice() const;
  };
  class InputActionsDocumentWidgets {
      struct Services { std::function<void(const Guid&)> beginRebind; std::function<bool(const Guid&)> isRebinding; std::function<float()> rebindRemaining; std::function<float(const Guid&)> glow;
                        std::function<bool()> inputSwallowed; };   // true while a capture is live and on its completing frame: keys and clicks belong to the capture
      void Draw(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services);
  };
  class InputActionsDocument {   // additions
      [[nodiscard]] static InputSnapshot SnapshotForCapture(const InputSnapshot& raw, bool anyItemActive);   // pure: wantCaptureMouse cleared, wantCaptureKeyboard = anyItemActive
      [[nodiscard]] const InputActionsDocumentState& State() const noexcept;
      [[nodiscard]] const InputActionsPreview& Preview() const noexcept;
  };
  struct AssetPanelActions { Arcane::Guid openAsText; };        // + MenuRequests::openAssetAsText; AssetPathAction(proj, guid, showInExplorer, copyPath, openAsText = false)
  ```
  A capture is cancelled when the document loses focus or its body does not draw; the completing input is consumed for that frame (`InputSwallowed()`), so the same-frame Delete/Enter/F2/arrow/click acts on nothing else.
  `AssetPill(text, variant)`: variant 2 = blue-grey (border `#3a4a5c`, text `#9fb3c8`) for the KeyboardMouse scheme, 3 = violet-grey (border `#4a3a5c`, text `#b8a3c8`) for every other scheme.

- [ ] **Step 0: Assets > Open as text** -- the repair banner in Step 2 names this command, so it must exist (spec B 2.1/s6: the JSON stays reachable). The editor's one-implementation-two-entry-points pattern (`AssetPathAction` + `AssetPanelActions`); the OS default handler opens the file until the in-editor text editor (mini-arc 4) lands.

(a) `ArcaneEditor/src/Panels/AssetPanelCommon.hpp:55-56`, the `AssetPanelActions` guid list:

```cpp
        Arcane::Guid createInstanceOf, createSpriteFrom, setBootScene,
                     showInExplorer, copyPath, copyGuid,
                     openAsText;   // Open as text: hand the asset file to the OS default editor (input editor spec s6)
```

(b) `AssetPanelCommon.cpp:334-335`, beside `Show in Explorer` in the row context menu:

```cpp
        if (ImGui::MenuItem("Show in Explorer"))
            actions.showInExplorer = e.guid;
        if (ImGui::MenuItem("Open as text"))
            actions.openAsText = e.guid;
```

and the preview-pane button column at `AssetBrowserPanel.cpp:1290-1291` (that block states it mirrors the context menu exactly):

```cpp
            if (ImGui::Button(ICON_LC_FOLDER_OPEN " Show in Explorer", btnSize))
                actions.showInExplorer = e->guid;
            if (ImGui::Button(ICON_LC_FILE_TEXT " Open as text", btnSize))
                actions.openAsText = e->guid;
```

(`ICON_LC_FILE_TEXT` is defined at `IconsLucide.h:754`.)

(c) `EditorAppFrame.cpp:174-193`, extend the shared helper:

```cpp
        void AssetPathAction(const Arcane::Project* proj, const Arcane::Guid& guid,
                             bool showInExplorer, bool copyPath, bool openAsText = false)
        {
            const auto assetPath = proj
                ? proj->ResolveAsset(Arcane::AssetId::FromGuid(guid))
                : std::nullopt;
            if (!assetPath)
            {
                ARC_WARN("Assets: the selected asset no longer resolves to a file");
                return;
            }
            if (showInExplorer)
            {
                // explorer /select opens the folder WITH the file focused.
                const std::wstring args = L"/select,\"" + assetPath->wstring() + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe",
                              args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            if (copyPath)
                ImGui::SetClipboardText(assetPath->string().c_str());
            if (openAsText)
            {
                // The OS default handler for the file (a .arcinput is JSON: the
                // user's text editor). No SDL_OpenURL: a file path, not a URL.
                ShellExecuteW(nullptr, L"open", assetPath->wstring().c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
```

The row-context-menu dispatch at `:2752-2755` gains a third line:

```cpp
        if (panelActions.showInExplorer.IsValid())
            AssetPathAction(m_runtime->CurrentProject(), panelActions.showInExplorer, true, false);
        if (panelActions.copyPath.IsValid())
            AssetPathAction(m_runtime->CurrentProject(), panelActions.copyPath, false, true);
        if (panelActions.openAsText.IsValid())
            AssetPathAction(m_runtime->CurrentProject(), panelActions.openAsText, false, false, true);
```

Menu-bar route: `EditorPanels.hpp:94` `MenuRequests` gains

```cpp
        bool copyAssetPath = false;    // Assets -> Copy Path        (on the browser's tracked row)
        bool openAssetAsText = false;  // Assets -> Open as text     (on the browser's tracked row)
```

`EditorPanels.cpp:292-294`:

```cpp
                if (ImGui::MenuItem("Show in Explorer", nullptr, false, hasAssetSelection))
                    requests.showInExplorer = true;
                if (ImGui::MenuItem("Copy Path", nullptr, false, hasAssetSelection))
                    requests.copyAssetPath = true;
                if (ImGui::MenuItem("Open as text", nullptr, false, hasAssetSelection))
                    requests.openAssetAsText = true;
```

`EditorAppFrame.cpp:2589-2593`:

```cpp
        if ((menuReq.showInExplorer || menuReq.copyAssetPath || menuReq.openAssetAsText) &&
            m_assetModel.selected.IsValid())
        {
            AssetPathAction(m_runtime->CurrentProject(), m_assetModel.selected,
                            menuReq.showInExplorer, menuReq.copyAssetPath, menuReq.openAssetAsText);
        }
```

Build (editor only; no test touches these) before moving on.

- [ ] **Step 1: AssetPill variants** -- in `EditorWidgets.cpp`'s `AssetPill` (`grep -n "void AssetPill" ArcaneEditor/src/Widgets/EditorWidgets.cpp`), extend the `variant` switch: 2 -> border `IM_COL32(0x3a,0x4a,0x5c,255)`, text `IM_COL32(0x9f,0xb3,0xc8,255)`; 3 -> border `IM_COL32(0x4a,0x3a,0x5c,255)`, text `IM_COL32(0xb8,0xa3,0xc8,255)`. Update the header comment at `EditorWidgets.hpp:179-181` ("2 = blue-grey scheme tint, 3 = violet-grey scheme tint (input editor spec s2.3)").

- [ ] **Step 2: The document owns capture + preview** (`InputActionsDocument.hpp`)

Test first -- append to `ArcaneTests/src/InputActionsEditorModelTest.cpp` (it already includes `Documents/InputActionsDocument.hpp`; `InputActionsDocument.cpp` is already in the ArcaneTests `files` block, premake5.lua:1207). ImGui's `WantCaptureMouse` is true over EVERY editor window, so it must never gate a capture; the keyboard keeps ActiveId semantics (a text field being typed into still claims keys):

```cpp
TEST_CASE("input document: the capture snapshot ignores ImGui's mouse claim and keeps the keyboard's ActiveId claim", "[editor][input]")
{
    Arcane::InputSnapshot raw; raw.mouseButtons = 0x2; raw.wantCaptureMouse = true; raw.wantCaptureKeyboard = false;
    const auto s = Arcane::Editor::InputActionsDocument::SnapshotForCapture(raw, false);
    CHECK_FALSE(s.wantCaptureMouse); CHECK_FALSE(s.wantCaptureKeyboard); CHECK(s.mouseButtons == 0x2);
    CHECK(Arcane::Editor::InputActionsDocument::SnapshotForCapture(raw, true).wantCaptureKeyboard);
}
```

Build: `SnapshotForCapture` undeclared -- the expected red.

Replace the public additions + the private section with this ONE final shape (the lines marked Task 10 / Task 11 are the members those tasks declare -- `InputActionsInspectorPage` does not exist until Task 10 -- so Task 8 writes the section without those marked lines and each later task adds its own, converging on this text):

```cpp
    public:
        // ... existing public API, plus:
        // The snapshot a rebind capture observes: the document is the sole
        // claimant of the pointer while a capture is live (ImGui's
        // WantCaptureMouse is true over EVERY editor window, so it must not
        // gate the capture); the keyboard keeps ActiveId semantics (a text
        // field being typed into still claims keys). Pure; tested.
        [[nodiscard]] static InputSnapshot SnapshotForCapture(const InputSnapshot& raw, bool anyItemActive);
        [[nodiscard]] const InputActionsDocumentState& State() const noexcept { return state_; }
        [[nodiscard]] const InputActionsPreview& Preview() const noexcept { return preview_; }
        void SetOnSaved(std::function<void(const Guid&, const InputActionAsset&)> fn) { onSaved_ = std::move(fn); }   // Task 11
    private:
        InputActionsDocument(std::filesystem::path path, nlohmann::json draft, Arcane::CommandStack* commands);
        void SelectFirstMapAndAction();
        void TickCapture(bool bodyDrawn);
        void BeginRebind(const Guid& target);
        // True while a capture is live and on the frame it completed/cancelled:
        // keys and clicks belong to the capture (UE consumes the heard key at
        // the selector; ImGui has no event consumption, so the frame stamp does).
        [[nodiscard]] bool InputSwallowed() const noexcept { return captureTarget_.IsValid() || captureSwallowFrame_ == ImGui::GetFrameCount(); }
        void PublishWarnings();   // Task 11

        std::filesystem::path path_;
        std::string title_;
        std::string windowLabel_;
        Guid guid_;
        InputActionsEditorModel model_;
        InputActionsDocumentState state_;
        InputActionsDocumentWidgets widgets_;
        InputActionsPreview preview_;
        InputSnapshot previewSnapshot_{};
        InputRebindOperation capture_;
        Guid captureTarget_;
        int captureSwallowFrame_ = -1;
        bool focused_ = false;
        InputActionsInspectorPage page_;                                          // Task 10 (constructed after model_/state_/preview_)
        std::function<void(const Guid&, const InputActionAsset&)> onSaved_;         // Task 11
        std::string diagKey_;                                                     // Task 11: "input:" + guid
        std::vector<std::string> publishedWarnings_;                              // Task 11
```

Delete `RefreshText`, `text_` and the `<array>` include (the JSON tab is gone) and the old `TextDisabled("Unsaved changes"/"Saved")` line (current .cpp:106): the `UnsavedDocument` flag below is the only dirty affordance (spec B 2.1). Includes in the .hpp: `<Arcane/Input/InputRebindOperation.hpp>`, `<imgui.h>` (`InputSwallowed()` reads `ImGui::GetFrameCount()` inline), `<functional>`, `<vector>`. Includes in `InputActionsDocument.cpp`: add `#include "Widgets/EditorTheme.hpp"` and `#include "Widgets/IconsLucide.h"` (the banner uses `Theme::kAmber` and `ICON_LC_TRIANGLE_ALERT`). Public: keep `SetPreviewSnapshot`; `State()` and `Preview()` are what Task 10's page reads.

`InputActionsDocument.cpp` -- FINAL for these four functions (the `PublishWarnings();` line is Task 11's: Task 8 writes `Draw` without it):

```cpp
    InputSnapshot InputActionsDocument::SnapshotForCapture(const InputSnapshot& raw, bool anyItemActive)
    {
        InputSnapshot s = raw;
        s.wantCaptureMouse = false;
        s.wantCaptureKeyboard = anyItemActive;
        return s;
    }

    void InputActionsDocument::BeginRebind(const Guid& target)
    {
        if (!target.IsValid()) return;
        captureTarget_ = target;
        capture_.Begin(target, std::nullopt, 10.0f, previewSnapshot_);   // any device; the initiating control is not a capture (existing rule)
    }

    void InputActionsDocument::TickCapture(bool bodyDrawn)
    {
        if (!captureTarget_.IsValid()) return;
        captureSwallowFrame_ = ImGui::GetFrameCount();   // every frame the capture is live, INCLUDING the completing/cancelling one
        // A capture is bound to the focused, visible document (UE's
        // SInputKeySelector ends selection on focus loss): a hidden tab, a
        // collapsed window or a click into the Viewport/Inspector cancels it, so
        // a key typed elsewhere can never land in a binding and the timeout
        // cannot freeze while the tab is hidden.
        if (!bodyDrawn || !focused_) capture_.Cancel();
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) capture_.Cancel();
        else capture_.Observe(SnapshotForCapture(previewSnapshot_, ImGui::IsAnyItemActive()),
                              ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : 1.0f / 60.0f);
        const auto& result = capture_.Result();
        if (result.state == InputRebindState::Completed)
        {
            (void)model_.SetField(captureTarget_, "path", result.replacementPath);   // ONE undoable edit
            captureTarget_ = {};
        }
        else if (result.state == InputRebindState::Canceled || result.state == InputRebindState::TimedOut)
            captureTarget_ = {};
    }

    void InputActionsDocument::Draw(bool& requestClose)
    {
        bool open = true;
        // Tab dot = polled model state each frame (MeshDocument/SpriteDocument/
        // ShaderEditorDocument pattern): undo back to the saved revision clears
        // it with no bookkeeping.
        const ImGuiWindowFlags flags = Dirty() ? ImGuiWindowFlags_UnsavedDocument : 0;
        const bool bodyDrawn = ImGui::Begin(windowLabel_.c_str(), &open, flags);
        focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);   // valid on both branches
        TickCapture(bodyDrawn);                                                      // BEFORE the shortcut: the swallow stamp is written here
        if (bodyDrawn)
        {
            if (!InputSwallowed() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))
                if (!Save()) ARC_WARN("InputActionsDocument: save refused for '{}'", path_.generic_string());
            preview_.Sync(model_);
            if (state_.previewArmed)
            {
                InputSnapshot raw = previewSnapshot_;
                raw.wantCaptureKeyboard = false;
                raw.wantCaptureMouse = false;
                preview_.Update(raw);
            }
            if (!model_.Diagnostics().empty())
            {
                // The draft is not a valid asset: say so above the columns (the
                // columns draw what they can; a missing actionMaps draws nothing).
                ImGui::PushStyleColor(ImGuiCol_Text, Arcane::Editor::Theme::kAmber);
                ImGui::TextUnformatted(ICON_LC_TRIANGLE_ALERT);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextWrapped("%s -- fix it in a text editor (Assets > Open as text), then reopen.",
                                   model_.Diagnostics().front().c_str());
                ImGui::Separator();
            }
            InputActionsDocumentWidgets::Services services;
            services.beginRebind     = [this](const Guid& id) { BeginRebind(id); };
            services.isRebinding     = [this](const Guid& id) { return captureTarget_ == id; };
            services.rebindRemaining = [this] { return capture_.Remaining(); };
            services.glow            = [this](const Guid& id) { return state_.previewArmed ? preview_.BindingValue(id) : 0.0f; };
            services.inputSwallowed  = [this] { return InputSwallowed(); };
            widgets_.Draw(model_, state_, services);
            PublishWarnings();   // Task 11
        }
        ImGui::End();
        requestClose = !open;
    }
```

A modifier chord arrives as ONE `replacementPath` (`<Keyboard>/scancode/lshift+<Keyboard>/scancode/a`, Task 5's `InputRebindOperation`): `TickCapture` stores it unchanged through `SetField`, and the row's `DisplayForPath` renders it as `Left Shift + A`.

`InputActionsPreview` -- the document-level preview evaluator the widgets and Task 10's page borrow -- is declared in `InputActionsDocumentWidgets.hpp`; Step 3 carries its full text.

- [ ] **Step 3: Rewrite InputActionsDocumentWidgets.hpp**

```cpp
#pragma once

// The Input Actions document's presentation (input-editor redesign spec s2):
// toolbar (+ Add, search, scheme filter, Preview), maps column, actions
// column drawn from InputActionsRows. View/tool state lives in
// InputActionsDocumentState (spec A s3.4 -- the document's own toolbar,
// never the Inspector); every mutation goes through the model (undoable).
// The asset model stays independent of ImGui.

#include "Documents/InputActionsEditorModel.hpp"
#include "Documents/InputActionsRows.hpp"
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

namespace Arcane::Editor
{
    struct InputActionsDocumentState
    {
        char search[128] = {};
        std::string schemeFilter;                     // bindingGroup; "" = All schemes
        bool previewArmed = false;
        std::unordered_set<std::string> collapsedActions;   // action ids; a non-empty search overrides it without editing it
        Guid renameTarget;                            // inline rename (F2 / context menu); swept every frame when its row is not drawable
        std::string renameBuf;
        bool renameFocusPending = false;
        bool scrollToSelection = false;               // consumed by the row that draws as selected (keyboard step, F2, a new row)
        enum class DragVerdict : std::uint8_t { None, Legal, Illegal };
        DragVerdict dragVerdict = DragVerdict::None;      // written by the hovered drop target this frame
        DragVerdict dragVerdictPrev = DragVerdict::None;  // read by the drag source's preview (one frame behind)
        bool schemePopupPending = false;              // opened at window scope (a popup cannot open from inside another)
        char newSchemeName[64] = "Gamepad";
        char newSchemeGroup[64] = "Gamepad";
    };

    // The document-level preview evaluator (owned by InputActionsDocument;
    // the widgets and the Inspector page borrow it): rebuilt whenever the
    // model's LastValidPreview changes, fed the raw snapshot while the
    // toolbar's Preview is armed.
    struct InputActionsPreview
    {
        std::unique_ptr<InputActions> evaluator;
        nlohmann::json source;
        void Sync(const InputActionsEditorModel& model)
        {
            if (!model.LastValidPreview()) return;
            const nlohmann::json next = model.LastValidPreview()->ToJson();
            if (evaluator && source == next) return;
            evaluator = InputActions::Create();
            if (!evaluator->LoadAsset(*model.LastValidPreview())) evaluator.reset();
            source = next;
        }
        void Update(const InputSnapshot& raw) { if (evaluator) evaluator->Update(1.0 / 60.0, raw); }
        [[nodiscard]] InputActionValue Value(const Guid& action) const { return evaluator ? evaluator->Value(action) : InputActionValue{}; }
        [[nodiscard]] float BindingValue(const Guid& binding) const { return evaluator ? evaluator->BindingValue(binding) : 0.0f; }
        [[nodiscard]] InputDevice ActiveDevice() const { return evaluator ? evaluator->ActiveDevice() : InputDevice::Kbm; }
    };

    class InputActionsDocumentWidgets
    {
    public:
        struct Services
        {
            std::function<void(const Guid&)> beginRebind;
            std::function<bool(const Guid&)> isRebinding;
            std::function<float()> rebindRemaining;
            std::function<float(const Guid&)> glow;   // 0 = off
            std::function<bool()> inputSwallowed;     // true while a capture is live and on its completing frame: keys and clicks belong to the capture
        };
        void Draw(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services);

    private:
        using Edit = std::function<void()>;
        void DrawToolbar(InputActionsEditorModel& model, InputActionsDocumentState& state, Edit& edit);
        void DrawMaps(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services, Edit& edit);
        void DrawActions(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services, Edit& edit);
        void DrawRow(const InputRow& row, InputActionsEditorModel& model, InputActionsDocumentState& state,
                     const Services& services, Edit& edit, const std::vector<InputRow>& rows);
        void DrawSchemePopup(InputActionsEditorModel& model, InputActionsDocumentState& state, Edit& edit);
        // Task 9 fills both; Task 8 gives them empty bodies. Each runs inside its
        // own column child, so IsWindowFocused(ChildWindows) routes the keys to
        // whichever column has focus.
        void HandleKeys(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services,
                        Edit& edit, const std::vector<InputRow>& rows);
        void HandleMapKeys(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services, Edit& edit);
        static void SelectRow(InputActionsEditorModel& model, const InputRow& row);
        [[nodiscard]] static bool RowSelected(const InputActionsEditorModel& model, const InputRow& row);
    };
}
```

- [ ] **Step 4: Rewrite InputActionsDocumentWidgets.cpp** -- the toolbar, the columns and the row loop:

```cpp
#include "Documents/InputActionsDocumentWidgets.hpp"

#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace Arcane::Editor
{
    namespace
    {
        constexpr float kMapsColumnWidth = 180.0f;
        constexpr float kIndent = 16.0f;
        constexpr const char* kDragPayload = "ARC_INPUT_ROW";

        Guid IdOf(const nlohmann::json& row)
        {
            if (!row.is_object() || !row.contains("id") || !row["id"].is_string()) return {};
            return Guid::FromString(row["id"].get<std::string>()).value_or(Guid{});
        }
        std::string Str(const nlohmann::json& row, const char* key)
        { return row.is_object() && row.contains(key) && row[key].is_string() ? row[key].get<std::string>() : std::string{}; }
        const nlohmann::json* FindMap(const nlohmann::json& draft, const Guid& id)
        {
            if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return nullptr;
            for (const auto& m : draft["actionMaps"]) if (IdOf(m) == id) return &m;
            return nullptr;
        }
        bool IsMapId(const nlohmann::json& draft, const Guid& id) { return FindMap(draft, id) != nullptr; }
        bool IsActionId(const nlohmann::json& draft, const Guid& id)
        {
            if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return false;
            for (const auto& m : draft["actionMaps"])
                if (m.is_object() && m.contains("actions") && m["actions"].is_array())
                    for (const auto& a : m["actions"]) if (IdOf(a) == id) return true;
            return false;
        }
        // Leading/trailing spaces and tabs stripped: the model's name rules
        // compare trimmed (Task 6 ValidateName), so the rename commits trimmed.
        std::string Trim(std::string s)
        {
            const auto notBlank = [](unsigned char c) { return c != ' ' && c != '\t'; };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notBlank));
            s.erase(std::find_if(s.rbegin(), s.rend(), notBlank).base(), s.end());
            return s;
        }
        std::string NameOf(const InputActionsEditorModel& model, const Guid& id)
        {
            const auto* n = model.FindNode(id);
            return n ? Str(*n, "name") : std::string{};
        }
        const char* DeviceIcon(const std::string& device)
        {
            if (device == "Keyboard") return ICON_LC_KEYBOARD;
            if (device == "Mouse")    return ICON_LC_MOUSE;
            if (device == "Gamepad")  return ICON_LC_GAMEPAD_2;
            return ICON_LC_CIRCLE_DOT;
        }
        int SchemeVariant(const std::string& badge) { return badge == "KeyboardMouse" ? 2 : 3; }

        void MoveRowMenu(InputActionsEditorModel& model, const Guid& id, std::function<void()>& edit)
        {
            if (ImGui::MenuItem("Move up"))   edit = [&model, id] { (void)model.MoveRow(id, -1); };
            if (ImGui::MenuItem("Move down")) edit = [&model, id] { (void)model.MoveRow(id, 1); };
        }
        // A new map/action opens in a rename box on its (unique, Task 6) name --
        // UE's new-item kick-off. Shared by the toolbar, the maps `+` and the
        // actions-column `+ Action`.
        void OpenRenameOn(InputActionsEditorModel& model, InputActionsDocumentState& state, const Guid& id)
        {
            state.renameTarget = id;
            state.renameBuf = NameOf(model, id);
            state.renameFocusPending = state.scrollToSelection = true;
        }
    }

    void InputActionsDocumentWidgets::SelectRow(InputActionsEditorModel& model, const InputRow& row)
    {
        switch (row.kind)
        {
        case InputRowKind::Action:          model.SelectAction(row.id); model.SelectBinding({}); break;
        case InputRowKind::Binding:
        case InputRowKind::CompositeHeader: model.SelectAction(row.actionId); model.SelectBinding(row.id); break;
        case InputRowKind::Part:            model.SelectAction(row.actionId); model.SelectBinding(row.bindingId); model.SelectPart(row.id); break;
        case InputRowKind::AddBinding:      break;
        }
    }

    bool InputActionsDocumentWidgets::RowSelected(const InputActionsEditorModel& model, const InputRow& row)
    {
        switch (row.kind)
        {
        case InputRowKind::Action:          return model.SelectedAction() == row.id && !model.SelectedBinding().IsValid();
        case InputRowKind::Binding:
        case InputRowKind::CompositeHeader: return model.SelectedBinding() == row.id && !model.SelectedPart().IsValid();
        case InputRowKind::Part:            return model.SelectedPart() == row.id;
        default:                            return false;
        }
    }

    void InputActionsDocumentWidgets::Draw(InputActionsEditorModel& model, InputActionsDocumentState& state,
                                            const Services& services)
    {
        const bool swallowed = services.inputSwallowed && services.inputSwallowed();
        Edit edit;
        // While a capture is armed the capture row is the only live control
        // (UE SKeySelector: the listening widget holds focus + capture): the
        // toolbar is disabled and neither column takes mouse input, so the
        // completing click retargets nothing and no other row's menu opens.
        // Scrolling pauses for the capture's duration (<= 10 s; Escape ends it).
        ImGui::BeginDisabled(swallowed);
        DrawToolbar(model, state, edit);
        ImGui::EndDisabled();
        ImGui::Separator();
        const ImGuiWindowFlags colFlags = swallowed ? ImGuiWindowFlags_NoInputs : 0;
        ImGui::BeginChild("##input_maps", ImVec2(kMapsColumnWidth, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX, colFlags);
        DrawMaps(model, state, services, edit);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##input_actions", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, colFlags);
        DrawActions(model, state, services, edit);
        ImGui::EndChild();
        if (state.schemePopupPending) { ImGui::OpenPopup("Control schemes##input"); state.schemePopupPending = false; }
        DrawSchemePopup(model, state, edit);
        if (edit) edit();   // AFTER the draw: the draft must not mutate under the row loop
    }

    void InputActionsDocumentWidgets::DrawToolbar(InputActionsEditorModel& model, InputActionsDocumentState& state, Edit& edit)
    {
        const Guid map = model.SelectedMap(), action = model.SelectedAction();
        if (ImGui::Button(ICON_LC_PLUS " Add " ICON_LC_CHEVRON_DOWN)) ImGui::OpenPopup("##input_add");
        if (ImGui::BeginPopup("##input_add"))
        {
            // The model selects the new row and gives it a unique sibling name
            // ("Action Map 2", "Action 3" -- Task 6); the new row opens in rename.
            if (ImGui::MenuItem("Action map")) edit = [&model, &state] { if (model.AddMap()) OpenRenameOn(model, state, model.SelectedMap()); };
            if (ImGui::MenuItem("Action", nullptr, false, map.IsValid())) edit = [&model, &state, map] { if (model.AddAction(map)) OpenRenameOn(model, state, model.SelectedAction()); };
            if (ImGui::MenuItem("Binding", nullptr, false, action.IsValid())) edit = [&model, &state, map, action] { if (model.AddBinding(map, action)) state.scrollToSelection = true; };
            if (ImGui::BeginMenu("Composite", action.IsValid()))
            {
                if (ImGui::MenuItem("1D axis"))   edit = [&model, &state, map, action] { if (model.AddComposite(map, action, "1DAxis")) state.scrollToSelection = true; };
                if (ImGui::MenuItem("2D vector")) edit = [&model, &state, map, action] { if (model.AddComposite(map, action, "2DVector")) state.scrollToSelection = true; };
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Control scheme")) state.schemePopupPending = true;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputTextWithHint("##input_search", ICON_LC_SEARCH " Search", state.search, sizeof(state.search));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        const std::string preview = state.schemeFilter.empty() ? "All schemes" : state.schemeFilter;
        if (ImGui::BeginCombo("##input_scheme", preview.c_str()))
        {
            if (ImGui::Selectable("All schemes", state.schemeFilter.empty())) state.schemeFilter.clear();
            if (model.Draft().is_object() && model.Draft().contains("controlSchemes") && model.Draft()["controlSchemes"].is_array())
                for (const auto& s : model.Draft()["controlSchemes"])
                {
                    const std::string group = Str(s, "bindingGroup");
                    if (ImGui::Selectable(Str(s, "name").c_str(), state.schemeFilter == group)) state.schemeFilter = group;
                }
            ImGui::Separator();
            if (ImGui::Selectable("Edit schemes...")) state.schemePopupPending = true;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (state.previewArmed) ImGui::PushStyleColor(ImGuiCol_Button, Theme::WithAlpha(Theme::kAmber, 0.35f));
        if (ImGui::Button(ICON_LC_PLAY " Preview")) state.previewArmed = !state.previewArmed;
        if (state.previewArmed) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Live preview: bindings glow as they fire; the Inspector's Live preview block reads live values");
    }

    void InputActionsDocumentWidgets::DrawMaps(InputActionsEditorModel& model, InputActionsDocumentState& state,
                                                const Services& services, Edit& edit)
    {
        ImGui::TextDisabled("ACTION MAPS");
        // Right-edge idiom (GetWindowContentRegionMax is obsolete in 1.92).
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());
        if (ImGui::SmallButton(ICON_LC_PLUS "##addmap")) edit = [&model, &state] { if (model.AddMap()) OpenRenameOn(model, state, model.SelectedMap()); };
        const auto& draft = model.Draft();
        if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return;
        // A rename target that no row can draw (undo removed it) would wedge the
        // key handlers shut: sweep it -- the Outliner's sweep, EditorPanels.cpp:1644-1658.
        if (state.renameTarget.IsValid() && !IsMapId(draft, state.renameTarget) && !IsActionId(draft, state.renameTarget)) state.renameTarget = {};
        for (const auto& m : draft["actionMaps"])
        {
            const Guid id = IdOf(m);
            if (!id.IsValid()) continue;
            ImGui::PushID(id.ToString().c_str());
            const std::string name = Str(m, "name");
            if (state.renameTarget == id)
            {
                // Same rules as the action rename in DrawRow: live validation with
                // its reason, Enter on invalid text re-arms the box, focus loss on
                // invalid text cancels, Escape reverts (ImGui restores the buffer
                // before deactivating, so the Escape check is the cancel path).
                if (state.scrollToSelection) { ImGui::SetScrollHereY(); state.scrollToSelection = false; }
                if (state.renameFocusPending) { ImGui::SetKeyboardFocusHere(); state.renameFocusPending = false; }
                ImGui::SetNextItemWidth(-FLT_MIN);
                const bool entered = InputTextString("##rename", &state.renameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                const auto reason = InputActionsEditorModel::ValidateName(draft, id, state.renameBuf);
                if (reason && ImGui::IsItemActive()) ImGui::SetItemTooltip("%s", reason->c_str());
                if (ImGui::IsItemDeactivated())
                {
                    const bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape);
                    if (cancelled || !reason)
                    {
                        const std::string trimmed = Trim(state.renameBuf);
                        if (!cancelled && trimmed != name) edit = [&model, id, trimmed] { (void)model.SetField(id, "name", trimmed); };
                        state.renameTarget = {};
                    }
                    else if (entered) state.renameFocusPending = true;   // keep renameTarget + renameBuf
                    else state.renameTarget = {};
                }
                ImGui::PopID();
                continue;
            }
            const auto row = RowWithThumb("##map", 0, ICON_LC_LAYERS, name.c_str(), model.SelectedMap() == id, 0.0f);
            const ImVec2 rowBottom = ImGui::GetCursorScreenPos();   // restored after the trailing pills (same rule as DrawRow)
            if (row.clicked) model.SelectMap(id);
            if (model.SelectedMap() == id && state.scrollToSelection) { ImGui::SetScrollHereY(); state.scrollToSelection = false; }
            if (ImGui::BeginPopupContextItem("##mapmenu"))
            {
                if (ImGui::MenuItem("Rename", "F2")) { state.renameTarget = id; state.renameBuf = name; state.renameFocusPending = state.scrollToSelection = true; }
                if (ImGui::MenuItem("Duplicate")) edit = [&model, &state, id] { if (model.DuplicateRow(id)) state.scrollToSelection = true; };
                if (ImGui::MenuItem("Set as default", nullptr, draft.value("defaultMap", std::string{}) == id.ToString()))
                    edit = [&model, id] { (void)model.SetDefaultMap(id); };
                ImGui::Separator();
                MoveRowMenu(model, id, edit);
                ImGui::Separator();
                if (ImGui::MenuItem("Delete", "Del")) edit = [&model, id] { (void)model.RemoveMap(id); };
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(row.trailingPos);
            if (m.value("blocking", false)) { AssetPill("blocks", 1); ImGui::SameLine(); }
            const std::size_t count = m.contains("actions") && m["actions"].is_array() ? m["actions"].size() : 0;
            AssetPill(std::to_string(count).c_str(), 0);
            ImGui::SetCursorScreenPos(rowBottom);
            ImGui::PopID();
        }
        // Empty space in the maps column deselects: the ASSET is the container
        // (spec A s3.1). Not while an inline rename is live -- that click commits
        // the rename.
        if (!state.renameTarget.IsValid() && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
            model.SelectMap({});
        HandleMapKeys(model, state, services, edit);   // still inside ##input_maps
    }

    void InputActionsDocumentWidgets::DrawActions(InputActionsEditorModel& model, InputActionsDocumentState& state,
                                                   const Services& services, Edit& edit)
    {
        const Guid map = model.SelectedMap();
        const nlohmann::json* m = FindMap(model.Draft(), map);
        if (!m) { ImGui::TextDisabled("Select an action map."); return; }
        ImGui::TextUnformatted(Str(*m, "name").c_str());
        ImGui::SameLine(); ImGui::TextDisabled("· actions");
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(ICON_LC_PLUS " Action").x - ImGui::GetStyle().FramePadding.x * 2.0f);
        if (ImGui::SmallButton(ICON_LC_PLUS " Action")) edit = [&model, &state, map] { if (model.AddAction(map)) OpenRenameOn(model, state, model.SelectedAction()); };
        ImGui::Separator();
        InputRowFilter filter;
        filter.search = state.search;
        filter.schemeGroup = state.schemeFilter;
        const std::vector<InputRow> rows = BuildInputRows(model.Draft(), map, filter, model.Conflicts(), state.collapsedActions);
        // A rename target can stop being drawable without its InputText ever
        // deactivating (undo/redo or an Inspector page removed the action; the
        // search/scheme filter or a collapse dropped it from `rows`): sweep it.
        if (state.renameTarget.IsValid())
        {
            bool drawn = false;
            for (const InputRow& r : rows) if (r.kind == InputRowKind::Action && r.id == state.renameTarget) { drawn = true; break; }
            if (!drawn && !IsMapId(model.Draft(), state.renameTarget)) state.renameTarget = {};
        }
        state.dragVerdictPrev = state.dragVerdict;
        state.dragVerdict = InputActionsDocumentState::DragVerdict::Illegal;   // hovering no row reads "Cannot move" (the drag op starts invalid)
        for (const InputRow& row : rows) DrawRow(row, model, state, services, edit, rows);
        // Empty space under the rows: the MAP is the container (spec A s3.1);
        // SelectMap(same) clears action/binding/part.
        if (!state.renameTarget.IsValid() && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
            model.SelectMap(map);
        HandleKeys(model, state, services, edit, rows);
    }
```

`DrawRow` -- one row of any kind:

```cpp
    void InputActionsDocumentWidgets::DrawRow(const InputRow& row, InputActionsEditorModel& model,
                                               InputActionsDocumentState& state, const Services& services,
                                               Edit& edit, const std::vector<InputRow>& rows)
    {
        ImGui::PushID(row.id.ToString().c_str());
        ImGui::PushID(static_cast<int>(row.kind));
        const float indent = kIndent * static_cast<float>(row.depth);
        const bool swallowed = services.inputSwallowed && services.inputSwallowed();
        const Guid map = model.SelectedMap();
        if (row.kind == InputRowKind::AddBinding)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::kNone);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::kTextDim);
            if (ImGui::SmallButton(ICON_LC_PLUS " Binding") && !swallowed)
                edit = [&model, &state, map, action = row.actionId] { if (model.AddBinding(map, action)) state.scrollToSelection = true; };
            ImGui::PopStyleColor(2);
            ImGui::PopID(); ImGui::PopID();
            return;
        }

        // Inline rename (actions only). Validation runs every frame (blank,
        // duplicate sibling) and shows its reason; Enter with invalid text re-arms
        // the box next frame (ImGui deactivates on Enter; UE stays in edit),
        // focus loss with invalid text cancels. Escape reverts the buffer to its
        // seed BEFORE deactivating, so the Escape check is the cancel path.
        if (row.kind == InputRowKind::Action && state.renameTarget == row.id)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent + ImGui::GetFrameHeight());
            if (state.scrollToSelection) { ImGui::SetScrollHereY(); state.scrollToSelection = false; }
            if (state.renameFocusPending) { ImGui::SetKeyboardFocusHere(); state.renameFocusPending = false; }
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool entered = InputTextString("##rename", &state.renameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            const auto reason = InputActionsEditorModel::ValidateName(model.Draft(), row.id, state.renameBuf);
            if (reason && ImGui::IsItemActive()) ImGui::SetItemTooltip("%s", reason->c_str());
            if (ImGui::IsItemDeactivated())
            {
                const bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape);
                if (cancelled || !reason)
                {
                    const std::string trimmed = Trim(state.renameBuf);
                    if (!cancelled && trimmed != row.name) edit = [&model, id = row.id, trimmed] { (void)model.SetField(id, "name", trimmed); };
                    state.renameTarget = {};
                }
                else if (entered) state.renameFocusPending = true;   // keep renameTarget + renameBuf
                else state.renameTarget = {};
            }
            ImGui::PopID(); ImGui::PopID();
            return;
        }

        // Expander chevron before an action row. A non-empty search forces every
        // action open (rows are built that way) and the click is a no-op then, so
        // the user's own collapse state survives the search.
        const bool searching = state.search[0] != '\0';
        if (row.kind == InputRowKind::Action)
        {
            const bool collapsed = !searching && state.collapsedActions.count(row.id.ToString()) != 0;
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::kNone);
            if (ImGui::SmallButton(collapsed ? ICON_LC_CHEVRON_RIGHT "##x" : ICON_LC_CHEVRON_DOWN "##x") && !searching && !swallowed)
            {
                if (collapsed) state.collapsedActions.erase(row.id.ToString());
                else state.collapsedActions.insert(row.id.ToString());
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }

        const bool selected = RowSelected(model, row);
        const bool rebinding = (row.kind == InputRowKind::Binding || row.kind == InputRowKind::Part) && services.isRebinding && services.isRebinding(row.id);
        std::string label = row.name;
        if (rebinding)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "Press a control... Esc cancels · %.0f s", services.rebindRemaining ? services.rebindRemaining() : 0.0f);
            label = buf;
        }
        const char* icon = row.kind == InputRowKind::Action ? "" : row.kind == InputRowKind::CompositeHeader ? ICON_LC_LAYERS_2 : DeviceIcon(row.device);
        if (rebinding) ImGui::PushStyleColor(ImGuiCol_Text, Theme::kAmber);
        const AssetRowResult r = RowWithThumb("##row", 0, icon, label.c_str(), selected, row.kind == InputRowKind::Action ? 0.0f : indent);
        if (rebinding) ImGui::PopStyleColor();
        const ImVec2 rowBottom = ImGui::GetCursorScreenPos();   // RowWithThumb parked the cursor at the next row's start; restored at the end
        if (r.clicked && !swallowed) SelectRow(model, row);
        if (selected && state.scrollToSelection) { ImGui::SetScrollHereY(); state.scrollToSelection = false; }

        // Live glow: an amber bar at the row's left edge + a faint wash.
        if (const float v = services.glow ? services.glow(row.id) : 0.0f; v > 0.0f && row.kind != InputRowKind::Action)
        {
            const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(lo, hi, ImGui::ColorConvertFloat4ToU32(Theme::WithAlpha(Theme::kAmber, 0.12f + 0.2f * std::min(v, 1.0f))));
            dl->AddRectFilled(lo, ImVec2(lo.x + 2.0f, hi.y), ImGui::ColorConvertFloat4ToU32(Theme::kAmber));
        }

        // Drag-drop reorder within the parent (spec s6: plus Move up/down below).
        // Legality is decided while HOVERING, not on release: the target writes a
        // verdict, the source's preview reads it next frame (the drag decorator),
        // and only a legal target draws a drop highlight. Inert while a capture is
        // armed. The AddBinding ghost never reaches here (it returned above).
        if (!swallowed && ImGui::BeginDragDropSource())
        {
            const std::string id = row.id.ToString();
            ImGui::SetDragDropPayload(kDragPayload, id.c_str(), id.size() + 1);
            const bool legal = state.dragVerdictPrev == InputActionsDocumentState::DragVerdict::Legal;
            ImGui::PushStyleColor(ImGuiCol_Text, legal ? Theme::kText : Theme::kTextDim);
            ImGui::Text(legal ? "%s  Move '%s' here" : "%s  Cannot move '%s' here", legal ? ICON_LC_CHECK : ICON_LC_BAN, row.name.c_str());
            ImGui::PopStyleColor();
            ImGui::EndDragDropSource();
        }
        if (!swallowed && ImGui::BeginDragDropTarget())
        {
            // AcceptPeekOnly = AcceptBeforeDelivery | AcceptNoDrawDefaultRect: the
            // payload is visible every hovered frame and ImGui draws nothing itself.
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragPayload, ImGuiDragDropFlags_AcceptPeekOnly))
            {
                const Guid src = Guid::FromString(static_cast<const char*>(p->Data)).value_or(Guid{});
                const auto from = SiblingIndex(model.Draft(), src), to = SiblingIndex(model.Draft(), row.id);
                // Same parent array only (a Part cannot land among an action's
                // bindings, a Binding not among a composite's parts, nothing on
                // itself), and the index must change -- a no-op is not a target.
                const bool legal = from && to && from->parent == to->parent && src != row.id && from->index != to->index;
                state.dragVerdict = legal ? InputActionsDocumentState::DragVerdict::Legal : InputActionsDocumentState::DragVerdict::Illegal;
                if (legal)
                {
                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                        ImGui::ColorConvertFloat4ToU32(Theme::kSelection), 0.0f, 0, 2.0f);
                    if (p->IsDelivery())
                        edit = [&model, src, index = to->index] { (void)model.MoveRowTo(src, index); };
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Context menu (inert while a capture is armed).
        if (!swallowed && ImGui::BeginPopupContextItem("##rowmenu"))
        {
            if (row.kind == InputRowKind::Action)
            {
                if (ImGui::MenuItem("Rename", "F2")) { state.renameTarget = row.id; state.renameBuf = row.name; state.renameFocusPending = state.scrollToSelection = true; }
                if (ImGui::MenuItem("Duplicate")) edit = [&model, &state, map, id = row.id] { if (model.DuplicateAction(map, id)) state.scrollToSelection = true; };
                if (ImGui::MenuItem("Add binding")) edit = [&model, &state, map, id = row.id] { if (model.AddBinding(map, id)) state.scrollToSelection = true; };
                if (ImGui::BeginMenu("Add composite"))
                {
                    if (ImGui::MenuItem("1D axis"))   edit = [&model, &state, map, id = row.id] { if (model.AddComposite(map, id, "1DAxis")) state.scrollToSelection = true; };
                    if (ImGui::MenuItem("2D vector")) edit = [&model, &state, map, id = row.id] { if (model.AddComposite(map, id, "2DVector")) state.scrollToSelection = true; };
                    ImGui::EndMenu();
                }
                ImGui::Separator(); MoveRowMenu(model, row.id, edit); ImGui::Separator();
                if (ImGui::MenuItem("Delete", "Del")) edit = [&model, map, id = row.id] { (void)model.RemoveAction(map, id); };
            }
            else if (row.kind == InputRowKind::CompositeHeader)
            {
                if (ImGui::BeginMenu("Add part"))
                {
                    const bool axis = row.name == "1D Axis";
                    for (const char* role : axis ? std::vector<const char*>{ "negative", "positive" } : std::vector<const char*>{ "up", "down", "left", "right" })
                        if (ImGui::MenuItem(role)) edit = [&model, &state, id = row.id, role] { if (model.AddPart(id, role)) state.scrollToSelection = true; };
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Duplicate")) edit = [&model, &state, id = row.id] { if (model.DuplicateRow(id)) state.scrollToSelection = true; };
                ImGui::Separator(); MoveRowMenu(model, row.id, edit); ImGui::Separator();
                if (ImGui::MenuItem("Delete", "Del")) edit = [&model, map, action = row.actionId, id = row.id] { (void)model.RemoveBinding(map, action, id); };
            }
            else   // Binding / Part
            {
                if (ImGui::MenuItem("Rebind...", "Enter") && services.beginRebind) services.beginRebind(row.id);
                if (ImGui::MenuItem("Duplicate")) edit = [&model, &state, id = row.id] { if (model.DuplicateRow(id)) state.scrollToSelection = true; };
                ImGui::Separator(); MoveRowMenu(model, row.id, edit); ImGui::Separator();
                if (ImGui::MenuItem("Delete", "Del"))
                {
                    if (row.kind == InputRowKind::Part) edit = [&model, composite = row.bindingId, id = row.id] { (void)model.RemovePart(composite, id); };
                    else edit = [&model, map, action = row.actionId, id = row.id] { (void)model.RemoveBinding(map, action, id); };
                }
            }
            ImGui::EndPopup();
        }
        if (!row.path.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", row.path.c_str());

        // Trailing: conflict dot, badges, detail, the Rebind hit region.
        ImGui::SetCursorScreenPos(r.trailingPos);
        if (row.conflict)
        {
            ImGui::TextColored(Theme::kAmber, ICON_LC_CIRCLE_DOT);
            if (ImGui::IsItemHovered())
            {
                std::string who;
                for (const auto& c : model.Conflicts()) if (c.binding == row.id) { if (!who.empty()) who += ", "; who += c.otherActionName; }
                ImGui::SetTooltip("Also bound by %s", who.c_str());
            }
            ImGui::SameLine();
        }
        if (row.kind == InputRowKind::Action)
        {
            if (!row.badge.empty()) AssetPill(row.badge.c_str(), 0);
            if (!row.detail.empty()) { if (!row.badge.empty()) ImGui::SameLine(); ImGui::TextDisabled("%s", row.detail.c_str()); }
        }
        else
        {
            if (!row.detail.empty()) { ImGui::TextDisabled("%s", row.detail.c_str()); ImGui::SameLine(); }
            for (const auto& g : row.groups) { AssetPill(g.c_str(), SchemeVariant(g)); ImGui::SameLine(); }   // one tinted pill PER scheme
            if (row.kind != InputRowKind::CompositeHeader && !rebinding && !swallowed)
            {
                // The Rebind button: its hit region is SUBMITTED every frame and
                // only its PAINT is gated on hover/selection. Gating the submission
                // on r.hovered oscillates on an AllowOverlap row (the Asset Browser
                // rail's documented bug, AssetBrowserPanel.cpp:304-341): the rule
                // for any trailing widget on a RowWithThumb row.
                const ImVec2 sz(ImGui::CalcTextSize("Rebind").x + ImGui::GetStyle().FramePadding.x * 2.0f, ImGui::GetFrameHeight());
                const bool rbClicked = ImGui::InvisibleButton("##rebind", sz);
                const bool rbHovered = ImGui::IsItemHovered();
                if (r.hovered || selected || rbHovered)
                {
                    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(lo, hi, ImGui::GetColorU32(rbHovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button), ImGui::GetStyle().FrameRounding);
                    dl->AddText(ImVec2(lo.x + ImGui::GetStyle().FramePadding.x, lo.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), "Rebind");
                }
                if (rbClicked && services.beginRebind) services.beginRebind(row.id);
            }
        }
        ImGui::SetCursorScreenPos(rowBottom);   // every row pitches exactly one RowWithThumb height whatever the trailing item's height was
        ImGui::PopID(); ImGui::PopID();
    }
```

`DrawSchemePopup`:

```cpp
    void InputActionsDocumentWidgets::DrawSchemePopup(InputActionsEditorModel& model, InputActionsDocumentState& state, Edit& edit)
    {
        if (!ImGui::BeginPopupModal("Control schemes##input", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        if (ImGui::BeginTable("##schemes", 3, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Group"); ImGui::TableSetupColumn("##x", ImGuiTableColumnFlags_WidthFixed, 24.0f);
            ImGui::TableHeadersRow();
            if (model.Draft().is_object() && model.Draft().contains("controlSchemes") && model.Draft()["controlSchemes"].is_array())
                for (const auto& s : model.Draft()["controlSchemes"])
                {
                    const Guid id = IdOf(s);
                    ImGui::PushID(id.ToString().c_str());
                    ImGui::TableNextRow();
                    std::string name = Str(s, "name"), group = Str(s, "bindingGroup");
                    ImGui::TableSetColumnIndex(0); ImGui::SetNextItemWidth(-FLT_MIN);
                    InputTextString("##name", &name);
                    if (ImGui::IsItemDeactivatedAfterEdit() && !name.empty()) edit = [&model, id, name, group] { (void)model.EditScheme(id, name, group); };
                    ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN);
                    InputTextString("##group", &group);
                    if (ImGui::IsItemDeactivatedAfterEdit() && !group.empty()) edit = [&model, id, name, group] { (void)model.EditScheme(id, name, group); };
                    ImGui::TableSetColumnIndex(2);
                    if (ImGui::SmallButton(ICON_LC_TRASH_2)) edit = [&model, id] { (void)model.RemoveScheme(id); };
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(120.0f); ImGui::InputText("Name", state.newSchemeName, sizeof state.newSchemeName);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f); ImGui::InputText("Group", state.newSchemeGroup, sizeof state.newSchemeGroup);
        ImGui::SameLine();
        if (ImGui::Button(ICON_LC_PLUS " Scheme"))
            edit = [&model, name = std::string(state.newSchemeName), group = std::string(state.newSchemeGroup)] { (void)model.AddScheme(name, group); };
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
```

`HandleKeys` and `HandleMapKeys` are Task 9's; for this task give both an empty body (`{ (void)model; (void)state; (void)services; (void)edit; }` -- `HandleKeys` also `(void)rows;`). The `InputTextString` calls with a per-frame local `name`/`group` are the documented pattern (EditorWidgets.hpp:33-37). `Renaming a scheme renames its group references` is the model's existing `EditScheme` behaviour. `SelectRow`/`RowSelected` are unchanged by the model's epoch rule (Task 6): every `Select*` call with a valid id bumps the epoch, so a re-click on the selected row is the gesture that brings the Inspector back.

- [ ] **Step 5: Build, run, capture**

Build; `./ArcaneTests.exe "[editor]"` green (the `SnapshotForCapture` case included; the widgets have no unit test, the rows do). Then from `bin/Debug-windows-x86_64-md/ArcaneEditor/`: `rm imgui.ini; MSYS_NO_PATHCONV=1 ./ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 90 --settle 10 --open-asset 97260310-8b35-4b29-b12f-1fd6f8e99071 --report r.json --screenshot s.png`. Read `s.png` (the Read tool renders it). Expected picture: toolbar `+ Add v | search | All schemes | Preview`; maps column with one `Player` row (layers icon, `2` count pill); actions column `Player · actions`, `Move` with `Axis1D` pill, `1D Axis` header with `KeyboardMouse` pill, `A  1D Axis · negative`, `D  1D Axis · positive`, `Left Stick X` with `Gamepad` pill, `+ Binding`, `Jump` with `Button`, `W`, `Space`, `South Button`, `+ Binding`. No Save button, no tab strip, no PROPERTIES block. Every row on its own 24 px line -- `1D Axis` directly under `Move`, `W` directly under `Jump`; if any row appears to the right of a pill, the trailing block left the cursor on the previous line; parts read `A  1D Axis · negative`; a binding in two schemes shows two tinted pills. The headless capture opens the asset clean, so the tab shows a plain X. Note in the report what differs from the mockup.

Desk (windowed, `ArcaneEditor.exe --project ReferenceProject`): rename or rebind anything -> the tab shows the unsaved dot; Ctrl+S -> plain X; edit then Ctrl+Z back to the saved revision -> plain X. Right-click `Player.arcinput` in the Asset Browser > Open as text opens it in the OS default editor; the Assets menu carries the same item. Click empty space under the action rows: the Inspector (once Task 10 lands) drops to `Player.arcinput > Player`; click empty space in the maps column: `Select an action map.` and the asset page. `+ Add > Action map` and the maps `+`: the new `Action Map` row opens in a rename box, scrolled into view; `+ Action` (toolbar or column header): `Action`, then `Action 2`, each in a rename box; typing a sibling's name shows the reason in a tooltip, Enter keeps the box open, Escape keeps the default.

- [ ] **Step 6: Commit**

```bash
git add ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp ArcaneEditor/src/Documents/InputActionsDocument.hpp ArcaneEditor/src/Documents/InputActionsDocument.cpp ArcaneEditor/src/Widgets/EditorWidgets.hpp ArcaneEditor/src/Widgets/EditorWidgets.cpp ArcaneEditor/src/Panels/AssetPanelCommon.hpp ArcaneEditor/src/Panels/AssetPanelCommon.cpp ArcaneEditor/src/Panels/AssetBrowserPanel.cpp ArcaneEditor/src/Panels/EditorPanels.hpp ArcaneEditor/src/Panels/EditorPanels.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/InputActionsEditorModelTest.cpp
git commit -m "feat(editor): Input Actions document redesigned -- toolbar (+ Add, search, scheme filter, Preview), maps column, actions column with expanded readable binding rows, composites as header + parts, per-scheme pills, hover-decided drag-drop, validated inline rename, context menus, scheme popup; the rebind capture consumes its completing input and cancels on focus loss; tab dot via UnsavedDocument; Assets > Open as text; Save button + JSON tab retired (input editor plan T8)"
```

---

### Task 9: Rebind on the row, conflicts, drag-drop, keyboard navigation

**Files:**
- Modify: `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp` / `.cpp` (`HandleKeys`, `HandleMapKeys` -- each runs inside its own child, so `ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)` routes the keys to whichever column has focus). Nothing in `InputActionsDocument.cpp` or `EditorAppFrame.cpp`.

**Interfaces:**
- Consumes: `StepSelection` (Task 7), `SelectRow` / `RowSelected` (Task 8), `Services::beginRebind` and `Services::inputSwallowed` (Task 8), `InputActionsDocumentState::{search, collapsedActions, renameTarget, renameBuf, renameFocusPending, scrollToSelection}` (Task 8), the `HandleMapKeys` declaration Task 8 left with an empty body, the model's remove calls (`RemoveAction`, `RemoveBinding`, `RemovePart`, `RemoveMap`), the file-local `IdOf` / `Str` helpers (Task 8).
- Produces: keyboard behaviour per spec B s2.3 in BOTH columns. Actions column: Up/Down move the selection across visible rows (auto-repeat, the selected row scrolls into view); Left/Right follow the tree convention (Left collapses an expanded action, otherwise selects the parent row; Right expands a collapsed action, otherwise descends to the first child); Enter rebinds the selected binding/part; Delete removes the selected row (undoable); F2 renames the selected action. Maps column: Up/Down move the map selection, F2 renames the map, Delete removes it (undoable). Left/Right/Enter/F2/Delete never auto-repeat; Alt-modified Left/Right are left alone; a non-empty search never edits `collapsedActions`. Every key is inert while a rebind capture is live (and on its completing frame), during a drag-drop, while any text box owns the keyboard (`WantTextInput`), or while an inline rename is live.

- [ ] **Step 1: Implement HandleKeys and HandleMapKeys**

Both bodies replace the empty ones Task 8 left in `InputActionsDocumentWidgets.cpp`. `HandleMapKeys` uses `std::find` and `std::clamp`: make sure `#include <algorithm>` is in the file's include block (add it after `#include <cstdio>` if Task 8 did not already need it).

```cpp
    void InputActionsDocumentWidgets::HandleKeys(InputActionsEditorModel& model, InputActionsDocumentState& state,
                                                  const Services& services, Edit& edit, const std::vector<InputRow>& rows)
    {
        // A rebind capture owns the keyboard: the key that completed (or is
        // feeding) it is consumed there (UE SInputKeySelector's OnPreviewKeyDown
        // rule). No commands mid-drag (UE FUICommandList). Then the Outliner's
        // guard: this column focused, no text box typing.
        if (services.inputSwallowed && services.inputSwallowed()) return;
        if (ImGui::GetDragDropPayload() != nullptr) return;   // the public form; IsDragDropActive is imgui_internal
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || ImGui::GetIO().WantTextInput) return;
        if (state.renameTarget.IsValid()) return;   // safe: DrawActions/DrawMaps sweep a target whose row is not drawn this frame
        const Guid current = model.SelectedPart().IsValid() ? model.SelectedPart()
                           : model.SelectedBinding().IsValid() ? model.SelectedBinding()
                           : model.SelectedAction();
        const InputRow* row = nullptr;
        for (const auto& r : rows) if (r.kind != InputRowKind::AddBinding && r.id == current) { row = &r; break; }

        auto step = [&](int dir)
        {
            if (const auto next = StepSelection(rows, current, dir))
                for (const auto& r : rows) if (r.id == *next && r.kind != InputRowKind::AddBinding) { SelectRow(model, r); state.scrollToSelection = true; break; }
        };
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) step(+1);   // navigation repeats (UE SListView)
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   step(-1);
        if (!row) return;
        const std::string actionKey = row->actionId.ToString();
        const bool searching = state.search[0] != '\0';   // a search forces every action open: Left/Right never edit collapsedActions then
        const bool collapsed = !searching && state.collapsedActions.count(actionKey) != 0;
        auto rowById = [&](InputRowKind kind, const Guid& id) -> const InputRow*
        {
            for (const auto& r : rows) if (r.kind == kind && r.id == id) return &r;
            return nullptr;
        };
        // Tree convention: Left collapses an expanded action, otherwise selects
        // the parent row (no collapse); Right expands a collapsed action,
        // otherwise descends to the first child. Commands (Left/Right/Enter/F2/
        // Delete) never auto-repeat: repeat = false, as the Outliner passes.
        // Alt-modified presses are left alone (window/menu chords).
        if (!ImGui::GetIO().KeyAlt)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
            {
                if (row->kind == InputRowKind::Action)
                {
                    if (!collapsed && !searching) state.collapsedActions.insert(actionKey);   // already collapsed: no-op
                }
                else if (row->kind == InputRowKind::Part)
                {
                    if (const auto* parent = rowById(InputRowKind::CompositeHeader, row->bindingId)) { SelectRow(model, *parent); state.scrollToSelection = true; }
                }
                else if (const auto* parent = rowById(InputRowKind::Action, row->actionId)) { SelectRow(model, *parent); state.scrollToSelection = true; }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
            {
                if (row->kind == InputRowKind::Action && collapsed) state.collapsedActions.erase(actionKey);
                else if (row->kind == InputRowKind::Action || row->kind == InputRowKind::CompositeHeader)
                {
                    const std::size_t at = static_cast<std::size_t>(row - rows.data());
                    if (at + 1 < rows.size() && rows[at + 1].depth > row->depth && rows[at + 1].kind != InputRowKind::AddBinding)
                    { SelectRow(model, rows[at + 1]); state.scrollToSelection = true; }
                }
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && (row->kind == InputRowKind::Binding || row->kind == InputRowKind::Part) && services.beginRebind)
            services.beginRebind(row->id);
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && row->kind == InputRowKind::Action)
        { state.renameTarget = row->id; state.renameBuf = row->name; state.renameFocusPending = state.scrollToSelection = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            const Guid map = model.SelectedMap();
            switch (row->kind)
            {
            case InputRowKind::Action:          edit = [&model, map, id = row->id] { (void)model.RemoveAction(map, id); }; break;
            case InputRowKind::Binding:
            case InputRowKind::CompositeHeader: edit = [&model, map, action = row->actionId, id = row->id] { (void)model.RemoveBinding(map, action, id); }; break;
            case InputRowKind::Part:            edit = [&model, composite = row->bindingId, id = row->id] { (void)model.RemovePart(composite, id); }; break;
            default: break;
            }
        }
    }

    void InputActionsDocumentWidgets::HandleMapKeys(InputActionsEditorModel& model, InputActionsDocumentState& state,
                                                     const Services& services, Edit& edit)
    {
        if (services.inputSwallowed && services.inputSwallowed()) return;
        if (ImGui::GetDragDropPayload() != nullptr) return;
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || ImGui::GetIO().WantTextInput) return;
        if (state.renameTarget.IsValid()) return;
        const auto& draft = model.Draft();
        if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return;
        std::vector<Guid> ids; std::vector<std::string> names;
        for (const auto& m : draft["actionMaps"]) if (const Guid id = IdOf(m); id.IsValid()) { ids.push_back(id); names.push_back(Str(m, "name")); }
        if (ids.empty()) return;
        const Guid current = model.SelectedMap();
        auto index = std::find(ids.begin(), ids.end(), current);
        auto step = [&](int dir)
        {
            const std::ptrdiff_t i = index == ids.end() ? (dir > 0 ? 0 : static_cast<std::ptrdiff_t>(ids.size()) - 1)
                                                        : std::clamp<std::ptrdiff_t>((index - ids.begin()) + dir, 0, static_cast<std::ptrdiff_t>(ids.size()) - 1);
            model.SelectMap(ids[static_cast<std::size_t>(i)]); state.scrollToSelection = true;
        };
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) step(+1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   step(-1);
        if (index == ids.end()) return;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
        { state.renameTarget = current; state.renameBuf = names[static_cast<std::size_t>(index - ids.begin())]; state.renameFocusPending = state.scrollToSelection = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) edit = [&model, id = current] { (void)model.RemoveMap(id); };
    }
```

The scene's Delete/F2 are the Outliner's own, not the app's: EditorPanels.cpp:1662-1678 gates them on `ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)` for the Outliner window (plus editMode, not renaming, `!WantTextInput`), so they already stand down while a document window is focused. There is no app-level Delete/F2 path: `InputEdges` (EditorApp.hpp:1075-1084) carries no Delete/F2 edge and the `sc*` flags (EditorApp.hpp:264) are New/Open/Save only. Verify at the desk (select an entity in the Outliner, focus the document, press Delete and F2: no entity is removed or renamed; the document row is) and record the line numbers; do not touch EditorAppFrame.cpp. Ctrl+S from an Inspector page is already covered by Task 4's `saveRequested`/`focusedSource` route; do not add a second gate here.

- [ ] **Step 2: Build, desk-verify** -- capture is not enough here; run the editor and record each item in the report (an item with no controller on the desk is recorded OWED, never skipped silently):

> Open Player.arcinput. Rebind: click `Space` under Jump, Enter, press `J`: the row reads `J`, Ctrl+Z restores `Space`; Enter then Escape: the row returns; Enter then wait 10 s: the row returns. Consumption: Enter on Space then Delete: the row reads `Delete` and still exists; Enter then Enter: the row reads `Return` and no second capture starts (no amber text); Enter then F2: the row reads `F2`, no rename box; Enter then Down: the row reads `Down Arrow`, the selection does not move; during a capture right-click another row: no menu; hover another row: no Rebind button. Chords: Enter, hold Shift, press J: `Left Shift + J`; Enter, tap Shift alone: `Left Shift`. Mouse: Enter on Space, click the RIGHT button over the maps column: the row reads `Right Button`, the map selection does NOT change, no context menu opens, Ctrl+Z restores. Focus: Enter, click the Viewport, press W: Space unchanged, the row no longer amber; Enter, switch to another docked tab, wait 2 s, switch back: no frozen countdown. Conflicts: add a second `<Keyboard>/space` binding to Move: both rows show the amber dot naming the other action; rebind Crouch (add one) to Space via the CAPTURE (scancode form): the dot still appears against Jump's authored `<Keyboard>/space`. Drag: drag `South Button` above `W`: only the Move bindings show the outline while hovered, the preview reads "Move 'South Button' here", release reorders, Ctrl+Z reverts; drag `Space` (Jump) over `Move` or `W`: no outline, "Cannot move 'Space' here", release does nothing; drag a part over a plain binding: no outline. Keys: select `A  1D Axis · negative` under Move: Left selects the `1D Axis` header (Move stays expanded); Left again selects Move (still expanded); Left again collapses Move; Left again no-op; Right re-expands; Right again selects `1D Axis`; Right on `1D Axis` selects the part; select Jump: Right selects `W`; Left on `W` selects Jump without collapsing it; Alt+Left/Right change nothing; with 30+ actions hold Down: the selection never leaves the visible area; hold Delete for one second on an action: exactly one row goes, one Ctrl+Z restores it; hold F2: one rename box, text intact. Rename: F2 on Jump, type `Move` (the sibling's name): the tooltip names the collision, Enter keeps the box open, Escape reverts; type `Jump2`, Enter: renamed; blank + click away: reverts; start a rename, Ctrl+Z (global): the box vanishes and Up/Down/Delete/F2 still work; start a rename, then type a search that excludes the action: same. New rows: `+ Action` (toolbar or column header) and the maps `+`: the new row is scrolled into view with a unique name (`Action`, `Action 2`) selected in a rename box; Enter commits, Escape keeps the default. Maps column: focus it, Up/Down move the map selection, F2 renames it, Delete removes it (Ctrl+Z restores); focus the actions column: the same keys act on the row; in the toolbar search box: none fire. Undo: select Jump's new `W` row, Ctrl+Z: the row disappears and the selection falls back to Jump (no dead selection); Ctrl+Y: `W` is back and selected. Preview: toggle Preview and hold Space: the Space row glows. Gamepad (the editor feeds the document the raw `InDevices().Sample` snapshot, EditorAppFrame.cpp:730-734, so a pad reaches the capture outside Play): select `South Button`, Enter, press East: `East Button` with the violet Gamepad pill unchanged, Ctrl+Z restores; select `Left Stick X`, Enter, push the left stick horizontally past half: still `Left Stick X`; vertically: `Left Stick Y`, Ctrl+Z restores; Preview + hold South: the row glows.

- [ ] **Step 3: Commit**

```bash
git add ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp
git commit -m "feat(editor): input document -- keyboard navigation in both columns (Up/Down/Left/Right tree convention, Enter/Delete/F2 without auto-repeat, selection scrolls into view, new rows open in rename); the completing key/click is consumed by the capture (input editor plan T9)"
```

---
### Task 10: The document's Inspector pages (asset / map / action / binding, Live preview, picker)

**Files:**
- Create: `ArcaneEditor/src/Documents/InputActionsInspectorPage.hpp`, `ArcaneEditor/src/Documents/InputActionsInspectorPage.cpp`
- Modify: `ArcaneEditor/src/Documents/InputActionsDocument.hpp` / `.cpp` (Page/PageFor/SelectionKey/RestoreSelection/Resolves/SelectionEpoch/SelectByPath/SourceName)
- Modify: `premake5.lua` (the `ArcaneTests` `files` block: one new entry for `InputActionsInspectorPage.cpp`, added in Step 4)
- Test: `ArcaneTests/src/InputActionsEditorModelTest.cpp` (document-level cases)
- NOT modified here: `ArcaneEditor/src/Documents/InputActionsEditorModel.hpp` / `.cpp` -- `FindNode` and `Resolves` landed in Task 6.

**Interfaces:**
- Consumes: `PropertyGrid` + `PropertyGridState::TextDraft` (Task 1: `TextRow` STORES its commit callable in the draft and `CommitOrphans` may invoke it OUTSIDE `Draw`), `InspectorPage`/`InspectorCrumb::key` (Task 2; `InspectorHost::RepinKey` consumes the keys), the model's `FindNode`/`Resolves`/`SelectionKey` (Task 6), `InputActions::KnownControls`/`DisplayForPath` (Task 5), `InputActionsPreview` + `InputActionsDocumentState` (Task 8).
- Produces:
  ```cpp
  struct InputSelection { Guid map, action, binding, part; };
  class InputActionsInspectorPage final : public InspectorPage {
      struct Services { std::function<void(const Guid&)> beginRebind; const InputActionsDocumentState* state; const InputActionsPreview* preview; };
      InputActionsInspectorPage(InputActionsEditorModel& model, std::string assetName, std::string assetPath, Services services);
      void SetSelection(const InputSelection& sel);
      // Every crumb carries a `key` in the model's 4-segment format (three slashes always): asset root "", map "<map>///", action "<map>/<action>//", binding "<map>/<action>/<binding>/", part all four -- a PINNED instance re-targets itself through InspectorHost::RepinKey; `select` re-selects in the model for an unpinned one.
      std::vector<InspectorCrumb> Breadcrumb() const override;
      void Draw(PropertyGrid& grid) override;
      // private: void Defer(std::function<void()> fn) -- inside Draw the edit is queued past the Draft() references (edit_); outside Draw (an orphan TextRow commit flushed by PropertyGrid::CommitOrphans) it applies at once, so a following Ctrl+S saves it. TextRow commits capture BY VALUE plus a std::weak_ptr<bool> liveness token (alive_): a draft outlives the page that made it.
  };
  // InputActionsDocument overrides: SourceName() = path_.filename().string(); Page() = PageFor(model_.SelectionKey()); PageFor(key) parses the 4-segment key and validates each id through model_.FindNode (nullptr when one is gone; "" IS the asset page); SelectionKey/RestoreSelection/Resolves/SelectionEpoch forward to the model; SelectByPath forwards to model_.SelectByPath.
  ```

- [ ] **Step 1: Write the failing tests** (append to `InputActionsEditorModelTest.cpp`; the file already opens a document via `InputActionsDocument::Open` in its "document saves and reloads" case -- reuse that temp-file idiom)

```cpp
TEST_CASE("input document: is an Inspector source with keyed pages and a breadcrumb", "[editor][input][inspector]")
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "inspector-source-test.arcinput";
    { std::ofstream out(path); out << DocumentJson().dump(2); }
    auto doc = Arcane::Editor::InputActionsDocument::Open(path);
    REQUIRE(doc);
    CHECK(doc->SourceName() == "inspector-source-test.arcinput");
    // Open selects the first map + action (ca171951): the page is the action page.
    const auto e0 = doc->SelectionEpoch();
    REQUIRE(doc->Page() != nullptr);
    auto crumbs = doc->Page()->Breadcrumb();
    REQUIRE(crumbs.size() == 3);
    CHECK(crumbs[0].label == "inspector-source-test.arcinput");
    CHECK(crumbs[1].label == "Player");
    CHECK(crumbs[2].label == "Jump");
    REQUIRE(doc->SelectByPath("Player/Jump/0"));
    CHECK(doc->SelectionEpoch() > e0);
    crumbs = doc->Page()->Breadcrumb();
    REQUIRE(crumbs.size() == 4);
    CHECK(crumbs[2].label == "Jump");
    CHECK(crumbs[3].label == "Space");
    const std::string key = doc->SelectionKey();
    const std::string mapId = "22222222-2222-4222-8222-222222222222", actionId = "33333333-3333-4333-8333-333333333333";
    REQUIRE(crumbs[0].key); CHECK(crumbs[0].key->empty());                       // the asset root re-pins to the asset page
    REQUIRE(crumbs[1].key); CHECK(*crumbs[1].key == mapId + "///");
    REQUIRE(crumbs[2].key); CHECK(*crumbs[2].key == mapId + "/" + actionId + "//");
    REQUIRE(crumbs[3].key); CHECK(*crumbs[3].key == key);
    CHECK(doc->PageFor(*crumbs[1].key) != nullptr);
    CHECK(doc->Resolves(key)); CHECK_FALSE(doc->Resolves("bogus"));
    REQUIRE(crumbs[1].select);
    crumbs[1].select();                                    // the map crumb selects the map
    CHECK(doc->Page()->Breadcrumb().size() == 2);
    REQUIRE(doc->PageFor(key) != nullptr);                 // a pinned page for the binding
    CHECK(doc->PageFor(key)->Breadcrumb().size() == 4);
    CHECK(doc->PageFor("bogus") == nullptr);
    REQUIRE(doc->RestoreSelection(key));
    CHECK(doc->SelectionKey() == key);
    doc->Model().SelectMap({});                            // nothing selected: the ASSET page, never null
    REQUIRE(doc->Page() != nullptr);
    CHECK(doc->Page()->Breadcrumb().size() == 1);
    fs::remove(path);
}
```

- [ ] **Step 2: Run to verify it fails** -- build. Expected: `SourceName` is `Title()` ("inspector-source-test") and `Page()` is null: assertion failures.

- [ ] **Step 3: Write InputActionsInspectorPage.hpp**

```cpp
#pragma once

// The Input Actions document's Inspector pages (input-editor redesign spec
// s2.4): asset / map / action / binding(part), every one drawn with
// PropertyGrid, edits routed through the model (undoable). The Live preview
// block reads the document's preview evaluator while its toolbar's Preview is
// armed. The page is KEYED (SetSelection) so a pinned Inspector can hold one
// binding while the document selects another.

#include "Documents/InputActionsDocumentWidgets.hpp"   // InputActionsDocumentState, InputActionsPreview
#include "Documents/InputActionsEditorModel.hpp"
#include "Panels/InspectorSource.hpp"

#include <functional>
#include <memory>
#include <string>

namespace Arcane::Editor
{
    struct InputSelection
    {
        Guid map, action, binding, part;
    };

    class InputActionsInspectorPage final : public InspectorPage
    {
    public:
        struct Services
        {
            std::function<void(const Guid&)> beginRebind;
            const InputActionsDocumentState* state = nullptr;
            const InputActionsPreview* preview = nullptr;
        };
        InputActionsInspectorPage(InputActionsEditorModel& model, std::string assetName,
                                  std::string assetPath, Services services);
        void SetSelection(const InputSelection& sel) { sel_ = sel; }

        std::vector<InspectorCrumb> Breadcrumb() const override;
        void Draw(PropertyGrid& grid) override;

    private:
        void DrawAsset(PropertyGrid& grid);
        void DrawMap(PropertyGrid& grid, const nlohmann::json& map);
        void DrawAction(PropertyGrid& grid, const nlohmann::json& action);
        void DrawBinding(PropertyGrid& grid, const nlohmann::json& row, bool isPart);
        void DrawLivePreview(PropertyGrid& grid, const Guid& action);
        void DrawPicker(const Guid& target);
        // Edits never mutate the draft under the row loop: inside Draw they are
        // queued (edit_) and run after the last Draft() reference; OUTSIDE Draw
        // -- a TextRow draft deactivated while this page was not drawn, flushed
        // by PropertyGrid::CommitOrphans before any Inspector window Begins --
        // the edit applies at once, so a following Ctrl+S saves it.
        void Defer(std::function<void()> fn) { if (drawing_) edit_ = std::move(fn); else fn(); }

        InputActionsEditorModel& model_;
        std::string assetName_, assetPath_;
        Services services_;
        InputSelection sel_;
        char pickerSearch_[64] = {};
        std::function<void()> edit_;
        bool drawing_ = false;
        // Liveness token for the commits TextRow stores in its draft: a draft
        // outlives the page (the document closed while a box was active), so a
        // stored commit checks the weak_ptr before touching model_.
        std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    };
}
```

- [ ] **Step 4: Write InputActionsInspectorPage.cpp**

```cpp
#include "Documents/InputActionsInspectorPage.hpp"

#include "Widgets/IconsLucide.h"
#include "Widgets/PropertyGrid.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>   // std::strtof (interaction / processor parameters)
#include <map>       // perGroup (DrawAction's Bindings section)

namespace Arcane::Editor
{
    namespace
    {
        Guid IdOf(const nlohmann::json& row)
        {
            if (!row.is_object() || !row.contains("id") || !row["id"].is_string()) return {};
            return Guid::FromString(row["id"].get<std::string>()).value_or(Guid{});
        }
        std::string Str(const nlohmann::json& row, const char* key)
        { return row.is_object() && row.contains(key) && row[key].is_string() ? row[key].get<std::string>() : std::string{}; }
        const nlohmann::json* Find(const nlohmann::json& node, const Guid& id)
        {
            if (!id.IsValid()) return nullptr;
            if (node.is_object())
            {
                if (IdOf(node) == id) return &node;
                for (const auto& [k, child] : node.items()) if (const auto* m = Find(child, id)) return m;
            }
            else if (node.is_array())
                for (const auto& child : node) if (const auto* m = Find(child, id)) return m;
            return nullptr;
        }
        std::vector<std::string> Groups(const nlohmann::json& row)
        {
            std::vector<std::string> out;
            if (row.is_object() && row.contains("groups") && row["groups"].is_array())
                for (const auto& g : row["groups"]) if (g.is_string()) out.push_back(g.get<std::string>());
            return out;
        }
        std::string Joined(const nlohmann::json& arr)
        {
            std::string s;
            if (arr.is_array()) for (const auto& e : arr) if (e.is_string()) { if (!s.empty()) s += ", "; s += e.get<std::string>(); }
            return s;
        }
        nlohmann::json Split(const std::string& csv)
        {
            nlohmann::json out = nlohmann::json::array();
            std::size_t start = 0;
            while (start <= csv.size())
            {
                const std::size_t comma = csv.find(',', start);
                std::string tok = csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                const auto b = tok.find_first_not_of(" \t"), e = tok.find_last_not_of(" \t");
                if (b != std::string::npos) out.push_back(tok.substr(b, e - b + 1));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            return out;
        }
        constexpr const char* kTypes[] = { "Button", "Axis1D", "Axis2D" };
        constexpr const char* kInteractions[] = { "--", "Press", "Hold", "Tap" };
    }

    InputActionsInspectorPage::InputActionsInspectorPage(InputActionsEditorModel& model, std::string assetName,
                                                         std::string assetPath, Services services)
        : model_(model), assetName_(std::move(assetName)), assetPath_(std::move(assetPath)), services_(std::move(services)) {}

    std::vector<InspectorCrumb> InputActionsInspectorPage::Breadcrumb() const
    {
        // `select` re-selects in the model (an unpinned instance follows);
        // `key` is that level's selection key in the model's 4-segment format
        // -- three slashes ALWAYS, PageFor rejects fewer -- so a PINNED instance
        // re-targets its own pin (InspectorHost::RepinKey) and never touches
        // the source. The asset root's key is "": PageFor("") IS the asset page.
        std::vector<InspectorCrumb> crumbs;
        crumbs.push_back({ assetName_, [m = &model_] { m->SelectMap({}); }, std::optional<std::string>{ std::string{} } });
        const auto& draft = model_.Draft();
        if (const auto* map = Find(draft, sel_.map))
        {
            const std::string mapKey = sel_.map.ToString();
            crumbs.push_back({ Str(*map, "name"), [m = &model_, id = sel_.map] { m->SelectMap(id); },
                               std::optional<std::string>{ mapKey + "///" } });
            if (const auto* action = Find(*map, sel_.action))
            {
                const std::string actionKey = mapKey + "/" + sel_.action.ToString();
                crumbs.push_back({ Str(*action, "name"), [m = &model_, map = sel_.map, id = sel_.action]
                                   { m->SelectMap(map); m->SelectAction(id); },
                                   std::optional<std::string>{ actionKey + "//" } });
                if (const auto* binding = Find(*action, sel_.binding))
                {
                    const std::string bindingKey = actionKey + "/" + sel_.binding.ToString();
                    const std::string label = binding->contains("composite")
                        ? (Str(*binding, "composite") == "1DAxis" ? "1D Axis" : "2D Vector")
                        : InputActions::DisplayForPath(Str(*binding, "path")).control;
                    crumbs.push_back({ label, [m = &model_, map = sel_.map, a = sel_.action, id = sel_.binding]
                                       { m->SelectMap(map); m->SelectAction(a); m->SelectBinding(id); },
                                       std::optional<std::string>{ bindingKey + "/" } });
                    if (const auto* part = Find(*binding, sel_.part))
                        crumbs.push_back({ Str(*part, "name") + " · " + InputActions::DisplayForPath(Str(*part, "path")).control,
                                           [m = &model_, map = sel_.map, a = sel_.action, b = sel_.binding, id = sel_.part]
                                           { m->SelectMap(map); m->SelectAction(a); m->SelectBinding(b); m->SelectPart(id); },
                                           std::optional<std::string>{ bindingKey + "/" + sel_.part.ToString() } });
                }
            }
        }
        return crumbs;
    }

    void InputActionsInspectorPage::Draw(PropertyGrid& grid)
    {
        edit_ = nullptr;
        drawing_ = true;   // Defer() queues until the end of this call
        const auto& draft = model_.Draft();
        const auto* map = Find(draft, sel_.map);
        const auto* action = map ? Find(*map, sel_.action) : nullptr;
        const auto* binding = action ? Find(*action, sel_.binding) : nullptr;
        const auto* part = binding ? Find(*binding, sel_.part) : nullptr;
        if (part) DrawBinding(grid, *part, true);
        else if (binding) DrawBinding(grid, *binding, false);
        else if (action) DrawAction(grid, *action);
        else if (map) DrawMap(grid, *map);
        else DrawAsset(grid);
        drawing_ = false;
        if (edit_) edit_();   // AFTER the draw: the draft must not mutate under the rows above
    }

    void InputActionsInspectorPage::DrawAsset(PropertyGrid& grid)
    {
        const auto& draft = model_.Draft();
        if (grid.Section("Asset"))
        {
            PropertyGrid::Rows rows(grid, "##asset");
            if (rows)
            {
                grid.ReadOnlyRow("Name", assetName_);
                grid.ReadOnlyRow("Path", assetPath_);
                std::vector<std::string> mapNames; std::vector<const char*> items; int current = -1; std::vector<Guid> ids;
                if (draft.is_object() && draft.contains("actionMaps") && draft["actionMaps"].is_array())
                    for (const auto& m : draft["actionMaps"]) { mapNames.push_back(Str(m, "name")); ids.push_back(IdOf(m)); }
                for (const auto& n : mapNames) items.push_back(n.c_str());
                const std::string def = draft.value("defaultMap", std::string{});
                for (std::size_t i = 0; i < ids.size(); ++i) if (ids[i].ToString() == def) current = static_cast<int>(i);
                if (const int picked = grid.ComboRow("Default map", items.data(), static_cast<int>(items.size()), current); picked >= 0)
                    edit_ = [m = &model_, id = ids[static_cast<std::size_t>(picked)]] { (void)m->SetDefaultMap(id); };
                std::size_t actions = 0, bindings = 0;
                if (draft.is_object() && draft.contains("actionMaps") && draft["actionMaps"].is_array())
                    for (const auto& m : draft["actionMaps"]) if (m.contains("actions") && m["actions"].is_array())
                        for (const auto& a : m["actions"]) { ++actions; if (a.contains("bindings") && a["bindings"].is_array()) bindings += a["bindings"].size(); }
                grid.ReadOnlyRow("Contents", std::to_string(ids.size()) + " maps · " + std::to_string(actions) + " actions · " + std::to_string(bindings) + " bindings");
            }
        }
        if (grid.Section("Control schemes"))
        {
            PropertyGrid::Rows rows(grid, "##schemes");
            if (rows && draft.is_object() && draft.contains("controlSchemes") && draft["controlSchemes"].is_array())
            {
                // Two schemes may share a name: the row id is the scheme's guid,
                // never its label (ImGui's id-conflict detection would paint the
                // twins red and keyboard activation would toggle both).
                for (const auto& s : draft["controlSchemes"])
                {
                    ImGui::PushID(IdOf(s).ToString().c_str());
                    grid.ReadOnlyRow(Str(s, "name").c_str(), "group " + Str(s, "bindingGroup"));
                    ImGui::PopID();
                }
                if (draft["controlSchemes"].empty()) grid.ReadOnlyRow("(none)", "add one from the toolbar's scheme combo");
            }
        }
    }

    // Row ids are scoped by the TARGET (PushID(id) around every Rows scope,
    // INSIDE the Section so section open-state stays shared per page kind):
    // a TextRow draft is keyed by ImGui id and would otherwise be shared by
    // every map/action/binding that draws a "Name" row (PropertyGrid rule).
    // TextRow commits capture BY VALUE plus the liveness token -- never [&]:
    // the draft STORES the callable and may run it after this frame's locals
    // are gone. Target validation is the model's: SetField -> FindId returns
    // false for a deleted id, and ApplyEdit's before == after guard makes a
    // duplicate delivery a no-op.
    void InputActionsInspectorPage::DrawMap(PropertyGrid& grid, const nlohmann::json& map)
    {
        const Guid id = IdOf(map);
        if (grid.Section("Action map"))
        {
            ImGui::PushID(id.ToString().c_str());
            PropertyGrid::Rows rows(grid, "##map");
            if (rows)
            {
                grid.TextRow("Name", Str(map, "name"), [this, id, w = std::weak_ptr<bool>(alive_)](std::string v)
                             { if (w.expired()) return; Defer([m = &model_, id, v] { (void)m->SetField(id, "name", v); }); });
                bool blocking = map.value("blocking", false);
                if (grid.CheckboxRow("Blocking", blocking)) edit_ = [m = &model_, id, blocking] { (void)m->SetField(id, "blocking", blocking); };
                int priority = map.value("priority", 0);
                if (grid.IntRow("Priority", priority)) edit_ = [m = &model_, id, priority] { (void)m->SetField(id, "priority", priority); };
            }
            ImGui::PopID();
        }
        if (grid.Section("Contents"))
        {
            ImGui::PushID(id.ToString().c_str());
            PropertyGrid::Rows rows(grid, "##contents");
            if (rows)
            {
                std::size_t actions = 0, bindings = 0;
                if (map.contains("actions") && map["actions"].is_array())
                    for (const auto& a : map["actions"]) { ++actions; if (a.contains("bindings") && a["bindings"].is_array()) bindings += a["bindings"].size(); }
                grid.ReadOnlyRow("Actions", std::to_string(actions));
                grid.ReadOnlyRow("Bindings", std::to_string(bindings));
            }
            ImGui::PopID();
        }
    }

    void InputActionsInspectorPage::DrawAction(PropertyGrid& grid, const nlohmann::json& action)
    {
        const Guid id = IdOf(action);
        if (grid.Section("Action"))
        {
            ImGui::PushID(id.ToString().c_str());
            PropertyGrid::Rows rows(grid, "##action");
            if (rows)
            {
                grid.TextRow("Name", Str(action, "name"), [this, id, w = std::weak_ptr<bool>(alive_)](std::string v)
                             { if (w.expired()) return; Defer([m = &model_, id, v] { (void)m->SetField(id, "name", v); }); });
                const std::string type = Str(action, "type");
                int current = 0; for (int i = 0; i < 3; ++i) if (type == kTypes[i]) current = i;
                if (const int picked = grid.ComboRow("Type", kTypes, 3, current); picked >= 0)
                    edit_ = [m = &model_, id, picked] { (void)m->SetField(id, "type", kTypes[picked]); };
                // Interaction: the first token decides the combo; Hold/Tap carry a duration.
                const nlohmann::json interactions = action.value("interactions", nlohmann::json::array());
                std::string first = interactions.is_array() && !interactions.empty() && interactions[0].is_string() ? interactions[0].get<std::string>() : "";
                const std::string name = first.substr(0, first.find('('));
                int kind = name == "press" ? 1 : name == "hold" ? 2 : name == "tap" ? 3 : 0;
                float seconds = kind == 2 ? 0.4f : 0.2f;
                if (const auto d = first.find("duration="); d != std::string::npos) seconds = std::strtof(first.c_str() + d + 9, nullptr);
                auto compose = [](int k, float s) -> nlohmann::json {
                    char buf[48];
                    if (k == 1) return nlohmann::json::array({ "press" });
                    if (k == 2) { std::snprintf(buf, sizeof buf, "hold(duration=%.2f)", s); return nlohmann::json::array({ buf }); }
                    if (k == 3) { std::snprintf(buf, sizeof buf, "tap(duration=%.2f)", s); return nlohmann::json::array({ buf }); }
                    return nlohmann::json::array(); };
                if (const int picked = grid.ComboRow("Interaction", kInteractions, 4, kind); picked >= 0)
                    edit_ = [m = &model_, id, v = compose(picked, picked == 2 ? 0.4f : 0.2f)] { (void)m->SetField(id, "interactions", v); };
                if (kind == 2 || kind == 3)
                    if (grid.FloatRow("Seconds", seconds, 0.01f))
                        edit_ = [m = &model_, id, v = compose(kind, seconds)] { (void)m->SetField(id, "interactions", v); };
                grid.TextRow("Processors", Joined(action.value("processors", nlohmann::json::array())),
                             [this, id, w = std::weak_ptr<bool>(alive_)](std::string v)
                             { if (w.expired()) return; Defer([m = &model_, id, arr = Split(v)] { (void)m->SetField(id, "processors", arr); }); });
            }
            ImGui::PopID();
        }
        if (grid.Section("Bindings"))
        {
            ImGui::PushID(id.ToString().c_str());
            PropertyGrid::Rows rows(grid, "##bindings");
            if (rows)
            {
                std::map<std::string, int> perGroup; int ungrouped = 0;
                if (action.contains("bindings") && action["bindings"].is_array())
                    for (const auto& b : action["bindings"]) { const auto g = Groups(b); if (g.empty()) ++ungrouped; for (const auto& n : g) ++perGroup[n]; }
                for (const auto& [g, n] : perGroup) grid.ReadOnlyRow(g.c_str(), std::to_string(n));
                grid.ReadOnlyRow("Ungrouped", std::to_string(ungrouped));
            }
            ImGui::PopID();
        }
        DrawLivePreview(grid, id);
    }

    void InputActionsInspectorPage::DrawBinding(PropertyGrid& grid, const nlohmann::json& row, bool isPart)
    {
        const Guid id = IdOf(row);
        const bool composite = row.contains("composite");
        if (grid.Section(isPart ? "Composite part" : composite ? "Composite" : "Binding"))
        {
            ImGui::PushID(id.ToString().c_str());
            PropertyGrid::Rows rows(grid, "##binding");
            if (rows)
            {
                if (composite)
                    grid.ReadOnlyRow("Type", Str(row, "composite") == "1DAxis" ? "1D Axis" : "2D Vector");
                else
                {
                    const std::string path = Str(row, "path");
                    grid.ReadOnlyRow("Control", InputActions::DisplayForPath(path).control);
                    // The raw path, editable: a monospace face is not installed
                    // (EditorFonts.hpp has Inter/Roboto/brand only) -- owed.
                    grid.TextRow("Path", path, [this, id, w = std::weak_ptr<bool>(alive_)](std::string v)
                                 { if (w.expired()) return; Defer([m = &model_, id, v] { (void)m->SetField(id, "path", v); }); });
                    static const char* const kButtons[] = { "Rebind...", "Pick..." };
                    const int clicked = grid.ButtonRow("", kButtons, 2);
                    if (clicked == 0 && services_.beginRebind) services_.beginRebind(id);
                    if (clicked == 1) ImGui::OpenPopup("##input_pick");
                    DrawPicker(id);
                }
                if (isPart) grid.ReadOnlyRow("Role", Str(row, "name"));
            }
            ImGui::PopID();
        }
        if (grid.Section("Control schemes"))
        {
            ImGui::PushID(id.ToString().c_str());
            PropertyGrid::Rows rows(grid, "##groups");
            if (rows)
            {
                const auto groups = Groups(row);
                const auto& draft = model_.Draft();
                if (draft.is_object() && draft.contains("controlSchemes") && draft["controlSchemes"].is_array())
                    for (const auto& s : draft["controlSchemes"])
                    {
                        // Keyed by the scheme's guid, not its name: two schemes
                        // may share a name (see DrawAsset).
                        ImGui::PushID(IdOf(s).ToString().c_str());
                        const std::string group = Str(s, "bindingGroup");
                        bool on = std::find(groups.begin(), groups.end(), group) != groups.end();
                        if (grid.CheckboxRow(Str(s, "name").c_str(), on))
                        {
                            nlohmann::json next = nlohmann::json::array();
                            for (const auto& g : groups) if (g != group) next.push_back(g);
                            if (on) next.push_back(group);
                            edit_ = [m = &model_, id, next] { (void)m->SetField(id, "groups", next); };
                        }
                        ImGui::PopID();
                    }
                if (groups.empty()) grid.ReadOnlyRow("(ungrouped)", "applies in every scheme");
            }
            ImGui::PopID();
        }
        if (!composite && grid.Section("Processors"))
        {
            ImGui::PushID(id.ToString().c_str());
            PropertyGrid::Rows rows(grid, "##processors");
            if (rows)
            {
                const nlohmann::json procs = row.value("processors", nlohmann::json::array());
                bool invert = false; float scale = 1.0f; bool hasScale = false;
                if (procs.is_array()) for (const auto& p : procs) if (p.is_string())
                {
                    const std::string t = p.get<std::string>();
                    if (t.rfind("invert", 0) == 0) invert = true;
                    if (t.rfind("scale", 0) == 0) { hasScale = true; if (const auto f = t.find("factor="); f != std::string::npos) scale = std::strtof(t.c_str() + f + 7, nullptr); }
                }
                auto rebuild = [&](bool inv, bool sc, float factor) {
                    nlohmann::json next = nlohmann::json::array();
                    if (procs.is_array()) for (const auto& p : procs) if (p.is_string()) { const auto t = p.get<std::string>(); if (t.rfind("invert", 0) != 0 && t.rfind("scale", 0) != 0) next.push_back(t); }
                    if (inv) next.push_back("invert");
                    if (sc) { char buf[40]; std::snprintf(buf, sizeof buf, "scale(factor=%.3f)", factor); next.push_back(buf); }
                    return next; };
                if (grid.CheckboxRow("Invert", invert)) edit_ = [m = &model_, id, v = rebuild(invert, hasScale, scale)] { (void)m->SetField(id, "processors", v); };
                if (grid.FloatRow("Scale", scale, 0.01f)) edit_ = [m = &model_, id, v = rebuild(invert, true, scale)] { (void)m->SetField(id, "processors", v); };
                grid.ReadOnlyRow("Raw", Joined(procs));
            }
            ImGui::PopID();
        }
        DrawLivePreview(grid, sel_.action);
    }

    void InputActionsInspectorPage::DrawLivePreview(PropertyGrid& grid, const Guid& action)
    {
        const bool armed = services_.state && services_.state->previewArmed && services_.preview;
        if (!grid.Section("Live preview")) return;
        PropertyGrid::Rows rows(grid, "##live");
        if (!rows) return;
        if (!armed) { grid.ReadOnlyRow("Phase", "turn on Preview in the document's toolbar"); return; }
        const InputActionValue v = services_.preview->Value(action);
        const char* phase = v.phase == InputActionPhase::Started ? "Started" : v.phase == InputActionPhase::Performed ? "Performed"
                          : v.phase == InputActionPhase::Canceled ? "Canceled" : "Waiting";
        grid.ReadOnlyRow("Phase", phase);
        grid.ReadOnlyRow("Device", services_.preview->ActiveDevice() == InputDevice::Gamepad ? "Gamepad" : "Keyboard / Mouse");
        char overlay[48];
        if (v.type == InputActionType::Axis2D) std::snprintf(overlay, sizeof overlay, "(%.2f, %.2f)", v.vector.x, v.vector.y);
        else std::snprintf(overlay, sizeof overlay, "%.2f", v.scalar);
        const float magnitude = v.type == InputActionType::Axis2D ? std::sqrt(v.vector.x * v.vector.x + v.vector.y * v.vector.y) : std::fabs(v.scalar);
        grid.MeterRow("Value", magnitude, overlay);
    }

    void InputActionsInspectorPage::DrawPicker(const Guid& target)
    {
        if (!ImGui::BeginPopup("##input_pick")) return;
        // Cleared and focused on EVERY opening: typing goes to the search box
        // and never reaches the document's row key handlers.
        if (ImGui::IsWindowAppearing()) { pickerSearch_[0] = '\0'; ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(260.0f);
        const bool enter = ImGui::InputTextWithHint("##picksearch", ICON_LC_SEARCH " Search controls", pickerSearch_, sizeof pickerSearch_,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }); return s; };
        // Tokenised search: a choice is visible when EVERY whitespace-separated
        // token occurs in "<device> <control> <path>" ("stick x" finds the left
        // AND right stick X rows); no tokens = everything visible.
        std::vector<std::string> tokens;
        {
            std::string cur;
            for (const char* p = pickerSearch_; ; ++p)
            {
                const char ch = *p;
                if (ch == '\0' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    if (!cur.empty()) { tokens.push_back(lower(cur)); cur.clear(); }
                    if (ch == '\0') break;
                }
                else cur.push_back(ch);
            }
        }
        static const std::vector<InputControlChoice> kKnown = InputActions::KnownControls();   // the evaluator's tables, once, in ITS emitted order
        const InputControlChoice* firstVisible = nullptr;
        auto commit = [&](const InputControlChoice& c)
        {
            edit_ = [m = &model_, target, path = c.path] { (void)m->SetField(target, "path", path); };
            ImGui::CloseCurrentPopup();
        };
        ImGui::BeginChild("##picklist", ImVec2(300.0f, 260.0f));
        std::string lastDevice;   // a device header is emitted lazily, when the device changes from the previous VISIBLE row: no empty headers
        for (const auto& c : kKnown)
        {
            const std::string hay = lower(c.display.device + " " + c.display.control + " " + c.path);
            bool visible = true;
            for (const auto& t : tokens) if (hay.find(t) == std::string::npos) { visible = false; break; }
            if (!visible) continue;
            if (c.display.device != lastDevice) { ImGui::TextDisabled("%s", c.display.device.c_str()); lastDevice = c.display.device; }
            if (!firstVisible) firstVisible = &c;
            const char* icon = c.display.device == "Keyboard" ? ICON_LC_KEYBOARD : c.display.device == "Mouse" ? ICON_LC_MOUSE : ICON_LC_GAMEPAD_2;
            const std::string label = std::string(icon) + " " + c.display.control + "##" + c.path;
            if (ImGui::Selectable(label.c_str())) commit(c);
            ImGui::SameLine(200.0f);
            ImGui::TextDisabled("%s", c.path.c_str());
        }
        ImGui::EndChild();
        // Enter picks the FIRST visible choice (UE SKeySelector's search-then-
        // Enter); with nothing visible it does nothing and the popup stays open.
        if (enter && firstVisible) commit(*firstVisible);
        ImGui::EndPopup();
    }
}
```

Then register the new .cpp with the test exe. `ArcaneTests` lists editor sources one by one (`premake5.lua`, the `files {` block of `project "ArcaneTests"`): `InputActionsInspectorPage.cpp` is a LINK dependency of `InputActionsDocument.cpp` (already listed; `Page`/`PageFor` construct the page), so without this entry the test exe fails with LNK2019. Insert directly under Task 7's `InputActionsRows.cpp` line (which sits after `"%{wks.location}/ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp",`):

```lua
        -- Input editor T10: InputActionsInspectorPage is a LINK dependency of
        -- InputActionsDocument.cpp (Page/PageFor construct it); Draw is never
        -- called headlessly, same reason as EditorWidgets.cpp below.
        "%{wks.location}/ArcaneEditor/src/Documents/InputActionsInspectorPage.cpp",
```

Then `./ThirdParty/premake5/premake5.exe vs2026` before the build.

- [ ] **Step 5: Wire the document** (`InputActionsDocument.hpp` / `.cpp`)

Members: `InputActionsInspectorPage page_;` constructed in the ctor init list AFTER `model_`, `state_`, `preview_`: `page_(model_, path_.filename().string(), path_.generic_string(), { [this](const Guid& id) { BeginRebind(id); }, &state_, &preview_ })`. Overrides:

```cpp
        std::string SourceName() const override { return path_.filename().string(); }
        InspectorPage* Page() override { return PageFor(model_.SelectionKey()); }
        InspectorPage* PageFor(std::string_view key) override;
        std::string SelectionKey() const override { return model_.SelectionKey(); }
        bool RestoreSelection(std::string_view key) override { return model_.RestoreSelection(key); }
        bool Resolves(std::string_view key) const override { return model_.Resolves(key); }   // PURE: the host's PruneStale runs it once per frame per history entry
        std::uint64_t SelectionEpoch() const override { return model_.SelectionEpoch(); }
        bool SelectByPath(std::string_view path) override { return model_.SelectByPath(path); }
```

```cpp
    InspectorPage* InputActionsDocument::PageFor(std::string_view key)
    {
        InputSelection sel;
        if (!key.empty())
        {
            std::array<Guid*, 4> slots{ &sel.map, &sel.action, &sel.binding, &sel.part };
            std::size_t start = 0;
            for (std::size_t level = 0; level < 4; ++level)
            {
                const std::size_t slash = key.find('/', start);
                const std::string_view seg = key.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
                if (level < 3 && slash == std::string_view::npos) return nullptr;
                if (!seg.empty())
                {
                    const auto id = Guid::FromString(std::string(seg));
                    if (!id || !id->IsValid()) return nullptr;
                    *slots[level] = *id;
                }
                if (slash == std::string_view::npos) break;
                start = slash + 1;
            }
            // Every named level must still exist (a pinned page for a deleted binding is "gone").
            auto exists = [&](const Guid& id) { return !id.IsValid() || model_.FindNode(id) != nullptr; };
            if (!exists(sel.map) || !exists(sel.action) || !exists(sel.binding) || !exists(sel.part)) return nullptr;
        }
        page_.SetSelection(sel);
        return &page_;   // an empty key IS a page: the asset page (spec A s3.1, container fallback)
    }
```

`FindNode` is the model's const lookup added in Task 6. Add `#include "Documents/InputActionsInspectorPage.hpp"` to the header, and `#include <array>` + `<string_view>` to `InputActionsDocument.cpp` for `PageFor` (Task 8 dropped the header's `<array>` together with `text_`). `page_` is a NEW member line this task adds to `InputActionsDocument.hpp` at the slot Task 8's final-shape listing reserved for it (after `focused_`, before the Task 11 members -- Task 8 wrote its section WITHOUT that line because the type did not exist yet) and is initialised in the ctor init list AFTER `model_`, `state_` and `preview_`, whose addresses it captures.

- [ ] **Step 6: Build and run** -- `./ArcaneTests.exe "[editor][input]"` green. Headless capture as in Task 8 Step 5 (no `--select-in-document` yet -- that is Task 12): Open selects the first map + first action, so the Inspector shows `Player.arcinput > Player > Move` with the Action page for an Axis1D action (Name, Type = Axis1D, Interaction = --, Processors; Bindings: KeyboardMouse 1, Gamepad 1, Ungrouped 0; Live preview: 'turn on Preview ...'). `Player > Jump` is what Task 12's `--select-in-document Player/Jump` produces.

Desk (record each in the report): (1) pin the Inspector on Jump, click Move: the pinned page stays; Window > New Inspector follows; (2) pin on a binding, click the action crumb in the PINNED window: THIS window shows the action page, the follower and the history do not move; (3) click `Pick...`, type `stick x`, Enter: the row reads the left stick X control and the popup closes; click `Pick...` again: the field is empty and focused; Ctrl+Z reverts; (4, container pages) click empty space under the action rows: the breadcrumb drops to `Player.arcinput > Player`; click empty space in the maps column: `Player.arcinput` alone (the asset page); the Inspector never reads `No selection` while the document is the source; (5, mockup step 3 on the document) with `Player.arcinput > Player > Jump` showing, click the Console tab, the Problems tab, the Asset Browser tab, the document's own empty background below the last action, the Outliner's empty space, then activate the Viewport tab and click its background: the breadcrumb still reads `Player.arcinput > Player > Jump` -- if any of them moves the Inspector, a PRODUCER (`SelectionContext::Epoch` or the model's `SelectionEpoch`) bumped on a non-gesture: fix the producer, not the edge; (6, re-select) select Jump, select MeshCube in the Outliner, click Jump again: the Inspector returns to `Player.arcinput > Player > Jump` with no new history entry; the reverse (document page showing, click the already-selected MeshCube): back to `Scene > MeshCube`; (7, Ctrl+S) click into the Inspector on Jump's page with no text field active, Ctrl+S: the tab dot clears and the scene is NOT saved; click into the Name row, type, Ctrl+S while the field is active: the document saves; pin a second instance on Jump, focus the first on the scene page, Ctrl+S: the scene saves, the document does not; (8, hidden-row commit) type into the Name row, then click the Inspector 2 tab so the first instance hides: the typed name is committed (the tab dot appears), Ctrl+Z reverts it; (9, gamepad) hold South with Jump selected: Live preview's Device reads `Gamepad`, Value follows; release and press Space: `Keyboard / Mouse`.

- [ ] **Step 7: Commit**

```bash
git add premake5.lua ArcaneEditor/src/Documents/InputActionsInspectorPage.hpp ArcaneEditor/src/Documents/InputActionsInspectorPage.cpp ArcaneEditor/src/Documents/InputActionsDocument.hpp ArcaneEditor/src/Documents/InputActionsDocument.cpp ArcaneTests/src/InputActionsEditorModelTest.cpp
git commit -m "feat(editor): the input document is an Inspector source -- asset/map/action/binding pages on PropertyGrid, keyed breadcrumb (a pinned instance navigates itself), Live preview block, evaluator-table picker with tokenised search + Enter; orphaned text edits still land (input editor plan T10)"
```

---

### Task 11: Save republishes the game input; Warnings reach the Problems panel

**Files:**
- Modify: `ArcaneEditor/src/Documents/InputActionsDocument.hpp` / `.cpp`, `ArcaneEditor/src/App/EditorApp.hpp`, `ArcaneEditor/src/App/EditorApp.cpp` (:826-830, the `.arcinput` factory), `ArcaneEditor/src/App/EditorAppFrame.cpp:2406-2442`. No engine files: the `LocalInputUser` same-project re-Configure this task relies on landed in Task 5 (Step 4b).

**Interfaces:**
- Consumes: `ClientRuntime::ConfigureGameInput(const InputActionAsset&, const Guid& projectId)` (`ArcaneClient/src/Arcane/Client/ClientRuntime.hpp:81`, forwards to `LocalInputUser::Configure`, whose same-project branch from Task 5 replays the map stack, scheme and dirty overrides and primes the new evaluator), `InputActionsEditorModel::LastValidPreview()` (`const std::optional<InputActionAsset>&`, `InputActionsEditorModel.hpp:27` -- after a successful `Save` it IS the asset just written), `Arcane::Diagnostics::Publish/Clear` (`ArcaneCore/src/Arcane/Base/Diagnostics.hpp:701-704`).
- Produces: `void InputActionsDocument::SetOnSaved(std::function<void(const Guid&, const InputActionAsset&)>)` (invoked exactly once per SUCCESSFUL save, from inside `Save()`, with the asset the model just wrote); `void EditorApp::RepublishGameInput(const Guid& asset, const InputActionAsset& parsed)` (no disk read; applied at the frame boundary from `m_pendingInputRepublish`). The settings.select path (`EditorAppFrame.cpp:2412-2425`) keeps its inline read-parse-configure block untouched: it has no model to hand it a parsed asset.

- [ ] **Step 1: Write the failing test** (append to `InputActionsEditorModelTest.cpp`)

```cpp
TEST_CASE("input document: Save invokes onSaved exactly once per successful save; a refused save does not", "[editor][input]")
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "on-saved.arcinput";
    { std::ofstream out(path); out << DocumentJson().dump(2); }
    auto doc = Arcane::Editor::InputActionsDocument::Open(path);
    REQUIRE(doc);
    int calls = 0;
    Arcane::Guid seen;
    doc->SetOnSaved([&](const Arcane::Guid& g, const Arcane::InputActionAsset&) { ++calls; seen = g; });
    REQUIRE(doc->Save());
    CHECK(calls == 1);
    CHECK(seen == doc->AssetGuid());
    auto invalid = doc->Model().Draft(); invalid["version"] = 99;
    REQUIRE(doc->Model().ApplyEdit("break", doc->Model().Draft(), invalid));
    CHECK_FALSE(doc->Save());                              // refused
    CHECK(calls == 1);                                     // a refused save never fires the callback
    fs::remove(path);
}
```

- [ ] **Step 2: Implement**

Document (`InputActionsDocument.hpp`). Task 8's final-shape listing MARKED these lines as Task 11's and wrote its section WITHOUT them (`grep -n "onSaved_\|diagKey_\|publishedWarnings_\|PublishWarnings\|SetOnSaved" ArcaneEditor/src/Documents/InputActionsDocument.hpp` finds nothing before this step): add them now at the slots that listing reserved (the public `SetOnSaved` after `Preview()`; the private four after Task 10's `page_`), so the header carries exactly these (public / private):

```cpp
        void SetOnSaved(std::function<void(const Guid&, const InputActionAsset&)> fn) { onSaved_ = std::move(fn); }   // Task 11
        // Pushes the republish request from INSIDE Save(): DocumentHost::
        // ConfirmSaveAndClose runs Save() then Close() within DrawAll
        // (DocumentHost.cpp:205 -> :116-120), so a poll after DrawAll would
        // never see a save-and-close. After a successful model Save,
        // LastValidPreview() IS the asset just written -- no disk re-read.
        bool Save() override
        {
            if (!model_.Save(path_)) return false;
            if (onSaved_ && model_.LastValidPreview()) onSaved_(guid_, *model_.LastValidPreview());
            return true;
        }
        ~InputActionsDocument() override { Arcane::Diagnostics::Clear(diagKey_); }
    private:
        void PublishWarnings();   // Task 11
        std::function<void(const Guid&, const InputActionAsset&)> onSaved_;         // Task 11
        std::string diagKey_;                                                     // Task 11: "input:" + guid
        std::vector<std::string> publishedWarnings_;                              // Task 11
```

(`#include <Arcane/Base/Diagnostics.hpp>`, `<functional>`, `<vector>` in the header; the old one-line `bool Save() override { return model_.Save(path_); }` is replaced by the body above.) In the ctor, after `guid_` is known: `diagKey_ = "input:" + guid_.ToString();`. Problems: add the call `PublishWarnings();` as the LAST statement of `Draw`'s `bodyDrawn` branch, directly after `widgets_.Draw(model_, state_, services);` (Task 8 wrote `Draw` without it -- its listing marked that line `// Task 11`), and give the private `PublishWarnings()` its body in `InputActionsDocument.cpp`:

```cpp
    void InputActionsDocument::PublishWarnings()
    {
        std::vector<std::string> warnings = model_.Warnings();
        if (warnings == publishedWarnings_) return;
        publishedWarnings_ = std::move(warnings);
        std::vector<Arcane::Diagnostic> diags;
        for (const auto& w : publishedWarnings_)
        {
            Arcane::Diagnostic d;
            d.severity = Arcane::DiagSeverity::Warning;
            d.scope = Arcane::DiagScope::Assets;
            // The model's three warning families (Task 6): unknown control
            // paths, invalid/duplicate names, binding conflicts.
            d.code = w.rfind("Unknown control path", 0) == 0 ? "input.path.unknown"
                   : w.rfind("Invalid name", 0) == 0         ? "input.name.invalid"
                                                             : "input.binding.conflict";
            d.message = w;
            d.detail = title_ + ".arcinput";
            d.locator = Arcane::DiagLocator::Asset(guid_);
            diags.push_back(std::move(d));
        }
        Arcane::Diagnostics::Publish(diagKey_, diags);
    }
```

App (`EditorApp.hpp`): add the member `std::optional<std::pair<Arcane::Guid, Arcane::InputActionAsset>> m_pendingInputRepublish;   // pushed by a saved input document's onSaved, applied after DrawAll; one slot: two saves in a frame collapse to the last (= the disk state)` (`#include <Arcane/Input/InputActionAsset.hpp>`; `<optional>` and `<utility>` are already included) and the declaration `void RepublishGameInput(const Arcane::Guid& asset, const Arcane::InputActionAsset& parsed);`.

`EditorApp.cpp:826-830`, the `.arcinput` factory lambda -- the ONE place that constructs the concrete type, so no `dynamic_cast` anywhere and Task 4's `EditorDocument&`-typed observer is untouched:

```cpp
        m_documents.RegisterFactory(".arcinput",
            [this](const std::filesystem::path& path) -> std::unique_ptr<Arcane::Editor::EditorDocument>
            {
                auto doc = Arcane::Editor::InputActionsDocument::Open(path, m_undo ? &*m_undo : nullptr);
                if (doc)
                    doc->SetOnSaved([this](const Arcane::Guid& g, const Arcane::InputActionAsset& a)
                    { m_pendingInputRepublish.emplace(g, a); });
                return doc;
            },
            [](const std::filesystem::path& path) -> Arcane::Guid
            { return Arcane::Editor::InputActionsDocument::PeekGuid(path); });
```

`EditorAppFrame.cpp`: the settings.select block at :2412-2425 stays exactly as it is (it reads, parses and configures from disk because it has no model). Add the republish function:

```cpp
    void EditorApp::RepublishGameInput(const Arcane::Guid& asset, const Arcane::InputActionAsset& parsed)
    {
        const auto* project = m_runtime->CurrentProject();
        if (!project) return;
        const auto designated = Arcane::Guid::FromString(project->Manifest().inputActions);
        if (!designated || *designated != asset) return;   // not the project's gameplay input: nothing to republish
        const auto projectId = Arcane::Guid::FromString(project->Manifest().guid);
        if (!projectId) return;
        // Same project, same LocalInputUser: the re-Configure branch (Task 5,
        // LocalInputUser::Configure) replays the pushed map stack, the control
        // scheme and the dirty overrides, and primes the new evaluator with the
        // last snapshot so a control held across the save does not re-fire.
        (void)m_runtime->ConfigureGameInput(parsed, *projectId);
    }
```

and, right after `m_documents.DrawAll(m_viewportDockId);` (:2442):

```cpp
        // Input-editor spec s2.6: a saved input document pushed a republish
        // request from Save(); apply it here, at the frame boundary, whether or
        // not the document still exists (save-and-close destroys it inside
        // DrawAll -- DocumentHost.cpp:205 -> :116-120). One slot: two saves in a
        // frame collapse to the last, which is the disk state.
        if (m_pendingInputRepublish)
        {
            auto req = std::move(*m_pendingInputRepublish);
            m_pendingInputRepublish.reset();
            RepublishGameInput(req.first, req.second);
        }
```

- [ ] **Step 3: Build, run, desk** -- `./ArcaneTests.exe "[editor][input]"` green. Desk: Play (in viewport), open Player.arcinput, rebind Jump's Space to J, Ctrl+S: J jumps in the running session; pause the game so its pause map is pushed (or push any second map from the game), edit + Ctrl+S: the pushed map is still active and the chosen control scheme unchanged; close the dirty tab and choose Save in the confirm: J still jumps (the save-and-close path republishes). Add a duplicate Space binding: the Problems panel lists `Conflicting '<Keyboard>/space': ...` under the asset; rename an action to a sibling's name via a hand-edited file: `Invalid name in Player/Jump: ...` appears; remove it: gone; close the document: rows gone.

- [ ] **Step 4: Commit**

```bash
git add ArcaneEditor/src/Documents/InputActionsDocument.hpp ArcaneEditor/src/Documents/InputActionsDocument.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneEditor/src/App/EditorApp.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/InputActionsEditorModelTest.cpp
git commit -m "feat(editor): a saved input document pushes a republish of the project's gameplay input, applied at the frame boundary (save-and-close included); its Warnings reach the Problems panel as asset diagnostics (input editor plan T11)"
```

---

### Task 12: `--select-in-document`, the report's `inspector` block (schema 11), witness E3, the `editor-input-doc` golden

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.hpp:268-275`, `HostConfig.cpp:115,204,350-367`, `ArcaneRuntime/src/main.cpp:171-176`, `ArcaneEditor/src/main.cpp:197-201` (help comment, insertion after `--tool`), `ArcaneEditor/src/App/EditorApp.cpp:1235-1247` (boot), `:3143` (report), `ArcaneClient/src/Arcane/Host/VerifyReport.hpp:220,476`, `VerifyReport.cpp:604-609,718-719`, `scripts/golden-gate.ps1:329-336,394`
- Test: `ArcaneTests/src/HostConfigTest.cpp`, `ArcaneTests/src/VerifyReportTest.cpp`, `ArcaneTests/src/EditorWitnessTest.cpp`
- Golden: `ReferenceProject/Verify/References/editor-input-doc.png` (new, blessed here)

**Interfaces:**
- Produces: `HostConfig::selectInDocument` (`--select-in-document <map>[/<action>[/<binding index>]]`, editor only, requires `--open-asset`); `void VerifyReport::SetInspector(std::string source, std::string breadcrumb)` emitting `"inspector": {"source": ..., "breadcrumb": ...}`; `kSchemaVersion = 11`.

- [ ] **Step 1: Write the failing tests**

`HostConfigTest.cpp` (after the `--open-asset` case):

```cpp
// --select-in-document (inspector-ownership arc, spec B s3.5): the scripted
// in-document selection a golden needs to show a document page.
TEST_CASE("host config: --select-in-document round-trips and requires --open-asset", "[host]") {
    const auto plain = Run({"--project", "P", "--headless", "--frames", "1"});
    REQUIRE(plain.config.has_value());
    CHECK(plain.config->selectInDocument.empty());
    const auto both = Run({"--project", "P", "--headless", "--frames", "1",
                           "--open-asset", "97260310-8b35-4b29-b12f-1fd6f8e99071",
                           "--select-in-document", "Player/Jump"});
    REQUIRE(both.config.has_value());
    CHECK(both.config->selectInDocument == "Player/Jump");
    const auto orphan = Run({"--project", "P", "--headless", "--frames", "1",
                             "--select-in-document", "Player/Jump"});
    CHECK_FALSE(orphan.config.has_value());   // refused at parse: nothing to select inside
    CHECK(orphan.exitCode == 2);
}
```

(`Run` is the file's helper at :30 returning `HostConfig::ParseOutcome`; `.config` and `.exitCode` are its fields, read exactly as the `bad arg exits 2` case at :84-87 reads them.) `VerifyReportTest.cpp`: change every schema-10 assertion to 11: (a) :962 retitle the case `schemaVersion is 11 and declares a supported range`; (b) :967 `STATIC_REQUIRE(Arcane::VerifyReport::kSchemaVersion == 11);` (a static_assert -- the test exe does not compile until this changes); (c) after the `IsSupportedSchemaVersion(10)` line (:976) add `CHECK(Arcane::VerifyReport::IsSupportedSchemaVersion(11));`; (d) :978 becomes `CHECK_FALSE(Arcane::VerifyReport::IsSupportedSchemaVersion(12));` (`v <= kSchemaVersion`, VerifyReport.hpp:225); (e) the nine `== 10` JSON assertions (:75, :736, :760, :857, :984, :1020, :1041, :1069, :1111) -> 11. Verify: `grep -n "kSchemaVersion == 10\|schemaVersion is 10\|SchemaVersion(12))" ArcaneTests/src/VerifyReportTest.cpp` returns only the new `(12)` line; lines 143/332 are unrelated `== 10`s. Then append:

```cpp
// ---- Inspector ownership arc: schemaVersion 11 -- `inspector` ----
TEST_CASE("schema 11: inspector carries the source and breadcrumb the editor's Inspector resolved to, absent when never set", "[host][verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("dx12", 1, "frames-complete");
    r.SetInspector("Player.arcinput", "Player.arcinput > Player > Jump");
    const auto j = nlohmann::json::parse(r.ToJson());
    REQUIRE(j["schemaVersion"].get<int>() == 11);
    REQUIRE(j.contains("inspector"));
    CHECK(j["inspector"].at("source") == "Player.arcinput");
    CHECK(j["inspector"].at("breadcrumb") == "Player.arcinput > Player > Jump");
    Arcane::VerifyReport silent;
    silent.SetRun("dx12", 1, "frames-complete");
    CHECK_FALSE(nlohmann::json::parse(silent.ToJson()).contains("inspector"));
}
```

`EditorWitnessTest.cpp`:

```cpp
// E3: the INPUT DOCUMENT witness (inspector-ownership spec s5 / input-editor
// spec s4). --open-asset puts Player.arcinput on screen, --select-in-document
// selects Player/Jump inside it, and the report's `inspector` block says the
// document -- not the scene -- owns the Inspector, with the breadcrumb a
// person would read. Compared against its own golden slot once blessed.
TEST_CASE("E3: an opened input document with a scripted selection owns the Inspector and matches the editor-input-doc golden", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e3-input-doc");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneEditor.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "dx12", "--frames", "90",
                 "--settle", "10", "--report", inv.reportPath.generic_string(),
                 "--open-asset", "97260310-8b35-4b29-b12f-1fd6f8e99071",
                 "--select-in-document", "Player/Jump", "--compare", "editor-input-doc" };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.contains("inspector"));
    CHECK(run.report["inspector"].at("source") == "Player.arcinput");
    CHECK(run.report["inspector"].at("breadcrumb") == "Player.arcinput > Player > Jump");
    REQUIRE(run.report.contains("compare"));
    CHECK(run.report["compare"].at("passed") == true);
}
```

- [ ] **Step 2: Run to verify they fail** -- build: `selectInDocument`/`SetInspector` undeclared -- and until Step 4 bumps the constant, the edited STATIC_REQUIRE at :967 is itself a compile error; that is the expected red for this step.

- [ ] **Step 3: HostConfig** -- `HostConfig.hpp` after `openAsset`:

```cpp
        // --select-in-document: editor only, and only WITH --open-asset. A
        // human path inside the opened document -- "<map>[/<action>[/<binding
        // index>]]" for an input asset -- selected after the open, so a golden
        // can show that document's page in the Inspector (inspector-ownership
        // spec s4). Refused at parse without --open-asset (nothing to select
        // inside); an unresolvable path is a loud boot ERROR, the same rule as
        // --select-name. ArcaneRuntime refuses the flag.
        std::string     selectInDocument = "";
```

`HostConfig.cpp`: `cli.Option("select-in-document", "", "editor only, with --open-asset: select this path inside the opened document, e.g. Player/Jump (empty = none)");`, `cfg.selectInDocument = r.Get("select-in-document");`, and beside the `--settle-timeout requires --settle` refusal (:350):

```cpp
        if (r.Supplied("select-in-document") && cfg.openAsset.empty())
        {
            std::fprintf(stderr, "error: --select-in-document requires --open-asset (it selects INSIDE the document that flag opens)\n");
            return { std::nullopt, 2 };
        }
```

`ArcaneRuntime/src/main.cpp` after the `--open-asset` refusal: the same block for `selectInDocument` ("this host has no documents"). `ArcaneEditor/src/main.cpp` help comment: there is NO `--open-asset` entry yet (the flag was added in HostConfig.cpp:115 without documenting it here). Add two entries after `--tool`'s (:197-200, before `--view-mode` at :201), in the block's existing style:

```cpp
//   --open-asset <guid> -- HONOURED: open the document for that asset Guid at
//                          the end of boot (EditorApp::StageFinalize; a bad
//                          Guid / no opener is a loud ARC_ERROR). REFUSED by
//                          ArcaneRuntime (that exe's main.cpp:171-176).
//   --select-in-document <path> -- HONOURED, only WITH --open-asset: select
//                          Map[/Action[/binding index]] inside the opened
//                          document and route it to the Inspector; an
//                          unresolvable path is a loud ARC_ERROR with the run
//                          completing. Refused at parse without --open-asset;
//                          REFUSED outright by ArcaneRuntime.
```

- [ ] **Step 4: Boot + report** (`EditorApp.cpp`)

In the `--open-asset` block (:1235-1247), inside the success branch after `m_scriptedOpenFocusFrames = 3;`:

```cpp
                if (!m_config.selectInDocument.empty())
                {
                    Arcane::Editor::EditorDocument* doc = m_documents.FindByGuid(*guid);
                    if (!doc || !doc->SelectByPath(m_config.selectInDocument))
                        ARC_ERROR("--select-in-document '{}': the opened document has no such path",
                                  m_config.selectInDocument);
                }
```

The report (:3143, after `SetViewMode`):

```cpp
            // THE INSPECTOR (schemaVersion 11, inspector-ownership spec s5):
            // which source the Inspector resolved to and the breadcrumb it
            // shows -- read from the host itself, never from the flags, so a
            // --select-in-document that failed to apply reports the scene.
            {
                Arcane::Editor::InspectorSource& src = m_inspectorHost.Current();
                report.SetInspector(src.SourceName(), Arcane::Editor::InspectorCrumbText(src, src.Page()));
            }
```

(`InspectorCrumbText` is Task 2's inline helper in `Panels/InspectorSource.hpp` -- the same string the history labels and the header tooltips carry; it is reachable through the `InspectorHost.hpp` include `EditorApp.hpp` already has from Task 4.)

`VerifyReport.hpp`: `kSchemaVersion = 11` with the changelog line ("Bumped 10 -> 11 by the inspector-ownership arc: `inspector` {source, breadcrumb} -- see SetInspector. Absent on the runtime host and any run that never set it; 10 remains readable."), and after `SetViewMode`: `void SetInspector(std::string source, std::string breadcrumb);`. `VerifyReport.cpp`: members `bool m_inspectorSet = false; std::string m_inspectorSource, m_inspectorBreadcrumb;`, the setter, and in `ToJson` after `viewMode`: `if (m_inspectorSet) j["inspector"] = { { "source", m_inspectorSource }, { "breadcrumb", m_inspectorBreadcrumb } };`. `golden-gate.ps1:394`: `$script:ReportSchemaMax = 11`.

- [ ] **Step 5: The golden lanes** (`golden-gate.ps1:336`, after the perspective lanes)

```powershell
    # Inspector-ownership arc: the input document open, Player/Jump selected,
    # its page in the Inspector (spec B s4). Shared slot like editor-ui.
    # SelfTestExpect 'Green' because the document tab covers the viewport: the
    # boot-scene mutation the self-test makes is not in this picture (verified
    # by the -SelfTest run in this task's Step 6; flip to 'Failed' if it fails).
    @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-input-doc'; Backend = 'dx12';   ExpectedLevel = 'shared'; ExtraArgs = @('--open-asset', '97260310-8b35-4b29-b12f-1fd6f8e99071', '--select-in-document', 'Player/Jump'); SelfTestExpect = 'Green' }
    @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-input-doc'; Backend = 'vulkan'; ExpectedLevel = 'shared'; ExtraArgs = @('--open-asset', '97260310-8b35-4b29-b12f-1fd6f8e99071', '--select-in-document', 'Player/Jump'); SelfTestExpect = 'Green' }
```

Update the header comment block (:11-19, "Eight combinations": it names all eight lanes the table holds today, the two `f3-cull-blend` lanes included -- `grep -c "@{ Host" scripts/golden-gate.ps1` prints 8) to "Ten combinations" and list the two new lanes. Check the script derives its lane count from the table (`$combos.Count`, never a literal) -- fix any literal `8`/`4` the grep `grep -n "eight\|Eight\| 8 \|four\|Four" scripts/golden-gate.ps1` surfaces in assertions, not just prose.

- [ ] **Step 6: Build, tests, bless, gate**

In this order -- the bless must run against a host that loads the ABI-45 module (Task 5 Step 4c), and the gate is what rebuilds and restages ReferenceProject:

(1) Build. `./ArcaneTests.exe "[host]"` green (HostConfig + VerifyReport).
(2) Run `scripts/golden-gate.ps1 -Configuration Debug` FIRST -- it rebuilds and restages ReferenceProject: expect every pre-existing lane green (eight today -- exactly the eight the header comment names, `f3-cull-blend` included; derive the number from `$combos.Count`, do not recall it) and the 2 new `editor-input-doc` lanes red/NotRun (no reference PNG yet).
(3) Bless the new slot: from the staged editor dir, `rm imgui.ini`, then `MSYS_NO_PATHCONV=1 ./ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 90 --settle 10 --report r.json --compare editor-input-doc --bless --open-asset 97260310-8b35-4b29-b12f-1fd6f8e99071 --select-in-document Player/Jump`; read the produced PNG (the Inspector must show `Player.arcinput > Player > Jump` and the Action page; the document its two columns; no "Open Project Failed" modal) and `r.json` (`inspector.source == "Player.arcinput"`); copy `ReferenceProject/Verify/References/editor-input-doc.png` from the staged dir to the source tree IMMEDIATELY.
(4) Run `./ArcaneTests.exe "[witness][gpu]"` (E1, E2 and the new E3 must all pass; derive the case count from the run, never from memory).
(5) Run the gate again: every lane green (`$combos.Count` = the pre-existing lanes + 2; ten today).
(6) Run `scripts/golden-gate.ps1 -Configuration Debug -SelfTest`: it must pass; if the new lanes fail the self-test, set their `SelfTestExpect = 'Failed'` and say why in the commit.

- [ ] **Step 7: Full baselines** -- `./ArcaneTests.exe "~[gpu]"`: expected 2093 + the cases this plan added (derive the count from the run, do not recall it), 0 failures, 4 skipped. `[gpu]~[witness]`: every case passes (the count is whatever the run prints; the memory baseline was 66 before this plan).

- [ ] **Step 8: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/HostConfig.hpp ArcaneClient/src/Arcane/Host/HostConfig.cpp ArcaneRuntime/src/main.cpp ArcaneEditor/src/main.cpp ArcaneEditor/src/App/EditorApp.cpp ArcaneClient/src/Arcane/Host/VerifyReport.hpp ArcaneClient/src/Arcane/Host/VerifyReport.cpp scripts/golden-gate.ps1 ReferenceProject/Verify/References/editor-input-doc.png ArcaneTests/src/HostConfigTest.cpp ArcaneTests/src/VerifyReportTest.cpp ArcaneTests/src/EditorWitnessTest.cpp
git commit -m "feat(host): --select-in-document beside --open-asset; the report's inspector block (schema 11); witness E3 + the editor-input-doc golden lanes (input editor plan T12)"
```

---

## Spec coverage map (self-review)

| Spec requirement | Task |
|---|---|
| A 3.1 sources, last-selecting wins, focus is not selection, container fallback, closed source releases | 2 (host + edge), 2 Step 4b (SelectionContext epoch), 3, 4, 8 (click-empty-space gesture), 10 |
| A 3.2 pages, breadcrumb with clickable crumbs, `PropertyGrid`, `EditorDocument::Page()`, separate selection models | 1, 2, 3, 10 |
| A 3.3 pin (not lock), closed-source note + unpin on click, New Inspector, back/forward | 2, 4 (pin only for a resolvable page; RepinKey; New Inspector reopens a hidden primary before minting) |
| A 3.4 documents keep their toolbar | 8 (`InputActionsDocumentState`) |
| A 3.5 retirements: PROPERTIES block gone; Material tab + Asset Browser pane untouched (mini-arcs 2/3) | 8; out of scope by decision |
| A 4 `--select-in-document`, `inspector.source` + breadcrumb in the report, `editor-input-doc` golden; pin/instance state per layout | 12 (the gate grows from eight lanes to ten: `editor-input-doc` dx12 + vulkan); 4 (instance ID LIST in the layout ini; pins deliberately not -- Task 4 Step 5; editor-ui goldens DO change for the header row -- see Deviations) |
| A 5 unit (host doubles), witness E-lane, desk | 2, 12; desk: 4 Step 7 (mockup steps 1/3/4/5 on the scene), 10 Step 6 (steps 2/3/4/5 on the document); step 6 n/a (model B rejected) |
| A 6 Asset Browser source (this arc, AFTER the input page proves the seam) | NOT in this plan: mini-arc 2 per spec B s7 |
| A 6.2 pins release on project switch; 6.3 depth 32, prune, skip, never persisted | 2 (PruneStale/InvalidateSource), 4 (ClearSceneReferences hook) |
| B 2.1 toolbar: Add, search, scheme combo + Edit schemes, Preview; no Save, no JSON | 8 |
| B 2.1 / s6 JSON reachable as Assets > Open as text | 8 (Step 0) |
| B 2.2 maps column rows, badges, context menu, selecting pushes the map page | 8, 10 |
| B 2.3 action rows, expanded bindings with icon/readable/part suffix/scheme badge/hover Rebind, composites header+parts, `+ Binding`, rebind capture text, conflicts dot + Problems, reorder (drag + menu), keyboard | 5 (names, chords, canonical keys), 6 (conflicts), 7 (rows), 8, 9, 11 (Problems) |
| B 2.4 pages asset/map/action/binding, Live preview, evaluator-table picker, edits via ApplyEdit | 5, 10 |
| B 2.5 schemes popup, rename renames group references | 8 (model already does) |
| B 2.6 Ctrl+S atomic; republish via ConfigureGameInput | 11 (push-from-Save, applied at the frame boundary), 5 (LocalInputUser same-project re-Configure preserves the map stack, scheme and dirty overrides -- now TRUE) |
| B 3 order: PropertyGrid -> InspectorHost -> Page + signal -> presentation -> flag | 1 -> 2 -> 3/10 -> 7/8/9 -> 12 |
| B 4 model tests incl. Warnings + picker property; host tests; E-input witness; desk with keyboard + gamepad | 5, 6, 2, 12; desk with keyboard + gamepad: 9 Step 2, 10 Step 6 |

**Deviations recorded:** spec A s4 says the editor-ui goldens do not change by the spec alone; the page header the same spec mandates (s3.2 breadcrumb, s3.3 pin + back/forward) is new pixels inside every Inspector window, so `editor-ui` and `editor-ui-perspective` are re-blessed in Task 4 Step 7 -- the scene BODY is unchanged, which Task 1 Step 7 proves with the 8/8 gate (eight lanes on main today, `$combos.Count`) before the header lands. Pins are not persisted (Task 4 Step 5 reason). No monospace face for the Path row (owed: a mono face in EditorFonts). The live glow tints the whole row plus a left bar (desk call in Task 8); per-binding glow uses `BindingValue` (raw, pre-processor). Undo/redo restores the document's selection SILENTLY (no Inspector event): a mechanical change is not a gesture, so Ctrl+Z never steals the Inspector from the scene; the following instance re-resolves the page every frame.

**Owed after this plan (carry to memory):** Asset Browser as a source + preview-pane trim (mini-arc 2); Material tab migration (mini-arc 3); in-editor text editor (mini-arc 4; `Assets > Open as text` hands the file to the OS editor until then); monospace editor font; Aphelyon restamp to ABI 45; undo entries of a CLOSED document are silent no-ops on the shared CommandStack (DraftEditCommand's dead anchor; MeshDocument has the same shape) -- an owner-tagged `CommandStack::Purge(owner)` with its own ABI bump, not this plan; the re-Configure prime (Task 5) leaves `lastPressFrame`/`bufConsumed` residue so `Buffered(action, 6)` can report a held control for six live frames, and `heldTime` restarts for an in-progress Hold -- both need a small `InputActionsImpl` seam.
