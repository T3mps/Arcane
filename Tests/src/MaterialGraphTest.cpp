// Material node graph (Slice 9): node table, topological codegen (SSA locals,
// SG adaptation table, line map), structured errors, JSON round-trip, .armat
// integration, and the graph -> snippet -> dual-target compile proof.
// CPU + dxc only, no GPU.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialGraph.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Arcane;

namespace
{
    GraphNode Node(std::uint32_t id, GraphNodeType type)
    {
        GraphNode n;
        n.id = id;
        n.type = type;
        return n;
    }

    GraphNode ParamNode(std::uint32_t id, std::string name, MatParamType type,
                        MatParamValue def, bool ranged = false, float mn = 0, float mx = 1)
    {
        GraphNode n = Node(id, GraphNodeType::Param);
        n.paramName = std::move(name);
        n.paramType = type;
        n.paramDefault = def;
        n.hasRange = ranged;
        n.rangeMin = mn;
        n.rangeMax = mx;
        return n;
    }

    GraphNode TexNode(std::uint32_t id, std::string name)
    {
        GraphNode n = Node(id, GraphNodeType::TextureSample);
        n.paramName = std::move(name);
        n.paramType = MatParamType::Texture;
        return n;
    }

    GraphLink Link(std::uint32_t from, std::uint32_t fromPin,
                   std::uint32_t to, std::uint32_t toPin)
    {
        GraphLink l;
        l.fromNode = from;
        l.fromPin = fromPin;
        l.toNode = to;
        l.toPin = toPin;
        return l;
    }

    bool HasErrorOn(const GraphCodegenResult& r, std::uint32_t nodeId)
    {
        return std::any_of(r.errors.begin(), r.errors.end(),
                           [&](const GraphError& e) { return e.nodeId == nodeId; });
    }
}

TEST_CASE("Graph node table covers every type with round-tripping tokens", "[material]")
{
    const auto infos = AllGraphNodeInfos();
    REQUIRE(infos.size() == static_cast<std::size_t>(GraphNodeType::Custom) + 1);
    for (const GraphNodeTypeInfo& info : infos)
    {
        CHECK(GraphNodeInfo(info.type).token == info.token);
        GraphNodeType parsed{};
        REQUIRE(GraphNodeTypeFromToken(info.token, parsed));
        CHECK(parsed == info.type);
    }
    GraphNodeType t{};
    CHECK_FALSE(GraphNodeTypeFromToken("not_a_node", t));

    // Pin-order contract spot checks (append-only; these indices are serialized).
    CHECK(GraphNodeInfo(GraphNodeType::Lerp).inputs.size() == 3);
    CHECK(GraphNodeInfo(GraphNodeType::TextureSample).outputs.size() == 2);
    CHECK(std::string(GraphNodeInfo(GraphNodeType::TextureSample).outputs[1].name) == "a");
    CHECK(GraphNodeInfo(GraphNodeType::Split).outputs.size() == 4);
    CHECK(GraphNodeInfo(GraphNodeType::Output).outputs.empty());
}

TEST_CASE("Golden codegen: const color to output", "[material]")
{
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    GraphNode c = Node(2, GraphNodeType::ConstColor);
    c.value[0] = 0.25f; c.value[1] = 0.5f; c.value[2] = 0.75f; c.value[3] = 1.0f;
    g.nodes.push_back(c);
    g.links.push_back(Link(2, 0, 1, 0));

    const GraphCodegenResult r = GenerateGraphSnippet(g);
    REQUIRE(r.Ok());
    CHECK(r.snippet ==
          "float4 shade(Varyings v)\n"
          "{\n"
          "    float4 _n2 = float4(0.25, 0.5, 0.75, 1);\n"
          "    return _n2;\n"
          "}\n");
    // Line map: statement lines carry their node, engine scaffolding carries 0.
    REQUIRE(r.lineNodeIds.size() == 5);
    CHECK(r.lineNodeIds[0] == 0);   // signature
    CHECK(r.lineNodeIds[1] == 0);   // {
    CHECK(r.lineNodeIds[2] == 2);   // const
    CHECK(r.lineNodeIds[3] == 1);   // return (the Output node)
    CHECK(r.lineNodeIds[4] == 0);   // }
}

