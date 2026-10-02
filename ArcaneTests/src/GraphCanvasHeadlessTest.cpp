// Slice 9 diagnostic + regression: drive the REAL ShaderEditorDocument::Draw()
// for a GRAPH-OWNED document through a device-less ImGui frame (null backend --
// no window, no GPU; software font atlas). This is the exact path the desk
// crash took (New Graph Material -> doc opens -> first canvas frame), which no
// other device-less test exercises: the imgui-node-editor canvas only runs inside
// a live ImGui frame.

// The R5 record case reads an input pin's bounds through the node editor's
// internal header (Detail::Pin::m_Bounds), as GraphFitTest.cpp does: the define
// and that header lead, with C4996 silenced around it alone (its vendored
// crude_json.h uses std::aligned_storage, deprecated in C++23).
#define IMGUI_DEFINE_MATH_OPERATORS
#pragma warning(push)
#pragma warning(disable : 4996)
#include <imgui_node_editor_internal.h>
#pragma warning(pop)

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialGraph.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Astra/Registry/Registry.hpp>

#include "Documents/DocumentHost.hpp"
#include "Documents/ShaderEditorDocument.hpp"
#include "Documents/ShaderNodeKey.hpp"   // FormatNodeKey: the s5.1.11 canvas cases
#include "Panels/InspectorHost.hpp"      // the harness routes the doc's page like the editor
#include "Panels/InspectorWindows.hpp"
#include "Widgets/PropertyGrid.hpp"
#include "Helpers/NodePageDocs.hpp"   // SpriteNodeDoc / ChainNodeDoc / HeadlessImGui (node page s5.1.11)

#include <imgui.h>
#include <imgui_internal.h>   // OpenPopupStack: the modal-hoist case
#include <imgui_node_editor.h>   // the s5.1.11 canvas cases ask the canvas what is selected

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace Arcane;
using namespace Arcane::Editor;

TEST_CASE("Graph document survives device-less ImGui frames", "[editor][graphcanvas]")
{
    // The New-Graph-Material seed: Color wired to Output (EditorApp's shape).
    MaterialGraph g;
    GraphNode out;
    out.id = 1;
    out.type = GraphNodeType::Output;
    out.posX = 420.0f;
    out.posY = 200.0f;
    GraphNode color;
    color.id = 2;
    color.type = GraphNodeType::ConstColor;
    color.posX = 160.0f;
    color.posY = 200.0f;
    color.value[0] = 0.2f; color.value[1] = 0.8f; color.value[2] = 1.0f; color.value[3] = 1.0f;
    GraphNode custom;
    custom.id = 3;
    custom.type = GraphNodeType::Custom;
    custom.posX = 160.0f;
    custom.posY = 360.0f;
    custom.customPins = { { "x", 1 } };
    custom.customBody = "return float4(x, x, x, 1.0);";
    g.nodes = { out, color, custom };
    GraphLink l;
    l.fromNode = 2;
    l.toNode = 1;
    g.links.push_back(l);
    g.nextId = 4;

    MaterialAssetData data;
    data.id = Guid::FromString("dddd4444-4444-4444-8444-444444444444").value();
    data.name = "graphcanvas";
    data.kind = "fullscreen";
    auto gen = GenerateGraphSnippet(g);
    REQUIRE(gen.Ok());
    data.snippet = gen.snippet;
    data.graph = std::move(g);

    // Device-less ImGui: no backend; a 1x1 white font texture satisfies NewFrame.
    IMGUI_CHECKVERSION();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);   // software build, no upload

    {
        // Device-less services: the ctor skips preview resources; Rebuild
        // no-ops without a compiler/sources -- the CANVAS is what we exercise.
        DocServices services{};
        ShaderEditorDocument doc(services, std::filesystem::path("graphcanvas.arcmat"),
                                 std::move(data));
        REQUIRE(doc.IsGraphOwned());

        // Several frames: frame 1 creates the editor context + seeds node
        // positions; frame 2+ runs the readback path; all draw nodes, pins,
        // links, and the create/delete/context-menu queries.
        for (int frame = 0; frame < 4; ++frame)
        {
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            bool requestClose = false;
            doc.Draw(requestClose);
            CHECK_FALSE(requestClose);
            ImGui::Render();   // draw data discarded -- no backend
        }
        // Doc dtor runs here, inside the live context (DestroyEditor).
    }

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

namespace
{
    // One frame of the document window; the canvas draws inside it.
    void DocFrame(ShaderEditorDocument& doc)
    {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        bool requestClose = false;
        doc.Draw(requestClose);
        ImGui::Render();
    }
}

TEST_CASE("Node page mirror: a restore selects on the canvas with no event, and the canvas keeps it",
          "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;   // before the doc: its dtor needs the context
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    REQUIRE(doc.RestoreSelection("node:0:3"));
    DocFrame(doc);
    DocFrame(doc);
    // The read after ed::End rebuilds the mirror from the canvas EVERY drawn
    // frame: had the Select request not landed, it would read 0 selected
    // nodes and the key would have fallen back to "material".
    CHECK(doc.SelectionKey() == "node:0:3");
    CHECK(doc.SelectionEpoch() == 1);
    REQUIRE(doc.RestoreSelection("material"));       // arms Clear
    DocFrame(doc);
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);
}

