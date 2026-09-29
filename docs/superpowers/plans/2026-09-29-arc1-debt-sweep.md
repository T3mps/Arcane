# Arc-1 Debt Sweep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pay down the user-visible defects, the refactor and the missing tests left by editor mini-arc 1 (Inspector ownership + Input Actions editor, merged at 4e9796ba), so mini-arc 2 does not copy them.

**Architecture:** Twelve small, independent fixes over the Input Actions document, its Inspector page, `PropertyGrid`, the app's shortcut gate and the engine's control-path helpers, each with the test that pins it. Where a behaviour is only reachable through ImGui, the test drives real ImGui frames device-less (the `PropertyGridTest` harness shape); everything else is a headless Catch2 case. No ABI change, no report-schema change.

**Tech Stack:** C++23, MSVC (VS 18), Dear ImGui 1.92.9 WIP (vendored), Catch2, SDL3 3.4.0 (vcpkg), premake5.

**Spec:** the debt list in the arc's final review, recorded in memory `project_go_after_clear_editor_arc` and in the direction record `project_direction_and_sequencing_2026_09_29` (step 2). The arc's own specs travel with it: `docs/superpowers/specs/2026-09-28-inspector-ownership-design.md` (spec A) and `docs/superpowers/specs/2026-09-28-input-actions-editor-redesign-design.md` (spec B); plan `docs/superpowers/plans/2026-09-28-inspector-ownership-and-input-editor.md`. Every item below was mapped to code and then adversarially checked (2026-09-29 debt-map run, 30 agents); the corrections the checkers made are already folded in.

## Global Constraints

- Branch: `chore/arc1-debt-sweep` off Arcane main `4e9796ba`. Never push; the user decides integration.
- Build (whole solution, needed before any witness run because the editor exe is staged):
  `"/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nr:false -v:minimal -nologo`
- Fast loop (tests only):
  `"/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" ArcaneTests/ArcaneTests.vcxproj -p:Configuration=Debug -p:Platform=x64 -p:SolutionDir=D:\\dev\\starworks\\Arcane\\ -m -nr:false -v:minimal -nologo`
- Run tests FROM THE EXE DIR: `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "<filter>"`. The order is random; on a failure capture the "Randomness seeded to" line and re-run with `--rng-seed N`.
- A NEW test `.cpp` needs a premake regen (ArcaneTests globs `src/**.cpp`): `_APH_NOPAUSE=1 ARCANE_SDK='D:\dev\starworks\Arcane' cmd //c "scripts\generate.bat"`.
- Debug builds use `/ZI`: Catch2 assertion line numbers in a Debug run are WRONG (TEST_CASE lines are right). Trust the case name, read the source.
- Runtime in tests: `Arcane::Runtime runtime(Arcane::Test::Process());` (Runtime takes `ProcessContext&`). None of this plan's tests needs one.
- Before a build: `tasklist | grep -qiE "^(cl|link)\.exe"` means a build is live; wait. `LNK1104` on `ArcaneEditor.exe` means the user's editor is running; relink after they close it, never kill it.
- No `PluginABI.hpp` change. The engine edits (Tasks 8, 9) change behaviour only, no header layout.
- Commit per task, message style `fix(editor): ...` / `test(editor): ...` / `fix(input): ...`, ending with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
  ```

## Decisions taken (recommended defaults; the user may overturn at review)

- **D1 (item E):** fix `AddPart` to select top-down FIRST, then adopt the strict 4-segment key grammar (gaps rejected). The checker proved a gap key IS reachable today via right-click > Add part.
- **D2 (item E):** the key codec gets its own header `Documents/InputSelectionKey.hpp` (the `SceneSelectionKey.hpp` precedent), not `InputActionsJson.hpp`. The shared trim is `TrimName` in `InputActionsJson.hpp` (a generic `Trim` would collide with `AssetGraphViewModel.cpp`'s).
- **D3 (item A):** Problems publish from `Tick`, memoised on a new `DraftRevision()`; the Draw-path call is removed.
- **D4 (item B):** the page's Name rows mirror the column rename box: Enter on a refused name keeps the text and re-arms the box; focus loss reverts; the reason shows as a tooltip while typing.
- **D5 (item C):** a press on a floating document's chrome is CLAIMED: it neither binds nor cancels; the capture stays armed.
- **D6 (item D):** conflict lines read `Conflicting '<path>': Player/Jump and Player/Crouch share it in scheme KeyboardMouse`; the grouped-vs-ungrouped label is fixed; all overlapping schemes are named; a same-action duplicate reads `Player/Jump binds it twice`.
- **D7 (item I1):** option (a): the page's Rebind scrolls to the capture row when it is drawn and expands its collapsed action; an other-map or filtered target is left as is.
- **D8 (item I2):** the capture term goes into `ShortcutsLive`, so F/Home, Alt+G/J AND W/E/R/Q all stand down.
- **D9 (item I3):** a mixed-device chord has an empty `device`; each part names its own device in `control`.
- **D10 (item I4):** `"+<"` is the only chord separator; the control keeps SDL's name. `"<Keyboard>/a+"` stops compiling to `a` (it was already flagged unknown).
- **D11 (item H4):** test-only E3b (exit 0, fallback breadcrumb, the stderr line); no report-schema bump.
- **D12 (item F):** NOT a defect as written: `ClearAllFn` is never called and the instance list is layout by design (`EditorInspectorHostTest.cpp:199`). The real bug is bigger: a windowed project switch never loads the incoming project's `imgui.ini`. Recorded as a new owed item (Task 12); not fixed here.
- **D13 (H2 sibling):** the Outliner's identical key guard gets the same `NoPopupHierarchy` flag (Task 10).

## Review Focus

1. **A key the editor itself produces must always resolve.** After Task 1 any `SelectionKey()` the model emits (including after Add part from a context menu on a deselected column) must satisfy `Resolves()` and give a non-null `PageFor()`; the Inspector must never go blank on a live selection. Pinned in Task 1.
2. **Problems rows follow the draft, not the window.** Undo/redo with the tab hidden, a pinned-page edit with the tab hidden, and a save must all leave Problems matching `Warnings()` one frame later. Pinned in Task 2.
3. **A refused name never leaves stale text on screen.** Enter-hold must expire if focus never returns (row not drawn, or 3 frames without activation). Pinned in Task 4 (the focus-loss half) and Task 10 (the page half).
4. **No shortcut fires on the completing key of a capture**, including gizmo W/E/R/Q while the cursor hovers the Viewport. Pinned by the predicate test in Task 7; the wiring is desk-verified.
5. **A captured Keypad + (or Keypad +/-) path compiles, displays and conflicts like any other key**, and a game-side rebind to it is accepted. Pinned in Task 8.

---

### Task 1: One selection-key grammar, and Add part selects top-down (item E)

**Files:**
- Create: `ArcaneEditor/src/Documents/InputSelectionKey.hpp`
- Modify: `ArcaneEditor/src/Documents/InputActionsJson.hpp` (add `TrimName`)
- Modify: `ArcaneEditor/src/Documents/InputActionsEditorModel.hpp:57-61,103,107` and `.cpp:167-172,253-329,570-588`
- Modify: `ArcaneEditor/src/Documents/InputActionsDocument.cpp:51-78`
- Modify: `ArcaneEditor/src/Documents/InputActionsInspectorPage.cpp:70-104`
- Modify: `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp:33-41,109`
- Test: `ArcaneTests/src/InputActionsEditorModelTest.cpp`

**Interfaces:**
- Produces (namespace `Arcane::Editor`, header-only):
  - `std::vector<std::string_view> SplitKey(std::string_view key)`
  - `std::optional<std::array<Guid, 4>> ParseSelectionKey(std::string_view key)`
  - `std::string EncodeSelectionKey(const std::array<Guid, 4>& ids)`
  - `std::string TrimName(std::string_view s)` (in `InputActionsJson.hpp`)
  - `std::optional<std::array<Guid, 4>> InputActionsEditorModel::ResolveKey(std::string_view key) const` (public; replaces private `ParseKey`)

- [ ] **Step 1: Write the failing tests** (append to `InputActionsEditorModelTest.cpp`; add `#include "Documents/InputSelectionKey.hpp"`)

```cpp
TEST_CASE("input editor: Add part from a deselected column selects top-down, so its key resolves", "[editor][input][inspector]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto jump = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    REQUIRE(model.AddComposite(map, jump, "1DAxis"));
    const Arcane::Guid composite = model.SelectedBinding();
    REQUIRE(composite.IsValid());
    model.DeselectToMap(map);                                   // empty space under the rows
    REQUIRE(model.AddPart(composite, "positive", "<Keyboard>/d")); // right-click the header > Add part
    CHECK(model.SelectedMap() == map);
    CHECK(model.SelectedAction() == jump);
    CHECK(model.SelectedBinding() == composite);
    CHECK(model.SelectedPart().IsValid());
    CHECK(model.Resolves(model.SelectionKey()));
}

TEST_CASE("input editor: ParseSelectionKey is the one 4-segment grammar", "[editor][input]")
{
    using Arcane::Editor::ParseSelectionKey;
    using Arcane::Editor::SplitKey;
    const std::string M = "22222222-2222-4222-8222-222222222222", A = "33333333-3333-4333-8333-333333333333",
                      B = "44444444-4444-4444-8444-444444444444", P = "55555555-5555-4555-8555-555555555555";
    CHECK(ParseSelectionKey(M + "///"));
    CHECK(ParseSelectionKey(M + "/" + A + "//"));
    CHECK(ParseSelectionKey(M + "/" + A + "/" + B + "/"));
    REQUIRE(ParseSelectionKey(M + "/" + A + "/" + B + "/" + P));
    CHECK((*ParseSelectionKey(M + "/" + A + "/" + B + "/" + P))[3].ToString() == P);
    for (const std::string bad : { std::string{}, std::string("///"), "/" + A + "//", M, M + "/", M + "//", M + "////",
                                   M + "/" + A + "/" + B + "/" + P + "/", M + "/" + A + "/" + B + "/" + P + "/junk",
                                   M + "//" + B + "/", M + "/bogus//", std::string("00000000-0000-0000-0000-000000000000///"),
                                   std::string("not-a-key") })
    {
        INFO(bad);
        CHECK_FALSE(ParseSelectionKey(bad));
    }
    CHECK(SplitKey("") == std::vector<std::string_view>{ "" });
    CHECK(SplitKey("a/") == std::vector<std::string_view>{ "a", "" });
    CHECK(SplitKey("a//b").size() == 3);
    CHECK(Arcane::Editor::EncodeSelectionKey({ *Arcane::Guid::FromString(M), {}, {}, {} }) == M + "///");
    CHECK(Arcane::Editor::EncodeSelectionKey({}).empty());
}

TEST_CASE("input document: PageFor and Resolves agree on every key shape", "[editor][input][inspector]")
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / ("key-agree-" + Arcane::Guid::Generate().ToString() + ".arcinput");
    { std::ofstream out(path); out << DocumentJson().dump(2); }
    {
        auto doc = Arcane::Editor::InputActionsDocument::Open(path);
        REQUIRE(doc);
        const std::string M = "22222222-2222-4222-8222-222222222222", A = "33333333-3333-4333-8333-333333333333",
                          B = "44444444-4444-4444-8444-444444444444";
        for (const std::string key : { std::string("///"), "/" + A + "//", M + "////", M + "/" + A + "/" + B + "/junk", M + "//" + B + "/" })
        {
            INFO(key);
            CHECK(doc->PageFor(key) == nullptr);
            CHECK_FALSE(doc->Resolves(key));
        }
        CHECK(doc->PageFor("") != nullptr);          // the asset root: the one intended asymmetry
        CHECK_FALSE(doc->Resolves(""));
        REQUIRE(doc->SelectByPath("Player/Jump/0"));
        for (const auto& crumb : doc->Page()->Breadcrumb())
        {
            if (!crumb.key || crumb.key->empty()) continue;
            INFO(*crumb.key);
            CHECK(doc->PageFor(*crumb.key) != nullptr);
            CHECK(doc->Resolves(*crumb.key));
        }
    }
    fs::remove(path);
}

TEST_CASE("input editor: RestoreSelectionOrAncestor treats a malformed key as no selection", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const std::string M = "22222222-2222-4222-8222-222222222222", A = "33333333-3333-4333-8333-333333333333";
    model.SelectMap(*Arcane::Guid::FromString(M));
    model.SelectAction(*Arcane::Guid::FromString(A));
    const auto epoch = model.SelectionEpoch();
    model.RestoreSelectionOrAncestor(M + "/bogus//");
    CHECK(model.SelectionKey().empty());
    model.RestoreSelectionOrAncestor(M + "/" + A + "/" + Arcane::Guid::Generate().ToString() + "/");   // a deleted binding
    CHECK(model.SelectedAction().ToString() == A);                                                     // falls back to its action
    CHECK_FALSE(model.SelectedBinding().IsValid());
    CHECK(model.SelectionEpoch() == epoch);                                                            // silent throughout
}
```

