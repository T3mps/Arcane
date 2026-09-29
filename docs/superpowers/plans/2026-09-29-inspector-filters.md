# Inspector Filters + Asset Page (Editor Mini-Arc 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every Inspector instance carries a checkbox filter over source kinds. The Asset Browser becomes an Inspector source, and its in-panel preview pane is removed. The default layout gets two Inspectors: the main one shows everything but Assets, and "Inspector 2" shows only Assets.

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

- Kind ids (exact strings): `"scene"`, `"input-actions"`, `"assets"`. Display names: `"Scene"`, `"Input Actions"`, `"Assets"`. `"shader"` is NOT added in this arc (mini-arc 3 adds it).
- Filters persist the UNTICKED kinds (exclusions). Empty exclusions = All. A kind added to the catalog later is admitted by every existing filter.
- At least one catalog kind stays ticked. A filter that excludes every catalog kind is refused, and on ini load it is sanitized to All.
- A source whose `Kind()` is empty is admitted only by All.
- Routing order: pinned -> `Current()` if admitted -> highest selection stamp among admitted sources -> fallback (scene) if admitted -> most recently added admitted source -> null.
- A clear (empty key) is never a selection event. It moves no stamp and never moves `Current()`.
- ini line format: `Filters=<id>:<kind>+<kind>,<id>:<kind>`, written AFTER `Ids=`, always written (possibly empty: `Filters=`). A section with no `Filters=` line means the layout predates this feature, and the layout is upgraded ONCE.
- Window titles:
  - instance 0: `"Inspector###Inspector"`, or `"Inspector - <label>###Inspector"` when filtered;
  - instance N >= 1: `"Inspector <N+1>###inspector_<N>"` or `"Inspector <N+1> - <label>###inspector_<N>"`.
  - Instance 0 MOVES from the bare name `"Inspector"` to the stable id `"###Inspector"`. `ImHashStr` resets its crc at `###` but still hashes the `###` characters (`imgui.cpp` ~line 2539), so `ImHashStr("X###Inspector") != ImHashStr("Inspector")`: no ### suffix keeps the old id. The one-time legacy upgrade (Task 7) re-docks `###Inspector` where `Inspector` was docked, so an existing layout keeps its main Inspector position. Every lookup of the primary window by name (`FindWindowByName("Inspector")`, `SelectDockTab("Inspector")`, `DockBuilderDockWindow("Inspector", ...)`) switches to `"###Inspector"` through one constant, `kPrimaryInspectorWindowId`.
- Default layout = exactly instances {0, 1}: 0 = All but Assets (`excluded = {"assets"}`), 1 = Assets only (`excluded = every catalog kind except "assets"`), docked in a split to the RIGHT of the Asset Browser's node.
- Every new `Panels/*.cpp` the tests need must be added to the EXPLICIT `ArcaneTests` file list in `premake5.lua` (near line 1237/1552); the editor project globs, the test project does not. Re-run `generate.bat` (or `ThirdParty\premake5\premake5.exe vs2026`) after any file-list change.
- Build: `msbuild Arcane.slnx /p:Configuration=Debug /p:Platform=x64 -m -nr:false`. Tests: run `bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[inspector]"` FROM ITS OWN DIRECTORY. The final suite gate is `~[gpu]` plus `[gpu]~[witness]` plus the witnesses.
- Never push. Commit per task on branch `feat/inspector-filters` (already created off main `6f8a05e3`).

## Review Focus

1. **The last admitted document closes under a filtered instance.** Example: an "Input Actions only" instance, and the only `.arcinput` closes. Expect the one-line "No Input Actions document open", no dangling pointer, and no crash. Pinned test in Task 2 ("closing the last admitted source").
2. **Project switch with filtered instances.**
   - Filters survive.
   - The asset source stays registered (permanent). Its history entries and pins drop.
   - The Assets instance shows "Nothing selected" because the model's selection is reset.
   - Pinned test in Task 2 ("ReleaseAll keeps a permanent source").
