// A dock node's first tab sits flush with the node's left edge when the node
// has no left window-menu button (Arcane's dockspace: NoWindowMenuButton). This
// pins the ARCANE LOCAL FIX in ThirdParty/imgui/imgui.cpp DockNodeCalcTabBarLayout
// (user desk, 2026-10-02: "the tab is inset a bit to the right"); upstream always
// insets the tab bar by FramePadding.x. A node WITH the left menu button keeps
// upstream's padding before its button.

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <utility>

namespace
{
    struct DockHarness
    {
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;

        DockHarness()
        {
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
        ~DockHarness()
        {
            ImGui::DestroyContext(ctx);
            ImGui::SetCurrentContext(prev);
        }

        // Two tabs in one dockspace node; returns that node after `frames` frames.
        bool openA = true, openB = true;
        // When set, both windows push this ImGuiCol_Text around their Begin --
        // the Problems/Console alert tint (node-page phase s8.2).
        const ImVec4* labelTint = nullptr;

        // closable: the two tabs carry close buttons (Begin with p_open).
        ImGuiDockNode* Run(ImGuiDockNodeFlags spaceFlags, int frames = 3, bool closable = false)
        {
            const ImGuiID dockId = ImHashStr("InsetDock");
            for (int i = 0; i < frames; ++i)
            {
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                if (i == 0)
                {
                    ImGui::DockBuilderRemoveNode(dockId);
                    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace | spaceFlags);
                    ImGui::DockBuilderSetNodeSize(dockId, ImVec2(600.0f, 300.0f));
                    ImGui::DockBuilderDockWindow("Inset A", dockId);
                    ImGui::DockBuilderDockWindow("Inset B", dockId);
                    ImGui::DockBuilderFinish(dockId);
                }
                ImGui::SetNextWindowPos(ImVec2(40.0f, 30.0f));
                ImGui::SetNextWindowSize(ImVec2(600.0f, 300.0f));
                ImGui::Begin("##insethost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
                ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), spaceFlags);
                ImGui::End();
                const auto begin = [&](const char* name, bool* open)
                {
                    if (labelTint) ImGui::PushStyleColor(ImGuiCol_Text, *labelTint);
                    ImGui::Begin(name, open);
                    if (labelTint) ImGui::PopStyleColor();
                };
                begin("Inset A", closable ? &openA : nullptr); ImGui::TextUnformatted("a"); ImGui::End();
                begin("Inset B", closable ? &openB : nullptr); ImGui::TextUnformatted("b"); ImGui::End();
                ImGui::Render();
            }
            return ImGui::DockBuilderGetNode(dockId);
        }
    };
}

TEST_CASE("Dock tab bar: no left window-menu button -> the first tab is flush with the node's left edge", "[editor][docking]")
{
    DockHarness h;
    ImGuiDockNode* node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton);
    REQUIRE(node != nullptr);
    REQUIRE(node->TabBar != nullptr);
    // Nothing separates the tab bar from the node edge -- no border, no FramePadding.x
    // (user desk, 2026-10-02: the remaining 1 px was WindowBorderSize).
    CHECK(node->TabBar->BarRect.Min.x == node->Pos.x);
}

TEST_CASE("Dock tab bar: a left window-menu button keeps upstream's padding before it", "[editor][docking]")
{
    DockHarness h;
    ImGuiDockNode* node = h.Run(ImGuiDockNodeFlags_None);
    REQUIRE(node != nullptr);
    REQUIRE(node->TabBar != nullptr);
    const ImGuiStyle& style = ImGui::GetStyle();
    REQUIRE(node->HasWindowMenuButton);
    // Border + padding + the button + its spacing, exactly as upstream lays it out.
    CHECK(node->TabBar->BarRect.Min.x ==
          node->Pos.x + style.WindowBorderSize + style.FramePadding.x + ImGui::GetFontSize() + style.ItemInnerSpacing.x);
}

namespace
{
    // How many vertices of `col` lie inside `r` in this frame's draw data.
    int CountVerticesOfColor(const ImRect& r, ImU32 col)
    {
        int n = 0;
        const ImDrawData* dd = ImGui::GetDrawData();
        for (const ImDrawList* list : dd->CmdLists)
            for (const ImDrawVert& v : list->VtxBuffer)
                if (v.col == col && r.Contains(v.pos))
                    ++n;
        return n;
    }
}