- [ ] **Step 2: Run to verify they fail**

Build the fast loop; expect a compile failure on `InputSelectionKey.hpp` (missing). That is the red.

- [ ] **Step 3: Create `InputSelectionKey.hpp`**

```cpp
#pragma once

// The Input Actions document's Inspector selection key (spec A s3.1, spec B
// s2.4): "<map>/<action>/<binding>/<part>", THREE slashes ALWAYS, filled
// top-down with no gaps. One grammar for every producer (the model's
// SelectionKey, the page's crumbs) and every consumer (Resolves,
// RestoreSelection, RestoreSelectionOrAncestor, the document's PageFor). The
// asset root "" is each caller's own case. SceneSelectionKey.hpp is the
// scene's counterpart.

#include <Arcane/Guid.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    // Every '/'-separated segment, empty ones kept: "" -> {""}, "a/" -> {"a",""}.
    [[nodiscard]] inline std::vector<std::string_view> SplitKey(std::string_view key)
    {
        std::vector<std::string_view> segs;
        for (std::size_t start = 0;;)
        {
            const std::size_t slash = key.find('/', start);
            segs.push_back(key.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
            if (slash == std::string_view::npos) return segs;
            start = slash + 1;
        }
    }

    // Syntax only, no existence check. nullopt unless: EXACTLY 4 segments;
    // segment 0 a valid Guid; every other segment empty or a valid Guid; no
    // non-empty segment after an empty one.
    [[nodiscard]] inline std::optional<std::array<Guid, 4>> ParseSelectionKey(std::string_view key)
    {
        const auto segs = SplitKey(key);
        if (segs.size() != 4) return std::nullopt;
        std::array<Guid, 4> ids{};
        bool ended = false;
        for (std::size_t i = 0; i < 4; ++i)
        {
            if (segs[i].empty()) { if (i == 0) return std::nullopt; ended = true; continue; }
            if (ended) return std::nullopt;   // a gap
            const auto id = Guid::FromString(segs[i]);
            if (!id || !id->IsValid()) return std::nullopt;
            ids[i] = *id;
        }
        return ids;
    }

    // The inverse: "" when ids[0] is invalid, else four segments joined by '/'.
    [[nodiscard]] inline std::string EncodeSelectionKey(const std::array<Guid, 4>& ids)
    {
        if (!ids[0].IsValid()) return {};
        auto seg = [](const Guid& g) { return g.IsValid() ? g.ToString() : std::string{}; };
        return seg(ids[0]) + "/" + seg(ids[1]) + "/" + seg(ids[2]) + "/" + seg(ids[3]);
    }
}
```

If `Guid::FromString` does not accept `std::string_view` directly, wrap the argument in `std::string(...)`.

- [ ] **Step 4: Add `TrimName` to `InputActionsJson.hpp`** (add `#include <cctype>` and `<string_view>`)

```cpp
    // Leading/trailing whitespace (std::isspace) stripped: THE name-rule trim
    // for ValidateName, SetField, AddMap/AddAction and the rename box.
    [[nodiscard]] inline std::string TrimName(std::string_view s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
        return std::string(s);
    }
```

Delete the model's anonymous-namespace `Trim` (`InputActionsEditorModel.cpp:167-172`) and rename its callers (`:464, :502, :622, :627, :644, :655`) to `TrimName`. Delete the widgets' `Trim` (`InputActionsDocumentWidgets.cpp:35-41`) and make the rename box at `:109` call `TrimName`; update the comment at `:33-34` to say it is the shared name-rule trim. The widget box now trims the full `isspace` set (it trimmed only space and tab).

- [ ] **Step 5: Model changes**

In `InputActionsEditorModel.hpp`: add `#include "Documents/InputSelectionKey.hpp"`; replace the private `ParseKey` declaration with a public
`[[nodiscard]] std::optional<std::array<Guid, 4>> ResolveKey(std::string_view key) const;   // ParseSelectionKey + every named id exists`.

In `InputActionsEditorModel.cpp`:

```cpp
    std::string InputActionsEditorModel::SelectionKey() const
    { return EncodeSelectionKey({ selectedMap_, selectedAction_, selectedBinding_, selectedPart_ }); }

    std::optional<std::array<Guid, 4>> InputActionsEditorModel::ResolveKey(std::string_view key) const
    {
        auto ids = ParseSelectionKey(key);
        if (!ids) return std::nullopt;
        for (const Guid& g : *ids) if (g.IsValid() && !FindNode(g)) return std::nullopt;
        return ids;
    }
    bool InputActionsEditorModel::Resolves(std::string_view key) const { return ResolveKey(key).has_value(); }
    bool InputActionsEditorModel::RestoreSelection(std::string_view key)
    {
        const auto ids = ResolveKey(key);
        if (!ids) return false;
        SelectMap((*ids)[0]);
        if ((*ids)[1].IsValid()) SelectAction((*ids)[1]);
        if ((*ids)[2].IsValid()) SelectBinding((*ids)[2]);
        if ((*ids)[3].IsValid()) SelectPart((*ids)[3]);
        return true;
    }
    void InputActionsEditorModel::RestoreSelectionOrAncestor(std::string_view key)
    {
        // Strict parse (a malformed key selects nothing), then keep the deepest
        // chain of levels that still exist: a deleted binding falls back to its action.
        std::array<Guid, 4> raw{};
        if (const auto ids = ParseSelectionKey(key)) raw = *ids;
        std::array<Guid, 4> keep{};
        for (std::size_t level = 0; level < 4; ++level)
        {
            if (!raw[level].IsValid() || !FindNode(raw[level])) break;
            keep[level] = raw[level];
        }
        SetSelectionSilently(keep);
    }
```

In `SelectByPath` replace the local `segs` loop with `const auto segs = SplitKey(namePath);` (keep the `segs.size() > 4 || segs[0].empty()` guard) and pass `std::string(segs[i])` where `unique`/`index` take a `std::string`.

`AddPart` selects top-down. Add a private helper and use it:

```cpp
    // The map and action that own `binding` in the draft; false when none.
    bool InputActionsEditorModel::OwnerOfBinding(const Guid& binding, Guid& map, Guid& action) const
    {
        if (!draft_.is_object() || !draft_.contains("actionMaps") || !draft_["actionMaps"].is_array()) return false;
        for (const auto& m : draft_["actionMaps"])
        {
            if (!m.is_object() || !m.contains("actions") || !m["actions"].is_array()) continue;
            for (const auto& a : m["actions"])
            {
                if (!a.is_object() || !a.contains("bindings") || !a["bindings"].is_array()) continue;
                for (const auto& b : a["bindings"])
                    if (IdIs(b, binding)) { map = IdOf(m); action = IdOf(a); return map.IsValid() && action.IsValid(); }
            }
        }
        return false;
    }
```

(declare `bool OwnerOfBinding(const Guid&, Guid&, Guid&) const;` in the private section) and in `AddPart` replace `SelectBinding(binding); SelectPart(id);` with

```cpp
        Guid map, action;
        if (OwnerOfBinding(binding, map, action)) { SelectMap(map); SelectAction(action); }
        SelectBinding(binding); SelectPart(id);
```

- [ ] **Step 6: Document and page**

`InputActionsDocument::PageFor` becomes:

```cpp
    InspectorPage* InputActionsDocument::PageFor(std::string_view key)
    {
        InputSelection sel;
        if (!key.empty())
        {
            const auto ids = model_.ResolveKey(key);   // the model's one grammar + existence
            if (!ids) return nullptr;                   // a pinned page for a deleted binding is "gone"
            sel = { (*ids)[0], (*ids)[1], (*ids)[2], (*ids)[3] };
        }
        page_.SetSelection(sel);
        return &page_;   // an empty key IS a page: the asset page (spec A s3.1, container fallback)
    }
```

Drop `<array>` from its includes if nothing else uses it. In `InputActionsInspectorPage::Breadcrumb` build every crumb key through `EncodeSelectionKey` (map: `{sel_.map,{},{},{}}`; action: `{sel_.map, sel_.action, {}, {}}`; binding: `{..., sel_.binding, {}}`; part: all four) instead of the hand-built `"///"`, `"//"`, `"/"` suffixes, and change the comment at `:74` to "PageFor rejects any other shape".

- [ ] **Step 7: Run the tests**

`./ArcaneTests.exe "[editor][input]"` then `./ArcaneTests.exe "[inspector]"`. Expected: all pass, including the existing `:243-313`, `:425-467` cases.

- [ ] **Step 8: Commit**

```bash
git add ArcaneEditor/src/Documents ArcaneTests/src/InputActionsEditorModelTest.cpp
git commit -m "fix(editor): one input selection-key grammar (InputSelectionKey.hpp: split/parse/encode, 4 segments, no gaps) for the model, PageFor and the crumbs; Add part selects top-down so its key always resolves; one TrimName (arc-1 debt E)"
```

---

### Task 2: Problems rows follow the draft, not the window (items A + H3)

**Files:**
- Modify: `ArcaneEditor/src/Documents/InputActionsEditorModel.hpp` / `.cpp:224-231,781`
- Modify: `ArcaneEditor/src/Documents/InputActionsDocument.hpp:45,112-115` / `.cpp:225,231-235`
- Test: `ArcaneTests/src/InputActionsEditorModelTest.cpp`

**Interfaces:**
- Produces: `std::uint64_t InputActionsEditorModel::DraftRevision() const noexcept`; `void InputActionsDocument::Tick(double) override`; `const std::string& InputActionsDocument::DiagnosticKey() const noexcept`.

- [ ] **Step 1: Write the failing tests** (add `#include "Panels/DiagnosticStore.hpp"`)

