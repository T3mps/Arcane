// T3-D4 (user desk findings round 2, 2026-10-02): "Reveal in Browser does not
// work". Drives the REAL Asset Browser panel through device-less ImGui frames
// inside a REAL dock node shared with a sibling tab -- the shipped default
// layout docks Asset Browser, Asset Graph, Asset Status, Console and Problems
// into ONE node (EditorPanels.cpp BuildDefaultLayout) -- and replays the
// host's `revealInBrowse` consumer verbatim (EditorAppFrame.cpp
// ConsumeAssetPanelActions: RevealAssetInBrowser, then FocusDockTab).
//
// "Revealed" is measured on the panel, not on the model: the Browser tab is
// the selected one, the target row is selected, and the row lies inside the
// asset table's visible scroll range.

#include <catch2/catch_test_macros.hpp>

#include "Documents/DocumentHost.hpp"
#include "Panels/AssetBrowserPanel.hpp"
#include "Panels/AssetPanelCommon.hpp"
#include "Panels/AssetPanelModel.hpp"

#include <Arcane/Guid.hpp>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace Arcane;
using namespace Arcane::Editor;
namespace fs = std::filesystem;

namespace
{
    void WriteFile(const fs::path& file, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream(file, std::ios::binary) << text;
    }

    Guid RevealGuid(int n)
    {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "dddd0000-0000-4000-8000-%012d", n);
        return Guid::FromString(buf).value();
    }

    // EditorPanels.cpp's SelectDockTab + FocusDockTab, mirrored: that unit is
    // not compiled into ArcaneTests (it drags the whole dock shell along).
    void SelectTab(const char* windowName)
    {
        ImGuiWindow* w = ImGui::FindWindowByName(windowName);
        if (!w || !w->DockNode || !w->DockNode->TabBar)
            return;
        if (ImGuiTabItem* tab = ImGui::TabBarFindTabByID(w->DockNode->TabBar, w->TabId))
            ImGui::TabBarQueueFocus(w->DockNode->TabBar, tab);
    }
    void FocusTab(const char* windowName)
    {
        SelectTab(windowName);
        ImGui::SetWindowFocus(windowName);
    }

    AssetPanelProviders Providers()
    {
        AssetPanelProviders p;
        p.refsFor = [](const Guid&) -> std::optional<std::vector<AssetRef>> { return std::vector<AssetRef>{}; };
        p.cookStateFor = [](const Guid&) { return CookState::Cooked; };
        p.surfaceFor = [](const Guid&) -> std::optional<MaterialSurface> { return MaterialSurface::Sprite; };
        return p;
    }

    // 60 materials under materials/ (a list far taller than the 300 px dock
    // node) plus the target, alone at the BOTTOM under zz/deep/.
    struct RevealHarness
    {
        fs::path root;
        std::optional<Project> project;
        AssetPanelModel model;
        AssetBrowserPanelState state;
        DocumentHost docs;
        AssetPanelServices services{};
        Guid target, firstMaterial;
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        bool docked = false;

        explicit RevealHarness(const char* name) : root(fs::temp_directory_path() / name)
        {
            std::error_code ec;
            fs::remove_all(root, ec);
            REQUIRE(Project::Create(root, "Reveal").has_value());
            const fs::path content = root / "Content";
            for (int i = 0; i < 60; ++i)
            {
                char file[32];
                std::snprintf(file, sizeof(file), "m%02d.arcmat", i);
                WriteFile(content / "materials" / file,
                          R"({"id":")" + RevealGuid(100 + i).ToString() + R"(","type":"material","kind":"sprite"})");
            }
            target = RevealGuid(1);
            firstMaterial = RevealGuid(100);
            WriteFile(content / "zz" / "deep" / "target.arcmat",
                      R"({"id":")" + target.ToString() + R"(","type":"material","kind":"sprite"})");
            project = Project::Open(root);
            REQUIRE(project.has_value());
            model.MarkAllDirty();
            REQUIRE(model.RebuildIfDirty(&project->Registry(), Providers()));
            REQUIRE(model.Find(target) != nullptr);

            services.resolveAssetThumb = [](const Guid&) -> std::uint64_t { return 0ull; };
            services.browserOpen = services.graphOpen = true;

            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 720.0f);
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        }
        ~RevealHarness()
        {
            ImGui::DestroyContext(ctx);
            ImGui::SetCurrentContext(prev);
            project.reset();
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        // One editor frame: the dock host, the Browser and a sibling "Asset
        // Graph" tab in ONE node, then -- when `between` is set -- the host's
        // consumer, which runs AFTER the panels drew (EditorAppFrame.cpp).
        AssetPanelActions Frame(const std::function<void()>& between = {})
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            const ImGuiID dockId = ImHashStr("RevealDock");
            if (!docked)
            {
                ImGui::DockBuilderRemoveNode(dockId);
                ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockId, ImVec2(900.0f, 300.0f));
                ImGui::DockBuilderDockWindow("Asset Browser", dockId);
                ImGui::DockBuilderDockWindow("Asset Graph", dockId);
                ImGui::DockBuilderFinish(dockId);
                docked = true;
            }
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(900.0f, 300.0f));
            ImGui::Begin("##revealhost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f));
            ImGui::End();
            model.RebuildIfDirty(&project->Registry(), Providers());
            AssetPanelActions actions = DrawAssetBrowserPanel(state, model, &*project, docs, services);
            ImGui::Begin("Asset Graph");
            ImGui::TextUnformatted("graph");
            ImGui::End();
            if (between)
                between();
            ImGui::Render();
            return actions;
        }
        void Frames(int n) { for (int i = 0; i < n; ++i) Frame(); }

        // The host consumer, verbatim (EditorAppFrame.cpp ConsumeAssetPanelActions).
        void Reveal(const Guid& g)
        {
            Frame([&] { RevealAssetInBrowser(state, model, g); FocusTab("Asset Browser"); });
        }

        static ImGuiWindow* Browser() { return ImGui::FindWindowByName("Asset Browser"); }
        static bool BrowserTabSelected()
        {
            ImGuiWindow* w = Browser();
            return w && w->DockNode && w->DockNode->TabBar && w->DockNode->TabBar->SelectedTabId == w->TabId;
        }
        // The asset table's own scroll window (BeginTable + ScrollY makes one,
        // a child of the "##assetscenter" child).
        static ImGuiWindow* TableWindow()
        {
            ImGuiContext& g = *ImGui::GetCurrentContext();
            for (ImGuiWindow* w : g.Windows)
            {
                const std::string n = w->Name;
                const auto center = n.find("##assetscenter");
                if (center != std::string::npos && n.find("##assets", center + 1) != std::string::npos)
                    return w;
            }
            return nullptr;
        }
        int RowIndex(const Guid& g)
        {
            const auto& rows = model.Rows();
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                if (rows[i].type != AssetPanelRow::Type::Group && rows[i].guid == g)
                    return i;
            return -1;
        }
        // Row i spans [header + i*h, header + (i+1)*h) in the table's content;
        // the frozen header covers the first row-height of the view.
        bool RowVisible(const Guid& g)
        {
            ImGuiWindow* t = TableWindow();
            const int i = RowIndex(g);
            if (!t || i < 0)
                return false;
            const float top = kTableRowHeight * static_cast<float>(i + 1);
            const float bottom = top + kTableRowHeight;
            const float viewTop = t->Scroll.y + kTableRowHeight;
            const float viewBottom = t->Scroll.y + t->InnerRect.GetHeight();
            UNSCOPED_INFO("row " << i << " [" << top << "," << bottom << ") view [" << viewTop << "," << viewBottom
                          << ") scrollMax " << t->ScrollMax.y);
            return top >= viewTop - 0.5f && bottom <= viewBottom + 0.5f;
        }
    };
}