TEST_CASE("Node page mirror: a selection the canvas makes itself is ONE event (the Problems locator path)",
          "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    DocFrame(doc);                                   // the context exists, nothing selected
    CHECK(doc.SelectionEpoch() == 1);
    doc.RequestFocusGraphNode(3);                    // ed::SelectNode + NavigateToSelection, not a request
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "node:0:3");
    CHECK(doc.SelectionEpoch() == 2);
    DocFrame(doc);                                   // unchanged selection: no event
    CHECK(doc.SelectionEpoch() == 2);
}

TEST_CASE("Node page mirror: undo/redo of the selected node with the canvas drawing -- material with no event, then the node again",
          "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    REQUIRE(doc.RestoreSelection("node:0:4"));
    DocFrame(doc);
    DocFrame(doc);
    REQUIRE(doc.SelectionKey() == "node:0:4");
    std::optional<Arcane::MaterialGraph> with = Arcane::Test::SpriteNodeDoc().graph;
    std::optional<Arcane::MaterialGraph> without = with;
    std::erase_if(without->nodes, [](const Arcane::GraphNode& n) { return n.id == 4; });
    doc.ApplyGraphState(0, without);                 // GraphEditCommand::Undo's path
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);
    doc.ApplyGraphState(0, with);                    // Redo restores the same id
    DocFrame(doc);
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "node:0:4");
    CHECK(doc.SelectionEpoch() == 1);
}

TEST_CASE("Node page mirror: a pass switch is not an event, and the fresh canvas resets the mirror",
          "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    ShaderEditorDocument doc(DocServices{}, "chain.arcmat", Arcane::Test::ChainNodeDoc());
    REQUIRE(doc.RestoreSelection("node:0:2"));       // leaves the overview (EnterPass(0))
    DocFrame(doc);
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "node:0:2");
    CHECK(doc.SelectionEpoch() == 1);
    ShaderEditorDocument::PassListState s = doc.CapturePassListState();
    s.activePass = 1;
    doc.ApplyPassListState(std::move(s));            // the rebuilt context starts empty
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "material");
    CHECK(doc.SelectionEpoch() == 1);
}

TEST_CASE("Node page mirror: restoring a node in another pass switches the canvas and the selection survives the rebuild",
          "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    ShaderEditorDocument doc(DocServices{}, "chain.arcmat", Arcane::Test::ChainNodeDoc());
    REQUIRE(doc.RestoreSelection("node:0:2"));
    DocFrame(doc);
    DocFrame(doc);
    REQUIRE(doc.SelectionKey() == "node:0:2");
    REQUIRE(doc.RestoreSelection("node:1:2"));       // EnterPass(1): the canvas context is rebuilt next frame
    DocFrame(doc);
    DocFrame(doc);
    CHECK(doc.SelectionKey() == "node:1:2");         // the Select landed AFTER the switch's ClearSelection
    CHECK(doc.SelectionEpoch() == 1);                // a restore is never an event
}

TEST_CASE("Node page modal hoist: the canvas still opens the HLSL body editor", "[editor][graphcanvas][nodepage]")
{
    Arcane::Test::HeadlessImGui imgui;
    ShaderEditorDocument doc(DocServices{}, "nodes.arcmat", Arcane::Test::SpriteNodeDoc());
    DocFrame(doc);
    doc.RequestBodyEdit(0, 4);
    DocFrame(doc);
    bool open = false;
    for (const ImGuiPopupData& p : imgui.ctx->OpenPopupStack)
        open = open || (p.Window && std::string(p.Window->Name).find("Edit HLSL") != std::string::npos);
    CHECK(open);
}

// ---- Node page s5.1.11 canvas acceptance: the selection mirror (s5.1.1) seen
// from the canvas side -- what imgui-node-editor itself holds selected, a real
// mouse click, a programmatic multi-select, pass switches and the chain view. ----
namespace
{
    namespace ed = ax::NodeEditor;

    // Device-less ImGui (HeadlessImGui, FIRST member so it destructs last) + a
    // document drawn through its REAL Draw() (the canvas), one DocFrame per frame.
    struct CanvasHarness
    {
        Arcane::Test::HeadlessImGui imgui;
        // A real undo stack over an EMPTY registry (ShaderEditorDocumentTest's
        // fixture): the node page's live rows open gestures on it.
        Astra::Registry registry;
        Arcane::CommandStack stack{ [this]() -> Astra::Registry& { return registry; } };
        std::unique_ptr<ShaderEditorDocument> doc;

        explicit CanvasHarness(MaterialAssetData data)
            : doc(std::make_unique<ShaderEditorDocument>(Services(&stack), std::filesystem::path("canvas.arcmat"), std::move(data)))
        {
        }
        static DocServices Services(Arcane::CommandStack* stack)
        {
            DocServices services{};
            services.undo = [stack]() -> Arcane::CommandStack* { return stack; };
            return services;
        }
        ~CanvasHarness() { doc.reset(); }   // inside the context (DestroyEditor)
        CanvasHarness(const CanvasHarness&) = delete;
        CanvasHarness& operator=(const CanvasHarness&) = delete;

