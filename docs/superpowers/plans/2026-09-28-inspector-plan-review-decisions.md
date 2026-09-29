# Decided edit list -- inspector-ownership + input-editor plan (fold of both reviews)

Inputs: `2026-09-28-inspector-plan-codebase-review-findings.json` (C01..C39) and `2026-09-28-inspector-plan-ue-review-findings.json` (U01..U42). Plan parts: `_fold/00-header.md .. 12-task.md, 99-tail.md`. Repo facts verified on Arcane main `decb4e41` on 2026-09-28.

How to read: decisions are grouped by TARGET PART in plan order. Each has `id | part | severity | folds: <findings>`, a one-line rationale, the EXACT edit (paste-ready), and a `ripple:` line. Where a decision rewrites a whole file the FINAL text is given once and later decisions reference it. Severity = the highest of the folded findings.

Ids D19, D38 and D39 are intentionally unused (their content folded into D16, D37 and D35). 45 decisions in all.

Standing rules applied everywhere:
- The decided layout and flow stand: one window, one Inspector with sources/pages/pin/instances/history, the two-column input document, the 12-task order. No UE code is copied; only mechanisms.
- Every engine LAYOUT change (InputActions vtable, InputRebindOperation members, LocalInputUser members) lands in Task 5 so there is exactly ONE ABI bump (44 -> 45) and ONE ReferenceProject rebuild (D22).
- Line numbers in the plan are advisory; quoted text anchors and the named greps govern.

## Dedupe map (finding -> decision)

| Findings | Decision |
|---|---|
| C16, C25 | D01 (premake ArcaneTests list) |
| C36 + ABI consequence of U23/U31 | D02, D22, D46 |
| C06, C21, C33 | D04 |
| C09, U07, U28, U10 | D05 (numeric drafts) |
| U08, U09, U11 | D06 (TextRow drafts, CommitOrphans, target-scoped ids) |
| C30 | D07 |
| C01, U02, U35, C13 (scene half) | D08 (selection epoch) |
| U36, U01, C20 | D09 (Resolves / PruneStale / InvalidateSource) |
| U37 (host half), U41 | D10 (re-snapshot, labels, JumpTo) |
| U04, U40 | D11 (instance id pool) |
| U05, U38 | D12 (pin validity) |
| U06 | D13 (RepinKey) |
| U03, U37 (scene half) | D15 (whole-set scene key) |
| C37, U27, U39, U41, U06, U38, U08, U36 (draw ripples) | D16 (final InspectorWindows) |
| U42, U04/U40, U01, U27, U08, C24 (app ripples) | D17 (app wiring) |
| C04, C05, C03, U01, U03, U37, U42, U41, C37 | D18 (Task 4 desk) |
| C34 | D20 |
| C17 | D21 |
| C36 | D22 |
| U21 | D23 |
| U26 | D24 |
| U23 | D25 |
| C08, U31, U32 | D26 (LocalInputUser re-Configure, moved to Task 5) |
| C13 (doc half), U33, U36, C19, C26 | D27 (model selection) |
| U14, U24 | D28 (names) |
| U21 (model half) | D29 |
| C32 | D30 |
| C14 | D31 |
| U17 | D32 |
| C07 | D34 (Open as text) |
| C12, U12, U19, U20, U22, U29, C27, C31 | D35 (document Draw/TickCapture) |
| C10, C11, C14, U12, U15, U17, U18, U20 | D36 (final DrawRow) |
| C02, U13, U14, U18, U24, C24, U17, C39 | D37 (toolbar/maps/actions) |
| C23, C28, U12, U16, U17, U18, U20, C39 | D40 (HandleKeys + HandleMapKeys) |
| C15, U12..U23, U33 (desk) | D41 (Task 9 desk) |
| C19, C26, U06, U08, U09, U25, U36, U38, C38, C02, C05, C13, U27, C15 | D42 (Task 10) |
| U30, C31, U14/U24 diag code, C08/U31 relocation | D43 (Task 11) |
| C18, C35 | D44 |
| C22, C29 | D45 |
| C36 (ordering) | D46 |
| U41 (report) | D47 |
| C03, C05, C14, U31, U34, U32 | D48 (tail) |

---

## 00-header.md

### D01 | 00-header (+ Tasks 1, 2, 7, 10) | BLOCKER | folds: C16, C25
Rationale: ArcaneTests links editor sources by NAME (premake5.lua:1176-~1400); nothing globs them, so every test-linked new .cpp must be listed or the test exe fails with LNK2019 from Task 1 Step 6 on.

EXACT EDIT -- Global Constraints, replace the build bullet's second sentence (`premake5.lua` is glob-based ... before the build.) with:

> `ArcaneEditor` globs its own `src/**.cpp`; **`ArcaneTests` lists editor sources one by one** (`premake5.lua`, the `files {` block of `project "ArcaneTests"`, :1176-~1400, one commented line per file). A new editor `.cpp` that a test or an already-listed file depends on goes into that list IN THE STEP THAT WRITES THE .CPP (an entry for a file that does not exist yet makes MSBuild fail on the missing source), then `./ThirdParty/premake5/premake5.exe vs2026`, then the build. `SceneInspectorSource.cpp` (T3) and `InspectorWindows.cpp` (T4) are NEVER listed: both need `EditorPanels.cpp`, which the test exe deliberately excludes (:1222, :1329).

File Structure > Modify, add: `premake5.lua` -- the `ArcaneTests` `files` block (:1176-~1400): four new entries (T1 PropertyGrid.cpp, T2 InspectorHost.cpp, T7 InputActionsRows.cpp, T10 InputActionsInspectorPage.cpp).