TEST_CASE("Codegen: adaptation, dynamic widths, multi-output, params", "[material]")
{
    // Speed(param) * Time -> scalar; UV + scalar -> float2 (splat); sample at
    // that uv; lerp base color toward the sample by its alpha (multi-output).
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    g.nodes.push_back(ParamNode(2, "Speed", MatParamType::Float,
                                MatParamValue::MakeFloat(2.0f), true, 0.0f, 4.0f));
    g.nodes.push_back(Node(3, GraphNodeType::Time));
    g.nodes.push_back(Node(4, GraphNodeType::Mul));
    g.nodes.push_back(Node(5, GraphNodeType::UV));
    g.nodes.push_back(Node(6, GraphNodeType::Add));
    g.nodes.push_back(TexNode(7, "Noise"));
    g.nodes.push_back(Node(8, GraphNodeType::Lerp));
    GraphNode base = Node(9, GraphNodeType::ConstColor);
    base.value[3] = 1.0f;
    g.nodes.push_back(base);

    g.links.push_back(Link(3, 0, 4, 0));   // Time -> Mul.a
    g.links.push_back(Link(2, 0, 4, 1));   // Speed -> Mul.b
    g.links.push_back(Link(5, 0, 6, 0));   // UV -> Add.a
    g.links.push_back(Link(4, 0, 6, 1));   // Mul -> Add.b (scalar into float2)
    g.links.push_back(Link(6, 0, 7, 0));   // Add -> TextureSample.uv
    g.links.push_back(Link(9, 0, 8, 0));   // base -> Lerp.a
    g.links.push_back(Link(7, 0, 8, 1));   // sample.rgba -> Lerp.b
    g.links.push_back(Link(7, 1, 8, 2));   // sample.a -> Lerp.t (scalar splat)
    g.links.push_back(Link(8, 0, 1, 0));   // Lerp -> Output

    const GraphCodegenResult r = GenerateGraphSnippet(g);
    REQUIRE(r.Ok());

    // Param block: node-id order, grammar matches ParseMaterialSource.
    CHECK(r.snippet.find("//@param float Speed = 2 [0..4]\n") != std::string::npos);
    CHECK(r.snippet.find("//@param texture Noise\n") != std::string::npos);
    CHECK(r.snippet.find("//@param float Speed") < r.snippet.find("//@param texture Noise"));

    // Dynamic widths + adaptation.
    CHECK(r.snippet.find("float _n4 = _n3 * _n2;") != std::string::npos);
    CHECK(r.snippet.find("float2 _n6 = _n5 + (_n4).xx;") != std::string::npos);
    CHECK(r.snippet.find("float4 _n7_rgba = Noise.Sample(MaterialSampler, _n6);") != std::string::npos);
    CHECK(r.snippet.find("float _n7_a = _n7_rgba.a;") != std::string::npos);
    CHECK(r.snippet.find("float4 _n8 = lerp(_n9, _n7_rgba, (_n7_a).xxxx);") != std::string::npos);
    CHECK(r.snippet.find("return _n8;") != std::string::npos);

    // The generated snippet parses cleanly through the REAL grammar.
    const MaterialSourceParse parsed = ParseMaterialSource(r.snippet);
    CHECK(parsed.errors.empty());
    REQUIRE(parsed.decls.size() == 2);
    CHECK(parsed.decls[0].name == "Speed");
    CHECK(parsed.metas[0].sliderMax == 4.0f);
    CHECK(parsed.decls[1].type == MatParamType::Texture);

    // Line map points the sample statement at node 7.
    std::size_t line = 0, pos = 0;
    const std::string needle = "float4 _n7_rgba";
    const std::size_t at = r.snippet.find(needle);
    for (std::size_t i = 0; i < at; ++i)
        if (r.snippet[i] == '\n')
            ++line;
    (void)pos;
    REQUIRE(line < r.lineNodeIds.size());
    CHECK(r.lineNodeIds[line] == 7);
}

