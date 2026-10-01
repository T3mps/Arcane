// GraphFit (node-page phase s4.5): the pure fit maths and the
// editor.graph.fitMaxZoom cvar. T2-C4 appends the headless shader-editor case.
//
// imgui_internal.h refuses a TU whose imgui.h came first without
// IMGUI_DEFINE_MATH_OPERATORS, and T2-C4's case needs the node editor's
// internal header -- so the define and that header lead. C4996 is silenced
// around it alone: its vendored crude_json.h (:150) uses std::aligned_storage,
// deprecated in C++23 (STL4034).
#define IMGUI_DEFINE_MATH_OPERATORS
#pragma warning(push)
#pragma warning(disable : 4996)
#include <imgui_node_editor_internal.h>
#pragma warning(pop)

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Widgets/GraphFit.hpp>

#include <Arcane/Config/CVarRegistry.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

using Arcane::Editor::GraphRect;
using Arcane::Editor::ComputeGraphFitRect;
using Catch::Approx;

namespace
{
    float FitZoom(const GraphRect& r, ImVec2 view)
    {
        return std::min(view.x / (r.max.x - r.min.x), view.y / (r.max.y - r.min.y));
    }
    ImVec2 Mid(const GraphRect& r) { return ImVec2((r.min.x + r.max.x) * 0.5f, (r.min.y + r.max.y) * 0.5f); }
}

TEST_CASE("ComputeGraphFitRect: a small graph grows about its centre to the cap", "[editor][graphfit]")
{
    const ImVec2 view(1000.0f, 500.0f);
    const GraphRect content{ ImVec2(100.0f, 200.0f), ImVec2(300.0f, 260.0f) };   // 200x60
    const GraphRect out = ComputeGraphFitRect(content, view, 1.0f);
    CHECK(out.max.x - out.min.x >= 1000.0f - 1e-3f);
    CHECK(out.max.y - out.min.y >= 500.0f - 1e-3f);
    CHECK(FitZoom(out, view) == Approx(1.0f));
    CHECK(Mid(out).x == Approx(200.0f));
    CHECK(Mid(out).y == Approx(230.0f));
}

TEST_CASE("ComputeGraphFitRect: a graph already in range is unchanged", "[editor][graphfit]")
{
    const ImVec2 view(1000.0f, 500.0f);
    const GraphRect content{ ImVec2(0.0f, 0.0f), ImVec2(4000.0f, 1000.0f) };
    const GraphRect out = ComputeGraphFitRect(content, view, 1.0f);
    CHECK(out.min.x == Approx(0.0f)); CHECK(out.min.y == Approx(0.0f));
    CHECK(out.max.x == Approx(4000.0f)); CHECK(out.max.y == Approx(1000.0f));
    CHECK(FitZoom(out, view) == Approx(0.25f));
}

TEST_CASE("ComputeGraphFitRect: a huge graph frames its centre at the 0.1 floor", "[editor][graphfit]")
{
    const ImVec2 view(1000.0f, 500.0f);
    const GraphRect content{ ImVec2(-10000.0f, 0.0f), ImVec2(10000.0f, 1000.0f) };   // 20000x1000
    const GraphRect out = ComputeGraphFitRect(content, view, 1.0f);
    CHECK(out.max.x - out.min.x == Approx(10000.0f));
    CHECK(out.max.y - out.min.y == Approx(1000.0f));
    CHECK(FitZoom(out, view) == Approx(0.1f));
    CHECK(Mid(out).x == Approx(0.0f));
    CHECK(Mid(out).y == Approx(500.0f));
}

TEST_CASE("ComputeGraphFitRect: zero-extent axes give a finite, positive rect", "[editor][graphfit]")
{
    const ImVec2 view(1000.0f, 500.0f);
    for (const GraphRect content : { GraphRect{ ImVec2(50.0f, 0.0f), ImVec2(50.0f, 300.0f) },     // zero width
                                     GraphRect{ ImVec2(0.0f, 40.0f), ImVec2(300.0f, 40.0f) },     // zero height
                                     GraphRect{ ImVec2(7.0f, 9.0f), ImVec2(7.0f, 9.0f) } })       // a point
    {
        const GraphRect out = ComputeGraphFitRect(content, view, 1.0f);
        CHECK(std::isfinite(out.min.x)); CHECK(std::isfinite(out.max.y));
        CHECK(out.max.x - out.min.x > 0.0f);
        CHECK(out.max.y - out.min.y > 0.0f);
        CHECK(FitZoom(out, view) <= 1.0f + 1e-4f);
        CHECK(Mid(out).x == Approx(Mid(content).x));
        CHECK(Mid(out).y == Approx(Mid(content).y));
    }
}

TEST_CASE("ComputeGraphFitRect: a zero-size view leaves the content alone", "[editor][graphfit]")
{
    const GraphRect content{ ImVec2(0.0f, 0.0f), ImVec2(10.0f, 10.0f) };
    const GraphRect out = ComputeGraphFitRect(content, ImVec2(0.0f, 500.0f), 1.0f);
    CHECK(out.min.x == 0.0f); CHECK(out.max.x == 10.0f);
}

TEST_CASE("editor.graph.fitMaxZoom is an Archive Float32 cvar defaulting to 1.0", "[editor][graphfit]")
{
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    const Arcane::CVarHandle h = reg.Find("editor.graph.fitMaxZoom");
    REQUIRE_FALSE(h.IsStale());
    const auto v = reg.Get(h);
    REQUIRE(v.has_value());
    CHECK(v->type == Arcane::CVarType::Float32);
    CHECK(v->AsFloat32() == 1.0f);
    CHECK(Arcane::Editor::GraphFitMaxZoom() == 1.0f);
    const auto explain = reg.Explain("editor.graph.fitMaxZoom");
    REQUIRE(explain.has_value());
    CHECK((static_cast<std::uint32_t>(explain->flags) & static_cast<std::uint32_t>(Arcane::CVarFlags::Archive)) != 0u);
    CHECK(explain->help == "Largest zoom a graph's frame-to-fit may pick (1.0 = never magnify).");
}