        // The editor's frame order (EditorAppFrame.cpp: DrawAll, then the
        // epoch -> NotifySelected loop, then DrawInspectorWindows). PageMode:
        // None = the canvas alone; Plain = the document's CURRENT page in a
        // bare window; Host = the real InspectorHost + DrawInspectorWindows
        // over a real undo stack, the way the editor routes and draws it.
        enum class PageMode { None, Plain, Host };
        PageMode pageMode = PageMode::None;
        PropertyGridState grid;
        struct NullSource final : InspectorSource
        {
            std::string SourceName() const override { return "Scene"; }
            std::string_view Kind() const override { return "scene"; }
            InspectorPage* Page() override { return nullptr; }
            InspectorPage* PageFor(std::string_view) override { return nullptr; }
            std::string SelectionKey() const override { return {}; }
            bool RestoreSelection(std::string_view) override { return false; }
            bool Resolves(std::string_view) const override { return false; }
        } scene;
        InspectorHost host{ scene };
        InspectorWindowsState windows;
        std::uint64_t lastEpoch = 0;

        void UseHost()
        {
            pageMode = PageMode::Host;
            host.AddSource(*doc);
        }

        void Frame(int n = 1)
        {
            for (int i = 0; i < n; ++i)
            {
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                bool requestClose = false;
                doc->Draw(requestClose);
                if (pageMode == PageMode::Plain)
                {
                    PropertyGrid(grid).CommitOrphans();
                    ImGui::SetNextWindowPos(ImVec2(1060.0f, 0.0f));
                    ImGui::SetNextWindowSize(ImVec2(220.0f, 720.0f));
                    (void)ImGui::Begin("Inspector");
                    if (InspectorPage* page = doc->Page())
                    {
                        PropertyGrid g(grid);
                        page->Draw(g);
                    }
                    ImGui::End();
                }
                else if (pageMode == PageMode::Host)
                {
                    if (const std::uint64_t e = doc->SelectionEpoch(); e != lastEpoch)
                    {
                        lastEpoch = e;
                        if (!doc->SelectionKey().empty())
                            host.NotifySelected(*doc);
                    }
                    ImGui::SetNextWindowPos(ImVec2(1060.0f, 0.0f));
                    ImGui::SetNextWindowSize(ImVec2(220.0f, 720.0f));
                    (void)DrawInspectorWindows(host, windows, nullptr);
                }
                ImGui::Render();
            }
        }
        void Click(ImVec2 at)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(at.x, at.y); Frame();
            io.AddMouseButtonEvent(0, true); Frame();
            io.AddMouseButtonEvent(0, false); Frame(2);
        }
        template <typename F> auto InCanvas(F&& f)
        {
            ed::SetCurrentEditor(doc->GraphCanvasContext());
            auto r = f();
            ed::SetCurrentEditor(nullptr);
            return r;
        }
        // A point inside the node's title band, in screen space.
        ImVec2 NodeTitle(std::uint32_t id)
        {
            return InCanvas([&]
            {
                const ImVec2 p = ed::CanvasToScreen(ed::GetNodePosition(ed::NodeId(id)));
                return ImVec2(p.x + ed::GetNodeSize(ed::NodeId(id)).x * 0.5f, p.y + 8.0f);
            });
        }
    };
    MaterialAssetData TwoNodeGraph(const char* kind, MaterialGraph g)
    {
        MaterialAssetData data;
        data.id = Guid::Generate();
        data.name = "Canvas";
        data.kind = kind;
        data.graph = std::move(g);
        return data;
    }
    // Output 1 <- Float 2; Float 3 unwired.
    MaterialGraph OutputAndFloats()
    {
        MaterialGraph g;
        GraphNode out; out.id = 1; out.type = GraphNodeType::Output; out.posX = 420.0f; out.posY = 80.0f;
        GraphNode a;   a.id = 2;   a.type = GraphNodeType::ConstFloat; a.posX = 60.0f; a.posY = 80.0f;
        GraphNode b;   b.id = 3;   b.type = GraphNodeType::ConstFloat; b.posX = 60.0f; b.posY = 220.0f;
        g.nodes = { out, a, b };
        g.links = { { 2, 0, 1, 0 } };
        g.nextId = 4;
        return g;
    }
    std::string Key(std::size_t pass, std::uint32_t id) { return FormatNodeKey({ pass, id }); }
}

TEST_CASE("Node page s5.1.11 canvas: a restore selects in the canvas with no event; 2+ selected is the material page",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(2);
    REQUIRE(h.doc->RestoreSelection(Key(0, 2)));
    const std::uint64_t e0 = h.doc->SelectionEpoch();
    h.Frame(2);
    CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(2)); }));
    CHECK(h.doc->SelectionKey() == Key(0, 2));
    CHECK(h.doc->SelectionEpoch() == e0);                      // a restore is not an event

    h.InCanvas([] { ed::SelectNode(ed::NodeId(3), /*append*/ true); return 0; });   // programmatic 2-node selection
    h.Frame(2);
    CHECK(h.doc->SelectionKey() == "material");                // s5.1.9: no multi-select page
}

