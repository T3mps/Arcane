// Node page fixture (spec 2026-09-30 s5.1.10): ReferenceProject's one
// graph-owned material. Its node ids are FIXED so the node-page tests and the
// E9 witness can name them; this file is the tripwire that keeps them there.
#include <catch2/catch_test_macros.hpp>

#include "Helpers/ReferenceProjectDir.hpp"

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialGraph.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <filesystem>
#include <optional>
#include <string>

using namespace Arcane;

namespace
{
    std::optional<MaterialAssetData> LoadFixture()
    {
        const std::filesystem::path root = Arcane::Test::FindReferenceProjectDir();
        REQUIRE_FALSE(root.empty());
        return LoadMaterialAsset(root / "Content" / "materials" / "node_page_graph.arcmat");
    }

    bool Linked(const MaterialGraph& g, std::uint32_t from, std::uint32_t fromPin,
                std::uint32_t to, std::uint32_t toPin)
    {
        for (const GraphLink& l : g.links)
            if (l.fromNode == from && l.fromPin == fromPin && l.toNode == to && l.toPin == toPin)
                return true;
        return false;
    }
}

TEST_CASE("Node page fixture: NodePageGraph is a graph-owned sprite base with fixed node ids",
          "[material][graph][nodepage]")
{
    const std::optional<MaterialAssetData> data = LoadFixture();
    REQUIRE(data.has_value());
    CHECK(data->id == Guid::FromString("7e5a0012-0012-4012-8012-000000000012").value());
    CHECK(data->name == "NodePageGraph");
    CHECK(data->kind == "sprite");
    CHECK_FALSE(data->IsInstance());
    CHECK(data->passes.empty());                       // no chain
    REQUIRE(data->IsGraphOwned());
    const MaterialGraph& g = *data->graph;

    struct Want { std::uint32_t id; GraphNodeType type; };
    for (const Want w : { Want{ 1, GraphNodeType::Output },   Want{ 2, GraphNodeType::SpriteTexture },
                          Want{ 3, GraphNodeType::Param },    Want{ 4, GraphNodeType::Mul },
                          Want{ 5, GraphNodeType::Power },    Want{ 6, GraphNodeType::Swizzle },
                          Want{ 7, GraphNodeType::Custom },   Want{ 8, GraphNodeType::Panner },
                          Want{ 9, GraphNodeType::Comment } })
    {
        INFO("node " << w.id);
        const GraphNode* n = g.FindNode(w.id);
        REQUIRE(n != nullptr);
        CHECK(n->type == w.type);
    }
    CHECK(g.nodes.size() == 9);
    CHECK(g.nextId == 10);

    CHECK(g.FindNode(3)->paramName == "tint");
    CHECK(g.FindNode(3)->paramType == MatParamType::Color);
    CHECK(g.links.size() == 3);
    CHECK(Linked(g, 2, 0, 4, 0));                      // Sprite Texture.rgba -> Multiply.a
    CHECK(Linked(g, 3, 0, 4, 1));                      // tint -> Multiply.b
    CHECK(Linked(g, 4, 0, 1, 0));                      // Multiply -> Output.color
    const GraphNode* power = g.FindNode(5);
    REQUIRE(power->FindPinLiteral(1) != nullptr);
    CHECK(power->FindPinLiteral(1)->v[0] == 2.0f);     // exponent literal 2
    CHECK(power->FindPinLiteral(0) == nullptr);        // x unwired, no literal
    CHECK(g.FindNode(6)->swizzleMask == "xy");
    const GraphNode* custom = g.FindNode(7);
    REQUIRE(custom->customPins.size() == 1);
    CHECK(custom->customPins[0].name == "p1");
    CHECK(custom->customPins[0].width == 4);
    CHECK(custom->customBody == "return p1;");
    CHECK(custom->customOutWidth == 4);
    CHECK(g.FindNode(8)->pannerFractional);
    CHECK(g.FindNode(9)->paramName == "Node page fixture");

    // The file carries no "snippet": the loader's graph self-heal generated it,
    // and it is exactly what codegen says for the sprite surface.
    const GraphCodegenResult gen = GenerateGraphSnippet(g, MaterialSurface::Sprite);
    REQUIRE(gen.Ok());
    CHECK(data->snippet == gen.snippet);
}

TEST_CASE("Node page fixture: NodePageGraph's snippet compiles on both targets", "[shadercompile][nodepage]")
{
    const std::optional<MaterialAssetData> data = LoadFixture();
    REQUIRE(data.has_value());
    ShaderSourceProvider provider;
    provider.AddRoot("data/shaders");
    ShaderCompiler sc;
    REQUIRE(sc.InitializeWithDebounce(0.0));
    const auto templateText = provider.Get("materials/sprite_material.hlsl");
    REQUIRE(templateText.has_value());
    const MaterialBuildResult build = BuildMaterialShaderSource(
        *templateText, data->snippet, "node_page_graph", MaterialSurface::Sprite);
    REQUIRE(build.errors.empty());
    for (const char* entry : { kPsEntry, kVsEntry })
    {
        ShaderCompileRequest req;
        req.debugName = "node_page_graph.hlsl";
        req.sourceUtf8 = build.hlsl;
        req.entry = entry;
        req.profile = entry == kPsEntry ? kPsProfile : kVsProfile;
        const ShaderCompileResult r = sc.CompileNow(req);
        for (const ShaderDiag& d : r.dxil.diags)  INFO("dxil " << entry << ": " << d.line << ": " << d.message);
        for (const ShaderDiag& d : r.spirv.diags) INFO("spirv " << entry << ": " << d.line << ": " << d.message);
        CHECK(r.AllSucceeded());
    }
}