3. **A hand-edited or garbage `Filters=` line.** Cases: an id not in `Ids=`, unknown kinds, all kinds excluded, trailing commas, a missing colon. Expect it sanitized, never a throw, never an all-excluded instance. Pinned test in Task 4.
4. **Changing the filter of a PINNED instance.** Expect the pin to win (the page stays) and the dropdown to stay usable. After unpinning, the instance follows the new filter. Pinned test in Task 2.
5. **The selected asset is deleted or renamed on disk.** `Resolves` becomes false, its history entries are pruned, and the Assets inspector shows "Nothing selected", never a stale page or a crash. Pinned test in Task 5.

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

TEST_CASE("InspectorKinds: the catalog is scene, input-actions, assets in that order", "[editor][inspector]")
{
    REQUIRE(kInspectorKinds.size() == 3);
    CHECK(kInspectorKinds[0].id == "scene");
    CHECK(kInspectorKinds[1].id == "input-actions");
    CHECK(kInspectorKinds[2].id == "assets");
    CHECK(FindInspectorKind("assets")->displayName == "Assets");
    CHECK(FindInspectorKind("shader") == nullptr);
}

TEST_CASE("InspectorFilter: exclusions admit everything else, and an empty kind only under All", "[editor][inspector]")
{
    InspectorFilter all;
    CHECK(all.IsAll());
    CHECK(all.Admits("scene"));
    CHECK(all.Admits(""));                         // a non-selecting document: All only
    CHECK(all.Admits("shader"));                   // a kind this build does not know: admitted

    const InspectorFilter noAssets = InspectorFilter::AllBut("assets");
    CHECK_FALSE(noAssets.IsAll());
    CHECK(noAssets.Admits("scene"));
    CHECK_FALSE(noAssets.Admits("assets"));
    CHECK_FALSE(noAssets.Admits(""));              // filtered: empty kinds are out
    CHECK(noAssets.Admits("shader"));              // a later kind appears (decision 8.3)

    const InspectorFilter onlyAssets = InspectorFilter::Only("assets");
    CHECK(onlyAssets.Admits("assets"));
    CHECK_FALSE(onlyAssets.Admits("scene"));
    CHECK_FALSE(onlyAssets.Admits("input-actions"));
}

TEST_CASE("InspectorFilter: Sanitized drops unknown kinds, duplicates, and an all-excluded set", "[editor][inspector]")
{
    InspectorFilter f;
    f.excluded = { "assets", "bogus", "assets", "scene" };
    CHECK(f.Sanitized().excluded == std::vector<std::string>{ "scene", "assets" });   // catalog order
    f.excluded = { "scene", "input-actions", "assets" };
    CHECK(f.Sanitized().IsAll());                  // nothing left to show: All, never empty
}

