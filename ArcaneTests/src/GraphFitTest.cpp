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

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialGraph.hpp>

#include "Documents/DocumentHost.hpp"
#include "Documents/ShaderEditorDocument.hpp"

#include <filesystem>
#include <initializer_list>
#include <string_view>

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

TEST_CASE("CanvasNavLatch: issues once, confirms on a later draw at the issued size", "[editor][graphfit]")
{
    Arcane::Editor::CanvasNavLatch latch;
    const ImVec2 a(964.0f, 577.0f);
    CHECK_FALSE(latch.Update(a, 0.0, 0.0f));        // not armed: nothing to do
    latch.Arm();
    CHECK(latch.Pending());
    CHECK_FALSE(latch.Update(a, 0.0, 0.0f, /*canIssue*/ false));   // the seed draw cannot issue
    CHECK(latch.Update(a, 0.1, 0.0f));               // issued at a
    CHECK(latch.Pending());                          // not yet landed
    CHECK_FALSE(latch.Update(a, 0.2, 0.0f));         // a held: landed
    CHECK_FALSE(latch.Pending());
    CHECK_FALSE(latch.Update(ImVec2(100.0f, 100.0f), 0.3, 0.0f));   // a later resize is not its business
}

TEST_CASE("CanvasNavLatch: a size change after the issue re-issues (A,A,B and A,B,A)", "[editor][graphfit]")
{
    using Arcane::Editor::CanvasNavLatch;
    const ImVec2 a(964.0f, 577.0f), b(950.0f, 577.0f);
    {
        // A held size, THEN a resize: a backward-looking held-size check
        // consumed the fit on the second a and lost it to b for good.
        CanvasNavLatch latch; latch.Arm();
        CHECK_FALSE(latch.Update(a, 0.0, 0.0f, /*canIssue*/ false));   // seed draw
        CHECK(latch.Update(a, 0.1, 0.0f));                              // issued at a
        CHECK(latch.Update(b, 0.2, 0.0f));                              // b discarded it: re-issued
        CHECK(latch.Pending());
        CHECK_FALSE(latch.Update(b, 0.3, 0.0f));                        // b held: landed
        CHECK_FALSE(latch.Pending());
    }
    {
        CanvasNavLatch latch; latch.Arm();
        CHECK(latch.Update(a, 0.0, 0.0f));   // issued at a
        CHECK(latch.Update(b, 0.1, 0.0f));   // b: discarded by Begin, re-issued at b
        CHECK(latch.Update(a, 0.2, 0.0f));   // a: discarded again, re-issued at a
        CHECK_FALSE(latch.Update(a, 0.3, 0.0f));
        CHECK_FALSE(latch.Pending());
    }
    {
        // A resize on a draw that cannot issue is still seen: the next draw
        // re-issues even though its size equals the original issue size.
        CanvasNavLatch latch; latch.Arm();
        CHECK(latch.Update(a, 0.0, 0.0f));
        CHECK_FALSE(latch.Update(b, 0.1, 0.0f, /*canIssue*/ false));
        CHECK(latch.Pending());
        CHECK(latch.Update(a, 0.2, 0.0f));
        CHECK_FALSE(latch.Update(a, 0.3, 0.0f));
        CHECK_FALSE(latch.Pending());
    }
}

TEST_CASE("CanvasNavLatch: an animated navigation lands only after its settle time", "[editor][graphfit]")
{
    Arcane::Editor::CanvasNavLatch latch; latch.Arm();
    const ImVec2 a(964.0f, 577.0f), b(1184.0f, 637.0f);
    CHECK(latch.Update(a, 1.0, 0.35f));
    CHECK_FALSE(latch.Update(a, 1.2, 0.35f));
    CHECK(latch.Pending());                  // mid-animation
    CHECK(latch.Update(b, 1.25, 0.35f));     // resized mid-flight: re-issued, clock restarts
    CHECK_FALSE(latch.Update(b, 1.5, 0.35f));
    CHECK(latch.Pending());
    CHECK_FALSE(latch.Update(b, 1.6, 0.35f));
    CHECK_FALSE(latch.Pending());
    latch.Arm(); CHECK(latch.Pending()); latch.Disarm(); CHECK_FALSE(latch.Pending());
}

namespace
{
    namespace ne = ax::NodeEditor;

    struct FitRun { bool allInside = true; bool inside[2] = { false, false }; float viewScale = 0.0f; int selectedBefore = -1, selectedAfter = -1; };