TEST_CASE("Codegen: unconnected defaults and unreachable params", "[material]")
{
    SECTION("bare Output returns opaque black")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE(r.Ok());
        CHECK(r.snippet.find("return float4(0.0, 0.0, 0.0, 1.0);") != std::string::npos);
    }
    SECTION("TextureSample defaults its uv to v.uv")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(TexNode(2, "Tex"));
        g.links.push_back(Link(2, 0, 1, 0));
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE(r.Ok());
        CHECK(r.snippet.find("Tex.Sample(MaterialSampler, v.uv)") != std::string::npos);
    }
    SECTION("unconnected operand reads zero at the resolved width")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(2, GraphNodeType::UV));
        g.nodes.push_back(Node(3, GraphNodeType::Add));
        g.links.push_back(Link(2, 0, 3, 0));
        g.links.push_back(Link(3, 0, 1, 0));
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE(r.Ok());
        CHECK(r.snippet.find("float2 _n3 = _n2 + (0.0).xx;") != std::string::npos);
    }
    SECTION("unreachable Param still declares (params panel == canvas)")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        GraphNode c = Node(2, GraphNodeType::ConstColor);
        g.nodes.push_back(c);
        g.links.push_back(Link(2, 0, 1, 0));
        g.nodes.push_back(ParamNode(9, "Extra", MatParamType::Float,
                                    MatParamValue::MakeFloat(0.0f)));
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE(r.Ok());
        CHECK(r.snippet.find("//@param float Extra = 0\n") != std::string::npos);
        CHECK(r.snippet.find("_n9") == std::string::npos);   // no statement emitted
    }
}

TEST_CASE("Codegen: Split lanes follow the SG rule", "[material]")
{
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    GraphNode c2 = Node(2, GraphNodeType::ConstFloat2);
    c2.value[0] = 0.5f; c2.value[1] = 0.75f;
    g.nodes.push_back(c2);
    g.nodes.push_back(Node(3, GraphNodeType::Split));
    g.nodes.push_back(Node(4, GraphNodeType::Add));
    g.links.push_back(Link(2, 0, 3, 0));   // float2 -> Split.x
    g.links.push_back(Link(3, 0, 4, 0));   // r
    g.links.push_back(Link(3, 2, 4, 1));   // b (beyond width -> 0)
    g.links.push_back(Link(4, 0, 1, 0));

    const GraphCodegenResult r = GenerateGraphSnippet(g);
    REQUIRE(r.Ok());
    CHECK(r.snippet.find("float _n3_r = (_n2).x;") != std::string::npos);
    CHECK(r.snippet.find("float _n3_b = 0.0;") != std::string::npos);
    CHECK(r.snippet.find("_n3_g") == std::string::npos);   // unconsumed pin: no local
}