TEST_CASE("InspectorFilterLabel: All, one name, All but X, or the ticked list", "[editor][inspector]")
{
    CHECK(InspectorFilterLabel(InspectorFilter{}) == "All");
    CHECK(InspectorFilterLabel(InspectorFilter::Only("scene")) == "Scene");
    CHECK(InspectorFilterLabel(InspectorFilter::AllBut("assets")) == "All but Assets");
    InspectorFilter two;
    two.excluded = { "input-actions" };            // 3 kinds, 1 unticked -> "All but"
    CHECK(InspectorFilterLabel(two) == "All but Input Actions");
    // With 3 catalog kinds, "one ticked" and "one unticked" cover every
    // non-All case; the joined-list branch is exercised by a 2-excluded set
    // whose remainder is ONE kind -- i.e. the "one ticked" rule wins first.
    InspectorFilter one;
    one.excluded = { "scene", "input-actions" };
    CHECK(InspectorFilterLabel(one) == "Assets");
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

    // Catalog order = dropdown order = Sanitized() order. Mini-arc 3 appends "shader".
    inline constexpr std::array<InspectorKind, 3> kInspectorKinds{ {
        { "scene",         "Scene" },
        { "input-actions", "Input Actions" },
        { "assets",        "Assets" },
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
    none.excluded = { "scene", "input-actions", "assets" };
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

`InspectorWindows.cpp`: `const bool canPin = inst.pinned || host.CanPin();` becomes `host.CanPin(inst.id)`. The comment above it keeps its meaning. Replace "CanPin calls PageFor on the current source" with "CanPin calls PageFor on the instance's routed source".

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
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1,3\nFilters=0:assets,1:scene+input-actions\n");
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());
    CHECK(host.Find(0)->filter == InspectorFilter::AllBut("assets"));
    CHECK(host.Find(1)->filter == InspectorFilter::Only("assets"));
    CHECK(host.Find(3)->filter.IsAll());
    const std::string saved = ImGui::SaveIniSettingsToMemory();
    CHECK(saved.find("Ids=1,3\nFilters=0:assets,1:scene+input-actions\n") != std::string::npos);
}

TEST_CASE("InspectorHost ini: garbage Filters= entries are sanitized, never thrown on", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory(
        "[EditorInspector][Instances]\nIds=2\nFilters=7:scene,x:assets,2,0:bogus+assets,,2:scene+input-actions+assets,\n");
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
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=\nFilters=\n");   // empty line: a real answer
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());
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
- **ClearAll.** Extend `handler.ClearAllFn` to also reset instance 0's filter: `host->SetInstanceIds({}); (void)host->SetFilter(0, InspectorFilter{});`.
- **ReadLine.** Parse `Filters=` after the `Ids=` branch:
```cpp
            if (std::strncmp(line, "Filters=", 8) == 0)
            {
                host->NoteFiltersLine();
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
- Modify: `ArcaneEditor/src/Panels/EditorPanels.cpp`. Delete `DrawTextureMetaSettingsBlock` and `DrawTextureAssetPanel` from its anonymous namespace. Their only caller is the scene fallback, which Task 6 removes. Do the deletion in THIS task and have `DrawInspectorBody`'s fallback call nothing yet: temporarily keep the branch but call `DrawTextureImportSettings` so the build stays green until Task 6.
- Modify: `premake5.lua` (ArcaneTests list: `AssetInspectorSource.cpp`, `TextureImportSettings.cpp`, beside `AssetBrowserPanel.cpp` ~line 1555)
- Test: `ArcaneTests/src/AssetInspectorSourceTest.cpp`

**Interfaces:**
- Consumes: `AssetPanelModel` (`selected`, `selectionStamp`, `Select`, `Find`), `AssetPanelServices`, `AssetPanelActions`, `OpenAssetRow`, `DrawAssetPeekTooltip`, `KindIcon`, `KindLabel`, `SubkindPillText`, `CookStateLabel`, `AssetPill`, `EllipsisToWidth` (all existing), `PropertyGrid`.
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
        [[nodiscard]] std::uint64_t SelectionEpoch() const;   // model->selectionStamp (0 unbound)

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
  It draws the `.png`-only note or the four knobs, and writes the `.meta` merge on edit. It is the body of the old `DrawTextureAssetPanel` after its `Separator()` following the preview, plus `DrawTextureMetaSettingsBlock`, moved verbatim with its comments. Use `AssetKindOf`/a local case-insensitive `.png` check in place of `EditorPanels.cpp`'s private `HasExtensionCI`.

- [ ] **Step 1: Write the failing test** (`ArcaneTests/src/AssetInspectorSourceTest.cpp`)

Build the model the way `AssetPanelModelTest.cpp` does. Use a temp dir under `fs::temp_directory_path()`, `WriteFile` two `.png` files and an `.arcmat`, then `Arcane::AssetRegistry::ScanContent`. Then `model.RebuildIfDirty(&registry, providers)`, where the `FakeProviders` shape is copied into this file. Then:
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
    CHECK(src.SelectionEpoch() == model.selectionStamp);
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
    CHECK(src.Page() == nullptr);                           // "Nothing selected", never a stale page
    CHECK(src.PageFor(gBrick.ToString()) == nullptr);
}

TEST_CASE("DrawAssetPage: the Open/Copy Path/Show in Explorer buttons report actions, never act", "[editor][inspector]")
{
    // Device-less ImGui frames, the AssetPanelCommonTest.cpp / EditorInspectorVectorTest.cpp
    // pattern (a bare context, io.DisplaySize set, the font atlas built, NewFrame/Render).
    // Draw the page for gBrick inside a Begin("t"); click "Copy Path" by driving
    // io.AddMousePosEvent/AddMouseButtonEvent at the button's rect (captured through
    // ImGui::GetItemRectMin/Max in a first frame, exactly as those tests do), then:
    CHECK(actions.copyPath == gBrick);
    // and a Texture entry draws the import-settings block (the ".png" note is absent,
    // the "sRGB##texmeta" checkbox exists: ImGui::FindWindowByName("t") + an id probe).
}
```
Write the fixture code in full in the file, not as comments: copy `WriteFile`/`FakeProviders` from `AssetPanelModelTest.cpp:36-110` and the device-less frame helper from `AssetPanelCommonTest.cpp`. The `// ...` lines above mark where that copy goes.

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
        return m_deps.model ? m_deps.model->selectionStamp : 0;
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
- **Moved content.** Move the body of `DrawPreviewPane` (`AssetBrowserPanel.cpp:1071-1328`) here WITHOUT the `BeginChild("##assetspreview")`/`EndChild` and the "No selection" branch. The page only draws for a resolved entry, and the Inspector window is the container.
- **Header layout.** Keep the compact/stacked header breakpoint logic, measured against `ImGui::GetContentRegionAvail().x` in place of the old `width` parameter. Move the constants it uses (`kPreviewThumbSize` as `kAssetPageThumbSize`, `kPreviewCompactHeaderMinWidth`, `kPreviewCompactTextColumnMin`, `kActionButtonHeight`), `ContentRelativePath`, and `DrawDerivedRow` into this file's anonymous namespace. Carry their comments and drop the pane-width history.
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
- Modify: `ArcaneEditor/src/App/EditorApp.hpp` (members)
- Modify: `ArcaneEditor/src/App/EditorApp.cpp` (register the source as permanent beside `RegisterInspectorInstancesSettings`, ~line 569)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (bind after `assetPanelServices` is built ~line 2340; drain the page actions after the three panel consumes ~line 2387; add the selection edge beside the scene's ~line 3693)
- Modify: `ArcaneEditor/src/App/EditorAppScene.cpp`/`EditorAppProject.cpp` (a project switch: `m_assetModel.ResetForProjectSwitch()` already clears `selected`; re-arm the asset edge's `lastEpoch` to the new stamp exactly as `EditorAppScene.cpp:161` does for the scene)
- Modify: `ArcaneEditor/src/Panels/SceneInspectorSource.hpp/.cpp` (drop `Deps::selectedAsset`)
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp/.cpp` (`DrawInspectorBody` loses the `selectedAsset` parameter and the no-selection texture branch: "No selection" only; update its doc comment)
- Modify: `ArcaneEditor/src/Panels/AssetBrowserPanel.hpp/.cpp` (remove the preview pane: `kAssetsPreviewPaneDefaultWidth`, `previewPaneWidth`, `PreviewPaneSplitter`, `ClampPreview*`, `kPreview*` constants, `DrawPreviewPane`, `showPreview`; the table takes the width after the rail; update both header comments)
- Test: `ArcaneTests/src/EditorInspectorHostTest.cpp` (no new host behaviour. The wiring is proven by the witness and golden in Task 8.) Search `ArcaneTests/src` for `previewPaneWidth|kAssetsPreviewPaneDefaultWidth|DrawInspectorBody(` and fix any compile fallout.

**Interfaces:**
- Consumes: `AssetInspectorSource` (Task 5), `InspectorHost::AddSource(.., true)` (Task 2), `SelectionEdge`.
- Produces: `EditorApp::m_assetSource`, `m_assetSelectionEdge`, `m_assetPageActions`.

- [ ] **Step 1: Write the failing check**

This is wiring, and its test is the build plus the existing suites. Before touching code, record the baseline:
```
ArcaneTests.exe "~[gpu]"     (from the exe dir)  -> note the case/pass counts
```

- [ ] **Step 2: Implement**

`EditorApp.hpp`:
- **Declaration order.** Declare the new members BEFORE `m_inspectorHost`. The host holds raw source pointers, so the source must outlive it. Declaration order = construction order, and destruction runs in reverse. Match the existing `m_sceneSource` placement comment.
- **Members:**
```cpp
        Arcane::Editor::AssetInspectorSource m_assetSource;          // permanent Inspector source over m_assetModel.selected
        Arcane::Editor::SelectionEdge        m_assetSelectionEdge;   // Observe(m_assetModel.selectionStamp, key)
        Arcane::Editor::AssetPanelActions    m_assetPageActions;     // the asset page's clicks, drained next frame
```
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
        m_assetSource.Bind({ &m_assetModel, proj, &m_documents, &assetPanelServices, &m_assetPageActions });
```
- **Services lifetime.** `assetPanelServices` is a LOCAL here, but the Inspector draws later in the frame. If that draw is in a different function or scope, `&assetPanelServices` dangles. Make it a member `m_assetPanelServices` assigned at this site instead, and bind `&m_assetPanelServices`. Check the scopes of line ~2340 and line ~3690 before choosing. If both are in one function whose frame outlives the Inspector draw, the local is fine.

Beside the scene edge (~line 3693):
```cpp
        if (m_assetSelectionEdge.Observe(m_assetModel.selectionStamp, m_assetSource.SelectionKey()))
            m_inspectorHost.NotifySelected(m_assetSource);
```
Scene source bind: drop `&m_assetModel.selected` from the `Deps` initializer. Update the comment block above it: the "trailing fallback" sentence goes, and the asset selection now routes as its own source.

`AssetBrowserPanel.cpp`, `DrawAssetBrowserBody`: delete `showPreview`, `drawnWidth` and the splitter/pane calls. `DrawTable(..., /*tableWidth*/ 0.0f)` fills the line. Delete the now-unused helpers and constants, the `previewPaneWidth` field and `kAssetsPreviewPaneDefaultWidth`. Update the file-header and `DrawAssetBrowserBody` doc comments ("the rail + the table; the asset's details are the Assets Inspector's page").

- [ ] **Step 3: Build and run the suites**

Run: build Debug, then `ArcaneTests.exe "~[gpu]"` from the exe dir.
Expected: the same pass count as Step 1 plus Tasks 1-5's new cases, 0 failures.

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
- Modify: `ArcaneEditor/src/Panels/InspectorWindows.cpp`
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp/.cpp` (`BuildDefaultLayout`, `EndDockSpace`)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (consume `EndDockSpace`'s result, ~line 2305)
- Test: `ArcaneTests/src/EditorInspectorHostTest.cpp` (a device-less window test)

**Interfaces:**
- Consumes: Tasks 2-4 host API; `InspectorFilterLabel`; `kInspectorKinds`.
- Produces:
  ```cpp
  // InspectorWindows.hpp (EditorPanels.cpp includes it)
  inline constexpr const char* kPrimaryInspectorWindowId = "###Inspector";   // instance 0's stable ImGui id
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
    // The stable id: every title of instance 0 hashes to the same window id.
    CHECK(ImHashStr("Inspector###Inspector") == ImHashStr("Inspector - Scene###Inspector"));
    CHECK(ImHashStr("Inspector###Inspector") == ImHashStr(kPrimaryInspectorWindowId));
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
  - `EditorAppFrame.cpp`'s two `SelectDockTab("Inspector")` calls (~lines 3536/3547) become `SelectDockTab(kPrimaryInspectorWindowId)`. `SelectDockTab` looks the window up by name.
  - `PanelRegistry.hpp`'s `"Inspector"` entry is the Window-menu label and the visibility ini key, NOT a window name. Leave it.
  - Tests that `Begin("Inspector")` on their own context (`EditorInspectorVectorTest`, `InputActionsDocumentUiTest`, `PropertyGridTest`) draw their own window and are unaffected. `InputActionsDocumentUiTest` drives the real `DrawInspectorWindows` too: if it looks the primary window up by `"Inspector"`, switch it to the constant.
- **Filter dropdown.** Draw it in `DrawHeader`, after the forward arrow and before the breadcrumb child. Reserve its width in `crumbWidth` like the pin's.
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
  ```
  `HeaderActions` gains `std::optional<InspectorFilter> filter;`, and `ApplyHeaderActions` runs `if (a.filter) (void)host.SetFilter(inst.id, *a.filter);`.
- **Filtered arrows.** In `DrawHeader`, the back/forward arrows and their right-click lists use the filter overloads:
  - `host.CanGoBack(inst.filter)`, `host.BackEntry(inst.filter)`;
  - the back list iterates `host.BackIndices(inst.filter)`, and the forward list `host.ForwardIndices(inst.filter)`;
  - `actions.back` calls `host.GoBack(inst.filter)` in `ApplyHeaderActions`. `ApplyHeaderActions` receives `inst` already, so the forward arrow follows the same pattern.
- **Empty states.** In `DrawInspectorWindows`, when `!inst.pinned && src == nullptr`:
  - a single-kind filter draws `ImGui::TextDisabled("No %s document open", displayName)`, using the one admitted kind's name;
  - otherwise it draws `ImGui::TextDisabled("Nothing to show for this filter")`.

  A routed source with a null page keeps today's behaviour: the page-less header, and "No selection" drawn by the page. Check that the asset and scene sources both render "No selection"/"Nothing selected" consistently. The scene body prints "No selection". For a null asset `Page()` nothing draws, so add `else if (src && !page && !inst.pinned) ImGui::TextDisabled("No selection");` to match.

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
- **Docking.** Dock `"Asset Browser"`/`"Console"`/`"Problems"` into `browserNodeId`, then `ImGui::DockBuilderDockWindow(kAssetsInspectorWindowId, assetsInspectorId);`. The right node's `DockBuilderDockWindow("Inspector", rightId)` becomes `DockBuilderDockWindow(kPrimaryInspectorWindowId, rightId)`.
- **Return value.** `EndDockSpace` returns `{ .builtDefault = true }` when it builds.
- **Upgrade branch.** Add it after the build check. Run it only when the layout was NOT just built and `upgradeLegacyInspectorId >= 1`:
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
            // The main Inspector moves from the bare name "Inspector" to the
            // stable id "###Inspector" (a different ImGui id): re-dock it
            // where the old window was, so the upgrade keeps its position.
            ImGuiID oldInspectorDock = 0;
            if (ImGuiWindowSettings* s = ImGui::FindWindowSettingsByID(ImHashStr("Inspector"))) oldInspectorDock = s->DockId;
            if (oldInspectorDock != 0 && ImGui::DockBuilderGetNode(oldInspectorDock) != nullptr)
                ImGui::DockBuilderDockWindow(kPrimaryInspectorWindowId, oldInspectorDock);
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
        const bool legacy = !menuReq.resetLayout && m_inspectorHost.TakeLegacyLayoutUpgrade();
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
  - after restoring the OLD ini and relaunching, the main Inspector keeps its old dock position (the `###Inspector` re-dock) and "Inspector 2 - Assets" sits right of the Asset Browser;
- an asset click leaves the main Inspector's entity page alone;
- restoring an old ini (keep a copy BEFORE deleting) upgrades once, and relaunching does not re-upgrade.

- [ ] **Step 6: Commit**

```bash
git add -A ArcaneEditor/src ArcaneTests/src
git commit -m "feat(editor): Inspector filter dropdown + filtered titles/history/empty states; default layout = All-but-Assets + Assets-only beside the browser; pre-feature layouts upgraded once (inspector filters s5/s6)"
```

---

### Task 8: Automation. Report `instances[].excluded`, `--select-asset`, re-author the seed, goldens

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/VerifyReport.hpp/.cpp` (schemaVersion 12, `SetInspector(source, breadcrumb, instances)`)
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.hpp/.cpp` (`--select-asset <guid>`, editor only, beside `--open-asset`)
- Modify: `ArcaneEditor/src/App/EditorApp.cpp` (apply `--select-asset` at boot beside `--open-asset` ~line 1300; snapshot the instances before `CloseAll` ~line 2872; pass them to `SetInspector`)
- Modify: `ReferenceProject/Saved/verify-layout.ini` (re-authored with `--dump-layout`)
- Modify: `scripts/golden-gate.ps1` (a new lane pair `editor-asset-page`; update the header list to twelve combinations)
- Modify: `ArcaneTests/src/EditorWitnessTest.cpp` (E3 asserts `instances`; a new E4 asset-page witness)
- Re-bless: `editor-ui`, `editor-ui-perspective`, `editor-input-doc` (both backends), plus the new `editor-asset-page`, per the procedure in memory "Golden RE-BLESS: bless the STAGED slot, copy to source IMMEDIATELY", and delete the exe-dir `imgui.ini` before any golden run.

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
    CHECK(inst[1].at("excluded") == nlohmann::json::array({ "scene", "input-actions" }));
```
Add a new E4 case, copying E3's harness exactly and swapping `--open-asset ... --select-in-document ...` for `--select-asset <brick texture guid from ReferenceProject>`. Pick a `.png` under `ReferenceProject/Content` and read its `.meta` guid:
```cpp
    CHECK(run.report["inspector"].at("source") == "Assets");          // Current(): the asset click
    CHECK(run.report["inspector"]["instances"][0].at("source") == "Scene");   // All but Assets: untouched
    CHECK(run.report["inspector"]["instances"][1].at("source") == "Assets");
```

- [ ] **Step 2: Run to verify they fail**

Run: `ArcaneTests.exe "[witness]" -# ` using the E3/E4 names (witnesses need the built editor and run from the exe dir).
Expected: FAIL (no `instances`, unknown flag).

- [ ] **Step 3: Implement**

`VerifyReport`:
```cpp
        struct InspectorInstance { int id = 0; std::vector<std::string> excluded; std::string source; };
        void SetInspector(std::string source, std::string breadcrumb, std::vector<InspectorInstance> instances = {});
```
- **Serialization.** Serialize `"instances"` as an array of `{id, excluded, source}`, always present when `SetInspector` ran.
- **Schema.** Bump `kSchemaVersion` to 12. Extend the schema-history comment: "12: inspector.instances (inspector filters)".
- **Older consumers.** Check `kOldestSupportedSchemaVersion` and any consumer that pins 11: grep `schemaVersion.*11` across `ArcaneTests`/`scripts`.
- **ABI.** `VerifyReport` lives in ArcaneClient. If its layout change trips the engine ABI check at build or boot, bump `engine.abi` per memory "ABI bumps are CHEAP", restamp ReferenceProject, and note that the Aphelyon restamp is owed.

`HostConfig`:
- Add `cli.Option("select-asset", "", "editor only: select this asset guid in the Asset Browser at boot (its page shows in the Assets Inspector)");`.
- Add `cfg.selectAsset = r.Get("select-asset");`.
- Add the field `std::string selectAsset;` beside `openAsset`.

`EditorApp.cpp` boot (beside `--open-asset`, same loudness rule):
```cpp
        if (!m_config.selectAsset.empty())
        {
            const auto guid = Arcane::Guid::FromString(m_config.selectAsset);
            if (!guid || !m_assetModel.Find(*guid))   // the model must be rebuilt first: call RebuildIfDirty here if Find misses on a fresh model
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
   Because `--headless` pins the OLD seed, this run exercises the legacy upgrade: the old seed has no `Filters=`. Check that `%TEMP%\seed.ini` now carries `[EditorInspector][Instances]` with `Ids=1` and `Filters=0:assets,1:scene+input-actions`, plus `[Window][Inspector - All but Assets###Inspector]` and `[Window][Inspector 2 - Assets###inspector_1]` entries (ImGui keys window settings by the full title, and the ID is what matters) docked into the seed's nodes. Copy it over `ReferenceProject/Saved/verify-layout.ini`. Keep the file's hand-written header comment block (lines 1-60 explain the seed), and paste the dumped body under it.
3. Add the lane pair to `scripts/golden-gate.ps1`, after `editor-input-doc`, then update the header list and "Ten combinations" to twelve:
   ```powershell
   @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-asset-page'; Backend = 'dx12';   ExpectedLevel = 'shared'; ExtraArgs = @('--select-asset', '<brick guid>'); SelfTestExpect = 'Failed' }
   @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-asset-page'; Backend = 'vulkan'; ExpectedLevel = 'shared'; ExtraArgs = @('--select-asset', '<brick guid>'); SelfTestExpect = 'Failed' }
   ```
   `SelfTestExpect 'Failed'`: the viewport is visible, so the self-test's boot-scene mutation shows. Verify with `-SelfTest` and flip it if wrong, as the input-doc lane's comment describes.
4. Re-bless `editor-ui`, `editor-ui-perspective`, `editor-input-doc` and the new `editor-asset-page` on both backends with the staged-slot procedure. LOOK at every new image before copying it to source. Expected differences:
   - editor-ui: no preview pane in the Asset Browser, an "Inspector 2 - Assets" pane on its right, and the main Inspector titled "Inspector - All but Assets";
   - editor-asset-page: the Assets Inspector showing the texture's page (thumbnail, rows, buttons, import settings) while the main Inspector still shows "No selection".
5. Run the full gate Debug and Release: `powershell -File scripts\golden-gate.ps1` (see its header for the config switch).
   - Expected: Debug 12/12.
   - Expected: Release 11/12 at worst, where the only permitted red is the known cook-counter race on the Release dx12 editor-ui status-bar count. Any other red is a failure to fix.

- [ ] **Step 5: Run the full suites**

Run from the exe dir: `ArcaneTests.exe "~[gpu]"`, `ArcaneTests.exe "[gpu]~[witness]"`, `ArcaneTests.exe "[witness][gpu]"`.
Expected: all green except documented pre-existing skips (the usual 4).

- [ ] **Step 6: Commit**

```bash
git add -A ArcaneClient/src ArcaneEditor/src ArcaneTests/src ReferenceProject/Saved/verify-layout.ini scripts/golden-gate.ps1 ReferenceProject/Saved/Verify
git commit -m "test(editor): report inspector.instances (schema 12), --select-asset, re-authored verify seed with the two-inspector layout, editor-asset-page golden lanes; editor goldens re-blessed (inspector filters s9)"
```
(Stage the golden reference images from wherever the gate keeps source references. Grep `golden-gate.ps1` for the reference root, and never stage `bin/`.)

---

### Task 9: Spec and doc close-out

**Files:**
- Modify: `docs/superpowers/specs/2026-09-29-inspector-filters-design.md` (Status: "Implemented <date>, branch feat/inspector-filters"; §9 lists the witnesses/lanes actually added)
- Modify: `ArcaneEditor/src/Documents/ShaderEditorDocument.hpp:892` comment. It names `BuildDefaultLayout`'s Inspector/Material right node, so re-read it and fix it if the layout description changed.

- [ ] **Step 1: Grep sweep for stale references**

Run: `grep -rn "preview pane\|previewPaneWidth\|kAssetsPreviewPaneDefaultWidth\|selectedAsset\|CanPin()" ArcaneEditor ArcaneTests docs/superpowers/specs`
Expected: only historical mentions in older specs/plans, which stay as history, and none in live code. Fix any live one.

- [ ] **Step 2: Commit**

```bash
git add -A docs ArcaneEditor/src
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
  | s9 verification | every task, plus 8 |

- **Known consequence for mini-arc 3.** "Assets only" is stored as exclusions (`scene+input-actions`). When mini-arc 3 adds `"shader"`, every saved "Assets only" instance will start admitting Shader. Its title will say "Assets, Shader", so the change stays visible. That is decision 8.3 working as designed, but mini-arc 3's plan must decide whether the DEFAULT Assets instance should also exclude Shader. A one-line upgrade keyed on a `FiltersV=` marker would do it. Carry this into mini-arc 3's brainstorm.
- **Risky spots flagged in their tasks:**
  - instance 0's move to the stable id `###Inspector`, plus its re-dock in the legacy upgrade (Task 7, Steps 3-4);
  - the `DockBuilderSplitNode` direction and the leaf-node check (Task 7, Step 4);
  - the `assetPanelServices` local's lifetime (Task 6, Step 2);
  - ArcaneClient ABI (Task 8, Step 3).