    // The New-Graph-Material seed (Output <- ConstColor) at caller positions,
    // through the REAL ShaderEditorDocument::Draw on a device-less frame
    // (GraphCanvasHeadlessTest.cpp's shape). Frame 1 seeds + arms the fit;
    // node 2 is selected. The layout then settles -- the canvas narrows by a
    // scrollbar on frame 2 and widens back on frame 3 -- and the fit, issued
    // from frame 2 on, is re-issued by its CanvasNavLatch on each draw that
    // sees a new canvas size until a draw sees its issue size again; the
    // remaining settle frames draw under the landed view.
    //
    // resizeAtDraw (1-based draw count, 0 = never): just before that draw the
    // document window grows to 1200x700, a canvas RESIZE -- the node editor's
    // Begin then re-centres the PREVIOUS draw's view and discards any
    // navigation still in flight (imgui_node_editor.cpp:1221-1254). Growing,
    // not shrinking: a resize AFTER the fit landed keeps the view's height and
    // widens it (CanvasSizeMode::FitVerticalView), so a landed fit stays framed.
    FitRun RunShaderFit(ImVec2 colorPos, ImVec2 outputPos, std::uint32_t focusNode = 0, int settleFrames = 4,
                        int resizeAtDraw = 0)
    {
        using namespace Arcane;
        MaterialGraph g;
        GraphNode out;   out.id = 1;   out.type = GraphNodeType::Output;     out.posX = outputPos.x; out.posY = outputPos.y;
        GraphNode color; color.id = 2; color.type = GraphNodeType::ConstColor; color.posX = colorPos.x; color.posY = colorPos.y;
        color.value[0] = 0.2f; color.value[1] = 0.8f; color.value[2] = 1.0f; color.value[3] = 1.0f;
        g.nodes = { out, color };
        GraphLink l; l.fromNode = 2; l.toNode = 1; g.links.push_back(l);
        g.nextId = 3;
        MaterialAssetData data;
        data.id = Guid::FromString("dddd5555-5555-4555-8555-555555555555").value();
        data.name = "graphfit";
        data.kind = "sprite";   // a fullscreen document ALWAYS opens on the chain overview (ShaderEditorDocument.cpp:946-956; EnterPass is private), so DrawGraphPanel -- and m_graphCtx -- would never run; a sprite surface has no chain and opens straight onto the graph canvas
        auto gen = GenerateGraphSnippet(g);
        REQUIRE(gen.Ok());
        data.snippet = gen.snippet;
        data.graph = std::move(g);

        IMGUI_CHECKVERSION();
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr; int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

        FitRun run;
        {
            Arcane::Editor::DocServices services{};
            Arcane::Editor::ShaderEditorDocument doc(services, std::filesystem::path("graphfit.arcmat"), std::move(data));
            REQUIRE(doc.IsGraphOwned());
            if (focusNode != 0)
                doc.RequestFocusGraphNode(focusNode);   // the Problems route (EditorAppProject.cpp:248): open + focus in one frame
            int draws = 0;
            auto frame = [&]
            {
                io.DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                if (++draws == resizeAtDraw)
                {
                    // The document's own top-level window (Draw's ImGui::Begin):
                    // the one non-child window besides the implicit Debug window.
                    ImGuiWindow* docWindow = nullptr;
                    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
                        if (!(w->Flags & ImGuiWindowFlags_ChildWindow) && std::string_view(w->Name).rfind("Debug##", 0) != 0)
                            docWindow = w;
                    REQUIRE(docWindow != nullptr);
                    ImGui::SetWindowSize(docWindow->Name, ImVec2(1200.0f, 700.0f));
                }
                bool requestClose = false;
                doc.Draw(requestClose);
                ImGui::Render();
            };
            frame();
            REQUIRE(doc.GraphCanvasContext() != nullptr);
            ne::SetCurrentEditor(doc.GraphCanvasContext());
            ne::SelectNode(ne::NodeId(2));
            run.selectedBefore = ne::GetSelectedObjectCount();
            ne::SetCurrentEditor(nullptr);
            for (int i = 0; i < 1 + settleFrames; ++i)
                frame();

            ne::SetCurrentEditor(doc.GraphCanvasContext());
            auto* editor = reinterpret_cast<ne::Detail::EditorContext*>(doc.GraphCanvasContext());
            const ImRect view = editor->GetViewRect();   // canvas-space visible rect of the last Begin
            for (const std::uint32_t id : { 1u, 2u })
            {
                const ImVec2 pos = ne::GetNodePosition(ne::NodeId(id));
                const ImVec2 size = ne::GetNodeSize(ne::NodeId(id));
                const bool in = size.x > 0.0f && view.Contains(ImRect(pos, pos + size));
                run.inside[id - 1] = in;
                run.allInside = run.allInside && in;
            }
            run.viewScale = 1.0f / ne::GetCurrentZoom();   // GetCurrentZoom is the RECIPROCAL (InvScale, imgui_node_editor_api.cpp:665-668)
            run.selectedAfter = ne::GetSelectedObjectCount();
            ne::SetCurrentEditor(nullptr);
        }
        ImGui::DestroyContext(ctx);
        ImGui::SetCurrentContext(prev);
        return run;
    }
}