TEST_CASE("Codegen: Custom (HLSL) node emits a function + call", "[material]")
{
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    g.nodes.push_back(Node(2, GraphNodeType::UV));
    g.nodes.push_back(Node(3, GraphNodeType::Time));
    GraphNode custom = Node(4, GraphNodeType::Custom);
    custom.customPins = { { "uv", 2 }, { "t", 1 } };
    custom.customOutWidth = 4;
    custom.customBody = "float2 p = uv * 2.0;\nreturn float4(p, sin(t), 1.0);";
    g.nodes.push_back(custom);
    g.links.push_back(Link(2, 0, 4, 0));
    g.links.push_back(Link(3, 0, 4, 1));
    g.links.push_back(Link(4, 0, 1, 0));
    g.nextId = 5;   // load self-heals nextId past max id; match for the byte compare

    const GraphCodegenResult r = GenerateGraphSnippet(g);
    REQUIRE(r.Ok());
    CHECK(r.snippet.find("float4 _cf4(float2 uv, float t)") != std::string::npos);
    CHECK(r.snippet.find("    float2 p = uv * 2.0;") != std::string::npos);
    CHECK(r.snippet.find("float4 _n4 = _cf4(_n2, _n3);") != std::string::npos);
    // The function precedes shade().
    CHECK(r.snippet.find("float4 _cf4") < r.snippet.find("float4 shade(Varyings v)"));
    // Line map: body lines badge the Custom node (compile errors inside the
    // designer's HLSL land on the right node).
    std::size_t line = 0;
    const std::size_t at = r.snippet.find("float2 p = uv");
    for (std::size_t i = 0; i < at; ++i)
        if (r.snippet[i] == '\n')
            ++line;
    REQUIRE(line < r.lineNodeIds.size());
    CHECK(r.lineNodeIds[line] == 4);

    SECTION("validation: empty body, bad/duplicate pin names")
    {
        GraphNode* c = g.FindNode(4);
        c->customBody.clear();
        CHECK(HasErrorOn(GenerateGraphSnippet(g), 4));
        c->customBody = "return 0.0;";
        c->customPins[0].name = "9bad";
        CHECK(HasErrorOn(GenerateGraphSnippet(g), 4));
        c->customPins[0].name = "t";   // duplicates pin 1
        CHECK(HasErrorOn(GenerateGraphSnippet(g), 4));
    }
    SECTION("JSON round-trips pins, body, and out width")
    {
        const auto back = GraphFromJson(GraphToJson(g));
        REQUIRE(back.has_value());
        const GraphNode* c = back->FindNode(4);
        REQUIRE(c != nullptr);
        CHECK(c->type == GraphNodeType::Custom);
        REQUIRE(c->customPins.size() == 2);
        CHECK(c->customPins[0].name == "uv");
        CHECK(c->customPins[0].width == 2);
        CHECK(c->customPins[1].name == "t");
        CHECK(c->customOutWidth == 4);
        CHECK(c->customBody == custom.customBody);
        REQUIRE(back->links.size() == 3);
        CHECK(GraphToJson(*back).dump() == GraphToJson(g).dump());
    }
}

TEST_CASE("Codegen: structured errors", "[material]")
{
    SECTION("no Output")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(2, GraphNodeType::Time));
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE_FALSE(r.Ok());
        CHECK(r.snippet.empty());
        CHECK(HasErrorOn(r, 0));
    }
    SECTION("two Outputs")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(2, GraphNodeType::Output));
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE_FALSE(r.Ok());
        CHECK(HasErrorOn(r, 2));
    }
    SECTION("cycle")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(2, GraphNodeType::Add));
        g.nodes.push_back(Node(3, GraphNodeType::Add));
        g.links.push_back(Link(2, 0, 3, 0));
        g.links.push_back(Link(3, 0, 2, 0));
        g.links.push_back(Link(2, 0, 1, 0));
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE_FALSE(r.Ok());
        CHECK(r.snippet.empty());
        CHECK((HasErrorOn(r, 2) || HasErrorOn(r, 3)));
    }
    SECTION("param type conflict errors BOTH nodes, even unreachable ones")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(ParamNode(2, "X", MatParamType::Float,
                                    MatParamValue::MakeFloat(1.0f)));
        g.nodes.push_back(ParamNode(3, "X", MatParamType::Float4,
                                    MatParamValue::MakeFloat4(0, 0, 0, 0)));
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE_FALSE(r.Ok());
        CHECK(HasErrorOn(r, 2));
        CHECK(HasErrorOn(r, 3));
    }
    SECTION("reserved and invalid param names")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(ParamNode(2, "Time", MatParamType::Float,
                                    MatParamValue::MakeFloat(0.0f)));
        g.nodes.push_back(ParamNode(3, "_n5", MatParamType::Float,
                                    MatParamValue::MakeFloat(0.0f)));
        g.nodes.push_back(ParamNode(4, "9bad", MatParamType::Float,
                                    MatParamValue::MakeFloat(0.0f)));
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE_FALSE(r.Ok());
        CHECK(HasErrorOn(r, 2));
        CHECK(HasErrorOn(r, 3));
        CHECK(HasErrorOn(r, 4));
    }
    SECTION("Param node cannot declare a texture")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(ParamNode(2, "T", MatParamType::Texture, MatParamValue{}));
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE_FALSE(r.Ok());
        CHECK(HasErrorOn(r, 2));
    }
    SECTION("sprite-only nodes are gated by surface")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(2, GraphNodeType::VertexColor));
        g.links.push_back(Link(2, 0, 1, 0));
        CHECK_FALSE(GenerateGraphSnippet(g, MaterialSurface::Fullscreen).Ok());
        CHECK(GenerateGraphSnippet(g, MaterialSurface::Sprite).Ok());
    }
    SECTION("link to a missing node")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.links.push_back(Link(99, 0, 1, 0));
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE_FALSE(r.Ok());
    }
}