```cpp
namespace
{
    // DocumentJson plus one conflict (Crouch's ungrouped space) and one unknown path (Fire's 'spaec').
    nlohmann::json WarningsJson()
    {
        auto json = DocumentJson();
        auto& actions = json["actionMaps"][0]["actions"];
        actions.push_back(nlohmann::json::parse(R"JSON({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","name":"Crouch","type":"Button",
            "bindings":[{"id":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","path":"<Keyboard>/space"}]})JSON"));
        actions.push_back(nlohmann::json::parse(R"JSON({"id":"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee","name":"Fire","type":"Button",
            "bindings":[{"id":"12121212-1212-4121-8121-121212121212","path":"<Keyboard>/spaec"}]})JSON"));
        return json;
    }
}

TEST_CASE("input editor: DraftRevision bumps on every draft change and never on selection or save", "[editor][input]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::CommandStack commands([&]() -> Astra::Registry& { return runtime.Registry(); });
    Arcane::Editor::InputActionsEditorModel model(DocumentJson(), &commands);
    const auto r0 = model.DraftRevision();
    model.SelectMap(*Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222"));
    model.SelectAction(*Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333"));
    CHECK(model.DraftRevision() == r0);
    REQUIRE(model.SetField(*Arcane::Guid::FromString("44444444-4444-4444-8444-444444444444"), "path", "<Keyboard>/k"));
    const auto r1 = model.DraftRevision();
    CHECK(r1 > r0);
    const auto file = std::filesystem::temp_directory_path() / ("rev-" + Arcane::Guid::Generate().ToString() + ".arcinput");
    REQUIRE(model.Save(file));
    CHECK(model.DraftRevision() == r1);
    REQUIRE(model.Undo());
    CHECK(model.DraftRevision() > r1);
    const auto r2 = model.DraftRevision();
    REQUIRE(model.Redo());
    CHECK(model.DraftRevision() > r2);
    std::filesystem::remove(file);
}

TEST_CASE("input document: Tick publishes its Warnings as asset rows with no draw, keeps them over a save, retracts a fixed one, clears on close", "[editor][input][diagnostics]")
{
    namespace fs = std::filesystem;
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();
    const auto path = fs::temp_directory_path() / ("problems-" + Arcane::Guid::Generate().ToString() + ".arcinput");
    { std::ofstream out(path); out << WarningsJson().dump(2); }
    {
        auto doc = Arcane::Editor::InputActionsDocument::Open(path);
        REQUIRE(doc);
        CHECK(doc->DiagnosticKey() == "input:11111111-1111-4111-8111-111111111111");
        const auto w = doc->Model().Warnings();
        REQUIRE(w.size() >= 2);
        doc->Tick(0.0);                                                   // no ImGui frame, no Draw
        auto rows = store.Snapshot();
        REQUIRE(rows.size() == w.size());
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            CHECK(rows[i].message == w[i]);
            CHECK(rows[i].severity == Arcane::DiagSeverity::Warning);
            CHECK(rows[i].scope == Arcane::DiagScope::Assets);
            CHECK(rows[i].locator.kind == Arcane::DiagLocator::Kind::Asset);
            CHECK(rows[i].locator.asset == doc->AssetGuid());
            CHECK(rows[i].detail == path.stem().string() + ".arcinput");
        }
        CHECK(std::any_of(rows.begin(), rows.end(), [](const auto& d) { return d.code == "input.path.unknown"; }));
        CHECK(std::any_of(rows.begin(), rows.end(), [](const auto& d) { return d.code == "input.binding.conflict"; }));
        const auto n0 = rows.size();
        REQUIRE(doc->Save());                                             // a save changes no draft: rows persist
        doc->Tick(0.0);
        CHECK(store.Snapshot().size() == n0);
        REQUIRE(doc->Model().SetField(*Arcane::Guid::FromString("12121212-1212-4121-8121-121212121212"), "path", "<Keyboard>/k"));
        doc->Tick(0.0);                                                   // as a hidden tab's page edit would be picked up
        rows = store.Snapshot();
        CHECK(rows.size() == n0 - 1);
        CHECK_FALSE(std::any_of(rows.begin(), rows.end(), [](const auto& d) { return d.code == "input.path.unknown"; }));
    }
    CHECK(store.Snapshot().empty());                                      // closing clears
    store.UninstallEngineSink();
    fs::remove(path);
}
```

- [ ] **Step 2: Run to verify they fail** (compile error on `DraftRevision` / `DiagnosticKey`).

- [ ] **Step 3: Implement**

Model header: `[[nodiscard]] std::uint64_t DraftRevision() const noexcept { return draftRevision_; }   // bumped by every draft write (Validate is the one funnel)` and a private `std::uint64_t draftRevision_ = 0;`. Model cpp: first line of `Validate()` is `++draftRevision_;`. Above `Warnings()` add the comment: "A pure function of the draft (draft_/preview_, set only by Validate) plus SDL's key-name/layout state; the document memoises it on DraftRevision(), so a keyboard-layout change shows on the next edit."

Document header: add `void Tick(double) override { PublishWarnings(); }   // every frame, visible or not: a hidden tab's page edit or undo still reaches Problems` and `[[nodiscard]] const std::string& DiagnosticKey() const noexcept { return diagKey_; }` in the public section; add private `std::uint64_t publishedRevision_ = 0;`.

Document cpp: delete `PublishWarnings();` from `Draw` (`:225`). `PublishWarnings` begins:

```cpp
        if (model_.DraftRevision() == publishedRevision_) return;   // Warnings() runs once per draft revision, not per frame
        publishedRevision_ = model_.DraftRevision();
```

and keeps the rest (the output-diff gate stays).

- [ ] **Step 4: Run** `./ArcaneTests.exe "[editor][input]"` and `./ArcaneTests.exe "[diagnostics]"`. Expected: pass.

- [ ] **Step 5: Commit**

```bash
git commit -am "fix(editor): the input document publishes Problems from Tick, memoised on a new DraftRevision -- a hidden tab's page edit or undo reaches Problems; Warnings no longer recomputed every frame; publication tested headless (arc-1 debt A + H3)"
```

---

### Task 3: A conflict line names both actions, the map and the true schemes (item D)

**Files:**
- Modify: `ArcaneEditor/src/Documents/InputActionsEditorModel.hpp:88-91` / `.cpp:743-779,819-826`
- Modify: `ArcaneEditor/src/Documents/InputActionsRows.hpp` / `.cpp` (add `ConflictTooltip`)
- Modify: `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp:503-508`
- Test: `ArcaneTests/src/InputActionsEditorModelTest.cpp`, `ArcaneTests/src/InputActionsRowsTest.cpp`

