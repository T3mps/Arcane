// EditorWidgets (node-page phase T2, spec 2026-09-30 s4.4/s4.7/s4.8/s4.9):
// the anchored popup, the link widgets, the mono-font scope and the toggle
// colours, driven device-less through the REAL ImGui path -- a bare context at
// 800x600 (ImGuiTest.cpp's shape), one host window covering the display, draw
// data scanned the way PropertyGridTest.cpp scans for Theme::kError.
#include <catch2/catch_test_macros.hpp>

#include <Widgets/EditorWidgets.hpp>

#include <imgui.h>
#include <imgui_internal.h>   // BeginPopupStack / OpenPopupStack / NextWindowData / ColorStack / FontStack

#include <cstdlib>
#include <functional>

using namespace Arcane::Editor;

namespace
{
    struct WidgetHarness
    {
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        std::function<void()> body;   // drawn inside the host window every frame

        WidgetHarness()
        {
            IMGUI_CHECKVERSION();
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(800.0f, 600.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        }
        ~WidgetHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }

        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f), ImGuiCond_Always);
            ImGui::Begin("Host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings);
            if (body) body();
            ImGui::End();
            ImGui::Render();
        }
        void Frames(int n) { for (int i = 0; i < n; ++i) Frame(); }
        void MoveTo(ImVec2 at) { ImGui::GetIO().AddMousePosEvent(at.x, at.y); Frame(); }
        void Click(ImVec2 at, int button = 0)
        {
            MoveTo(at);
            ImGui::GetIO().AddMouseButtonEvent(button, true);  Frame();
            ImGui::GetIO().AddMouseButtonEvent(button, false); Frame();
        }
        // Draw data, not pixels: did the host window emit a vertex in `col` THIS frame?
        bool HostDrew(ImU32 col) const
        {
            for (const ImDrawVert& v : ImGui::FindWindowByName("Host")->DrawList->VtxBuffer)
                if (v.col == col) return true;
            return false;
        }
    };

    ImVec2 Centre(const PopupAnchor& r) { return ImVec2((r.min.x + r.max.x) * 0.5f, (r.min.y + r.max.y) * 0.5f); }

    struct BelowProbe
    {
        PopupAnchor anchor{};
        ImVec2 pos{}, size{};
        bool drawn = false;
        int stackDrift = 0;   // |BeginPopupStack change| across BeginPopupBelow..EndPopup, summed over every frame
    };

    // One button at (40, buttonY) opening "##below" on frame 1, then three
    // measured frames (spec s4.4: "three frames after the open").
    BelowProbe RunBelow(float buttonY, float contentH, float minWidth)
    {
        WidgetHarness h;
        BelowProbe p;
        bool open = true;
        h.body = [&]
        {
            ImGui::SetCursorScreenPos(ImVec2(40.0f, buttonY));
            (void)ImGui::Button("Open##below");
            p.anchor = LastItemAnchor();
            if (open) { ImGui::OpenPopup("##below"); open = false; }
            const int before = h.ctx->BeginPopupStack.Size;
            p.drawn = false;
            if (BeginPopupBelow("##below", p.anchor, minWidth))
            {
                ImGui::Dummy(ImVec2(150.0f, contentH));
                p.pos = ImGui::GetWindowPos();
                p.size = ImGui::GetWindowSize();
                p.drawn = true;
                ImGui::EndPopup();
            }
            p.stackDrift += std::abs(h.ctx->BeginPopupStack.Size - before);
        };
        h.Frame();
        h.Frames(3);
        CHECK(h.ctx->BeginPopupStack.Size == 0);
        CHECK(h.ctx->OpenPopupStack.Size == 1);
        return p;
    }
}

TEST_CASE("LastItemAnchor is the rect of the item just submitted", "[editor][widgets]")
{
    WidgetHarness h;
    PopupAnchor a{}; ImVec2 lo{}, hi{};
    h.body = [&] { ImGui::SetCursorScreenPos(ImVec2(40.0f, 20.0f)); (void)ImGui::Button("Anchor");
                   a = LastItemAnchor(); lo = ImGui::GetItemRectMin(); hi = ImGui::GetItemRectMax(); };
    h.Frame();
    CHECK(a.min.x == lo.x); CHECK(a.min.y == lo.y);
    CHECK(a.max.x == hi.x); CHECK(a.max.y == hi.y);
    CHECK(a.min.y == 20.0f);
}