TEST_CASE("Graph JSON round-trip is stable, sorted, and self-healing", "[material]")
{
    MaterialGraph g;
    // Deliberately out of id order: serialization must sort.
    g.nodes.push_back(TexNode(7, "Noise"));
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    GraphNode c = Node(4, GraphNodeType::ConstFloat2);
    c.value[0] = 1.5f; c.value[1] = -2.0f;
    c.posX = 100.0f; c.posY = 50.0f;
    g.nodes.push_back(c);
    g.nodes.push_back(ParamNode(2, "Speed", MatParamType::Float,
                                MatParamValue::MakeFloat(2.0f), true, 0.0f, 4.0f));
    g.links.push_back(Link(7, 0, 1, 0));
    g.links.push_back(Link(4, 0, 7, 0));
    g.nextId = 11;

    const nlohmann::json j = GraphToJson(g);
    CHECK(j["nodes"][0]["id"] == 1);
    CHECK(j["nodes"][1]["id"] == 2);
    CHECK(j["nodes"][3]["id"] == 7);
    CHECK(j["nextId"] == 11);

    const auto back = GraphFromJson(j);
    REQUIRE(back.has_value());
    REQUIRE(back->nodes.size() == 4);
    CHECK(back->nextId == 11);
    const GraphNode* speed = back->FindNode(2);
    REQUIRE(speed != nullptr);
    CHECK(speed->type == GraphNodeType::Param);
    CHECK(speed->paramName == "Speed");
    CHECK(speed->paramType == MatParamType::Float);
    CHECK(speed->paramDefault.f[0] == 2.0f);
    CHECK(speed->hasRange);
    CHECK(speed->rangeMax == 4.0f);
    const GraphNode* c2 = back->FindNode(4);
    REQUIRE(c2 != nullptr);
    CHECK(c2->value[0] == 1.5f);
    CHECK(c2->value[1] == -2.0f);
    CHECK(c2->posX == 100.0f);
    REQUIRE(back->links.size() == 2);

    // Byte-stable second pass (sorted output => same text).
    CHECK(GraphToJson(*back).dump() == j.dump());

    SECTION("unknown node type refuses the whole graph")
    {
        nlohmann::json bad = j;
        bad["nodes"][0]["type"] = "warp_drive";
        CHECK_FALSE(GraphFromJson(bad).has_value());
    }
    SECTION("dangling links drop silently on load")
    {
        nlohmann::json dangling = j;
        dangling["links"].push_back({ { "from", 99 }, { "fromPin", 0 },
                                      { "to", 1 }, { "toPin", 0 } });
        const auto healed = GraphFromJson(dangling);
        REQUIRE(healed.has_value());
        CHECK(healed->links.size() == 2);
    }
    SECTION("stale nextId self-heals past the max id")
    {
        nlohmann::json stale = j;
        stale["nextId"] = 3;
        const auto healed = GraphFromJson(stale);
        REQUIRE(healed.has_value());
        CHECK(healed->nextId >= 8);
    }
}

TEST_CASE("MintId is monotonic and never reuses", "[material]")
{
    MaterialGraph g;
    g.nodes.push_back(Node(5, GraphNodeType::Output));
    g.nextId = 2;                       // stale (hand-edited file)
    const std::uint32_t a = g.MintId();
    CHECK(a == 6);                      // healed past the max id
    g.nodes.push_back(Node(a, GraphNodeType::Time));
    g.nodes.pop_back();                 // delete the node...
    CHECK(g.MintId() == 7);             // ...its id stays dead
}

