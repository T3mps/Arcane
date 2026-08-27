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

#include <imgui.h>

#include <filesystem>

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
