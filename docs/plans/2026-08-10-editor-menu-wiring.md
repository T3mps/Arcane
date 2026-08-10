# Editor Menu Wiring Pass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Window menu (panel show/hide backed by a scalable registry + Reset Layout) and wire every placeholder menu item: Edit's selection/clipboard ops, File's Exit / Save All / both recents, Assets' Show in Explorer / Copy Path.

**Architecture:** A constexpr panel table (`kPanels`) is the single source of truth for the Window menu, the tab X buttons, ini persistence, and Reset Layout. Menu clicks land in `MenuRequests` and are consumed app-side in `DrawEditorUi`'s existing consume block; structural edits wrap in `ApplyRegistryMutation` (whole-registry memento) exactly like the Outliner's ops. The entity clipboard is two new engine-side ops in `Arcane/Edit` reusing `SceneSerializer`'s reflection walk, with a JSON envelope on the OS clipboard.

**Tech Stack:** C++23, ImGui (docking), nlohmann::json via `<Json.hpp>`, Catch2 (ArcaneTests), premake5/msbuild.

**Spec:** `docs/superpowers/specs/2026-08-10-editor-menu-wiring-design.md` — read it first.

## Global Constraints

- **/MD everywhere** in the Arcane workspace (dynamic CRT). UTF-8 without BOM, ASCII comments, no `/fp:fast`.
- **JSON include is `#include <Json.hpp>`** (the vendored single header) — NEVER `<nlohmann/json.hpp>`. Namespace is still `nlohmann::json`.
- **Engine-side (Arcane.dll) functions** are declared `ARCANE_API` in a header, defined in a `.cpp` (the `EntityOps.hpp/.cpp` pattern).
- **New files require regenerating the solution**: run `GenerateProjects.bat` from `D:\dev\starworks\Gacha\Arcane` after creating any file. `Arcane` and `ArcaneEditor` glob their sources; **ArcaneTests does NOT glob editor sources** — editor `.cpp` files consumed by tests must be added to the ArcaneTests `files` list in `Arcane/premake5.lua` (~:552-664, entries like `"%{wks.location}/ArcaneEditor/src/ConsoleModel.cpp",`). `ArcaneEditor/src` is already on the ArcaneTests include path.
- **Build:** locate MSBuild via `& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\amd64\MSBuild.exe` (use the VS 18 one it prints, NOT whatever is on PATH), then `& "<msbuild>" D:\dev\starworks\Gacha\Arcane\Arcane.slnx /p:Configuration=Debug /m /nologo`.
- **Tests run FROM THE EXE DIR** and GPU is excluded in the dev loop: `cd D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests` then `.\ArcaneTests.exe ~[gpu]`. ArcaneTests runs in RANDOM order — capture the seed banner from any failure. **Never construct a bare `Arcane::Runtime rt;` in a test** — use the `Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());` scoped-pin pattern (see `Arcane/Tests/src/EntityOpsTest.cpp:28-56`).
- **Do not touch** the pre-existing unstaged `EditorApp.cpp` window-title tweak in the working tree (the `EditorTitle` "Arcane | X | Y" change) — commit around it (`git add` specific files, never `git add -A`).
- Commit messages follow the house `feat(editor):` / `test(editor):` style.

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/ArcaneEditor/src/PanelRegistry.hpp` (new, header-only, NO ImGui) | `PanelId`, `PanelInfo`, `kPanels`, `PanelVisibility`, visibility ini-line parse |
| `Arcane/ArcaneEditor/src/SelectionOps.hpp` (new, header-only, NO ImGui) | Select All / Invert collectors over the SceneRoot subtree |
| `Arcane/ArcaneEditor/src/EntityClipboard.hpp` (new, header-only, NO ImGui) | clipboard JSON envelope wrap/parse |
| `Arcane/ArcaneEditor/src/SceneRecents.hpp/.cpp` (new) | per-project scene recents: pure list ops + file IO |
| `Arcane/Arcane/src/Arcane/Edit/EntityOps.hpp/.cpp` | + `SubtreeEntities`, `SerializeSubtrees`, `InstantiateSubtrees` |
| `Arcane/ArcaneEditor/src/EditorPanels.hpp/.cpp` | menu bar (Window menu, wired items), promoted `ApplyStructural`/`BeginRename`/`DeleteSelection`, `bool* open` params, `EndDockSpace(resetLayout)` |
| `Arcane/ArcaneEditor/src/ProblemsPanel.hpp/.cpp`, `AssetBrowser.hpp/.cpp` | `bool* open`; AssetBrowser also gains tracked selection |
| `Arcane/ArcaneEditor/src/DocumentHost.hpp/.cpp` | + `SaveAllDirty()` |
| `Arcane/ArcaneEditor/src/EditorApp.hpp`, `EditorApp.cpp`, `EditorAppFrame.cpp`, `EditorAppScene.cpp`, `EditorAppProject.cpp` | state members, ini handler, consume-block wiring, shortcuts, recents hooks, switch teardown |
| `Arcane/Tests/src/PanelRegistryTest.cpp`, `SelectionOpsTest.cpp`, `EntityClipboardTest.cpp`, `SceneRecentsTest.cpp` (new) | headless suites |
| `Arcane/premake5.lua` | ArcaneTests files list: + `SceneRecents.cpp` |

Tasks 1–3 are Part I (Window menu); 4–11 are Part II (wiring); 12 is the final gate. Tasks 2+ each depend on the previous task's committed state, except: Task 6 (engine clipboard) only needs Task 1's build baseline, and Tasks 8, 9, 10, 11 are mutually independent (all touch the File/Assets menu + consume block, so execute them in order to avoid conflicts, but a reviewer can reject one without invalidating the others).

---

### Task 1: Panel registry + PanelVisibility + ini-line parse (headless)

**Files:**
- Create: `Arcane/ArcaneEditor/src/PanelRegistry.hpp`
- Test: `Arcane/Tests/src/PanelRegistryTest.cpp`

**Interfaces:**
- Consumes: nothing (self-contained header).
- Produces (later tasks rely on these exact names):
  - `enum class Arcane::Editor::PanelId : uint8_t { Viewport, Outliner, Inspector, Assets, Console, Problems, Count }`
  - `struct PanelInfo { PanelId id; const char* name; bool permanent; }`
  - `inline constexpr PanelInfo kPanels[6]`
  - `struct PanelVisibility { std::array<bool, 6> visible; bool IsVisible(PanelId) const; bool* OpenFlag(PanelId); }` (default all-visible)
  - `std::optional<std::pair<PanelId, bool>> ParsePanelVisibilityLine(const char* line)`

- [ ] **Step 1: Write the header**

```cpp
// Arcane/ArcaneEditor/src/PanelRegistry.hpp
#pragma once

// The editor's dockable-panel registry: ONE table drives the Window menu, the
// tab close buttons, ini persistence, and Reset Layout (spec: 2026-08-10
// editor-menu-wiring, Part I). Adding a panel = one enum value + one table
// row + gating its draw site; nothing else changes.
//
// Deliberately ImGui-free so ArcaneTests exercises it headless.

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

namespace Arcane::Editor
{
    enum class PanelId : std::uint8_t
    {
        Viewport, Outliner, Inspector, Assets, Console, Problems,
        Count
    };

    struct PanelInfo
    {
        PanelId     id;
        const char* name;       // ImGui::Begin title AND menu label AND ini key
        bool        permanent;  // true = no X, cannot hide (Viewport only today)
    };

    // Window-menu order. Names must match each panel's ImGui::Begin title
    // exactly (PanelRegistryTest pins the invariants).
    inline constexpr PanelInfo kPanels[] = {
        { PanelId::Viewport,  "Viewport",  true  },
        { PanelId::Outliner,  "Outliner",  false },
        { PanelId::Inspector, "Inspector", false },
        { PanelId::Assets,    "Assets",    false },
        { PanelId::Console,   "Console",   false },
        { PanelId::Problems,  "Problems",  false },
    };
    static_assert(std::size(kPanels) == static_cast<std::size_t>(PanelId::Count));

    struct PanelVisibility
    {
        std::array<bool, static_cast<std::size_t>(PanelId::Count)> visible;

        PanelVisibility() { visible.fill(true); }

        // Permanent panels always report visible, whatever the array says --
        // one lie-proof read for the draw-site gates.
        [[nodiscard]] bool IsVisible(PanelId id) const
        {
            const auto i = static_cast<std::size_t>(id);
            return kPanels[i].permanent || visible[i];
        }

        // The p_open to hand ImGui::Begin: null for permanent panels (no X).
        [[nodiscard]] bool* OpenFlag(PanelId id)
        {
            const auto i = static_cast<std::size_t>(id);
            return kPanels[i].permanent ? nullptr : &visible[i];
        }
    };

    // One "[EditorPanels][Visibility]" ini line, e.g. "Console=0". Name-keyed
    // so reordering or adding panels never breaks an old ini. nullopt for a
    // malformed line, an unknown name, or a permanent panel (never persisted;
    // a hand-edited "Viewport=0" must not smuggle the viewport away).
    [[nodiscard]] inline std::optional<std::pair<PanelId, bool>>
    ParsePanelVisibilityLine(const char* line)
    {
        if (!line)
            return std::nullopt;
        const char* eq = std::strchr(line, '=');
        if (!eq || eq == line)
            return std::nullopt;
        if (eq[1] != '0' && eq[1] != '1')
            return std::nullopt;
        if (eq[2] != '\0')
            return std::nullopt;
        const std::size_t nameLen = static_cast<std::size_t>(eq - line);
        for (const PanelInfo& p : kPanels)
        {
            if (p.permanent)
                continue;
            if (std::strlen(p.name) == nameLen &&
                std::strncmp(p.name, line, nameLen) == 0)
                return std::make_pair(p.id, eq[1] == '1');
        }
        return std::nullopt;
    }
}
```

- [ ] **Step 2: Write the failing tests**

```cpp
// Arcane/Tests/src/PanelRegistryTest.cpp
// Window-menu panel registry invariants + the visibility ini-line parse
// (spec 2026-08-10 editor-menu-wiring, Part I). Headless -- no ImGui.

#include <catch2/catch_test_macros.hpp>

#include "PanelRegistry.hpp"

#include <cstring>
#include <set>
#include <string>

using namespace Arcane::Editor;

TEST_CASE("panel table: entry i has id i, names unique, exactly one permanent",
          "[editor]")
{
    std::set<std::string> names;
    int permanents = 0;
    for (std::size_t i = 0; i < std::size(kPanels); ++i)
    {
        CHECK(static_cast<std::size_t>(kPanels[i].id) == i);
        CHECK(names.insert(kPanels[i].name).second);
        if (kPanels[i].permanent)
            ++permanents;
    }
    CHECK(std::size(kPanels) == static_cast<std::size_t>(PanelId::Count));
    CHECK(permanents == 1);
    CHECK(kPanels[0].id == PanelId::Viewport);
    CHECK(kPanels[0].permanent);
}