TEST_CASE(".armat carries the graph; graph-only files self-heal a snippet", "[material]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_material_graph_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    GraphNode c = Node(2, GraphNodeType::ConstColor);
    c.value[0] = 1.0f; c.value[3] = 1.0f;
    g.nodes.push_back(c);
    g.links.push_back(Link(2, 0, 1, 0));
    g.nextId = 3;

    const GraphCodegenResult gen = GenerateGraphSnippet(g);
    REQUIRE(gen.Ok());

    MaterialAssetData data;
    data.id = Guid::FromString("aaaa1111-1111-4111-8111-111111111111").value();
    data.name = "graphmat";
    data.kind = "fullscreen";
    data.snippet = gen.snippet;
    data.graph = g;

    const fs::path path = dir / "graphmat.armat";
    REQUIRE(SaveMaterialAsset(path, data));

    const auto loaded = LoadMaterialAsset(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->IsGraphOwned());
    REQUIRE(loaded->graph.has_value());
    CHECK(loaded->graph->nodes.size() == 2);
    CHECK(loaded->graph->nextId == 3);
    CHECK(loaded->snippet == gen.snippet);

    SECTION("hand-authored graph-only file regenerates its snippet on load")
    {
        const fs::path bare = dir / "bare.armat";
        nlohmann::json doc;
        doc["id"] = "bbbb2222-2222-4222-8222-222222222222";
        doc["name"] = "bare";
        doc["kind"] = "fullscreen";
        doc["graph"] = GraphToJson(g);
        std::ofstream(bare, std::ios::binary) << doc.dump(2);

        const auto healed = LoadMaterialAsset(bare);
        REQUIRE(healed.has_value());
        CHECK(healed->IsGraphOwned());
        CHECK(healed->snippet.find("float4 shade(Varyings v)") != std::string::npos);
    }
    SECTION("a graph on an instance is ignored")
    {
        const fs::path inst = dir / "inst.armat";
        nlohmann::json doc;
        doc["id"] = "cccc3333-3333-4333-8333-333333333333";
        doc["parent"] = "aaaa1111-1111-4111-8111-111111111111";
        doc["graph"] = GraphToJson(g);
        std::ofstream(inst, std::ios::binary) << doc.dump(2);

        const auto loaded2 = LoadMaterialAsset(inst);
        REQUIRE(loaded2.has_value());
        CHECK(loaded2->IsInstance());
        CHECK_FALSE(loaded2->IsGraphOwned());
    }
}

