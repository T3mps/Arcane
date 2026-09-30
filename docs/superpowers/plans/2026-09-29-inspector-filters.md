# Inspector Filters + Asset Page (Editor Mini-Arc 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every Inspector instance carries a checkbox filter over source kinds. The Asset Browser becomes an Inspector source, and its in-panel preview pane is removed. The default layout gets two Inspectors: the main one shows everything but Assets, and "Inspector 2" shows only Assets. In the same phase, the shader editor's "Material" window retires into a material page in the Inspector, and the Sprite and Mesh documents' property forms move into Inspector pages too.

**Architecture:**
- `InspectorHost` stays pure (no ImGui). It gains:
  - a source-kind catalog;
  - a per-instance exclusion filter;
  - selection stamps for routing;
  - filtered history navigation;
  - per-instance pin;
  - permanent sources.
- `InspectorWindows` draws the filter dropdown and persists `Filters=` in the existing `[EditorInspector][Instances]` ini section. It also detects layouts saved before this feature (no `Filters=` line) so the app can upgrade them once.
- A new `AssetInspectorSource` wraps `AssetPanelModel`'s shared selection. Its page is the old preview pane's content plus the texture import settings that used to sit in the scene page's fallback branch.
- The app wires the source, drains the page's actions, and builds and upgrades the default layout.

**Tech Stack:** C++23, Dear ImGui 1.92.9 docking (DockBuilder, settings handlers with `ReadInitFn`/`ApplyAllFn`), Catch2, premake5, the golden gate (`scripts/golden-gate.ps1`).

**Spec:** `docs/superpowers/specs/2026-09-29-inspector-filters-design.md` (amends `docs/superpowers/specs/2026-09-28-inspector-ownership-design.md` s3.3 and s3.5). Read both before any task.

## Global Constraints

- Kind catalog, in this exact order (id -> display name): `"scene"` Scene, `"assets"` Assets, `"input-actions"` Input Actions, `"material"` Materials, `"sprite"` Sprites, `"mesh"` Meshes. There is no `"shader"` id: the shader editor's documents are kind `"material"`.
- One selection rule for EVERY source (spec s3): opening a document selects its page, a selection gesture inside a source (including a click in a document's content, and a re-click of the already-selected asset) is an event, and tab/focus/window activation never is. No source gets a focus-follow exception. The asset source's epoch is `AssetPanelModel::selectionGesture` (bumped on every `Select`, monotonic), never the change-only `selectionStamp`. No call site may call `Select` every frame (Task 6 gates the Browser row context menu to its first frame). The one scoped exception: the Graph lens mirrors its persistent canvas selection only on change (`AssetGraphPanel.cpp:2152`), so re-clicking the already-selected Graph node is not an event.
- The one empty-state string for a routed source with nothing selected is "No selection" (the scene body's existing literal, `EditorPanels.cpp:2410`; spec s3, decision 5).
- Filters persist the UNTICKED kinds (exclusions). Empty exclusions = All. A kind added to the catalog later is admitted by every existing filter.
- At least one catalog kind stays ticked. A filter that excludes every catalog kind is refused, and on ini load it is sanitized to All.
- A source whose `Kind()` is empty is admitted only by All.
- Routing order: pinned -> `Current()` if admitted -> highest selection stamp among admitted sources -> fallback (scene) if admitted -> most recently added admitted source -> null.
- A clear (empty key) is never a selection event. It moves no stamp and never moves `Current()`.
- ini line format: `Filters=<id>:<kind>+<kind>,<id>:<kind>`, written AFTER `Ids=`, always written (possibly empty: `Filters=`). A section with no `Filters=` line means the layout predates this feature, and the layout is upgraded ONCE.
- Window titles:
  - instance 0: `"Inspector###Inspector"`, or `"Inspector - <label>###Inspector"` when filtered;
  - instance N >= 1: `"Inspector <N+1>###inspector_<N>"` or `"Inspector <N+1> - <label>###inspector_<N>"`.
  - Instance 0 KEEPS its ImGui id. In the vendored 1.92.9, `ImHashStr` resets its crc at `###` AND skips the three `#` characters (`imgui.cpp:2539-2544`), so `ImHashStr("X###Inspector") == ImHashStr("Inspector")`: the titled window has the legacy bare-`"Inspector"` id, and the old `[Window][Inspector]` ini entry (its DockId included) carries over unchanged. `CreateNewWindowSettings` names a new entry by the text after `###` (`imgui.cpp:16416`), so the saved ini keys are `[Window][Inspector]` and `[Window][inspector_<N>]`, never the full title. No re-dock is needed. Lookups of the primary window by name (`FindWindowByName`, `DockBuilderDockWindow`) go through one constant, `kPrimaryInspectorWindowId = "###Inspector"`, for clarity (same id).
- Default layout = exactly instances {0, 1}: 0 = All but Assets (`excluded = {"assets"}`), 1 = Assets only (`excluded = {"scene","input-actions","material","sprite","mesh"}`), docked in a split to the RIGHT of the Asset Browser's node.
- Every new `Panels/*.cpp` the tests need must be added to the EXPLICIT `ArcaneTests` file list in `premake5.lua` (near line 1237/1552); the editor project globs, the test project does not. Re-run `generate.bat` (or `ThirdParty\premake5\premake5.exe vs2026`) after any file-list change.
- Build: `msbuild Arcane.slnx /p:Configuration=Debug /p:Platform=x64 -m -nr:false`. Tests: run `bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[inspector]"` FROM ITS OWN DIRECTORY. The final suite gate is `~[gpu]` plus `[gpu]~[witness]` plus the witnesses.
- Never push. Commit per task on branch `feat/inspector-filters` (already created off main `6f8a05e3`).
- Plugin ABI: Task 10 bumps `kGamePluginABIVersion` 45 -> 46 unconditionally and restamps `ReferenceProject.arcproj`; the Aphelyon restamp is owed (report it, do not do it).

## Review Focus

1. **The last admitted document closes under a filtered instance.** Example: an "Input Actions only" instance, and the only `.arcinput` closes. Expect the one-line "No Input Actions document open", no dangling pointer, and no crash. Pinned test in Task 2 ("closing the last admitted source").
2. **Project switch with filtered instances.**
   - Filters survive.
   - The asset source stays registered (permanent). Its history entries and pins drop.
   - The Assets instance shows "No selection" because the model's selection is reset.
   - Pinned test in Task 2 ("ReleaseAll keeps a permanent source").
3. **A hand-edited or garbage `Filters=` line.** Cases: an id not in `Ids=`, unknown kinds, all kinds excluded, trailing commas, a missing colon. Expect it sanitized, never a throw, never an all-excluded instance. An instance absent from the line reads as All, even on a second load without `ClearIniSettings` between (spec s7). Pinned tests in Task 4.
4. **Changing the filter of a PINNED instance.** Expect the pin to win (the page stays) and the dropdown to stay usable. After unpinning, the instance follows the new filter. Pinned test in Task 2.
5. **The selected asset is deleted or renamed on disk.** `Resolves` becomes false, its history entries are pruned, and the Assets inspector shows "No selection", never a stale page or a crash. Pinned test in Task 5.
6. **Asset re-click and the row context menu.** Re-clicking the selected asset in the Browser re-routes it (spec s3; pinned test in Task 5). Holding the Browser row context menu open must NOT re-fire the asset edge every frame (Task 6's first-frame gate). Re-clicking the selected Graph node is not an event (the scoped exception).

---

### Task 1: The kind catalog and `InspectorFilter` (pure)

**Files:**
- Create: `ArcaneEditor/src/Panels/InspectorKinds.hpp`
- Create: `ArcaneEditor/src/Panels/InspectorKinds.cpp`
- Create: `ArcaneTests/src/EditorInspectorKindsTest.cpp`
- Modify: `premake5.lua` (the ArcaneTests explicit list, beside `InspectorHost.cpp` at ~line 1237)

**Interfaces:**
- Produces: `InspectorKind`, `kInspectorKinds`, `FindInspectorKind`, `InspectorFilter` (`excluded`, `Admits`, `IsAll`, `Sanitized`, `AllBut`, `Only`), `InspectorFilterLabel`. Every later task uses these names.

- [ ] **Step 1: Write the failing test** (`ArcaneTests/src/EditorInspectorKindsTest.cpp`)

```cpp
// Inspector filters (spec 2026-09-29 s2/s3/s5): the kind catalog, the
// exclusion filter and its label -- pure, no ImGui.
#include <catch2/catch_test_macros.hpp>
#include <Panels/InspectorKinds.hpp>

using namespace Arcane::Editor;

TEST_CASE("InspectorKinds: the catalog order is scene, assets, input-actions, material, sprite, mesh", "[editor][inspector]")
{
    REQUIRE(kInspectorKinds.size() == 6);
    CHECK(kInspectorKinds[0].id == "scene");
    CHECK(kInspectorKinds[1].id == "assets");
    CHECK(kInspectorKinds[2].id == "input-actions");
    CHECK(kInspectorKinds[3].id == "material");
    CHECK(kInspectorKinds[4].id == "sprite");
    CHECK(kInspectorKinds[5].id == "mesh");
    CHECK(FindInspectorKind("assets")->displayName == "Assets");
    CHECK(FindInspectorKind("material")->displayName == "Materials");
    CHECK(FindInspectorKind("shader") == nullptr);
}

TEST_CASE("InspectorFilter: exclusions admit everything else, and an empty kind only under All", "[editor][inspector]")
{
    InspectorFilter all;
    CHECK(all.IsAll());
    CHECK(all.Admits("scene"));
    CHECK(all.Admits(""));                         // a document with nothing to edit: All only
    CHECK(all.Admits("future-kind"));              // a kind this build does not know: admitted

    const InspectorFilter noAssets = InspectorFilter::AllBut("assets");
    CHECK_FALSE(noAssets.IsAll());
    CHECK(noAssets.Admits("scene"));
    CHECK(noAssets.Admits("material"));
    CHECK_FALSE(noAssets.Admits("assets"));
    CHECK_FALSE(noAssets.Admits(""));              // filtered: empty kinds are out
    CHECK(noAssets.Admits("future-kind"));         // a later kind appears (decision 8.3)

    const InspectorFilter onlyAssets = InspectorFilter::Only("assets");
    CHECK(onlyAssets.excluded == std::vector<std::string>{ "scene", "input-actions", "material", "sprite", "mesh" });
    CHECK(onlyAssets.Admits("assets"));
    CHECK_FALSE(onlyAssets.Admits("scene"));
    CHECK_FALSE(onlyAssets.Admits("material"));
}

TEST_CASE("InspectorFilter: Sanitized drops unknown kinds, duplicates, and an all-excluded set", "[editor][inspector]")
{
    InspectorFilter f;
    f.excluded = { "assets", "bogus", "assets", "scene" };
    CHECK(f.Sanitized().excluded == std::vector<std::string>{ "scene", "assets" });   // catalog order
    f.excluded = { "mesh", "scene", "sprite", "input-actions", "material", "assets" };
    CHECK(f.ExcludesEveryKind());
    CHECK(f.Sanitized().IsAll());                  // nothing left to show: All, never empty
}

TEST_CASE("InspectorFilterLabel: All, one name, All but X, or the ticked list", "[editor][inspector]")
{
    CHECK(InspectorFilterLabel(InspectorFilter{}) == "All");
    CHECK(InspectorFilterLabel(InspectorFilter::Only("scene")) == "Scene");
    CHECK(InspectorFilterLabel(InspectorFilter::AllBut("assets")) == "All but Assets");
    InspectorFilter two;
    two.excluded = { "input-actions", "material", "sprite", "mesh" };   // Scene + Assets ticked
    CHECK(InspectorFilterLabel(two) == "Scene, Assets");
    InspectorFilter four;
    four.excluded = { "scene", "assets" };
    CHECK(InspectorFilterLabel(four) == "Input Actions, Materials, Sprites, Meshes");
}
```

- [ ] **Step 2: Add the header and a stub .cpp, register the files, and run the test to verify it fails**

Add to `premake5.lua`'s ArcaneTests list, directly after the `InspectorHost.cpp` line:
```lua
        "%{wks.location}/ArcaneEditor/src/Panels/InspectorKinds.cpp",
```
Run `generate.bat`, build, then run `ArcaneTests.exe "[inspector]"` from its directory.
Expected: compile failure (header missing) or FAIL.

- [ ] **Step 3: Write the implementation**

`ArcaneEditor/src/Panels/InspectorKinds.hpp`:
```cpp
#pragma once

// Inspector filters (spec 2026-09-29 s2/s3): the fixed CATALOG of source
// kinds an Inspector instance can filter on, and the per-instance filter
// itself. The filter stores the UNTICKED kinds, so a kind added to the
// catalog later is admitted by every existing filter (decision 8.3: the tool
// stays honest -- a filter never silently narrows when the catalog grows).
// Pure: no ImGui (InspectorHost and the tests use it).

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    struct InspectorKind
    {
        std::string_view id;            // InspectorSource::Kind(), persisted in imgui.ini
        std::string_view displayName;   // the dropdown row and the label
    };

    // Catalog order = dropdown order = Sanitized() order = the Filters= write order.
    inline constexpr std::array<InspectorKind, 6> kInspectorKinds{ {
        { "scene",         "Scene" },
        { "assets",        "Assets" },
        { "input-actions", "Input Actions" },
        { "material",      "Materials" },       // ShaderEditorDocument
        { "sprite",        "Sprites" },
        { "mesh",          "Meshes" },
    } };

    [[nodiscard]] const InspectorKind* FindInspectorKind(std::string_view id);

    struct InspectorFilter
    {
        std::vector<std::string> excluded;   // catalog ids; empty = All

        [[nodiscard]] bool IsAll() const noexcept { return excluded.empty(); }
        // An empty kind (a document that never selects) is admitted only by All.
        [[nodiscard]] bool Admits(std::string_view kind) const;
        // Catalog kinds only, catalog order, no duplicates; an all-excluded
        // set becomes All (an instance that can show nothing reads as broken).
        [[nodiscard]] InspectorFilter Sanitized() const;
        [[nodiscard]] bool ExcludesEveryKind() const;   // true = would be refused

        static InspectorFilter AllBut(std::string_view kind);
        static InspectorFilter Only(std::string_view kind);

        friend bool operator==(const InspectorFilter&, const InspectorFilter&) = default;
    };

    // "All" / "<name>" (one ticked) / "All but <name>" (one unticked) /
    // "<name>, <name>" (the ticked names, catalog order).
    [[nodiscard]] std::string InspectorFilterLabel(const InspectorFilter& filter);
}
```

`ArcaneEditor/src/Panels/InspectorKinds.cpp`:
```cpp
#include "Panels/InspectorKinds.hpp"

#include <algorithm>

namespace Arcane::Editor
{
    const InspectorKind* FindInspectorKind(std::string_view id)
    {
        for (const InspectorKind& k : kInspectorKinds)
            if (k.id == id) return &k;
        return nullptr;
    }

    bool InspectorFilter::Admits(std::string_view kind) const
    {
        if (IsAll()) return true;
        if (kind.empty()) return false;
        return std::find(excluded.begin(), excluded.end(), kind) == excluded.end();
    }

    bool InspectorFilter::ExcludesEveryKind() const
    {
        return std::all_of(kInspectorKinds.begin(), kInspectorKinds.end(), [&](const InspectorKind& k)
        { return std::find(excluded.begin(), excluded.end(), k.id) != excluded.end(); });
    }

    InspectorFilter InspectorFilter::Sanitized() const
    {
        InspectorFilter out;
        for (const InspectorKind& k : kInspectorKinds)
            if (std::find(excluded.begin(), excluded.end(), k.id) != excluded.end())
                out.excluded.emplace_back(k.id);
        if (out.ExcludesEveryKind()) out.excluded.clear();
        return out;
    }

    InspectorFilter InspectorFilter::AllBut(std::string_view kind)
    {
        InspectorFilter f;
        f.excluded.emplace_back(kind);
        return f.Sanitized();
    }

    InspectorFilter InspectorFilter::Only(std::string_view kind)
    {
        InspectorFilter f;
        for (const InspectorKind& k : kInspectorKinds)
            if (k.id != kind) f.excluded.emplace_back(k.id);
        return f.Sanitized();
    }

    std::string InspectorFilterLabel(const InspectorFilter& filter)
    {
        if (filter.IsAll()) return "All";
        std::vector<std::string_view> ticked, unticked;
        for (const InspectorKind& k : kInspectorKinds)
            (filter.Admits(k.id) ? ticked : unticked).push_back(k.displayName);
        if (ticked.size() == 1) return std::string(ticked[0]);
        if (unticked.size() == 1) return "All but " + std::string(unticked[0]);
        std::string out;
        for (const std::string_view n : ticked) { if (!out.empty()) out += ", "; out += n; }
        return out;
    }
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `ArcaneTests.exe "[inspector]"` (from its directory).
Expected: all `[inspector]` cases PASS, including the pre-existing host cases.

- [ ] **Step 5: Commit**

```bash
git add ArcaneEditor/src/Panels/InspectorKinds.hpp ArcaneEditor/src/Panels/InspectorKinds.cpp ArcaneTests/src/EditorInspectorKindsTest.cpp premake5.lua
git commit -m "feat(editor): Inspector kind catalog + exclusion filter + label (inspector filters s2/s3)"
```

---

### Task 2: `Kind()` on sources, filtered routing, per-instance pin, permanent sources

**Files:**
- Modify: `ArcaneEditor/src/Panels/InspectorSource.hpp` (add `Kind()`)
- Modify: `ArcaneEditor/src/Panels/InspectorHost.hpp`, `ArcaneEditor/src/Panels/InspectorHost.cpp`
- Modify: `ArcaneEditor/src/Panels/SceneInspectorSource.hpp` (`Kind()` -> `"scene"`)
- Modify: `ArcaneEditor/src/Documents/EditorDocument.hpp` (default `Kind()` -> `""`)
- Modify: `ArcaneEditor/src/Documents/InputActionsDocument.hpp` (`Kind()` -> `"input-actions"`)
- Modify: `ArcaneEditor/src/Panels/InspectorWindows.cpp` (callers of `CanPin()` -> `CanPin(inst.id)`)
- Test: `ArcaneTests/src/EditorInspectorHostTest.cpp`

**Interfaces:**
- Consumes: `InspectorFilter` (Task 1).
- Produces:
  - `virtual std::string_view InspectorSource::Kind() const = 0;`
  - `InspectorHost::Instance::filter` (`InspectorFilter`).
  - `bool InspectorHost::SetFilter(int id, InspectorFilter f)`. Returns false and leaves the filter unchanged for an unknown id or when `f.ExcludesEveryKind()`. Otherwise it stores `f.Sanitized()`.
  - `void InspectorHost::AddSource(InspectorSource&, bool permanent = false)`.
  - `bool InspectorHost::CanPin(int id)`.
  - `SetPinned(int id, bool)`, which now pins `SourceFor(id)`.
  - `SourceFor(int)` implementing the Global Constraints routing order.

- [ ] **Step 1: Give the test fakes a kind and write the failing tests**

In `EditorInspectorHostTest.cpp`:
- **Fakes.** Give `FakeSource` a `std::string kind;` member and a constructor `FakeSource(std::string n, std::string k = "scene")`, and override `std::string_view Kind() const override { return kind; }`.
- **World.** Change `World` to `FakeSource scene{ "Scene", "scene" }, doc{ "Player.arcinput", "input-actions" }, other{ "brick.png", "assets" }, doc2{ "Menu.arcinput", "input-actions" };` and add `host.AddSource(doc2);` to the constructor.
- **Existing tests.** Every existing test must still pass unchanged. It uses All instances, and All routing is `Current()`.

Append:

```cpp
TEST_CASE("InspectorHost filters: a Scene instance keeps the scene while a document selects", "[editor][inspector]")
{
    World w;
    const int scene = w.host.AddInstance();                        // 1
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("scene")));
    w.Select(w.scene, "A");
    w.Select(w.scene, "B");
    w.Select(w.doc, "Player/Jump");
    CHECK(w.host.SourceFor(0) == &w.doc);                          // All follows the binding
    CHECK(w.host.SourceFor(scene) == &w.scene);                    // Scene keeps B
    CHECK(w.scene.key == "B");
}

TEST_CASE("InspectorHost filters: a clear in one source never moves an instance routed elsewhere", "[editor][inspector]")
{
    World w;
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    w.Select(w.doc, "Player/Jump");
    w.Select(w.scene, "7");
    w.Select(w.scene, "");                                         // empty-space click: a clear
    CHECK(w.host.SourceFor(input) == &w.doc);
    CHECK(w.doc.key == "Player/Jump");
    // An All instance stays on the scene (a clear is not an event) and a
    // document clear does not move it either.
    CHECK(w.host.SourceFor(0) == &w.scene);
    w.Select(w.doc, "");
    CHECK(w.host.SourceFor(0) == &w.scene);
}

TEST_CASE("InspectorHost filters: Current wins when admitted, else the latest admitted stamp", "[editor][inspector]")
{
    World w;
    const int noAssets = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(noAssets, InspectorFilter::AllBut("assets")));
    w.Select(w.doc, "a");
    w.Select(w.doc2, "b");
    w.Select(w.other, "brick");                                    // an asset click
    CHECK(w.host.SourceFor(0) == &w.other);
    CHECK(w.host.SourceFor(noAssets) == &w.doc2);                  // the latest admitted, never the asset
    w.host.RemoveSource(w.doc2);                                   // closing it falls back to the other
    CHECK(w.host.SourceFor(noAssets) == &w.doc);
    w.Select(w.scene, "7");
    CHECK(w.host.SourceFor(noAssets) == &w.scene);                 // Current() admitted: agrees with All
}