TEST_CASE("PanelVisibility defaults all-visible; permanent panel cannot hide",
          "[editor]")
{
    PanelVisibility v;
    for (const PanelInfo& p : kPanels)
        CHECK(v.IsVisible(p.id));

    // The array slot for the permanent panel is ignored by IsVisible and
    // unreachable through OpenFlag.
    v.visible[static_cast<std::size_t>(PanelId::Viewport)] = false;
    CHECK(v.IsVisible(PanelId::Viewport));
    CHECK(v.OpenFlag(PanelId::Viewport) == nullptr);

    // A non-permanent panel round-trips through its OpenFlag (the tab X and
    // the menu checkmark write the same bool).
    bool* console = v.OpenFlag(PanelId::Console);
    REQUIRE(console != nullptr);
    *console = false;
    CHECK(!v.IsVisible(PanelId::Console));
}

TEST_CASE("ParsePanelVisibilityLine: valid lines, junk, and the viewport ban",
          "[editor]")
{
    auto r = ParsePanelVisibilityLine("Console=0");
    REQUIRE(r.has_value());
    CHECK(r->first == PanelId::Console);
    CHECK(r->second == false);

    r = ParsePanelVisibilityLine("Problems=1");
    REQUIRE(r.has_value());
    CHECK(r->first == PanelId::Problems);
    CHECK(r->second == true);

    CHECK(!ParsePanelVisibilityLine(nullptr).has_value());
    CHECK(!ParsePanelVisibilityLine("").has_value());
    CHECK(!ParsePanelVisibilityLine("Console").has_value());
    CHECK(!ParsePanelVisibilityLine("=1").has_value());
    CHECK(!ParsePanelVisibilityLine("Console=2").has_value());
    CHECK(!ParsePanelVisibilityLine("Console=10").has_value());
    CHECK(!ParsePanelVisibilityLine("NoSuchPanel=1").has_value());
    // Permanent panels are never persisted and never parsed back.
    CHECK(!ParsePanelVisibilityLine("Viewport=0").has_value());
}
```

- [ ] **Step 3: Regenerate + build + verify the tests fail before the header exists is moot (header and test land together) — verify they PASS**

```powershell
cd D:\dev\starworks\Gacha\Arcane; .\GenerateProjects.bat
$msbuild = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\amd64\MSBuild.exe
& $msbuild Arcane.slnx /p:Configuration=Debug /m /nologo
cd bin\Debug-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe "[editor]" ~[gpu]
```
Expected: the three new cases PASS (plus all pre-existing `[editor]` cases still green). If anything fails, note the RNG seed banner before rerunning.

- [ ] **Step 4: Commit**

```powershell
cd D:\dev\starworks\Gacha
git add Arcane/ArcaneEditor/src/PanelRegistry.hpp Arcane/Tests/src/PanelRegistryTest.cpp
git commit -m "feat(editor): panel registry + visibility model for the Window menu (headless)"
```

### Task 2: Window menu, draw-site gating, tab X buttons, Reset Layout

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (BeginDockSpace/EndDockSpace decls, MenuRequests, Draw*Panel decls at :130, :215-220, :306-309)
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (menu bar :70-179, EndDockSpace :239-289, panel Begins :483/:1006/:1665)
- Modify: `Arcane/ArcaneEditor/src/ProblemsPanel.hpp:26` + `ProblemsPanel.cpp:30`
- Modify: `Arcane/ArcaneEditor/src/AssetBrowser.hpp:212-214` + `AssetBrowser.cpp:57`
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp` (~:352, new member) + `Arcane/ArcaneEditor/src/EditorAppFrame.cpp` (:1106 call site, :1117 EndDockSpace, :1209/:1263-1264/:1266-1277 gating, :1650-1654 gating)

**Interfaces:**
- Consumes: Task 1's `PanelRegistry.hpp` (`PanelId`, `kPanels`, `PanelVisibility`), existing `SelectDockTab(const char*)` (EditorPanels.hpp/.cpp:214-237).
- Produces:
  - `MenuRequests` grows `bool resetLayout = false;`
  - `BeginDockSpace(Arcane::CommandStack& undo, MenuRequests& requests, bool sceneDirty, bool playing, bool buildingModule, bool hasGameModule, PanelVisibility& panels, const RecentSelection* recents = nullptr);`
  - `EndDockSpace(bool resetLayout = false);`
  - Panel draw functions each gain a trailing `bool* open = nullptr` forwarded to `ImGui::Begin(name, open)`:
    `DrawConsolePanel(ConsoleBuffer&, ConsoleUiState&, bool* open = nullptr)`,
    `DrawProblemsPanel(const DiagnosticStore&, ProblemsUiState&, bool* open = nullptr)`,
    `DrawAssetBrowserPanel(AssetBrowserState&, const Arcane::Project*, DocumentHost&, bool* open = nullptr)`,
    `DrawOutlinerPanel(..., std::uint64_t savedStateId, bool* open = nullptr)`,
    `DrawInspectorPanel(..., const InspectorServices* services = nullptr, bool* open = nullptr)`
  - `EditorApp` member: `Arcane::Editor::PanelVisibility m_panelVis;`

- [ ] **Step 1: Header changes.** In `EditorPanels.hpp`: add `#include "PanelRegistry.hpp"`; add `bool resetLayout = false;   // Window -> Reset Layout (rebuild default dock layout, re-show all)` to `MenuRequests`; change the two signatures (add `PanelVisibility& panels` after `hasGameModule`; `EndDockSpace(bool resetLayout = false)`); append the `bool* open = nullptr` params to the three panel decls it owns. Same param on `ProblemsPanel.hpp:26` and `AssetBrowser.hpp:212`.

- [ ] **Step 2: The Window menu.** In `EditorPanels.cpp`, between the `Assets` menu's `ImGui::EndMenu()` (:156-157) and `if (ImGui::BeginMenu("Build"))` (:158), insert:

```cpp
            if (ImGui::BeginMenu("Window"))
            {
                // One loop over the registry -- the menu can never drift from
                // the panels that exist (PanelRegistry.hpp). Permanent entries
                // (the Viewport) are an always-checked focus action, the UE
                // shape; the rest are checkmark toggles bound to the same
                // bools the tabs' X buttons write.
                for (const PanelInfo& p : kPanels)
                {
                    if (p.permanent)
                    {
                        if (ImGui::MenuItem(p.name, nullptr, true))
                            SelectDockTab(p.name);
                    }
                    else
                    {
                        ImGui::MenuItem(p.name, nullptr,
                                        &panels.visible[static_cast<std::size_t>(p.id)]);
                    }
                }
                ImGui::Separator();
                // Rebuild the stock dock layout (the first-run path, on
                // demand) and re-show everything -- also the standing cure for
                // an old imgui.ini hiding newly shipped panels.
                if (ImGui::MenuItem("Reset Layout"))
                    requests.resetLayout = true;
                ImGui::EndMenu();
            }
```

- [ ] **Step 3: EndDockSpace reset path.** Change `EditorPanels.cpp:239-245` to:

```cpp
    void EndDockSpace(bool resetLayout)
    {
        const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");

        // First run (no saved .ini layout) -- or an explicit Window -> Reset
        // Layout: arrange the default editor layout. Same call, same safe
        // point (DockBuilder mutations before the DockSpace() submission).
        if (resetLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
            BuildDefaultLayout(dockspaceId);
```

- [ ] **Step 4: Panel Begins take the open flag.** Each panel forwards its new param — `EditorPanels.cpp:483` becomes `ImGui::Begin("Console", open);` (signature at :481 gains `bool* open`); same one-line change for Outliner (:1006), Inspector (:1665), Problems (`ProblemsPanel.cpp:30`), Assets (`AssetBrowser.cpp:57`). The Viewport's `Begin` (:775) is untouched. Do NOT add early-outs on Begin's return — current behavior draws collapsed windows' bodies; keep it.

- [ ] **Step 5: App side.** `EditorApp.hpp`: next to `m_outliner` (~:352) add
```cpp
        // Window-menu panel visibility (PanelRegistry.hpp). The menu's
        // checkmarks and the tabs' X buttons write these same bools;
        // persisted by the [EditorPanels][Visibility] ini handler.
        Arcane::Editor::PanelVisibility m_panelVis;
```
`EditorAppFrame.cpp:1106` call site adds `m_panelVis` after `hasGameModule`; `:1117` becomes:
```cpp
        Arcane::Editor::EndDockSpace(menuReq.resetLayout);
        if (menuReq.resetLayout)
            m_panelVis = Arcane::Editor::PanelVisibility{};   // reset re-shows everything
```
Gate the five draw sites on `m_panelVis` (Viewport untouched):
```cpp
        // :1209 -- Assets. Actions default-construct empty when hidden.
        Arcane::Editor::AssetBrowserActions browserActions;
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Assets))
            browserActions = Arcane::Editor::DrawAssetBrowserPanel(
                m_assetBrowser, m_runtime->CurrentProject(), m_documents,
                m_panelVis.OpenFlag(Arcane::Editor::PanelId::Assets));
```
```cpp
        // :1262-1264 -- Console (the capacity sync stays outside the gate).
        if (static_cast<std::size_t>(m_consoleUi.lineCap) != m_console.Capacity())
            m_console.SetCapacity(static_cast<std::size_t>(m_consoleUi.lineCap));
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Console))
            Arcane::Editor::DrawConsolePanel(m_console, m_consoleUi,
                m_panelVis.OpenFlag(Arcane::Editor::PanelId::Console));
```
```cpp
        // :1272-1277 -- Problems.
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Problems))
            if (const std::optional<Arcane::DiagLocator> hit =
                    Arcane::Editor::DrawProblemsPanel(m_diagnostics, m_problemsUi,
                        m_panelVis.OpenFlag(Arcane::Editor::PanelId::Problems)))
                RouteLocator(*hit);
```
```cpp
        // DrawSelectionPanels (:1650-1654) -- Outliner + Inspector. Prune and
        // editMode still run every frame (selection hygiene is not panel UI).
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Outliner))
            Arcane::Editor::DrawOutlinerPanel(m_runtime->Registry(), m_selection,
                                              *m_undo, m_editBinding, m_outliner,
                                              m_scene.SavedStateId(),
                                              m_panelVis.OpenFlag(Arcane::Editor::PanelId::Outliner));
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Inspector))
            Arcane::Editor::DrawInspectorPanel(m_runtime->Registry(), m_selection, *m_undo,
                                               m_editBinding, m_runtime->CurrentProject(),
                                               m_inspector, &m_inspectorServices,
                                               m_panelVis.OpenFlag(Arcane::Editor::PanelId::Inspector));
```

- [ ] **Step 6: Update the BeginDockSpace doc comment** (EditorPanels.hpp:57-70) — add one line: `panels` drives the Window menu's toggles; only Reset Layout goes through `requests` (it must run at EndDockSpace's DockBuilder-safe point).

