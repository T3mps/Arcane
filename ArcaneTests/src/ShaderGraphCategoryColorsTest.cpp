// Node page spec 2026-09-30 s5.1.4 / s5.1.11 "Colours": the per-category node
// header fill (critique Shader #7) must keep the title text readable, and the
// Output sink must stand out. Pure: no ImGui context.
#include <catch2/catch_test_macros.hpp>
#include <Documents/ShaderGraphCategoryColors.hpp>
#include <Widgets/EditorTheme.hpp>
#include <cmath>

using namespace Arcane::Editor;

TEST_CASE("Graph category colours: every category reads its title text at >= 4.5:1 and Output stands out", "[editor][graphcanvas][nodepage]")
{
    constexpr ImVec4 kTitleText(0.808f, 0.808f, 0.831f, 1.0f);   // kNodeTitleText, ShaderEditorDocument.cpp (#cecfd4)
    using C = Arcane::GraphNodeCategory;
    const C all[] = { C::Uncategorized, C::Input, C::Math, C::Vector, C::Procedural, C::Output, C::Utility };
    for (const C c : all)
    {
        INFO(Arcane::GraphNodeCategoryName(c));
        CHECK(Theme::ContrastRatio(GraphCategoryHeaderColor(c), kTitleText) >= 4.5f);
    }
    const ImVec4 out = GraphCategoryHeaderColor(C::Output);
    for (const C c : all)
    {
        if (c == C::Output) continue;
        const ImVec4 o = GraphCategoryHeaderColor(c);
        INFO(Arcane::GraphNodeCategoryName(c));
        CHECK_FALSE((o.x == out.x && o.y == out.y && o.z == out.z));
    }
    // Uncategorized is today's kNodeTitleColor (#232326), so an unset row looks unchanged.
    CHECK(GraphCategoryHeaderColor(C::Uncategorized).x == 0x23 / 255.0f);
    CHECK(GraphCategoryHeaderColor(C::Uncategorized).z == 0x26 / 255.0f);
}

TEST_CASE("Theme::ContrastRatio is the WCAG ratio: 21 for black on white, 1 for a colour on itself, symmetric", "[editor][theme]")
{
    const ImVec4 black(0, 0, 0, 1), white(1, 1, 1, 1), grey(0.5f, 0.5f, 0.5f, 1);
    CHECK(std::abs(Theme::ContrastRatio(black, white) - 21.0f) < 0.01f);
    CHECK(std::abs(Theme::ContrastRatio(grey, grey) - 1.0f) < 0.0001f);
    CHECK(Theme::ContrastRatio(white, grey) == Theme::ContrastRatio(grey, white));
}
