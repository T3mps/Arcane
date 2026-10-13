// T3-D1 (2026-10-01 desk): the shader graph's pin colours explain themselves --
// the paint rule for a pin on a resolved graph, the type / tooltip words, and
// the canvas legend's cvar. Pure except the last two cases (the cvar registry;
// a device-less ImGui context for the legend's font metrics).
#include <catch2/catch_test_macros.hpp>

#include <Documents/ShaderGraphPinLegend.hpp>
#include <Documents/ShaderGraphPinTypes.hpp>

#include <Arcane/Config/CVarRegistry.hpp>

#include "Helpers/NodePageDocs.hpp"   // HeadlessImGui

#include <imgui.h>

#include <cstdint>
#include <string>

using namespace Arcane::Editor;

namespace
{
    bool Same(const ImVec4& a, const ImVec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
}

TEST_CASE("Pin type words cover every state: three fixed widths, dynamic resolved to each, and unresolved",
          "[editor][graphcanvas][nodepage]")
{
    CHECK(PinTypeText(1, 0) == "float");
    CHECK(PinTypeText(2, 0) == "float2");
    CHECK(PinTypeText(4, 0) == "float4");
    CHECK(PinTypeText(2, 4) == "float2");                   // a fixed pin ignores the resolution
    CHECK(PinTypeText(0, 1) == "dynamic (now float)");
    CHECK(PinTypeText(0, 2) == "dynamic (now float2)");
    CHECK(PinTypeText(0, 4) == "dynamic (now float4)");
    CHECK(PinTypeText(0, 0) == "dynamic (unresolved)");
    CHECK(std::string(PinWidthName(0)) == "dynamic");      // the one width -> word map
}

TEST_CASE("Pin tooltip: name, type word, then wired from / wired to N input(s) / not connected",
          "[editor][graphcanvas]")
{
    CHECK(PinTooltipText("b", 0, 4, true, "Param 'tint'.out", 0) == "b\ndynamic (now float4)\nwired from Param 'tint'.out");
    CHECK(PinTooltipText("uv", 2, 0, true, "", 0) == "uv\nfloat2\nnot connected");
    CHECK(PinTooltipText("out", 0, 0, false, "", 0) == "out\ndynamic (unresolved)\nnot connected");
    CHECK(PinTooltipText("rgba", 4, 0, false, "", 1) == "rgba\nfloat4\nwired to 1 input");
    CHECK(PinTooltipText("a", 1, 0, false, "", 3) == "a\nfloat\nwired to 3 inputs");
}

TEST_CASE("Pin paint: fixed pins keep their colour, a resolved dynamic pin takes its width's colour plus the ring, an unresolved one stays plain grey",
          "[editor][graphcanvas]")
{
    for (const int w : { 1, 2, 4 })
    {
        INFO("width " << w);
        const GraphPinPaint fixed = PinPaintFor(w, 0);
        CHECK(Same(fixed.color, PinColorForWidth(w)));
        CHECK_FALSE(fixed.adapts);
        CHECK_FALSE(PinPaintFor(w, 4).adapts);              // fixed pins never ring
        const GraphPinPaint resolved = PinPaintFor(0, w);
        CHECK(Same(resolved.color, PinColorForWidth(w)));
        CHECK(resolved.adapts);
    }
    const GraphPinPaint unresolved = PinPaintFor(0, 0);
    CHECK(Same(unresolved.color, PinColorForWidth(0)));
    CHECK_FALSE(unresolved.adapts);
    // No new colours: four, one per value type, all distinct (the defaults,
    // GraphThemeDefaults; editor.theme.graph.pin* may re-tone them).
    namespace D = GraphThemeDefaults;
    CHECK_FALSE(Same(D::kPinScalar, D::kPinVec2));
    CHECK_FALSE(Same(D::kPinScalar, D::kPinVec4));
    CHECK_FALSE(Same(D::kPinVec2, D::kPinVec4));
    CHECK_FALSE(Same(D::kPinDynamic, D::kPinScalar));
    CHECK(Same(PinColorForWidth(0), D::kPinDynamic));   // drawn at the default: the constant itself
}

TEST_CASE("editor.graph.showPinLegend is an Archive Bool cvar defaulting to true; the legend toggle writes it",
          "[editor][graphcanvas]")
{
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    const Arcane::CVarHandle h = reg.Find("editor.graph.showPinLegend");
    REQUIRE_FALSE(h.IsStale());
    const auto explain = reg.Explain("editor.graph.showPinLegend");
    REQUIRE(explain.has_value());
    CHECK(explain->published.type == Arcane::CVarType::Bool);
    CHECK((static_cast<std::uint32_t>(explain->flags) & static_cast<std::uint32_t>(Arcane::CVarFlags::Archive)) != 0u);
    CHECK(explain->help == "Show the shader graph's pin colour legend (false folds it to a ? chip).");
    CHECK(GraphPinLegendShown());   // the default: the full key

    SetGraphPinLegendShown(false);
    reg.Publish();                  // the editor publishes once per frame
    CHECK_FALSE(GraphPinLegendShown());
    CHECK(reg.Explain("editor.graph.showPinLegend")->setBy == Arcane::SetBy::User);   // the layer an archive write keeps
    SetGraphPinLegendShown(true);
    reg.Publish();
    CHECK(GraphPinLegendShown());
}

TEST_CASE("Pin legend: the folded chip is a small square; the open key is wider and two lines tall",
          "[editor][graphcanvas]")
{
    Arcane::Test::HeadlessImGui imgui;
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    const ImVec2 chip = GraphPinLegendBoxSize(false);
    const ImVec2 open = GraphPinLegendBoxSize(true);
    ImGui::EndFrame();
    CHECK(chip.x == chip.y);
    CHECK(open.x > chip.x * 4.0f);   // four type entries on the first line
    CHECK(open.y > chip.y);          // a second line of rules
}