TEST_CASE("BeginPopupBelow opens under its button, left-aligned with it", "[editor][widgets]")
{
    const BelowProbe p = RunBelow(20.0f, 60.0f, 0.0f);
    REQUIRE(p.drawn);
    CHECK(p.pos.y >= p.anchor.max.y - 0.5f);
    CHECK(std::abs(p.pos.x - p.anchor.min.x) <= 1.0f);
    CHECK(p.stackDrift == 0);
}

TEST_CASE("BeginPopupBelow flips above when there is no room below", "[editor][widgets]")
{
    const BelowProbe p = RunBelow(570.0f, 200.0f, 0.0f);
    REQUIRE(p.drawn);
    CHECK(p.size.y >= 200.0f);
    CHECK(p.pos.y + p.size.y <= p.anchor.min.y + 0.5f);   // above: never covers its own button
    CHECK(std::abs(p.pos.x - p.anchor.min.x) <= 1.0f);
    CHECK(p.stackDrift == 0);
}

TEST_CASE("BeginPopupBelow honours minWidth", "[editor][widgets]")
{
    const BelowProbe p = RunBelow(20.0f, 10.0f, 300.0f);
    REQUIRE(p.drawn);
    CHECK(p.size.x >= 300.0f);
}

TEST_CASE("BeginPopupBelow near the right edge stays on screen, below and right-aligned to its button", "[editor][widgets]")
{
    WidgetHarness h;
    PopupAnchor anchor{}; ImVec2 pos{}, size{};
    bool drawn = false, open = true;
    h.body = [&]
    {
        ImGui::SetCursorScreenPos(ImVec2(700.0f, 20.0f));
        (void)ImGui::Button("Pick...##edge", ImVec2(60.0f, 0.0f));
        anchor = LastItemAnchor();
        if (open) { ImGui::OpenPopup("##edge"); open = false; }
        drawn = false;
        if (BeginPopupBelow("##edge", anchor))
        {
            ImGui::Dummy(ImVec2(260.0f, 40.0f));   // the Pick... search box width
            pos = ImGui::GetWindowPos(); size = ImGui::GetWindowSize(); drawn = true;
            ImGui::EndPopup();
        }
    };
    h.Frames(4);   // the open frame + three measured frames
    REQUIRE(drawn);
    CHECK(pos.x >= 0.0f);
    CHECK(pos.x + size.x <= 800.0f);                              // on screen
    CHECK(pos.y >= anchor.max.y - 0.5f);                          // still below: never covers its own button
    CHECK(std::abs((pos.x + size.x) - anchor.max.x) <= 1.0f);     // ComboBox policy: below, toward left
    CHECK(h.ctx->BeginPopupStack.Size == 0);
}

TEST_CASE("BeginPopupBelow on a closed popup returns false and consumes the next-window data", "[editor][widgets]")
{
    WidgetHarness h;
    bool began = true; int flagsAfter = -1;
    h.body = [&]
    {
        (void)ImGui::Button("Closed");
        const PopupAnchor a = LastItemAnchor();
        ImGui::SetNextWindowSize(ImVec2(10.0f, 10.0f));
        began = BeginPopupBelow("##closed", a);
        if (began) ImGui::EndPopup();
        flagsAfter = static_cast<int>(h.ctx->NextWindowData.HasFlags);
    };
    h.Frames(2);
    CHECK_FALSE(began);
    CHECK(flagsAfter == 0);
    CHECK(h.ctx->BeginPopupStack.Size == 0);
}