TEST_CASE("Dock tab labels: the unselected tab's label is dim (TextDisabled), the selected tab's is Text", "[editor][docking]")
{
    // The ARCANE LOCAL FIX in imgui_widgets.cpp TabItemLabelAndCloseButton
    // (user desk, 2026-10-02: Visual Studio tab language).
    DockHarness h;
    ImGui::GetIO().MousePos = ImVec2(-10000.0f, -10000.0f);   // nothing hovered
    ImGuiDockNode* node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton);
    REQUIRE(node != nullptr);
    REQUIRE(node->TabBar != nullptr);
    REQUIRE(node->TabBar->Tabs.Size == 2);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim  = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    REQUIRE(text != dim);
    const ImRect bar = node->TabBar->BarRect;
    int checkedSelected = 0, checkedUnselected = 0;
    for (const ImGuiTabItem& tab : node->TabBar->Tabs)
    {
        const ImRect r(ImVec2(bar.Min.x + tab.Offset, bar.Min.y), ImVec2(bar.Min.x + tab.Offset + tab.Width, bar.Max.y));
        if (tab.ID == node->TabBar->SelectedTabId)
        {
            CHECK(CountVerticesOfColor(r, text) > 0);
            CHECK(CountVerticesOfColor(r, dim) == 0);
            ++checkedSelected;
        }
        else
        {
            CHECK(CountVerticesOfColor(r, dim) > 0);
            CHECK(CountVerticesOfColor(r, text) == 0);
            ++checkedUnselected;
        }
    }
    CHECK(checkedSelected == 1);
    CHECK(checkedUnselected == 1);
}

TEST_CASE("Dock tab labels: hovering an unselected tab brightens its label to Text", "[editor][docking]")
{
    DockHarness h;
    ImGui::GetIO().MousePos = ImVec2(-10000.0f, -10000.0f);
    ImGuiDockNode* node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton);
    REQUIRE(node != nullptr);
    REQUIRE(node->TabBar != nullptr);
    const ImGuiTabItem* unselected = nullptr;
    for (const ImGuiTabItem& tab : node->TabBar->Tabs)
        if (tab.ID != node->TabBar->SelectedTabId)
            unselected = &tab;
    REQUIRE(unselected != nullptr);
    const ImRect bar = node->TabBar->BarRect;
    const ImRect r(ImVec2(bar.Min.x + unselected->Offset, bar.Min.y),
                   ImVec2(bar.Min.x + unselected->Offset + unselected->Width, bar.Max.y));
    ImGui::GetIO().MousePos = r.GetCenter();                // hover it (no click)
    node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton);
    REQUIRE(node != nullptr);
    CHECK(CountVerticesOfColor(r, ImGui::GetColorU32(ImGuiCol_Text)) > 0);
    CHECK(CountVerticesOfColor(r, ImGui::GetColorU32(ImGuiCol_TextDisabled)) == 0);
}

TEST_CASE("Dock tab fill: hovering the SELECTED tab keeps its fill (no hover lift)", "[editor][docking]")
{
    DockHarness h;
    ImGui::GetIO().MousePos = ImVec2(-10000.0f, -10000.0f);
    ImGuiDockNode* node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton);
    REQUIRE(node != nullptr);
    REQUIRE(node->TabBar != nullptr);
    const ImGuiTabItem* selected = nullptr;
    for (const ImGuiTabItem& tab : node->TabBar->Tabs)
        if (tab.ID == node->TabBar->SelectedTabId)
            selected = &tab;
    REQUIRE(selected != nullptr);
    const ImRect bar = node->TabBar->BarRect;
    const ImRect r(ImVec2(bar.Min.x + selected->Offset, bar.Min.y),
                   ImVec2(bar.Min.x + selected->Offset + selected->Width, bar.Max.y));
    ImGui::GetIO().MousePos = r.GetCenter();                // hover it (no click)
    node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton);
    REQUIRE(node != nullptr);
    REQUIRE(ImGui::GetColorU32(ImGuiCol_TabHovered) != ImGui::GetColorU32(ImGuiCol_TabSelected));
    REQUIRE(ImGui::GetColorU32(ImGuiCol_TabHovered) != ImGui::GetColorU32(ImGuiCol_TabDimmedSelected));
    CHECK(CountVerticesOfColor(r, ImGui::GetColorU32(ImGuiCol_TabHovered)) == 0);
    // Focused or not, the fill is one of the two SELECTED tones.
    CHECK(CountVerticesOfColor(r, ImGui::GetColorU32(ImGuiCol_TabSelected)) +
          CountVerticesOfColor(r, ImGui::GetColorU32(ImGuiCol_TabDimmedSelected)) > 0);
}

TEST_CASE("Dock tabs butt together: no gap between neighbouring tabs", "[editor][docking]")
{
    // ARCANE LOCAL FIX in imgui_widgets.cpp TabBarLayout (user desk, 2026-10-02):
    // upstream spaces tabs by ItemInnerSpacing.x.
    DockHarness h;
    ImGui::GetIO().MousePos = ImVec2(-10000.0f, -10000.0f);
    ImGuiDockNode* node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton);
    REQUIRE(node != nullptr);
    REQUIRE(node->TabBar != nullptr);
    REQUIRE(node->TabBar->Tabs.Size == 2);
    const ImGuiTabItem* a = &node->TabBar->Tabs[0];
    const ImGuiTabItem* b = &node->TabBar->Tabs[1];
    if (b->Offset < a->Offset)
        std::swap(a, b);
    CHECK(a->Offset == 0.0f);
    CHECK(b->Offset == a->Offset + a->Width);
}