TEST_CASE("InspectorHost filters: closing the last admitted source leaves null, never a dangling pointer", "[editor][inspector]")
{
    World w;
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    w.Select(w.doc, "a");
    w.host.RemoveSource(w.doc);
    w.host.RemoveSource(w.doc2);
    CHECK(w.host.SourceFor(input) == nullptr);
}

TEST_CASE("InspectorHost filters: nothing stamped falls back to the scene, else the newest admitted source", "[editor][inspector]")
{
    World w;
    const int noAssets = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(noAssets, InspectorFilter::AllBut("assets")));
    CHECK(w.host.SourceFor(noAssets) == &w.scene);
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    CHECK(w.host.SourceFor(input) == &w.doc2);                     // most recently added
}

TEST_CASE("InspectorHost filters: an empty-kind source is admitted only by All", "[editor][inspector]")
{
    World w;
    FakeSource mesh{ "rock.arcmesh", "" };
    w.host.AddSource(mesh);
    const int noAssets = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(noAssets, InspectorFilter::AllBut("assets")));
    w.Select(mesh, "m");                                           // (a real mesh doc never selects; the rule still holds)
    CHECK(w.host.SourceFor(0) == &mesh);
    CHECK(w.host.SourceFor(noAssets) == &w.scene);
}

TEST_CASE("InspectorHost filters: SetFilter refuses an all-excluded set and an unknown id", "[editor][inspector]")
{
    World w;
    InspectorFilter none;
    none.excluded = { "scene", "assets", "input-actions", "material", "sprite", "mesh" };
    CHECK_FALSE(w.host.SetFilter(0, none));
    CHECK(w.host.Find(0)->filter.IsAll());
    CHECK_FALSE(w.host.SetFilter(5, InspectorFilter::Only("scene")));
}

TEST_CASE("InspectorHost filters: a pin captures THIS instance's page; the pin wins over a filter change", "[editor][inspector]")
{
    World w;
    const int scene = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("scene")));
    w.Select(w.scene, "B");
    w.Select(w.doc, "Player/Jump");                                // Current() is the document
    REQUIRE(w.host.CanPin(scene));
    w.host.SetPinned(scene, true);
    CHECK(w.host.Find(scene)->pinnedSource == &w.scene);           // not the binding
    CHECK(w.host.Find(scene)->pinnedKey == "B");
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("input-actions")));
    CHECK(w.host.SourceFor(scene) == &w.scene);                    // pinned: unchanged
    w.host.SetPinned(scene, false);
    CHECK(w.host.SourceFor(scene) == &w.doc);                      // follows the new filter
}

TEST_CASE("InspectorHost filters: ReleaseAll keeps a permanent source and drops its history and pins", "[editor][inspector]")
{
    FakeSource scene{ "Scene", "scene" }, assets{ "Assets", "assets" }, doc{ "P.arcinput", "input-actions" };
    InspectorHost host{ scene };
    host.AddSource(assets, /*permanent*/ true);
    host.AddSource(doc);
    const int onlyAssets = host.AddInstance();
    REQUIRE(host.SetFilter(onlyAssets, InspectorFilter::Only("assets")));
    assets.key = "g1"; host.NotifySelected(assets);
    host.SetPinned(onlyAssets, true);
    host.ReleaseAll();
    CHECK(host.SourceFor(onlyAssets) == &assets);                  // still registered, pin released
    CHECK_FALSE(host.Find(onlyAssets)->pinned);
    CHECK(host.History().empty());
    CHECK(host.Find(onlyAssets)->filter == InspectorFilter::Only("assets"));   // filters are layout
    doc.key = "x"; host.NotifySelected(doc);                       // doc was dropped: ignored
    CHECK(&host.Current() == &scene);
    assets.key = "g2"; host.NotifySelected(assets);                // assets still live
    CHECK(&host.Current() == &assets);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: build, then `ArcaneTests.exe "[inspector]"`.
Expected: compile errors (`Kind`, `SetFilter`, `CanPin(int)`, the `AddSource` overload).

- [ ] **Step 3: Implement**

`InspectorSource.hpp`, add inside `class InspectorSource` after `SourceName()`:
```cpp
        // The source's KIND (InspectorKinds.hpp's catalog id: "scene",
        // "input-actions", "assets"); an Inspector instance's filter admits or
        // excludes by it. "" = a source that never selects (mesh/sprite
        // documents): admitted only by an unfiltered (All) instance.
        [[nodiscard]] virtual std::string_view Kind() const = 0;
```
- `SceneInspectorSource.hpp`: `std::string_view Kind() const override { return "scene"; }`
- `EditorDocument.hpp` (next to its `SourceName()` default): `std::string_view Kind() const override { return {}; }`
- `InputActionsDocument.hpp` (next to its `SourceName()`): `std::string_view Kind() const override { return "input-actions"; }`
- `ShaderEditorDocument`, `SpriteDocument` and `MeshDocument` keep the empty default in this task; Tasks 8-9 give them their kinds with their pages.

`InspectorHost.hpp` changes:
- `#include "Panels/InspectorKinds.hpp"` and `#include <unordered_map>`.
- `Instance` gains `InspectorFilter filter;   // per-instance; layout state (persisted in [EditorInspector][Instances], Task 4)`.
- Replace `void AddSource(InspectorSource& source);` with:
  ```cpp
        // `permanent`: lives as long as the host, like the fallback (the Asset
        // Browser's source) -- ReleaseAll invalidates it instead of dropping it.
        void AddSource(InspectorSource& source, bool permanent = false);
  ```
- Replace `CanPin()` with `[[nodiscard]] bool CanPin(int instanceId);` and update the comment: "True when the page instance `instanceId` shows is resolvable".
- `SetPinned` comment: "pin captures SourceFor(id) + its key + name".
- Add `bool SetFilter(int id, InspectorFilter filter);   // false: unknown id, or it excludes every catalog kind (refused, unchanged)`.
- Private members:
  ```cpp
        void Stamp(InspectorSource& source) { m_stamps[&source] = ++m_stampClock; }
        std::vector<InspectorSource*> m_permanent;                   // AddSource(.., true); the fallback is implicitly permanent
        std::unordered_map<InspectorSource*, std::uint64_t> m_stamps; // last selection event / history landing per source
        std::uint64_t m_stampClock = 0;
  ```
- Update the class's header comment: add "each instance routes through its own filter (spec 2026-09-29 s3)".

`InspectorHost.cpp` changes:
```cpp
    void InspectorHost::AddSource(InspectorSource& source, bool permanent)
    {
        if (!Registered(&source)) m_sources.push_back(&source);
        if (permanent && std::find(m_permanent.begin(), m_permanent.end(), &source) == m_permanent.end())
            m_permanent.push_back(&source);
    }
```
- `RemoveSource`: add `m_stamps.erase(&source);` and `std::erase(m_permanent, &source);` after `std::erase(m_sources, &source);`.
- `NotifySelected`: after `m_current = &source;` add `Stamp(source);`.
- `TryLand` success branch: after `m_current = source;` add `Stamp(*source);`.
- `SourceFor`:
```cpp
    InspectorSource* InspectorHost::SourceFor(int instanceId) const
    {
        const Instance* inst = nullptr;
        for (const Instance& i : m_instances) if (i.id == instanceId) { inst = &i; break; }
        if (!inst) return nullptr;
        if (inst->pinned) return inst->pinnedSource;
        const InspectorFilter& f = inst->filter;
        if (f.Admits(m_current->Kind())) return m_current;          // All always lands here
        InspectorSource* best = nullptr;
        std::uint64_t bestStamp = 0;
        for (InspectorSource* s : m_sources)
        {
            if (!f.Admits(s->Kind())) continue;
            const auto it = m_stamps.find(s);
            if (it != m_stamps.end() && it->second > bestStamp) { best = s; bestStamp = it->second; }
        }
        if (best) return best;
        if (f.Admits(m_fallback->Kind())) return m_fallback;
        for (auto it = m_sources.rbegin(); it != m_sources.rend(); ++it)   // most recently added
            if (f.Admits((*it)->Kind())) return *it;
        return nullptr;
    }
```
- `CanPin` / `SetPinned`:
```cpp
    bool InspectorHost::CanPin(int instanceId)
    {
        InspectorSource* src = SourceFor(instanceId);
        return src && src->PageFor(src->SelectionKey()) != nullptr;
    }

    void InspectorHost::SetPinned(int id, bool pinned)
    {
        Instance* inst = Find(id);
        if (!inst) return;
        if (pinned && !CanPin(id)) return;   // nothing to hold: a pin never holds emptiness
        InspectorSource* src = pinned ? SourceFor(id) : nullptr;   // BEFORE the flag flips: SourceFor reads it
        inst->pinned = pinned;
        inst->sourceClosed = false;
        inst->pinnedSource = src;
        inst->pinnedKey = src ? src->SelectionKey() : std::string{};
        inst->pinnedName = src ? src->SourceName() : std::string{};
    }

    bool InspectorHost::SetFilter(int id, InspectorFilter filter)
    {
        Instance* inst = Find(id);
        if (!inst || filter.ExcludesEveryKind()) return false;
        inst->filter = filter.Sanitized();
        return true;
    }
```
- `ReleaseAll`, replace the first line:
```cpp
        m_sources.assign(1, m_fallback);
        for (InspectorSource* p : m_permanent) m_sources.push_back(p);   // the asset source survives a project switch
        m_stamps.clear();
```
  Keep the rest (history clear, unpin loop). The permanent source's keys die with the project, and clearing the whole history and every pin already covers "invalidate". Filters are NOT touched.
- `SetInstanceIds`: an instance that SURVIVES keeps its filter. Rewrite:
```cpp
    void InspectorHost::SetInstanceIds(std::span<const int> extras)
    {
        std::vector<Instance> kept;
        kept.push_back(*Find(0));
        for (const int id : extras)
        {
            if (id < 1 || id >= kMaxInstances) continue;
            if (std::any_of(kept.begin(), kept.end(), [id](const Instance& i) { return i.id == id; })) continue;
            if (const Instance* old = Find(id)) kept.push_back(*old);
            else { Instance inst; inst.id = id; kept.push_back(inst); }
        }
        std::sort(kept.begin(), kept.end(), [](const Instance& a, const Instance& b) { return a.id < b.id; });
        m_instances = std::move(kept);
    }
```

`InspectorWindows.cpp`: `const bool canPin = inst.pinned || host.CanPin();` (line 174) becomes `const bool canPin = inst.pinned || host.CanPin(inst.id);`. The comment above it keeps its meaning. Its phrase "CanPin calls PageFor on the current source" is split across two comment lines (169 ends "...CanPin calls PageFor on the current", 170 begins "// source, which re-targets..."), so rewrite lines 169-170 by hand to say "CanPin calls PageFor on the instance's routed source", keeping the rest of both lines' wording.

- [ ] **Step 4: Run the tests to verify they pass**

Run: build, `ArcaneTests.exe "[inspector]"`, then `ArcaneTests.exe "[input]"` (InputActionsDocumentUiTest drives an Inspector window).
Expected: PASS, and every pre-existing `[inspector]` case is still green.

- [ ] **Step 5: Commit**

```bash
git add -A ArcaneEditor/src/Panels ArcaneEditor/src/Documents ArcaneTests/src/EditorInspectorHostTest.cpp
git commit -m "feat(editor): InspectorHost routes each instance through its filter -- Kind() on sources, selection stamps, per-instance pin, permanent sources (inspector filters s3)"
```

---

### Task 3: Filtered back/forward over the shared history

**Files:**
- Modify: `ArcaneEditor/src/Panels/InspectorHost.hpp`, `ArcaneEditor/src/Panels/InspectorHost.cpp`
- Test: `ArcaneTests/src/EditorInspectorHostTest.cpp`

**Interfaces:**
- Consumes: `InspectorFilter`, `Instance::filter`.
- Produces: filter-taking overloads. The existing no-arg forms stay and mean All:
  ```cpp
  [[nodiscard]] std::optional<std::size_t> BackIndex(const InspectorFilter& f) const;
  [[nodiscard]] std::optional<std::size_t> ForwardIndex(const InspectorFilter& f) const;
  [[nodiscard]] bool CanGoBack(const InspectorFilter& f) const;
  [[nodiscard]] bool CanGoForward(const InspectorFilter& f) const;
  bool GoBack(const InspectorFilter& f);
  bool GoForward(const InspectorFilter& f);
  [[nodiscard]] const HistoryEntry* BackEntry(const InspectorFilter& f) const;
  [[nodiscard]] const HistoryEntry* ForwardEntry(const InspectorFilter& f) const;
  // The history dropdowns: admitted indices before the instance's position (nearest first) / after the cursor.
  [[nodiscard]] std::vector<std::size_t> BackIndices(const InspectorFilter& f) const;
  [[nodiscard]] std::vector<std::size_t> ForwardIndices(const InspectorFilter& f) const;
  ```
  The **instance's position** is the last admitted index at or before the cursor: the entry it is showing. Back = the nearest admitted index BEFORE that position. Forward = the nearest admitted index AFTER the cursor. With All these reduce to exactly `m_cursor - 1` / `m_cursor + 1`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("InspectorHost filters: Back in a Scene instance skips document entries and re-selects in the scene", "[editor][inspector]")
{
    World w;
    const InspectorFilter scene = InspectorFilter::Only("scene");
    w.Select(w.scene, "A");        // 0
    w.Select(w.doc, "d1");         // 1
    w.Select(w.scene, "B");        // 2
    w.Select(w.doc, "d2");         // 3  <- cursor
    REQUIRE(w.host.CanGoBack(scene));
    CHECK(w.host.BackEntry(scene)->key == "A");                     // position = 2 (B); back = 0 (A)
    CHECK(w.host.BackIndices(scene) == std::vector<std::size_t>{ 0 });
    CHECK_FALSE(w.host.CanGoForward(scene));
    REQUIRE(w.host.GoBack(scene));
    CHECK(w.scene.key == "A");
    CHECK(&w.host.Current() == &w.scene);                           // a landing IS a selection: All follows
    CHECK(w.host.HistoryCursor() == 0);
    CHECK(w.host.ForwardEntry(scene)->key == "B");                  // skips d1
    CHECK(w.host.ForwardIndices(scene) == std::vector<std::size_t>{ 2 });
}

TEST_CASE("InspectorHost filters: the All overloads match the unfiltered history exactly", "[editor][inspector]")
{
    World w;
    const InspectorFilter all;
    w.Select(w.scene, "A");
    w.Select(w.doc, "d1");
    w.Select(w.scene, "B");
    CHECK(w.host.BackIndex(all) == std::optional<std::size_t>{ 1 });
    CHECK(w.host.BackEntry(all) == w.host.BackEntry());
    REQUIRE(w.host.GoBack(all));
    CHECK(w.host.ForwardIndex(all) == std::optional<std::size_t>{ 2 });
}

TEST_CASE("InspectorHost filters: a filtered GoBack prunes a stale admitted entry and keeps walking", "[editor][inspector]")
{
    World w;
    const InspectorFilter input = InspectorFilter::Only("input-actions");
    w.Select(w.doc2, "old");       // 0 -- will not restore
    w.Select(w.doc, "d1");         // 1
    w.Select(w.scene, "S");        // 2
    w.Select(w.doc, "d2");         // 3
    w.doc2.restoreOk = false;
    REQUIRE(w.host.GoBack(input)); // position 3 -> back 1 (d1) restores fine
    CHECK(w.doc.key == "d1");
    CHECK_FALSE(w.host.GoBack(input));   // only the stale doc2 entry is left: pruned, no landing
    CHECK(w.host.History().size() == 3);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: build, then `ArcaneTests.exe "[inspector]"`.
Expected: compile errors for the new overloads.

- [ ] **Step 3: Implement** (`InspectorHost.cpp`; add `#include <optional>` to the header)

```cpp
    std::optional<std::size_t> InspectorHost::BackIndex(const InspectorFilter& f) const
    {
        if (m_history.empty()) return std::nullopt;
        std::size_t pos = m_cursor + 1;                        // one past: the scan below walks down from the cursor
        while (pos-- > 0)
            if (f.Admits(m_history[pos].source->Kind())) break; // the entry this instance shows
        if (pos == static_cast<std::size_t>(-1)) return std::nullopt;
        for (std::size_t i = pos; i-- > 0;)
            if (f.Admits(m_history[i].source->Kind())) return i;
        return std::nullopt;
    }

    std::optional<std::size_t> InspectorHost::ForwardIndex(const InspectorFilter& f) const
    {
        for (std::size_t i = m_cursor + 1; i < m_history.size(); ++i)
            if (f.Admits(m_history[i].source->Kind())) return i;
        return std::nullopt;
    }

    bool InspectorHost::CanGoBack(const InspectorFilter& f) const { return BackIndex(f).has_value(); }
    bool InspectorHost::CanGoForward(const InspectorFilter& f) const { return ForwardIndex(f).has_value(); }
    const InspectorHost::HistoryEntry* InspectorHost::BackEntry(const InspectorFilter& f) const
    { const auto i = BackIndex(f); return i ? &m_history[*i] : nullptr; }
    const InspectorHost::HistoryEntry* InspectorHost::ForwardEntry(const InspectorFilter& f) const
    { const auto i = ForwardIndex(f); return i ? &m_history[*i] : nullptr; }

    bool InspectorHost::GoBack(const InspectorFilter& f)
    {
        RefreshCursorLabel();
        while (const auto i = BackIndex(f))
            if (TryLand(*i)) return true;                      // failure erased *i; recompute
        return false;
    }

    bool InspectorHost::GoForward(const InspectorFilter& f)
    {
        RefreshCursorLabel();
        while (const auto i = ForwardIndex(f))
            if (TryLand(*i)) return true;
        return false;
    }

    std::vector<std::size_t> InspectorHost::BackIndices(const InspectorFilter& f) const
    {
        std::vector<std::size_t> out;
        for (auto i = BackIndex(f); i; )
        {
            out.push_back(*i);
            std::optional<std::size_t> next;
            for (std::size_t j = *i; j-- > 0;)
                if (f.Admits(m_history[j].source->Kind())) { next = j; break; }
            i = next;
        }
        return out;
    }

    std::vector<std::size_t> InspectorHost::ForwardIndices(const InspectorFilter& f) const
    {
        std::vector<std::size_t> out;
        for (std::size_t i = m_cursor + 1; i < m_history.size(); ++i)
            if (f.Admits(m_history[i].source->Kind())) out.push_back(i);
        return out;
    }
```
Check the loop guard in `BackIndex`: `pos` is `std::size_t`, and `while (pos-- > 0)` exits with `pos` wrapped to `SIZE_MAX` when no admitted entry exists at or before the cursor. The `static_cast<std::size_t>(-1)` compare catches that. Keep the existing no-arg `CanGoBack()`/`GoBack()` etc. as they are (they are the All case and existing tests pin them).

- [ ] **Step 4: Run the tests to verify they pass**

Run: `ArcaneTests.exe "[inspector]"`.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add ArcaneEditor/src/Panels/InspectorHost.hpp ArcaneEditor/src/Panels/InspectorHost.cpp ArcaneTests/src/EditorInspectorHostTest.cpp
git commit -m "feat(editor): filtered back/forward over the shared Inspector history (inspector filters s4)"
```

---

### Task 4: Persist filters; detect pre-feature layouts; the default configuration

**Files:**
- Modify: `ArcaneEditor/src/Panels/InspectorHost.hpp`, `ArcaneEditor/src/Panels/InspectorHost.cpp`
- Modify: `ArcaneEditor/src/Panels/InspectorWindows.hpp`, `ArcaneEditor/src/Panels/InspectorWindows.cpp` (the settings handler)
- Test: `ArcaneTests/src/EditorInspectorHostTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // InspectorHost
  static constexpr int kAssetsInstanceId = 1;   // the default layout's "Assets only" instance
  // Default layout (spec s6): exactly {0, 1}; 0 = All but Assets, 1 = Assets only. Pins released.
  void ApplyDefaultInspectorLayout();
  // A pre-feature layout: 0 = All but Assets, and an Assets-only instance at the lowest free id
  // (kept if one already exists). Returns that instance's id (-1 = pool full: 0's filter still applies).
  int  UpgradeLegacyInspectorLayout();
  // Set by the ini handler's ApplyAllFn when a load saw no Filters= line; the app consumes it once.
  [[nodiscard]] bool TakeLegacyLayoutUpgrade() noexcept { return std::exchange(m_legacyLayoutPending, false); }
  // Handler plumbing (InspectorWindows.cpp), not for app code:
  void NoteLayoutReadBegin() noexcept { m_sawFiltersLine = false; }
  void NoteFiltersLine() noexcept { m_sawFiltersLine = true; }
  void NoteLayoutReadEnd() noexcept { if (!m_sawFiltersLine) m_legacyLayoutPending = true; }
  ```
  Also: the ini line `Filters=<id>:<kind>+<kind>,...`, always written after `Ids=`.

- [ ] **Step 1: Write the failing tests** (extend the existing ini test file section)

```cpp
namespace
{
    // One bare ImGui context per case (the existing ini case's shape).
    struct IniContext
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        IniContext() { ImGui::SetCurrentContext(ctx); ImGui::GetIO().IniFilename = nullptr; }
        ~IniContext() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
    };
}

TEST_CASE("InspectorHost ini: Filters= round-trips after Ids=; a Filters= line means no upgrade", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1,3\nFilters=0:assets,1:scene+input-actions+material+sprite+mesh\n");
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());
    CHECK(host.Find(0)->filter == InspectorFilter::AllBut("assets"));
    CHECK(host.Find(1)->filter == InspectorFilter::Only("assets"));
    CHECK(host.Find(3)->filter.IsAll());
    const std::string saved = ImGui::SaveIniSettingsToMemory();
    CHECK(saved.find("Ids=1,3\nFilters=0:assets,1:scene+input-actions+material+sprite+mesh\n") != std::string::npos);
}