TEST_CASE("Graph-generated snippets compile on both targets and surfaces", "[shadercompile]")
{
    ShaderSourceProvider provider;
    provider.AddRoot("shaders");
    ShaderCompiler sc;
    REQUIRE(sc.Initialize(0.0));

    auto compileBoth = [&](const std::string& hlsl, const char* name)
    {
        for (const char* entry : { kPsEntry, kVsEntry })
        {
            ShaderCompileRequest req;
            req.debugName = name;
            req.sourceUtf8 = hlsl;
            req.entry = entry;
            req.profile = entry == kPsEntry ? kPsProfile : kVsProfile;
            const ShaderCompileResult r = sc.CompileNow(req);
            for (const ShaderDiag& d : r.dxil.diags)
                INFO("dxil " << entry << ": " << d.line << ": " << d.message);
            for (const ShaderDiag& d : r.spirv.diags)
                INFO("spirv " << entry << ": " << d.line << ": " << d.message);
            CHECK(r.AllSucceeded());
        }
    };

    SECTION("fullscreen: params, adaptation, multi-output, Split")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(ParamNode(2, "Speed", MatParamType::Float,
                                    MatParamValue::MakeFloat(2.0f), true, 0.0f, 4.0f));
        g.nodes.push_back(Node(3, GraphNodeType::Time));
        g.nodes.push_back(Node(4, GraphNodeType::Mul));
        g.nodes.push_back(Node(5, GraphNodeType::UV));
        g.nodes.push_back(Node(6, GraphNodeType::Add));
        g.nodes.push_back(TexNode(7, "Noise"));
        g.nodes.push_back(Node(8, GraphNodeType::Lerp));
        GraphNode base = Node(9, GraphNodeType::ConstColor);
        base.value[3] = 1.0f;
        g.nodes.push_back(base);
        g.nodes.push_back(Node(10, GraphNodeType::Split));
        g.nodes.push_back(Node(11, GraphNodeType::Saturate));
        g.links.push_back(Link(3, 0, 4, 0));
        g.links.push_back(Link(2, 0, 4, 1));
        g.links.push_back(Link(5, 0, 6, 0));
        g.links.push_back(Link(4, 0, 6, 1));
        g.links.push_back(Link(6, 0, 7, 0));
        g.links.push_back(Link(9, 0, 8, 0));
        g.links.push_back(Link(7, 0, 8, 1));
        g.links.push_back(Link(7, 1, 8, 2));
        g.links.push_back(Link(8, 0, 10, 0));
        g.links.push_back(Link(10, 1, 11, 0));   // g lane
        g.links.push_back(Link(8, 0, 1, 0));

        const GraphCodegenResult gen = GenerateGraphSnippet(g, MaterialSurface::Fullscreen);
        REQUIRE(gen.Ok());
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());
        const MaterialBuildResult build = BuildMaterialShaderSource(
            *templateText, gen.snippet, "graph_fullscreen", MaterialSurface::Fullscreen);
        REQUIRE(build.errors.empty());
        compileBoth(build.hlsl, "graph_fullscreen.hlsl");
    }

    SECTION("fullscreen: Custom node body reads a param + Time directly")
    {
        // The perk of our template layout: the emitted _cf function sits AFTER
        // the cbuffer/Globals declarations, so custom HLSL can use param names
        // and Time without piping them through pins (SG's CFN cannot).
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(ParamNode(2, "Tint", MatParamType::Color,
                                    MatParamValue::MakeColor(1, 1, 1, 1)));
        g.nodes.push_back(Node(3, GraphNodeType::UV));
        GraphNode custom = Node(4, GraphNodeType::Custom);
        custom.customPins = { { "uv", 2 } };
        custom.customOutWidth = 4;
        custom.customBody =
            "float w = 0.5 + 0.5 * sin(uv.x * 12.566 + Time);\n"
            "return Tint * w;";
        g.nodes.push_back(custom);
        g.links.push_back(Link(3, 0, 4, 0));
        g.links.push_back(Link(4, 0, 1, 0));

        const GraphCodegenResult gen = GenerateGraphSnippet(g, MaterialSurface::Fullscreen);
        REQUIRE(gen.Ok());
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());
        const MaterialBuildResult build = BuildMaterialShaderSource(
            *templateText, gen.snippet, "graph_custom", MaterialSurface::Fullscreen);
        REQUIRE(build.errors.empty());
        compileBoth(build.hlsl, "graph_custom.hlsl");
    }

    SECTION("sprite: SpriteTexture * VertexColor * param tint")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(2, GraphNodeType::SpriteTexture));
        g.nodes.push_back(Node(3, GraphNodeType::VertexColor));
        g.nodes.push_back(Node(4, GraphNodeType::Mul));
        g.nodes.push_back(ParamNode(5, "Tint", MatParamType::Color,
                                    MatParamValue::MakeColor(1, 1, 1, 1)));
        g.nodes.push_back(Node(6, GraphNodeType::Mul));
        g.links.push_back(Link(2, 0, 4, 0));
        g.links.push_back(Link(3, 0, 4, 1));
        g.links.push_back(Link(4, 0, 6, 0));
        g.links.push_back(Link(5, 0, 6, 1));
        g.links.push_back(Link(6, 0, 1, 0));

        const GraphCodegenResult gen = GenerateGraphSnippet(g, MaterialSurface::Sprite);
        REQUIRE(gen.Ok());
        CHECK(gen.snippet.find("SpriteTexture.Sample(MaterialSampler, v.uv)") != std::string::npos);
        CHECK(gen.snippet.find("v.color") != std::string::npos);
        const auto templateText = provider.Get("materials/sprite_material.hlsl");
        REQUIRE(templateText.has_value());
        const MaterialBuildResult build = BuildMaterialShaderSource(
            *templateText, gen.snippet, "graph_sprite", MaterialSurface::Sprite);
        REQUIRE(build.errors.empty());
        compileBoth(build.hlsl, "graph_sprite.hlsl");
    }

    sc.Shutdown();
}