TEST_CASE("Dock tab hover: the mouse on an unselected tab's close button keeps the whole tab lifted", "[editor][docking]")
{
    // ARCANE LOCAL FIX in imgui_widgets.cpp TabItemEx (user desk, 2026-10-02):
    // upstream drops the tab's TabHovered fill while the mouse is on its X.
    DockHarness h;
    ImGui::GetIO().MousePos = ImVec2(-10000.0f, -10000.0f);
    ImGuiDockNode* node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton, 3, true);
    REQUIRE(node != nullptr);
    REQUIRE(node->TabBar != nullptr);
    const ImGuiTabItem* unselected = nullptr;
    for (const ImGuiTabItem& tab : node->TabBar->Tabs)
        if (tab.ID != node->TabBar->SelectedTabId)
            unselected = &tab;
    REQUIRE(unselected != nullptr);
    const ImGuiID tabId = unselected->ID;
    const ImRect bar = node->TabBar->BarRect;
    const ImRect r(ImVec2(bar.Min.x + unselected->Offset, bar.Min.y),
                   ImVec2(bar.Min.x + unselected->Offset + unselected->Width, bar.Max.y));
    // The close button's centre, as TabItemLabelAndCloseButton places it.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float sz = ImGui::GetFontSize();
    ImGui::GetIO().MousePos = ImVec2(r.Max.x - style.FramePadding.x - sz * 0.5f, r.Min.y + style.FramePadding.y + sz * 0.5f);
    node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton, 3, true);
    REQUIRE(node != nullptr);
    // The mouse is on the X (something other than the tab itself is hovered) ...
    REQUIRE(GImGui->HoveredId != 0);
    REQUIRE(GImGui->HoveredId != tabId);
    // ... and the tab is still painted in its hover fill.
    CHECK(CountVerticesOfColor(r, ImGui::GetColorU32(ImGuiCol_TabHovered)) > 0);
}

TEST_CASE("Dock tab labels: a window that tints its own label keeps the tint on an unselected tab", "[editor][docking]")
{
    // ARCANE LOCAL FIX in imgui.cpp DockNodeUpdateTabBar + imgui_widgets.cpp
    // TabItemLabelAndCloseButton (node-page phase s8.2): the unselected-label dim
    // is for the theme's own label colour only; a window that pushed ImGuiCol_Text
    // around its Begin (the Problems/Console alert tint) keeps it.
    DockHarness h;
    const ImVec4 amber(0.950f, 0.770f, 0.300f, 1.00f);
    h.labelTint = &amber;
    ImGui::GetIO().MousePos = ImVec2(-10000.0f, -10000.0f);   // nothing hovered
    ImGuiDockNode* node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton);
    REQUIRE(node != nullptr);
    REQUIRE(node->TabBar != nullptr);
    const ImGuiTabItem* unselected = nullptr;
    for (const ImGuiTabItem& tab : node->TabBar->Tabs)
        if (tab.ID != node->TabBar->SelectedTabId)
            unselected = &tab;
    REQUIRE(unselected != nullptr);
    const ImU32 tint = ImGui::ColorConvertFloat4ToU32(amber);
    const ImU32 dim  = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    REQUIRE(tint != dim);
    const ImRect bar = node->TabBar->BarRect;
    const ImRect r(ImVec2(bar.Min.x + unselected->Offset, bar.Min.y),
                   ImVec2(bar.Min.x + unselected->Offset + unselected->Width, bar.Max.y));
    CHECK(CountVerticesOfColor(r, tint) > 0);
    CHECK(CountVerticesOfColor(r, dim) == 0);
}

TEST_CASE("Docked window: no WindowPadding -- content runs to the panel's edges; undocked windows keep the style's", "[editor][docking]")
{
    // ARCANE LOCAL FIX in ThirdParty/imgui/imgui.cpp Begin (user desk, 2026-10-03: "a thin
    // margin or padding around the interior of every panel"). Popups, tooltips, modals and
    // floating windows still read style.WindowPadding.
    DockHarness h;
    ImGuiDockNode* node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton);
    REQUIRE(node != nullptr);
    const ImGuiStyle& style = ImGui::GetStyle();
    REQUIRE(style.WindowPadding.x > 0.0f);
    REQUIRE(style.WindowPadding.y > 0.0f);

    ImGuiWindow* docked = ImGui::FindWindowByName("Inset A");
    REQUIRE(docked != nullptr);
    REQUIRE(docked->DockIsActive);
    CHECK(docked->WindowPadding.x == 0.0f);
    CHECK(docked->WindowPadding.y == 0.0f);
    CHECK(docked->DC.CursorStartPos.x == docked->Pos.x);

    ImGuiWindow* floating = ImGui::FindWindowByName("##insethost");
    REQUIRE(floating != nullptr);
    REQUIRE_FALSE(floating->DockIsActive);
    CHECK(floating->WindowPadding.x == style.WindowPadding.x);
    CHECK(floating->WindowPadding.y == style.WindowPadding.y);
}