TEST_CASE("InspectorHost ini: garbage Filters= entries are sanitized, never thrown on", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory(
        "[EditorInspector][Instances]\nIds=2\nFilters=7:scene,x:assets,2,0:bogus+assets,,2:scene+assets+input-actions+material+sprite+mesh,\n");
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());           // the line was present: not legacy
    CHECK(host.Find(0)->filter == InspectorFilter::AllBut("assets"));   // bogus dropped
    CHECK(host.Find(2)->filter.IsAll());                    // all-excluded -> All
    CHECK(host.Instances().size() == 2);                    // id 7 never created
}

TEST_CASE("InspectorHost ini: a layout without Filters= is flagged once for the legacy upgrade", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=\n");
    CHECK(host.TakeLegacyLayoutUpgrade());
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());            // consumed
    ImGui::LoadIniSettingsFromMemory("[EditorPanels][Visibility]\nConsole=1\n");   // no section at all: also legacy
    CHECK(host.TakeLegacyLayoutUpgrade());
    // A second load WITHOUT ClearIniSettings between: instances absent from
    // the Filters= line read as All (spec s7), survivors of Ids= included.
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1\nFilters=0:assets,1:scene\n");
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());
    REQUIRE(host.Find(0)->filter == InspectorFilter::AllBut("assets"));
    REQUIRE(host.Find(1)->filter == InspectorFilter::AllBut("scene"));
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1\nFilters=\n");   // empty line: a real answer
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());
    CHECK(host.Find(0)->filter.IsAll());
    CHECK(host.Find(1)->filter.IsAll());
}

TEST_CASE("InspectorHost: the default and the legacy-upgrade configurations", "[editor][inspector]")
{
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    (void)host.AddInstance(); (void)host.AddInstance();     // {0,1,2}
    host.ApplyDefaultInspectorLayout();
    REQUIRE(host.Instances().size() == 2);
    CHECK(host.Find(0)->filter == InspectorFilter::AllBut("assets"));
    CHECK(host.Find(InspectorHost::kAssetsInstanceId)->filter == InspectorFilter::Only("assets"));

    InspectorHost legacy{ scene };
    const int ids[] = { 1 };
    legacy.SetInstanceIds(ids);                             // the user already had "Inspector 2"
    const int assetsId = legacy.UpgradeLegacyInspectorLayout();
    CHECK(assetsId == 2);                                   // lowest FREE id; the user's 1 is untouched
    CHECK(legacy.Find(1)->filter.IsAll());
    CHECK(legacy.Find(0)->filter == InspectorFilter::AllBut("assets"));
    CHECK(legacy.Find(2)->filter == InspectorFilter::Only("assets"));
}

TEST_CASE("InspectorHost ini: ClearAllFn resets filters with the list", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1\nFilters=0:assets\n");
    ImGui::ClearIniSettings();
    CHECK(host.Instances().size() == 1);
    CHECK(host.Find(0)->filter.IsAll());
}
```
Also update the EXISTING ini round-trip case. Its saved-text expectation `"[EditorInspector][Instances]\nIds=2,3"` still holds, because `Filters=` follows on the next line. Leave it.

- [ ] **Step 2: Run the tests to verify they fail**

Run: build, then `ArcaneTests.exe "[inspector]"`.
Expected: compile errors, then FAILs.

- [ ] **Step 3: Implement**

`InspectorHost` (members `bool m_sawFiltersLine = false; bool m_legacyLayoutPending = false;`, `#include <utility>`):
```cpp
    void InspectorHost::ApplyDefaultInspectorLayout()
    {
        const int ids[] = { kAssetsInstanceId };
        SetInstanceIds(ids);
        for (Instance& inst : m_instances)
        {
            inst.pinned = false; inst.pinnedSource = nullptr; inst.pinnedKey.clear(); inst.pinnedName.clear(); inst.sourceClosed = false;
        }
        Find(0)->filter = InspectorFilter::AllBut("assets");
        Find(kAssetsInstanceId)->filter = InspectorFilter::Only("assets");
    }

    int InspectorHost::UpgradeLegacyInspectorLayout()
    {
        Find(0)->filter = InspectorFilter::AllBut("assets");
        for (const Instance& inst : m_instances)
            if (inst.filter == InspectorFilter::Only("assets")) return inst.id;   // idempotent
        const int id = AddInstance();
        if (id >= 0) Find(id)->filter = InspectorFilter::Only("assets");
        return id;
    }
```
Also `ReleaseAll` must NOT touch `m_legacyLayoutPending` (a windowed project switch loads the incoming ini AFTER releasing).

`InspectorWindows.cpp` settings handler:
- **ReadInit.** Add `handler.ReadInitFn = [](ImGuiContext*, ImGuiSettingsHandler* h) { static_cast<InspectorHost*>(h->UserData)->NoteLayoutReadBegin(); };`
- **ApplyAll.** Add `handler.ApplyAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h) { static_cast<InspectorHost*>(h->UserData)->NoteLayoutReadEnd(); };`
- **ClearAll.** Extend `handler.ClearAllFn` to also reset instance 0's filter. The lambda is captureless and has only `h`, so cast first:
  ```cpp
        handler.ClearAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h)
        {
            auto* host = static_cast<InspectorHost*>(h->UserData);
            host->SetInstanceIds({});
            (void)host->SetFilter(0, InspectorFilter{});
        };
  ```
- **ReadLine.** Parse `Filters=` after the `Ids=` branch:
```cpp
            if (std::strncmp(line, "Filters=", 8) == 0)
            {
                host->NoteFiltersLine();
                // Spec s7: an instance absent from the line is All. Reset every
                // existing instance first -- instance 0 and every survivor of
                // Ids= keep their old filter through SetInstanceIds otherwise.
                std::vector<int> existing;
                for (const auto& inst : host->Instances()) existing.push_back(inst.id);
                for (const int id : existing) (void)host->SetFilter(id, InspectorFilter{});
                std::string_view rest(line + 8);
                while (!rest.empty())
                {
                    const std::size_t comma = rest.find(',');
                    const std::string_view item = rest.substr(0, comma);
                    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                    const std::size_t colon = item.find(':');
                    if (colon == std::string_view::npos || colon == 0) continue;
                    int id = -1;
                    const auto [p, ec] = std::from_chars(item.data(), item.data() + colon, id);
                    if (ec != std::errc{} || p != item.data() + colon) continue;
                    InspectorFilter f;
                    std::string_view kinds = item.substr(colon + 1);
                    while (!kinds.empty())
                    {
                        const std::size_t plus = kinds.find('+');
                        if (plus != 0) f.excluded.emplace_back(kinds.substr(0, plus));
                        kinds = plus == std::string_view::npos ? std::string_view{} : kinds.substr(plus + 1);
                    }
                    f = f.Sanitized();                 // unknown kinds dropped; all-excluded -> All
                    (void)host->SetFilter(id, f);      // unknown id: refused, harmless
                }
                return;
            }
```
  (`#include <charconv>`, `<string_view>`, `"Panels/InspectorKinds.hpp"`. Restructure the existing `Ids=` early-return into an if/else so both lines parse. `Ids=` MUST be read first: it is written first, and `SetFilter` on an id that does not exist yet is refused.)