- [ ] **Step 7: Build + full non-GPU suite**

```powershell
cd D:\dev\starworks\Gacha\Arcane
& $msbuild Arcane.slnx /p:Configuration=Debug /m /nologo
cd bin\Debug-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe ~[gpu]
```
Expected: build clean, suite green (this task has no new unit tests — its behavior is dock/ImGui-bound; the desk-verify list in Task 12 covers it).

- [ ] **Step 8: Commit**

```powershell
cd D:\dev\starworks\Gacha
git add Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp Arcane/ArcaneEditor/src/ProblemsPanel.hpp Arcane/ArcaneEditor/src/ProblemsPanel.cpp Arcane/ArcaneEditor/src/AssetBrowser.hpp Arcane/ArcaneEditor/src/AssetBrowser.cpp Arcane/ArcaneEditor/src/EditorApp.hpp Arcane/ArcaneEditor/src/EditorAppFrame.cpp
git commit -m "feat(editor): Window menu -- panel show/hide toggles, tab X buttons, Reset Layout"
```

---

### Task 3: Panel-visibility ini persistence

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp` (~:299-310, next to the PlayMode handler decls)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (~:92-158, next to the PlayMode handler)

**Interfaces:**
- Consumes: Task 1's `ParsePanelVisibilityLine`, `kPanels`; Task 2's `m_panelVis`; the house pattern at `EditorApp.cpp:92-158` (`[EditorPlayMode][State]`).
- Produces: ini section `[EditorPanels][Visibility]` with one `Name=0|1` line per non-permanent panel; `EditorApp::RegisterPanelVisibilitySettings()`.

- [ ] **Step 1: Declarations.** In `EditorApp.hpp`, directly below the PlayMode handler decls (:305-310), add:
```cpp
        // ImGuiSettingsHandler callbacks for m_panelVis ("[EditorPanels]
        // [Visibility]", one name-keyed line per hideable panel), mirroring
        // the PlayMode handler above. Registered at the same Init site.
        void RegisterPanelVisibilitySettings();
        static void* PanelVisibilitySettingsReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                                     const char* name);
        static void  PanelVisibilitySettingsReadLine(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                                     void* entry, const char* line);
        static void  PanelVisibilitySettingsWriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                                     ImGuiTextBuffer* buf);
```

- [ ] **Step 2: Implementation.** In `EditorApp.cpp`, below the PlayMode handler block (:158), add (`#include "PanelRegistry.hpp"` at the top with the other editor includes):
```cpp
    namespace
    {
        constexpr const char* kPanelsIniType = "EditorPanels";
        constexpr const char* kPanelsIniName = "Visibility";
    }

    void* EditorApp::PanelVisibilitySettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler,
                                                     const char* name)
    {
        return std::strcmp(name, kPanelsIniName) == 0 ? handler->UserData : nullptr;
    }

    void EditorApp::PanelVisibilitySettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*,
                                                    void* entry, const char* line)
    {
        auto* self = static_cast<EditorApp*>(entry);
        // Unknown names and junk are ignored (never trust a hand-edited ini);
        // missing lines keep the default (visible).
        if (const auto parsed = Arcane::Editor::ParsePanelVisibilityLine(line))
            self->m_panelVis.visible[static_cast<std::size_t>(parsed->first)] = parsed->second;
    }

    void EditorApp::PanelVisibilitySettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler,
                                                    ImGuiTextBuffer* buf)
    {
        auto* self = static_cast<EditorApp*>(handler->UserData);
        buf->reserve(buf->size() + 128);
        buf->appendf("[%s][%s]\n", handler->TypeName, kPanelsIniName);
        for (const Arcane::Editor::PanelInfo& p : Arcane::Editor::kPanels)
            if (!p.permanent)
                buf->appendf("%s=%d\n", p.name,
                             self->m_panelVis.visible[static_cast<std::size_t>(p.id)] ? 1 : 0);
        buf->append("\n");
    }

    void EditorApp::RegisterPanelVisibilitySettings()
    {
        if (ImGui::GetCurrentContext() == nullptr ||
            ImGui::FindSettingsHandler(kPanelsIniType) != nullptr)
            return;

        ImGuiSettingsHandler handler;
        handler.TypeName   = kPanelsIniType;
        handler.TypeHash   = ImHashStr(kPanelsIniType);
        handler.UserData   = this;
        handler.ReadOpenFn = &EditorApp::PanelVisibilitySettingsReadOpen;
        handler.ReadLineFn = &EditorApp::PanelVisibilitySettingsReadLine;
        handler.WriteAllFn = &EditorApp::PanelVisibilitySettingsWriteAll;
        ImGui::AddSettingsHandler(&handler);
    }
```

- [ ] **Step 3: Register it.** Grep for the single call site of `RegisterPlayModeSettings()` (it is called once, in the Init path after the ImGui context exists and before the first NewFrame reads the ini) and add `RegisterPanelVisibilitySettings();` on the next line.

- [ ] **Step 4: Build + suite green** (same commands as Task 2 Step 7). The parse logic is already unit-tested (Task 1); the handler itself is ImGui-context-bound — Task 12 desk-verifies the restart round-trip.

- [ ] **Step 5: Commit**

```powershell
git add Arcane/ArcaneEditor/src/EditorApp.hpp Arcane/ArcaneEditor/src/EditorApp.cpp
git commit -m "feat(editor): persist Window-menu panel visibility in imgui.ini"
```

### Task 4: Select All / Deselect All / Invert Selection

**Files:**
- Create: `Arcane/ArcaneEditor/src/SelectionOps.hpp`
- Test: `Arcane/Tests/src/SelectionOpsTest.cpp`
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (MenuRequests + BeginDockSpace signature), `EditorPanels.cpp` (Edit menu :130-132), `EditorAppFrame.cpp` (call site + consume block)

**Interfaces:**
- Consumes: `SelectionContext` (`Clear()`, `AddRange(span, primary)`, `Contains`, `HasSelection()` — SelectionContext.hpp), `Astra::Registry::GetResource<SceneRoot>()`, `GetRelations(e).ForEachDescendant`.
- Produces:
  - `std::vector<Astra::Entity> Arcane::Editor::CollectSceneEntities(Astra::Registry& reg)` — SceneRoot's DESCENDANTS in BFS pre-order (root itself EXCLUDED: selecting it would let Delete/Cut destroy the scene container); empty when no SceneRoot.
  - `std::vector<Astra::Entity> Arcane::Editor::InvertSelectionSet(const std::vector<Astra::Entity>& all, const SelectionContext& sel)` — entries of `all` not in `sel`, order preserved.
  - `MenuRequests` grows `bool selectAll`, `bool deselectAll`, `bool invertSelection` (all `= false`).
  - `BeginDockSpace` gains `bool hasSelection` after `panels` (greys Rename/Delete now, the clipboard items in Task 7).

- [ ] **Step 1: Write the header**

```cpp
// Arcane/ArcaneEditor/src/SelectionOps.hpp
#pragma once

// Edit-menu selection collectors (spec II.A). Registry access stays here, on
// the app side -- SelectionContext deliberately has none. ImGui-free so
// ArcaneTests exercises them headless.

#include "SelectionContext.hpp"

#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <vector>

namespace Arcane::Editor
{
    // Every entity under SceneRoot in scene walk order (BFS pre-order -- NOT
    // the Outliner's current sort, which is panel-local state). The root
    // itself is EXCLUDED: it is the scene container, and a Select All that
    // included it would hand Delete/Cut the whole scene. Empty when the
    // registry has no SceneRoot.
    [[nodiscard]] inline std::vector<Astra::Entity>
    CollectSceneEntities(Astra::Registry& reg)
    {
        std::vector<Astra::Entity> out;
        const Arcane::SceneRoot* root = reg.GetResource<Arcane::SceneRoot>();
        if (!root)
            return out;
        reg.GetRelations(root->entity).ForEachDescendant(
            [&](Astra::Entity e, std::size_t) { out.push_back(e); });
        return out;
    }

    // The entries of `all` not currently selected, order preserved.
    [[nodiscard]] inline std::vector<Astra::Entity>
    InvertSelectionSet(const std::vector<Astra::Entity>& all, const SelectionContext& sel)
    {
        std::vector<Astra::Entity> out;
        out.reserve(all.size());
        for (Astra::Entity e : all)
            if (!sel.Contains(e))
                out.push_back(e);
        return out;
    }
}
```

- [ ] **Step 2: Write the failing tests**

```cpp
// Arcane/Tests/src/SelectionOpsTest.cpp
// Edit-menu selection collectors, headless (spec II.A).

#include <catch2/catch_test_macros.hpp>

#include "SelectionOps.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <memory>

#include "Helpers/TestTypeContext.hpp"

using namespace Arcane;

namespace
{
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };
        World()
        {
            // Same cross-DLL TypeContext pin as EntityOpsTest.cpp -- Edit::
            // ops live in Arcane.dll and must agree on component IDs.
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
            RegisterSceneComponents(reg);
        }
    };
}

TEST_CASE("CollectSceneEntities: descendants only, root excluded, no-root empty",
          "[editor]")
{
    World w;
    CHECK(Editor::CollectSceneEntities(w.reg).empty());   // no SceneRoot yet

    const Astra::Entity root = Scene::CreateEmpty(w.reg); // root + Main Camera
    const Astra::Entity a = Edit::CreateEntityInScene(w.reg, Astra::Entity::Invalid());
    const Astra::Entity b = Edit::CreateEntityInScene(w.reg, a);

    const std::vector<Astra::Entity> all = Editor::CollectSceneEntities(w.reg);
    CHECK(std::find(all.begin(), all.end(), root) == all.end());
    CHECK(std::find(all.begin(), all.end(), a) != all.end());
    CHECK(std::find(all.begin(), all.end(), b) != all.end());
    CHECK(all.size() == 3);   // camera + a + b
    // BFS pre-order: the parent precedes its child.
    CHECK(std::find(all.begin(), all.end(), a) < std::find(all.begin(), all.end(), b));
}

TEST_CASE("InvertSelectionSet drops selected entries, keeps order", "[editor]")
{
    World w;
    Scene::CreateEmpty(w.reg);
    const Astra::Entity a = Edit::CreateEntityInScene(w.reg, Astra::Entity::Invalid());
    const Astra::Entity b = Edit::CreateEntityInScene(w.reg, Astra::Entity::Invalid());

    const std::vector<Astra::Entity> all = Editor::CollectSceneEntities(w.reg);
    Editor::SelectionContext sel;
    sel.Select(a);

    const std::vector<Astra::Entity> inv = Editor::InvertSelectionSet(all, sel);
    CHECK(std::find(inv.begin(), inv.end(), a) == inv.end());
    CHECK(std::find(inv.begin(), inv.end(), b) != inv.end());
    CHECK(inv.size() == all.size() - 1);
}
```