The four entries (each pasted with the neighbours' comment style), and where:
- Task 1 Step 4, after writing PropertyGrid.cpp -- insert after `"%{wks.location}/ArcaneEditor/src/Widgets/ColorPickerPopup.cpp",` (:1281):
```lua
        -- Inspector ownership T1: PropertyGrid (the shared page rows) source-
        -- compiles into the test exe: PropertyGridTest drives it under a real
        -- ImGui context, and InputActionsInspectorPage.cpp (T10) links against it.
        "%{wks.location}/ArcaneEditor/src/Widgets/PropertyGrid.cpp",
```
- Task 2 Step 5, after writing InspectorHost.cpp -- insert after `"%{wks.location}/ArcaneEditor/src/Panels/InspectorMeta.cpp",` (:1224):
```lua
        -- Inspector ownership T2: InspectorHost (pure routing, no ImGui) source-
        -- compiles into the test exe so EditorInspectorHostTest drives it with
        -- fake sources. SceneInspectorSource.cpp / InspectorWindows.cpp stay OUT
        -- (they need EditorPanels.cpp).
        "%{wks.location}/ArcaneEditor/src/Panels/InspectorHost.cpp",
```
- Task 7 Step 4, after writing InputActionsRows.cpp -- insert after `"%{wks.location}/ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp",` (:1208):
```lua
        -- Input editor T7: InputActionsRows (the actions column as pure rows)
        -- source-compiles here for InputActionsRowsTest, and the rewritten
        -- InputActionsDocumentWidgets.cpp above links against it.
        "%{wks.location}/ArcaneEditor/src/Documents/InputActionsRows.cpp",
```
- Task 10 Step 4, after writing InputActionsInspectorPage.cpp -- insert directly under the T7 line above:
```lua
        -- Input editor T10: InputActionsInspectorPage is a LINK dependency of
        -- InputActionsDocument.cpp (Page/PageFor construct it); Draw is never
        -- called headlessly, same reason as EditorWidgets.cpp below.
        "%{wks.location}/ArcaneEditor/src/Documents/InputActionsInspectorPage.cpp",
```
Add `premake5.lua` to the `git add` lines of Task 1 Step 8, Task 2 Step 8, Task 7 Step 6, Task 10 Step 7. Keep each task's Step 2 "premake + build -> header not found" expectation as written.

ripple: 01-task S4/S8, 02-task S5/S8, 07-task S4/S6, 10-task S4/S7.

### D02 | 00-header | BLOCKER | folds: C36 (+ ABI consequences of D25, D26)
Rationale: `ReferenceGame.dll` is built by the separate `ReferenceProject/ReferenceProject.slnx` and bakes `kGamePluginABIVersion` at ITS build; the root postbuild only COPYDIRs `ReferenceProject/` (Binaries included) beside each host. After the bump every host launch refuses the module ("Open Project Failed") until the sample is rebuilt.

EXACT EDIT -- replace the Global Constraints ABI bullet with:

> Every engine LAYOUT change in this plan lands in **Task 5** -- the `InputActions` virtual (`BindingValue`), the `InputRebindOperation` member (`heldModifiers_`), the `LocalInputUser` members (`mapStack_`, `scheme_`) -- so there is ONE bump: `kGamePluginABIVersion` 44 -> 45 at `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:972` with the neighbours' changelog line, `ReferenceProject/ReferenceProject.arcproj:6` `"abi": 44` -> `45` (as 04891dcd did), and the sample module REBUILT before the root build so the postbuild stages a matching DLL (Task 5 Step 4c). `Plugin.cpp:57-66` refuses a 44 DLL in a 45 host outright and the editor then shows the Open Project Failed modal in every capture. Aphelyon's restamp is already owed and absorbs it.

Add after the golden-gate bullet:

> Layout ini: the desk editor writes `%LOCALAPPDATA%\Arcane\editor\layouts\<project-guid>.ini` (`EditorApp.cpp:1585-1592`; ReferenceProject's guid is `cfafaf09-86bb-4b4b-a99f-9ecb0771bc15`), NEVER the exe-dir `imgui.ini`; a `--headless` run pins `io.IniFilename = nullptr` and reads only the committed seed `ReferenceProject/Saved/verify-layout.ini` (`EditorApp.cpp:1562-1581`).

ripple: 05-task (D22), 12-task (D46), 04-task S5/S7 (D17/D18).

### D03 | 00-header | minor | folds: file-structure ripples of D06, D08, D15, D25, D26, D34
EXACT EDIT -- File Structure:
- Create, add: `ArcaneEditor/src/Panels/SceneSelectionKey.hpp` (header-only: the scene's whole-set selection key encode/decode/alive-filter, testable without EditorPanels.cpp); `ArcaneTests/src/SceneSelectionKeyTest.cpp`.
- Modify, add: `ArcaneEditor/src/Scene/SelectionContext.hpp` (`Epoch()`); `ArcaneClient/src/Arcane/Input/InputRebindOperation.cpp` (modifier chords); `ArcaneClient/src/Arcane/Input/LocalInputUser.hpp` / `.cpp` (same-project re-Configure); `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp` + `ReferenceProject/ReferenceProject.arcproj` (ABI 45); `ArcaneEditor/src/Panels/AssetPanelCommon.hpp` / `.cpp`, `ArcaneEditor/src/Panels/AssetBrowserPanel.cpp` (Open as text); `ArcaneEditor/src/Panels/EditorPanels.cpp` (Assets menu + Window menu); tests `SelectionOpsTest.cpp`, `ClientRuntimeTest.cpp`.
- Review Focus item 1: append "Pinned also to Task 2's `PruneStale` / `InvalidateSource` tests: a stale entry is pruned BEFORE a click, and the arrows are never enabled for one."
- Review Focus item 4: append "The key that completes the capture is CONSUMED (same-frame Delete/Enter/F2/arrow/click must not act -- Task 8's `InputSwallowed`), a hidden/unfocused document CANCELS the capture, and a held modifier prefixes a chord (Task 5)."

ripple: none beyond the tasks named.

---

## 01-task.md (PropertyGrid)

### D04 | 01-task | minor | folds: C06, C21, C33
Rationale: the header anchors overshoot EOF (EditorPanels.hpp is 583 lines).

EXACT EDIT -- Files: replace `ArcaneEditor/src/Panels/EditorPanels.hpp:576-625 (InspectorState)` with `ArcaneEditor/src/Panels/EditorPanels.hpp:504-567 (InspectorState; labelColWidth at :553) and :577-582 (DrawInspectorPanel declaration)`. Step 5: `(InspectorState, :625)` -> `(InspectorState, :553)`; `(:649)` -> `(:577-582; drop the bool* open = nullptr parameter at :581 -- selectedAsset stays the last, defaulted parameter, the 8-argument shape the Task 1 call site and Task 3's Draw use)`. Add the sentence: "Line numbers are advisory; the quoted text and the grep sweep govern."

ripple: none.

### D05 | 01-task | major | folds: C09, U07, U28, U10
Rationale: pages rebuild their numeric locals from the JSON draft every frame; ImGui's drag accumulator is consumed by what it applied last frame and the release frame writes nothing, so a mouse drag on Seconds/Scale/Priority commits the untouched original (a no-op SetField). The ROW must own the in-flight number (UE SSpinBox InternalValue), commit once when the released value differs from the activation seed, and Escape mid-drag must restore the seed. IntRow keeps `InputInt` (matches the shipping InputActionsDocumentWidgets.cpp:218 pattern; U07's DragInt switch dropped) but shares the one draft/commit path.

EXACT EDIT -- Step 3, replace the whole `struct PropertyGridState` with the FINAL shape (D06 owns the TextDraft fields):
```cpp
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
```
Add `#include <functional>` to PropertyGrid.hpp. Row declarations become:
```cpp
        bool TextRow(const char* label, std::string_view current,
                     std::function<void(std::string)> commit, bool dimmed = false);   // commit is STORED in the draft (see TextDraft)
        bool CheckboxRow(const char* label, bool& value);
        bool IntRow(const char* label, int& value);                          // true once per gesture, on deactivate-after-edit AND value != seed; value follows the gesture every frame
        bool FloatRow(const char* label, float& value, float speed = 0.01f); // same rule; Escape mid-drag = cancel, no commit
        ...
        // Flush drafts whose box was deactivated while its row was not drawn
        // (window hidden/closed, section collapsed, selection moved): commit
        // the text if it changed, then drop the draft. Call ONCE per frame per
        // state BEFORE any window that draws this state Begins.
        void CommitOrphans();
```
Header comment ("Row primitives return ...") append: "While a drag or typed edit is in flight the row keeps its own draft and writes the in-flight number back into `value` every frame (a page may preview it) but commits nothing; on commit `value` holds the gesture's final number. Escape during a numeric drag cancels (no commit). TextRow selects all its text on activation (single-line rows)."

Step 4, PropertyGrid.cpp includes: add `#include <imgui_internal.h>   // ClearActiveID (numeric-row Escape cancel)` after EditorTheme.hpp. Replace `IntRow` and `FloatRow` bodies with:
```cpp
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
```
Step 1 test: add to `GridHarness` the members `float storedSeconds = 0.40f; int floatCommits = 0; float scale = 1.0f; int scaleCommits = 0;`, and inside the `if (rows)` block after `ReadOnlyRow`:
```cpp
                    float seconds = storedSeconds;   // re-derived from the "model" EVERY frame, as the input pages do
                    if (grid.FloatRow("Seconds", seconds, 0.01f)) { storedSeconds = seconds; ++floatCommits; }
                    if (grid.FloatRow("Scale", scale, 0.01f)) ++scaleCommits;
```
add the helper `void Press(ImVec2 at) { ImGuiIO& io = ImGui::GetIO(); io.AddMousePosEvent(at.x, at.y); Frame(); io.AddMouseButtonEvent(0, true); Frame(); }` and two cases:
```cpp
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
Step 6 expectation: "the three new cases pass; the drag case is the one a re-seeding-blind FloatRow fails."

ripple: 10-task pages need NO change (their per-frame locals stay; `edit_` captures the released value).

### D06 | 01-task | medium | folds: U08, U09, U11
Rationale: a draft whose box deactivates on a frame the row is not drawn (dock tab switched, section collapsed, selection moved) is silently lost, and a label-keyed draft is shared by every target. Store the commit in the draft, flush orphans before any window Begins, scope row ids by target, select-all on activation.

EXACT EDIT -- Step 4, replace `TextRow` with, and add `CommitOrphans`:
```cpp
    bool PropertyGrid::TextRow(const char* label, std::string_view current,
                               std::function<void(std::string)> commit, bool dimmed)
    {
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
```
Header (class comment) add the rule: "A row's ImGui id must include the TARGET's id -- the caller pushes it around the Rows scope (Task 10 does; the scene body's component rows already sit under the entity's id); the label alone is not an identity."

Step 1 test: add `bool drawName = true; int commitsB = 0; std::string nameB = "Beta";` to `GridHarness`; in `Frame()` call `Arcane::Editor::PropertyGrid(state).CommitOrphans();` right after `ImGui::NewFrame()` (before Begin); wrap the Name row: `if (drawName) grid.TextRow("Name", name, [&](std::string v) { name = std::move(v); ++commits; }); else { ImGui::PushID("B"); grid.TextRow("Name", nameB, [&](std::string v) { nameB = std::move(v); ++commitsB; }); ImGui::PopID(); }`. Add the case:
```cpp
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
```
ripple: 04-task D16 (`CommitOrphans` per instance before Begin), 10-task D42 (by-value lambdas + liveness token + `PushID(target)` around each Rows scope), 02-task Interfaces (TextRow signature line).

### D07 | 01-task | minor | folds: C30
EXACT EDIT -- Step 5, replace the final sentence with: "Then `grep -rn "labelColWidth\|DrawInspectorPanel" ArcaneEditor/src ArcaneTests/src`. Rename the DrawInspectorPanel comment mentions to DrawInspectorBody in EditorApp.hpp:1044, InspectorView.hpp:9, InspectorView.cpp:101,106, ComponentCatalog.hpp:27,75, EditorPanels.hpp:420,424, EditorPanels.cpp:2257, EditorAppFrame.cpp:3595 and EditorComponentCatalogTest.cpp:373; reword the two EditorWidgets.cpp:84,92 comments so the width authority is `PropertyGridState::labelColWidth`. Leave FieldGrid's own `labelColWidth` parameter (EditorWidgets.hpp:97,112; EditorWidgets.cpp:159-558) as is -- PropertyGrid::Rows wraps it." Step 8 `git add` becomes: `premake5.lua ArcaneEditor/src/Widgets/PropertyGrid.hpp ArcaneEditor/src/Widgets/PropertyGrid.cpp ArcaneEditor/src/Panels/EditorPanels.hpp ArcaneEditor/src/Panels/EditorPanels.cpp ArcaneEditor/src/Panels/InspectorView.hpp ArcaneEditor/src/Panels/InspectorView.cpp ArcaneEditor/src/Scene/ComponentCatalog.hpp ArcaneEditor/src/Widgets/EditorWidgets.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/PropertyGridTest.cpp ArcaneTests/src/EditorInspectorVectorTest.cpp ArcaneTests/src/EditorComponentCatalogTest.cpp`, preceded by "`git status --short` must show nothing unstaged under ArcaneEditor/ or ArcaneTests/."

ripple: none.

---

## 02-task.md (InspectorSource / InspectorHost)

Decisions D08-D13 together rewrite `InspectorSource.hpp`, `InspectorHost.hpp`, `InspectorHost.cpp` and the test file. The FINAL headers are in D09 and the FINAL .cpp in D10; test edits are collected in D14. Read D08-D13 for the why, D09/D10/D14 for the paste.

### D08 | 02-task (+ 04-task, 06-task) | major | folds: C01, U02, U35, C13 (scene half)
Rationale: a selection EVENT is a user GESTURE (epoch moved) that leaves a non-empty key, never a key delta: re-clicking the already-selected entity must bring the Inspector back from a document (spec A 3.1 "selected LAST"; the mockup's handlers), and a per-frame `Prune` re-primarying a multi-selection must never steal the Inspector. Documents already use an epoch; the scene gets one. `SelectionEdge` stays (as `(epoch, key)`), U02's deletion dropped.

EXACT EDIT:
- Task 2 Files: add `Modify: ArcaneEditor/src/Scene/SelectionContext.hpp (Epoch())`, `Test: ArcaneTests/src/SelectionOpsTest.cpp (one case)`.
- Interfaces: replace the SelectionEdge line with `struct SelectionEdge { std::uint64_t lastEpoch = 0; bool Observe(std::uint64_t epoch, std::string_view key); };   // true = a selection GESTURE (epoch moved) that left a non-empty key` and add: `SelectionContext gains [[nodiscard]] std::uint64_t Epoch() const noexcept -- monotonic, bumped by Select/Toggle/AddRange/Clear only; Prune() does NOT bump it (a prune is not a gesture).`
- New **Step 4b: SelectionContext epoch** (`ArcaneEditor/src/Scene/SelectionContext.hpp`): add `#include <cstdint>`; member `std::uint64_t m_epoch = 0;`; accessor `[[nodiscard]] std::uint64_t Epoch() const noexcept { return m_epoch; }` with the doc comment `// Bumped by every user selection ACTION (Select, Toggle, AddRange, Clear), including a re-select of the already-selected entity; NOT by Prune(), which is a sweep after a registry swap and must not read as the user re-selecting the scene (it would steal the Inspector from a document during undo/redo).`; add `++m_epoch;` as the last statement of `Select`, `Toggle`, `AddRange` and `Clear`. Leave `Prune` untouched and extend its comment with "Does NOT bump Epoch(): a sweep is not a selection gesture (inspector-ownership plan T2)."
- Append to `ArcaneTests/src/SelectionOpsTest.cpp` (add `#include "Scene/SelectionContext.hpp"`):
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
- Step 1: replace the "SelectionEdge: only a new, non-empty key" case with:
```cpp
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
```
- Step 1, extend "the last-selecting source wins" after the `w.Select(w.doc, "")` block:
```cpp
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
```
- Step 8 `git add`: add `ArcaneEditor/src/Scene/SelectionContext.hpp ArcaneTests/src/SelectionOpsTest.cpp`.

ripple: 04-task D17 (`m_sceneSelectionEdge.Observe(m_selection.Epoch(), key)`, priming on project switch / ClearSceneReferences), 06-task D27 (document setters bump on every valid-id call), 99-tail coverage row A 3.1 "2 (host + edge), 2 Step 4b (SelectionContext epoch)".

### D09 | 02-task | major | folds: U36, U01, C20 -- FINAL `InspectorSource.hpp` and `InspectorHost.hpp`
Rationale: staleness must be discoverable through a PURE `Resolves()` so the arrows are truthful and history is pruned on invalidate (spec A 6), the cursor recovers by index arithmetic (never a key search: a->b->a duplicates), and the scene's keys die on EVERY registry swap (New/Open Scene) via `InvalidateSource`, not only on project switch. RemoveSource = mark closed + InvalidateSource + erase.

EXACT EDIT -- Step 3, `InspectorSource.hpp` FINAL (also carries D13's crumb key and D10's crumb-text helper):
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
Step 4, `InspectorHost.hpp` FINAL (carries D08 SelectionEdge, D10 labels/JumpTo, D11 pool, D12 CanPin, D13 RepinKey):
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
Step 6 (`EditorDocument.hpp`): add beside the `RestoreSelection` default: `bool Resolves(std::string_view) const override { return false; }`. Interfaces block: mirror the new members (Resolves, InvalidateSource, PruneStale, JumpTo, BackEntry/ForwardEntry, SetInstanceIds, kMaxInstances, CanPin, RepinKey, HistoryEntry::label, InspectorCrumb::key, InspectorCrumbText, TextRow's std::function per D06).

ripple: 03-task D15 (`SceneInspectorSource::Resolves`), 04-task D16 (`host.PruneStale()` first in the draw) + D17 (`InvalidateSource` in ClearSceneReferences), 06-task D27 (`Resolves` on the model), 10-task D42 (`Resolves` forwarding), 12-task D47.

### D10 | 02-task | medium | folds: U37 (host half), U41 -- FINAL `InspectorHost.cpp`
Rationale: after a successful restore the entry is re-snapshotted from live state so next frame's re-report is an echo (forward history survives a normalized/shrunken key); entries carry a label so the arrows can say "Back to Scene > MeshCube" and a right-click can jump straight to an entry. U41's `SelectionLabel()` virtual dropped (InspectorCrumbText is the one string; Task 12 reuses it).

EXACT EDIT -- Step 5, `InspectorHost.cpp` FINAL (carries D09 prune/invalidate, D11 pool, D12 CanPin, D13 RepinKey):
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
ripple: 04-task D16 (tooltips "Back to %s", right-click jump lists), 12-task D47.

### D11 | 02-task (+ 04-task) | medium | folds: U04, U40
Rationale: ImGui keys dock settings on `###inspector_<id>`; minted ids + a persisted COUNT put a reopened window in a closed slot's dock entry and orphan the survivor's. Ids are a pool of 8 stable slots, lowest-free reuse, persisted as a list. The Window menu stays enabled (`AddInstance() == -1` is a silent no-op; U40's grey-out dropped -- the menu has no host access).
EXACT EDIT: already in D09/D10 (`kMaxInstances`, `AddInstance`, `SetInstanceIds`, no `m_nextId`). Tests in D14. Ini/menu ripple in D17 (`Ids=`), desk in D18.

### D12 | 02-task (+ 04-task) | medium | folds: U05, U38
Rationale: a pin must never hold emptiness (UE's lock exists only while objects are viewed); the right test is `PageFor(key) == nullptr`, NOT an empty key (the input document's empty key IS the asset page). A dead key on a LIVE source is NOT auto-released (U05's second half refuted by its own verdict): registry restore resurrects exact entity ids (delete->undo, Play->Stop), and the draw re-evaluates `PageFor(pinnedKey)` every frame so the page comes back by itself; only a click or RemoveSource releases.
EXACT EDIT: `CanPin()` + the `SetPinned` refusal are in D09/D10. Header ripple: D16 computes `canPin` BEFORE resolving the page and disables the pin button with a "Nothing to pin" tooltip. Add under Task 2 Step 5 the note: "Do NOT auto-release a live source's dead key: RegistryStateCommand.hpp:4-8 -- binary registry restore resurrects EXACT entity ids, so delete->undo and Play->Stop bring the pinned page back by themselves."

### D13 | 02-task (+ 03, 04, 10) | low | folds: U06
Rationale: in a PINNED instance a crumb click moved the SOURCE selection (follower + history move, the clicked window did not). A pinned instance navigates ITSELF: `InspectorCrumb::key` + `RepinKey`.
EXACT EDIT: `InspectorCrumb::key` and `RepinKey` are in D09/D10. Producers: 03-task D15 (scene root crumb `key = std::nullopt`, entity crumb `key = Encode(*m_drawSel)`), 10-task D42 (asset root `""`, map `"<map>///"`, action `"<map>/<action>//"`, binding `"<map>/<action>/<binding>/"`, part all four -- the model's 4-segment format, three slashes always). Consumer: 04-task D16.

### D14 | 02-task | major | folds: tests for D08-D13 (U01, U36, C20, U37, U41, U04/U40, U05/U38, U06, U02)
EXACT EDIT -- Step 1 (`EditorInspectorHostTest.cpp`):
- `FakePage` gains `std::vector<InspectorCrumb> crumbs;` and `Breadcrumb()` returns `crumbs`.
- `FakeSource` gains `std::string normalizeTo;` and `bool Resolves(std::string_view) const override { return restoreOk; }`; `RestoreSelection` becomes `if (!restoreOk) return false; key = normalizeTo.empty() ? std::string(k) : normalizeTo; restored.push_back(key); return true;`.
- "pin holds a page while others select": after the existing `w.host.SetPinned(0, false); CHECK(w.host.SourceFor(0) == &w.scene);` add:
```cpp
    w.scene.restoreOk = false;                       // PageFor(key) is null: nothing to hold
    w.host.SetPinned(second, true);
    CHECK_FALSE(w.host.Find(second)->pinned);
    w.scene.restoreOk = true; w.scene.key.clear();   // an EMPTY key whose page resolves (a document's asset page) IS pinnable
    w.host.SetPinned(second, true);
    CHECK(w.host.Find(second)->pinned);
    CHECK(w.host.Find(second)->pinnedKey.empty());
    w.host.SetPinned(second, false);
```
- "ReleaseAll drops...": replace `(void)w.host.AddInstance(); w.host.SetInstanceCount(3);` with `const int ids[] = { 2, 3, 3, 0, 99 }; w.host.SetInstanceIds(ids);   // {0,2,3}: 0 implicit, dup + out-of-range dropped` (keep both `== 3` CHECKs).
- "history back/forward restores": after `REQUIRE(w.host.History().size() == 3);` add `CHECK(w.host.History()[2].label == "Scene");   // FakePage has no crumbs: the label is the source name`.
- Append these cases:
```cpp
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
- Step 7 expectation: "all cases pass; `[editor]` green." Step 8 commit subject: "... history 32 with prune-on-invalidate (pure Resolves, PruneStale, InvalidateSource), instance pool of 8, labelled entries + JumpTo, RepinKey; SelectionContext epoch; EditorDocument is a source with opt-out defaults (T2)".

ripple: none beyond D08-D13.

---

## 03-task.md (DocumentHost observer + SceneInspectorSource)

### D15 | 03-task | medium | folds: U03, U37 (scene half), D09/D13 ripples
Rationale: the Inspector body is already multi-edit (EditorPanels.cpp:2549/2826 fan out over `sel.Entities()`), so a primary-only key lies about what was pinned/restored, and a pinned multi-selection dies with its primary while the live selection survives via Prune. The key names the WHOLE ordered set, primary first; PageFor/RestoreSelection keep the alive members and fail only when none survive (UE's locked details view). The pure key helpers live in a header so they test without EditorPanels.cpp (D01).

EXACT EDIT -- Files: add `Create: ArcaneEditor/src/Panels/SceneSelectionKey.hpp`, `Test: ArcaneTests/src/SceneSelectionKeyTest.cpp`. Interfaces comment for SceneInspectorSource becomes: `// InspectorSource: SourceName()=="Scene"; SelectionKey()= the whole selection SET "<primary>;<e1>,<e2>,..." (decimal entity values, Entities() order) or ""; RestoreSelection(key) re-selects the ALIVE subset (false only when none survive); Resolves(key) = some member alive; Page()/PageFor(key) return this, PageFor pinning the alive subset of the keyed set; crumbs carry keys for a pinned instance (scene root: none; entity crumb: the drawn set's key)`.

New **Step 3b: SceneSelectionKey.hpp** (header-only, no ImGui, no EditorPanels):
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

#include <charconv>
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
(Add `#include <algorithm>` for `std::find`.)

Step 4 header: drop `KeyOf`/`EntityOf`; add `bool Resolves(std::string_view key) const override;` under `RestoreSelection`; change the `m_pinnedSel` comment to `// PageFor's pinned selection: the alive subset of the pinned key, rebuilt every call`. Step 5 bodies (replace the whole file body after `Alive`):
```cpp
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
```
(`Draw` unchanged. Includes: `#include "Panels/SceneSelectionKey.hpp"`; drop `<charconv>`.) Note under the code: "The key is opaque to InspectorHost; any change to the SET (add, Ctrl-remove, Prune) changes the key, and the epoch decides whether it was a gesture (D08). The pinned key is a snapshot: a member deleted then restored by undo reappears in the pinned page on the next PageFor. `SelectionContext::Toggle` must NOT be used to rebuild a set -- it re-assigns the primary."

Step 1, add a second test file `ArcaneTests/src/SceneSelectionKeyTest.cpp` (`[editor][inspector]`):
```cpp
#include <catch2/catch_test_macros.hpp>
#include <Panels/SceneSelectionKey.hpp>
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
Step 6 expectation: "green, the observer case and the SceneSelectionKey case included." Step 7 `git add`: add `ArcaneEditor/src/Panels/SceneSelectionKey.hpp ArcaneTests/src/SceneSelectionKeyTest.cpp`.

ripple: 04-task D18 desk (pinned multi-selection survives its primary's death), 02-task interfaces (key format is opaque; no host change).

---

## 04-task.md (Inspector windows + app wiring)

### D16 | 04-task | major | folds: C37, U27, U39, U41, U06, U38, U08, U36 -- FINAL `InspectorWindows.hpp/.cpp`
Rationale: (C37) the body must draw even when `Begin` returns false so the scene page's `EditGesture::ScopeGuard` closes an abandoned gesture on a hidden-tab frame (EditGesture.hpp:263-274); (U27) Ctrl+S in an Inspector instance must save the DOCUMENT whose page it shows (UE resolves save to the toolkit that OWNS the panel); (U39) the crumb run is clipped to the space left of the pin and end-scrolled so the leaf stays visible; (U41) arrows name their target and right-click jumps; (U06/U38/U08/U36) crumb keys, pin gating, orphan flush, stale prune.

EXACT EDIT -- Step 1, replace the two result/state structs:
```cpp
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
```
Step 2, `InspectorWindows.cpp` FINAL:
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
Step 6's sentence "Delete the ScopeGuard concern check ..." becomes: "The instance loop draws the body UNCONDITIONALLY after Begin (see the comment in DrawInspectorWindows): DrawInspectorBody's `EditGesture::ScopeGuard` therefore still destructs on every path, the same frame position as before (Phase 18), so its 'last guard to run' ordering with `ShaderEditorDocument::Draw` (EditorAppFrame.cpp:2380) is unchanged." Header comment above `InspectorWindowsState`: add "ids are pool slots, so a closed instance's `[Window]` entry is the one its next opener inherits." Also note: `GetWindowContentRegionMax` (obsolete in 1.92) is no longer used anywhere in this file (C24).

ripple: 04-task D17 (app consumes `saveRequested`/`focusedSource`), 02-task D09-D13, 01-task D06.

### D17 | 04-task | major | folds: U42, U04/U40, U01, U27, U08, C24, U35 (app half)
EXACT EDIT:
- Step 3 (Window menu): unchanged item; add the comment "// Stays enabled: InspectorHost::AddInstance() returns -1 when the pool of 8 is full and the app treats that as a no-op; the menu has no host access."
- Step 4 members: keep `Arcane::Editor::SelectionEdge m_sceneSelectionEdge;` (now epoch-based). Add `Arcane::Editor::InspectorSource* m_inspectorFocusedSource = nullptr;   // latched each Inspector draw; read by the Ctrl+S gate` and `[[nodiscard]] Arcane::Editor::EditorDocument* InspectorSaveTarget(Arcane::Editor::InspectorSource* src) const { return dynamic_cast<Arcane::Editor::EditorDocument*>(src); }   // null for the scene source and for null (EditorDocument derives from InspectorSource, Task 2)`. Reword the comment "the two edge detectors" to "the two epoch watermarks (the scene's SelectionEdge over SelectionContext::Epoch(), the per-document map over SelectionEpoch()) that turn a selection GESTURE into a host event (never focus, never a prune)".
- Step 5 observer `closing` lambda: add `if (m_inspectorFocusedSource == &d) m_inspectorFocusedSource = nullptr;` (the latch is a raw pointer into m_documents).
- Step 5 ini handler: replace the `Count=` design with `Ids=` (extra ids only, comma-separated; an empty `Ids=` line restores {0}):
```cpp
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
```
  Replace the prose "the instance COUNT is layout and is" with: "the instance ID LIST is layout and is -- ids are dock slots (ImGui keys `###inspector_<id>` settings on the id, imgui.cpp:2523), so a closed slot stays closed across a restart and Window > New Inspector reopens the lowest free slot (UE's Details 1..4 rule). The section lands in `%LOCALAPPDATA%\Arcane\editor\layouts\<project-guid>.ini` (EditorApp.cpp:1585-1592) -- never the exe-dir imgui.ini -- and in --headless runs is read only from `ReferenceProject/Saved/verify-layout.ini`, where its absence means {0}."
- Step 6 per-frame sync: replace the comment + scene line with:
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
        m_sceneSource.Bind({ ... unchanged ... });
        if (m_sceneSelectionEdge.Observe(m_selection.Epoch(), m_sceneSource.SelectionKey()))
            m_inspectorHost.NotifySelected(m_sceneSource);
```
  (the Prune at EditorAppFrame.cpp:3581 stays where it is, BEFORE the edge). After the `for (const int id : res.closed)` loop add:
```cpp
            m_inspectorFocusedSource = res.focusedSource;
            for (Arcane::Editor::InspectorSource* src : res.saveRequested)
                if (auto* doc = InspectorSaveTarget(src); doc && !doc->Save())
                    ARC_WARN("Inspector: save refused for '{}'", doc->Title());
```
  Change the gate at EditorAppFrame.cpp:2641 to `const bool docOwnsSave = fs.scSaveScene && (m_documents.FocusedDoc() != nullptr || InspectorSaveTarget(m_inspectorFocusedSource) != nullptr);` and extend its comment: "...or an Inspector instance showing a DOCUMENT's page (the document's Ctrl+S then routes through that instance, InspectorWindows.cpp). An instance on the scene page leaves the scene keybind alone."
- `ConsumeMenuRequests` -- reopen before create (UE's summon-details rule: reuse an open view, else the first CLOSED slot; never a copy beside a closed one). Instance 0 is the standing follower (spec s3.3) and its visibility is the Window > Inspector checkbox:
```cpp
        if (menuReq.newInspector)
        {
            bool* primary = m_panelVis.OpenFlag(Arcane::Editor::PanelId::Inspector);
            if (primary && !*primary) *primary = true;       // hidden follower: bring it back, no new instance
            else (void)m_inspectorHost.AddInstance();        // visible: another instance with its own pin (-1 = pool full, no-op)
        }
```
  Selection events never open an Inspector; only the explicit menu path reopens.
- Project switch block (`EditorAppProject.cpp:2164`) becomes ONLY:
```cpp
        // Inspector ownership (spec decision 2): every non-fallback source
        // releases on a project switch, pins included. The scene half (fallback
        // invalidation + its watermark) lives in ClearSceneReferences, which
        // this function calls below (:2168).
        m_inspectorHost.ReleaseAll();
        m_docSelectionEpochs.clear();
```
  and in `EditorApp::ClearSceneReferences()` (`EditorAppScene.cpp:150`, right after `m_inspector = {};`) add:
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
  (ClearSceneReferences runs on New Scene :156, Open Scene :184 and from ResetPerProjectState :2168.)
- Step 8 `git add`: add `ArcaneEditor/src/App/EditorAppScene.cpp`.

ripple: 02-task D08/D09/D11, 03-task D15, 10-task D42 desk, 11-task (nothing: the republish goes through the factory, D43).

### D18 | 04-task | minor | folds: C04, C05, C03, U01, U03, U37, U42, U41, C37, U12 (desk)
EXACT EDIT -- Step 7, replace the desk list with:
> Desk (check `Get-Process ArcaneEditor` first): launch `ArcaneEditor.exe --project ReferenceProject`, verify and record each: (1) selecting MeshCube in the Outliner shows `Scene > MeshCube` with the same component sections; (1b, mockup step 3, focus is not selection) with `Scene > MeshCube` showing, click the Console tab, the Problems tab, the Asset Browser tab and an Asset Browser row, the Outliner's empty space below the last entity, activate then leave the Viewport tab: the breadcrumb still reads `Scene > MeshCube`; (2) clicking the viewport background clears the selection: the page says "No selection" and the pin button is disabled with the tooltip "Nothing to pin"; (3) Window > New Inspector docks a second tab beside the first; pin it, select another entity: the pinned one holds; pin an instance on a binding-less multi-selection (ctrl-click three entities), delete the primary: the pinned page shows the two survivors with "(+1)"; (4) back/forward walks the selections; hovering Back reads "Back to Scene > <name>"; right-click Back lists prior entries nearest-first and clicking one lands there in one step; (5) close the second instance with its X; (5b) uncheck Window > Inspector (the primary hides), then Window > New Inspector: the primary reappears and no "Inspector 2" is created; with the primary visible, New Inspector still adds Inspector 2; (6) open a third instance, close the SECOND, drag the third somewhere distinct, restart: `Inspector 3` returns in that spot (`[EditorInspector][Instances]` in `%LOCALAPPDATA%\Arcane\editor\layouts\cfafaf09-86bb-4b4b-a99f-9ecb0771bc15.ini` shows `Ids=2`; no `Inspector 2` window), then Window > New Inspector reopens `Inspector 2` in its old slot; open extras until New Inspector stops adding (8 total); (7) create an entity (Outliner +), ctrl-click it as a second member, Ctrl+Z (the creation): the Inspector and history do not move (a prune is not a gesture); (8) pin the primary on MeshCube, File > Open Scene a different scene: the pinned instance reads "Pinned selection is gone -- click to follow the selection", never another entity, and Back is disabled (history pruned); (9) Play then Stop with an instance pinned on MeshCube: the pin still shows MeshCube; (10) drag a Position field in Inspector 1 and, mid-drag, click the Inspector 2 tab: Ctrl+Z reverts exactly that drag and a following gizmo drag is its own undo step; (11) `git status` shows no `imgui.ini` in the exe dir touched by the desk (the layout lives in %LOCALAPPDATA%).
> Then the goldens (unchanged procedure). Add to the Files line: "Golden: re-bless `editor-ui` and `editor-ui-perspective` (the header row is new pixels -- a recorded deviation from spec A s4, see 99-tail)."

ripple: 99-tail D48.

---

## 05-task.md (engine growth -- the ONE ABI bump)

Task 5 title becomes "Engine growth -- control tables, readable names, canonical keys, per-binding value, rebind timer + modifier chords, LocalInputUser same-project re-Configure, ABI 45 + ReferenceProject rebuild". Files: add `ArcaneClient/src/Arcane/Input/InputRebindOperation.cpp`, `ArcaneClient/src/Arcane/Input/LocalInputUser.hpp` / `.cpp`, `ReferenceProject/ReferenceProject.arcproj`; Test: add `ArcaneTests/src/ClientRuntimeTest.cpp`.

### D20 | 05-task | BLOCKER | folds: C34
Rationale: `InputActionAsset::FromJson` rejects a non-empty `actionMaps` without `defaultMap` (InputActionAsset.cpp:426-429); `REQUIRE(asset)` fails on every run.
EXACT EDIT -- Step 1 BindingValue fixture, first line of the raw string: `"version": 1, "id": "11111111-1111-4111-8111-111111111111", "defaultMap": "22222222-2222-4222-8222-222222222222",`. Add under the test: "`defaultMap` is REQUIRED by FromJson when actionMaps is non-empty (InputActionAsset.cpp:426-429)."
ripple: none.

### D21 | 05-task | major | folds: C17
EXACT EDIT -- Step 4, replace "every `return { Source, code };` kept" with: "every `return { Source, code };` becomes `return ControlId{ Source, code };` (a braced pair cannot copy-list-initialise a `std::optional<ControlId>`), and the leading `if (path.empty()) return {};` (:180, no warning today) becomes `if (path.empty()) return ControlId{};` so an empty path stays a silent None as today. `<optional>` is already reachable (InputActions.hpp:21)."
ripple: none.

### D22 | 05-task | BLOCKER | folds: C36 (+ D02)
EXACT EDIT -- replace "ABI: bump the plugin ABI integer (Global Constraints), one line." with a new **Step 4c: ABI 45 + the sample module**:
> (1) `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:972`: `kGamePluginABIVersion = 44` -> `45`, with the neighbours' changelog line: "45 (2026-09-28, inspector-ownership/input-editor arc): `InputActions` gained the pure virtual `BindingValue`; `InputRebindOperation` gained `heldModifiers_`; `LocalInputUser` (held by value inside ARCANE_API `ClientRuntime`) gained `mapStack_`/`scheme_`. A v44 module was compiled against the old vtable and layouts; reject the pairing. ReferenceProject.arcproj restamped." (2) `ReferenceProject/ReferenceProject.arcproj:6`: `"abi": 44` -> `45`. (3) BEFORE the root MSBuild of Step 5, rebuild the sample module against the new headers so the root postbuild stages a matching DLL: `cd ReferenceProject && ../ThirdParty/premake5/premake5.exe vs2026 && "<MSBuild.exe>" ReferenceProject.slnx -t:Rebuild -p:Configuration=Debug -m -nr:false -v:m -nologo && cd ..` (ReferenceGame.dll bakes the ABI at ITS build; the root premake's `{COPYDIR} ReferenceProject` postbuild then stages `Binaries/` beside each host). (4) Then the root premake + MSBuild. Verify: `ArcaneEditor.exe --project ReferenceProject --headless --frames 5` boots with no "Open Project Failed" modal and `--print-engine-info` (or the boot log) reports ABI 45.
Step 6 `git add`: add `ReferenceProject/ReferenceProject.arcproj ArcaneClient/src/Arcane/Input/InputRebindOperation.cpp ArcaneClient/src/Arcane/Input/LocalInputUser.hpp ArcaneClient/src/Arcane/Input/LocalInputUser.cpp ArcaneTests/src/ClientRuntimeTest.cpp`.
ripple: 12-task D46 (gate ordering), 11-task D43 (no engine files there).

### D23 | 05-task (+ 06-task D29) | high | folds: U21
Rationale: `Conflicts()` compared raw path strings; the capture writes `<Keyboard>/scancode/x` while assets author `<Keyboard>/x`, `<Mouse>/button/1` == `<Mouse>/leftButton`, `a+b` == `b+a`. Compare the COMPILED identity (UE compares resolved FKeys).
EXACT EDIT -- Step 3 header, beside `IsKnownControlPath`: `// The compiled identity of a control path, spelling-independent: two paths that drive the same physical control on the running layout yield the same key. Empty when any part fails to compile.` `[[nodiscard]] static std::string CanonicalControlKey(std::string_view path);`. Step 4: hoist `CompilePath`'s '+'-split rule (InputActions.cpp:284-302, a '+' inside `<...>` is not a separator) into an anonymous-namespace `std::vector<std::string> SplitChordParts(std::string_view path)` and make BOTH `IsKnownControlPath` (replace the plan's `path.find('+')` loop) and `CanonicalControlKey` use it. `CanonicalControlKey`: for each part `TryCompileSinglePath(part)`; nullopt -> return ""; if `id.source == ControlSource::Scancode`, translate with `SDL_GetKeyFromScancode((SDL_Scancode)id.code, SDL_KMOD_NONE, false)` (the exact call InputDevices.cpp:143-144 uses) and, when the result is not `SDLK_UNKNOWN`, rewrite the part to `{ ControlSource::Keycode, kc }` (an untranslatable scancode stays a Scancode part); render each part as `std::to_string((int)id.source) + ':' + std::to_string(id.code)`; sort; join with '+'. Step 1 tests, append to InputActionsTest.cpp:
```cpp
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
```
ripple: 06-task D29 (`Conflicts` compares `key`).

### D24 | 05-task | low | folds: U26
Rationale: captured paths carry SDL's lower-cased name ("left shift"); the camel-split fallback renders "Left shift" beside an authored "Left Shift". Resolve through the compiler's own lookup. U26's optional LOVE-token emission in Observe is NOT applied (D23 makes spelling irrelevant for conflicts).
EXACT EDIT -- Step 4 `ReadableControl` Keyboard branch: replace `if (control.starts_with(scan)) control.erase(0, scan.size());` with `const bool isScancode = control.starts_with(scan); if (isScancode) control.erase(0, scan.size());` and, after the F-key check and before the camel-split fallback, insert:
```cpp
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
```
(order: LOVE table, letters/F-keys, SDL lookup, camel-split; InputActions.cpp already includes the SDL keyboard header -- confirm with grep). Step 1 DisplayForPath test, append: `d = InputActions::DisplayForPath("<Keyboard>/scancode/left shift"); CHECK(d.control == "Left Shift"); d = InputActions::DisplayForPath("<Keyboard>/scancode/page down"); CHECK(d.control == "Page Down"); d = InputActions::DisplayForPath("<Keyboard>/scancode/keypad 1"); CHECK(d.control == "Keypad 1");`. Step 3 comment above `InputControlDisplay`: add "Captured scancode paths display with SDL's canonical name, resolved by the compiler's own lookup."
ripple: none.

### D25 | 05-task | medium | folds: U23
Rationale: `Observe` completes on the first newly-down scancode, so Shift then A writes "left shift" the instant Shift goes down although '+' chords are first-class in the compiler and the plan's own tests. Modifiers accumulate; the first non-modifier completes with them prefixed; a lone modifier binds on its release only when no other modifier is still held (UE SInputKeySelector rule).
EXACT EDIT -- Interfaces: after the `Remaining()` line add `// Observe: modifier keys accumulate; the first non-modifier key completes with the held modifiers prefixed ("<Keyboard>/scancode/lshift+<Keyboard>/scancode/a"); a modifier's release completes with the bare modifier only when no other modifier is still held.` Step 3 `InputRebindOperation.hpp`: private `std::vector<uint32_t> heldModifiers_;   // modifier scancodes newly pressed during this capture, in press order` (`#include <vector>`); `Begin` clears it. Step 4, new sub-step "InputRebindOperation.cpp keyboard branch (:48-59)": (1) file-local `IsModifierScancode(sc)` for SDL_SCANCODE_LCTRL..RGUI (224..231) and a table mapping each to its LOVE token (`lctrl`, `lshift`, `lalt`, `lgui`, `rctrl`, `rshift`, `ralt`, `rgui`) so the path round-trips through `LoveToSdlName`; (2) `wantCaptureKeyboard` skips the whole branch as today; (3) first pass -- every modifier newly down (not down in `previous_`) is appended to `heldModifiers_`; every held modifier now up is erased and, if `heldModifiers_` is then empty AND no other modifier scancode is down, complete with `"<Keyboard>/scancode/" + token`; (4) second pass -- the first NON-modifier scancode newly down completes with `join(held tokens as "<Keyboard>/scancode/<tok>", "+") + "+" + "<Keyboard>/scancode/<name>"` (bare when none held); (5) a Kbm mouse-button completion (:60-70) gets the same prefix (`<Keyboard>/scancode/lshift+<Mouse>/leftButton` compiles); gamepad completions ignore it; (6) modifiers already down at `Begin` are NOT seeded (the initiating control must be released and re-pressed -- the existing rule); (7) `previous_ = snapshot` stays last. Prefix order = press order (ResolveChord requires every part down; order is display-only). Step 1, append to InputRebindOperationTest.cpp (LSHIFT = 225, LCTRL = 224, A = 4; `Binding()` is the file's fixture):
```cpp
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
Existing InputRebindOperationTest cases that press a bare letter/mouse button are unaffected; run the file and fix any case that pressed a modifier expecting immediate completion (none known).
ripple: 08-task D35 (TickCapture consumes `replacementPath` unchanged; DisplayForPath renders "Left Shift + A"), 09-task D41 desk.

### D26 | 05-task (moved from 11-task) | major | folds: C08, U31, U32
Rationale: `LocalInputUser::Configure` (LocalInputUser.cpp:15-39) is the cold-start path: `Clear()` then `SetBaseMap(default)` + `LoadProfile` from disk, so a save-triggered re-Configure collapses the pushed map stack, drops the scheme, cancels an in-flight capture, discards dirty overrides, and the fresh evaluator re-fires a held control. Spec B 2.6 promises the opposite. Engine-side, so it lives in Task 5 (one ABI bump, D02).
EXACT EDIT -- new **Step 4b: LocalInputUser same-project re-Configure**: `LocalInputUser.hpp` gains `std::vector<Guid> mapStack_;` (SetBaseMap = {map}; PushMap appends on success; PopMap pops) and `std::string scheme_;` (SetControlScheme stores on success; SetBaseMap/Clear do not touch it), plus `[[nodiscard]] std::optional<Guid> ActiveMap() const { return mapStack_.empty() ? std::nullopt : std::optional<Guid>{ mapStack_.back() }; }`. `Configure` gains a RE-ENTRY branch when `asset_.has_value() && projectId == projectId_`: build `next` and `next->LoadAsset(asset)` as now (return false on failure, live session untouched); then `next->Update(0.0, lastSnapshot_);` (prime: prev/cur both hold the held state so the first live tick sees no edge; dt = 0 adds no hold time; no base map yet so no fixed-step transition is queued -- UE's "held key on a rebuilt mapping is ignored until release"); then instead of `Clear()`+`LoadProfile`: (a) `actions_ = std::move(next); asset_ = asset;` (b) replay `mapStack_` by id through `MapName` -- first resolving id `SetBaseMap`, later ones `PushMap`ed, unresolvable ids skipped; none resolve -> `asset.defaultMap`; rebuild `mapStack_` from the survivors; (c) non-empty `scheme_` re-applied via `actions_->SetControlScheme`, cleared if it no longer exists; (d) `profile_.Dirty()` -> keep the in-memory profile and `ApplyProfile()` (overrides whose binding id vanished fail `SetBindingPath` and drop with the existing WARN -- "compatible overrides"), else `LoadProfile(profileName_)`; (e) `rebind_ = {}`. The cold-start branch is unchanged. Step 1, append to `ArcaneTests/src/ClientRuntimeTest.cpp` (`[client][input]`; build the three assets inline with `InputActionAsset::FromJson(nlohmann::json::parse(R"(...)"))` in the shape of the BindingValue fixture, with `defaultMap`, maps "gameplay"(id 2222...) + "menu"(id 3333...), action Jump (id 4444...) bound to `<Keyboard>/space` (id 5555...), controlSchemes `[{"id":"aaaa...","name":"Gamepad","bindingGroup":"Gamepad"}]`; assetB = same ids with Jump's path `<Keyboard>/k`; assetC = assetB without "menu"):
```cpp
TEST_CASE("LocalInputUser re-Configure for the same project replays the map stack, scheme and dirty overrides", "[client][input]")
{
    Arcane::LocalInputUser user;
    REQUIRE(user.Configure(assetA, projectId));
    REQUIRE(user.PushMap(menuId));
    REQUIRE(user.SetControlScheme("Gamepad"));
    REQUIRE(user.SetOverride(jumpBindingId, "<Keyboard>/j"));
    REQUIRE(user.Configure(assetB, projectId));                     // the save's republish
    CHECK(user.ActiveMap() == menuId);                              // pushed map survived
    CHECK(user.AuthoredPath(jumpBindingId) == "<Keyboard>/k");      // the edit is live
    CHECK(user.ProfileDirty());                                     // unsaved override kept
    CHECK(user.BindingDisplayString(jumpBindingId) == "Keyboard J");
    REQUIRE(user.Configure(assetC, projectId));                     // "menu" removed
    CHECK(user.ActiveMap() == gameplayId);                          // collapsed to the surviving base
    REQUIRE(user.Configure(assetC, otherProjectId));                // a different project: cold start
    CHECK_FALSE(user.ProfileDirty());
}

TEST_CASE("ClientRuntime: a re-Configure does not re-fire a control held across it", "[client][input]")
{
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    const auto project = *Arcane::Guid::FromString("55555555-5555-4555-8555-555555555555");
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
(Adjust the `FindAction("Player","Jump")` names to `RuntimeInputAsset()`'s at ClientRuntimeTest.cpp:67-79.) Step 5 run: `./ArcaneTests.exe "[input]"` and `./ArcaneTests.exe "[client][input]"` green. Commit subject: append "; LocalInputUser re-Configure for the same project replays the map stack, scheme and dirty overrides and primes the new evaluator (spec B 2.6); rebind capture accumulates modifiers into a '+' chord; CanonicalControlKey; ABI 45 + ReferenceProject restamped and rebuilt".
ripple: 11-task D43 (no engine work there; desk keeps "pause map survives Ctrl+S"), 99-tail D48 (owed: Buffered residue + heldTime carry-over across the prime).

---

## 06-task.md (model growth)

### D27 | 06-task | major | folds: C13 (doc half), U33, U36 (model half), C19, C26
Rationale: (C13) re-clicking the already-selected row must re-assert the Inspector, so a setter called with a VALID id always bumps the epoch (clears bump only on change); (U36) staleness needs a PURE `Resolves`; (C19/C26) Task 10 needs the model's const lookup `FindNode`; (U33) undo/redo of a structural edit left the selection naming a dead row -- the command restores the selection live at push time (undo) / at undo time (redo), trimmed to the deepest surviving ancestor. That restore is SILENT (no epoch bump): a mechanical change is not a gesture (U35's rule), otherwise Ctrl+Z while the scene is current would steal the Inspector; the following instance re-resolves `Page()` every frame anyway.

EXACT EDIT -- Interfaces (Produces), add/replace:
```cpp
  [[nodiscard]] std::uint64_t SelectionEpoch() const noexcept;      // bumps on EVERY Select* call with a valid id (a re-select is a gesture) and on a clear that changes something
  [[nodiscard]] bool Resolves(std::string_view key) const;           // PURE: would RestoreSelection(key) succeed? no selection, no bump
  [[nodiscard]] const nlohmann::json* FindNode(const Guid& id) const; // nullptr when no node carries that id (Task 10 PageFor uses it)
  // private: bool ParseKey(std::string_view, std::array<Guid,4>&) const; void SetSelectionSilently(const std::array<Guid,4>&); void RestoreSelectionOrAncestor(std::string_view key);  // undo/redo: silent
```
Step 3 header: declare the above; `DraftEditCommand` (friend/private access as `RestoreDraft`) gains `std::string undoKey_, redoKey_;`.

Step 4 setters (replace the four):
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
```
Key parsing (replace `RestoreSelection`):
```cpp
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
with a const overload of the file-local helper beside :51: `const nlohmann::json* FindId(const nlohmann::json& node, const Guid& id)` (same body over const refs). `DraftEditCommand`: constructor takes `undoKey` (captured by `ApplyEdit` as `SelectionKey()` BEFORE `RestoreDraft(after)`); `Undo()`: `if (auto m = Model()) { redoKey_ = m->SelectionKey(); m->RestoreDraft(before_); m->RestoreSelectionOrAncestor(undoKey_); }`; `Redo()`: `if (auto m = Model()) { undoKey_ = m->SelectionKey(); m->RestoreDraft(after_); m->RestoreSelectionOrAncestor(redoKey_); }`. The redo-side key is captured AT UNDO TIME because callers such as `AddBinding` (:305-306) select the new row only after `ApplyEdit` returns. `ApplyEdit` itself never touches the selection. Replace the hedged note "if its signature is non-const, add a const overload" with the mandatory sentence above.

Step 1 tests: in "selection epoch, key and restore" change `model.SelectMap(map); // no change: no bump` + `CHECK(... == e0 + 1)` to `model.SelectMap(map);   // re-select: a gesture, bumps` + `CHECK(model.SelectionEpoch() == e0 + 2);`, and the later `== e0 + 3` to `== e0 + 4`; after `REQUIRE(model.RemoveBinding(...))` add `CHECK_FALSE(model.Resolves(key)); const auto eR = model.SelectionEpoch(); CHECK_FALSE(model.RestoreSelection(key)); CHECK(model.SelectionEpoch() == eR);   // Resolves/RestoreSelection failure: pure` and `CHECK(model.FindNode(action) != nullptr); CHECK(model.FindNode(binding) == nullptr);`. Append:
```cpp
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
```
ripple: 10-task D42 (`PageFor` uses `model_.FindNode`; `Resolves` forwarding), 04-task D17 (the poll is unchanged: `epoch != last`), 08-task D36 (`SelectRow` unchanged).

### D28 | 06-task (+ 08, 11) | high | folds: U14, U24
Rationale: the runtime's `LoadAsset` REFUSES the whole asset on a duplicate map name or a duplicate action name within a map (InputActions.cpp:757-775), and `FindAction(name)` is ambiguous across maps; the plan committed any non-empty rename and stacked "Action"/" Copy" defaults. Uniqueness is enforced at creation (numbered suffix) and at commit (validator); names compare trimmed and CASE-SENSITIVE (the runtime keys an unordered_set).
EXACT EDIT -- Interfaces, add:
```cpp
  // Name rules mirror the runtime's LoadAsset keys (InputActions.cpp:757-775): map names unique across the document, action names unique within their map; trimmed, case-sensitive. nullopt = acceptable; the unchanged name is acceptable.
  [[nodiscard]] static std::optional<std::string> ValidateName(const nlohmann::json& draft, const Guid& id, std::string_view proposed);
      // "Names cannot be blank" | "A map named 'X' already exists" | "Another action in this map is already named 'X'"
  [[nodiscard]] bool SiblingNameTaken(const Guid& id, std::string_view name) const;   // = ValidateName(draft_, id, name).has_value() && !blank
  // private: static std::string UniqueSiblingName(const nlohmann::json& siblings, std::string base);  // base, "base 2", "base 3"... until no sibling's "name" equals it
```
Rules: `SetField(id, "name", value)` trims the string and returns false (no edit, no undo entry) when `ValidateName` reports a reason. `AddMap(name)` / `AddAction(map, name)` pass their name through `UniqueSiblingName` (siblings = `actionMaps` / the map's `actions`), so "Action Map"/"Action" never collide; `DuplicateAction` and `DuplicateInArray`'s `" Copy"` branch use `UniqueSiblingName(siblings, name + " Copy")`. `Warnings()` gains one line per offending row BEFORE the unknown-path loop: `"Invalid name in <map>[/<action>]: <reason>"` (a file loaded from disk with duplicates surfaces in Problems). `SelectByPath`: when a name segment matches more than one sibling return false (the runtime's ambiguity rule). Step 1 tests, append:
```cpp
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
    REQUIRE(model.AddAction(map));  const auto a1 = model.SelectedAction();
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
    REQUIRE(model.DuplicateAction(map, jump)); CHECK(model.FindNode(model.SelectedAction())->at("name") == "Jump Copy");
    REQUIRE(model.DuplicateAction(map, jump)); CHECK(model.FindNode(model.SelectedAction())->at("name") == "Jump Copy 2");
    REQUIRE(model.AddMap()); CHECK(model.FindNode(model.SelectedMap())->at("name") == "Action Map");
    REQUIRE(model.AddMap()); CHECK(model.FindNode(model.SelectedMap())->at("name") == "Action Map 2");
    CHECK(M::ValidateName(model.Draft(), model.SelectedMap(), "Player") == "A map named 'Player' already exists");
    // A legacy file with two "Jump"s: Problems line + ambiguous SelectByPath refused.
    auto dup = DocumentJson();
    dup["actionMaps"][0]["actions"].push_back(nlohmann::json::parse(R"({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","name":"Jump","type":"Button","bindings":[]})"));
    M legacy(std::move(dup));
    CHECK(std::any_of(legacy.Warnings().begin(), legacy.Warnings().end(), [](const std::string& w) { return w.starts_with("Invalid name in Player/Jump"); }));
    CHECK_FALSE(legacy.SelectByPath("Player/Jump"));
}
```
(If `AddAction`/`AddMap`/`DuplicateAction` do not already select the new row, read the model -- InputActionsEditorModel.cpp:240-286 selects it -- and adjust the id capture accordingly.)
ripple: 08-task D37 (rename blocks validate live; Enter-with-invalid re-arms), 11-task D43 (diag code `input.name.invalid`), 09-task D41 desk.

### D29 | 06-task | high | folds: U21 (model half)
EXACT EDIT -- Step 4 `Conflicts()`: extend `struct Entry` with `std::string key;`, fill it with `InputActions::CanonicalControlKey(binding.path)` (and `part.path`) where entries are pushed; change the guard to `if (entries[i].key.empty() || entries[i].key != entries[j].key) continue;   // compare the COMPILED control, not the spelling: the rebind capture writes the scancode form while assets author the keycode form`. `BindingConflict::path` keeps the authored `entries[i].path` for the message/tooltip. Step 1 conflict test: change Crouch's path to `"<Keyboard>/scancode/space"` (keep every existing CHECK), add a fourth action `Aim` (id `13131313-1313-4131-8131-131313131313`) with bindings `"<Mouse>/button/1"` (id `14141414-...`) and `"<Keyboard>/lshift+<Keyboard>/a"` (id `15151515-...`), and a fifth `Block` (id `16161616-...`) with `"<Mouse>/leftButton"` (id `17171717-...`) and `"<Keyboard>/a+<Keyboard>/lshift"` (id `18181818-...`), all ungrouped; CHECK `conflictsOf("14141414-...") == {"Block"}`, `conflictsOf("15151515-...") == {"Block"}`, `conflictsOf("12121212-...").empty()` (`spaec` conflicts with nothing); the "Conflicting" warning count becomes 4 (Jump/Crouch, Fire/Crouch, Aim/Block mouse, Aim/Block chord). Use full 36-char guids in the fixture.
ripple: 05-task D23, 07-task (rows read `c.binding` -- unchanged).

---

## 07-task.md (InputActionsRows)

### D30 | 07-task | minor | folds: C32
EXACT EDIT -- Step 4: delete the line `header.name = InputActions::DisplayForPath(Str(b, "composite") == "1DAxis" ? "1DAxis" : "2DVector").control;` from the code block (keep `header.name = Str(b, "composite") == "1DAxis" ? "1D Axis" : "2D Vector";`) and delete the parenthetical after the block that told the implementer to delete it.
ripple: none.

### D31 | 07-task (+ 08-task) | minor | folds: C14
Rationale: spec B 2.3 shows a part as `1D Axis · negative` and one tinted pill PER scheme; the plan gave the role only and one joined pill.
EXACT EDIT -- `InputRow` gains `std::vector<std::string> groups;   // one entry per scheme group (badge is the joined form, for search/tests)`. In `bindingRow` set `r.groups = groups; r.badge = Joined(groups);`; for the composite header `header.groups = groups;`. In the parts loop, after `InputRow pr = bindingRow(InputRowKind::Part, ...)` add `pr.detail = header.name + " · " + pr.detail;` (UTF-8 middle dot, the same literal form as the `· actions` header). Step 1: `CHECK(rows[2].detail == "1D Axis · negative")`; append a small case:
```cpp
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
ripple: 08-task D36 draws one `AssetPill(g, SchemeVariant(g))` per `row.groups` entry.

### D32 | 07-task (+ 08, 09) | medium | folds: U17
Rationale: the search decides an action shows because a binding matched, then the collapse drops that binding. A non-empty search forces every surviving action open; the user's collapse set is untouched and returns when the search clears (UE `bForceParentItemsExpanded`).
EXACT EDIT -- Step 4: replace `if (collapsedActions.count(actionId.ToString())) continue;` with:
```cpp
            // A non-empty search overrides collapse: every surviving action draws
            // expanded so the matching binding is visible. collapsedActions itself
            // is untouched and returns when the search clears.
            if (search.empty() && collapsedActions.count(actionId.ToString())) continue;
```
Step 1, "collapse, conflicts, malformed drafts": after the two collapsed CHECKs add:
```cpp
    InputRowFilter f; f.search = "stick";                                  // search overrides collapse
    rows = BuildInputRows(Fixture(), kMap, f, {}, collapsed);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].name == "Move"); CHECK(rows[1].name == "Left Stick X"); CHECK(rows[2].kind == InputRowKind::AddBinding);
    rows = BuildInputRows(Fixture(), kMap, {}, {}, collapsed);             // search cleared: the collapse returns
    CHECK(rows.size() == 5);
```
ripple: 08-task D36 (chevron reads 'down' while searching; click is a no-op), 09-task D40 (Left/Right do not edit `collapsedActions` while searching).

### D33 | 07-task | BLOCKER ripple of D01
EXACT EDIT -- Step 4 ends with the premake entry from D01; Step 6 `git add` includes `premake5.lua`.

---

## 08-task.md (document presentation)

### D34 | 08-task | major | folds: C07
Rationale: the plan retires the JSON tab and points the repair banner at `Assets > Open as text`, which does not exist. Build it (spec B 2.1/6) through the editor's existing one-implementation-two-entry-points pattern (`AssetPathAction` + `AssetPanelActions`); ShellExecuteW "open" hands the file to the OS default editor. Deferral rejected: the banner would otherwise name a nonexistent command.
EXACT EDIT -- new **Step 0: Assets > Open as text** (before Step 2 deletes the JSON tab):
(a) `ArcaneEditor/src/Panels/AssetPanelCommon.hpp:55-56` -- add `openAsText` to the `Arcane::Guid ... showInExplorer, copyPath, copyGuid;` list.
(b) `AssetPanelCommon.cpp:334-335` -- beside `Show in Explorer`: `if (ImGui::MenuItem("Open as text")) actions.openAsText = e.guid;`; mirror it in the preview-pane button column at `AssetBrowserPanel.cpp:1290-1291` (that block states it mirrors the context menu exactly).
(c) `EditorAppFrame.cpp:174-193` -- extend `AssetPathAction(proj, guid, bool showInExplorer, bool copyPath, bool openAsText = false)`: `if (openAsText) ShellExecuteW(nullptr, L"open", assetPath->wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);` (the OS default handler; no SDL_OpenURL). Dispatch at :2752-2755: `if (panelActions.openAsText.IsValid()) AssetPathAction(m_runtime->CurrentProject(), panelActions.openAsText, false, false, true);`. Menu-bar route: `EditorPanels.hpp:94` `MenuRequests` gains `bool openAssetAsText = false;   // Assets -> Open as text (on the browser's tracked row)`; `EditorPanels.cpp:292-294` adds `if (ImGui::MenuItem("Open as text")) requests.openAssetAsText = true;` beside Show in Explorer; `EditorAppFrame.cpp:2589-2593` includes `menuReq.openAssetAsText` in the condition and passes it as the fifth argument.
(d) Step 6 `git add`: add `ArcaneEditor/src/Panels/AssetPanelCommon.hpp ArcaneEditor/src/Panels/AssetPanelCommon.cpp ArcaneEditor/src/Panels/AssetBrowserPanel.cpp ArcaneEditor/src/Panels/EditorPanels.hpp ArcaneEditor/src/Panels/EditorPanels.cpp ArcaneEditor/src/App/EditorAppFrame.cpp`.
(e) Step 5 desk line: "right-click Player.arcinput in the Asset Browser > Open as text opens it in the OS default editor; the Assets menu carries the same item."
(f) 99-tail coverage row: `B 2.1 / s6 JSON reachable as Assets > Open as text | 8 (Step 0)`.
ripple: 99-tail D48.

### D35 | 08-task | major | folds: C12, U12, U19, U20, U22, U29, C27, C31 -- FINAL document shape, `Draw`, `TickCapture`
Rationale: (U20/C12/U12) the key/click that completes or feeds a capture must be CONSUMED -- ImGui has no event consumption, so a frame stamp carries it and every sibling control asks `InputSwallowed()`; the Ctrl+S shortcut ran BEFORE TickCapture, so it moves after; (U19) `WantCaptureMouse` is true over every ImGui window, so the capture snapshot clears it (mouse buttons become capturable) and keeps ActiveId keyboard semantics, while the columns go `NoInputs` and the toolbar disables so the document is the sole claimant; (U22) a capture is bound to the focused, drawn document -- hidden tab / focus loss cancels, the timer never freezes; (U29) the tab dot has no producer without `ImGuiWindowFlags_UnsavedDocument`; (C27/C31) includes and members are named.

EXACT EDIT -- Step 2, the private section becomes (Task 10 adds `page_`, Task 11 adds `onSaved_`/`diagKey_`/`publishedWarnings_`/`PublishWarnings` -- listed here so the file has ONE final shape):
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
Includes in the .hpp: `<Arcane/Input/InputRebindOperation.hpp>`, `<functional>`, `<vector>`. Includes in `InputActionsDocument.cpp`: add `#include "Widgets/EditorTheme.hpp"` and `#include "Widgets/IconsLucide.h"` (the banner uses `Theme::kAmber` and `ICON_LC_TRIANGLE_ALERT`). Delete `RefreshText`, `text_` and the old `TextDisabled("Unsaved changes"/"Saved")` line (current .cpp:106): the `UnsavedDocument` flag below is the only dirty affordance (spec B 2.1).

`InputActionsDocument.cpp` FINAL for these three functions:
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
Step 3 header: `InputActionsDocumentState` FINAL:
```cpp
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
```
`Services` gains `std::function<bool()> inputSwallowed;   // true while a capture is live and on its completing frame: keys and clicks belong to the capture`. The class declares `void HandleMapKeys(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services, Edit& edit);` beside `HandleKeys` (Task 8 gives both empty bodies; Task 9 fills them). `InputActionsDocumentWidgets::Draw` becomes:
```cpp
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
```
(`DrawMaps` gains the `services` parameter for `HandleMapKeys`.) Interfaces/Produces add: "a capture is cancelled when the document loses focus or its body does not draw; the completing input is consumed for that frame." Step 5 expected picture, append: "The headless capture opens the asset clean, so the tab shows a plain X; desk (windowed): rename or rebind anything -> the tab shows the unsaved dot; Ctrl+S -> plain X; edit then Ctrl+Z back to the saved revision -> plain X." Test (append to InputActionsEditorModelTest.cpp, `[editor][input]`):
```cpp
TEST_CASE("input document: the capture snapshot ignores ImGui's mouse claim and keeps the keyboard's ActiveId claim", "[editor][input]")
{
    Arcane::InputSnapshot raw; raw.mouseButtons = 0x2; raw.wantCaptureMouse = true; raw.wantCaptureKeyboard = false;
    const auto s = Arcane::Editor::InputActionsDocument::SnapshotForCapture(raw, false);
    CHECK_FALSE(s.wantCaptureMouse); CHECK_FALSE(s.wantCaptureKeyboard); CHECK(s.mouseButtons == 0x2);
    CHECK(Arcane::Editor::InputActionsDocument::SnapshotForCapture(raw, true).wantCaptureKeyboard);
}
```
ripple: 09-task D40 (`HandleKeys` first line), D36 (`swallowed` gates in DrawRow), 10-task D42 (`page_` ctor), 11-task D43 (members already declared here), 12-task E3 (a scripted capture would assert the swallow lines).

### D36 | 08-task | major | folds: C10, C11, C14, U12, U15, U17, U18, U20 -- FINAL `DrawRow`
Rationale: (C10) a hover-gated SUBMISSION oscillates on an AllowOverlap row (the rail's documented bug): submit the Rebind hit region every frame, paint on hover; (C11) the action branch left `SameLine()` armed so the next row drew on the same line: restore the row bottom explicitly; (U15) drop legality is decided while HOVERING with a decorator, not on release; (U17) chevron reads down while searching; (U18) selection scrolls into view, new rows open in rename; (U12/U20) every control but the capture row is inert while swallowed; (C14) one pill per group.

EXACT EDIT -- Step 4, add to the anonymous namespace: `std::string Trim(std::string s)` (strip leading/trailing spaces and tabs), `std::string NameOf(const InputActionsEditorModel& model, const Guid& id)` (`const auto* n = model.FindNode(id); return n ? Str(*n, "name") : std::string{};`), and replace the whole `DrawRow` with:
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
Step 5 expected picture, append: "every row on its own 24 px line -- `1D Axis` directly under `Move`, `W` directly under `Jump`; if any row appears to the right of a pill, the trailing block left the cursor on the previous line; parts read `A  1D Axis · negative`; a binding in two schemes shows two tinted pills."
ripple: 09-task D40/D41, 07-task D31/D32.

### D37 | 08-task | major | folds: C02, U13, U14, U18, U24, C24, U17, C39
EXACT EDIT -- Step 4:
- Anonymous namespace: add `bool IsMapId(const nlohmann::json& draft, const Guid& id)` (true when `id` is an entry of `draft["actionMaps"]`) and `bool IsActionId(const nlohmann::json& draft, const Guid& id)` (walks every map's `actions`).
- `DrawToolbar` `##input_add` popup items: `Action map` -> `edit = [&model, &state] { if (model.AddMap()) { state.renameTarget = model.SelectedMap(); state.renameBuf = NameOf(model, state.renameTarget); state.renameFocusPending = state.scrollToSelection = true; } };` (the model selects the new map and gives it a unique name -- D28; the new row opens in rename, UE's new-item kick-off); `Action` -> the same shape with `model.AddAction(map)` / `model.SelectedAction()`; `Binding` and the two `Composite` items capture `&state` and set `state.scrollToSelection = true` on success. The Preview tooltip etc. unchanged.
- `DrawMaps(model, state, services, edit)`: (1) the `+` SmallButton right-edge idiom `ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());` (never `GetWindowContentRegionMax`, obsolete in 1.92 -- C24) and the same Action-map lambda as the toolbar; (2) after the `if (!draft.is_object() ...) return;` guard insert the stale-rename sweep: `if (state.renameTarget.IsValid() && !IsMapId(draft, state.renameTarget) && !IsActionId(draft, state.renameTarget)) state.renameTarget = {};   // a target that no row can draw (undo removed it) would wedge the key handlers shut -- the Outliner's sweep, EditorPanels.cpp:1644-1658`; (3) the rename block mirrors D36's action rename block exactly (`ValidateName(draft, id, buf)` live tooltip, cancelled/valid/entered/else, `Trim`, compare to `name`), with the scroll consume before `InputTextString`; (4) after `if (row.clicked) model.SelectMap(id);` add `if (model.SelectedMap() == id && state.scrollToSelection) { ImGui::SetScrollHereY(); state.scrollToSelection = false; }`; the Duplicate item becomes `edit = [&model, &state, id] { if (model.DuplicateRow(id)) state.scrollToSelection = true; };`; (5) after the loop: `// Empty space in the maps column deselects: the ASSET is the container (spec A s3.1). Not while an inline rename is live -- that click commits the rename.` `if (!state.renameTarget.IsValid() && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) model.SelectMap({});` then `HandleMapKeys(model, state, services, edit);` (still inside `##input_maps`).
- `DrawActions`: the `+ Action` SmallButton uses the right-edge idiom `ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(ICON_LC_PLUS " Action").x - ImGui::GetStyle().FramePadding.x * 2.0f);` and the toolbar's Action lambda. After `const std::vector<InputRow> rows = BuildInputRows(...)` insert:
```cpp
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
```
  and after the row loop, before `HandleKeys`: `if (!state.renameTarget.IsValid() && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) model.SelectMap(map);   // empty space under the rows: the MAP is the container (spec A s3.1); SelectMap(same) clears action/binding/part`.
- Step 5 expected picture, append: "click empty space under the action rows: the Inspector (once Task 10 lands) drops to `Player.arcinput > Player`; click empty space in the maps column: `Select an action map.` and the asset page."
ripple: 09-task D40 (`HandleMapKeys` body), D41 desk, 10-task D42 desk.

---

## 09-task.md (keys)

### D40 | 09-task | major | folds: C23, C28, U12, U16, U17, U18, U20, C39 -- FINAL `HandleKeys` + `HandleMapKeys`
Rationale: (U20/U12) the completing/feeding key belongs to the capture; (C28) the guard is `WantTextInput` like the Outliner's, not `IsAnyItemActive`; (U16) tree convention for Left/Right; (U17) no collapse edits while searching; (U18) Delete/F2/Enter/Left/Right never auto-repeat, selection scrolls into view, the maps column gets its own handler (C39's "Del"/"F2" hints become true); (C23/C28) the scene's Delete/F2 are the Outliner's own and already stand down -- no app change, no EditorAppFrame.cpp in the commit.

EXACT EDIT -- Files: `Modify: ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp / .cpp (HandleKeys, HandleMapKeys -- each runs inside its own child, so ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) routes the keys to whichever column has focus)`. Nothing in InputActionsDocument.cpp or EditorAppFrame.cpp. Step 1:
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
Replace the paragraph after the code block with: "The scene's Delete/F2 are the Outliner's own, not the app's: EditorPanels.cpp:1662-1678 gates them on `ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)` for the Outliner window (plus editMode, not renaming, `!WantTextInput`), so they already stand down while a document window is focused. There is no app-level Delete/F2 path: `InputEdges` (EditorApp.hpp:1075-1084) carries no Delete/F2 edge and the `sc*` flags (EditorApp.hpp:264) are New/Open/Save only. Verify at the desk (select an entity in the Outliner, focus the document, press Delete and F2: no entity is removed or renamed; the document row is) and record the line numbers; do not touch EditorAppFrame.cpp. Ctrl+S from an Inspector page is already covered by Task 4's `saveRequested`/`focusedSource` route; do not add a second gate here." Step 3: `git add ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp`, subject `feat(editor): input document -- keyboard navigation in both columns (Up/Down/Left/Right tree convention, Enter/Delete/F2 without auto-repeat, selection scrolls into view, new rows open in rename); the completing key/click is consumed by the capture (input editor plan T9)`.
ripple: 08-task D35/D36/D37.

### D41 | 09-task | minor | folds: C15, U12, U13, U14, U15, U16, U18, U19, U20, U22, U23, U33 (desk)
EXACT EDIT -- Step 2, replace the desk list with (record each in the report; an item with no controller on the desk is recorded OWED, never skipped silently):
> Open Player.arcinput. Rebind: click `Space` under Jump, Enter, press `J`: the row reads `J`, Ctrl+Z restores `Space`; Enter then Escape: the row returns; Enter then wait 10 s: the row returns. Consumption: Enter on Space then Delete: the row reads `Delete` and still exists; Enter then Enter: the row reads `Return` and no second capture starts (no amber text); Enter then F2: the row reads `F2`, no rename box; Enter then Down: the row reads `Down Arrow`, the selection does not move; during a capture right-click another row: no menu; hover another row: no Rebind button. Chords: Enter, hold Shift, press J: `Left Shift + J`; Enter, tap Shift alone: `Left Shift`. Mouse: Enter on Space, click the RIGHT button over the maps column: the row reads `Right Button`, the map selection does NOT change, no context menu opens, Ctrl+Z restores. Focus: Enter, click the Viewport, press W: Space unchanged, the row no longer amber; Enter, switch to another docked tab, wait 2 s, switch back: no frozen countdown. Conflicts: add a second `<Keyboard>/space` binding to Move: both rows show the amber dot naming the other action; rebind Crouch (add one) to Space via the CAPTURE (scancode form): the dot still appears against Jump's authored `<Keyboard>/space`. Drag: drag `South Button` above `W`: only the Move bindings show the outline while hovered, the preview reads "Move 'South Button' here", release reorders, Ctrl+Z reverts; drag `Space` (Jump) over `Move` or `W`: no outline, "Cannot move 'Space' here", release does nothing; drag a part over a plain binding: no outline. Keys: select `A  1D Axis · negative` under Move: Left selects the `1D Axis` header (Move stays expanded); Left again selects Move (still expanded); Left again collapses Move; Left again no-op; Right re-expands; Right again selects `1D Axis`; Right on `1D Axis` selects the part; select Jump: Right selects `W`; Left on `W` selects Jump without collapsing it; Alt+Left/Right change nothing; with 30+ actions hold Down: the selection never leaves the visible area; hold Delete for one second on an action: exactly one row goes, one Ctrl+Z restores it; hold F2: one rename box, text intact. Rename: F2 on Jump, type `Move` (the sibling's name): the tooltip names the collision, Enter keeps the box open, Escape reverts; type `Jump2`, Enter: renamed; blank + click away: reverts; start a rename, Ctrl+Z (global): the box vanishes and Up/Down/Delete/F2 still work; start a rename, then type a search that excludes the action: same. New rows: `+ Action` (toolbar or column header) and the maps `+`: the new row is scrolled into view with a unique name (`Action`, `Action 2`) selected in a rename box; Enter commits, Escape keeps the default. Maps column: focus it, Up/Down move the map selection, F2 renames it, Delete removes it (Ctrl+Z restores); focus the actions column: the same keys act on the row; in the toolbar search box: none fire. Undo: select Jump's new `W` row, Ctrl+Z: the row disappears and the selection falls back to Jump (no dead selection); Ctrl+Y: `W` is back and selected. Preview: toggle Preview and hold Space: the Space row glows. Gamepad (the editor feeds the document the raw `InDevices().Sample` snapshot, EditorAppFrame.cpp:730-734, so a pad reaches the capture outside Play): select `South Button`, Enter, press East: `East Button` with the violet Gamepad pill unchanged, Ctrl+Z restores; select `Left Stick X`, Enter, push the left stick horizontally past half: still `Left Stick X`; vertically: `Left Stick Y`, Ctrl+Z restores; Preview + hold South: the row glows.
ripple: 10-task D42 (the Inspector-side gamepad line lives there).

---

## 10-task.md (the document's Inspector pages)

### D42 | 10-task | major | folds: C19, C26, U06, U08, U09, U25, U36, U38, C38, C02, C05, C13, U27, C15
EXACT EDIT:
- Files > Modify: add `ArcaneEditor/src/Documents/InputActionsEditorModel.hpp / .cpp` are NOT modified here (FindNode/Resolves landed in Task 6, D27); drop them from Step 7's `git add`. Add `premake5.lua` (D01).
- Step 3 header: add members `bool drawing_ = false; std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);` and the helper `void Defer(std::function<void()> fn) { if (drawing_) edit_ = std::move(fn); else fn(); }` (`#include <memory>`). An orphan TextRow commit flushed by the host OUTSIDE Draw (D06) applies immediately, so a following Ctrl+S saves it; inside Draw it is deferred past the `Draft()` references as before.
- Step 4 `Draw`: `edit_ = nullptr; drawing_ = true;` at the top; `drawing_ = false; if (edit_) edit_();` at the end.
- Step 4 TextRow call sites (DrawMap Name, DrawAction Name + Processors, DrawBinding Path): capture BY VALUE with the liveness token -- never `[&]`, the local `id` would dangle once the draft stores the callable (D06). Shape: `grid.TextRow("Name", Str(map, "name"), [this, id, w = std::weak_ptr<bool>(alive_)](std::string v) { if (w.expired()) return; Defer([m = &model_, id, v] { (void)m->SetField(id, "name", v); }); });`. Target validation is the model's `SetField` -> `FindId` returning false for a deleted id; `ApplyEdit`'s `before == after` guard makes a duplicate delivery a no-op.
- Step 4 id hygiene: in `DrawMap`, `DrawAction` and `DrawBinding` wrap each `PropertyGrid::Rows rows(grid, "##..."); if (rows) { ... }` block (INSIDE `if (grid.Section(...))`, so section open-state stays shared per page kind) in `ImGui::PushID(id.ToString().c_str()); ... ImGui::PopID();`. In `DrawBinding`'s "Control schemes" loop and `DrawAsset`'s scheme rows push `IdOf(s).ToString().c_str()` around each row (two schemes may share a name; ImGui's id-conflict detection would otherwise paint them red and keyboard activation toggle both).
- Step 4 `Breadcrumb`: every crumb gets a `key` in the model's 4-segment format (three slashes always -- `PageFor` rejects fewer): asset root `""` (PageFor("") is the asset page), map `sel_.map.ToString() + "///"`, action `map + "/" + action + "//"`, binding `map + "/" + action + "/" + binding + "/"`, part all four. Keep the `select` lambdas as written.
- Step 4 `DrawPicker` -- replace the body with this behaviour (UE SKeySelector mechanism, written fresh): (1) after `BeginPopup` succeeds: `if (ImGui::IsWindowAppearing()) { pickerSearch_[0] = '\0'; ImGui::SetKeyboardFocusHere(); }` before the InputText (cleared + focused on EVERY opening; typing never reaches the row's HandleKeys); (2) the InputText carries `ImGuiInputTextFlags_EnterReturnsTrue`; `const bool enter = ...`; (3) tokenise the lower-cased search on whitespace; a choice is visible when EVERY token is found in `lower(device + " " + control + " " + path)`; no tokens = all visible; (4) iterate `kKnown` in its emitted order grouped by `display.device`, emitting `ImGui::TextDisabled("%s", device)` lazily when the device changes from the previous VISIBLE row (no empty headers); keep the per-row icon + `##path` id + `SameLine(200)` path as written; (5) `const InputControlChoice* firstVisible` set on the first visible row; the commit path shared by click and Enter: `edit_ = [m = &model_, target, path = choice.path] { (void)m->SetField(target, "path", path); }; ImGui::CloseCurrentPopup();`; (6) `if (enter && firstVisible) commit(*firstVisible);` runs AFTER `EndChild()` and before `EndPopup()` (Enter with nothing visible does nothing and leaves the popup open).
- Step 5 `PageFor`: delete `const auto& draft = model_.Draft();` and use `auto exists = [&](const Guid& id) { return !id.IsValid() || model_.FindNode(id) != nullptr; };`; replace the sentence after the block with "`FindNode` is the model's const lookup added in Task 6." Add to the overrides list: `bool Resolves(std::string_view key) const override { return model_.Resolves(key); }`. `page_` is constructed in the ctor init list AFTER `model_`, `state_`, `preview_` (D35's member order).
- Step 1 test, after `const std::string key = doc->SelectionKey();` add:
```cpp
    const std::string mapId = "22222222-2222-4222-8222-222222222222", actionId = "33333333-3333-4333-8333-333333333333";
    REQUIRE(crumbs[0].key); CHECK(crumbs[0].key->empty());                       // the asset root re-pins to the asset page
    REQUIRE(crumbs[1].key); CHECK(*crumbs[1].key == mapId + "///");
    REQUIRE(crumbs[2].key); CHECK(*crumbs[2].key == mapId + "/" + actionId + "//");
    REQUIRE(crumbs[3].key); CHECK(*crumbs[3].key == key);
    CHECK(doc->PageFor(*crumbs[1].key) != nullptr);
    CHECK(doc->Resolves(key)); CHECK_FALSE(doc->Resolves("bogus"));
```
- Step 6: replace the capture sentence with: "Headless capture as in Task 8 Step 5 (no `--select-in-document` yet -- that is Task 12): Open selects the first map + first action, so the Inspector shows `Player.arcinput > Player > Move` with the Action page for an Axis1D action (Name, Type = Axis1D, Interaction = --, Processors; Bindings: KeyboardMouse 1, Gamepad 1, Ungrouped 0; Live preview: 'turn on Preview ...'). `Player > Jump` is what Task 12's `--select-in-document Player/Jump` produces." Desk list becomes: "(1) pin the Inspector on Jump, click Move: the pinned page stays; Window > New Inspector follows; (2) pin on a binding, click the action crumb in the PINNED window: THIS window shows the action page, the follower and the history do not move; (3) click `Pick...`, type `stick x`, Enter: the row reads the left stick X control and the popup closes; click `Pick...` again: the field is empty and focused; Ctrl+Z reverts; (4, container pages) click empty space under the action rows: the breadcrumb drops to `Player.arcinput > Player`; click empty space in the maps column: `Player.arcinput` alone (the asset page); the Inspector never reads `No selection` while the document is the source; (5, mockup step 3 on the document) with `Player.arcinput > Player > Jump` showing, click the Console tab, the Problems tab, the Asset Browser tab, the document's own empty background below the last action, the Outliner's empty space, then activate the Viewport tab and click its background: the breadcrumb still reads `Player.arcinput > Player > Jump` -- if any of them moves the Inspector, a PRODUCER (`SelectionContext::Epoch` or the model's `SelectionEpoch`) bumped on a non-gesture: fix the producer, not the edge; (6, re-select) select Jump, select MeshCube in the Outliner, click Jump again: the Inspector returns to `Player.arcinput > Player > Jump` with no new history entry; the reverse (document page showing, click the already-selected MeshCube): back to `Scene > MeshCube`; (7, Ctrl+S) click into the Inspector on Jump's page with no text field active, Ctrl+S: the tab dot clears and the scene is NOT saved; click into the Name row, type, Ctrl+S while the field is active: the document saves; pin a second instance on Jump, focus the first on the scene page, Ctrl+S: the scene saves, the document does not; (8, hidden-row commit) type into the Name row, then click the Inspector 2 tab so the first instance hides: the typed name is committed (the tab dot appears), Ctrl+Z reverts it; (9, gamepad) hold South with Jump selected: Live preview's Device reads `Gamepad`, Value follows; release and press Space: `Keyboard / Mouse`."
ripple: 02-task D13, 01-task D06, 06-task D27, 08-task D35.

---

## 11-task.md (save republish + Problems)

### D43 | 11-task | major | folds: U30, C31, U14/U24 (diag code), C08/U31/U32 (relocated to Task 5)
Rationale: `DocumentHost::ConfirmSaveAndClose` runs `Save()` then `Close()` inside `DrawAll`, so a post-DrawAll poll never sees a save-and-close; push the request from `Save()` and apply at the frame boundary (UE: the mutation requests, the tick applies). The re-read + re-parse from disk is redundant: after a successful model Save, `LastValidPreview()` IS the saved asset. `LocalInputUser` preservation is delivered in Task 5 (D26), not deferred.
EXACT EDIT:
- Files: `Modify: ArcaneEditor/src/Documents/InputActionsDocument.hpp / .cpp, ArcaneEditor/src/App/EditorApp.hpp, ArcaneEditor/src/App/EditorApp.cpp (:826-830 factory), ArcaneEditor/src/App/EditorAppFrame.cpp:2406-2442`. No engine files.
- Interfaces > Produces: replace `ConsumeSaved` with `void InputActionsDocument::SetOnSaved(std::function<void(const Guid&, const InputActionAsset&)>)` (invoked exactly once per SUCCESSFUL save, from inside Save(), with the asset the model just wrote) and `void EditorApp::RepublishGameInput(const Guid& asset, const InputActionAsset& parsed)` (no disk read). The settings.select path keeps its inline read-parse (it has no model) -- or factors it into `LoadInputAssetFromDisk(guid) -> std::optional<InputActionAsset>` that only it calls.
- Step 1 test becomes "input document: Save invokes onSaved exactly once per successful save; a refused save does not": `int calls = 0; Arcane::Guid seen; doc->SetOnSaved([&](const Arcane::Guid& g, const Arcane::InputActionAsset&) { ++calls; seen = g; }); REQUIRE(doc->Save()); CHECK(calls == 1); CHECK(seen == doc->AssetGuid());` then the `version = 99` ApplyEdit, `CHECK_FALSE(doc->Save()); CHECK(calls == 1);`.
- Step 2 Document: `bool Save() override { if (!model_.Save(path_)) return false; if (onSaved_ && model_.LastValidPreview()) onSaved_(guid_, *model_.LastValidPreview()); return true; }` (members `onSaved_`, `diagKey_`, `publishedWarnings_` and `PublishWarnings()` are already declared by D35's final shape; `diagKey_ = "input:" + guid_.ToString()` in the ctor; `~InputActionsDocument() override { Arcane::Diagnostics::Clear(diagKey_); }`; `#include <Arcane/Base/Diagnostics.hpp>`). `PublishWarnings` code mapping: `d.code = w.rfind("Unknown control path", 0) == 0 ? "input.path.unknown" : w.rfind("Invalid name", 0) == 0 ? "input.name.invalid" : "input.binding.conflict";`.
- Step 2 App: member `std::optional<std::pair<Arcane::Guid, Arcane::InputActionAsset>> m_pendingInputRepublish;` (`#include <Arcane/Input/InputActionAsset.hpp>`). In the `.arcinput` factory lambda (EditorApp.cpp:826-830) -- the one place that constructs the concrete type, so no dynamic_cast and Task 4's `EditorDocument&`-typed observer is untouched: `auto doc = Arcane::Editor::InputActionsDocument::Open(path, m_undo ? &*m_undo : nullptr); if (doc) doc->SetOnSaved([this](const Arcane::Guid& g, const Arcane::InputActionAsset& a) { m_pendingInputRepublish.emplace(g, a); }); return doc;`. Replace the post-DrawAll ForEach block with:
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
  `RepublishGameInput(guid, parsed)` keeps the designated-asset check and projectId resolution, drops ResolveAsset/ifstream/parse/FromJson, and calls `m_runtime->ConfigureGameInput(parsed, *projectId)` directly (the same-project re-Configure of D26 preserves the map stack, scheme and dirty overrides and primes the evaluator).
- Delete the paragraph "Read `LocalInputUser::Configure` ... do not widen this task" entirely (delivered in Task 5).
- Step 3 desk: "Play (in viewport), open Player.arcinput, rebind Jump's Space to J, Ctrl+S: J jumps in the running session; pause the game so its pause map is pushed (or push any second map from the game), edit + Ctrl+S: the pushed map is still active and the chosen control scheme unchanged; close the dirty tab and choose Save in the confirm: J still jumps (the save-and-close path republishes). Add a duplicate Space binding: the Problems panel lists `Conflicting '<Keyboard>/space': ...` under the asset; rename an action to a sibling's name via a hand-edited file: `Invalid name in Player/Jump: ...` appears; remove it: gone; close the document: rows gone."
- Step 4 `git add`: `ArcaneEditor/src/Documents/InputActionsDocument.hpp ArcaneEditor/src/Documents/InputActionsDocument.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneEditor/src/App/EditorApp.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/InputActionsEditorModelTest.cpp`; subject `feat(editor): a saved input document pushes a republish of the project's gameplay input, applied at the frame boundary (save-and-close included); its Warnings reach the Problems panel as asset diagnostics (input editor plan T11)`.
ripple: 05-task D26, 99-tail D48 (owed item removed).

---

## 12-task.md (--select-in-document, schema 11, E3, golden lanes)

### D44 | 12-task | BLOCKER | folds: C18, C35
Rationale: `STATIC_REQUIRE(kSchemaVersion == 10)` at VerifyReportTest.cpp:967 is a compile-time assertion the plan's nine-line list omits: the whole test exe stops building; `:979 CHECK_FALSE(IsSupportedSchemaVersion(11))` then fails at runtime.
EXACT EDIT -- Step 1, replace the VerifyReportTest.cpp sentence with: "change every schema-10 assertion to 11: (a) :962 retitle the case `schemaVersion is 11 and declares a supported range`; (b) :967 `STATIC_REQUIRE(Arcane::VerifyReport::kSchemaVersion == 11);` (a static_assert -- the test exe does not compile until this changes); (c) after the `IsSupportedSchemaVersion(10)` line (:976) add `CHECK(Arcane::VerifyReport::IsSupportedSchemaVersion(11));`; (d) :978 becomes `CHECK_FALSE(Arcane::VerifyReport::IsSupportedSchemaVersion(12));` (`v <= kSchemaVersion`, VerifyReport.hpp:225); (e) the nine `== 10` JSON assertions (:75, :736, :760, :857, :984, :1020, :1041, :1069, :1111) -> 11. Verify: `grep -n "kSchemaVersion == 10\|schemaVersion is 10\|SchemaVersion(12))" ArcaneTests/src/VerifyReportTest.cpp` returns only the new `(12)` line; lines 143/332 are unrelated `== 10`s." Step 2: "build: `selectInDocument`/`SetInspector` undeclared -- and until Step 4 bumps the constant, the edited STATIC_REQUIRE at :967 is itself a compile error; that is the expected red for this step."
ripple: none.

### D45 | 12-task | minor | folds: C22, C29
EXACT EDIT -- Step 3 last sentence becomes: "`ArcaneEditor/src/main.cpp` help comment: there is NO `--open-asset` entry yet (the flag was added in HostConfig.cpp:115 without documenting it here). Add two entries after `--tool`'s (:197-200, before `--view-mode` at :201), in the block's existing style: `//   --open-asset <guid> -- HONOURED: open the document for that asset Guid at the end of boot (EditorApp::StageFinalize; a bad Guid / no opener is a loud ARC_ERROR). REFUSED by ArcaneRuntime (that exe's main.cpp:171-176).` and `//   --select-in-document <path> -- HONOURED, only WITH --open-asset: select Map[/Action[/binding index]] inside the opened document and route it to the Inspector; an unresolvable path is a loud ARC_ERROR with the run completing. Refused at parse without --open-asset; REFUSED outright by ArcaneRuntime.`" Files line: `ArcaneEditor/src/main.cpp:197-201 (help comment, insertion after --tool)`.
ripple: none.

### D46 | 12-task | BLOCKER | folds: C36 (ordering)
Rationale: the bless must run against a host that loads the ABI-45 module; the gate is what rebuilds + restages ReferenceProject.
EXACT EDIT -- Step 6 reordered: "(1) Build. `./ArcaneTests.exe "[host]"` green. (2) Run `scripts/golden-gate.ps1 -Configuration Debug` FIRST -- it rebuilds and restages ReferenceProject: expect the 8 old lanes green and the 2 new `editor-input-doc` lanes red/NotRun (no reference PNG yet). (3) Bless the new slot from the staged editor dir (`rm imgui.ini`, the `--bless` command as written); read the PNG (Inspector `Player.arcinput > Player > Jump`, the Action page; the document's two columns; no 'Open Project Failed' modal) and `r.json` (`inspector.source == "Player.arcinput"`); copy `ReferenceProject/Verify/References/editor-input-doc.png` to the source tree IMMEDIATELY. (4) `./ArcaneTests.exe "[witness][gpu]"` (66 -> 67 cases). (5) The gate again: 10/10 green. (6) `-SelfTest` as written."
ripple: 05-task D22.

### D47 | 12-task | low | folds: U41 (report)
EXACT EDIT -- Step 4 report block: replace the inline join with `report.SetInspector(src.SourceName(), Arcane::Editor::InspectorCrumbText(src, src.Page()));` (the same string the history labels and the header tooltips carry), keeping the comment.
ripple: 02-task D09.

---

## 99-tail.md

### D48 | 99-tail | minor | folds: C03, C05, C14, U31, U34, U32, C07, C02, D08 traceability
EXACT EDIT:
- Coverage map: A 3.1 -> `2 (host + edge), 2 Step 4b (SelectionContext epoch), 3, 4, 8 (click-empty-space gesture), 10`; A 3.3 -> `2, 4 (pin only for a resolvable page; RepinKey; New Inspector reopens a hidden primary before minting)`; A 4 -> `12; 4 (instance ID LIST in the layout ini; pins deliberately not -- Task 4 Step 5; editor-ui goldens DO change for the header row -- see Deviations)`; A 5 -> `2, 12; desk: 4 Step 7 (mockup steps 1/3/4/5 on the scene), 10 Step 6 (steps 2/3/4/5 on the document); step 6 n/a (model B rejected)`; A 6.3 -> `2 (PruneStale/InvalidateSource), 4 (ClearSceneReferences hook)`; B 2.3 -> add `5 (chords, canonical keys)`; B 2.6 -> `11 (push-from-Save, applied at the frame boundary), 5 (LocalInputUser same-project re-Configure preserves the map stack, scheme and dirty overrides -- now TRUE)`; new row `B 2.1 / s6 JSON reachable as Assets > Open as text | 8 (Step 0)`; B 4 -> add `desk with keyboard + gamepad: 9 Step 2, 10 Step 6`.
- Deviations recorded, replace with: "spec A s4 says the editor-ui goldens do not change by the spec alone; the page header the same spec mandates (s3.2 breadcrumb, s3.3 pin + back/forward) is new pixels inside every Inspector window, so `editor-ui` and `editor-ui-perspective` are re-blessed in Task 4 Step 7 -- the scene BODY is unchanged, which Task 1 Step 7 proves with the 8/8 gate before the header lands. Pins are not persisted (Task 4 Step 5 reason). No monospace face for the Path row (owed: a mono face in EditorFonts). The live glow tints the whole row plus a left bar (desk call in Task 8); per-binding glow uses `BindingValue` (raw, pre-processor). Undo/redo restores the document's selection SILENTLY (no Inspector event): a mechanical change is not a gesture, so Ctrl+Z never steals the Inspector from the scene; the following instance re-resolves the page every frame."
- Owed after this plan, replace with: "Asset Browser as a source + preview-pane trim (mini-arc 2); Material tab migration (mini-arc 3); in-editor text editor (mini-arc 4; `Assets > Open as text` hands the file to the OS editor until then); monospace editor font; Aphelyon restamp to ABI 45; undo entries of a CLOSED document are silent no-ops on the shared CommandStack (DraftEditCommand's dead anchor; MeshDocument has the same shape) -- an owner-tagged `CommandStack::Purge(owner)` with its own ABI bump, not this plan; the re-Configure prime (Task 5) leaves `lastPressFrame`/`bufConsumed` residue so `Buffered(action, 6)` can report a held control for six live frames, and `heldTime` restarts for an in-progress Hold -- both need a small `InputActionsImpl` seam."

---

## Dropped (not applied) -- one line each

- U34 (low, CommandStack owner purge on document close): DROPPED to the owed list -- an engine API change on `CommandStack::Push` (its own ABI-relevant surface) for a pre-existing class shared with MeshDocument; the blunt fallback (`Clear()` the whole stack) would discard scene undo.
- U05 second half (auto-follow when a pinned key dies on a live source): DROPPED per its own verdict -- registry restore resurrects exact entity ids (delete->undo, Play->Stop); the "Pinned selection is gone" note stays, the page returns by itself (D12).
- U07's switch of IntRow to `DragInt`: DROPPED -- `InputInt` matches the shipping Priority-row pattern; both rows share the one draft/commit path anyway (D05).
- U02's "delete SelectionEdge": DROPPED -- kept as the `(epoch, key)` struct (U35's form) so the edge test and the app site stay (D08).
- U33's "every path bumps selectionEpoch_" on undo restore: REPLACED by a silent restore -- a bump would make Ctrl+Z steal the Inspector while the scene is current (U35's own rule); the follower re-resolves `Page()` live (D27).
- U40's greying-out of Window > New Inspector at the pool cap: DROPPED -- the menu has no host access; `AddInstance() == -1` is a silent no-op (D11/D17).
- U19's desk line "click into the search box during a capture and type w": DROPPED -- moot, the toolbar is disabled while a capture is armed (D35).
- U26's optional LOVE-token emission in `InputRebindOperation::Observe`: DROPPED -- `CanonicalControlKey` makes spelling irrelevant for conflicts and SDL's own lookup fixes display (D23/D24).
- U41's `InspectorSource::SelectionLabel()` virtual: REPLACED by the free `InspectorCrumbText` helper per its own correctedChange (D10).
- U08's "flush the grids of instances closed this frame": DROPPED -- the X click deactivates the box on the frame the row still draws (PressedOnClickRelease), so the ordinary commit fires.
- U18's `state.renameBuf = "Action Map"` literal for a new map: REPLACED by `NameOf(model, SelectedMap())` -- with unique naming (D28) the new name may be "Action Map 2" (D37).
- C07's "defer + reword the banner" alternative: NOT chosen -- Open as text is built (D34).
- C13's "accept the scene-side re-click gap as a limitation": SUPERSEDED by the epoch (D08).
- U04's `ARCANE_LOG_INFO` on a full pool: DROPPED -- silent no-op (D17).
- C36's "rebuild ReferenceProject BEFORE the root build" ordering nuance: kept as written in D22 (bump header first, then the sample, then the root build whose postbuild restages); D46 orders Task 12's gate/bless.
- All three UE refuted items and the one codebase refuted item: no action.

---

## Ripple index (name -> every part that mentions it)

- `premake5.lua` ArcaneTests `files` entries -> 00 (D01), 01, 02, 07, 10.
- `kGamePluginABIVersion` 45 / `ReferenceProject.arcproj` / ReferenceProject rebuild -> 00 (D02), 05 (D22), 12 (D46), 99.
- `SelectionContext::Epoch()` -> 02 (D08, SelectionOpsTest), 04 (D17 sync, ClearSceneReferences, project switch), 99.
- `SelectionEdge::Observe(epoch, key)` / `lastEpoch` -> 02 (D08/D09), 04 (D17).
- `InspectorSource::Resolves` -> 02 (D09, EditorDocument default), 03 (D15), 06 (D27), 10 (D42).
- `InspectorCrumb::key` (+ `<optional>`) -> 02 (D09/D13), 03 (D15), 04 (D16), 10 (D42).
- `InspectorCrumbText` -> 02 (D09/D10), 04 (D16 tooltips), 12 (D47).
- `InspectorHost::{InvalidateSource, PruneStale, EraseHistoryIf}` -> 02 (D09/D10/D14), 04 (D16 first line; D17 ClearSceneReferences).
- `InspectorHost::{JumpTo, BackEntry, ForwardEntry, RefreshCursorLabel, HistoryEntry::label}` -> 02 (D10/D14), 04 (D16).
- `InspectorHost::{kMaxInstances, AddInstance == -1, SetInstanceIds}` -> 02 (D11/D14), 04 (D17 ini `Ids=`, menu, desk D18).
- `InspectorHost::{CanPin, SetPinned refusal}` -> 02 (D12/D14), 04 (D16 canPin before page, pin tooltip).
- `InspectorHost::RepinKey` -> 02 (D13/D14), 04 (D16), 10 (D42 crumb keys).
- history re-snapshot after restore -> 02 (D10/D14).
- `SceneSelectionKey::{Encode, Decode, AliveSubset, KeySet}` / `SceneInspectorSource` whole-set key -> 03 (D15), 04 (D18 desk), 00 (D03).
- `PropertyGridState::{TextDraft{text,seed,active,lastFrame,commit}, NumericDraft, numericDrafts}` -> 01 (D05/D06), 04 (D16 CommitOrphans), 10 (D42 lambdas/PushID).
- `PropertyGrid::CommitOrphans` -> 01 (D06), 04 (D16).
- `PropertyGrid::TextRow(std::function)` + `AutoSelectAll` -> 01 (D06), 02 (interfaces), 10 (D42).
- `NumericRow` / Escape cancel / `imgui_internal.h` in PropertyGrid.cpp -> 01 (D05).
- `InspectorWindowsResult::{saveRequested, focusedSource}` -> 04 (D16/D17), 10 (D42 desk 7).
- `EditorApp::{m_inspectorFocusedSource, InspectorSaveTarget}` + the :2641 Ctrl+S gate -> 04 (D17), 09 (D40 note).
- `ConsumeMenuRequests` reopen-before-create -> 04 (D17/D18).
- `ClearSceneReferences` hook (`InvalidateSource` + edge prime) -> 04 (D17/D18).
- `InputActions::{CanonicalControlKey, SplitChordParts}` -> 05 (D23), 06 (D29).
- `ReadableControl` SDL lookup / `isScancode` -> 05 (D24).
- `TryCompileSinglePath` `ControlId{...}` returns -> 05 (D21).
- `InputRebindOperation::heldModifiers_` / modifier chords -> 05 (D25), 08 (D35 note), 09 (D41 desk).
- `LocalInputUser::{mapStack_, scheme_, ActiveMap, same-project re-Configure, prime}` -> 05 (D26), 11 (D43), 99.
- `InputActionsEditorModel::{SelectionEpoch bump rule, ParseKey, Resolves, FindNode, SetSelectionSilently, RestoreSelectionOrAncestor, DraftEditCommand undoKey_/redoKey_}` -> 06 (D27), 10 (D42), 08 (D37 NameOf).
- `InputActionsEditorModel::{ValidateName, SiblingNameTaken, UniqueSiblingName, "Invalid name" warnings, SelectByPath ambiguity}` -> 06 (D28), 08 (D36/D37), 09 (D41), 11 (D43 `input.name.invalid`).
- `Conflicts()` compares `key` -> 06 (D29), 05 (D23).
- `InputRow::groups` + part suffix `"1D Axis · negative"` -> 07 (D31), 08 (D36).
- search overrides collapse -> 07 (D32), 08 (D36), 09 (D40).
- `AssetPanelActions::openAsText` / `MenuRequests::openAssetAsText` / `AssetPathAction(..., openAsText)` -> 08 (D34), 99.
- `InputActionsDocument::{TickCapture(bool), SnapshotForCapture, InputSwallowed, captureSwallowFrame_, focused_ on both branches, UnsavedDocument flag}` -> 08 (D35), 09 (D40/D41), 00 (D03 review focus).
- `Services::inputSwallowed` -> 08 (D35/D36/D37), 09 (D40).
- `InputActionsDocumentState::{scrollToSelection, DragVerdict, dragVerdict, dragVerdictPrev}` -> 08 (D35/D36/D37), 09 (D40).
- `HandleMapKeys` -> 08 (D35/D37), 09 (D40/D41).
- rename sweep (`IsMapId`, `IsActionId`) + live `ValidateName` + Enter re-arm -> 08 (D36/D37), 09 (D40/D41).
- click-empty-space container pages -> 08 (D37), 10 (D42 desk), 99.
- Rebind hit region always submitted / `rowBottom` restore / per-group pills -> 08 (D36).
- drag-drop verdict on hover -> 08 (D36/D37), 09 (D41).
- `InputActionsInspectorPage::{Defer, drawing_, alive_}` + by-value TextRow lambdas + `PushID(target)` -> 10 (D42), 01 (D06).
- picker rewrite (tokens, Enter, focus, device headers) -> 10 (D42).
- `InputActionsDocument::SetOnSaved` / `EditorApp::{m_pendingInputRepublish, RepublishGameInput(guid, asset)}` / factory lambda -> 11 (D43).
- `PublishWarnings` diag codes -> 11 (D43), 06 (D28).
- VerifyReportTest :962/:967/:976/:978 -> 12 (D44).
- `ArcaneEditor/src/main.cpp` help entries after `--tool` -> 12 (D45).
- Task 12 Step 6 ordering (gate, bless, witness, gate) -> 12 (D46), 05 (D22).
- Task 10 capture expectation `Player > Move` -> 10 (D42).
- layout ini path `%LOCALAPPDATA%\Arcane\editor\layouts\<guid>.ini` -> 00 (D02), 04 (D17/D18).
