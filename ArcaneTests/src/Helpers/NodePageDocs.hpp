#pragma once

// Node page test fixtures (spec 2026-09-30 s5.1.11): two in-memory
// graph-owned bases for ShaderEditorDocument and a device-less ImGui context
// (the GraphCanvasHeadlessTest.cpp:60-69 shape). Header-only; shared by
// ShaderEditorDocumentTest.cpp and GraphCanvasHeadlessTest.cpp.

#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialGraph.hpp>

#include <imgui.h>

#include <cstdint>
#include <utility>

namespace Arcane::Test
{
    inline GraphNode PageNode(std::uint32_t id, GraphNodeType type, float x, float y)
    {
        GraphNode n;
        n.id = id;
        n.type = type;
        n.posX = x;
        n.posY = y;
        return n;
    }

    // A graph-owned SPRITE base: no chain view, so the canvas draws on frame 1.
    // Output 1 <- Multiply 3 (pin a) <- Color 2; Custom 4 (pin p1, float4) unwired.
    inline MaterialAssetData SpriteNodeDoc()
    {
        MaterialGraph g;
        g.nodes = { PageNode(1, GraphNodeType::Output, 600.0f, 200.0f),
                    PageNode(2, GraphNodeType::ConstColor, 100.0f, 120.0f),
                    PageNode(3, GraphNodeType::Mul, 340.0f, 200.0f),
                    PageNode(4, GraphNodeType::Custom, 340.0f, 380.0f) };
        for (float& v : g.nodes[1].value) v = 1.0f;
        g.nodes[3].customPins = { { "p1", 4 } };
        g.nodes[3].customBody = "return p1;";
        g.nodes[3].customOutWidth = 4;
        g.links = { GraphLink{ 2, 0, 3, 0 }, GraphLink{ 3, 0, 1, 0 } };
        g.nextId = 5;
        MaterialAssetData data;
        data.id = Guid::FromString("dddd5555-5555-4555-8555-555555555555").value();
        data.name = "Nodes";
        data.kind = "sprite";
        data.graph = std::move(g);
        return data;
    }

    // A graph-owned FULLSCREEN base with ONE graph pass "blur": ChainMode, so
    // the document opens on the chain overview. Base: Output 1 <- Float4 2.
    // Pass 1: Output 1 <- Custom 2 (pin p1).
    inline MaterialAssetData ChainNodeDoc()
    {
        MaterialGraph base;
        base.nodes = { PageNode(1, GraphNodeType::Output, 600.0f, 200.0f),
                       PageNode(2, GraphNodeType::ConstFloat4, 200.0f, 200.0f) };
        for (float& v : base.nodes[1].value) v = 1.0f;
        base.links = { GraphLink{ 2, 0, 1, 0 } };
        base.nextId = 3;
        MaterialGraph passGraph;
        passGraph.nodes = { PageNode(1, GraphNodeType::Output, 600.0f, 200.0f),
                            PageNode(2, GraphNodeType::Custom, 200.0f, 200.0f) };
        passGraph.nodes[1].customPins = { { "p1", 4 } };
        passGraph.nodes[1].customBody = "return p1;";
        passGraph.nodes[1].customOutWidth = 4;
        passGraph.links = { GraphLink{ 2, 0, 1, 0 } };
        passGraph.nextId = 3;
        MaterialPass pass;
        pass.name = "blur";
        pass.inputs = { 0 };
        pass.graph = std::move(passGraph);
        MaterialAssetData data;
        data.id = Guid::FromString("dddd6666-6666-4666-8666-666666666666").value();
        data.name = "Chain";
        data.kind = "fullscreen";
        data.graph = std::move(base);
        data.passes = { std::move(pass) };
        return data;
    }

    // Device-less ImGui: no backend, a software font atlas. Declare it BEFORE
    // the document so the document's dtor (DestroyEditor) runs inside it.
    struct HeadlessImGui
    {
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        HeadlessImGui()
        {
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 720.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        }
        ~HeadlessImGui() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        HeadlessImGui(const HeadlessImGui&) = delete;
        HeadlessImGui& operator=(const HeadlessImGui&) = delete;
    };
}