- [ ] **Step 3: Regenerate + build + run `[editor]` — expect the two new cases PASS.** (Header-only + test; ArcaneTests reaches both via its `ArcaneEditor/src` include path — no premake file-list change.)

- [ ] **Step 4: Menu + requests + param.** `MenuRequests` grows the three bools (comment each with its menu item). `BeginDockSpace` signature gains `bool hasSelection` after `panels` (hpp + cpp + doc comment). Replace `EditorPanels.cpp:130-132`:
```cpp
                if (ImGui::MenuItem("Select All"))       requests.selectAll = true;
                if (ImGui::MenuItem("Deselect All"))     requests.deselectAll = true;
                if (ImGui::MenuItem("Invert Selection")) requests.invertSelection = true;
```

- [ ] **Step 5: Consume.** `EditorAppFrame.cpp` call site (:1106) passes `m_selection.HasSelection()` after `m_panelVis`. In the consume block (after the `menuReq.rebuildModule` handling, before the material block), add:
```cpp
        // Edit -> selection ops. Pure selection state -- no undo step (UE
        // does not undo selection either).
        if (menuReq.deselectAll)
            m_selection.Clear();
        if (menuReq.selectAll || menuReq.invertSelection)
        {
            const std::vector<Astra::Entity> all =
                Arcane::Editor::CollectSceneEntities(m_runtime->Registry());
            const std::vector<Astra::Entity> next = menuReq.selectAll
                ? all
                : Arcane::Editor::InvertSelectionSet(all, m_selection);
            m_selection.Clear();
            m_selection.AddRange(next, next.empty() ? Astra::Entity::Invalid()
                                                    : next.back());
        }
```
(`#include "SelectionOps.hpp"` at the top of EditorAppFrame.cpp.)

- [ ] **Step 6: Build + suite green, commit**

```powershell
git add Arcane/ArcaneEditor/src/SelectionOps.hpp Arcane/Tests/src/SelectionOpsTest.cpp Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp Arcane/ArcaneEditor/src/EditorAppFrame.cpp
git commit -m "feat(editor): wire Edit -> Select All / Deselect All / Invert Selection"
```

---

### Task 5: Wire Edit → Rename / Delete to the Outliner's own code

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (promote three functions + 2 new MenuRequests fields), `EditorPanels.cpp` (:867-915 — move out of the anonymous namespace; :127-128 menu items), `EditorAppFrame.cpp` (consume)

**Interfaces:**
- Consumes: the file-local statics at `EditorPanels.cpp:867-915` (`ApplyStructural`, `BeginRename`, `DeleteSelection`) and their in-file callers (:1099, :1102, :1421, :1466, :1479); `OutlinerState` (EditorPanels.hpp:190-214); `Arcane::Identity`.
- Produces (promoted to `EditorPanels.hpp`, namespace `Arcane::Editor` — later tasks call `ApplyStructural`):
  - `bool ApplyStructural(Arcane::CommandStack& undo, const SceneEditBinding& b, std::string label, Arcane::FunctionRef<bool()> mutate, const std::vector<Astra::Entity>* touched = nullptr);`
  - `void BeginRename(OutlinerState& st, Astra::Entity e, const std::string& current);`
  - `void DeleteSelection(Astra::Registry& registry, SelectionContext& sel, Arcane::CommandStack& undo, const SceneEditBinding& binding);`
  - `MenuRequests` grows `bool renameSelected`, `bool deleteSelected`.

- [ ] **Step 1: Promote.** Move the three function definitions (and `CanEditStructure`, which stays file-local — nothing outside needs it) out of the anonymous namespace into namespace scope in `EditorPanels.cpp`; declare the three in `EditorPanels.hpp` after the `SceneEditBinding` definition (:189), carrying their existing comments. Their bodies do not change; in-file callers are unaffected (same names, same TU). `EditorPanels.hpp` needs `#include <Arcane/Core/FunctionRef.hpp>` — verify the actual include path by grepping how `EditorPanels.cpp` sees `Arcane::FunctionRef` today (it comes via `RegistryStateCommand.hpp`; include what the hpp needs explicitly).

- [ ] **Step 2: Menu items.** Replace `EditorPanels.cpp:127-128`:
```cpp
                if (ImGui::MenuItem("Rename", "F2", false, hasSelection))
                    requests.renameSelected = true;
                if (ImGui::MenuItem("Delete", "Del", false, hasSelection))
                    requests.deleteSelected = true;
```
(The stale "wiring pass" comment above them (:124-126) shrinks to: `// Same code paths as the Outliner's F2/Del bindings.`)

