// Settings arc S4 (spec s7.3, s16.11): one UI scale and one font size; the
// editor's hard pixel sizes derive from them, exactly unchanged at 1.0 / 16.
#include <catch2/catch_test_macros.hpp>
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/GraphLegend.hpp"
#include "Widgets/UiMetrics.hpp"
#include <imgui.h>

using namespace Arcane::Editor;

TEST_CASE("Ui metrics: identity at scale 1 / font 16; scale and font size apply as documented", "[settings-ui][editor]")
{
    const Ui::ScopedMetrics base(Ui::Metrics{});
    CHECK(Ui::Px(8.0f) == 8.0f);
    CHECK(Ui::FontPx(13.0f) == 13.0f);
    CHECK(Ui::TextPx(24.0f) == 24.0f);
    CHECK(PillLineHeight() == 16.0f);
    CHECK(GraphLegendPadX() == 10.0f);
    CHECK(GraphLegendFontPx() == 13.0f);
    {
        const Ui::ScopedMetrics big(Ui::Metrics{ 1.5f, 20.0f });
        CHECK(Ui::Px(8.0f) == 12.0f);
        CHECK(Ui::FontPx(16.0f) == 20.0f);
        CHECK(Ui::TextPx(16.0f) == 30.0f);
        CHECK(PillLineHeight() == 30.0f);
        CHECK(GraphLegendPadX() == 15.0f);
        CHECK(GraphLegendFontPx() == 16.25f);
    }
}

TEST_CASE("Ui metrics: a MeterBar is taller at scale 1.5 than at 1.0, and exactly today's height at 1.0", "[settings-ui][editor]")
{
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600); io.IniFilename = nullptr;
    unsigned char* px = nullptr; int w = 0, h = 0; io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    const MeterSegment segs[2] = { { "a", 1, IM_COL32(255, 0, 0, 255) }, { "b", 2, IM_COL32(0, 255, 0, 255) } };
    const auto height = [&]
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::Begin("M");
        MeterBar("m", segs, 2, 300.0f);
        const float hgt = ImGui::GetItemRectSize().y;
        ImGui::End();
        ImGui::Render();
        return hgt;
    };
    float at1 = 0.0f, at15 = 0.0f;
    { const Ui::ScopedMetrics m(Ui::Metrics{}); at1 = height(); }
    { const Ui::ScopedMetrics m(Ui::Metrics{ 1.5f, 16.0f }); at15 = height(); }
    CHECK(at1 == 12.0f + 6.0f + ImGui::GetTextLineHeight());   // kBarHeight + kLegendGapY + one legend row: today's Dummy
    CHECK(at15 > at1);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}