// The full sequence from the shipped layout's usual state: the Browser is a
// buried tab, its search hides the target and the target's folder is closed.
TEST_CASE("Reveal in Browser: from a buried tab the Browser comes forward, clears the search, opens the folder and shows the row", "[editor][reveal]")
{
    RevealHarness h("arcane_reveal_buried_test");
    h.Frames(2);
    h.Frame([] { FocusTab("Asset Graph"); });
    std::snprintf(h.state.search, sizeof(h.state.search), "m0");
    h.state.groupOpen["zz/deep/"] = false;
    h.model.SetGroupOpen("zz/deep/", false);
    h.Frames(3);
    REQUIRE_FALSE(RevealHarness::BrowserTabSelected());
    REQUIRE(h.RowIndex(h.target) < 0);

    h.Reveal(h.target);
    h.Frames(3);
    CHECK(RevealHarness::BrowserTabSelected());
    CHECK(h.state.search[0] == '\0');
    CHECK(h.state.groupOpen.at("zz/deep/"));
    CHECK(h.model.selected == h.target);
    CHECK(h.RowVisible(h.target));
    CHECK_FALSE(h.state.revealPending);
}

// The Browser row menu's own Reveal: the opening right-click selected the row
// (stamp spent while it was on screen), then Reveal clears the search and the
// row lands 60 rows further down.
TEST_CASE("Reveal in Browser: the row its own right-click selected is followed after the search clears", "[editor][reveal]")
{
    RevealHarness h("arcane_reveal_rowmenu_test");
    h.Frames(2);
    h.Frame([] { FocusTab("Asset Browser"); });
    std::snprintf(h.state.search, sizeof(h.state.search), "target");
    h.Frames(3);
    h.model.Select(h.target);
    h.Frames(3);
    REQUIRE(h.RowVisible(h.target));

    h.Reveal(h.target);
    h.Frames(3);
    CHECK(h.state.search[0] == '\0');
    CHECK(h.RowVisible(h.target));
}