**Interfaces:**
- Produces: `BindingConflict` gains, APPENDED after `group` (positional init at `InputActionsRowsTest.cpp:81` must keep compiling): `Guid action; std::string actionName; Guid map; std::string mapName; std::string scheme;` (`scheme` = the overlapping schemes' display names joined `", "`, empty when `group == "*"`). Free function `std::string ConflictTooltip(const std::vector<InputActionsEditorModel::BindingConflict>&, const Guid& row)` in `InputActionsRows.hpp`.

- [ ] **Step 1: Write the failing tests**

In the existing `Conflicts and Warnings` case (`:374`), after the `:414` check, add:

```cpp
    for (const std::string expected : {
            std::string("Conflicting '<Keyboard>/space': Player/Jump and Player/Crouch share it in scheme KeyboardMouse"),
            std::string("Conflicting '<Keyboard>/scancode/space': Player/Crouch and Player/Fire share it in scheme Gamepad"),
            std::string("Conflicting '<Mouse>/button/1': Player/Aim and Player/Block share it in every scheme"),
            std::string("Conflicting '<Keyboard>/lshift+<Keyboard>/a': Player/Aim and Player/Block share it in every scheme") })
    {
        INFO(expected);
        CHECK(std::find(warnings.begin(), warnings.end(), expected) != warnings.end());
    }
    const auto jumpConflict = std::find_if(conflicts.begin(), conflicts.end(), [](const auto& c) { return c.binding.ToString() == "44444444-4444-4444-8444-444444444444"; });
    REQUIRE(jumpConflict != conflicts.end());
    CHECK(jumpConflict->actionName == "Jump"); CHECK(jumpConflict->otherActionName == "Crouch");
    CHECK(jumpConflict->mapName == "Player"); CHECK(jumpConflict->scheme == "KeyboardMouse");
```

In the `same path warnings remain nonblocking` case (`:176`, which adds a second `<Keyboard>/space` to Jump), after its `CHECK_FALSE(model.Warnings().empty());` add:

```cpp
    const auto w = model.Warnings();
    CHECK(std::find(w.begin(), w.end(), std::string("Conflicting '<Keyboard>/space': Player/Jump binds it twice in every scheme")) != w.end());
```

New case:

```cpp
TEST_CASE("input editor: a conflict line names the scheme's display name and every overlapping scheme; conflicts stay per map", "[editor][input]")
{
    auto json = nlohmann::json::parse(R"JSON({
        "version":1,"id":"11111111-1111-4111-8111-111111111111","defaultMap":"22222222-2222-4222-8222-222222222222",
        "controlSchemes":[{"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","name":"Keyboard and Mouse","bindingGroup":"KeyboardMouse"},
                          {"id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","name":"Gamepad","bindingGroup":"Gamepad"}],
        "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
            {"id":"33333333-3333-4333-8333-333333333333","name":"Jump","type":"Button","bindings":[
              {"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/space","groups":["KeyboardMouse","Gamepad"]}]},
            {"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","name":"Crouch","type":"Button","bindings":[
              {"id":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","path":"<Keyboard>/space"}]}]},
          {"id":"99999999-9999-4999-8999-999999999999","name":"Menus","actions":[
            {"id":"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee","name":"Confirm","type":"Button","bindings":[
              {"id":"ffffffff-ffff-4fff-8fff-ffffffffffff","path":"<Keyboard>/space"}]}]}]})JSON");
    Arcane::Editor::InputActionsEditorModel model(std::move(json));
    REQUIRE(model.LastValidPreview());
    const auto w = model.Warnings();
    CHECK(std::count_if(w.begin(), w.end(), [](const std::string& s) { return s.starts_with("Conflicting"); }) == 1);
    CHECK(std::find(w.begin(), w.end(), std::string("Conflicting '<Keyboard>/space': Player/Jump and Player/Crouch share it in schemes Keyboard and Mouse, Gamepad")) != w.end());
}
```

In `InputActionsRowsTest.cpp`:

```cpp
TEST_CASE("input rows: ConflictTooltip names each other action once with its schemes", "[editor][input]")
{
    using C = InputActionsEditorModel::BindingConflict;
    const auto a = *Arcane::Guid::FromString("99999999-9999-4999-8999-999999999999");
    const auto b = *Arcane::Guid::FromString("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    std::vector<C> cs;
    C c1{}; c1.binding = a; c1.otherBinding = b; c1.otherActionName = "Crouch"; c1.group = "KeyboardMouse"; c1.scheme = "KeyboardMouse";
    C c2 = c1;                                   // the same other action again: named once
    C c3{}; c3.binding = a; c3.otherActionName = "Fire"; c3.group = "*";
    cs = { c1, c2, c3 };
    CHECK(ConflictTooltip(cs, a) == "Also bound by Crouch (KeyboardMouse), Fire (every scheme)");
    CHECK(ConflictTooltip(cs, b).empty());
}
```

- [ ] **Step 2: Run to verify they fail** (compile error on the new fields / `ConflictTooltip`).

- [ ] **Step 3: Implement**

Header: append the five fields after `group`; comment "both sides always share one map (conflicts are per map, spec B s2.3); `scheme` is display text".

`Conflicts()`: replace `overlap` with one returning every overlapping group, and add the name lookup:

```cpp
            // Every scheme group in which BOTH bindings are live. Ungrouped is
            // live in every scheme: both ungrouped -> {"*"}; one ungrouped ->
            // the grouped side's groups; else the intersection.
            auto overlap = [](const Entry& a, const Entry& b) -> std::vector<std::string> {
                if (a.groups.empty() && b.groups.empty()) return { "*" };
                if (a.groups.empty()) return b.groups;
                if (b.groups.empty()) return a.groups;
                std::vector<std::string> out;
                for (const auto& g : a.groups)
                    if (std::find(b.groups.begin(), b.groups.end(), g) != b.groups.end()) out.push_back(g);
                return out; };
            auto schemeNames = [&](const std::vector<std::string>& groups) {
                std::string out;
                if (groups.size() == 1 && groups[0] == "*") return out;
                for (const auto& g : groups)
                {
                    std::string name = g;
                    for (const auto& s : preview_->controlSchemes) if (s.bindingGroup == g) { name = s.name; break; }
                    if (!out.empty()) out += ", ";
                    out += name;
                }
                return out; };
```

and the push:

```cpp
                    const auto groups = overlap(entries[i], entries[j]);
                    if (groups.empty()) continue;
                    const std::string group = groups.front(), scheme = schemeNames(groups);
                    out.push_back({ entries[i].id, entries[j].id, entries[j].action, entries[j].actionName, entries[i].path, group,
                                    entries[i].action, entries[i].actionName, map.id, map.name, scheme });
                    out.push_back({ entries[j].id, entries[i].id, entries[i].action, entries[i].actionName, entries[i].path, group,
                                    entries[j].action, entries[j].actionName, map.id, map.name, scheme });
```

`Warnings()` text:

```cpp
            const std::string where = c.group == "*" ? std::string(" in every scheme")
                                    : (c.scheme.find(", ") != std::string::npos ? " in schemes " : " in scheme ") + c.scheme;
            if (c.action == c.otherAction)
                warnings.push_back("Conflicting '" + c.path + "': " + c.mapName + "/" + c.actionName + " binds it twice" + where);
            else
                warnings.push_back("Conflicting '" + c.path + "': " + c.mapName + "/" + c.actionName + " and " +
                                   c.mapName + "/" + c.otherActionName + " share it" + where);
```

`ConflictTooltip` in `InputActionsRows.cpp`:

```cpp
    std::string ConflictTooltip(const std::vector<InputActionsEditorModel::BindingConflict>& conflicts, const Guid& row)
    {
        std::string out;
        std::vector<std::string> seen;
        for (const auto& c : conflicts)
        {
            if (c.binding != row) continue;
            const std::string item = c.otherActionName + " (" + (c.group == "*" ? std::string("every scheme") : c.scheme) + ")";
            if (std::find(seen.begin(), seen.end(), item) != seen.end()) continue;
            seen.push_back(item);
            out += out.empty() ? "Also bound by " + item : ", " + item;
        }
        return out;
    }
```

Widgets `:503-508`: `ImGui::SetTooltip("%s", ConflictTooltip(model.Conflicts(), row.id).c_str());`.

- [ ] **Step 4: Run** `./ArcaneTests.exe "[editor][input]"`. Expected: pass; `:414`'s count of 4 is unchanged.

- [ ] **Step 5: Commit**

```bash
git commit -am "fix(editor): input conflict lines name both actions with their map and the schemes they really collide in (grouped-vs-ungrouped no longer says 'every scheme'; a same-action duplicate says 'binds it twice'); the row tooltip adds the scheme (arc-1 debt D)"
```

---

### Task 4: A refused Inspector-page rename keeps its text (item B)

**Files:**
- Modify: `ArcaneEditor/src/Widgets/PropertyGrid.hpp:11-18,44-51,93-94` / `.cpp:47-100`
- Modify: `ArcaneEditor/src/Documents/InputActionsInspectorPage.cpp:199,231`
- Test: `ArcaneTests/src/PropertyGridTest.cpp`

**Interfaces:**
- Produces: `bool PropertyGrid::TextRow(const char* label, std::string_view current, std::function<void(std::string)> commit, bool dimmed = false, std::function<std::optional<std::string>(std::string_view)> validate = {})` (validator called synchronously, never stored). `TextDraft` gains, AFTER `commit`: `bool hold = false; int holdFrame = 0; bool focusPending = false;`.

- [ ] **Step 1: Write the failing test** (add to `GridHarness`: `std::function<std::optional<std::string>(std::string_view)> validate;` and pass `validate` as the Name row's 5th argument, `dimmed` stays `false`; add `#include <optional>`; add `void Type(const char* s) { ImGui::GetIO().AddInputCharactersUTF8(s); Frame(); }` and `void Key(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frame(); ImGui::GetIO().AddKeyEvent(k, false); Frame(); }`)

```cpp
TEST_CASE("PropertyGrid: a TextRow value refused on Enter keeps the typed text and re-arms; focus loss with a refused value reverts", "[editor][inspector]")
{
    GridHarness h;
    h.validate = [](std::string_view v) -> std::optional<std::string> { return v == "X" ? std::optional<std::string>("taken") : std::nullopt; };
    h.Frame();
    h.Click(h.Centre("Name"));
    h.Type("X");                                      // AutoSelectAll: replaces "Alpha"
    h.Key(ImGuiKey_Enter);
    h.Frame(); h.Frame();
    CHECK(h.commits == 0);
    CHECK(h.name == "Alpha");
    REQUIRE(h.state.textDrafts.size() == 1);
    CHECK(h.state.textDrafts.begin()->second.text == "X");      // kept
    CHECK(h.state.textDrafts.begin()->second.active);           // re-armed
    h.Type("Y");                                      // re-activation selected all: "Y" replaces "X"
    h.Click(ImVec2(600, 900));
    CHECK(h.commits == 1);
    CHECK(h.name == "Y");

    h.Click(h.Centre("Name"));
    h.Type("X");
    h.Click(ImVec2(600, 900));                        // focus loss with a refused value
    h.Frame();
    CHECK(h.commits == 1);
    CHECK(h.name == "Y");
    for (const auto& [id, d] : h.state.textDrafts) CHECK(d.text != "X");
}
```

- [ ] **Step 2: Run** `./ArcaneTests.exe "PropertyGrid*"`. Expected: the new case FAILS (the text vanishes, `textDrafts.size() == 1` or `text == "X"` fails). The four existing cases pass (the parameter is defaulted).

- [ ] **Step 3: Implement** in `PropertyGrid.cpp` `TextRow` (add `#include <optional>` to the header):

1. After `TextDraft& draft = it->second;`: expire a stale hold, and skip the re-seed while holding:
   ```cpp
        if (draft.hold && (draft.lastFrame + 1 < now || now > draft.holdFrame + 3)) draft.hold = false;   // row vanished, or focus never came back
        if (!inserted && !draft.hold && (!draft.active || draft.lastFrame + 1 < now) && draft.text != current)
   ```
2. Just before `InputTextString`: `if (draft.focusPending) { ImGui::SetKeyboardFocusHere(); draft.focusPending = false; }`.
3. Right after `InputTextString` (still the last item):
   ```cpp
        const std::optional<std::string> reason = (validate && draft.text != current) ? validate(draft.text) : std::nullopt;
        if (reason && ImGui::IsItemActive()) ImGui::SetItemTooltip("%s", reason->c_str());
   ```
4. After `draft.active = ImGui::IsItemActive();`: `if (draft.active) draft.hold = false;`.
5. In the non-`inserted` deactivation branch, before erasing:
   ```cpp
                const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
                if (reason && enter) { draft.hold = true; draft.holdFrame = now; draft.focusPending = true; ImGui::PopID(); return false; }   // keep text + re-arm (the rename box's rule)
                if (reason) { m_state.textDrafts.erase(it); ImGui::PopID(); return false; }                                            // focus loss: revert, no commit
   ```
   then the existing erase-then-commit path.

Update the contract comment (`PropertyGrid.hpp:11-18`) and the `TextRow` comment with: "An optional `validate` returns a refusal reason: shown as a tooltip while typing; Enter on a refused value keeps the text and re-arms the box; focus loss with a refused value reverts without committing. Mirrors the Input Actions rename box." Page: pass the rule to both Name rows:

```cpp
        grid.TextRow("Name", Str(map, "name"), /* existing commit lambda */, false,
                     [m = &model_, id](std::string_view v) { return InputActionsEditorModel::ValidateName(m->Draft(), id, v); });
```

(same for the action row at `:231`). Keep the deferred `SetField`; its `(void)` stays as the backstop.

- [ ] **Step 4: Run** `./ArcaneTests.exe "[inspector]"`. Expected: pass. If the "re-armed" check fails, confirm the Enter frame first; `GridHarness` sets no `NavEnableKeyboard`, and `SetKeyboardFocusHere` resolves through a nav request a frame or two later, which the `h.Frame(); h.Frame();` allows for.

- [ ] **Step 5: Commit**

```bash
git commit -am "fix(editor): the Inspector page's Name rows validate like the rename box -- the reason shows while typing, Enter on a refused name keeps the text and re-arms, focus loss reverts (PropertyGrid::TextRow validator) (arc-1 debt B)"
```

---

### Task 5: A press on a floating document's chrome never completes a capture (item C)

**Files:**
- Modify: `ArcaneEditor/src/Documents/InputActionsDocument.hpp` (the `SnapshotForCapture` declaration + comment) / `.cpp:123-129,165-172`
- Test: `ArcaneTests/src/InputActionsEditorModelTest.cpp:417-423`

**Interfaces:**
- Produces: `static InputSnapshot SnapshotForCapture(const InputSnapshot& raw, bool anyItemActive, bool pointerOnChrome);` and `static bool PressOnChrome(ImVec2 pressPos, ImVec2 contentMin, ImVec2 contentMax, float borderPad) noexcept;`

- [ ] **Step 1: Write the failing test** (and add a third argument `false` to both existing calls at `:420`/`:422`, plus `CHECK(Arcane::Editor::InputActionsDocument::SnapshotForCapture(raw, false, true).wantCaptureMouse);`)

```cpp
TEST_CASE("input document: a press on the window's chrome is claimed -- it neither binds nor is heard later; a content press still binds", "[editor][input]")
{
    using Doc = Arcane::Editor::InputActionsDocument;
    CHECK(Doc::PressOnChrome({ 50, 10 }, { 0, 20 }, { 400, 300 }, 4.0f));    // title bar
    CHECK_FALSE(Doc::PressOnChrome({ 50, 100 }, { 0, 20 }, { 400, 300 }, 4.0f));
    CHECK(Doc::PressOnChrome({ 398, 100 }, { 0, 20 }, { 400, 300 }, 4.0f));  // inner border band
    Arcane::InputRebindOperation op;
    op.Begin(*Arcane::Guid::FromString("44444444-4444-4444-8444-444444444444"), std::nullopt, 10.0f, Arcane::InputSnapshot{});
    Arcane::InputSnapshot down; down.mouseButtons = 0x1;
    op.Observe(Doc::SnapshotForCapture(down, false, true), 1.0f / 60.0f);         // the chrome press
    CHECK(op.Result().state == Arcane::InputRebindState::Waiting);
    op.Observe(Doc::SnapshotForCapture(down, false, false), 1.0f / 60.0f);        // still held, now over content
    CHECK(op.Result().state == Arcane::InputRebindState::Waiting);
    op.Observe(Doc::SnapshotForCapture(Arcane::InputSnapshot{}, false, false), 1.0f / 60.0f);
    CHECK(op.Result().state == Arcane::InputRebindState::Waiting);
    op.Observe(Doc::SnapshotForCapture(down, false, false), 1.0f / 60.0f);        // a fresh press in the content
    CHECK(op.Result().state == Arcane::InputRebindState::Completed);
    CHECK(op.Result().replacementPath == "<Mouse>/leftButton");
}
```

- [ ] **Step 2: Run to verify it fails** (compile error on the new signature).

- [ ] **Step 3: Implement**

```cpp
    InputSnapshot InputActionsDocument::SnapshotForCapture(const InputSnapshot& raw, bool anyItemActive, bool pointerOnChrome)
    {
        InputSnapshot s = raw;
        s.wantCaptureMouse = pointerOnChrome;   // the document owns the pointer inside its content; its title bar, borders and grips belong to the window
        s.wantCaptureKeyboard = anyItemActive;
        return s;
    }

    bool InputActionsDocument::PressOnChrome(ImVec2 p, ImVec2 lo, ImVec2 hi, float pad) noexcept
    { return p.x < lo.x + pad || p.y < lo.y + pad || p.x > hi.x - pad || p.y > hi.y - pad; }
```

In `TickCapture`, after the `clickedAway` loop (add `#include <imgui_internal.h>`, the `ShaderEditorDocument.cpp:45` precedent):

```cpp
        // A press that began on this window's own chrome (title bar, close/
        // collapse buttons, resize border or grip) is the WINDOW's: claim it,
        // so it neither binds nor is heard later (Observe latches the held bit
        // into previous_ on the claimed frame). Docked, the tab belongs to the
        // host window and the clickedAway rule above already cancels.
        bool onChrome = false;
        if (bodyDrawn)
        {
            const ImRect inner = ImGui::GetCurrentWindow()->InnerRect;
            for (int b = 0; b < ImGuiMouseButton_COUNT; ++b)
                if (ImGui::IsMouseDown(b) && PressOnChrome(ImGui::GetIO().MouseClickedPos[b], inner.Min, inner.Max, ImGui::GetStyle().WindowBorderHoverPadding))
                    onChrome = true;
            onChrome = onChrome || ImGui::IsAnyItemActive();   // the grip corner sits inside InnerRect; the columns are NoInputs, so an active item here is window decoration
        }
```

and pass `onChrome` as the third argument of `SnapshotForCapture` in the `Observe` call. `ImGui::IsAnyItemActive()` still also feeds the keyboard argument. Update the header comment accordingly.

- [ ] **Step 4: Run** `./ArcaneTests.exe "[editor][input]"`. Expected: pass.

- [ ] **Step 5: Commit**

```bash
git commit -am "fix(editor): a press on a floating input document's title bar, buttons, border or grip is claimed by the window -- it no longer completes a rebind as Left Mouse; the capture stays armed (arc-1 debt C)"
```

Desk item (record in the handoff): float the document, arm a Rebind, drag the title bar: nothing binds; press a key: it binds.

---

### Task 6: The page's Rebind scrolls to the capture row, not the selection (item I1)

**Files:**
- Modify: `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp:33-43` (state) and the class (new static)
- Modify: `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp:290-323,387`
- Modify: `ArcaneEditor/src/Documents/InputActionsDocument.hpp:89-93` / `.cpp:138-144`
- Test: `ArcaneTests/src/InputActionsRowsTest.cpp`

**Interfaces:**
- Produces: `Guid InputActionsDocumentState::scrollRowToId;` and `static bool InputActionsDocumentWidgets::ScrollsIntoView(const InputActionsEditorModel&, const InputActionsDocumentState&, const InputRow&)` (public).

- [ ] **Step 1: Write the failing test** (add `#include <Documents/InputActionsDocumentWidgets.hpp>`)

```cpp
TEST_CASE("input rows: ScrollsIntoView targets the capture row over the selection", "[editor][input]")
{
    const auto id = [](const char* s) { return *Arcane::Guid::FromString(s); };
    const auto rows = BuildInputRows(Fixture(), kMap, {}, {}, {});
    InputActionsEditorModel model(Fixture());
    model.SelectMap(kMap); model.SelectAction(id("33333333-3333-4333-8333-333333333333")); model.SelectBinding(id("77777777-7777-4777-8777-777777777777"));
    InputActionsDocumentState state;
    auto hits = [&] { std::vector<std::string> out; for (const auto& r : rows) if (InputActionsDocumentWidgets::ScrollsIntoView(model, state, r)) out.push_back(r.id.ToString() + (r.kind == InputRowKind::CompositeHeader ? "#h" : "")); return out; };
    state.scrollRowToSelection = true;
    state.scrollRowToId = id("99999999-9999-4999-8999-999999999999");   // Jump's Space: not selected
    CHECK(hits() == std::vector<std::string>{ "99999999-9999-4999-8999-999999999999" });
    state.scrollRowToId = id("55555555-5555-4555-8555-555555555555");   // a part
    CHECK(hits() == std::vector<std::string>{ "55555555-5555-4555-8555-555555555555" });
    state.scrollRowToId = id("44444444-4444-4444-8444-444444444444");   // the composite id: its header is not a capture row
    CHECK(hits().empty());
    state.scrollRowToId = {};
    CHECK(hits() == std::vector<std::string>{ "77777777-7777-4777-8777-777777777777" });   // the selection path is unchanged
    state.scrollRowToSelection = false;
    CHECK(hits().empty());
}
```

- [ ] **Step 2: Run to verify it fails** (compile error).

- [ ] **Step 3: Implement**

State: add after `scrollRowToSelection`:
`Guid scrollRowToId;   // one-shot, set by the Inspector page's Rebind...: scroll the actions column to THIS binding/part row (the capture row), winning over scrollRowToSelection; cleared on every DrawActions exit, so an undrawn target never fires later`.

Widgets:

```cpp
    bool InputActionsDocumentWidgets::ScrollsIntoView(const InputActionsEditorModel& model, const InputActionsDocumentState& state, const InputRow& row)
    {
        if (state.scrollRowToId.IsValid())
            return (row.kind == InputRowKind::Binding || row.kind == InputRowKind::Part) && row.id == state.scrollRowToId;
        return state.scrollRowToSelection && RowSelected(model, row);
    }
```

`DrawRow :387` becomes:

```cpp
        if (ScrollsIntoView(model, state, row))
        {
            ImGui::SetScrollHereY();
            if (state.scrollRowToId.IsValid()) state.scrollRowToId = {}; else state.scrollRowToSelection = false;
        }
```

`DrawActions`: first line `struct ClearScrollToId { InputActionsDocumentState& s; ~ClearScrollToId() { s.scrollRowToId = {}; } } clearScrollToId{ state };` so every exit (including the `!m` early return) drops the one-shot.

`BeginRebindFromPage`: replace `state_.scrollRowToSelection = true;` with

```cpp
        state_.scrollRowToId = target;   // the countdown row, which a PINNED page's target need not be the selection of
        // A collapsed owner would hide the countdown row: expand it (view state, not a selection event).
        if (const auto* owner = OwnerActionOfBinding(model_.Draft(), target)) state_.collapsedActions.erase(IdOf(*owner).ToString());
```

with this file-local helper in `InputActionsDocument.cpp`'s anonymous namespace (add `#include "Documents/InputActionsJson.hpp"` if not already reachable):

```cpp
        // The action whose bindings (or a composite's parts) carry `target`; nullptr when none.
        const nlohmann::json* OwnerActionOfBinding(const nlohmann::json& draft, const Guid& target)
        {
            if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return nullptr;
            for (const auto& m : draft["actionMaps"])
            {
                if (!m.is_object() || !m.contains("actions") || !m["actions"].is_array()) continue;
                for (const auto& a : m["actions"])
                    if (a.is_object() && a.contains("bindings") && FindById(a["bindings"], target)) return &a;
            }
            return nullptr;
        }
```

Update the header comment at `:89-92` ("scrolls the CAPTURE row into view").

- [ ] **Step 4: Run** `./ArcaneTests.exe "[editor][input]"`. Expected: pass.

- [ ] **Step 5: Commit**

```bash
git commit -am "fix(editor): the Inspector page's Rebind... scrolls the document to the CAPTURE row (a pinned page's binding, not the selection), expands its collapsed action, and the one-shot never lingers (arc-1 debt I1)"
```

Desk item: pin the Inspector on Jump's Space, select Move's W in the document, scroll Space off-screen, press the pinned page's Rebind...: the Space row comes into view reading "Press a control...".

---

### Task 7: No app shortcut fires while a capture owns the keyboard (item I2)

**Files:**
- Modify: `ArcaneEditor/src/Viewport/ViewportInput.hpp` (new pure predicate)
- Modify: `ArcaneEditor/src/App/EditorApp.hpp` (member) and `EditorAppFrame.cpp:751-755,864-869,896-910`
- Test: `ArcaneTests/src/EditorViewportInputTest.cpp`

**Interfaces:**
- Produces: `inline bool EditorShortcutsLive(bool playMode, bool wantCaptureKeyboard, bool rebindCaptureLive, bool requireViewportFocus, bool viewportActive) noexcept`; `bool EditorApp::m_rebindCaptureLive`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("EditorShortcutsLive stands down while a rebind capture owns the keyboard", "[editor]")
{
    using Arcane::Editor::EditorShortcutsLive;
    CHECK(EditorShortcutsLive(false, false, false, false, false));          // idle Edit mode
    CHECK_FALSE(EditorShortcutsLive(false, false, true, false, false));     // F/Home, Alt+G/J, undo/redo gates
    CHECK_FALSE(EditorShortcutsLive(false, false, true, true, true));       // W/E/R/Q over the Viewport
    CHECK_FALSE(EditorShortcutsLive(true, false, false, false, false));     // Play
    CHECK_FALSE(EditorShortcutsLive(false, true, false, false, false));     // typing
    CHECK_FALSE(EditorShortcutsLive(false, false, false, true, false));     // viewport keys without viewport focus
    CHECK(EditorShortcutsLive(false, false, false, true, true));
}
```

- [ ] **Step 2: Run to verify it fails** (compile error).

- [ ] **Step 3: Implement**

`ViewportInput.hpp`:

```cpp
    // May an editor shortcut fire this frame? Not in Play, not while ImGui
    // owns the keyboard, not while an Input Actions rebind capture owns it
    // (its completing key would also fire the shortcut: ImGui's
    // WantCaptureKeyboard is false during a capture), and viewport tools
    // only while the Viewport is active.
    inline bool EditorShortcutsLive(bool playMode, bool wantCaptureKeyboard, bool rebindCaptureLive,
                                    bool requireViewportFocus, bool viewportActive) noexcept
    { return !playMode && !wantCaptureKeyboard && !rebindCaptureLive && (!requireViewportFocus || viewportActive); }
```

`EditorApp.hpp`, next to `m_viewportActive`: `bool m_rebindCaptureLive = false;   // an Input Actions rebind capture owns the keyboard this frame; computed at the top of FrameInput, read by ShortcutsLive (every caller is inside FrameInput)`.

`EditorAppFrame.cpp :751-755`:

```cpp
        m_rebindCaptureLive = false;
        m_documents.ForEach([&](EditorDocument& document)
        {
            if (auto* input = dynamic_cast<InputActionsDocument*>(&document))
            {
                input->SetPreviewSnapshot(snap);
                m_rebindCaptureLive = m_rebindCaptureLive || input->InputSwallowed();
            }
        });
```

`ShortcutsLive` becomes `return EditorShortcutsLive(InPlayMode(), snap.wantCaptureKeyboard, m_rebindCaptureLive, requireViewportFocus, m_viewportActive);` with the explanatory comment moved from `:896-903` onto it. In `HandleUndoRedoAndSceneShortcuts` delete the local `captureLive` loop and use `const bool active = ShortcutsLive(snap, false);`.

- [ ] **Step 4: Run** `./ArcaneTests.exe "[editor]"` and build the whole solution (the editor compiles). Expected: pass.

- [ ] **Step 5: Commit**

```bash
git commit -am "fix(editor): every app shortcut (F/Home framing, Alt+G/J, W/E/R/Q, undo/redo, Ctrl scene keys) stands down while an Input Actions rebind capture owns the keyboard -- one gate, EditorShortcutsLive (arc-1 debt I2)"
```

Desk item: arm a Rebind, hover the Viewport, press F, then Alt+G, then W: each binds; the camera, view mode and gizmo tool do not change.

---

### Task 8: A control name containing '+' is not a chord separator (item I4)

**Files:**
- Modify: `ArcaneClient/src/Arcane/Input/InputActions.cpp:296-318` (SplitChordParts + CompilePath comments)
- Modify (comments only): `ArcaneClient/src/Arcane/Input/InputActions.hpp:4,153`, `InputRebindOperation.hpp:38`, `InputRebindOperation.cpp:128`
- Test: `ArcaneTests/src/InputActionsTest.cpp`, `InputRebindOperationTest.cpp`, `InputBindingProfileTest.cpp`, `InputActionsEditorModelTest.cpp`

- [ ] **Step 1: Write the failing tests**

`InputActionsTest.cpp` (add `constexpr uint32_t kScancodeKpPlus = 87;   // SDL_SCANCODE_KP_PLUS` beside `kScancodeW`, and `constexpr uint32_t kScancodeLShift = 225;` if not present):

```cpp
TEST_CASE("input: a control name containing '+' is not a chord separator", "[input]")
{
    using Arcane::InputActions;
    CHECK(InputActions::IsKnownControlPath("<Keyboard>/scancode/keypad +"));
    CHECK(InputActions::IsKnownControlPath("<Keyboard>/scancode/keypad +/-"));
    CHECK(InputActions::IsKnownControlPath("<Keyboard>/scancode/lshift+<Keyboard>/scancode/keypad +"));
    CHECK(InputActions::IsKnownControlPath("<Keyboard>/scancode/keypad <+<Keyboard>/a"));
    CHECK_FALSE(InputActions::IsKnownControlPath("<Keyboard>/a+"));
    CHECK_FALSE(InputActions::IsKnownControlPath("<Keyboard>/a++<Keyboard>/b"));
    auto d = InputActions::DisplayForPath("<Keyboard>/scancode/keypad +");
    CHECK(d.device == "Keyboard"); CHECK(d.control == "Keypad +");
    CHECK(InputActions::DisplayForPath("<Keyboard>/scancode/lshift+<Keyboard>/scancode/keypad +").control == "Left Shift + Keypad +");
    CHECK_FALSE(InputActions::CanonicalControlKey("<Keyboard>/scancode/keypad +").empty());
    CHECK(InputActions::CanonicalControlKey("<Keyboard>/scancode/keypad +") != InputActions::CanonicalControlKey("<Keyboard>/scancode/keypad -"));
}

TEST_CASE("input: a captured Keypad + binding fires in the evaluator", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(nlohmann::json::parse(R"JSON({"actionMaps":[{"name":"demo","actions":[
        {"name":"plus","type":"Button","bindings":[{"path":"<Keyboard>/scancode/keypad +"}]},
        {"name":"shiftPlus","type":"Button","bindings":[{"path":"<Keyboard>/scancode/lshift+<Keyboard>/scancode/keypad +"}]}]}]})JSON")));
    input->SetBaseContext("demo");
    InputSnapshot snap; snap.SetScancode(kScancodeKpPlus);
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Down("plus")); CHECK(input->Pressed("plus"));
    CHECK_FALSE(input->Down("shiftPlus"));
    snap.SetScancode(kScancodeLShift);
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Down("shiftPlus"));
    input->Update(1.0 / 60.0, InputSnapshot{});
    CHECK_FALSE(input->Down("plus"));
}
```

The document shape is `PadAndChordDoc()`'s (`InputActionsTest.cpp:126`), used by `"input: chord requires every part"` (`:186`).

`InputRebindOperationTest.cpp` (add `#include <Arcane/Input/InputActions.hpp>`):

```cpp
TEST_CASE("input profile: capturing Keypad + round-trips to a known, compilable path", "[input][profile]")
{
    Arcane::InputRebindOperation capture;
    capture.Begin(Binding(), Arcane::InputDevice::Kbm, 5.0f, {});
    Arcane::InputSnapshot plus; plus.SetScancode(87);
    capture.Observe(plus, 0.1f);
    REQUIRE(capture.Result().state == Arcane::InputRebindState::Completed);
    CHECK(capture.Result().replacementPath == "<Keyboard>/scancode/keypad +");
    CHECK(Arcane::InputActions::IsKnownControlPath(capture.Result().replacementPath));
}
```

`InputBindingProfileTest.cpp`:

```cpp
TEST_CASE("input profile: a game-side rebind to Keypad + is accepted", "[input][profile]")
{
    const auto asset = ProfileAsset();
    Arcane::InputBindingProfile profile;
    CHECK(profile.SetOverride(kJump, "<Keyboard>/scancode/keypad +", asset));
}
```

`InputActionsEditorModelTest.cpp`:

```cpp
TEST_CASE("input editor: a captured Keypad + path is not an unknown control and conflicts by compiled key", "[editor][input]")
{
    auto json = DocumentJson();
    json["actionMaps"][0]["actions"][0]["bindings"][0]["path"] = "<Keyboard>/scancode/keypad +";
    json["actionMaps"][0]["actions"].push_back(nlohmann::json::parse(R"JSON({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","name":"Crouch","type":"Button",
        "bindings":[{"id":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","path":"<Keyboard>/scancode/keypad +"}]})JSON"));
    Arcane::Editor::InputActionsEditorModel model(std::move(json));
    const auto w = model.Warnings();
    CHECK(std::none_of(w.begin(), w.end(), [](const std::string& s) { return s.starts_with("Unknown control path"); }));
    CHECK(std::count_if(w.begin(), w.end(), [](const std::string& s) { return s.starts_with("Conflicting"); }) == 1);
}
```

- [ ] **Step 2: Run** `./ArcaneTests.exe "[input]"`. Expected: the new cases FAIL (keypad + splits into two parts).

- [ ] **Step 3: Implement** (replace `SplitChordParts` and its comment)

```cpp
        // CompilePath's '+' rule: a '+' separates chord parts only where the
        // next part begins. Every simple path starts with '<Device>', so "+<"
        // is the separator and any other '+' belongs to a control name (SDL's
        // "Keypad +", "Keypad +/-"). The last part is always emitted.
        std::vector<std::string> SplitChordParts(std::string_view path)
        {
            std::vector<std::string> parts;
            std::size_t start = 0;
            for (std::size_t i = 0; i + 1 < path.size(); ++i)
                if (path[i] == '+' && path[i + 1] == '<') { parts.emplace_back(path.substr(start, i - start)); start = i + 1; }
            parts.emplace_back(path.substr(start));
            return parts;
        }
```

Update the `CompilePath` comment: "an empty INTERIOR part (`+<Keyboard>/a`) compiles to a silent {None}; a trailing '+' is now part of the control name (`<Keyboard>/a+` is one unknown control)". Update the four comment sites to say `"+<"` separates parts.

- [ ] **Step 4: Run** `./ArcaneTests.exe "[input]"` and `./ArcaneTests.exe "[editor][input]"`. Expected: pass, including the existing chord cases.

- [ ] **Step 5: Commit**

```bash
git commit -am "fix(input): '+<' is the only chord separator -- a captured Keypad + / Keypad +/- compiles, displays, conflicts and rebinds; '<Keyboard>/a+' is one unknown control (arc-1 debt I4)"
```

---

### Task 9: A mixed-device chord names each part's device (item I3)

**Files:**
- Modify: `ArcaneClient/src/Arcane/Input/InputActions.cpp:1731-1746`, `InputActions.hpp:84-90` (comment)
- Test: `ArcaneTests/src/InputActionsTest.cpp`, `ArcaneTests/src/InputActionsRowsTest.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("input: DisplayForPath names each part's own device in a mixed-device chord", "[input]")
{
    using Arcane::InputActions;
    struct Row { const char* path; const char* device; const char* control; };
    const Row rows[] = {
        { "<Mouse>/leftButton", "Mouse", "Left Button" },
        { "<Keyboard>/lshift+<Keyboard>/a", "Keyboard", "Left Shift + A" },
        { "<Keyboard>/lctrl+<Mouse>/leftButton", "", "Keyboard Left Ctrl + Mouse Left Button" },
        { "<Keyboard>/scancode/lshift+<Mouse>/leftButton", "", "Keyboard Left Shift + Mouse Left Button" },
        { "<Wheel>/up", "Wheel", "Up" },
        { "<Wheel>/up+<Keyboard>/a", "", "Wheel Up + Keyboard A" },
        { "garbage+<Keyboard>/a", "", "garbage+<Keyboard>/a" },   // an unparseable part: the raw text back whole
    };
    for (const auto& r : rows)
    {
        INFO(r.path);
        const auto d = InputActions::DisplayForPath(r.path);
        CHECK(d.device == r.device);
        CHECK(d.control == r.control);
    }
}

TEST_CASE("input: BindingDisplayString of a mixed-device chord names both devices", "[input][native]")
{
    auto asset = Arcane::InputActionAsset::FromJson(NativeActionDoc());
    REQUIRE(asset);
    auto input = InputActions::Create();
    REQUIRE(input->LoadAsset(*asset));
    const auto jumpBinding = *Arcane::Guid::FromString("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    REQUIRE(input->SetBindingPath(jumpBinding, "<Keyboard>/lctrl+<Mouse>/leftButton"));
    CHECK(input->BindingDisplayString(jumpBinding) == "Keyboard Left Ctrl + Mouse Left Button");
}
```

In `InputActionsRowsTest.cpp` add a row check: a draft binding at `"<Keyboard>/lshift+<Mouse>/leftButton"` builds a row whose `device` is `""`.

- [ ] **Step 2: Run** `./ArcaneTests.exe "[input]"`. Expected: the mixed rows FAIL (device `"Keyboard"`).

- [ ] **Step 3: Implement**

```cpp
    InputControlDisplay InputActions::DisplayForPath(std::string_view pathView)
    {
        const std::string path(pathView);
        std::vector<std::pair<std::string, std::string>> parts;   // (device, readable)
        for (const std::string& part : SplitChordParts(path))
        {
            const std::size_t close = part.find(">/");
            if (part.empty() || part.front() != '<' || close == std::string::npos)
                return { {}, path };   // unparseable: hand the raw text back whole
            std::string device = part.substr(1, close - 1);
            std::string name = ReadableControl(device, part.substr(close + 2));
            parts.emplace_back(std::move(device), std::move(name));
        }
        const bool oneDevice = std::all_of(parts.begin(), parts.end(), [&](const auto& p) { return p.first == parts.front().first; });
        InputControlDisplay out;
        if (oneDevice) out.device = parts.front().first;
        for (const auto& [device, name] : parts)
        {
            if (!out.control.empty()) out.control += " + ";
            out.control += oneDevice ? name : device + " " + name;
        }
        return out;
    }
```

Header comment: "A chord whose parts span devices has an empty device; each part names its own device in `control`. An empty device otherwise means a path the compiler cannot parse."

- [ ] **Step 4: Run** `./ArcaneTests.exe "[input]"` and `"[editor][input]"`. Expected: pass.

- [ ] **Step 5: Commit**

```bash
git commit -am "fix(input): DisplayForPath gives a mixed-device chord no single device and names each part's own ('Keyboard Left Ctrl + Mouse Left Button'); the row draws the neutral icon (arc-1 debt I3)"
```

---

### Task 10: Device-less ImGui tests for the page Rebind focus, the popup key guard and the page rename (items H1, H2; B's page half; the Outliner sibling)

**Files:**
- Create: `ArcaneTests/src/InputActionsDocumentUiTest.cpp` (regen premake after creating it)
- Modify: `ArcaneEditor/src/Widgets/PropertyGrid.cpp:226-242` (per-button probe key; test seam only)
- Modify: `ArcaneEditor/src/Documents/InputActionsDocumentWidgets.hpp` (state `probe` seam) / `.cpp:259-260` (maps rows record their centre)
- Modify: `ArcaneEditor/src/Panels/EditorPanels.cpp:1672` (Outliner guard flag)

**Interfaces:**
- Consumes: `InputActionsDocument::State()`, `WindowFocused()`, `InputSwallowed()`, `Page()`; `scrollRowToId` (Task 6); the Task 4 validator.
- Produces: `std::unordered_map<std::string, ImVec2>* InputActionsDocumentState::probe = nullptr;` (TEST SEAM, maps rows keyed by id string); `PropertyGrid::ButtonRow` records `"<label>#<button text>"` per button when `probe` is set.

- [ ] **Step 1: Seams**

`PropertyGrid::ButtonRow`, right after each `SmallButton`: `if (m_state.probe) Probe((std::string(label) + "#" + buttons[i]).c_str());` (keep the trailing `Probe(label)`).
`InputActionsDocumentState`: add `#include <imgui.h>` and `<unordered_map>`, and
`std::unordered_map<std::string, ImVec2>* probe = nullptr;   // TEST SEAM (InputActionsDocumentUiTest): map rows record their centre under their id. Production: nullptr.`
`DrawMaps`: take `const ImVec2 rowTop = ImGui::GetCursorScreenPos();` before `RowWithThumb` at `:259`, and after `rowBottom`: `if (state.probe) (*state.probe)[id.ToString()] = ImVec2(rowTop.x + 40.0f, (rowTop.y + rowBottom.y) * 0.5f);`.
Outliner (`EditorPanels.cpp:1672`): `ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_NoPopupHierarchy)` with the same comment as `InputActionsDocumentWidgets.cpp:128-131`.

- [ ] **Step 2: Write the test file**

```cpp
// Device-less ImGui tests for the Input Actions document (the PropertyGridTest
// harness shape: own context, software font atlas, windows pinned, events
// injected between frames). Covers what only ImGui can observe: the page's
// Rebind focus (final review I1), keys idle under a context menu (I2), the
// page's validated Name row (arc-1 debt B).
#include <catch2/catch_test_macros.hpp>
#include "Documents/InputActionsDocument.hpp"
#include "Documents/InputActionsDocumentWidgets.hpp"
#include "Documents/InputActionsEditorModel.hpp"
#include "Widgets/PropertyGrid.hpp"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace
{
    namespace fs = std::filesystem;
    using Arcane::Guid;
    Guid G(const char* s) { return *Guid::FromString(s); }
    const char* kDoc = R"JSON({
        "version":1,"id":"11111111-1111-4111-8111-111111111111","defaultMap":"22222222-2222-4222-8222-222222222222","controlSchemes":[],
        "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
            {"id":"33333333-3333-4333-8333-333333333333","name":"Jump","type":"Button","bindings":[{"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/space"}]},
            {"id":"66666666-6666-4666-8666-666666666666","name":"Crouch","type":"Button","bindings":[{"id":"77777777-7777-4777-8777-777777777777","path":"<Keyboard>/c"}]}]},
          {"id":"55555555-5555-4555-8555-555555555555","name":"UI","actions":[]}]})JSON";

    struct Ui
    {
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        std::unordered_map<std::string, ImVec2> probe;
        Ui()
        {
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1600.0f, 900.0f);
            io.IniFilename = nullptr;
            unsigned char* px = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        }
        ~Ui() { if (ctx) { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); } }   // DestroyContext(nullptr) would destroy the CURRENT context
        ImVec2 At(const std::string& key) { INFO(key); REQUIRE(probe.count(key) == 1); return probe.at(key); }
        static bool AnyPopup() { return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel); }
    };

    // The document drawn FIRST (as DocumentHost::DrawAll does), then a plain
    // "Inspector" window drawing its page (as DrawInspectorWindows does).
    struct DocUi : Ui
    {
        fs::path path;
        std::unique_ptr<Arcane::Editor::InputActionsDocument> doc;
        Arcane::Editor::PropertyGridState grid;
        DocUi()
        {
            path = fs::temp_directory_path() / ("ui-" + Guid::Generate().ToString() + ".arcinput");
            { std::ofstream out(path); out << kDoc; }
            doc = Arcane::Editor::InputActionsDocument::Open(path);
            grid.probe = &probe;
        }
        ~DocUi() { ImGui::DestroyContext(ctx); ctx = nullptr; ImGui::SetCurrentContext(prev); doc.reset(); fs::remove(path); }
        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            Arcane::Editor::PropertyGrid(grid).CommitOrphans();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Always);
            bool close = false;
            doc->Draw(close);
            ImGui::SetNextWindowPos(ImVec2(820, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(700, 880), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            { Arcane::Editor::PropertyGrid g(grid); if (auto* page = doc->Page()) page->Draw(g); }
            ImGui::End();
            ImGui::Render();
        }
        void Move(ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); Frame(); }
        void Button(int b, bool down) { ImGui::GetIO().AddMouseButtonEvent(b, down); Frame(); }
        void Key(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frame(); ImGui::GetIO().AddKeyEvent(k, false); Frame(); }
        void Type(const char* s) { ImGui::GetIO().AddInputCharactersUTF8(s); Frame(); }
    };
}

TEST_CASE("input document: the Inspector page's Rebind... takes focus, so the capture survives the frames after the click; Escape still cancels (final review I1)", "[editor][input][inspector]")
{
    DocUi ui;
    REQUIRE(ui.doc);
    REQUIRE(ui.doc->SelectByPath("Player/Jump/0"));
    ui.Frame(); ui.Frame();
    const ImVec2 rebind = ui.At("#Rebind...");   // ButtonRow's "" label + "#" + the button text
    CHECK_FALSE(ui.doc->InputSwallowed());
    ui.Move(rebind);
    ui.Button(0, true);                          // press: the Inspector takes focus
    ui.Button(0, false);                         // release: the button fires AFTER the document drew
    CHECK(ui.doc->InputSwallowed());
    CHECK(ui.doc->State().scrollRowToId == G("44444444-4444-4444-8444-444444444444"));   // set during the Inspector's draw, consumed next frame
    ui.Frame(); ui.Frame();
    CHECK(ui.doc->WindowFocused());              // SetNextWindowFocus pulled the document forward
    CHECK(ui.doc->InputSwallowed());             // pre-fix the capture cancelled on the first frame
    CHECK_FALSE(ui.doc->State().scrollRowToId.IsValid());
    ui.Key(ImGuiKey_Escape);
    CHECK_FALSE(ui.doc->InputSwallowed());
}

TEST_CASE("input document: the Inspector page's Name row keeps a refused duplicate on Enter with no edit; a unique name commits", "[editor][input][inspector]")
{
    DocUi ui;
    REQUIRE(ui.doc);
    REQUIRE(ui.doc->SelectByPath("Player/Crouch"));
    ui.Frame(); ui.Frame();
    ui.Move(ui.At("Name")); ui.Button(0, true); ui.Button(0, false);
    ui.Type("Jump");
    ui.Key(ImGuiKey_Enter);
    ui.Frame(); ui.Frame();
    const auto* crouch = ui.doc->Model().FindNode(G("66666666-6666-4666-8666-666666666666"));
    REQUIRE(crouch);
    CHECK((*crouch)["name"] == "Crouch");                        // no edit, no undo entry
    bool kept = false;
    for (const auto& [id, d] : ui.grid.textDrafts) kept = kept || d.text == "Jump";
    CHECK(kept);
    ui.Type("Duck");
    ui.Move(ImVec2(1200, 850)); ui.Button(0, true); ui.Button(0, false);
    ui.Frame();
    CHECK((*ui.doc->Model().FindNode(G("66666666-6666-4666-8666-666666666666")))["name"] == "Duck");
}

namespace
{
    // The widgets drawn directly with a test-owned model (no document, file or
    // Diagnostics). Action rows are located through the glow service (DrawRow
    // calls it for EVERY row while RowWithThumb's Selectable is the last item);
    // map rows through the state probe seam.
    struct KeysUi : Ui
    {
        Arcane::Editor::InputActionsEditorModel model{ nlohmann::json::parse(kDoc) };
        Arcane::Editor::InputActionsDocumentState state;
        Arcane::Editor::InputActionsDocumentWidgets widgets;
        Arcane::Editor::InputActionsDocumentWidgets::Services services;
        KeysUi()
        {
            state.probe = &probe;
            services.glow = [this](const Guid& id) {
                const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
                probe[id.ToString()] = ImVec2(lo.x + 40.0f, (lo.y + hi.y) * 0.5f);
                return 0.0f; };
        }
        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_Always);
            ImGui::Begin("Input##keys");
            widgets.Draw(model, state, services);
            ImGui::End();
            ImGui::Render();
        }
        void Click(ImVec2 at, int b = 0)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(at.x, at.y); Frame();
            io.AddMouseButtonEvent(b, true); Frame();
            io.AddMouseButtonEvent(b, false); Frame();
        }
        void Key(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frame(); ImGui::GetIO().AddKeyEvent(k, false); Frame(); }
    };
    const char* kPlayer = "22222222-2222-4222-8222-222222222222";
    const char* kUi = "55555555-5555-4555-8555-555555555555";
}

TEST_CASE("input document: Delete under a map row's context menu never deletes the SELECTED map (4e9796ba, I2)", "[editor][input]")
{
    KeysUi ui;
    ui.model.SelectMap(G(kPlayer));
    ui.Frame();
    ui.Click(ui.At(kPlayer));
    ui.Click(ui.At(kUi), 1);                                  // right-click UI: its menu opens, selection stays
    REQUIRE(Ui::AnyPopup());
    REQUIRE(ui.model.SelectedMap() == G(kPlayer));
    ui.Key(ImGuiKey_Delete);
    CHECK(ui.model.Draft()["actionMaps"].size() == 2);         // pre-fix: Player deleted
    CHECK(ui.model.SelectedMap() == G(kPlayer));
    // Positive control: close the menu with a click on the row (Escape does not
    // close popups here: no NavEnableKeyboard), select Player, Delete removes it.
    ui.Click(ui.At(kPlayer));
    ui.Click(ui.At(kPlayer));
    REQUIRE_FALSE(Ui::AnyPopup());
    ui.Key(ImGuiKey_Delete);
    CHECK(ui.model.Draft()["actionMaps"].size() == 1);
    CHECK(ui.model.Draft()["actionMaps"][0]["id"] == kUi);
}

TEST_CASE("input document: Delete under an action row's context menu never deletes the SELECTED action", "[editor][input]")
{
    KeysUi ui;
    const char* kJump = "33333333-3333-4333-8333-333333333333";
    const char* kCrouch = "66666666-6666-4666-8666-666666666666";
    ui.model.SelectMap(G(kPlayer)); ui.model.SelectAction(G(kJump));
    ui.Frame();
    ui.Click(ui.At(kJump));
    ui.Click(ui.At(kCrouch), 1);
    REQUIRE(Ui::AnyPopup());
    REQUIRE(ui.model.SelectedAction() == G(kJump));
    ui.Key(ImGuiKey_Delete);
    CHECK(ui.model.Draft()["actionMaps"][0]["actions"].size() == 2);
    ui.Click(ui.At(kJump));
    ui.Click(ui.At(kJump));
    REQUIRE_FALSE(Ui::AnyPopup());
    ui.Key(ImGuiKey_Delete);
    CHECK(ui.model.Draft()["actionMaps"][0]["actions"].size() == 1);
    CHECK(ui.model.Draft()["actionMaps"][0]["actions"][0]["id"] == kCrouch);
}
```

- [ ] **Step 3: Regenerate projects, build, run**

`./ArcaneTests.exe "[editor][input]"`. Expected: pass. Then prove the key tests are real: temporarily drop `| ImGuiFocusedFlags_NoPopupHierarchy` at `InputActionsDocumentWidgets.cpp:132`, rebuild, confirm both Delete cases FAIL, restore the flag. Record both runs in the task report. If a harness frame-timing assumption fails (e.g. the popup needs one more frame to take focus), add a `Frame()` and say so in the report; never weaken a `CHECK` to make it pass.

- [ ] **Step 4: Commit**

```bash
git add ArcaneTests/src/InputActionsDocumentUiTest.cpp
git commit -am "test(editor): device-less ImGui cases for the input document -- the page's Rebind keeps the capture (I1), Delete under a context menu spares the selected map/action (I2), the page's Name row keeps a refused duplicate; ButtonRow per-button probe + maps probe seams; the Outliner's key guard ignores popups too (arc-1 debt H1, H2, B)"
```

---

### Task 11: An unresolvable --select-in-document is pinned at host and document level (item H4)

**Files:**
- Modify: `ArcaneTests/src/EditorWitnessTest.cpp` (add `<fstream>`, `<iterator>`, `<string>`; new case after E3)
- Modify: `ArcaneTests/src/InputActionsEditorModelTest.cpp`

- [ ] **Step 1: Write the tests**

Document level (headless):

```cpp
TEST_CASE("input document: a refused SelectByPath leaves the opening selection and epoch untouched", "[editor][input][inspector]")
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / ("select-refused-" + Arcane::Guid::Generate().ToString() + ".arcinput");
    { std::ofstream out(path); out << DocumentJson().dump(2); }
    {
        auto doc = Arcane::Editor::InputActionsDocument::Open(path);
        REQUIRE(doc);
        const std::string key = doc->SelectionKey();   // opening selection: first map + first action
        const auto epoch = doc->SelectionEpoch();
        Arcane::Editor::EditorDocument& base = *doc;    // the host's virtual dispatch
        CHECK_FALSE(base.SelectByPath("Player/NoSuchAction"));
        CHECK_FALSE(base.SelectByPath("Player/Jump/7"));
        CHECK(doc->SelectionKey() == key);
        CHECK(doc->SelectionEpoch() == epoch);
    }
    fs::remove(path);
}
```

Host level (`[witness][gpu]`, after E3):

```cpp
TEST_CASE("E3b: an unresolvable --select-in-document is a loud ERROR, the run completes, and the Inspector keeps the document's opening selection", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e3b-input-doc-unresolved");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneEditor.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "dx12", "--frames", "60",
                 "--report", inv.reportPath.generic_string(),
                 "--open-asset", "97260310-8b35-4b29-b12f-1fd6f8e99071",
                 "--select-in-document", "Player/NoSuchAction" };   // no --compare: no golden slot for this state
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);                                  // the run still completes (plan Review Focus 5)
    CHECK(run.report.at("exitReason") == "frames-complete");
    REQUIRE(run.report.contains("inspector"));
    CHECK(run.report["inspector"].at("source") == "Player.arcinput");
    CHECK(run.report["inspector"].at("breadcrumb") == "Player.arcinput > Player > Move");   // the OPENING selection (Player.arcinput's first map + action)
    CHECK_FALSE(run.report.contains("compare"));
    std::ifstream err(run.stderrPath);
    const std::string all((std::istreambuf_iterator<char>(err)), std::istreambuf_iterator<char>());
    CHECK(all.find("--select-in-document 'Player/NoSuchAction': the opened document has no such path") != std::string::npos);
}
```

- [ ] **Step 2: Build the whole solution, run**

`./ArcaneTests.exe "[editor][input]"` then `./ArcaneTests.exe "E3*"` (spawns the staged Debug editor; the ReferenceProject slot must hold the Debug module: `grep -a -o MSVCP140D ReferenceProject/Binaries/ReferenceGame.dll`). Expected: E3 and E3b pass.

- [ ] **Step 3: Commit**

```bash
git commit -am "test(editor): E3b witness -- an unresolvable --select-in-document logs its ERROR, exits 0 and keeps the document's opening selection; the refused SelectByPath is pinned headless too (arc-1 debt H4)"
```

---

### Task 12: Spec wording and the ClearAllFn ruling (items G, F)

**Files:**
- Modify: `docs/superpowers/specs/2026-09-28-inspector-ownership-design.md:141`
- Modify: the same spec's owed / out-of-scope section (find it with `grep -n "^## " <spec>`; if there is none, append a `## Owed after the arc (2026-09-29)` section at the end)

- [ ] **Step 1: Spec A line 141.** Replace `field \`inspector.source\` = the document path` so the bullet reads:

  field `inspector.source` = the Inspector source's `SourceName()` (for an input document its filename, e.g. `"Player.arcinput"` -- not a path; the scene reports `"Scene"`), and the breadcrumb text (e.g. `"Player.arcinput > Player > Jump"`) in the report.

- [ ] **Step 2: Record the F ruling and the owed item** in the owed section:

  - **`[EditorInspector][Instances]` has no ClearAllFn -- closed, not a defect (2026-09-29).** `ClearAllFn` runs only from `ImGui::ClearIniSettings`, which the editor never calls; no editor ini handler sets it; the instance list is layout by design (`EditorInspectorHostTest.cpp:199`). The headless path loads the ini once, when the list is already `{0}`.
  - **OWED: a windowed project switch never loads the incoming project's layout.** `RetargetLayoutIni` saves the outgoing layout and repoints `io.IniFilename`, but ImGui reads the ini only while `!SettingsLoaded`, so the outgoing project's docking, windows, panel visibility, camera, play mode, Material-panel preference and Inspector instances stay live and then overwrite the incoming project's file. Fixing it means `ClearIniSettings` + `LoadIniSettingsFromDisk` after the flush, with every editor handler (PlayMode, Viewport, Panels, Inspector, ShaderEditorDocument's layout) resetting to defaults in a ReadInit hook, and a desk check of ImGui's mid-session dock re-application. Its own small arc.

- [ ] **Step 3: Commit**

```bash
git commit -am "docs(editor): spec A s5 names inspector.source as the source's SourceName (a document's filename); the ClearAllFn debt closed as not-a-defect and the real bug (a windowed project switch never loads the incoming layout) recorded as owed (arc-1 debt G, F)"
```

---

### Task 13: Final gate

- [ ] **Step 1:** Build `Arcane.slnx` Debug and Release (see Global Constraints; Release needs the ReferenceProject slot rebuilt Release and restaged into `bin/Release-windows-x86_64-md/{ArcaneServer,ArcaneRuntime,ArcaneEditor}/ReferenceProject/Binaries/`, then back to Debug afterwards, exactly as `scripts/golden-gate.ps1` does).
- [ ] **Step 2:** Debug `./ArcaneTests.exe "~[gpu]"`: 0 failed. Record cases/passed/skipped against the 2026-09-29 Release baseline (2147 / 2143 / 4) plus this plan's new cases.
- [ ] **Step 3:** Debug `./ArcaneTests.exe "[gpu]~[witness]"` and `./ArcaneTests.exe "[witness][gpu]"` (G1 skips without `ARCANE_DIAG_DESK`): 0 failed.
- [ ] **Step 4:** Release `./ArcaneTests.exe "~[gpu]"`: 0 failed.
- [ ] **Step 5:** `powershell -File scripts/golden-gate.ps1 -Configuration Debug`: all lanes pass (the editor-input-doc golden must be unchanged; nothing here changes what that selection draws). Delete the exe-dir `imgui.ini` first if a lane fails on layout.
- [ ] **Step 6:** Hand the user the desk list (Tasks 4, 5, 6, 7, 10's Outliner flag) and the integration choice. Do not merge or push.

---

## Self-Review

- **Coverage:** A (T2), B (T4 + T10), C (T5), D (T3), E (T1), F (T12, ruled), G (T12), H1/H2 (T10), H3 (T2), H4 (T11), I1 (T6), I2 (T7), I3 (T9), I4 (T8). The Outliner sibling (T10). Every item from the direction record's step 2 has a task.
- **Type consistency:** `scrollRowToId` (T6) is read by T10; `SnapshotForCapture`'s 3-argument form (T5) is the only form after T5; `TrimName`/`ResolveKey`/`EncodeSelectionKey` (T1) are not renamed later; `BindingConflict` fields are appended after `group` (T3); `TextRow`'s 5th parameter (T4) is used by T4's page change and exercised by T10.
- **Known plan-code risk:** T4's and T10's frame counts are the harness's best reading of ImGui 1.92.9 timing (focus requests resolve through nav a frame or two late; popups take focus on the frame after they open); T8's evaluator case assumes `LoadJson` accepts `PadAndChordDoc()`'s shape. Implementers adjust these with a note, never by weakening an assertion.