- [ ] **Step 3: Consume.** In the consume block, after the selection-ops block from Task 4:
```cpp
        // Edit -> Rename / Delete: the Outliner's own F2/Del code paths
        // (promoted in EditorPanels.hpp) -- menu and keybind cannot drift.
        if (menuReq.renameSelected && m_selection.HasSelection())
        {
            if (const Arcane::Identity* info =
                    m_runtime->Registry().GetComponent<Arcane::Identity>(m_selection.Primary()))
                Arcane::Editor::BeginRename(m_outliner, m_selection.Primary(), info->name);
        }
        if (menuReq.deleteSelected)
            Arcane::Editor::DeleteSelection(m_runtime->Registry(), m_selection,
                                            *m_undo, m_editBinding);
```
(The rename box lives in the Outliner panel, which draws later this same frame in `DrawSelectionPanels` — the focus-pending flag lands in time. A hidden Outliner simply parks the rename; acceptable, same as the panel's own deferred-rename path.)

- [ ] **Step 4: Build + full non-GPU suite green** (Rename/Delete behavior is pinned by the existing `[outliner]` suites — this task adds routing, not behavior).

- [ ] **Step 5: Commit**

```powershell
git add Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp Arcane/ArcaneEditor/src/EditorAppFrame.cpp
git commit -m "feat(editor): wire Edit -> Rename/Delete through the Outliner's F2/Del code paths"
```

### Task 6: Engine-side entity clipboard ops (SerializeSubtrees / InstantiateSubtrees)

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Edit/EntityOps.hpp` (+3 decls), `Arcane/Arcane/src/Arcane/Edit/EntityOps.cpp` (+3 defs)
- Test: `Arcane/Tests/src/EntityClipboardTest.cpp`

**Interfaces:**
- Consumes: `SelectionRoots` + the file-local `CollectSubtree` (EntityOps.cpp:27-33, same TU), `Scene::kSceneJsonVersion` + `Scene::Detail::AddComponentByTypeName` + the SaveJson per-entity walk shape (`SceneSerializer.hpp:58-192`), `ReflectionJsonWriter`, `reg.GetEntityComponents/GetComponentByHash/GetRelationshipGraph().GetLinks/SetParent/AddLink/CreateEntity/DestroyEntity/CreateView<Identity>`, `Guid::Generate()`, `SceneRoot`.
- Produces (all `ARCANE_API`, namespace `Arcane::Edit`):
  - `std::vector<Astra::Entity> SubtreeEntities(Astra::Registry& reg, std::span<const Astra::Entity> roots);`
  - `nlohmann::json SerializeSubtrees(Astra::Registry& reg, std::span<const Astra::Entity> set);`
  - `std::vector<Astra::Entity> InstantiateSubtrees(Astra::Registry& reg, const nlohmann::json& payload);` (returns created ROOTS)
- Payload schema = the scene schema (`{"version", "entities":[{"components", "parent", "links"?}]}`), plus on each root entry (`"parent": -1`) an optional `"rootParentGuid": "<guid text>"`.

- [ ] **Step 1: Declarations.** Append to `EntityOps.hpp` (after `SelectionRoots`, before `WorldMatrix`; add `#include <Json.hpp>` to the header's includes):

```cpp
    // Every entity of every subtree rooted in `roots` (each root + all its
    // descendants; duplicates collapse, dead roots skip). The set a Cut must
    // hand DeleteEntities: DeleteEntities SPLICES children up to survivors,
    // so cutting only the roots would orphan the children the clipboard just
    // captured.
    ARCANE_API std::vector<Astra::Entity> SubtreeEntities(Astra::Registry& reg,
                                                          std::span<const Astra::Entity> roots);

    // Serialize the subtrees rooted at SelectionRoots(set) -- a nested
    // selection copies once. Same {"version","entities"} document SaveJson
    // produces (components via the reflection walk, "parent" = payload-
    // internal index, -1 for roots; internal "links" kept), plus each ROOT
    // entry records "rootParentGuid" -- the Identity Guid of its ORIGINAL
    // parent (key absent when the parent has no Identity or none exists).
    // "entities" is empty when `set` holds nothing alive.
    ARCANE_API nlohmann::json SerializeSubtrees(Astra::Registry& reg,
                                                std::span<const Astra::Entity> set);

    // Instantiate a SerializeSubtrees payload: fresh entities with fresh
    // Identity GUIDs and names uniquified against the registry (paste/
    // duplicate unique-ify, interactive rename does not -- the UE split
    // documented on RenameEntity above). Internal parents/links remap to the
    // new entities; each root re-parents to the live entity whose Identity
    // Guid matches its recorded rootParentGuid, else under SceneRoot --
    // one rule that makes Duplicate a sibling and cross-instance Paste sane.
    // Returns the created ROOT entities. Refuses (returns {}, creates
    // nothing lasting) when the registry has no SceneRoot, on a version
    // mismatch, on a malformed document, or on a component field error --
    // partial creations are destroyed (all-or-nothing).
    ARCANE_API std::vector<Astra::Entity> InstantiateSubtrees(Astra::Registry& reg,
                                                              const nlohmann::json& payload);
```

- [ ] **Step 2: Definitions.** In `EntityOps.cpp` (needs `#include <Arcane/Serialization/SceneSerializer.hpp>`, `#include <Arcane/Serialization/ReflectionJson.hpp>`, `#include <unordered_map>`):

```cpp
    std::vector<Astra::Entity> SubtreeEntities(Astra::Registry& reg,
                                               std::span<const Astra::Entity> roots)
    {
        std::vector<Astra::Entity> out;
        std::unordered_set<Astra::Entity> seen;
        for (Astra::Entity r : roots)
        {
            if (!reg.IsValid(r) || seen.contains(r))
                continue;
            std::vector<Astra::Entity> subtree;
            CollectSubtree(reg, r, subtree);
            for (Astra::Entity e : subtree)
                if (seen.insert(e).second)
                    out.push_back(e);
        }
        return out;
    }

    nlohmann::json SerializeSubtrees(Astra::Registry& reg,
                                     std::span<const Astra::Entity> set)
    {
        nlohmann::json doc;
        doc["version"]  = Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array();

        const std::vector<Astra::Entity> roots = SelectionRoots(reg, set);
        const std::unordered_set<Astra::Entity> rootSet(roots.begin(), roots.end());
        const std::vector<Astra::Entity> order = SubtreeEntities(reg, roots);

        std::unordered_map<Astra::Entity, int> index;
        for (std::size_t i = 0; i < order.size(); ++i)
            index.emplace(order[i], static_cast<int>(i));
        auto indexOf = [&](Astra::Entity e) -> int
        {
            const auto it = index.find(e);
            return it == index.end() ? -1 : it->second;
        };

        for (Astra::Entity e : order)
        {
            nlohmann::json entry;

            // Per-entity component emit: SaveJson's exact walk
            // (SceneSerializer.hpp) -- reflected roster, null wire shape for
            // zero-size tag components.
            nlohmann::json components = nlohmann::json::object();
            for (const Astra::ComponentDescriptor* desc : reg.GetEntityComponents(e))
            {
                if (!desc || !desc->meta || !desc->visitFields)
                    continue;
                void* instance = const_cast<void*>(
                    static_cast<const Astra::Registry&>(reg).GetComponentByHash(e, desc->hash));
                if (!instance)
                {
                    if (desc->is_empty)
                        components[std::string(desc->meta->typeName)] = nullptr;
                    continue;
                }
                nlohmann::json cj;
                ReflectionJsonWriter writer(cj);
                desc->visitFields(instance, writer);
                components[std::string(desc->meta->typeName)] = std::move(cj);
            }
            entry["components"] = std::move(components);

            entry["parent"] = indexOf(reg.GetParent(e));
            if (rootSet.contains(e))
            {
                const Astra::Entity parent = reg.GetParent(e);
                if (parent.IsValid() && reg.IsValid(parent))
                    if (const Identity* info = reg.GetComponent<Identity>(parent))
                        if (info->id.IsValid())
                            entry["rootParentGuid"] = info->id.ToString();
            }

            // Internal links only, forward edges (SaveJson's rule).
            const int self = indexOf(e);
            nlohmann::json links = nlohmann::json::array();
            for (Astra::Entity linked : reg.GetRelationshipGraph().GetLinks(e))
            {
                const int j = indexOf(linked);
                if (j > self)
                    links.push_back(j);
            }
            if (!links.empty())
                entry["links"] = std::move(links);

            doc["entities"].push_back(std::move(entry));
        }
        return doc;
    }

    std::vector<Astra::Entity> InstantiateSubtrees(Astra::Registry& reg,
                                                   const nlohmann::json& payload)
    {
        const SceneRoot* sceneRoot = reg.GetResource<SceneRoot>();
        if (!sceneRoot)
            return {};   // nowhere safe to live -- CreateEntityInScene's rule

        std::vector<Astra::Entity> created;
        const auto destroyPartial = [&]() -> std::vector<Astra::Entity>
        {
            for (Astra::Entity e : created)
                if (reg.IsValid(e))
                    reg.DestroyEntity(e);
            return {};
        };

        try
        {
            if (!payload.is_object() || !payload.contains("entities"))
                return {};
            const auto vit = payload.find("version");
            if (vit == payload.end() || !vit->is_number_integer() ||
                vit->get<int>() != Scene::kSceneJsonVersion)
                return {};
            const auto& entities = payload["entities"];
            if (!entities.is_array() || entities.empty())
                return {};

            // Pre-existing state, captured BEFORE creating anything: the guid
            // map for rootParentGuid targets (fresh guids can never match),
            // and the taken-name set for uniquify.
            std::unordered_map<std::string, Astra::Entity> byGuid;
            std::unordered_set<std::string> taken;
            reg.CreateView<Identity>().ForEach(
                [&](Astra::Entity e, Identity& info)
                {
                    if (info.id.IsValid())
                        byGuid.emplace(info.id.ToString(), e);
                    taken.insert(info.name);
                });

            Astra::ComponentRegistry* creg = reg.GetComponentRegistry();

            // Pass 1: create + components (LoadJson's tolerant per-entry walk).
            for (const auto& entry : entities)
            {
                if (!entry.is_object())
                    return destroyPartial();
                const Astra::Entity e = reg.CreateEntity();
                created.push_back(e);

                const auto cit = entry.find("components");
                if (cit == entry.end() || !cit->is_object())
                    continue;
                static const nlohmann::json kEmptyFields = nlohmann::json::object();
                for (auto it = cit->begin(); it != cit->end(); ++it)
                {
                    const nlohmann::json* fields;
                    if (it.value().is_object())    fields = &it.value();
                    else if (it.value().is_null()) fields = &kEmptyFields;
                    else                            continue;
                    if (Scene::Detail::AddComponentByTypeName(reg, creg, e, it.key(), *fields)
                        == Scene::Detail::AddComponentResult::Error)
                        return destroyPartial();   // unsupported field type: fail loud
                }
            }

            // Pass 2: fresh identity -- new Guid, uniquified name (paste/
            // duplicate unique-ify; the taken set grows so N pastes of "Foo"
            // yield Foo_2, Foo_3, ...).
            for (Astra::Entity e : created)
            {
                Identity* info = reg.GetComponent<Identity>(e);
                if (!info)
                    continue;
                info->id = Guid::Generate();
                std::string name = info->name.empty() ? std::string("Entity") : info->name;
                if (taken.contains(name))
                {
                    for (int i = 2;; ++i)
                    {
                        std::string candidate = name + "_" + std::to_string(i);
                        if (!taken.contains(candidate))
                        {
                            name = std::move(candidate);
                            break;
                        }
                    }
                }
                taken.insert(name);
                info->name = std::move(name);
            }

            // Pass 3: structure. Internal parents/links remap; roots go to
            // their recorded parent when it still lives, else SceneRoot.
            std::vector<Astra::Entity> roots;
            for (std::size_t i = 0; i < entities.size(); ++i)
            {
                const auto& entry = entities[i];
                int parent = -1;
                if (const auto pit = entry.find("parent");
                    pit != entry.end() && pit->is_number_integer())
                    parent = pit->get<int>();

                if (parent >= 0 && parent < static_cast<int>(created.size()))
                {
                    reg.SetParent(created[i], created[static_cast<std::size_t>(parent)]);
                }
                else
                {
                    Astra::Entity target = sceneRoot->entity;
                    if (const auto git = entry.find("rootParentGuid");
                        git != entry.end() && git->is_string())
                    {
                        const auto hit = byGuid.find(git->get<std::string>());
                        if (hit != byGuid.end() && reg.IsValid(hit->second))
                            target = hit->second;
                    }
                    reg.SetParent(created[i], target);
                    roots.push_back(created[i]);
                }

                if (const auto lit = entry.find("links");
                    lit != entry.end() && lit->is_array())
                {
                    for (const auto& lj : *lit)
                    {
                        if (!lj.is_number_integer())
                            continue;
                        const int j = lj.get<int>();
                        if (j >= 0 && j < static_cast<int>(created.size()))
                            reg.AddLink(created[i], created[static_cast<std::size_t>(j)]);
                    }
                }
            }
            return roots;
        }
        catch (...)
        {
            return destroyPartial();   // exception-free contract at the API edge
        }
    }
```

- [ ] **Step 3: Write the tests** — `Arcane/Tests/src/EntityClipboardTest.cpp`, using EntityOpsTest.cpp's `World` fixture shape verbatim (shared `ComponentRegistry`, `Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());` in the ctor, `RegisterSceneComponents(reg)`), tag `"[outliner][json]"`. Cases (write them all; each is a few lines on the fixture):
  1. **Round-trip**: build root→a(name "Foo", child b)→sibling c; `SerializeSubtrees(reg, {a})` has 2 entities, entry 0 parent `-1` with `rootParentGuid` == root's Identity guid text, entry 1 parent `0`; `InstantiateSubtrees` on the same registry returns 1 root, parented under root (guid matched), with a fresh Guid (`!=` a's), name `"Foo_2"`, and its child's parent is the new root.
  2. **Nested selection collapses**: `SerializeSubtrees(reg, {a, b})` (b is a's child) emits exactly a's subtree once (2 entities).
  3. **Missing rootParentGuid target → SceneRoot**: serialize a; delete a's parent chain entity the guid names (or erase the key from the json); instantiate → new root's parent is the SceneRoot entity.
  4. **Cross-registry paste**: instantiate a's payload into a second `World` whose scene is `Scene::CreateEmpty` — root lands under that registry's SceneRoot; names uniquified against IT.
  5. **No SceneRoot → refusal**: fresh registry without `CreateEmpty`; `InstantiateSubtrees` returns `{}` and creates nothing (`CreateView<Identity>` count unchanged).
  6. **Version mismatch → refusal**: doctor `payload["version"] = 999`.
  7. **SubtreeEntities**: for roots {a} returns a+b; duplicate roots collapse; dead root skipped.
  8. **Cut shape + undo** (RegistryStateCommandTest's `World` with Snapshot/Restore): `ApplyRegistryMutation(stack, "Cut", snapshot, restore, [&]{ return DeleteEntities(reg, SubtreeEntities(reg, roots)) > 0; })`, then `Undo()` restores a AND b (the splice hazard pinned).

- [ ] **Step 4: Regenerate, build, run** `.\ArcaneTests.exe "[outliner]" ~[gpu]` — new cases pass, existing `[outliner]` suite untouched.

- [ ] **Step 5: Commit**

```powershell
git add Arcane/Arcane/src/Arcane/Edit/EntityOps.hpp Arcane/Arcane/src/Arcane/Edit/EntityOps.cpp Arcane/Tests/src/EntityClipboardTest.cpp
git commit -m "feat(edit): entity subtree clipboard ops -- SerializeSubtrees/InstantiateSubtrees/SubtreeEntities"
```

### Task 7: Editor clipboard wiring — Cut / Copy / Paste / Duplicate + Ctrl+X/C/V/D

**Files:**
- Create: `Arcane/ArcaneEditor/src/EntityClipboard.hpp`
- Test: append to `Arcane/Tests/src/EntityClipboardTest.cpp`
- Modify: `EditorPanels.hpp` (4 MenuRequests fields), `EditorPanels.cpp` (:120-123 menu items), `EditorApp.hpp` (FrameState + edge members), `EditorAppFrame.cpp` (shortcuts + consume)

**Interfaces:**
- Consumes: Task 5's promoted `ApplyStructural`; Task 6's `SerializeSubtrees` / `InstantiateSubtrees` / `SubtreeEntities` / `SelectionRoots`; `ImGui::SetClipboardText/GetClipboardText`; the Ctrl+N/O/S shortcut shape (`EditorAppFrame.cpp:436-497`, `FrameState` at EditorApp.hpp:157-170).
- Produces:
  - `Arcane::Editor::kEntityClipboardKey` (`"arcane_entities"`), `std::string WrapEntityClipboard(const nlohmann::json& payload)`, `std::optional<nlohmann::json> ParseEntityClipboard(const char* text)`
  - `MenuRequests` grows `bool cutSelection`, `bool copySelection`, `bool paste`, `bool duplicateSelection`
  - `FrameState` grows `bool scCut, scCopy, scPaste, scDuplicate`

- [ ] **Step 1: The envelope header**

```cpp
// Arcane/ArcaneEditor/src/EntityClipboard.hpp
#pragma once

// The OS-clipboard envelope around Edit::SerializeSubtrees payloads (spec
// II.B). A versioned wrapper key so Paste can tell "our entities" from
// arbitrary clipboard text without guessing -- UE puts T3D text on the OS
// clipboard the same way. ImGui-free; the SetClipboardText/GetClipboardText
// calls stay at the consume site.

#include <Json.hpp>

#include <optional>
#include <string>
#include <utility>

namespace Arcane::Editor
{
    inline constexpr const char* kEntityClipboardKey = "arcane_entities";

    [[nodiscard]] inline std::string WrapEntityClipboard(nlohmann::json payload)
    {
        nlohmann::json env;
        env[kEntityClipboardKey] = std::move(payload);
        return env.dump();
    }

    // The payload, or nullopt for foreign/absent/unparseable clipboard text.
    // Non-throwing parse: pasting after copying prose must be a quiet no-op,
    // not an exception path.
    [[nodiscard]] inline std::optional<nlohmann::json> ParseEntityClipboard(const char* text)
    {
        if (!text || !*text)
            return std::nullopt;
        nlohmann::json env = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
        if (env.is_discarded() || !env.is_object())
            return std::nullopt;
        const auto it = env.find(kEntityClipboardKey);
        if (it == env.end() || !it->is_object())
            return std::nullopt;
        return std::move(*it);
    }
}
```

- [ ] **Step 2: Envelope tests** (append to `EntityClipboardTest.cpp`, tag `"[editor]"` — no registry needed): wrap→parse round-trips a `{"version":2,"entities":[]}` payload; `ParseEntityClipboard(nullptr)`, `""`, `"plain prose"`, `"{}"`, and `"{\"arcane_entities\": 3}"` all return nullopt. Build + run `"[editor]"` — pass.

- [ ] **Step 3: Menu items.** Replace `EditorPanels.cpp:120-123` (the stale comment above shrinks accordingly):
```cpp
                if (ImGui::MenuItem("Cut", "Ctrl+X", false, hasSelection))
                    requests.cutSelection = true;
                if (ImGui::MenuItem("Copy", "Ctrl+C", false, hasSelection))
                    requests.copySelection = true;
                // Always enabled: parsing the clipboard every frame to grey
                // this is not worth it -- a foreign-clipboard paste no-ops
                // with a Console INFO instead.
                if (ImGui::MenuItem("Paste", "Ctrl+V"))
                    requests.paste = true;
                if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSelection))
                    requests.duplicateSelection = true;
```
Add the four `MenuRequests` bools (one comment each, naming the menu item).

- [ ] **Step 4: Shortcuts.** In `EditorApp.hpp`: `FrameState` gains `bool scCut = false, scCopy = false, scPaste = false, scDuplicate = false;` (extend the existing scNewScene comment to cover them); next to `m_prevKeyN/O/S` add `bool m_prevKeyX = false, m_prevKeyC = false, m_prevKeyV = false, m_prevKeyD = false;`. In `HandleUndoRedoAndSceneShortcuts` (EditorAppFrame.cpp:436-497), after the Ctrl+N/O/S block, add the same edge shape (find where `kScN/kScO/kScS` are defined and add `kScX = 27, kScC = 6, kScV = 25, kScD = 7` beside them — SDL scancodes, same table as the existing kScZ=29/kScY=28):
```cpp
        // Ctrl+X/C/V/D -- the Edit menu's clipboard items. Same raised-as-
        // request shape as Ctrl+N/O/S above: ONE handler at the menu-request
        // site. `active` already suppresses these while ImGui captures the
        // keyboard (text fields keep their own clipboard) and during Play.
        const bool xDown = ctrl && !shift && snap.ScancodeDown(kScX);
        const bool cDown = ctrl && !shift && snap.ScancodeDown(kScC);
        const bool vDown = ctrl && !shift && snap.ScancodeDown(kScV);
        const bool dDown = ctrl && !shift && snap.ScancodeDown(kScD);
        fs.scCut       = active && xDown && !m_prevKeyX;
        fs.scCopy      = active && cDown && !m_prevKeyC;
        fs.scPaste     = active && vDown && !m_prevKeyV;
        fs.scDuplicate = active && dDown && !m_prevKeyD;
        m_prevKeyX = xDown;
        m_prevKeyC = cDown;
        m_prevKeyV = vDown;
        m_prevKeyD = dDown;
```

- [ ] **Step 5: Consume.** In `DrawEditorUi`'s consume block, fold the edges in next to the existing scene folds (:1165-1168): `menuReq.cutSelection |= fs.scCut;` (×4), then after the Task 5 rename/delete block (`#include "EntityClipboard.hpp"` at the top):

```cpp
        // Edit -> clipboard (spec II.B). Copy serializes the selection's
        // subtree roots to a JSON envelope on the OS clipboard; every
        // structural half wraps in ApplyStructural like the Outliner's ops.
        const auto copySelectionToClipboard = [&]() -> bool
        {
            if (!m_selection.HasSelection())
                return false;
            nlohmann::json payload = Arcane::Edit::SerializeSubtrees(
                m_runtime->Registry(), m_selection.Entities());
            if (payload["entities"].empty())
                return false;
            ImGui::SetClipboardText(
                Arcane::Editor::WrapEntityClipboard(std::move(payload)).c_str());
            return true;
        };
        if (menuReq.copySelection)
            copySelectionToClipboard();
        if (menuReq.cutSelection && copySelectionToClipboard())
        {
            // Delete the FULL captured subtrees -- DeleteEntities splices
            // children up, so passing only the roots would orphan what the
            // clipboard just took (EntityOps.hpp, SubtreeEntities).
            const std::vector<Astra::Entity> roots = Arcane::Edit::SelectionRoots(
                m_runtime->Registry(), m_selection.Entities());
            const std::vector<Astra::Entity> doomed =
                Arcane::Edit::SubtreeEntities(m_runtime->Registry(), roots);
            if (Arcane::Editor::ApplyStructural(*m_undo, m_editBinding, "Cut",
                    [&] { return Arcane::Edit::DeleteEntities(m_runtime->Registry(),
                                                              doomed) > 0; },
                    &doomed))
                m_selection.Clear();
        }
        const auto instantiateAndSelect = [&](const nlohmann::json& payload,
                                              std::string label)
        {
            std::vector<Astra::Entity> roots;
            std::vector<Astra::Entity> made;
            if (Arcane::Editor::ApplyStructural(*m_undo, m_editBinding, std::move(label),
                    [&]
                    {
                        roots = Arcane::Edit::InstantiateSubtrees(m_runtime->Registry(),
                                                                  payload);
                        if (!roots.empty())
                            made = Arcane::Edit::SubtreeEntities(m_runtime->Registry(),
                                                                 roots);
                        return !roots.empty();
                    },
                    &made))
            {
                m_selection.Clear();
                m_selection.AddRange(roots, roots.back());
            }
        };
        if (menuReq.paste)
        {
            if (const auto payload =
                    Arcane::Editor::ParseEntityClipboard(ImGui::GetClipboardText()))
                instantiateAndSelect(*payload, "Paste");
            else
                ARC_INFO("Paste: the clipboard holds no Arcane entities");
        }
        if (menuReq.duplicateSelection && m_selection.HasSelection())
        {
            const nlohmann::json payload = Arcane::Edit::SerializeSubtrees(
                m_runtime->Registry(), m_selection.Entities());
            if (!payload["entities"].empty())
                instantiateAndSelect(payload, "Duplicate");
        }
```

- [ ] **Step 6: Build + full non-GPU suite green; commit**

```powershell
git add Arcane/ArcaneEditor/src/EntityClipboard.hpp Arcane/Tests/src/EntityClipboardTest.cpp Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp Arcane/ArcaneEditor/src/EditorApp.hpp Arcane/ArcaneEditor/src/EditorAppFrame.cpp
git commit -m "feat(editor): wire Edit -> Cut/Copy/Paste/Duplicate over the OS clipboard"
```

---

### Task 8: File → Exit and Save All

**Files:**
- Modify: `Arcane/ArcaneEditor/src/DocumentHost.hpp` (+decl), `DocumentHost.cpp` (+def)
- Modify: `EditorPanels.hpp` (2 MenuRequests fields), `EditorPanels.cpp` (:100-104), `EditorAppFrame.cpp` (consume)

**Interfaces:**
- Consumes: `SceneSession::Request` + the `SceneIntent::Exit` continuation (`RunSceneAction` → `ls.running = false`, EditorAppFrame.cpp:196-226) and the "Unsaved Scene" modal (both already handle Exit end-to-end); `EditorDocument::Dirty()/Save()`; `DoSaveScene`/`ShowSceneSaveDialog` (EditorAppFrame.cpp:1202-1207 shape).
- Produces: `std::size_t DocumentHost::SaveAllDirty();` `MenuRequests` grows `bool exitEditor`, `bool saveAll`.

- [ ] **Step 1: DocumentHost::SaveAllDirty.** Decl in `DocumentHost.hpp` next to `AnyDirty()`:
```cpp
        // Save every dirty document in place. Every document has a real path
        // (they are only born from files via OpenPath), so no dialog is ever
        // needed here -- unlike the scene. Returns how many saves FAILED
        // (0 = everything clean now); a failed save keeps its dirty flag.
        std::size_t SaveAllDirty();
```
Def in `DocumentHost.cpp` next to `AnyDirty` (:74-80):
```cpp
    std::size_t DocumentHost::SaveAllDirty()
    {
        std::size_t failed = 0;
        for (const auto& d : m_docs)
            if (d->Dirty() && !d->Save())
                ++failed;
        return failed;
    }
```

- [ ] **Step 2: Menu.** Replace `EditorPanels.cpp:101-104`:
```cpp
                if (ImGui::MenuItem("Open Project")) requests.openProject = true;
                // UE's File-menu item is "Save All", and that is what this
                // saves: the scene + every dirty open document. There is no
                // project-level state to save (the manifest is immutable
                // outside narrow seams) -- the old "Save Project" label lied.
                if (ImGui::MenuItem("Save All", nullptr, false, !playing))
                    requests.saveAll = true;
                if (playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Stop play mode to save");
                ImGui::Separator();
                if (ImGui::MenuItem("Exit"))
                    requests.exitEditor = true;
```
Add the two `MenuRequests` bools.

- [ ] **Step 3: Consume.** In the consume block (after the clipboard block):
```cpp
        // File -> Save All: scene first (never-saved routes through the Save
        // As dialog, exactly like Save), then every dirty document in place.
        if (menuReq.saveAll)
        {
            if (m_scene.IsDirty(*m_undo))
            {
                if (!m_scene.Path().empty()) DoSaveScene(m_scene.Path());
                else                         ShowSceneSaveDialog();
            }
            if (const std::size_t failed = m_documents.SaveAllDirty())
                ARC_ERROR("Save All: {} document save(s) failed (see Console)", failed);
        }
        // File -> Exit: the SAME SceneIntent::Exit flow the OS quit uses --
        // dirty scene parks behind the confirm modal; a clean exit defers to
        // the top of the next frame like every other scene action.
        if (menuReq.exitEditor &&
            m_scene.Request(Arcane::Editor::SceneIntent::Exit, {}, *m_undo))
        {
            ls.sceneAction = { Arcane::Editor::SceneIntent::Exit, {} };
        }
```

- [ ] **Step 4: Build + full non-GPU suite green** (DocumentHost has existing close-flow tests; if a `DocumentHostTest.cpp` exists with a fake-document fixture, add one `SaveAllDirty` case on it: two docs, one dirty whose `Save()` succeeds → returns 0 and clears the flag; one whose `Save()` fails → returns 1 and stays dirty. If no such fixture exists, skip — the logic is four lines over an already-tested contract).

- [ ] **Step 5: Commit**

```powershell
git add Arcane/ArcaneEditor/src/DocumentHost.hpp Arcane/ArcaneEditor/src/DocumentHost.cpp Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp Arcane/ArcaneEditor/src/EditorAppFrame.cpp
git commit -m "feat(editor): wire File -> Exit (quit-confirm flow) and Save All (scene + dirty documents)"
```

### Task 9: File → Open Recent Project (resurface the parked chain)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (File menu, directly under "Open Project")

**Interfaces:**
- Consumes: the `recents` param (already passed, currently unread), `MenuRequests::openRecentPath` and its intact consumer (`EditorAppFrame.cpp:1130-1139` → `ConsumeProjectDialogResult` :285-306 — every guard included), `RecentSelection`/`RecentProject` (RecentProjects.hpp:43-63), the File-menu rising-edge refresh (:1140-1146, already live via `fileMenuOpen`).
- Produces: a working "Open Recent Project" submenu; no signature or consumer changes at all.

- [ ] **Step 1: Restore the submenu.** This is the block commit `39919b07` deleted, restored under the new item order and renamed "Open Recent Project". Insert directly after the `if (ImGui::MenuItem("Open Project")) requests.openProject = true;` line:

```cpp
                // Open Recent Project: the Hub's shared list, already filtered
                // to what THIS editor's ABI can open (RecentProjects.hpp).
                // Greyed when there is nothing at all to show. The picked path
                // lands in the SAME slot the Open Project dialog fills, so it
                // inherits every guard that path has (unsaved-scene confirm,
                // rival-editor lock, ABI gate, failure modal).
                const bool anyRecents =
                    recents && (!recents->visible.empty() || recents->hiddenForAbi > 0);
                if (ImGui::BeginMenu("Open Recent Project", anyRecents))
                {
                    for (const RecentProject& r : recents->visible)
                    {
                        if (ImGui::MenuItem(r.name.c_str()))
                            requests.openRecentPath = r.path;
                        // Full path on hover: the name alone cannot separate two
                        // projects sharing a folder name -- UE's tooltip choice
                        // (FRecentProjectsMenu::MakeMenu).
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", r.path.c_str());
                    }
                    // Never leave the list silently short: an ABI-filtered
                    // absence must be distinguishable from a broken list.
                    if (recents->hiddenForAbi > 0)
                    {
                        if (!recents->visible.empty())
                            ImGui::Separator();
                        char hidden[128];
                        std::snprintf(hidden, sizeof(hidden),
                                      "%zu project%s hidden (built for another engine version)",
                                      recents->hiddenForAbi,
                                      recents->hiddenForAbi == 1 ? "" : "s");
                        ImGui::BeginDisabled();
                        ImGui::MenuItem(hidden);
                        ImGui::EndDisabled();
                    }
                    ImGui::EndMenu();
                }
```
Also: delete the now-stale "NO MENU RAISES THIS TODAY" paragraph on `MenuRequests::openRecentPath` (EditorPanels.hpp:35-38) and the "recents is currently PARKED" paragraph in the BeginDockSpace doc comment; `<cstdio>` is already included (:34); `RecentProject` is visible via the existing `RecentSelection` include — verify EditorPanels.hpp includes `RecentProjects.hpp` (it declares the pointer param, so it does, possibly as a forward decl — if forward-declared, add `#include "RecentProjects.hpp"` to EditorPanels.cpp).

- [ ] **Step 2: Build + suite green; commit**

```powershell
git add Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(editor): resurface File -> Open Recent Project over the Hub's shared recents"
```

---

### Task 10: File → Open Recent Scene (per-project tracking)

**Files:**
- Create: `Arcane/ArcaneEditor/src/SceneRecents.hpp`, `Arcane/ArcaneEditor/src/SceneRecents.cpp`
- Test: `Arcane/Tests/src/SceneRecentsTest.cpp`
- Modify: `Arcane/premake5.lua` (ArcaneTests `files` list: add `"%{wks.location}/ArcaneEditor/src/SceneRecents.cpp",` beside the RecentProjects.cpp entry ~:652)
- Modify: `EditorPanels.hpp/.cpp` (signature + submenu + 1 MenuRequests field), `EditorApp.hpp` (member + 2 methods), `EditorAppScene.cpp` (push hooks), `EditorApp.cpp` + `EditorAppProject.cpp` (reload/clear at the project-open/switch sites), `EditorAppFrame.cpp` (call site, rising-edge reload, consume)

**Interfaces:**
- Consumes: `Project::Root()`, the `Saved/` convention (`EditorLock::FileFor` = `root / "Saved" / "editor.lock"`, Project.cpp:477-540), the dialog-slot trick (`m_pendingSceneOpenPath` + `m_pendingSceneMutex`, EditorApp.hpp:642-664 — routing through it inherits the unsaved-scene guard via `ConsumeSceneDialogResults`), the success points `EditorAppScene.cpp:230-233` (open) and `:292-302` (save), the switch teardown/reload sites (`EditorAppProject.cpp:529-563` and `:649-673`), `RefreshRecents`'s rising-edge hook (:1140-1146).
- Produces:
  - `namespace Arcane::Editor::SceneRecents`: `inline constexpr std::size_t kMaxEntries = 10;` `inline constexpr int kFormatVersion = 1;` `struct List { std::vector<std::string> paths; };` (newest-first, lexically-normal generic strings)
  - `std::filesystem::path FileFor(const std::filesystem::path& projectRoot)` → `root / "Saved" / "recent_scenes.json"`
  - `List Parse(const std::string& jsonText)` / `std::string Serialize(const List&)` (schema `{"version":1,"scenes":["<path>",...]}`; tolerant reads: bad text, wrong shape, or newer version → empty)
  - `void Push(List&, const std::filesystem::path& scenePath)` (normalize via `lexically_normal().generic_string()`, dedup, front-insert, cap `kMaxEntries`)
  - `template<typename ExistsFn> void Prune(List&, ExistsFn exists)` (drop entries `!exists(path)`)
  - `List LoadFile(const std::filesystem::path& file)` / `void SaveFile(const std::filesystem::path& file, const List&)` (`create_directories` on the parent first; a recents write failure is never worth failing a scene save — swallow with `ARC_WARN`)
  - `MenuRequests` grows `std::string openRecentScenePath;` — `BeginDockSpace` gains `const SceneRecents::List* sceneRecents = nullptr` as the LAST param.
  - `EditorApp`: `SceneRecents::List m_sceneRecents;` `void NoteSceneOpened(const std::filesystem::path&);` `void ReloadSceneRecents();`

- [ ] **Step 1: Write `SceneRecents.hpp/.cpp`.** Header carries the declarations above (Prune as a template in the header; the rest defined in the .cpp). `.cpp` follows `RecentProjects.cpp`'s tolerant-parse shape exactly (non-throwing `nlohmann::json::parse(text, nullptr, false)`, version gate BEFORE touching the array, skip non-string entries). `Push`:
```cpp
    void Push(List& list, const std::filesystem::path& scenePath)
    {
        const std::string key = scenePath.lexically_normal().generic_string();
        if (key.empty())
            return;
        std::erase(list.paths, key);
        list.paths.insert(list.paths.begin(), key);
        if (list.paths.size() > kMaxEntries)
            list.paths.resize(kMaxEntries);
    }
```

- [ ] **Step 2: Write the failing tests** (`SceneRecentsTest.cpp`, tag `"[editor]"`, no filesystem — Parse/Serialize/Push/Prune only): push dedups to front and caps at 10; Serialize→Parse round-trips order; `Parse("")`, `Parse("junk")`, `Parse("{\"version\":99,\"scenes\":[]}")` → empty; Prune with `[](const std::string& p){ return p != "gone"; }` drops exactly `"gone"`. Add the premake entry, regenerate, build, run `"[editor]"` — pass.

- [ ] **Step 3: App state + hooks.** `EditorApp.hpp`: member + the two methods (decls near `RefreshRecents`/`NoteProjectOpened`, :688-705, with a comment mirroring theirs: per-PROJECT list, `<root>/Saved/recent_scenes.json`, refreshed on project open and on the File menu's rising edge; pushed ONLY from the two scene success paths). In `EditorApp.cpp` next to `NoteProjectOpened`:
```cpp
    void EditorApp::ReloadSceneRecents()
    {
        m_sceneRecents = {};
        const Arcane::Project* proj = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!proj)
            return;
        m_sceneRecents = Arcane::Editor::SceneRecents::LoadFile(
            Arcane::Editor::SceneRecents::FileFor(proj->Root()));
        Arcane::Editor::SceneRecents::Prune(m_sceneRecents,
            [](const std::string& p)
            {
                std::error_code ec;
                return std::filesystem::exists(p, ec);
            });
    }

    void EditorApp::NoteSceneOpened(const std::filesystem::path& file)
    {
        const Arcane::Project* proj = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!proj)
            return;   // project-less session: nowhere durable to record
        Arcane::Editor::SceneRecents::Push(m_sceneRecents, file);
        Arcane::Editor::SceneRecents::SaveFile(
            Arcane::Editor::SceneRecents::FileFor(proj->Root()), m_sceneRecents);
    }
```
Hooks: `NoteSceneOpened(file);` after `m_scene.Adopt(...)` at BOTH success points — `EditorAppScene.cpp:230` (open) and `:293` (save; covers Save As — `WriteAutoScreenshot()` there is the precedent for success-point side effects). `ReloadSceneRecents();` beside BOTH existing `NoteProjectOpened();` calls (EditorApp.cpp:674 area and EditorAppProject.cpp:671), and `m_sceneRecents = {};` in `switch_teardown` next to `m_scene.Reset` (EditorAppProject.cpp:556).

- [ ] **Step 4: Menu.** `BeginDockSpace` gains the trailing `const SceneRecents::List* sceneRecents = nullptr` (hpp includes `SceneRecents.hpp`); `MenuRequests` grows `std::string openRecentScenePath;` (comment: a picked recent-scene path, routed through the scene-open dialog slot). Replace the greyed placeholder (:77-82):
```cpp
                const bool anySceneRecents = sceneRecents && !sceneRecents->paths.empty();
                if (ImGui::BeginMenu("Open Recent Scene", anySceneRecents))
                {
                    for (const std::string& p : sceneRecents->paths)
                    {
                        // Stem as the label (its project-relative identity),
                        // full path in the tooltip -- the recents-menu shape
                        // Open Recent Project uses below.
                        const std::string label =
                            std::filesystem::path(p).stem().string() + "##" + p;
                        if (ImGui::MenuItem(label.c_str()))
                            requests.openRecentScenePath = p;
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", p.c_str());
                    }
                    ImGui::EndMenu();
                }
```
(`<filesystem>` include in EditorPanels.cpp if not present.)

- [ ] **Step 5: Wire the call site + consume.** `EditorAppFrame.cpp:1106` passes `&m_sceneRecents` as the last arg. Rising-edge block (:1140-1146) also calls `ReloadSceneRecents();` next to `RefreshRecents();`. Consume, next to the openRecentPath block (:1130-1139) and with the same comment shape:
```cpp
        // A picked recent scene lands in the SAME slot the Open Scene dialog's
        // callback fills, so it flows through ConsumeSceneDialogResults and
        // inherits the unsaved-scene guard -- a second open path would have to
        // re-earn it.
        if (!menuReq.openRecentScenePath.empty())
        {
            std::lock_guard<std::mutex> lk(m_pendingSceneMutex);
            m_pendingSceneOpenPath = menuReq.openRecentScenePath;
        }
```

- [ ] **Step 6: Build + full non-GPU suite green; commit**

```powershell
git add Arcane/ArcaneEditor/src/SceneRecents.hpp Arcane/ArcaneEditor/src/SceneRecents.cpp Arcane/Tests/src/SceneRecentsTest.cpp Arcane/premake5.lua Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp Arcane/ArcaneEditor/src/EditorApp.hpp Arcane/ArcaneEditor/src/EditorApp.cpp Arcane/ArcaneEditor/src/EditorAppFrame.cpp Arcane/ArcaneEditor/src/EditorAppScene.cpp Arcane/ArcaneEditor/src/EditorAppProject.cpp
git commit -m "feat(editor): File -> Open Recent Scene -- per-project recents at Saved/recent_scenes.json"
```

### Task 11: Assets → Show in Explorer / Copy Path

**Files:**
- Modify: `Arcane/ArcaneEditor/src/AssetBrowser.hpp` (:175-179 state), `AssetBrowser.cpp` (:94-114 row click), `EditorPanels.hpp/.cpp` (signature + Assets menu :153-155 + 2 MenuRequests fields), `EditorAppFrame.cpp` (call site + consume), `EditorAppProject.cpp` (switch teardown)

**Interfaces:**
- Consumes: `AssetEntry` (`guid`/`mountPath`, AssetBrowser.hpp:76-82), the row `Selectable` whose return is currently discarded (AssetBrowser.cpp:102-105), `Project::ResolveAsset(AssetId::FromGuid(guid))` (Project.cpp:431-440), `ImGui::SetClipboardText`.
- Produces: `AssetBrowserState` grows `Arcane::Guid selected;` — `BeginDockSpace` gains `bool hasAssetSelection` after `hasSelection`; `MenuRequests` grows `bool showInExplorer`, `bool copyAssetPath`.

- [ ] **Step 1: Track the selection.** `AssetBrowserState` (AssetBrowser.hpp:175-179) grows:
```cpp
        // Last-clicked row (nil = none): what the Assets menu's Show in
        // Explorer / Copy Path act on. Reset with the whole state on project
        // switch -- a Guid from the outgoing registry must not survive.
        Arcane::Guid selected;
```
Row draw (AssetBrowser.cpp:102-105): capture the click and feed the highlight —
```cpp
                if (ImGui::Selectable(label.c_str(), state.selected == e.guid,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick))
                    state.selected = e.guid;
```
Switch teardown (`EditorAppProject.cpp:529-563`, next to `m_diagnostics.ClearAll()`): `m_assetBrowser = {};` with a one-line comment (selection + search + filter all belong to the outgoing project).

- [ ] **Step 2: Menu.** `BeginDockSpace` gains `bool hasAssetSelection` after `hasSelection` (hpp + cpp + doc comment); call site passes `m_assetBrowser.selected.IsValid()`. Replace the two placeholders (:153-155):
```cpp
                // Act on the Assets panel's last-clicked row; greyed until one
                // exists. Resolution (Guid -> mount -> file) happens app-side
                // at the click -- a stale selection warns instead of acting.
                if (ImGui::MenuItem("Show in Explorer", nullptr, false, hasAssetSelection))
                    requests.showInExplorer = true;
                if (ImGui::MenuItem("Copy Path", nullptr, false, hasAssetSelection))
                    requests.copyAssetPath = true;
```
Add the two `MenuRequests` bools.

- [ ] **Step 3: Consume.** In the consume block (after the Save All/Exit block). `EditorAppFrame.cpp` needs the shell include: add `#include <Windows.h>` + `#include <shellapi.h>` guarded the way the editor's other win32 TUs do (grep for an existing `#include <Windows.h>` in `ArcaneEditor/src` — e.g. RuntimeLaunch.cpp — and copy its define set, typically `WIN32_LEAN_AND_MEAN`/`NOMINMAX` before it):
```cpp
        // Assets -> Show in Explorer / Copy Path, on the browser's tracked row.
        if ((menuReq.showInExplorer || menuReq.copyAssetPath) &&
            m_assetBrowser.selected.IsValid())
        {
            const Arcane::Project* proj = m_runtime->CurrentProject();
            const auto assetPath = proj
                ? proj->ResolveAsset(Arcane::AssetId::FromGuid(m_assetBrowser.selected))
                : std::nullopt;
            if (!assetPath)
            {
                ARC_WARN("Assets: the selected asset no longer resolves to a file");
            }
            else
            {
                if (menuReq.showInExplorer)
                {
                    // explorer /select opens the folder WITH the file focused.
                    const std::wstring args =
                        L"/select,\"" + assetPath->wstring() + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                  args.c_str(), nullptr, SW_SHOWNORMAL);
                }
                if (menuReq.copyAssetPath)
                    ImGui::SetClipboardText(assetPath->string().c_str());
            }
        }
```

- [ ] **Step 4: Build + full non-GPU suite green; commit**

```powershell
git add Arcane/ArcaneEditor/src/AssetBrowser.hpp Arcane/ArcaneEditor/src/AssetBrowser.cpp Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp Arcane/ArcaneEditor/src/EditorAppFrame.cpp Arcane/ArcaneEditor/src/EditorAppProject.cpp
git commit -m "feat(editor): wire Assets -> Show in Explorer / Copy Path off the browser's tracked row"
```

---

### Task 12: Final gate + desk-verify handoff

**Files:**
- Modify: `docs/superpowers/specs/2026-08-10-editor-menu-wiring-design.md` (status line only, if desired: Approved → Implemented, desk-verify pending)

- [ ] **Step 1: Full suite, both flavors that matter.** From the exe dir: `.\ArcaneTests.exe ~[gpu]` (capture the seed banner; on any failure rerun with `--rng-seed N` to reproduce). Then a Release build compile check: `& $msbuild Arcane.slnx /p:Configuration=Release /m /nologo` (no test run needed — this catches NDEBUG-only breakage, the workspace rule).

- [ ] **Step 2: Sanity sweep for leftovers.** `git grep -n "Save Project" Arcane/ArcaneEditor` → no hits; `git grep -n "NO MENU RAISES THIS TODAY" Arcane/ArcaneEditor` → no hits (both stale comments were rewritten in Tasks 8/9).

- [ ] **Step 3: Report the desk-verify list to the user** (the editor must be run at the desk — GPU/windowed runs are desk-only on this machine, and several behaviors are dock/OS-bound). The list, verbatim from the spec:
  - X on each non-viewport tab hides the panel; the Window menu item unchecks.
  - Re-show returns the panel to its previous dock spot **with its tab selected** (if the returning tab is NOT auto-selected, implement the spec's fallback: a one-frame-deferred `SelectDockTab(name)` after the panel's first re-Begin).
  - Visibility survives an editor restart (`[EditorPanels][Visibility]` in imgui.ini).
  - Reset Layout rebuilds the default layout and re-shows everything; an open shader-graph document survives it.
  - The Viewport tab has no X; its Window-menu entry stays checked and focuses it.
  - Edit menu: Select All/Deselect/Invert against the Outliner; Rename opens the Outliner's rename box; Delete removes and undoes.
  - Cut/Copy/Paste/Duplicate: paste lands under the source's parent, names uniquify, one undo step each, paste from a SECOND editor instance works (OS clipboard).
  - Exit with a dirty scene shows the confirm modal; Save All saves scene + dirty shader doc; both greyed during Play with tooltips.
  - Both recents submenus populate and open their targets; scene recents survive a restart and stay per-project.
  - Show in Explorer lands on the file; Copy Path pastes the absolute path.

- [ ] **Step 4: Done.** Do NOT push — main already carries unpushed work the user is holding; leave push timing to them.

---

## Plan self-review notes (already applied)

- **Spec coverage:** Part I §I.1-I.5 → Tasks 1-3; §II.A → Tasks 4-5; §II.B → Tasks 6-7; §II.C → Tasks 8-10; §II.D → Task 11; spec Testing section → the per-task test steps + Task 12's desk list. The spec's `openRecentScenePath`/`showInExplorer`/`copyAssetPath`/`resetLayout`/etc. request names are used verbatim.
- **Deviation from spec, deliberate:** the spec's §II.B "gated on `!io.WantTextInput`" is implemented as the house gate `active` (= `!playing && !snap.wantCaptureKeyboard`, the same suppression Ctrl+Z/N/O/S already use — strictly stronger, and the only gate the raw-scancode path can honestly evaluate). The spec's intent (text fields keep their clipboard) holds.
- **Sequencing note:** Tasks 4, 5, 7, 8, 10, 11 all append params to `BeginDockSpace` and blocks to the same consume region — execute in numeric order; the final signature is
  `BeginDockSpace(undo, requests, sceneDirty, playing, buildingModule, hasGameModule, panels, hasSelection, hasAssetSelection, recents = nullptr, sceneRecents = nullptr)`.
- **Type consistency check:** `PanelVisibility.visible` is `std::array<bool, 6>` accessed via `static_cast<std::size_t>(PanelId)` everywhere; `ApplyStructural`'s promoted signature matches its current definition byte-for-byte; `InstantiateSubtrees` returns roots while `touched` gets `SubtreeEntities(roots)` — the asterisk set includes descendants.