TEST_CASE("Node page s5.1.11 canvas: a click on a node is exactly ONE selection event beyond the content click",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(3);
    const std::uint64_t e0 = h.doc->SelectionEpoch();
    h.Click(ImVec2(1000.0f, 650.0f));                          // empty canvas: a content click, no selection change
    const std::uint64_t e1 = h.doc->SelectionEpoch();
    CHECK(h.doc->SelectionKey() == "material");
    const ImVec2 title = h.NodeTitle(2);
    INFO("node 2 title point " << title.x << ", " << title.y);
    h.Click(title);
    CHECK(h.doc->SelectionKey() == Key(0, 2));
    CHECK(h.doc->SelectionEpoch() - e1 == (e1 - e0) + 1);      // the same content click + one selection bump
}

TEST_CASE("Node page s5.1.11 canvas: pass switches and the chain view move no epoch; leaving the chain view restores the node",
          "[editor][graphcanvas][nodepage]")
{
    MaterialAssetData data = TwoNodeGraph("fullscreen", OutputAndFloats());
    MaterialPass blur; blur.name = "blur"; blur.inputs = { 0 }; blur.graph = OutputAndFloats();
    data.passes.push_back(std::move(blur));
    CanvasHarness h(std::move(data));
    h.Frame(2);                                                 // opens on the chain overview
    CHECK(h.doc->SelectionKey() == "material");
    const std::uint64_t e0 = h.doc->SelectionEpoch();
    REQUIRE(h.doc->RestoreSelection(Key(1, 2)));                // enters pass 1 (history: chain, pass 1)
    h.Frame(3);
    CHECK(h.doc->SelectionKey() == Key(1, 2));

    SECTION("pass switch")
    {
        REQUIRE(h.doc->RestoreSelection(Key(0, 2)));            // rebuilds the canvas context for the base
        h.Frame(3);
        CHECK(h.doc->SelectionKey() == Key(0, 2));
        CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(2)); }));
        CHECK(h.doc->SelectionEpoch() == e0);
    }
    SECTION("chain view")
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(500.0f, 400.0f); h.Frame();
        io.AddMouseButtonEvent(3, true); h.Frame(); io.AddMouseButtonEvent(3, false); h.Frame(2);   // back: the overview
        CHECK(h.doc->SelectionKey() == "material");             // pass-canvas nodes keep the material page
        io.AddMouseButtonEvent(4, true); h.Frame(); io.AddMouseButtonEvent(4, false); h.Frame(2);   // forward: pass 1
        CHECK(h.doc->SelectionKey() == Key(1, 2));              // the mirror survived the overview
        CHECK(h.doc->SelectionEpoch() == e0);
    }
}

// ---- R5 (spec s5.1.11 desk list, risk R5): does a click on a node's INLINE
// widget select the node? The question is recorded, not fixed, in T3. The probe:
// Sine's unwired `x` input carries the pin-literal DragFloat ("##lit", drawn
// right of the pin, ShaderEditorDocument.cpp:5555-5582). A drag on that point
// must change the literal -- that proves the point IS the widget -- and then the
// page key says whether imgui-node-editor also selected the node. ----
namespace
{
    // Output 1 <- Sin 2 (x unwired, so it shows the literal); Float 3 unwired.
    MaterialGraph OutputSineFloat()
    {
        MaterialGraph g;
        GraphNode out; out.id = 1; out.type = GraphNodeType::Output; out.posX = 420.0f; out.posY = 80.0f;
        GraphNode s;   s.id = 2;   s.type = GraphNodeType::Sin;      s.posX = 60.0f;  s.posY = 80.0f;
        GraphNode f;   f.id = 3;   f.type = GraphNodeType::ConstFloat; f.posX = 60.0f; f.posY = 260.0f;
        g.nodes = { out, s, f };
        g.links = { { 2, 0, 1, 0 } };
        g.nextId = 4;
        return g;
    }
    const GraphPinLiteral* SineLiteral(const ShaderEditorDocument& doc)
    {
        const MaterialGraph* g = doc.PassGraph(0);
        if (!g) return nullptr;
        for (const GraphNode& n : g->nodes)
            if (n.id == 2) return n.FindPinLiteral(0);
        return nullptr;
    }
}