- **WriteAll.** After the `Ids=` line, always write `Filters=` followed by `<id>:<k1>+<k2>` for each instance whose filter is not All, comma-joined, then `\n`:
```cpp
            buf->append("\nFilters=");
            bool firstF = true;
            for (const auto& inst : host->Instances())
            {
                if (inst.filter.IsAll()) continue;
                buf->appendf(firstF ? "%d:" : ",%d:", inst.id); firstF = false;
                for (std::size_t k = 0; k < inst.filter.excluded.size(); ++k)
                    buf->appendf(k ? "+%s" : "%s", inst.filter.excluded[k].c_str());
            }
            buf->append("\n\n");
```
  (Replace the existing trailing `buf->append("\n\n");` after `Ids=` accordingly.)
- **Comment.** Update the section comment: filters are layout and persisted; pins are still not.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `ArcaneTests.exe "[inspector]"`.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add ArcaneEditor/src/Panels/InspectorHost.* ArcaneEditor/src/Panels/InspectorWindows.* ArcaneTests/src/EditorInspectorHostTest.cpp
git commit -m "feat(editor): persist Inspector filters (Filters=), flag pre-feature layouts once, default + legacy-upgrade configurations (inspector filters s6/s7)"
```

---

### Task 5: `AssetInspectorSource` and the asset page

**Files:**
- Create: `ArcaneEditor/src/Panels/AssetInspectorSource.hpp`
- Create: `ArcaneEditor/src/Panels/AssetInspectorSource.cpp`
- Create: `ArcaneEditor/src/Panels/TextureImportSettings.hpp`
- Create: `ArcaneEditor/src/Panels/TextureImportSettings.cpp` (the ImGui block moved out of `EditorPanels.cpp`)
- Modify: `ArcaneEditor/src/Panels/EditorPanels.cpp`.
  - Delete `DrawTextureMetaSettingsBlock`, `DrawTextureAssetPanel` and `HasExtensionCI` from its anonymous namespace, with the "F2b Task 13: the Inspector's texture-asset panel" comment block (~lines 2270-2283) and `HasExtensionCI`'s own comment. `HasExtensionCI`'s only caller is `DrawTextureAssetPanel` (:2367), `DrawTextureMetaSettingsBlock`'s only caller is `DrawTextureAssetPanel` (:2377), and `DrawTextureAssetPanel`'s only caller is the scene fallback (:2406), which Task 6 removes. So the three go together.
  - Do the deletion in THIS task. Keep `DrawInspectorBody`'s fallback branch temporarily so the build stays green until Task 6, with its `DrawTextureAssetPanel(*project, selectedAsset, services);` call replaced by:
    ```cpp
                    if (const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(selectedAsset)))
                        DrawTextureImportSettings(*path);
    ```
  - Add `#include "Panels/TextureImportSettings.hpp"` to its include block.