TEST_CASE("LinkText: a live link clicks once and shows the hand cursor", "[editor][widgets]")
{
    WidgetHarness h;
    int clicks = 0; PopupAnchor rect{};
    h.body = [&] { ImGui::SetCursorScreenPos(ImVec2(40.0f, 40.0f));
                   if (LinkText("Player.arcinput##live")) ++clicks;
                   rect = LastItemAnchor(); };
    h.Frame();
    h.MoveTo(Centre(rect));
    CHECK(ImGui::GetMouseCursor() == ImGuiMouseCursor_Hand);
    h.Click(Centre(rect));
    CHECK(clicks == 1);
}

TEST_CASE("LinkText: a dead link never fires and keeps the arrow", "[editor][widgets]")
{
    WidgetHarness h;
    int clicks = 0; PopupAnchor rect{};
    h.body = [&] { ImGui::SetCursorScreenPos(ImVec2(40.0f, 40.0f));
                   if (LinkText("missing.txt##dead", false)) ++clicks;
                   rect = LastItemAnchor(); };
    h.Frame();
    CHECK(rect.max.x - rect.min.x > 0.0f);   // sized to its text: a real hover target
    h.MoveTo(Centre(rect));
    CHECK(ImGui::GetMouseCursor() != ImGuiMouseCursor_Hand);
    h.Click(Centre(rect));
    CHECK(clicks == 0);
}

TEST_CASE("LinkRow: a live row highlights, shows the hand and reports the click", "[editor][widgets]")
{
    WidgetHarness h;
    LinkRowResult last; int clicks = 0; PopupAnchor rect{};
    h.body = [&] { ImGui::SetCursorScreenPos(ImVec2(10.0f, 60.0f));
                   last = LinkRow("row", "Content/brick.png: refused", true);
                   if (last.clicked) ++clicks;
                   rect = LastItemAnchor(); };
    h.Frame();
    const ImU32 hoveredCol = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
    h.MoveTo(Centre(rect));
    CHECK(last.hovered);
    CHECK(ImGui::GetMouseCursor() == ImGuiMouseCursor_Hand);
    CHECK(h.HostDrew(hoveredCol));   // positive control for the dead-row scan below
    h.Click(Centre(rect));
    CHECK(clicks == 1);
}

TEST_CASE("LinkRow: a dead row has no hover highlight and never clicks", "[editor][widgets]")
{
    WidgetHarness h;
    LinkRowResult last; int clicks = 0; PopupAnchor rect{};
    h.body = [&] { ImGui::SetCursorScreenPos(ImVec2(10.0f, 60.0f));
                   last = LinkRow("row", "engine: device lost", false);
                   if (last.clicked) ++clicks;
                   rect = LastItemAnchor(); };
    h.Frame();
    const ImU32 hoveredCol = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
    h.MoveTo(Centre(rect));
    CHECK(last.hovered);                       // hovered is still reported (tooltips need it)
    CHECK_FALSE(h.HostDrew(hoveredCol));
    CHECK(ImGui::GetMouseCursor() != ImGuiMouseCursor_Hand);
    h.Click(Centre(rect));
    CHECK(clicks == 0);
}

namespace
{
    // Right-click the widget; did BeginPopupContextItem submitted RIGHT AFTER it open?
    bool ContextOpensOn(bool row, bool live)
    {
        WidgetHarness h;
        bool opened = false; PopupAnchor rect{};
        h.body = [&]
        {
            ImGui::SetCursorScreenPos(ImVec2(40.0f, 40.0f));
            if (row) (void)LinkRow("ctxrow", "Content/brick.png", live);
            else     (void)LinkText("brick.png##ctx", live);
            rect = LastItemAnchor();
            if (ImGui::BeginPopupContextItem("##linkctx")) { opened = true; ImGui::EndPopup(); }
        };
        h.Frame();
        h.Click(Centre(rect), 1);
        h.Frame();
        return opened;
    }
}

TEST_CASE("LinkText/LinkRow leave the hit item last, so a context menu attaches", "[editor][widgets]")
{
    CHECK(ContextOpensOn(false, true));
    CHECK(ContextOpensOn(false, false));
    CHECK(ContextOpensOn(true, true));
    CHECK(ContextOpensOn(true, false));
}