// R5 VERDICT (recorded 2026-10-01, T3-GATE fix round 2): NO. A press on an
// inline widget makes that ImGui item active, and imgui-node-editor's
// BuildControl returns an empty Control while any non-editor item is active
// (ThirdParty/imgui-node-editor/imgui_node_editor.cpp:2576-2577), so no node is
// "clicked" and the canvas selection is untouched: the page stays on its
// previous key, whether that is the material or another node. This case PINS
// that behaviour as the record; a later fix (select the owning node on an
// inline-widget press) flips these CHECKs on purpose.
TEST_CASE("Node page R5 record: a click on a node's inline widget (the Sine x literal) does NOT select the node",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputSineFloat()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));                          // empty canvas: the material page
    REQUIRE(h.doc->SelectionKey() == "material");
    REQUIRE(SineLiteral(*h.doc) == nullptr);                   // absent until touched

    // The literal's centre: right of the x pin's bounds (pin, SameLine, a 64 px
    // drag), on the pin's row, in canvas space -> screen.
    const ImVec2 lit = h.InCanvas([]
    {
        auto* editor = reinterpret_cast<ed::Detail::EditorContext*>(ed::GetCurrentEditor());
        const ed::Detail::Pin* pin = editor->FindPin(ed::PinId(2 * 1000ull + 1 + 0));   // InPin(2, 0)
        REQUIRE(pin != nullptr);
        const ImRect b = pin->m_Bounds;
        const ImVec2 c(b.Max.x + ImGui::GetStyle().ItemSpacing.x + 32.0f, (b.Min.y + b.Max.y) * 0.5f);
        return ed::CanvasToScreen(c);
    });
    INFO("Sine x literal point " << lit.x << ", " << lit.y);
    const auto sineSelected = [&] { return h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(2)); }); };

    ImGuiIO& io = ImGui::GetIO();
    SECTION("press-drag-release on the literal: the value moves, the page stays on the material")
    {
        io.AddMousePosEvent(lit.x, lit.y); h.Frame();
        io.AddMouseButtonEvent(0, true); h.Frame();
        io.AddMousePosEvent(lit.x + 30.0f, lit.y); h.Frame(2);
        io.AddMouseButtonEvent(0, false); h.Frame(2);
        const GraphPinLiteral* l = SineLiteral(*h.doc);
        REQUIRE(l != nullptr);                                  // the point IS the widget: the drag wrote x
        CHECK(l->v[0] != 0.0f);
        CHECK_FALSE(sineSelected());
        CHECK(h.doc->SelectionKey() == "material");
    }
    SECTION("plain click on the literal: the page stays on the material")
    {
        h.Click(lit);
        CHECK_FALSE(sineSelected());
        CHECK(h.doc->SelectionKey() == "material");
    }
    SECTION("plain click on the literal with another node selected: the page stays on that node")
    {
        h.Click(h.NodeTitle(3));
        REQUIRE(h.doc->SelectionKey() == Key(0, 3));
        h.Click(lit);
        CHECK_FALSE(sineSelected());
        CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(3)); }));
        CHECK(h.doc->SelectionKey() == Key(0, 3));
    }
}

TEST_CASE("Node page s5.1.11 canvas: a click on node A then DIRECTLY on node B lands on B, page drawn or not",
          "[editor][graphcanvas][nodepage]")
{
    const int mode = GENERATE(0, 1, 2);
    INFO("page mode (0 none, 1 plain window, 2 real InspectorHost): " << mode);
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    if (mode == 1) h.pageMode = CanvasHarness::PageMode::Plain;
    if (mode == 2) h.UseHost();
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));                          // settles the open fit: NodeTitle reads the landed view
    h.Click(h.NodeTitle(2));
    REQUIRE(h.doc->SelectionKey() == Key(0, 2));
    const std::uint64_t e1 = h.doc->SelectionEpoch();
    h.Click(h.NodeTitle(3));                                    // no deselect in between
    CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(3)); }));
    CHECK(h.doc->SelectionKey() == Key(0, 3));
    CHECK(h.doc->SelectionEpoch() > e1);
    h.Click(h.NodeTitle(2));                                    // and straight back
    CHECK(h.doc->SelectionKey() == Key(0, 2));
}

TEST_CASE("Node page s5.1.11 canvas: a click on a visible node lands while another node is CULLED off-screen",
          "[editor][graphcanvas][nodepage]")
{
    // The desk reselect report. A culled node (NodeCulled) is submitted as a
    // stand-in whose pins are ImGui::Dummy(0,0), so their bounds are zero-size.
    // Upstream imgui-node-editor's invisibleButtonEx returned `false` (= 0, i.e.
    // "clicked with button 0") for a zero-size area, so every culled pin
    // reported a left click on every frame. BuildControl walks m_Nodes back to
    // front and OVERWRITES clickedObject on each hit, so a real node click lost
    // to whichever culled pin was walked after it: the Control carried a
    // ClickedPin and no ClickedNode, and SelectAction selected nothing.
    //
    // Order matters, so it is arranged: m_Nodes is creation order (the doc
    // draws g.nodes in order: 1, 2, 3) with the pressed node rotated to the
    // back, and the walk is in REVERSE. Culling node 1 (the FIRST created)
    // puts its pins LAST in the walk, after whichever visible node is clicked.
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));                          // settles the open fit (and measures every node)

    // Move node 1 far outside the cull rect (viewport + its 0.25 guard band).
    // It was drawn already, so it has a measured size and NodeCulled applies.
    h.InCanvas([] { ed::SetNodePosition(ed::NodeId(1), ImVec2(40000.0f, 40000.0f)); return 0; });
    h.Frame(3);
    const ImVec2 parked = h.InCanvas([] { return ed::CanvasToScreen(ed::GetNodePosition(ed::NodeId(1))); });
    INFO("node 1 screen pos " << parked.x << ", " << parked.y);
    REQUIRE((parked.x > 1280.0f * 1.25f || parked.y > 720.0f * 1.25f));   // beyond the guard band: culled

    const std::uint64_t e0 = h.doc->SelectionEpoch();
    h.Click(h.NodeTitle(2));
    CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(2)); }));
    CHECK(h.doc->SelectionKey() == Key(0, 2));
    CHECK(h.doc->SelectionEpoch() > e0);

    const std::uint64_t e1 = h.doc->SelectionEpoch();
    h.Click(h.NodeTitle(3));                                    // A then B, no deselect in between
    CHECK(h.InCanvas([] { return ed::IsNodeSelected(ed::NodeId(3)); }));
    CHECK(h.doc->SelectionKey() == Key(0, 3));
    CHECK(h.doc->SelectionEpoch() > e1);
}