- Modify: `ArcaneEditor/src/Panels/TextureMetaPanel.hpp` (its header comment names the two deleted functions: point it at `TextureImportSettings.cpp`'s `DrawTextureImportSettings`)
- Modify: `ArcaneEditor/src/Panels/AssetPanelModel.hpp` (`selectionGesture`, below)
- Modify: `premake5.lua` (ArcaneTests list: `AssetInspectorSource.cpp`, `TextureImportSettings.cpp`, beside `AssetBrowserPanel.cpp` ~line 1555)
- Test: `ArcaneTests/src/AssetInspectorSourceTest.cpp`

**Interfaces:**
- Consumes: `AssetPanelModel` (`selected`, `Select`, `Find`), `AssetPanelServices`, `AssetPanelActions`, `OpenAssetRow`, `DrawAssetPeekTooltip`, `KindIcon`, `KindLabel`, `SubkindPillText`, `CookStateLabel`, `AssetPill`, `EllipsisToWidth` (all existing), `PropertyGrid`.
- Produces:
```cpp
namespace Arcane::Editor
{
    class AssetInspectorSource final : public InspectorSource, public InspectorPage
    {
    public:
        struct Deps
        {
            AssetPanelModel*          model = nullptr;
            const Arcane::Project*    project = nullptr;
            DocumentHost*             docs = nullptr;       // Open button (OpenAssetRow)
            const AssetPanelServices* services = nullptr;   // thumbnails + peek tooltips
            AssetPanelActions*        actions = nullptr;    // the page's clicks; the app drains them next frame
        };
        void Bind(const Deps& deps) { m_deps = deps; }

        std::string SourceName() const override { return "Assets"; }
        std::string_view Kind() const override { return "assets"; }
        InspectorPage* Page() override;
        InspectorPage* PageFor(std::string_view key) override;
        std::string SelectionKey() const override;            // model->selected.ToString(), "" when invalid
        bool RestoreSelection(std::string_view key) override; // Select(guid) when Find(guid) resolves
        bool Resolves(std::string_view key) const override;   // Find(guid) != nullptr
        [[nodiscard]] std::uint64_t SelectionEpoch() const;   // model->selectionGesture (0 unbound)

        std::vector<InspectorCrumb> Breadcrumb() const override;  // "Assets" (select: clears) > <fileName> (key = guid)
        void Draw(PropertyGrid& grid) override;

    private:
        Deps m_deps{};
        Arcane::Guid m_drawGuid;   // what Draw/Breadcrumb read: Page() -> selected, PageFor(key) -> key
    };

    // The thumbnail + identity + Derived list + action buttons + (texture) import
    // settings -- the old Asset Browser preview pane, now an Inspector page.
    // Exposed for the test; Draw() calls it.
    void DrawAssetPage(const AssetPanelEntry& e, AssetPanelModel& model, const Arcane::Project* project,
                       DocumentHost* docs, const AssetPanelServices& services, AssetPanelActions& actions);
}
```
- `TextureImportSettings.hpp`:
  `void DrawTextureImportSettings(const std::filesystem::path& sourcePath);`
  It draws the `.png`-only note or the four knobs, and writes the `.meta` merge on edit. It is the body of the old `DrawTextureAssetPanel` after its `Separator()` following the preview, plus `DrawTextureMetaSettingsBlock`, moved verbatim with its comments. The `.png` gate stays a local case-insensitive extension check: copy `HasExtensionCI`'s body (`EditorPanels.cpp:2284-2290`) into this file's anonymous namespace. Do NOT substitute `AssetKindOf(...) == AssetKind::Texture`: it also classifies `.jpg/.tga/.bmp/.hdr` as Texture, and the moved comment says the knobs are `.png`-only because that is the one extension the cook enumerates.
- `AssetPanelModel.hpp` gains a selection-GESTURE counter. `selectionStamp` keeps its only-on-change meaning (the Browser scroll at `AssetBrowserPanel.cpp:798` and the Graph re-center at `AssetGraphPanel.cpp:2106` key off it). The Browser, Status and feed click sites call `Select` on the click without comparing guids (e.g. `AssetBrowserPanel.cpp:625-626`, `:976-977`, `AssetStatusPanel.cpp:170-171`), so a re-click of the selected asset there bumps the gesture and IS a selection event (spec s3's one rule). Two sites need care:
  - The row context menu (`AssetBrowserPanel.cpp:236`) calls `Select` on EVERY frame its popup is open, relying on the old no-bump idempotence (its comment, `:230-235`). With a per-call gesture it would fire the asset edge every open frame. Task 6 gates it to the popup's first frame; until Task 6 nothing observes the gesture.
  - The Graph lens (`AssetGraphPanel.cpp:2152`) guards `Select` behind `if (e->guid != model.selected)`, and it must: the canvas selection persists, so that mirror runs every frame. A re-click of the already-selected Graph node is therefore NOT an event. This is the one scoped exception to the re-click rule, left as is. The Graph's right-click (`:2210`) is a one-shot and does count.
  ```cpp
        // Bumped on EVERY Select call, a re-select of the selected guid
        // included: the Inspector's asset source reads it as its selection
        // epoch (spec 2026-09-29 s3: a re-selection IS an event).
        // MONOTONIC: ResetForProjectSwitch deliberately leaves it alone, like
        // entriesStamp, so no consumer can hold a stale equal value.
        std::uint64_t  selectionGesture = 0;
        void Select(const Arcane::Guid& g) { ++selectionGesture; if (g != selected) { selected = g; ++selectionStamp; } }
  ```
  (This replaces the existing one-line `Select`. `ResetForProjectSwitch` does NOT reset `selectionGesture`.)

- [ ] **Step 1: Write the failing test** (`ArcaneTests/src/AssetInspectorSourceTest.cpp`)

Cases 1, 2 and 3 are registry-only: build the model the way `AssetPanelModelTest.cpp` does. Use a temp dir under `fs::temp_directory_path()`, `WriteFile` two `.png` files (`brick.png`, `stone.png`) and an `.arcmat`, then `Arcane::AssetRegistry::ScanContent(dir, "game")`. Get `gBrick`/`gStone` (sidecar-minted) with `GuidForPath(registry.All(), "game://brick.png")` / `"game://stone.png"`. Then `model.MarkAllDirty(); model.RebuildIfDirty(&registry, fake.Make())`. Case 4 (`DrawAssetPage`) needs a real `Arcane::Project` and is written out in full below. Then:
```cpp
TEST_CASE("AssetInspectorSource: the model's shared selection is the source's selection", "[editor][inspector]")
{
    // ... fixture: registry + model with brick.png (gBrick) and stone.png (gStone) ...
    AssetInspectorSource src;
    CHECK(src.SelectionKey().empty());                      // unbound
    src.Bind({ &model, nullptr, nullptr, nullptr, nullptr });
    CHECK(src.SelectionKey().empty());
    CHECK(src.Page() == nullptr);                           // nothing selected: no page
    model.Select(gBrick);
    CHECK(src.SelectionKey() == gBrick.ToString());
    CHECK(src.SelectionEpoch() == model.selectionGesture);
    REQUIRE(src.Page() == &src);
    const auto crumbs = src.Breadcrumb();
    REQUIRE(crumbs.size() == 2);
    CHECK(crumbs[0].label == "Assets");
    CHECK(crumbs[1].label == "brick.png");
    CHECK(crumbs[1].key == gBrick.ToString());
    REQUIRE(src.RestoreSelection(gStone.ToString()));
    CHECK(model.selected == gStone);
    CHECK_FALSE(src.RestoreSelection("not-a-guid"));
    CHECK(model.selected == gStone);                        // a failed restore selects nothing
    REQUIRE(src.PageFor(gBrick.ToString()) == &src);        // a pin's page, without touching the selection
    CHECK(src.Breadcrumb()[1].label == "brick.png");
    CHECK(model.selected == gStone);
    crumbs[0].select();                                     // the "Assets" crumb clears
    CHECK_FALSE(model.selected.IsValid());
}

TEST_CASE("AssetInspectorSource: a deleted asset stops resolving and its page goes away", "[editor][inspector]")
{
    // ... fixture as above; select gBrick; delete brick.png on disk; re-scan the registry;
    //     model.MarkDirty/RebuildIfDirty exactly as AssetPanelModelTest's removal case does ...
    CHECK_FALSE(src.Resolves(gBrick.ToString()));
    CHECK(src.Page() == nullptr);                           // "No selection", never a stale page
    CHECK(src.PageFor(gBrick.ToString()) == nullptr);
}

TEST_CASE("AssetInspectorSource: re-selecting the selected asset is a selection gesture", "[editor][inspector]")
{
    // ... fixture as above ...
    AssetInspectorSource src;
    src.Bind({ &model, nullptr, nullptr, nullptr, nullptr });
    model.Select(gBrick);
    const std::uint64_t epoch = src.SelectionEpoch();
    const std::uint32_t stamp = model.selectionStamp;
    model.Select(gBrick);                                   // a re-click: the Browser/Status click sites call Select without a guid compare
    CHECK(src.SelectionEpoch() == epoch + 1);               // spec s3: a re-selection IS an event
    CHECK(model.selectionStamp == stamp);                   // the Browser scroll / Graph re-center key stays change-only
    model.ResetForProjectSwitch();
    CHECK(src.SelectionEpoch() == epoch + 1);               // monotonic: a project switch never rewinds it
}

TEST_CASE("DrawAssetPage: Copy Path reports an action, never acts; a .png draws the import settings", "[editor][inspector]")
{
    // A REAL project: the texture block resolves its source path through
    // Project::ResolveAsset and draws nothing without one
    // (AssetStatusPanelClickTest.cpp's Project::Create shape).
    const fs::path root = fs::temp_directory_path() / "arcane_asset_page_draw_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    REQUIRE(Arcane::Project::Create(root, "AssetPage").has_value());
    WriteFile(root / "Content", "brick.png", "not a real png, just bytes");   // sidecar-minted guid
    auto project = Arcane::Project::Open(root);
    REQUIRE(project.has_value());
    const Arcane::Guid gBrick = GuidForPath(project->Registry().All(), "game://brick.png");
    REQUIRE(gBrick.IsValid());
    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&project->Registry(), fake.Make()));
    REQUIRE(model.Find(gBrick) != nullptr);
    REQUIRE(model.Find(gBrick)->kind == AssetKind::Texture);

    // Device-less ImGui: the EditorInspectorVectorTest.cpp:262-275 setup
    // (no backend; a software font atlas satisfies NewFrame).
    struct FrameContext
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        FrameContext()
        {
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        }
        ~FrameContext() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
    } fc;

    AssetPanelServices services;                            // no thumbnails, no peeks
    AssetPanelActions actions;
    // One frame of the page. `activate` presses an item BY ID through ImGui's
    // nav-activation path (ActivateItemByID, imgui_internal.h:3629; it lands
    // on the NEXT frame's ButtonBehavior). A rect read after DrawAssetPage
    // would be the LAST item's, never the button's.
    auto frame = [&](const char* activate)
    {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(640.0f, 1000.0f), ImGuiCond_Always);
        ImGui::Begin("t");
        if (activate) ImGui::ActivateItemByID(ImGui::GetID(activate));
        DrawAssetPage(*model.Find(gBrick), model, &*project, /*docs*/ nullptr, services, actions);
        ImGui::End();
        ImGui::Render();                                    // draw data discarded -- no backend
    };

    frame(nullptr);                                         // warm-up: the window exists
    frame(ICON_LC_COPY " Copy Path");                       // queue the press
    frame(nullptr);                                         // the press lands
    CHECK(actions.copyPath == gBrick);                      // reported, never performed

    // The import settings drew (the ".png only" note returns before the knobs):
    // pressing sRGB flips the value merge-written into the .meta sidecar.
    const fs::path meta = root / "Content" / "brick.png.meta";
    const bool srgbBefore = ReadTextureMetaSettingsDisplay(meta).srgb;
    frame("sRGB##texmeta");
    frame(nullptr);
    CHECK(ReadTextureMetaSettingsDisplay(meta).srgb != srgbBefore);
    fs::remove_all(root, ec);
}
```
Write the fixture code in full in the file, not as comments: copy `WriteFile`, `GuidForPath` and `FakeProviders` from `AssetPanelModelTest.cpp:36-119` (through `FakeProviders`' closing `};`). The `// ...` lines above mark where the registry-only fixture goes. Includes: `<imgui.h>`, `<imgui_internal.h>` (`ActivateItemByID`), `"Widgets/IconsLucide.h"` (`ICON_LC_COPY`), `"Panels/TextureMetaPanel.hpp"` (`ReadTextureMetaSettingsDisplay`), `<Arcane/Project/Project.hpp>`.

- [ ] **Step 2: Run the tests to verify they fail**

Register the two .cpp files in the ArcaneTests list, run `generate.bat`, build.
Expected: compile errors (missing header), then FAIL.

- [ ] **Step 3: Implement**

`AssetInspectorSource.cpp`:
```cpp
#include "Panels/AssetInspectorSource.hpp"
#include "Panels/AssetPanelCommon.hpp"
#include "Panels/AssetPanelModel.hpp"
#include "Panels/TextureImportSettings.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Project/Project.hpp>
#include <imgui.h>

namespace Arcane::Editor
{
    namespace
    {
        std::optional<Arcane::Guid> ParseKey(std::string_view key)
        {
            return key.empty() ? std::nullopt : Arcane::Guid::FromString(std::string(key));
        }
    }

    std::string AssetInspectorSource::SelectionKey() const
    {
        return (m_deps.model && m_deps.model->selected.IsValid()) ? m_deps.model->selected.ToString() : std::string{};
    }

    std::uint64_t AssetInspectorSource::SelectionEpoch() const
    {
        return m_deps.model ? m_deps.model->selectionGesture : 0;
    }

    bool AssetInspectorSource::Resolves(std::string_view key) const
    {
        const auto g = ParseKey(key);
        return g && m_deps.model && m_deps.model->Find(*g) != nullptr;
    }

    bool AssetInspectorSource::RestoreSelection(std::string_view key)
    {
        if (!Resolves(key)) return false;
        m_deps.model->Select(*ParseKey(key));
        return true;
    }

    InspectorPage* AssetInspectorSource::Page()
    {
        if (!m_deps.model || !m_deps.model->selected.IsValid() || !m_deps.model->Find(m_deps.model->selected))
            return nullptr;
        m_drawGuid = m_deps.model->selected;
        return this;
    }

    InspectorPage* AssetInspectorSource::PageFor(std::string_view key)
    {
        if (!Resolves(key)) return nullptr;
        m_drawGuid = *ParseKey(key);
        return this;
    }

    std::vector<InspectorCrumb> AssetInspectorSource::Breadcrumb() const
    {
        std::vector<InspectorCrumb> crumbs;
        crumbs.push_back({ "Assets", [m = m_deps.model] { if (m) m->Select(Arcane::Guid{}); }, std::nullopt });
        if (m_deps.model)
            if (const AssetPanelEntry* e = m_deps.model->Find(m_drawGuid))
                crumbs.push_back({ e->fileName, [m = m_deps.model, g = e->guid] { m->Select(g); }, e->guid.ToString() });
        return crumbs;
    }

    void AssetInspectorSource::Draw(PropertyGrid&)
    {
        if (!m_deps.model || !m_deps.services || !m_deps.actions) return;
        if (const AssetPanelEntry* e = m_deps.model->Find(m_drawGuid))
            DrawAssetPage(*e, *m_deps.model, m_deps.project, m_deps.docs, *m_deps.services, *m_deps.actions);
    }
}
```
`DrawAssetPage`, in the same file:
- **Copied content.** COPY the body of `DrawPreviewPane` (`AssetBrowserPanel.cpp:1071-1313`) here WITHOUT the `BeginChild("##assetspreview")`/`EndChild` and the "No selection" branch. The page only draws for a resolved entry, and the Inspector window is the container. Copy, not move: `DrawAssetBrowserBody` still calls `DrawPreviewPane` (:1386) until Task 6 deletes the pane with its helpers, and `AssetBrowserPanel.cpp` is not touched in this task.
- **Header layout.** Keep the compact/stacked header breakpoint logic, measured against `ImGui::GetContentRegionAvail().x` in place of the old `width` parameter. Copy the constants it uses (`kPreviewThumbSize` as `kAssetPageThumbSize`, `kPreviewCompactHeaderMinWidth`, `kPreviewCompactTextColumnMin`, `kActionButtonHeight`), `ContentRelativePath` and `DrawDerivedRow` into THIS file's anonymous namespace (Task 6 deletes the originals with the pane). Internal linkage in both files keeps the duplicate names from clashing. Carry their comments and drop the pane-width history.
- **Open button.** Call `OpenAssetRow` only when `docs` is non-null.
- **Texture settings.** After the action buttons, for `e.kind == AssetKind::Texture && project`, draw `ImGui::Separator();` and then `DrawTextureImportSettings(*project->ResolveAsset(Arcane::AssetId::FromGuid(e.guid)))`, guarded on the optional.

- [ ] **Step 4: Run the tests to verify they pass**

Run: build, `ArcaneTests.exe "[inspector]"`, `ArcaneTests.exe "[editor]"`.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add -A ArcaneEditor/src/Panels ArcaneTests/src/AssetInspectorSourceTest.cpp premake5.lua
git commit -m "feat(editor): AssetInspectorSource -- the Asset Browser's shared selection as an Inspector source; the preview pane's content + texture import settings become its page (inspector filters s6)"
```

---

### Task 6: Wire the asset source into the app; remove the preview pane and the scene page's asset fallback

**Files:**
- Modify: `ArcaneEditor/src/App/EditorApp.hpp` (members, including `m_assetPanelServices`; `#include "Panels/AssetInspectorSource.hpp"`)
- Modify: `ArcaneEditor/src/App/EditorApp.cpp` (register the source as permanent beside `RegisterInspectorInstancesSettings`, ~line 569; re-arm the asset edge in `OnProjectOpened` directly after `m_assetModel.ResetForProjectSwitch();` at ~line 1570, the model's only reset site)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (fill `m_assetPanelServices` where `assetPanelServices` is built ~line 2340; drain the page actions and bind after the three panel consumes ~line 2387; add the selection edge beside the scene's ~line 3693)
- Modify: `ArcaneEditor/src/Panels/SceneInspectorSource.hpp/.cpp` (drop `Deps::selectedAsset`)
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp/.cpp` (`DrawInspectorBody` loses the `selectedAsset` parameter and the no-selection texture branch: `ImGui::TextDisabled("No selection");` only, the one empty-state string (spec s3, decision 5); drop the `TextureImportSettings.hpp` include Task 5 added if nothing else uses it; update its doc comment)
- Modify: `ArcaneEditor/src/Panels/AssetBrowserPanel.hpp/.cpp` (gate `DrawRowContextMenu`'s `model.Select(e.guid)` (:236) to the popup's first frame and rewrite its comment (:230-235), below; remove the preview pane: `kAssetsPreviewPaneDefaultWidth`, `previewPaneWidth`, `PreviewPaneSplitter`, `ClampPreview*`, `kPreview*` constants, `DrawPreviewPane`, `showPreview`, and the anonymous-namespace symbols left with no user: `ContentRelativePath` (:177), `DrawDerivedRow` (:967), `kActionButtonHeight` (:92), `kMinReadableTableWidth` (:86, used only by `ClampPreviewForLayout`). Task 5 holds copies of the first three in `AssetInspectorSource.cpp`. The table takes the width after the rail; update both header comments.)
- Test: `ArcaneTests/src/EditorInspectorHostTest.cpp` (no new host behaviour. The wiring is proven by the witness and golden in Task 10.) Search `ArcaneTests/src` for `previewPaneWidth|kAssetsPreviewPaneDefaultWidth|DrawInspectorBody(` and fix any compile fallout.

**Interfaces:**
- Consumes: `AssetInspectorSource` (Task 5), `InspectorHost::AddSource(.., true)` (Task 2), `SelectionEdge`.
- Produces: `EditorApp::m_assetSource`, `m_assetSelectionEdge`, `m_assetPageActions`, `m_assetPanelServices`.

- [ ] **Step 1: Write the failing check**

This is wiring, and its test is the build plus the existing suites. Before touching code, record the baseline:
```
ArcaneTests.exe "~[gpu]"     (from the exe dir)  -> note the case/pass counts
```

- [ ] **Step 2: Implement**

`EditorApp.hpp`:
- **Include.** Add `#include "Panels/AssetInspectorSource.hpp"   // m_assetSource` beside the `SceneInspectorSource.hpp` include (line 46). The by-value member needs the complete type.
- **Declaration order.** Declare the new members BEFORE `m_inspectorHost`. The host holds raw source pointers, so the source must outlive it. Declaration order = construction order, and destruction runs in reverse. Match the existing `m_sceneSource` placement comment.
- **Members:**
```cpp
        Arcane::Editor::AssetPanelServices   m_assetPanelServices;   // the asset page's thumbnails + peeks; a MEMBER: the page draws after DrawEditorUi returns
        Arcane::Editor::AssetInspectorSource m_assetSource;          // permanent Inspector source over m_assetModel.selected
        Arcane::Editor::SelectionEdge        m_assetSelectionEdge;   // Observe(m_assetModel.selectionGesture, key)
        Arcane::Editor::AssetPanelActions    m_assetPageActions;     // the asset page's clicks, drained next frame
```
`EditorApp.cpp`, `OnProjectOpened`, directly after `m_assetModel.ResetForProjectSwitch();` (~line 1570):
```cpp
        m_assetSelectionEdge.lastEpoch = m_assetModel.selectionGesture;   // re-arm, as the scene edge does (EditorAppScene.cpp:161)
```
`selectionGesture` is monotonic (Task 5: `ResetForProjectSwitch` does not reset it), so the next gesture in the new project is always an event; the re-arm keeps the two edges' shape identical.
`EditorApp.cpp` (after `RegisterInspectorInstancesSettings(m_inspectorHost);`):
```cpp
        m_inspectorHost.AddSource(m_assetSource, /*permanent*/ true);
```
`EditorAppFrame.cpp`, directly after the three `ConsumeAssetPanelActions(...)` lines:
```cpp
        // The asset page's clicks (Inspector filters s6): raised during LAST
        // frame's Inspector draw, performed here with the panels' own -- one
        // frame late, invisible, and the same handler as the three panels.
        ConsumeAssetPanelActions(m_assetPageActions, ls);
        m_assetPageActions = {};
        m_assetSource.Bind({ &m_assetModel, proj, &m_documents, &m_assetPanelServices, &m_assetPageActions });
```
- **Services lifetime.** `assetPanelServices` is a LOCAL of `EditorApp::DrawEditorUi` (declared ~line 2340). The Inspector windows draw in `EditorApp::DrawSelectionPanels` (~line 3664, `DrawInspectorWindows` at ~3706), which `MainLoop` calls at ~line 470, AFTER `DrawEditorUi` has returned (~line 465). A pointer to the local dangles. So bind the MEMBER: directly after the local is fully built at ~2340 (every field assigned), add `m_assetPanelServices = assetPanelServices;`. The three panels keep using the local.

Beside the scene edge (~line 3693):
```cpp
        // selectionGesture, not selectionStamp: a re-click of the selected
        // asset IS a selection event (spec s3), and the stamp moves only on change.
        if (m_assetSelectionEdge.Observe(m_assetModel.selectionGesture, m_assetSource.SelectionKey()))
            m_inspectorHost.NotifySelected(m_assetSource);
```
Scene source bind: drop `&m_assetModel.selected` from the `Deps` initializer. Update the comment block above it: the "trailing fallback" sentence goes, and the asset selection now routes as its own source.

`AssetBrowserPanel.cpp`, `DrawAssetBrowserBody`: delete `showPreview`, `drawnWidth` and the splitter/pane calls. `DrawTable(..., /*tableWidth*/ 0.0f)` fills the line. Delete the now-unused helpers and constants (`DrawPreviewPane`, `PreviewPaneSplitter`, `ClampPreview*`, `kPreview*`, `ContentRelativePath`, `DrawDerivedRow`, `kActionButtonHeight`, `kMinReadableTableWidth`), the `previewPaneWidth` field and `kAssetsPreviewPaneDefaultWidth`. Update the file-header and `DrawAssetBrowserBody` doc comments ("the rail + the table; the asset's details are the Assets Inspector's page").

`AssetBrowserPanel.cpp`, `DrawRowContextMenu` (:228-236): the unconditional per-frame `model.Select(e.guid);` becomes a first-frame call. Task 5's `Select` bumps `selectionGesture` on every call, so the old per-frame call would fire the asset edge (and `NotifySelected` + a history `Stamp`) on every frame the popup is open. `BeginPopupContextItem` has made the popup the current window, so `IsWindowAppearing()` is true exactly on its first frame. Replace the comment and the call with:
```cpp
            // Right-click acts on this row: make it the tracked selection so
            // the highlight + the Inspector/Assets-menu follow (matches
            // AssetBrowser.cpp's own old behavior). ONCE, on the popup's first
            // frame: Select bumps AssetPanelModel::selectionGesture on every
            // call (a re-selection IS an Inspector selection event, spec
            // 2026-09-29 s3), so a per-frame call would re-fire the Inspector's
            // asset edge for as long as the menu stays open.
            if (ImGui::IsWindowAppearing())
                model.Select(e.guid);
```

- [ ] **Step 3: Build and run the suites**

Run: build Debug, then `ArcaneTests.exe "~[gpu]"` from the exe dir.
Expected: the same case/pass count as Step 1, 0 failures (Task 6 adds no tests; Tasks 1-5's cases are already in the Step 1 baseline).

- [ ] **Step 4: Desk smoke (headless, no window)**

Run: `bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project ReferenceProject --headless --frames 90 --report %TEMP%\t6.json`
Expected: exit 0, no ERROR lines about the Inspector. The goldens are EXPECTED to differ now (the pane is gone); do not run the gate yet.

- [ ] **Step 5: Commit**

```bash
git add -A ArcaneEditor/src ArcaneTests/src
git commit -m "feat(editor): the Asset Browser is an Inspector source (permanent, its own selection edge, page actions drained with the panels'); the preview pane and the scene page's asset fallback are gone (inspector filters s6)"
```

---

### Task 7: The filter dropdown, titles and "no source" line; the default layout and the one-time upgrade

**Files:**
- Modify: `ArcaneEditor/src/Panels/InspectorWindows.hpp` (`kPrimaryInspectorWindowId`, `InspectorWindowTitle`), `ArcaneEditor/src/Panels/InspectorWindows.cpp`
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp/.cpp` (`BuildDefaultLayout`, `EndDockSpace`; `EditorPanels.cpp` gains `#include "Panels/InspectorWindows.hpp"   // kPrimaryInspectorWindowId`: it does not include it today)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (consume `EndDockSpace`'s result, ~line 2305)
- Test: `ArcaneTests/src/EditorInspectorHostTest.cpp` (a device-less window test)

**Interfaces:**
- Consumes: Tasks 2-4 host API; `InspectorFilterLabel`; `kInspectorKinds`.
- Produces:
  ```cpp
  // InspectorWindows.hpp (EditorPanels.cpp adds the include in Step 4)
  // Instance 0's ImGui id. ImHashStr skips "###", so this is the same id as the
  // legacy bare "Inspector" window: the constant is for clarity, not a new id.
  inline constexpr const char* kPrimaryInspectorWindowId = "###Inspector";
  // EditorPanels.hpp
  inline constexpr const char* kAssetsInspectorWindowId = "###inspector_1";   // == InspectorHost::kAssetsInstanceId
  struct DockSpaceResult { bool builtDefault = false; bool upgradedLegacy = false; };
  // `upgradeLegacyInspectorId` >= 1: dock that instance into a split right of the Asset Browser's node
  // (the one-time pre-feature upgrade); -1 = no upgrade this frame.
  DockSpaceResult EndDockSpace(bool resetLayout = false, int upgradeLegacyInspectorId = -1);
  // InspectorWindows.hpp: the window title for an instance (tested directly)
  [[nodiscard]] std::string InspectorWindowTitle(const InspectorHost::Instance& inst);
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("InspectorWindowTitle: the filter label rides the title; the ### id never changes", "[editor][inspector]")
{
    InspectorHost::Instance main;                        // id 0, All
    CHECK(InspectorWindowTitle(main) == "Inspector###Inspector");
    // The stable id: every title of instance 0 hashes to the same window id,
    // and ImHashStr skips "###" (imgui.cpp:2539-2544), so that id IS the legacy
    // bare "Inspector" one -- true by construction; these CHECKs pin it.
    CHECK(ImHashStr("Inspector###Inspector") == ImHashStr("Inspector - Scene###Inspector"));
    CHECK(ImHashStr("Inspector###Inspector") == ImHashStr(kPrimaryInspectorWindowId));
    CHECK(ImHashStr(kPrimaryInspectorWindowId) == ImHashStr("Inspector"));   // [Window][Inspector] carries over
    main.filter = InspectorFilter::AllBut("assets");
    CHECK(InspectorWindowTitle(main) == "Inspector - All but Assets###Inspector");
    InspectorHost::Instance second; second.id = 1;
    CHECK(InspectorWindowTitle(second) == "Inspector 2###inspector_1");
    second.filter = InspectorFilter::Only("assets");
    CHECK(InspectorWindowTitle(second) == "Inspector 2 - Assets###inspector_1");
}
```
Plus a device-less frame case (the `IniContext` from Task 4, plus `io.DisplaySize = {1280,720}`, `io.Fonts->Build()`, `NewFrame`/`Render`), for a host whose instance 1 is `Only("input-actions")` with no input-actions source registered:
```cpp
    InspectorWindowsState state;
    // frame 1: DrawInspectorWindows(host, state, nullptr); ImGui::Render();
    ImGuiWindow* w = ImGui::FindWindowByID(ImHashStr("###inspector_1"));
    REQUIRE(w != nullptr);
    CHECK(std::string(w->Name) == "Inspector 2 - Input Actions###inspector_1");
```
Also assert the one-line note text through a probe. The note is `ImGui::TextDisabled("No Input Actions document open")`, and the simplest probe is that the window's `DC.CursorMaxPos` moved below the header. If that proves brittle, assert only the title and leave the note to the desk check. Record which one in the commit message.

- [ ] **Step 2: Run the tests to verify they fail**

Expected: `InspectorWindowTitle` undefined.

- [ ] **Step 3: Implement the window changes** (`InspectorWindows.cpp`)

- **Title.** `InspectorWindowTitle(inst)`:
  - `base` = `"Inspector"` for id 0, otherwise `"Inspector " + std::to_string(id + 1)`.
  - Append `" - " + InspectorFilterLabel(inst.filter)` when the filter is not All.
  - Append the stable id: `"###Inspector"` for id 0, `"###inspector_" + std::to_string(id)` otherwise.
  - Replace the `title` computation in `DrawInspectorWindows` with it.
  - `FindWindowByName("Inspector")` (the primary's dock lookup for a new instance) becomes `FindWindowByName(kPrimaryInspectorWindowId)`. `FindWindowByName` hashes its argument, so `"###Inspector"` resolves.
  - `EditorAppFrame.cpp`'s two `SelectDockTab("Inspector")` calls (~lines 3536/3547) are left alone: they sit inside `EditorApp::SyncCenterTabFocus`, which Task 8 deletes entirely. (They still resolve meanwhile: `"Inspector"` and `"###Inspector"` are the same id.)
  - `PanelRegistry.hpp`'s `"Inspector"` entry is the Window-menu label and the visibility ini key, NOT a window name. Leave it.
  - Tests that `Begin("Inspector")` on their own context (`EditorInspectorVectorTest`, `InputActionsDocumentUiTest`, `PropertyGridTest`) draw their own window and are unaffected. `InputActionsDocumentUiTest` drives the real `DrawInspectorWindows` too: if it looks the primary window up by `"Inspector"`, switch it to the constant.
- **Filter dropdown.** Draw it in `DrawHeader`, after the forward arrow and before the breadcrumb child: insert the snippet between line 91 (the forward arrow's `ImGui::EndDisabled();`) and line 92 (`ImGui::SameLine();`). The snippet opens with its own `SameLine()`, and the existing line-92 `SameLine()` then keeps the crumbs on the combo's row. Do NOT subtract the combo's width in `crumbWidth`: the combo is drawn BEFORE the crumb child, so `GetContentRegionAvail().x` at line 102 already excludes it (the pin is subtracted by hand only because it is drawn after).
  ```cpp
            ImGui::SameLine();
            const std::string label = InspectorFilterLabel(inst.filter);
            ImGui::SetNextItemWidth(std::min(ImGui::CalcTextSize(label.c_str()).x + ImGui::GetFrameHeight() + style.FramePadding.x * 2.0f, 160.0f));
            if (ImGui::BeginCombo("##filter", label.c_str()))
            {
                for (const InspectorKind& k : kInspectorKinds)
                {
                    bool ticked = inst.filter.Admits(k.id);
                    InspectorFilter next = inst.filter;
                    if (ticked) next.excluded.emplace_back(k.id);
                    else std::erase(next.excluded, std::string(k.id));
                    const bool lastTicked = ticked && next.ExcludesEveryKind();
                    ImGui::BeginDisabled(lastTicked);
                    if (ImGui::Checkbox(std::string(k.displayName).c_str(), &ticked)) actions.filter = next;
                    ImGui::EndDisabled();
                    if (lastTicked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("An Inspector must show at least one kind");
                }
                ImGui::EndCombo();
            }
            // Spec s5: a long multi-kind label is clipped to the combo; the
            // tooltip carries the full list (the combo frame is the last item
            // whether or not it is open).
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s", label.c_str());
  ```
  `HeaderActions` gains `std::optional<InspectorFilter> filter;`, and `ApplyHeaderActions` runs `if (a.filter) (void)host.SetFilter(inst.id, *a.filter);`.
- **Filtered arrows.** In `DrawHeader`, the back/forward arrows and their right-click lists use the filter overloads:
  - `host.CanGoBack(inst.filter)`, `host.BackEntry(inst.filter)`;
  - the back list iterates `host.BackIndices(inst.filter)`, and the forward list `host.ForwardIndices(inst.filter)`;
  - `actions.back` calls `host.GoBack(inst.filter)` in `ApplyHeaderActions`. `ApplyHeaderActions` receives `inst` already, so the forward arrow follows the same pattern.
- **Empty states.** In `DrawInspectorWindows`, when `!inst.pinned && src == nullptr`, a single-kind filter names the one admitted kind, otherwise one generic line. `displayName` is a `std::string_view`, so pass it with `%.*s` (a string_view through `...` to `%s` is undefined behaviour, and MSVC does not check ImGui's format args):
  ```cpp
            std::string_view onlyName;
            int admitted = 0;
            for (const InspectorKind& k : kInspectorKinds)
                if (inst.filter.Admits(k.id)) { onlyName = k.displayName; ++admitted; }
            if (admitted == 1)
                ImGui::TextDisabled("No %.*s document open", static_cast<int>(onlyName.size()), onlyName.data());
            else
                ImGui::TextDisabled("Nothing to show for this filter");
  ```

  A routed source with a null page keeps today's behaviour: the page-less header, and the empty-state line drawn by the page. The one empty-state string is "No selection" (spec s3, decision 5), which the scene body already prints (`EditorPanels.cpp:2410`). For a null asset `Page()` nothing draws, so add `else if (src && !page && !inst.pinned) ImGui::TextDisabled("No selection");` to match.

- [ ] **Step 4: Implement the default layout and the upgrade** (`EditorPanels.cpp`)

`BuildDefaultLayout`, after `bottomLeftId` is split:
```cpp
        // Inspector filters (spec s6): the "Assets only" Inspector sits where
        // the Asset Browser's preview pane used to -- a split to the RIGHT of
        // the browser's node -- so browsing assets never replaces the entity
        // page in the main Inspector.
        ImGuiID assetsInspectorId = 0;
        const ImGuiID browserNodeId = ImGui::DockBuilderSplitNode(bottomLeftId, ImGuiDir_Left, 0.70f,
                                                                  nullptr, &assetsInspectorId);
```
- **Include.** Add `#include "Panels/InspectorWindows.hpp"   // kPrimaryInspectorWindowId` to `EditorPanels.cpp`'s include block (lines 1-52 have no path to it today; `EditorPanels.hpp` does not pull it in).
- **Docking.** Dock `"Asset Browser"`/`"Console"`/`"Problems"` into `browserNodeId`, then `ImGui::DockBuilderDockWindow(kAssetsInspectorWindowId, assetsInspectorId);`. The right node's `DockBuilderDockWindow("Inspector", rightId)` becomes `DockBuilderDockWindow(kPrimaryInspectorWindowId, rightId)` (the same id; the constant is for clarity).
- **Return value and shape.** `EndDockSpace` must still run `DockSpace()`, the two flag scrubs and `ImGui::End()` after a build, so it cannot return at the build site. Its body becomes:
```cpp
    DockSpaceResult EndDockSpace(bool resetLayout, int upgradeLegacyInspectorId)
    {
        const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
        DockSpaceResult result;
        if (resetLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
        {
            BuildDefaultLayout(dockspaceId);
            result.builtDefault = true;
        }
        else if (upgradeLegacyInspectorId >= 1)
        {
            // ... the upgrade branch below ...
        }
        // ... unchanged: the DockSpace() submission, the central-node scrub,
        //     the HiddenTabBar scrub (with their comments) ...
        ImGui::End();
        return result;
    }
```
- **Upgrade branch.** It runs only when the layout was NOT just built and `upgradeLegacyInspectorId >= 1`:
```cpp
        else if (upgradeLegacyInspectorId >= 1)
        {
            // The one-time pre-feature upgrade (spec s6): split the Asset
            // Browser's node and dock the Assets instance on its right. The
            // browser's DockId comes from its window (or its ini settings when
            // the window has not been submitted yet this session).
            ImGuiID browserDock = 0;
            if (ImGuiWindow* w = ImGui::FindWindowByName("Asset Browser")) browserDock = w->DockId;
            else if (ImGuiWindowSettings* s = ImGui::FindWindowSettingsByID(ImHashStr("Asset Browser"))) browserDock = s->DockId;
            const std::string id = "###inspector_" + std::to_string(upgradeLegacyInspectorId);
            // The main Inspector needs no re-dock: "###Inspector" hashes to the
            // legacy bare "Inspector" id (ImHashStr skips "###", imgui.cpp:2539),
            // so its [Window][Inspector] entry and DockId carry over unchanged.
            if (browserDock != 0 && ImGui::DockBuilderGetNode(browserDock) != nullptr)
            {
                ImGuiID right = 0, left = browserDock;
                right = ImGui::DockBuilderSplitNode(left, ImGuiDir_Right, 0.30f, nullptr, &left);
                ImGui::DockBuilderDockWindow(id.c_str(), right);
            }
            ImGui::DockBuilderFinish(dockspaceId);
            result.upgradedLegacy = true;   // undocked browser: DrawInspectorWindows' New Inspector placement applies
        }
```
- **Split direction.** Check the argument order against `imgui_internal.h`'s `DockBuilderSplitNode(ImGuiID node_id, ImGuiDir split_dir, float size_ratio_for_node_at_dir, ImGuiID* out_id_at_dir, ImGuiID* out_id_at_opposite_dir)`. The call above must give the RIGHT 30% to the Inspector and keep the browser's tab set on the left. If `browserDock` names a node that is not a leaf (a split parent), use the leaf the window is docked in: `ImGuiDockNode::IsLeafNode()`, or the window's `DockNode`.

App (`EditorAppFrame.cpp`, replacing `Arcane::Editor::EndDockSpace(menuReq.resetLayout);`):
```cpp
        // ALWAYS consume the flag: on a Reset Layout frame a pending flag
        // would otherwise survive into the next frame and split the freshly
        // built default layout's browser node a second time.
        const bool pendingLegacy = m_inspectorHost.TakeLegacyLayoutUpgrade();
        const bool legacy = pendingLegacy && !menuReq.resetLayout;
        const int legacyAssetsId = legacy ? m_inspectorHost.UpgradeLegacyInspectorLayout() : -1;
        const Arcane::Editor::DockSpaceResult dock = Arcane::Editor::EndDockSpace(menuReq.resetLayout, legacyAssetsId);
        if (dock.builtDefault)
            m_inspectorHost.ApplyDefaultInspectorLayout();   // first run and Window > Reset Layout: exactly {0, 1}
```
- **Ordering hazard (document it in a comment).** On a FIRST run there is no ini, so `TakeLegacyLayoutUpgrade()` is false and `builtDefault` applies the defaults. If a loaded ini had no dock node (the build fires AND the legacy flag was set), `UpgradeLegacyInspectorLayout` ran first and `ApplyDefaultInspectorLayout` then overrides it to exactly {0, 1}. That is correct.
- **Grids.** When `ApplyDefaultInspectorLayout` removes instances, erase `m_inspectorWindows.grids` for ids no longer present, the same as the `res.closed` loop.

- [ ] **Step 5: Run the tests, then a windowed desk check**

Run: `ArcaneTests.exe "[inspector]"`, then `"~[gpu]"`.
Expected: PASS.

Windowed (the user runs it): delete `%LOCALAPPDATA%\Arcane\editor\layouts\<ReferenceProject guid>.ini`, launch the editor on ReferenceProject, and confirm:
- the two Inspectors;
  - after restoring the OLD ini and relaunching, the main Inspector keeps its old dock position (`###Inspector` is the legacy `Inspector` id, so `[Window][Inspector]` and its DockId carry over) and "Inspector 2 - Assets" sits right of the Asset Browser;
- an asset click leaves the main Inspector's entity page alone;
- restoring an old ini (keep a copy BEFORE deleting) upgrades once, and relaunching does not re-upgrade.

- [ ] **Step 6: Commit**

```bash
git add -A ArcaneEditor/src ArcaneTests/src
git commit -m "feat(editor): Inspector filter dropdown + filtered titles/history/empty states; default layout = All-but-Assets + Assets-only beside the browser; pre-feature layouts upgraded once (inspector filters s5/s6)"
```

---

### Task 8: The shared document-page selection; the material page; the Material window retires

**Files:**
- Create: `ArcaneEditor/src/Documents/DocumentPageSelection.hpp`
- Modify: `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp/.cpp`
- Modify: `ArcaneEditor/src/App/EditorApp.hpp`, `ArcaneEditor/src/App/EditorAppFrame.cpp` (retire `DrawMaterialPanel`'s call ~line 2415; `ResolveActiveMaterialDoc` (decl `EditorApp.hpp:348`, def ~`EditorAppFrame.cpp:3229-3250`); `EditorApp::SyncCenterTabFocus` ENTIRELY (decl + comment `EditorApp.hpp:351-355`, def + phase-16b comment ~`EditorAppFrame.cpp:3484-3550`, call `EditorAppFrame.cpp:468`); `m_activeMaterialGuid`, `m_materialDocCount`)
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp/.cpp` (`BuildDefaultLayout`: drop `DockBuilderDockWindow("Material", rightId)` and fix the comment above it; delete `ViewportPanelResult::appearing` (`EditorPanels.hpp:243-246`) and its assignment + comment (`EditorPanels.cpp:1170-1175`), whose only reader was `SyncCenterTabFocus`)
- Test: `ArcaneTests/src/DocumentPageSelectionTest.cpp` (new), plus the shader document test that exists today. Grep `ArcaneTests/src` for `ShaderEditorDocument` and extend the file that already constructs one headlessly (e.g. `ShaderEditorDocumentTest.cpp`, whose "material panel layout round-trips through imgui.ini" case is the precedent).

**Interfaces:**
- Consumes: `InspectorSource`/`InspectorPage`, `PropertyGrid`, `EditGesture::ScopeGuard`.
- Produces:
```cpp
// Documents/DocumentPageSelection.hpp (namespace Arcane::Editor) -- a document whose Inspector page is the
// WHOLE document's (material, sprite, mesh): one key, selected when the
// document opens, re-selected by a click in its content (spec s3's one
// selection rule). Header-only; ImGui calls only in NoteContentClick.
struct DocumentPageSelection
{
    std::string   key;         // "material" / "sprite" / "mesh"
    std::uint64_t epoch = 1;   // 1 = selected at open: the app's per-document epoch map starts at 0, so frame 1 is an event
    [[nodiscard]] std::string SelectionKey() const { return key; }
    [[nodiscard]] bool Resolves(std::string_view k) const { return k == key; }
    // Call between the document window's Begin/End, after its content: a
    // mouse click (left/right/middle) inside the window's INNER rect -- never
    // the title bar or a dock tab -- bumps the epoch. Tab switches and focus
    // never do (they are not clicks in the content).
    void NoteContentClick();
};
```
- `ShaderEditorDocument` overrides:
  - `Kind()` returns `"material"`.
  - `Page()`/`PageFor(key)` return the material page when the key resolves.
  - `SelectionKey()`, `RestoreSelection(key)` (true when it resolves, no epoch bump), `Resolves`, and `SelectionEpoch()` return `m_pageSel.epoch`.
- The page is a private nested `class MaterialInspectorPage final : public InspectorPage` holding `ShaderEditorDocument&`. The base MUST be public: with a private base, `Page()` returning `&m_page` as `InspectorPage*` is an inaccessible conversion (MSVC C2243).
  - `Breadcrumb()` returns one crumb, `{ m_title, <select: no-op>, "material" }`.
  - `Draw(PropertyGrid&)` calls `doc.DrawMaterialPageBody()`.

- [ ] **Step 1: Write the failing tests**

`DocumentPageSelectionTest.cpp` (a device-less ImGui context; the click-driving precedent is `AssetStatusPanelClickTest.cpp:183-193` / `InputActionsDocumentUiTest.cpp:89-90`):
```cpp
// DocumentPageSelection (spec 2026-09-29 s3): opened = selected; a click in a
// document's CONTENT re-selects its page; the title bar / a tab never does.
#include <catch2/catch_test_macros.hpp>
#include <Documents/DocumentPageSelection.hpp>
#include <imgui.h>
#include <optional>

using namespace Arcane::Editor;

namespace
{
    struct BareContext
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        BareContext()
        {
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(800.0f, 600.0f);
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        }
        ~BareContext() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
    };
}

TEST_CASE("DocumentPageSelection: selected at open; a content click re-selects; a click outside does not", "[editor][inspector]")
{
    BareContext bc;
    DocumentPageSelection sel{ "material" };
    CHECK(sel.epoch == 1);
    CHECK(sel.Resolves("material"));
    CHECK_FALSE(sel.Resolves("mesh"));

    const ImVec2 pos(100.0f, 100.0f), size(300.0f, 200.0f);
    // Every frame submits the same window at the same rect: IsWindowHovered
    // reads the hovered window NewFrame computed from the PREVIOUS frame's
    // windows, so the first frame is a warm-up. A button event only registers
    // as a click when it CHANGES the button state, hence the release frames.
    auto frame = [&](std::optional<ImVec2> mouse, std::optional<bool> leftDown)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (mouse) io.AddMousePosEvent(mouse->x, mouse->y);
        if (leftDown) io.AddMouseButtonEvent(ImGuiMouseButton_Left, *leftDown);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::Begin("doc");
        sel.NoteContentClick();
        ImGui::End();
        ImGui::Render();                                             // draw data discarded -- no backend
    };

    frame(std::nullopt, std::nullopt);                               // warm-up: "doc" exists
    frame(ImVec2(pos.x + 150.0f, pos.y + 100.0f), true);             // A: press in the content
    CHECK(sel.epoch == 2);
    frame(std::nullopt, false);                                      // release
    frame(ImVec2(pos.x + 150.0f, pos.y + 4.0f), true);               // B: press on the TITLE BAR
    CHECK(sel.epoch == 2);
    frame(std::nullopt, false);                                      // release
    frame(std::nullopt, std::nullopt);                               // C: no click
    CHECK(sel.epoch == 2);
}
```

In the shader document test, construct a document the way the existing test does, then:
```cpp
    CHECK(doc.Kind() == "material");
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);                  // selected at open
    REQUIRE(doc.Page() != nullptr);
    CHECK(doc.Page()->Breadcrumb().size() == 1);
    CHECK(doc.Page()->Breadcrumb()[0].key == std::optional<std::string>{ "material" });
    CHECK(doc.RestoreSelection("material"));
    CHECK(doc.SelectionEpoch() == 1);                  // a restore is not a click
    CHECK_FALSE(doc.Resolves("node:7"));
```

- [ ] **Step 2: Run the tests to verify they fail**

Expected: `DocumentPageSelection.hpp` missing; `Kind()` returns `""`.

- [ ] **Step 3: Implement**

`DocumentPageSelection::NoteContentClick` (inline in the header, `#include <imgui_internal.h>`):
```cpp
inline void DocumentPageSelection::NoteContentClick()
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImGuiIO& io = ImGui::GetIO();
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                      || ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    if (clicked && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
        && w->InnerRect.Contains(io.MousePos))
        ++epoch;
}
```
`ShaderEditorDocument`:
- **Selection member.** Add `DocumentPageSelection m_pageSel{ "material" };` and the overrides listed under Interfaces.
- **Content click.** In `Draw`, after the window's content and before `ImGui::End()`, on the non-collapsed path, call `m_pageSel.NoteContentClick();`. The node-editor canvas draws directly in the document window: this vendored imgui-node-editor has its `BeginChild` commented out (`imgui_node_editor.cpp:1210-1214`), and `ImGuiEx::Canvas` changes `io.MousePos` only between `ed::Begin`/`ed::End`, restoring it at `ed::End`. So the check, which runs after the canvas, sees the document window hovered and screen-space coordinates. `ChildWindows` is there for the real children: the snippet `InputTextMultiline` and the `##preview` child. Verify it at the desk (Step 5).
- **Page body.** Rename `DrawMaterialWindow()` to `DrawMaterialPageBody()` and drop its `Begin("Material")`/`End()`. KEEP its `EditGesture::ScopeGuard` as its first local. The page body now draws inside an Inspector instance window. Keep:
  - the title line;
  - "(Instance)";
  - the `PaneSplitter`-over-`Layout().previewSplit` preview/params split, unchanged.

  Two behaviour changes, both intended: the body now draws AFTER the documents (the Inspector phase, `DrawSelectionPanels`, runs after `DrawEditorUi`'s `m_documents.DrawAll`), and it draws on collapsed and background-tab frames too (`InspectorWindows` calls `page->Draw` even when `Begin` returns false, `InspectorWindows.cpp:174-179`), where the old body ran only inside `if (ImGui::Begin("Material"))`. Its `BeginChild`/`PaneSplitter` are safe there.
- **Remove:**
  - the free `DrawMaterialPanel` (declaration `ShaderEditorDocument.hpp:905` and definition);
  - `TabBecameVisible` (with its comment, `ShaderEditorDocument.hpp:~196-202`) and `m_tabBecameVisible` (with its comment, `.hpp:753-754`, and both writes in `Draw`, `.cpp:~1911-1915` and `~1945-1949`, with their comments). `SyncCenterTabFocus` was their only reader, and the App step deletes it.
- **Comments.**
  - The header's "SEAM" comment now points at the next phase's node page.
  - Rewrite the gesture-order comment at `ShaderEditorDocument.hpp:755-763`: the page body now draws AFTER the document (the Inspector phase), so `Draw`'s guard is no longer last in the frame; `Draw`'s guard plus the body's own guard still close every gesture through the ownership and abandonment rules.
  - The mesh hint text `"author baseColor / albedo in the Material panel"` (`.cpp:~2022`) and the one-column comment at `.cpp:~1950-1956` say "the Inspector's material page" instead of the Material panel.

App:
- Delete the `DrawMaterialPanel(ResolveActiveMaterialDoc())` call (~`EditorAppFrame.cpp:2415`) and the "The Material panel draws BEFORE the documents" comment above it (~:2407).
- Delete `ResolveActiveMaterialDoc`: its declaration (`EditorApp.hpp:348`) and definition (`EditorAppFrame.cpp:3229-3250`).
- Delete `EditorApp::SyncCenterTabFocus` ENTIRELY: its definition and phase-16b doc comment (`EditorAppFrame.cpp:~3484-3550`), its declaration and comment (`EditorApp.hpp:351-355`), and its call (`EditorAppFrame.cpp:468`, `SyncCenterTabFocus(fs);`). That removes all three `SelectDockTab` calls it made (`"Material"` at :3530, `"Inspector"` at :3536 and :3547). Its surviving branch (the viewport tab appearing -> select the Inspector tab) existed only because the Material window shared the Inspector's dock node; with Material gone it has no job. A deliberate loss: a user who docks another window into the Inspector's node no longer gets the Inspector tab raised when the viewport tab appears.
- Delete the `m_activeMaterialGuid`/`m_materialDocCount` members, their comments and the "---- Material panel: which document it shows ----" section header (`EditorApp.hpp:~2209-2226`).
- Grep `ArcaneEditor/src ArcaneTests/src` for `SyncCenterTabFocus|ResolveActiveMaterialDoc|m_activeMaterialGuid|m_materialDocCount|TabBecameVisible|vp.appearing|DrawMaterialPanel|DrawMaterialWindow` afterwards: expect no live hit.

`InspectorSaveTarget` already maps a document source to its document, so Ctrl+S in the page saves the material. Confirm it with a grep and do not change it.

- [ ] **Step 4: Run the tests**

Run: build Debug, `ArcaneTests.exe "[inspector]"`, then the shader document test's tag, then `"~[gpu]"`.
Expected: PASS.

- [ ] **Step 5: Desk check (windowed, the user)**

Open a material:
- its page shows in the main Inspector (All but Assets), with the preview and params;
- dragging a param undoes as one step;
- Ctrl+click a param into text entry, type a value, then click the canvas: exactly ONE undo step (the gesture closes across the Inspector/document draw order);
- the preview/params split drags;
- clicking the canvas background after selecting an entity brings the material page back;
- switching tabs between two material documents does NOT move the Inspector;
- Ctrl+S with the Inspector focused saves the material;
- no "Material" tab appears anywhere.

- [ ] **Step 6: Commit**

```bash
git add -A ArcaneEditor/src ArcaneTests/src
git commit -m "feat(editor): the Material window retires -- ShaderEditorDocument is an Inspector source (kind material) whose page is the preview + params; opened = selected, a content click re-selects, tab switches never do (inspector filters s6a)"
```

---

### Task 9: The Sprite and Mesh pages

**Files:**
- Modify: `ArcaneEditor/src/Documents/SpriteDocument.hpp/.cpp`
- Modify: `ArcaneEditor/src/Documents/MeshDocument.hpp/.cpp`
- Test: the existing sprite/mesh document tests. Grep `ArcaneTests/src` for `SpriteDocument` / `MeshDocument` and extend the file that constructs each one.

**Interfaces:**
- Consumes: `DocumentPageSelection` (Task 8).
- Produces:
  - `SpriteDocument`: `Kind()` returns `"sprite"`, key `"sprite"`, and a private `SpriteInspectorPage` whose `Draw` calls `DrawFormBody()`.
  - `MeshDocument`: `Kind()` returns `"mesh"`, key `"mesh"`, and a private `MeshInspectorPage` whose `Draw` calls `DrawFormBody()`.
  - Each page's breadcrumb is one crumb, `{ <document title>, no-op, "<key>" }`.

- [ ] **Step 1: Write the failing tests**

For each document, in its existing test file:
```cpp
    CHECK(doc.Kind() == "sprite");                 // "mesh" for MeshDocument
    CHECK(doc.SelectionKey() == "sprite");
    CHECK(doc.SelectionEpoch() == 1);
    REQUIRE(doc.Page() != nullptr);
    CHECK(doc.Page()->Breadcrumb()[0].key == std::optional<std::string>{ "sprite" });
```
Also add a device-less draw case, in the `InputActionsDocumentUiTest.cpp:75-92` harness shape: each frame draws the document (`doc.Draw(close)`), then pins the Inspector window (`SetNextWindowPos/Size(..., ImGuiCond_Always)`, as :82-83) and draws the page into `Begin("Inspector")`. Prove the form's first widget is submitted in THAT window through the ACTIVE id after a press (`ImGuiWindow::GetID` only hashes a label, and `LastItemData` is restored to the parent's at `End()`, `imgui.cpp:8849`, so neither shows a submission):
```cpp
    // (h = the harness; one warm-up Frame() first so the windows exist)
    ImGuiWindow* iw = ImGui::FindWindowByName("Inspector");
    REQUIRE(iw != nullptr);
    const ImVec2 row(iw->ContentRegionRect.Min.x + 10.0f,
                     iw->ContentRegionRect.Min.y + ImGui::GetFrameHeight() * 0.5f);   // the page's first row
    h.Move(row);
    h.Button(ImGuiMouseButton_Left, true);
    CHECK(ImGui::GetActiveID() == iw->GetID("Pixels Per Meter"));      // sprite; mesh: its first form widget's label
    CHECK(ImGui::GetCurrentContext()->ActiveIdWindow == iw);           // in the Inspector, not the document window
    h.Button(ImGuiMouseButton_Left, false);
```
- sprite: `"Pixels Per Meter"` (`SpriteDocument.cpp:258`, the form's first widget);
- mesh: the first widget `DrawFormBody` submits (read its label from `MeshDocument.cpp`'s source section).

- [ ] **Step 2: Run the tests to verify they fail**

Expected: `Kind()` returns `""`.

- [ ] **Step 3: Implement**

For each document:
- **Split the form.**
  - Sprite (it has NO preview, `SpriteDocument.hpp:53-56`): `DrawFormBody()` = `SpriteDocument.cpp:192-288`: the clamp-policy comment, the `bracket` lambda, the four drags, `if (changed) m_dirty = true;`, and the `Separator()` + read-only "Texture" line with its comment.
  - Mesh: move everything in `Draw` after `##meshpreview`'s `EndChild()` + `Separator()`, through the last form widget, into `void DrawFormBody();`. `DrawFormBody` recomputes `const bool imported = (m_data.source == Arcane::MeshSource::Imported);` as a local: the form's source section reads it (`MeshDocument.cpp:551`) but it is declared in the preview block that stays in `Draw` (:456). `Draw` keeps its own copy for the preview branch.
  - Move the `bracket`/`commit` lambdas along with the form.
  - Give `DrawFormBody` its OWN `EditGesture::ScopeGuard gestureGuard{ m_services.undo, m_gesture };` as its first local, because it now runs inside an Inspector window's Begin/End. The precedent is `DrawMaterialWindow`, which already held a second guard on the document's gesture.
- **What stays in `Draw`:** the document's toolbar (Save button, "(saved)"/"(unsaved)"; spec s6a: a document keeps its toolbar and preview), then:
  - sprite: the `Separator()` + "NO TEXTURE PREVIEW HERE" comment + `TextDisabled("(no texture)")` placeholder (`SpriteDocument.cpp:290-297`);
  - mesh: the preview;
  - and `m_pageSel.NoteContentClick();` before `End()`.
- **Ctrl+S.** The document keeps its own `ImGui::Shortcut(Ctrl+S)`.
- **Source overrides.** Add `DocumentPageSelection m_pageSel{ "sprite" }` (`"mesh"`) and the overrides, exactly as Task 8 does for materials.
- **Preview reads.** Check every preview read that USED a form-local value, and every form read of a preview-block local (the mesh's `imported`, above). Mesh's `m_validationReason` is computed from `m_data`, not from the form, so it should be unaffected. Any such dependency must read document state, never a local that moved.

- [ ] **Step 4: Run the tests**

Run: build, the two documents' test tags, then `"~[gpu]"`.
Expected: PASS.

- [ ] **Step 5: Desk check (windowed, the user)**

- Open a sprite: its form (including the read-only Texture line) is in the Inspector, and the document window shows only the toolbar and the "(no texture)" placeholder.
- A sprite pivot drag edits the sprite and marks the tab "(unsaved)"; one Ctrl+Z undoes it; Save shows it in the viewport (the sprite republishes on Save and undo/redo only, never live).
- A click in the sprite document's content area (the "(no texture)" region) re-selects the page after an entity click.
- Open a mesh: its form is in the Inspector; the document shows the toolbar and the preview; topology edits update the preview live and undo as one step; a click in the preview re-selects the page after an entity click.

- [ ] **Step 6: Commit**

```bash
git add -A ArcaneEditor/src ArcaneTests/src
git commit -m "feat(editor): Sprite and Mesh forms move into Inspector pages (kinds sprite/mesh); the documents keep their toolbar (+ the mesh preview) (inspector filters s6a)"
```

---

### Task 10: Automation. Report `instances[].excluded`, `--select-asset`, re-author the seed, goldens

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/VerifyReport.hpp/.cpp` (schemaVersion 12, `SetInspector(source, breadcrumb, instances)`)
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.hpp/.cpp` (`--select-asset <guid>`, editor only, beside `--open-asset`)
- Modify: `ArcaneRuntime/src/main.cpp` (refuse `--select-asset` with exit 2, like the other editor-only flags at :152-182)
- Modify: `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp` (`kGamePluginABIVersion` 45 -> 46 + history entry) and `ReferenceProject/ReferenceProject.arcproj` (`"abi": 46`)
- Modify: `ArcaneEditor/src/App/EditorApp.cpp` (apply `--select-asset` at boot beside `--open-asset` ~line 1300; snapshot the instances before `CloseAll` ~line 2872; pass them to `SetInspector`)
- Modify: `ReferenceProject/Saved/verify-layout.ini` (re-authored with `--dump-layout`)
- Modify: `scripts/golden-gate.ps1` (`$script:ReportSchemaMax = 12` + its history comment; TWO new lane pairs, `editor-asset-page` and `editor-material-page`; "Ten combinations" in the header (line 11) and "ten today" (line 310) become fourteen)
- Modify: `ArcaneTests/src/EditorWitnessTest.cpp` (E3 asserts `instances`; new E4 asset-page and E5 document-page witnesses)
- Modify: `ArcaneTests/src/VerifyReportTest.cpp` (the schema-11 pins become 12), `ArcaneTests/src/HostConfigTest.cpp` (a `--select-asset` round-trip case)
- Re-bless: `editor-ui`, `editor-ui-perspective`, `editor-input-doc` (both backends), plus the new `editor-asset-page` and `editor-material-page`, per the procedure in memory "Golden RE-BLESS: bless the STAGED slot, copy to source IMMEDIATELY", and delete the exe-dir `imgui.ini` before any golden run.

**Interfaces:**
- Produces: report JSON `inspector.instances: [ { "id": 0, "excluded": ["assets"], "source": "Scene" }, ... ]` and the `--select-asset <guid>` flag. `source` is the routed source's `SourceName()`, or `""` when null.

- [ ] **Step 1: Write the failing witness assertions**

In `EditorWitnessTest.cpp` E3 (after the existing breadcrumb check):
```cpp
    REQUIRE(run.report["inspector"].contains("instances"));
    const auto& inst = run.report["inspector"]["instances"];
    REQUIRE(inst.size() == 2);
    CHECK(inst[0].at("id") == 0);
    CHECK(inst[0].at("excluded") == nlohmann::json::array({ "assets" }));
    CHECK(inst[1].at("id") == 1);
    CHECK(inst[1].at("excluded") == nlohmann::json::array({ "scene", "input-actions", "material", "sprite", "mesh" }));
```
Add a new E4 case on E3's harness, with `--select-asset` of `ReferenceProject/Content/textures/uv_marker.png` (its `.meta` guid; the project's only `.png`) and its OWN golden slot:
```cpp
// E4: the ASSET PAGE witness (inspector filters s6). --select-asset selects
// uv_marker.png in the Asset Browser the way a click does; the Assets-only
// Inspector shows its page while the main (All but Assets) one stays on the scene.
TEST_CASE("E4: a selected asset routes the Assets Inspector to its page and matches the editor-asset-page golden", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e4-asset-page");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneEditor.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "dx12", "--frames", "90",
                 "--settle", "10", "--report", inv.reportPath.generic_string(),
                 "--select-asset", "d7f389fd-f687-407d-b9d7-9753eb6b0258",   // textures/uv_marker.png
                 "--compare", "editor-asset-page" };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.contains("inspector"));
    CHECK(run.report["inspector"].at("source") == "Assets");                  // Current(): the asset selection
    REQUIRE(run.report["inspector"].contains("instances"));
    CHECK(run.report["inspector"]["instances"][0].at("source") == "Scene");   // All but Assets: untouched
    CHECK(run.report["inspector"]["instances"][1].at("source") == "Assets");
    REQUIRE(run.report.contains("compare"));
    CHECK(run.report["compare"].at("passed") == true);
}
```
Add E5 on the same harness: one run per document kind with `--open-asset`. Only the material run compares (against `editor-material-page`); no sprite or mesh golden exists, so those runs pass no `--compare` and assert no `compare`. The titles are each file's `name` field (`Title()` = `name`, else the stem):
```cpp
// E5: DOCUMENT PAGES (inspector filters s6a). Opening a material, a sprite or a
// mesh selects that document's page: the main Inspector routes to it, and the
// Assets-only Inspector is untouched.
TEST_CASE("E5: opening a material, a sprite or a mesh routes the main Inspector to that document's page", "[witness][gpu]")
{
    struct Doc { const char* guid; const char* title; const char* compare; };
    const Doc docs[] = {
        { "7e5a0010-0010-4010-8010-000000000010", "ReferenceCubeMaterial", "editor-material-page" },   // materials/reference_mesh.arcmat
        { "87bd4fd3-c9e6-4fc8-a8e0-cf799378f049", "UvMarkerSprite",        nullptr },                  // sprites/uv_marker.arcsprite
        { "7e5a0011-0011-4011-8011-000000000011", "ReferenceCube",         nullptr },                  // meshes/reference_cube.arcmesh
    };
    for (const Doc& d : docs)
    {
        DYNAMIC_SECTION(d.title)
        {
            WitnessScratch scratch(StagedEditorDir(), std::string("e5-") + d.title);
            WitnessInvocation inv;
            inv.exePath = scratch.Dir() / "ArcaneEditor.exe"; inv.workingDir = scratch.Dir();
            inv.reportPath = scratch.Dir() / "witness-report.json";
            inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "dx12", "--frames", "90",
                         "--settle", "10", "--report", inv.reportPath.generic_string(),
                         "--open-asset", d.guid };
            if (d.compare) { inv.args.push_back("--compare"); inv.args.push_back(d.compare); }
            inv.hardCapMs = 180000;
            WitnessRun run = RunWitness(inv);
            INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
            REQUIRE_FALSE(GradeProcessFacts(run).has_value());
            REQUIRE(run.exitCode == 0);
            REQUIRE(run.report.contains("inspector"));
            REQUIRE(run.report["inspector"].contains("instances"));
            CHECK(run.report["inspector"]["instances"][0].at("source") == d.title);
            CHECK(run.report["inspector"]["instances"][1].at("source") == "Assets");   // untouched by a document open
            if (d.compare)
            {
                REQUIRE(run.report.contains("compare"));
                CHECK(run.report["compare"].at("passed") == true);
            }
        }
    }
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `ArcaneTests.exe "[witness]" -# ` using the E3/E4/E5 names (witnesses need the built editor and run from the exe dir).
Expected: FAIL (no `instances`, unknown flag; the E4/E5-material compares stay red until Step 4 blesses their slots).

- [ ] **Step 3: Implement**

`VerifyReport`:
```cpp
        struct InspectorInstance { int id = 0; std::vector<std::string> excluded; std::string source; };
        void SetInspector(std::string source, std::string breadcrumb, std::vector<InspectorInstance> instances = {});
```
- **Serialization.** Serialize `"instances"` as an array of `{id, excluded, source}`, always present when `SetInspector` ran.
- **Schema.** Bump `kSchemaVersion` to 12. Extend the schema-history comment: "12: inspector.instances (inspector filters)".
- **Schema consumers.** `kOldestSupportedSchemaVersion` stays 3. Two consumers pin 11 by name, and a `schemaVersion.*11` grep misses the first:
  - `scripts/golden-gate.ps1:404`: `$script:ReportSchemaMax = 11` becomes `12`, and its history comment (:392-402) gains "12 since the inspector-filters arc added `inspector.instances` [{id, excluded, source}]". Without it every lane grades red ("outside this gate's supported range", :1071-1074).
  - `ArcaneTests/src/VerifyReportTest.cpp`: every `== 11` schema pin becomes `== 12` (today :75, :736, :760, :857, :967, :985, :1021, :1042, :1070, :1112, :1158); the test name at :962 says "schemaVersion is 12"; add `CHECK(IsSupportedSchemaVersion(12))` and turn the `CHECK_FALSE(IsSupportedSchemaVersion(12))` into `CHECK_FALSE(IsSupportedSchemaVersion(13))` (11 stays supported).
- **ABI (unconditional).** No build or boot check detects a `HostConfig`/`VerifyReport` layout change: the gate is the hand-maintained `kGamePluginABIVersion`, and v45 bumped for exactly this kind of change. Bump it to 46 in `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp` with a history entry after v45's:
  ```cpp
    // v46 (2026-09-29, inspector filters): `HostConfig` gained `selectAsset`
    //     (--select-asset), and `VerifyReport` gained `InspectorInstance` /
    //     `m_inspectorInstances` behind a new `SetInspector(source, breadcrumb,
    //     instances)` signature (report schemaVersion 12) -- both layouts moved.
    //     ReferenceProject.arcproj restamped; the Aphelyon restamp is owed.
    inline constexpr uint32_t kGamePluginABIVersion = 46;
  ```
  Restamp `ReferenceProject/ReferenceProject.arcproj` (`"abi": 45` -> `46`). The Aphelyon `Game/Aphelyon.arcproj` restamp is OWED (another repo): record it in the task report.

`HostConfig`:
- Add `cli.Option("select-asset", "", "editor only: select this asset guid in the Asset Browser at boot (its page shows in the Assets Inspector)");`.
- Add `cfg.selectAsset = r.Get("select-asset");`.
- Add the field `std::string selectAsset;` beside `openAsset`.
- `HostConfigTest.cpp`: add a "host config: --select-asset round-trips and defaults empty" case, the `--open-asset` case at :148 with the flag and field swapped (guid `d7f389fd-f687-407d-b9d7-9753eb6b0258`).

`ArcaneRuntime/src/main.cpp`, after the `--select-in-document` refusal (~:177-182):
```cpp
    if (!parsed.config->selectAsset.empty())
    {
        std::fprintf(stderr, "error: --select-asset is an EDITOR-only flag (this host has no "
                             "Asset Browser). Use ArcaneEditor.exe.\n");
        return 2;
    }
```

`EditorApp.cpp` boot (beside `--open-asset`, same loudness rule):
```cpp
        // --select-asset (inspector filters s6): a scripted Asset Browser
        // selection. Validated through the PROJECT, the way OpenAssetDocument
        // does (EditorAppProject.cpp:182-192): the model is not rebuilt until
        // the first frame's DrawEditorUi, so its Find() misses every guid here.
        if (!m_config.selectAsset.empty())
        {
            const auto guid = Arcane::Guid::FromString(m_config.selectAsset);
            const Arcane::Project* p = m_runtime ? m_runtime->CurrentProject() : nullptr;
            if (!guid || !p || !p->ResolveAsset(Arcane::AssetId::FromGuid(*guid)))
                ARC_ERROR("--select-asset '{}': not a Guid or not in the project", m_config.selectAsset);
            else
                m_assetModel.Select(*guid);           // the asset edge routes it next frame, like a click
        }
```
Report snapshot (before `m_documents.CloseAll()`):
```cpp
        std::vector<Arcane::VerifyReport::InspectorInstance> inspectorInstances;
        for (const auto& i : m_inspectorHost.Instances())
        {
            Arcane::Editor::InspectorSource* s = m_inspectorHost.SourceFor(i.id);
            inspectorInstances.push_back({ i.id, i.filter.excluded, s ? s->SourceName() : std::string{} });
        }
```
Pass it as `SetInspector`'s third argument. Use the real namespace of `VerifyReport` (grep its header).

- [ ] **Step 4: Re-author the seed, add the lane, re-bless**

1. Build Debug and Release (both hosts the gate runs).
2. Re-author the seed:
   ```
   del bin\Debug-windows-x86_64-md\ArcaneEditor\imgui.ini
   bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project ReferenceProject --headless --frames 90 --dump-layout %TEMP%\seed.ini
   ```
   Dump with the OLD seed IN PLACE. This deliberately deviates from the seed header's own "TO REGENERATE" steps (which say to move the file aside first): `--headless` pins the old seed, which has no `Filters=`, so this run exercises the legacy upgrade. Check that `%TEMP%\seed.ini` now carries:
   - `[EditorInspector][Instances]` with `Ids=1` and `Filters=0:assets,1:scene+input-actions+material+sprite+mesh`;
   - `[Window][Inspector]`, still docked where the seed's Inspector was (`ImHashStr` skips `###`, so `"...###Inspector"` reuses the seed's entry by id);
   - `[Window][inspector_1]`, docked into the new split right of the Asset Browser. `CreateNewWindowSettings` names an entry by the text after `###` (`imgui.cpp:16416`), never by the full title.

   Build the new seed: keep the file's hand-written header comment block (lines 1-71) and paste the dumped body under it. Delete the stale `[Window][Material]` section by hand (unclaimed entries re-emit verbatim, and Task 8 retired that window). Add a paragraph at the end of the header recording this regen: date, "dumped with the old seed in place to exercise the one-time legacy Inspector upgrade (inspector filters s6), deviating from TO REGENERATE", and the hand-deleted `[Window][Material]`. Copy the result over `ReferenceProject/Saved/verify-layout.ini`.
3. Add TWO lane pairs to `scripts/golden-gate.ps1`, after `editor-input-doc`, then update the header list, "Ten combinations" (line 11) and "ten today" (line 310) to fourteen:
   ```powershell
   @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-asset-page';    Backend = 'dx12';   ExpectedLevel = 'shared'; ExtraArgs = @('--select-asset', 'd7f389fd-f687-407d-b9d7-9753eb6b0258'); SelfTestExpect = 'Failed' }
   @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-asset-page';    Backend = 'vulkan'; ExpectedLevel = 'shared'; ExtraArgs = @('--select-asset', 'd7f389fd-f687-407d-b9d7-9753eb6b0258'); SelfTestExpect = 'Failed' }
   @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-material-page'; Backend = 'dx12';   ExpectedLevel = 'shared'; ExtraArgs = @('--open-asset', '7e5a0010-0010-4010-8010-000000000010'); SelfTestExpect = 'Green' }
   @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-material-page'; Backend = 'vulkan'; ExpectedLevel = 'shared'; ExtraArgs = @('--open-asset', '7e5a0010-0010-4010-8010-000000000010'); SelfTestExpect = 'Green' }
   ```
   (`d7f389fd-...` = `textures/uv_marker.png`; `7e5a0010-...` = `materials/reference_mesh.arcmat`.) `editor-material-page` is `'Green'` because the document tab covers the viewport, as with input-doc.
   `SelfTestExpect 'Failed'`: the viewport is visible, so the self-test's boot-scene mutation shows. Verify with `-SelfTest` and flip it if wrong, as the input-doc lane's comment describes.
4. Re-bless `editor-ui`, `editor-ui-perspective`, `editor-input-doc` and the new `editor-asset-page` and `editor-material-page` on both backends with the staged-slot procedure. LOOK at every new image before copying it to source. Expected differences:
   - editor-ui: no preview pane in the Asset Browser, an "Inspector 2 - Assets" pane on its right, and the main Inspector titled "Inspector - All but Assets";
   - editor-asset-page: the Assets Inspector showing the texture's page (thumbnail, rows, buttons, import settings) while the main Inspector still shows "No selection".
   - editor-material-page: the material document in the center, its preview + params in the main Inspector, no "Material" tab.
5. Run the full gate Debug and Release: `powershell -File scripts\golden-gate.ps1` (see its header for the config switch).
   - Expected: Debug 14/14.
   - Expected: Release 13/14 at worst, where the only permitted red is the known cook-counter race on the Release dx12 editor-ui status-bar count. Any other red is a failure to fix.

- [ ] **Step 5: Run the full suites**

Run from the exe dir: `ArcaneTests.exe "~[gpu]"`, `ArcaneTests.exe "[gpu]~[witness]"`, `ArcaneTests.exe "[witness][gpu]"`.
Expected: all green except documented pre-existing skips (the usual 4).

- [ ] **Step 6: Commit**

```bash
git add -A ArcaneClient/src ArcaneEditor/src ArcaneTests/src ArcaneRuntime/src/main.cpp ArcaneCore/src/Arcane/Plugin/PluginABI.hpp ReferenceProject/ReferenceProject.arcproj ReferenceProject/Saved/verify-layout.ini scripts/golden-gate.ps1 ReferenceProject/Saved/Verify
git commit -m "test(editor): report inspector.instances (schema 12), --select-asset, re-authored verify seed with the two-inspector layout, editor-asset-page + editor-material-page golden lanes, document-page witnesses; plugin ABI 46 (ReferenceProject restamped; Aphelyon restamp owed); editor goldens re-blessed (inspector filters s9)"
```
(Stage the golden reference images from wherever the gate keeps source references. Grep `golden-gate.ps1` for the reference root, and never stage `bin/`.)

---

### Task 11: Spec and doc close-out

**Files:**
- Modify: `docs/superpowers/specs/2026-09-29-inspector-filters-design.md` (Status: "Implemented <date>, branch feat/inspector-filters"; §9 lists the witnesses/lanes actually added)
- Keep the header's existing next-phase line ("A full shader NODE page is the next phase, right after this one.", spec lines 9-10; s6a / decision 9) and fold it into the new Implemented status line. Do not add a duplicate.
- Record what shipped where it differs from the spec text:
  - s6a "Retired" bullet: after "(`SelectDockTab("Material")` / `SelectDockTab("Inspector")` on a material tab becoming visible)" add: "-- in fact all of `EditorApp::SyncCenterTabFocus`, including its viewport-appearing -> `SelectDockTab("Inspector")` branch and `ViewportPanelResult::appearing`. That branch existed only because Material shared the Inspector's dock node. Accepted loss: a window the user docks into the Inspector's node no longer gets the Inspector tab raised when the viewport tab appears."
  - s3, after the one selection rule: "Scoped exception: the Asset Graph lens mirrors its persistent canvas selection into the model only on change, so re-clicking the already-selected Graph node is not a selection event; every other asset click site (Browser, Status, feed, the Browser row context menu on its first frame) is."

- [ ] **Step 1: Grep sweep for stale references**

Run: `grep -rn "preview pane\|previewPaneWidth\|kAssetsPreviewPaneDefaultWidth\|selectedAsset\|CanPin()\|DrawMaterialPanel\|DrawMaterialWindow\|ResolveActiveMaterialDoc\|\"Material\" panel\|Material tab" ArcaneEditor ArcaneTests docs/superpowers/specs`
Expected: only historical mentions in older specs/plans, which stay as history, and no live-code hit. COMMENT hits in files no task touched may remain (at least 14 today, e.g. "the preview pane's New Instance..." in `EditorApp.hpp:343` and the "Material panel" mentions in `AssetBrowserPanel.cpp:990` / `.hpp:59` and `EditorPanels.hpp:583`): rewrite each one whose statement is now false, leave true history alone. Fix any live one.

- [ ] **Step 2: Commit**

```bash
git add -A docs ArcaneEditor/src ArcaneTests/src
git commit -m "docs(editor): inspector filters spec marked implemented; stale preview-pane references swept"
```

---

## Self-review notes (plan author)

- **Spec coverage.** Each spec section maps to a task:

  | Spec section | Task |
  |---|---|
  | s2 kinds | 1, 2 |
  | s3 model: filter, stamps, routing, permanent sources, empty kind, deselect, per-instance pin | 2 |
  | s4 history | 3 |
  | s5 UI | 7 |
  | s6 pane removal | 5, 6 |
  | s6 default layout and legacy upgrade | 4, 7 |
  | s7 persistence and Reset Layout | 4, 7 |
  | s6a material page, Material window retirement | 8 |
  | s6a sprite and mesh pages | 9 |
  | s3 one selection rule for every source | 2 (host), 8 (document-page selection), 9 |
  | s9 verification | every task, plus 10 |

- **The mini-arc 3 consequence is gone.** The material kind ships in this phase, so the default "Assets only" filter excludes it from day one and no saved layout ever holds the old narrower list.
- **Risky spots flagged in their tasks:**
  - instance 0's stable id `###Inspector`, which `ImHashStr` hashes to the legacy bare `Inspector` id, so no re-dock is needed (a Task 7 test pins the equality; Task 10's seed expects `[Window][Inspector]` + `[Window][inspector_1]`);
  - the `DockBuilderSplitNode` direction and the leaf-node check (Task 7, Step 4);
  - the asset page's services pointer: a MEMBER (`m_assetPanelServices`), because `DrawEditorUi`'s local is dead by the Inspector phase (Task 6, Step 2);
  - the asset re-click as a selection event: `AssetPanelModel::selectionGesture` (Task 5), observed by the asset edge (Task 6); the Browser row context menu's per-frame `Select` is gated to the popup's first frame (Task 6), and the Graph lens's change-only mirror is the one scoped exception (Task 5, recorded in the spec by Task 11);
  - `SyncCenterTabFocus` deleted whole (Task 8, ruling R7): the viewport-appearing -> Inspector-tab raise goes with it, a deliberate loss the spec's s6a records at close-out (Task 11);
  - the unconditional plugin ABI bump 45 -> 46 and the report schema 12 consumers (Task 10, Step 3);
  - `NoteContentClick` must see clicks on the node-editor canvas (drawn directly in the document window, no child) and must NOT see a click on the dock tab or the title bar (Task 8, Steps 1 and 5);
  - the sprite document window is nearly empty after Task 9 (toolbar + the "(no texture)" placeholder): a sprite preview is a natural follow-up.
