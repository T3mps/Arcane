// Slice 9 diagnostic + regression: drive the REAL ShaderEditorDocument::Draw()
// for a GRAPH-OWNED document through a device-less ImGui frame (null backend --
// no window, no GPU; software font atlas). This is the exact path the desk
// crash took (New Graph Material -> doc opens -> first canvas frame), which no
// other device-less test exercises: the imgui-node-editor canvas only runs inside
// a live ImGui frame.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialGraph.hpp>

#include "Documents/DocumentHost.hpp"
#include "Documents/ShaderEditorDocument.hpp"
#include "Documents/ShaderNodeKey.hpp"   // FormatNodeKey: the s5.1.11 canvas cases
#include "Helpers/NodePageDocs.hpp"   // SpriteNodeDoc / ChainNodeDoc / HeadlessImGui (node page s5.1.11)

#include <imgui.h>
#include <imgui_internal.h>   // OpenPopupStack: the modal-hoist case
#include <imgui_node_editor.h>   // the s5.1.11 canvas cases ask the canvas what is selected

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
        std::unique_ptr<ShaderEditorDocument> doc;

        explicit CanvasHarness(MaterialAssetData data)
            : doc(std::make_unique<ShaderEditorDocument>(DocServices{}, std::filesystem::path("canvas.arcmat"), std::move(data)))
        {
        }
        ~CanvasHarness() { doc.reset(); }   // inside the context (DestroyEditor)
        CanvasHarness(const CanvasHarness&) = delete;
        CanvasHarness& operator=(const CanvasHarness&) = delete;

        void Frame(int n = 1)
        {
            for (int i = 0; i < n; ++i)
                DocFrame(*doc);
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