TEST_CASE("Canvas pin paint: a Mul fed by a float4 Param resolves to 4 -- its pins paint PinColorForWidth(4) with the adapts ring; fixed and unresolved pins do not",
          "[editor][graphcanvas]")
{
    // Output 1 <- Mul 3 (a <- Param 2 'tint', float4); Add 4 unwired.
    MaterialGraph g;
    GraphNode out; out.id = 1; out.type = GraphNodeType::Output; out.posX = 520.0f; out.posY = 80.0f;
    GraphNode param; param.id = 2; param.type = GraphNodeType::Param; param.posX = 40.0f; param.posY = 80.0f;
    param.paramName = "tint";
    param.paramType = MatParamType::Float4;
    param.paramDefault = MatParamValue::MakeFloat4(1.0f, 1.0f, 1.0f, 1.0f);
    GraphNode mul; mul.id = 3; mul.type = GraphNodeType::Mul; mul.posX = 280.0f; mul.posY = 80.0f;
    GraphNode add; add.id = 4; add.type = GraphNodeType::Add; add.posX = 280.0f; add.posY = 260.0f;
    g.nodes = { out, param, mul, add };
    g.links = { { 2, 0, 3, 0 }, { 3, 0, 1, 0 } };
    g.nextId = 5;
    CanvasHarness h(TwoNodeGraph("sprite", g));
    h.Frame(3);

    const auto same = [](const ImVec4& a, const ImVec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; };
    const GraphPinPaint mulA = h.doc->CanvasPinPaint(3, 0, /*input*/ true);
    CHECK(same(mulA.color, PinColorForWidth(4)));
    CHECK(mulA.adapts);
    const GraphPinPaint mulB = h.doc->CanvasPinPaint(3, 1, true);    // unwired, but its node resolved
    CHECK(same(mulB.color, PinColorForWidth(4)));
    CHECK(mulB.adapts);
    const GraphPinPaint mulOut = h.doc->CanvasPinPaint(3, 0, false);
    CHECK(same(mulOut.color, PinColorForWidth(4)));
    CHECK(mulOut.adapts);
    const GraphPinPaint paramOut = h.doc->CanvasPinPaint(2, 0, false);   // Param's dynamic out: its type's lanes
    CHECK(same(paramOut.color, PinColorForWidth(4)));
    CHECK(paramOut.adapts);
    const GraphPinPaint outColor = h.doc->CanvasPinPaint(1, 0, true);    // a FIXED float4 pin: no ring
    CHECK(same(outColor.color, PinColorForWidth(4)));
    CHECK_FALSE(outColor.adapts);
    const GraphPinPaint addA = h.doc->CanvasPinPaint(4, 0, true);         // unresolved: plain grey
    CHECK(same(addA.color, kPinDynamicColor));
    CHECK_FALSE(addA.adapts);

    // The resolution is re-taken every frame: unwire the Param and the Mul goes grey.
    MaterialGraph unwired = *h.doc->PassGraph(0);
    unwired.links = { { 3, 0, 1, 0 } };
    h.doc->ApplyGraphState(0, unwired);
    h.Frame(2);
    const GraphPinPaint after = h.doc->CanvasPinPaint(3, 0, true);
    CHECK(same(after.color, kPinDynamicColor));
    CHECK_FALSE(after.adapts);
}

// ---- T3-D3 desk findings (2026-10-02): the marquee and its modifiers, undo
// keeping the view, Esc reverting an inline canvas drag. ----
namespace
{
    // The screen rect just covering nodes 2 and 3 of OutputAndFloats (the two
    // Floats in the left column), padded so the press lands on empty canvas.
    ImRect FloatsBox(CanvasHarness& h)
    {
        return h.InCanvas([]
        {
            ImRect r(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
            for (const std::uint32_t id : { 2u, 3u })
            {
                const ImVec2 p = ed::GetNodePosition(ed::NodeId(id));
                const ImVec2 s = ed::GetNodeSize(ed::NodeId(id));
                r.Add(ed::CanvasToScreen(p));
                r.Add(ed::CanvasToScreen(ImVec2(p.x + s.x, p.y + s.y)));
            }
            r.Expand(12.0f);
            return r;
        });
    }
    // Press on `from`, drag through the midpoint to `to`, release -- with
    // `mod` (ImGuiMod_Shift / ImGuiMod_Ctrl) held throughout when given.
    void Marquee(CanvasHarness& h, ImVec2 from, ImVec2 to, ImGuiKey mod = ImGuiKey_None)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (mod != ImGuiKey_None) io.AddKeyEvent(mod, true);
        io.AddMousePosEvent(from.x, from.y); h.Frame();
        io.AddMouseButtonEvent(0, true); h.Frame();
        // The press is the canvas's: a point off the canvas (the toolbar, the
        // window padding) grabs the WINDOW instead and no marquee can start.
        REQUIRE(ImGui::GetCurrentContext()->MovingWindow == nullptr);
        io.AddMousePosEvent((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f); h.Frame();
        io.AddMousePosEvent(to.x, to.y); h.Frame(2);
        io.AddMouseButtonEvent(0, false); h.Frame(2);
        if (mod != ImGuiKey_None) { io.AddKeyEvent(mod, false); h.Frame(); }
    }
    void ClickWith(CanvasHarness& h, ImVec2 at, ImGuiKey mod)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(mod, true);
        h.Click(at);
        io.AddKeyEvent(mod, false); h.Frame();
    }
    bool Selected(CanvasHarness& h, std::uint32_t id)
    {
        return h.InCanvas([id] { return ed::IsNodeSelected(ed::NodeId(id)); });
    }
}