TEST_CASE("Shader editor fit-on-open: a spread graph lands inside the canvas, selection untouched", "[editor][graphfit]")
{
    // Both nodes start outside the default (scale 1, origin 0) view.
    const FitRun r = RunShaderFit(ImVec2(-1500.0f, -800.0f), ImVec2(2600.0f, 900.0f));
    CHECK(r.allInside);
    CHECK(r.viewScale <= Arcane::Editor::GraphFitMaxZoom() + 1e-3f);
    CHECK(r.selectedBefore == 1);
    CHECK(r.selectedAfter == 1);
}

TEST_CASE("Shader editor fit-on-open: a tiny off-screen graph is framed at the cap, not magnified", "[editor][graphfit]")
{
    // Uncapped (ed::NavigateToContent) this lands at ~2.5x; the cap holds it at 1.0.
    const FitRun r = RunShaderFit(ImVec2(3000.0f, 3000.0f), ImVec2(3200.0f, 3000.0f));
    CHECK(r.allInside);
    CHECK(r.viewScale <= Arcane::Editor::GraphFitMaxZoom() + 1e-3f);
    CHECK(r.viewScale >= 0.1f - 1e-3f);
    CHECK(r.selectedAfter == r.selectedBefore);
}

TEST_CASE("Shader editor fit-on-open never overrides a Problems focus request", "[editor][graphfit]")
{
    // EditorAppProject.cpp:248 opens the document and calls RequestFocusGraphNode in
    // the same frame. The focus runs before the fit the seed armed and, while it is
    // pending, drops it, so the whole-graph frame never replaces the focused node's
    // frame. (Before the focus was latched this failed even with no fit at all: the
    // canvas resize on the second draw re-centred the old view and discarded the
    // seed-frame focus.)
    // 60 settle frames let the focus animation (style ScrollDuration 0.35 s) finish.
    const FitRun r = RunShaderFit(ImVec2(-1500.0f, -800.0f), ImVec2(2600.0f, 900.0f),
                                  /*focusNode*/ 2u, /*settleFrames*/ 60);
    CHECK(r.inside[1]);           // node 2, the focused one, is on screen
    CHECK_FALSE(r.inside[0]);     // node 1, ~4000 canvas units away, is not: the whole-graph frame did not win
    CHECK(r.selectedAfter == 1);
}

TEST_CASE("Shader editor fit-on-open survives a canvas resize on any settle draw", "[editor][graphfit]")
{
    // The fit must converge however the layout settles: a resize on the draw
    // AFTER the fit was issued discards it (the node editor re-centres the
    // previous draw's view), so the document must notice and re-issue. Each
    // run grows the window on a different draw -- including the draw right
    // after a held-size pair (A,A,B), which a backward-looking held-size check
    // cannot survive.
    for (const int resizeAtDraw : { 2, 3, 4, 5, 6, 7 })
    {
        CAPTURE(resizeAtDraw);
        const FitRun r = RunShaderFit(ImVec2(-1500.0f, -800.0f), ImVec2(2600.0f, 900.0f),
                                      /*focusNode*/ 0u, /*settleFrames*/ 8, resizeAtDraw);
        CHECK(r.allInside);
        CHECK(r.viewScale <= Arcane::Editor::GraphFitMaxZoom() + 1e-3f);
        CHECK(r.selectedAfter == r.selectedBefore);
    }
}

TEST_CASE("A Problems focus survives a canvas resize while it is in flight", "[editor][graphfit]")
{
    // The focus animates (style ScrollDuration 0.35 s); a resize mid-flight
    // freezes the view where the animation stood. The document re-issues it.
    for (const int resizeAtDraw : { 3, 5, 10 })
    {
        CAPTURE(resizeAtDraw);
        const FitRun r = RunShaderFit(ImVec2(-1500.0f, -800.0f), ImVec2(2600.0f, 900.0f),
                                      /*focusNode*/ 2u, /*settleFrames*/ 60, resizeAtDraw);
        CHECK(r.inside[1]);
        CHECK_FALSE(r.inside[0]);
        CHECK(r.selectedAfter == 1);
    }
}