// The Asset Graph's node menu with the Browser on screen: the right-click
// selected the asset while its folder was closed, so the Browser spent the
// stamp on "nothing to scroll to"; Reveal then opens the folder.
TEST_CASE("Reveal in Browser: a row selected while its folder was closed is scrolled to once the folder opens", "[editor][reveal]")
{
    RevealHarness h("arcane_reveal_closedfolder_test");
    h.Frames(2);
    h.Frame([] { FocusTab("Asset Browser"); });
    h.state.groupOpen["zz/deep/"] = false;
    h.model.SetGroupOpen("zz/deep/", false);
    h.Frames(2);
    h.model.Select(h.target);
    h.Frames(3);
    REQUIRE(h.state.seenSelectionStamp == h.model.selectionStamp);

    h.Reveal(h.target);
    h.Frames(3);
    CHECK(h.RowVisible(h.target));
}

TEST_CASE("Reveal in Browser: an already-selected row scrolled out of view comes back", "[editor][reveal]")
{
    RevealHarness h("arcane_reveal_scrolledaway_test");
    h.Frames(2);
    h.Frame([] { FocusTab("Asset Browser"); });
    h.model.Select(h.target);
    h.Frames(3);
    REQUIRE(h.RowVisible(h.target));
    h.Frame([] { if (ImGuiWindow* t = RevealHarness::TableWindow()) ImGui::SetScrollY(t, 0.0f); });
    h.Frames(2);
    REQUIRE_FALSE(h.RowVisible(h.target));

    h.Reveal(h.target);
    h.Frames(3);
    CHECK(h.RowVisible(h.target));
}

// Rows 7 and 8 (m05, m06): the first is cut by the bottom edge, the second is
// wholly below it. The old row-index test called both visible.
TEST_CASE("Reveal in Browser: a row at or just below the bottom edge scrolls fully into view", "[editor][reveal]")
{
    RevealHarness h("arcane_reveal_edge_test");
    h.Frames(2);
    h.Frame([] { FocusTab("Asset Browser"); });
    h.Frames(2);
    for (const int n : { 105, 106 })
    {
        const Guid g = RevealGuid(n);
        h.Frame([] { if (ImGuiWindow* t = RevealHarness::TableWindow()) ImGui::SetScrollY(t, 0.0f); });
        h.Frames(2);
        REQUIRE_FALSE(h.RowVisible(g));
        h.Reveal(g);
        h.Frames(3);
        INFO("m" << (n - 100) << " (row " << h.RowIndex(g) << ")");
        CHECK(h.RowVisible(g));
    }
}

// The ordinary (non-Reveal) selection keeps its rule: a row the mouse could
// click (any part on screen) never re-centres, and a row wholly out of view
// is scrolled to.
TEST_CASE("Asset Browser: an ordinary selection scrolls only for a row wholly out of view", "[editor][reveal]")
{
    RevealHarness h("arcane_reveal_ordinary_test");
    h.Frames(2);
    h.Frame([] { FocusTab("Asset Browser"); });
    h.Frames(2);
    ImGuiWindow* t = RevealHarness::TableWindow();
    REQUIRE(t != nullptr);

    h.model.Select(RevealGuid(105));   // row 7: cut by the bottom edge
    h.Frames(3);
    CHECK(t->Scroll.y == 0.0f);

    h.model.Select(RevealGuid(106));   // row 8: wholly below it
    h.Frames(3);
    CHECK(t->Scroll.y > 0.0f);
    CHECK(h.RowVisible(RevealGuid(106)));
}

// The menu entry point (DrawAssetMenuItems is the one body behind the Browser
// row menu and the Asset Graph node menu), driven inside a real popup: the
// item raises revealInBrowse, the action every Reveal entry point feeds the
// host consumer replayed above, and it is disabled while the Browser is closed.
TEST_CASE("Reveal in Browser: the asset menu item raises revealInBrowse, disabled while the Browser is closed", "[editor][reveal]")
{
    RevealHarness h("arcane_reveal_menuitem_test");
    const AssetPanelEntry* e = h.model.Find(h.target);
    REQUIRE(e != nullptr);
    AssetPanelActions raised;
    ImGuiID popupWindowId = 0;
    ImGuiID activate = 0;
    const auto menuFrame = [&]
    {
        h.Frame([&]
        {
            ImGui::Begin("menuhost");
            if (!ImGui::IsPopupOpen("##assetctx"))
                ImGui::OpenPopup("##assetctx");
            if (ImGui::BeginPopup("##assetctx"))
            {
                popupWindowId = ImGui::GetCurrentWindow()->ID;
                DrawAssetMenuItems(raised, *e, /*kindSpecific=*/true, h.services);
                ImGui::EndPopup();
            }
            ImGui::End();
            if (activate) { ImGui::ActivateItemByID(activate); activate = 0; }   // lands next frame
        });
    };
    for (int i = 0; i < 3; ++i) menuFrame();
    REQUIRE(popupWindowId != 0);

    activate = ImHashStr("Reveal in Browser", 0, popupWindowId);
    for (int i = 0; i < 3; ++i) menuFrame();
    CHECK(raised.revealInBrowse == h.target);

    raised = {};
    h.services.browserOpen = false;
    for (int i = 0; i < 2; ++i) menuFrame();
    activate = ImHashStr("Reveal in Browser", 0, popupWindowId);
    for (int i = 0; i < 3; ++i) menuFrame();
    CHECK_FALSE(raised.revealInBrowse.IsValid());
}