TEST_CASE("Canvas marquee (T3-D3): a left-drag from empty canvas selects the nodes inside the box -- 2+ selected is the material page",
          "[editor][graphcanvas][nodepage]")
{
    const int mode = GENERATE(0, 1, 2);
    const bool cull = GENERATE(false, true);
    INFO("page mode (0 none, 1 plain window, 2 real InspectorHost): " << mode << "; node 1 culled off-screen: " << cull);
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    if (mode == 1) h.pageMode = CanvasHarness::PageMode::Plain;
    if (mode == 2) h.UseHost();
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));                          // settles the open fit: the box reads the landed view
    if (cull)
    {
        h.InCanvas([] { ed::SetNodePosition(ed::NodeId(1), ImVec2(40000.0f, 40000.0f)); return 0; });
        h.Frame(3);
    }
    const ImRect box = FloatsBox(h);
    INFO("box " << box.Min.x << ", " << box.Min.y << " -> " << box.Max.x << ", " << box.Max.y);
    Marquee(h, box.Min, box.Max);
    CHECK(Selected(h, 2));
    CHECK(Selected(h, 3));
    CHECK_FALSE(Selected(h, 1));                                // the Output lies outside the box
    CHECK(h.doc->SelectionKey() == "material");                 // s5.1.9: 2+ selected is the material page
}

// UE's Material Editor: Shift ADDS (Shift+drag = FMarqueeOperation::Add,
// Shift+click = "Shift always adds to selection", SNodePanel.cpp:194-212 /
// MarqueeOperation.h:50-68). Ctrl+drag keeps the selection too (the library's
// own rule; UE inverts, which differs only on boxed nodes already selected).
TEST_CASE("Canvas marquee (T3-D3): Shift or Ctrl held, the box ADDS to the selection; Shift+click adds a node",
          "[editor][graphcanvas][nodepage]")
{
    const ImGuiKey mod = GENERATE(ImGuiMod_Shift, ImGuiMod_Ctrl);
    INFO("modifier: " << (mod == ImGuiMod_Shift ? "Shift" : "Ctrl"));
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));
    h.Click(h.NodeTitle(1));
    REQUIRE(h.doc->SelectionKey() == Key(0, 1));
    const ImRect box = FloatsBox(h);
    Marquee(h, box.Min, box.Max, mod);
    CHECK(Selected(h, 1));                                      // kept
    CHECK(Selected(h, 2));
    CHECK(Selected(h, 3));
    CHECK(h.doc->SelectionKey() == "material");
}

TEST_CASE("Canvas click (T3-D3): Shift+click on a node adds it to the selection, as in UE",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputAndFloats()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));
    h.Click(h.NodeTitle(2));
    REQUIRE(h.doc->SelectionKey() == Key(0, 2));
    ClickWith(h, h.NodeTitle(3), ImGuiMod_Shift);
    CHECK(Selected(h, 2));
    CHECK(Selected(h, 3));
    CHECK(h.doc->SelectionKey() == "material");
    ClickWith(h, h.NodeTitle(3), ImGuiMod_Shift);               // adds, never toggles (Ctrl toggles)
    CHECK(Selected(h, 3));
}

namespace
{
    // The Sine x literal's centre in screen space (the R5 probe point): right
    // of the x pin's bounds -- pin, SameLine, a 64 px drag -- on the pin's row.
    ImVec2 SineLiteralPoint(CanvasHarness& h)
    {
        return h.InCanvas([]
        {
            auto* editor = reinterpret_cast<ed::Detail::EditorContext*>(ed::GetCurrentEditor());
            const ed::Detail::Pin* pin = editor->FindPin(ed::PinId(2 * 1000ull + 1 + 0));   // InPin(2, 0)
            REQUIRE(pin != nullptr);
            const ImRect b = pin->m_Bounds;
            return ed::CanvasToScreen(ImVec2(b.Max.x + ImGui::GetStyle().ItemSpacing.x + 32.0f, (b.Min.y + b.Max.y) * 0.5f));
        });
    }
    struct CanvasView
    {
        float zoom = 0.0f;
        ImVec2 origin;   // canvas (0,0) on screen
    };
    CanvasView ViewOf(CanvasHarness& h)
    {
        return h.InCanvas([] { return CanvasView{ ed::GetCurrentZoom(), ed::CanvasToScreen(ImVec2(0.0f, 0.0f)) }; });
    }
    bool SameView(const CanvasView& a, const CanvasView& b)
    {
        constexpr float kEps = 1e-3f;
        return std::abs(a.zoom - b.zoom) <= kEps && std::abs(a.origin.x - b.origin.x) <= kEps &&
               std::abs(a.origin.y - b.origin.y) <= kEps;
    }
}

TEST_CASE("Canvas view (T3-D3): undo and redo of a graph edit keep the user's zoom and scroll -- the fit-on-open fires once per open, not per reseed",
          "[editor][graphcanvas]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputSineFloat()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));                          // the open fit has landed
    const CanvasView fitted = ViewOf(h);

    // The user's own view: two wheel notches in, off-centre (zoom AND scroll move).
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(300.0f, 250.0f); h.Frame();
    io.AddMouseWheelEvent(0.0f, 1.0f); h.Frame();
    io.AddMouseWheelEvent(0.0f, 1.0f); h.Frame(20);
    const CanvasView user = ViewOf(h);
    INFO("fitted zoom " << fitted.zoom << " user zoom " << user.zoom);
    REQUIRE_FALSE(SameView(fitted, user));

    // A real edit: drag the Sine x literal (one undo step on release).
    const ImVec2 lit = SineLiteralPoint(h);
    io.AddMousePosEvent(lit.x, lit.y); h.Frame();
    io.AddMouseButtonEvent(0, true); h.Frame();
    io.AddMousePosEvent(lit.x + 30.0f, lit.y); h.Frame(2);
    io.AddMouseButtonEvent(0, false); h.Frame(2);
    REQUIRE(SineLiteral(*h.doc) != nullptr);
    REQUIRE(h.stack.CanUndo());
    const CanvasView edited = ViewOf(h);
    CHECK(SameView(user, edited));

    h.stack.Undo();
    h.Frame(10);
    CHECK(SineLiteral(*h.doc) == nullptr);                      // the edit is gone ...
    const CanvasView afterUndo = ViewOf(h);
    INFO("after undo zoom " << afterUndo.zoom << " origin " << afterUndo.origin.x << ", " << afterUndo.origin.y
         << "; user origin " << user.origin.x << ", " << user.origin.y);
    CHECK(SameView(user, afterUndo));                           // ... the view is not

    h.stack.Redo();
    h.Frame(10);
    CHECK(SineLiteral(*h.doc) != nullptr);
    CHECK(SameView(user, ViewOf(h)));
}

// The node page's numeric rows restore their seed on Esc mid-drag
// (PropertyGrid.cpp NumericRow); a canvas inline drag does the same: the value
// from the drag's start, the gesture ended, NO undo step, ClearActiveID.
TEST_CASE("Canvas inline drag (T3-D3): Esc while the button is held reverts the Sine x literal and leaves the undo stack alone",
          "[editor][graphcanvas][nodepage]")
{
    CanvasHarness h(TwoNodeGraph("sprite", OutputSineFloat()));
    h.Frame(3);
    h.Click(ImVec2(1000.0f, 650.0f));
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 lit = SineLiteralPoint(h);
    INFO("Sine x literal point " << lit.x << ", " << lit.y);
    auto dragThenEsc = [&](float dx)
    {
        io.AddMousePosEvent(lit.x, lit.y); h.Frame();
        io.AddMouseButtonEvent(0, true); h.Frame();
        io.AddMousePosEvent(lit.x + dx, lit.y); h.Frame(2);
        REQUIRE(ImGui::GetActiveID() != 0);                      // mid-drag
        io.AddKeyEvent(ImGuiKey_Escape, true); h.Frame();
        CHECK(ImGui::GetActiveID() == 0);                        // the gesture ended at the Esc
        io.AddKeyEvent(ImGuiKey_Escape, false); h.Frame();
        io.AddMouseButtonEvent(0, false); h.Frame(2);
    };

    SECTION("an untouched literal: it is absent again, and no step was pushed")
    {
        REQUIRE(SineLiteral(*h.doc) == nullptr);
        dragThenEsc(30.0f);
        CHECK(SineLiteral(*h.doc) == nullptr);
        CHECK_FALSE(h.stack.CanUndo());
    }
    SECTION("an existing literal: back to its value at the drag's start, and the stack still holds only the first edit")
    {
        io.AddMousePosEvent(lit.x, lit.y); h.Frame();
        io.AddMouseButtonEvent(0, true); h.Frame();
        io.AddMousePosEvent(lit.x + 20.0f, lit.y); h.Frame(2);
        io.AddMouseButtonEvent(0, false); h.Frame(2);
        const GraphPinLiteral* first = SineLiteral(*h.doc);
        REQUIRE(first != nullptr);
        const float start = first->v[0];
        REQUIRE(start != 0.0f);
        REQUIRE(h.stack.CanUndo());

        dragThenEsc(40.0f);
        const GraphPinLiteral* after = SineLiteral(*h.doc);
        REQUIRE(after != nullptr);
        CHECK(after->v[0] == start);
        h.stack.Undo();                                          // undoes the FIRST drag ...
        h.Frame(2);
        CHECK(SineLiteral(*h.doc) == nullptr);
        CHECK_FALSE(h.stack.CanUndo());                          // ... and there was nothing else
    }
}
