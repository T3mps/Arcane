// Material node graph (Slice 9): node table, topological codegen (SSA locals,
// SG adaptation table, line map), structured errors, JSON round-trip, .arcmat
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

    // An inline pin literal. Lanes beyond the pin's declared width stay 0 (a
    // dynamic pin stores only x -- the scalar rule).
    GraphPinLiteral Literal(std::uint32_t pin, float x, float y = 0.0f,
                            float z = 0.0f, float w = 0.0f)
    {
        GraphPinLiteral l;
        l.pin = pin;
        l.v[0] = x;
        l.v[1] = y;
        l.v[2] = z;
        l.v[3] = w;
        return l;
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
    REQUIRE(infos.size() == static_cast<std::size_t>(GraphNodeType::Comment) + 1);
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
    CHECK(GraphNodeInfo(GraphNodeType::Combine).inputs.size() == 4);
    CHECK(GraphNodeInfo(GraphNodeType::Remap).inputs.size() == 3);
    CHECK(GraphNodeInfo(GraphNodeType::Remap).inputs[1].width == 2);
    CHECK(GraphNodeInfo(GraphNodeType::TilingOffset).inputs.size() == 3);
    CHECK(std::string(GraphNodeInfo(GraphNodeType::Smoothstep).inputs[2].name) == "x");
    CHECK(GraphNodeInfo(GraphNodeType::Swizzle).inputs.size() == 1);
    CHECK(GraphNodeInfo(GraphNodeType::SimpleNoise).inputs.size() == 2);
    CHECK(GraphNodeInfo(GraphNodeType::SimpleNoise).inputs[0].width == 2);
    CHECK(GraphNodeInfo(GraphNodeType::SimpleNoise).outputs[0].width == 1);
    CHECK(GraphNodeInfo(GraphNodeType::PassInput).inputs.size() == 1);     // uv
    CHECK(GraphNodeInfo(GraphNodeType::PassInput).outputs.size() == 2);    // rgba + a
    CHECK(GraphNodeInfo(GraphNodeType::Comment).inputs.empty());           // furniture
    CHECK(GraphNodeInfo(GraphNodeType::Comment).outputs.empty());
}

TEST_CASE("Comment nodes are canvas furniture: ignored by codegen, round-trip",
          "[material]")
{
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    GraphNode c2 = Node(2, GraphNodeType::ConstFloat);
    c2.value[0] = 0.5f;
    g.nodes.push_back(c2);
    g.links.push_back(Link(2, 0, 1, 0));
    GraphNode note = Node(3, GraphNodeType::Comment);
    note.paramName = "the base color block";
    note.value[0] = 280.0f;
    note.value[1] = 160.0f;
    g.nodes.push_back(note);

    // Codegen: identical output with or without the comment.
    const GraphCodegenResult with = GenerateGraphSnippet(g);
    REQUIRE(with.Ok());
    CHECK(with.snippet.find("comment") == std::string::npos);
    CHECK(with.snippet.find("_n3") == std::string::npos);
    MaterialGraph bare = g;
    std::erase_if(bare.nodes, [](const GraphNode& n)
                  { return n.type == GraphNodeType::Comment; });
    CHECK(GenerateGraphSnippet(bare).snippet == with.snippet);

    // Round-trip keeps title + size; node previews skip it.
    const auto back = GraphFromJson(GraphToJson(g));
    REQUIRE(back.has_value());
    const GraphNode* rt = back->FindNode(3);
    REQUIRE(rt != nullptr);
    CHECK(rt->type == GraphNodeType::Comment);
    CHECK(rt->paramName == "the base color block");
    CHECK(rt->value[0] == 280.0f);
    CHECK(rt->value[1] == 160.0f);
    CHECK_FALSE(GenerateNodePreviewSnippet(g, 3).Ok());
}

TEST_CASE("Codegen: the Vertex Output context emits displace()", "[material]")
{
    // Speed(param) * Time -> Sine -> posOffset; the same param also feeds the
    // pixel walk (shared decls, separate function scopes).
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    g.nodes.push_back(ParamNode(2, "Speed", MatParamType::Float,
                                MatParamValue::MakeFloat(2.0f)));
    g.nodes.push_back(Node(3, GraphNodeType::Time));
    g.nodes.push_back(Node(4, GraphNodeType::Mul));
    g.nodes.push_back(Node(5, GraphNodeType::Sin));
    g.nodes.push_back(Node(6, GraphNodeType::VertexOutput));
    g.links.push_back(Link(3, 0, 4, 0));
    g.links.push_back(Link(2, 0, 4, 1));
    g.links.push_back(Link(4, 0, 5, 0));
    g.links.push_back(Link(5, 0, 6, 0));   // -> posOffset
    g.links.push_back(Link(2, 0, 1, 0));   // Speed also -> Output (pixel walk)

    SECTION("both bodies generate; connected pins only")
    {
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE(r.Ok());
        REQUIRE_FALSE(r.vertexSnippet.empty());
        CHECK(r.vertexSnippet.find("Varyings displace(Varyings v)") != std::string::npos);
        CHECK(r.vertexSnippet.find("v.pos.xy += (_n5).xx;") != std::string::npos);
        CHECK(r.vertexSnippet.find("v.uv") == std::string::npos);      // unconnected
        CHECK(r.vertexSnippet.find("v.color") == std::string::npos);   // unconnected
        CHECK(r.vertexSnippet.find("return v;") != std::string::npos);
        // The pixel snippet is untouched by the vertex walk.
        CHECK(r.snippet.find("displace") == std::string::npos);
        CHECK(r.snippet.find("//@param float Speed") != std::string::npos);
    }

    SECTION("no Vertex Output = empty vertexSnippet (passthrough)")
    {
        std::erase_if(g.nodes, [](const GraphNode& n)
                      { return n.type == GraphNodeType::VertexOutput; });
        std::erase_if(g.links, [](const GraphLink& l) { return l.toNode == 6; });
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE(r.Ok());
        CHECK(r.vertexSnippet.empty());
    }

    SECTION("context violations are structured errors")
    {
        // A second Vertex Output.
        GraphNode dup = Node(7, GraphNodeType::VertexOutput);
        g.nodes.push_back(dup);
        CHECK_FALSE(GenerateGraphSnippet(g).Ok());
        g.nodes.pop_back();

        // Vertex Output inside a pass graph (the EXPLICIT pass flag -- a base
        // graph with wired scene inputs keeps its vertex stage).
        CHECK(HasErrorOn(
            GenerateGraphSnippet(g, MaterialSurface::Fullscreen, 1, true), 6));
        CHECK(GenerateGraphSnippet(g, MaterialSurface::Fullscreen, 1, false).Ok());

        // The color pin on the fullscreen surface.
        g.nodes.push_back(Node(8, GraphNodeType::ConstColor));
        g.links.push_back(Link(8, 0, 6, 2));
        CHECK(HasErrorOn(GenerateGraphSnippet(g), 6));
        // ...but fine on sprite.
        CHECK(GenerateGraphSnippet(g, MaterialSurface::Sprite).Ok());

        // Pass Input can never drive the vertex stage (the vertex context
        // lives on the base graph, which has no upstream passes).
        MaterialGraph pg;
        pg.nodes.push_back(Node(1, GraphNodeType::Output));
        GraphNode pin = Node(2, GraphNodeType::PassInput);
        pg.nodes.push_back(pin);
        pg.nodes.push_back(Node(3, GraphNodeType::VertexOutput));
        pg.links.push_back(Link(2, 0, 3, 0));
        CHECK(HasErrorOn(GenerateGraphSnippet(pg), 2));
    }

    SECTION("texture reads in the vertex walk emit SampleLevel")
    {
        // UE's WPO model: displacement may sample textures -- with an
        // explicit mip (no implicit derivatives in VS). The pixel walk of the
        // SAME node keeps plain Sample.
        MaterialGraph tg;
        tg.nodes.push_back(Node(1, GraphNodeType::Output));
        tg.nodes.push_back(TexNode(2, "Noise"));
        tg.nodes.push_back(Node(3, GraphNodeType::VertexOutput));
        tg.links.push_back(Link(2, 0, 3, 0));
        tg.links.push_back(Link(2, 0, 1, 0));
        const GraphCodegenResult r = GenerateGraphSnippet(tg);
        REQUIRE(r.Ok());
        CHECK(r.snippet.find("Noise.Sample(MaterialSampler, v.uv);") !=
              std::string::npos);
        CHECK(r.vertexSnippet.find(
                  "Noise.SampleLevel(MaterialSampler, v.uv, 0.0);") !=
              std::string::npos);
        CHECK(r.vertexSnippet.find(".Sample(") == std::string::npos);
    }

    SECTION("a Custom node reachable from BOTH walks defines _cf once")
    {
        MaterialGraph cg;
        cg.nodes.push_back(Node(1, GraphNodeType::Output));
        GraphNode custom = Node(2, GraphNodeType::Custom);
        custom.customPins = { { "x", 1 } };
        custom.customOutWidth = 2;
        custom.customBody = "return float2(x, -x);";
        cg.nodes.push_back(custom);
        cg.nodes.push_back(Node(3, GraphNodeType::Time));
        cg.nodes.push_back(Node(4, GraphNodeType::VertexOutput));
        cg.links.push_back(Link(3, 0, 2, 0));
        cg.links.push_back(Link(2, 0, 1, 0));   // -> pixel walk
        cg.links.push_back(Link(2, 0, 4, 0));   // -> vertex walk (posOffset)
        const GraphCodegenResult r = GenerateGraphSnippet(cg);
        REQUIRE(r.Ok());
        std::size_t defs = 0;
        for (std::size_t at = r.snippet.find("float2 _cf2(");
             at != std::string::npos; at = r.snippet.find("float2 _cf2(", at + 1))
            ++defs;
        CHECK(defs == 1);
        // ...and BOTH bodies call it (each walk resolves its own SSA local
        // for the Time input).
        CHECK(r.snippet.find("_cf2(_n3)") != std::string::npos);
        CHECK(r.vertexSnippet.find("_cf2(_n3)") != std::string::npos);
    }
}

TEST_CASE("Codegen: PassInput samples wired upstream slots only", "[material]")
{
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    GraphNode in0 = Node(2, GraphNodeType::PassInput);      // slot 0 default
    GraphNode in1 = Node(3, GraphNodeType::PassInput);
    in1.passInputSlot = 1;
    g.nodes.push_back(in0);
    g.nodes.push_back(in1);
    g.nodes.push_back(Node(4, GraphNodeType::Add));
    g.links.push_back(Link(2, 0, 4, 0));
    g.links.push_back(Link(3, 0, 4, 1));
    g.links.push_back(Link(4, 0, 1, 0));

    SECTION("wired slots emit InputTexture / InputTexture1 samples")
    {
        const GraphCodegenResult r =
            GenerateGraphSnippet(g, MaterialSurface::Fullscreen, /*availableInputs=*/2);
        REQUIRE(r.Ok());
        CHECK(r.snippet.find(
                  "float4 _n2_rgba = InputTexture.Sample(MaterialSampler, v.uv);") !=
              std::string::npos);
        CHECK(r.snippet.find(
                  "float4 _n3_rgba = InputTexture1.Sample(MaterialSampler, v.uv);") !=
              std::string::npos);
    }

    SECTION("an unwired slot is a structured error on the node")
    {
        const GraphCodegenResult r =
            GenerateGraphSnippet(g, MaterialSurface::Fullscreen, /*availableInputs=*/1);
        REQUIRE_FALSE(r.Ok());
        CHECK_FALSE(HasErrorOn(r, 2));   // slot 0 is wired
        CHECK(HasErrorOn(r, 3));         // slot 1 is not
    }

    SECTION("base graphs (no pass context) refuse PassInput entirely")
    {
        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE_FALSE(r.Ok());
        CHECK(HasErrorOn(r, 2));
        CHECK(HasErrorOn(r, 3));
    }

    SECTION("the slot survives the JSON round-trip")
    {
        const auto back = GraphFromJson(GraphToJson(g));
        REQUIRE(back.has_value());
        CHECK(back->FindNode(3)->passInputSlot == 1);
        CHECK(back->FindNode(2)->passInputSlot == 0);
    }
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

TEST_CASE("pin literal feeds an unwired input and beats the neutral default",
          "[material][graph]")
{
    // const_float(2) -> mul.a; mul.b UNWIRED but carrying the literal 3.
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    GraphNode two = Node(2, GraphNodeType::ConstFloat);
    two.value[0] = 2.0f;
    g.nodes.push_back(two);
    GraphNode mul = Node(3, GraphNodeType::Mul);
    mul.pinLiterals.push_back(Literal(1, 3.0f));
    g.nodes.push_back(mul);
    g.links.push_back(Link(2, 0, 3, 0));
    g.links.push_back(Link(3, 0, 1, 0));

    const GraphCodegenResult r = GenerateGraphSnippet(g);
    REQUIRE(r.Ok());
    // Inline into the consumer's statement: the literal is NOT a node, so it
    // adds no line -- 4 scaffolding lines + one statement per node, exactly
    // what a Const node feeding mul.b would NOT have produced.
    CHECK(r.snippet ==
          "float4 shade(Varyings v)\n"
          "{\n"
          "    float _n2 = 2;\n"
          "    float _n3 = _n2 * 3;\n"
          "    return (_n3).xxxx;\n"
          "}\n");
    CHECK(r.lineNodeIds.size() == 6);

    SECTION("a literal overrides a NON-ZERO neutral default")
    {
        // clamp.x wired (float2), clamp.max unwired with the literal 0.5 --
        // the neutral "1.0" that argOr would otherwise pass must lose.
        MaterialGraph c;
        c.nodes.push_back(Node(1, GraphNodeType::Output));
        c.nodes.push_back(Node(2, GraphNodeType::UV));
        GraphNode clamp = Node(3, GraphNodeType::Clamp);
        clamp.pinLiterals.push_back(Literal(2, 0.5f));
        c.nodes.push_back(clamp);
        c.links.push_back(Link(2, 0, 3, 0));
        c.links.push_back(Link(3, 0, 1, 0));

        const GraphCodegenResult cr = GenerateGraphSnippet(c);
        REQUIRE(cr.Ok());
        CHECK(cr.snippet.find("float2 _n3 = clamp(_n2, (0.0).xx, (0.5).xx);") !=
              std::string::npos);
        CHECK(cr.snippet.find("(1.0).xx") == std::string::npos);   // the neutral it replaced
        // min stayed neutral: only the pin with a literal changed.
        CHECK(cr.snippet.find("(0.0).xx") != std::string::npos);
    }

    SECTION("a WIRE beats the literal, which survives underneath")
    {
        // SG behavior: wiring hides the value, it is not destroyed -- so the
        // stored literal must still be there for an unwire to restore.
        GraphNode four = Node(4, GraphNodeType::ConstFloat);
        four.value[0] = 4.0f;
        g.nodes.push_back(four);
        g.links.push_back(Link(4, 0, 3, 1));   // -> mul.b, the pin holding the literal

        const GraphCodegenResult wired = GenerateGraphSnippet(g);
        REQUIRE(wired.Ok());
        CHECK(wired.snippet.find("float _n3 = _n2 * _n4;") != std::string::npos);
        CHECK(wired.snippet.find("* 3") == std::string::npos);
        REQUIRE(g.FindNode(3)->FindPinLiteral(1) != nullptr);
        CHECK(g.FindNode(3)->FindPinLiteral(1)->v[0] == 3.0f);
    }

    SECTION("Custom nodes get literals too -- they share the argOr seam")
    {
        // Custom pins are per-node data, but their emission calls the SAME
        // arg()/argOr() helper as every other node, so literals fall out with
        // no per-type work. FindPinLiteral indexes customPins order.
        MaterialGraph cg;
        cg.nodes.push_back(Node(1, GraphNodeType::Output));
        GraphNode custom = Node(2, GraphNodeType::Custom);
        custom.customPins = { { "uv", 2 }, { "t", 1 } };
        custom.customOutWidth = 4;
        custom.customBody = "return float4(uv, t, 1.0);";
        custom.pinLiterals.push_back(Literal(0, 0.25f, 0.75f));   // float2 pin
        custom.pinLiterals.push_back(Literal(1, 6.0f));           // scalar pin
        cg.nodes.push_back(custom);
        cg.links.push_back(Link(2, 0, 1, 0));

        const GraphCodegenResult cr = GenerateGraphSnippet(cg);
        REQUIRE(cr.Ok());
        CHECK(cr.snippet.find("float4 _n2 = _cf2(float2(0.25, 0.75), 6);") !=
              std::string::npos);
    }
}

TEST_CASE("pin literal on a dynamic pin stays scalar and does not pin width",
          "[material][graph]")
{
    // add.a wired to a float2 const, add.b unwired with the scalar literal
    // 1.5: width resolution counts CONNECTED inputs only, so the node stays
    // float2 and the literal splats through the same adaptation a wire would.
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    GraphNode c2 = Node(2, GraphNodeType::ConstFloat2);
    c2.value[0] = 0.25f;
    c2.value[1] = 0.75f;
    g.nodes.push_back(c2);
    GraphNode add = Node(3, GraphNodeType::Add);
    add.pinLiterals.push_back(Literal(1, 1.5f));
    g.nodes.push_back(add);
    g.links.push_back(Link(2, 0, 3, 0));
    g.links.push_back(Link(3, 0, 1, 0));

    const GraphCodegenResult r = GenerateGraphSnippet(g);
    REQUIRE(r.Ok());
    CHECK(r.snippet.find("float2 _n3 = _n2 + (1.5).xx;") != std::string::npos);

    SECTION("literals alone never widen a node")
    {
        // Nothing connected: the resolved width stays 1 even though a literal
        // sits on a dynamic pin -- literals are not inputs to the width rule.
        MaterialGraph l;
        l.nodes.push_back(Node(1, GraphNodeType::Output));
        GraphNode mul = Node(2, GraphNodeType::Mul);
        mul.pinLiterals.push_back(Literal(0, 4.0f));
        l.nodes.push_back(mul);
        l.links.push_back(Link(2, 0, 1, 0));

        const GraphCodegenResult lr = GenerateGraphSnippet(l);
        REQUIRE(lr.Ok());
        CHECK(lr.snippet.find("float _n2 = 4 * 0.0;") != std::string::npos);
    }
}

TEST_CASE("graph without literals emits byte-identical snippets", "[material][graph]")
{
    // The tripwire for "literals cost nothing when unused". There are no
    // golden snippet FILES in this suite, so the equivalence is proven two
    // ways: the same graph round-tripped through JSON (which, with no
    // literals, writes no "pinDefaults" key at all) must generate the exact
    // same text, AND that text must still show the pre-change neutral "0.0"
    // path on a deliberately unwired pin.
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    g.nodes.push_back(TexNode(2, "Albedo"));
    g.nodes.push_back(Node(3, GraphNodeType::Mul));   // b left unwired
    g.links.push_back(Link(2, 0, 3, 0));
    g.links.push_back(Link(3, 0, 1, 0));

    const GraphCodegenResult direct = GenerateGraphSnippet(g);
    REQUIRE(direct.Ok());
    CHECK(direct.snippet.find("float4 _n3 = _n2_rgba * (0.0).xxxx;") != std::string::npos);

    const nlohmann::json j = GraphToJson(g);
    for (const nlohmann::json& e : j["nodes"])
        CHECK_FALSE(e.contains("pinDefaults"));   // absent, not an empty array

    const auto back = GraphFromJson(j);
    REQUIRE(back.has_value());
    CHECK(back->FindNode(3)->pinLiterals.empty());
    const GraphCodegenResult roundTrip = GenerateGraphSnippet(*back);
    REQUIRE(roundTrip.Ok());
    CHECK(roundTrip.snippet == direct.snippet);
    CHECK(roundTrip.lineNodeIds == direct.lineNodeIds);
}

TEST_CASE("pinDefaults serialization round-trip and tolerance", "[material][graph]")
{
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    GraphNode tile = Node(2, GraphNodeType::TilingOffset);   // fixed float2 pins
    tile.pinLiterals.push_back(Literal(1, 2.0f, 3.0f));      // tiling
    g.nodes.push_back(tile);
    GraphNode mul = Node(3, GraphNodeType::Mul);             // dynamic pins
    mul.pinLiterals.push_back(Literal(1, 0.25f));
    g.nodes.push_back(mul);
    g.links.push_back(Link(2, 0, 3, 0));
    g.links.push_back(Link(3, 0, 1, 0));
    g.nextId = 4;

    const nlohmann::json j = GraphToJson(g);
    // Value shape follows the pin's declared width: array for the fixed
    // float2 pin, a bare number for the dynamic (scalar) one.
    REQUIRE(j["nodes"][1].contains("pinDefaults"));
    REQUIRE(j["nodes"][2].contains("pinDefaults"));
    const nlohmann::json& tileDefs = j["nodes"][1]["pinDefaults"];
    REQUIRE(tileDefs.is_array());
    REQUIRE(tileDefs.size() == 1);
    CHECK(tileDefs[0]["pin"] == 1);
    REQUIRE(tileDefs[0]["value"].is_array());
    REQUIRE(tileDefs[0]["value"].size() == 2);
    CHECK(tileDefs[0]["value"][0] == 2.0f);
    CHECK(tileDefs[0]["value"][1] == 3.0f);
    const nlohmann::json& mulDefs = j["nodes"][2]["pinDefaults"];
    REQUIRE(mulDefs.is_array());
    REQUIRE(mulDefs.size() == 1);
    REQUIRE(mulDefs[0]["value"].is_number());
    CHECK(mulDefs[0]["value"] == 0.25f);

    const auto back = GraphFromJson(j);
    REQUIRE(back.has_value());
    const GraphPinLiteral* t = back->FindNode(2)->FindPinLiteral(1);
    REQUIRE(t != nullptr);
    CHECK(t->v[0] == 2.0f);
    CHECK(t->v[1] == 3.0f);
    CHECK(back->FindNode(2)->FindPinLiteral(0) == nullptr);   // sparse: uv untouched
    const GraphPinLiteral* m = back->FindNode(3)->FindPinLiteral(1);
    REQUIRE(m != nullptr);
    CHECK(m->v[0] == 0.25f);
    CHECK(GraphToJson(*back).dump() == j.dump());   // byte-stable second pass

    SECTION("both value shapes load (hand-authored files)")
    {
        nlohmann::json hand = j;
        hand["nodes"][1]["pinDefaults"][0]["value"] = 0.5;   // number on a float2 pin
        hand["nodes"][2]["pinDefaults"][0]["value"] =
            nlohmann::json::array({ 0.75, 0.0, 0.0, 0.0 });   // array on a scalar pin
        // ...and a hand-built entry whose "pin" is a SIGNED integer (what any
        // JSON built in code, rather than parsed, produces).
        hand["nodes"][1]["pinDefaults"].push_back(
            nlohmann::json{ { "pin", 2 },
                            { "value", nlohmann::json::array({ 4.0, 5.0 }) } });
        const auto loaded = GraphFromJson(hand);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->FindNode(2)->FindPinLiteral(2) != nullptr);
        CHECK(loaded->FindNode(2)->FindPinLiteral(2)->v[0] == 4.0f);
        CHECK(loaded->FindNode(2)->FindPinLiteral(2)->v[1] == 5.0f);
        REQUIRE(loaded->FindNode(2)->FindPinLiteral(1) != nullptr);
        CHECK(loaded->FindNode(2)->FindPinLiteral(1)->v[0] == 0.5f);
        CHECK(loaded->FindNode(2)->FindPinLiteral(1)->v[1] == 0.0f);
        REQUIRE(loaded->FindNode(3)->FindPinLiteral(1) != nullptr);
        CHECK(loaded->FindNode(3)->FindPinLiteral(1)->v[0] == 0.75f);
    }

    SECTION("an unknown pin index is dropped; the file survives intact")
    {
        nlohmann::json hand = j;
        hand["nodes"][2]["pinDefaults"].push_back(
            nlohmann::json{ { "pin", 9 }, { "value", 1.0 } });   // Mul has 2 inputs
        const auto loaded = GraphFromJson(hand);
        REQUIRE(loaded.has_value());              // NOT the nullopt refusal path
        CHECK(loaded->nodes.size() == 3);
        CHECK(loaded->links.size() == 2);
        REQUIRE(loaded->FindNode(3)->pinLiterals.size() == 1);
        CHECK(loaded->FindNode(3)->FindPinLiteral(1) != nullptr);
        CHECK(loaded->FindNode(3)->FindPinLiteral(9) == nullptr);
    }

    SECTION("an absent pinDefaults field leaves the node literal-free")
    {
        nlohmann::json hand = j;
        hand["nodes"][2].erase("pinDefaults");
        const auto loaded = GraphFromJson(hand);
        REQUIRE(loaded.has_value());
        CHECK(loaded->FindNode(3)->pinLiterals.empty());
        CHECK(loaded->FindNode(3)->FindPinLiteral(1) == nullptr);
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

TEST_CASE("Codegen: library batch 1 kernels and their neutral defaults", "[material]")
{
    SECTION("Time -> Cosine -> Remap (default ranges) -> Combine.r")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(4, GraphNodeType::Time));
        g.nodes.push_back(Node(5, GraphNodeType::Cos));
        g.nodes.push_back(Node(6, GraphNodeType::Remap));
        g.nodes.push_back(Node(7, GraphNodeType::Combine));
        g.links.push_back(Link(4, 0, 5, 0));
        g.links.push_back(Link(5, 0, 6, 0));
        g.links.push_back(Link(6, 0, 7, 0));
        g.links.push_back(Link(7, 0, 1, 0));

        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE(r.Ok());
        CHECK(r.snippet ==
              "float4 shade(Varyings v)\n"
              "{\n"
              "    float _n4 = Time;\n"
              "    float _n5 = cos(_n4);\n"
              "    float _n6 = (float2(0.0, 1.0)).x + (_n5 - (float2(0.0, 1.0)).x) * "
              "((float2(0.0, 1.0)).y - (float2(0.0, 1.0)).x) / "
              "((float2(0.0, 1.0)).y - (float2(0.0, 1.0)).x);\n"
              "    float4 _n7 = float4(_n6, 0.0, 0.0, 1.0);\n"
              "    return _n7;\n"
              "}\n");
    }

    SECTION("float2 chain: clamp/smoothstep/step/pow/min/max/abs/tiling at width 2")
    {
        // One UV-fed chain; every unconnected operand takes its NEUTRAL
        // default (0, except clamp.max / smoothstep.edge1 / pow.b / tiling = 1).
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(2, GraphNodeType::UV));
        g.nodes.push_back(Node(3, GraphNodeType::Clamp));
        g.nodes.push_back(Node(4, GraphNodeType::Smoothstep));
        g.nodes.push_back(Node(5, GraphNodeType::Step));
        g.nodes.push_back(Node(6, GraphNodeType::Power));
        g.nodes.push_back(Node(7, GraphNodeType::Min));
        g.nodes.push_back(Node(8, GraphNodeType::Max));
        g.nodes.push_back(Node(9, GraphNodeType::Abs));
        g.nodes.push_back(Node(10, GraphNodeType::TilingOffset));
        g.links.push_back(Link(2, 0, 3, 0));    // uv -> clamp.x
        g.links.push_back(Link(3, 0, 4, 2));    // -> smoothstep.x
        g.links.push_back(Link(4, 0, 5, 0));    // -> step.edge
        g.links.push_back(Link(5, 0, 6, 0));    // -> pow.a
        g.links.push_back(Link(6, 0, 7, 0));    // -> min.a
        g.links.push_back(Link(7, 0, 8, 0));    // -> max.a
        g.links.push_back(Link(8, 0, 9, 0));    // -> abs.x
        g.links.push_back(Link(9, 0, 10, 0));   // -> tiling.uv
        g.links.push_back(Link(10, 0, 1, 0));

        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE(r.Ok());
        CHECK(r.snippet ==
              "float4 shade(Varyings v)\n"
              "{\n"
              "    float2 _n2 = v.uv;\n"
              "    float2 _n3 = clamp(_n2, (0.0).xx, (1.0).xx);\n"
              "    float2 _n4 = smoothstep((0.0).xx, (1.0).xx, _n3);\n"
              "    float2 _n5 = step(_n4, (0.0).xx);\n"
              "    float2 _n6 = pow(_n5, (1.0).xx);\n"
              "    float2 _n7 = min(_n6, (0.0).xx);\n"
              "    float2 _n8 = max(_n7, (0.0).xx);\n"
              "    float2 _n9 = abs(_n8);\n"
              "    float2 _n10 = _n9 * (1.0).xx + (0.0).xx;\n"
              "    return float4(_n10, 0.0, 1.0);\n"
              "}\n");
    }
}

TEST_CASE("Codegen: Swizzle masks and the SimpleNoise emit-once helper", "[material]")
{
    SECTION("all-present mask emits a real HLSL swizzle")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(2, GraphNodeType::UV));
        GraphNode s = Node(3, GraphNodeType::Swizzle);
        s.swizzleMask = "yx";
        g.nodes.push_back(s);
        g.links.push_back(Link(2, 0, 3, 0));
        g.links.push_back(Link(3, 0, 1, 0));

        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE(r.Ok());
        CHECK(r.snippet ==
              "float4 shade(Varyings v)\n"
              "{\n"
              "    float2 _n2 = v.uv;\n"
              "    float2 _n3 = (_n2).yx;\n"
              "    return float4(_n3, 0.0, 1.0);\n"
              "}\n");
    }

    SECTION("absent lanes read 0 through a constructor (Split rule)")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(2, GraphNodeType::UV));
        GraphNode s = Node(3, GraphNodeType::Swizzle);
        s.swizzleMask = "xyzw";   // z/w over a float2 source
        g.nodes.push_back(s);
        g.links.push_back(Link(2, 0, 3, 0));
        g.links.push_back(Link(3, 0, 1, 0));

        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE(r.Ok());
        CHECK(r.snippet.find("float4 _n3 = float4((_n2).x, (_n2).y, 0.0, 0.0);") !=
              std::string::npos);
    }

    SECTION("invalid masks are structured errors")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        GraphNode s = Node(2, GraphNodeType::Swizzle);
        s.swizzleMask = "xyz";   // no float3 in the value set
        g.nodes.push_back(s);
        g.links.push_back(Link(2, 0, 1, 0));
        CHECK(HasErrorOn(GenerateGraphSnippet(g), 2));

        g.nodes[1].swizzleMask = "rg";   // xyzw only
        CHECK(HasErrorOn(GenerateGraphSnippet(g), 2));
        g.nodes[1].swizzleMask = "";
        CHECK(HasErrorOn(GenerateGraphSnippet(g), 2));
    }

    SECTION("mask survives the JSON round-trip")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        GraphNode s = Node(2, GraphNodeType::Swizzle);
        s.swizzleMask = "wzyx";
        g.nodes.push_back(s);
        const auto back = GraphFromJson(GraphToJson(g));
        REQUIRE(back.has_value());
        CHECK(back->FindNode(2)->swizzleMask == "wzyx");
    }

    SECTION("two SimpleNoise nodes share ONE emitted helper")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(2, GraphNodeType::SimpleNoise));
        g.nodes.push_back(Node(3, GraphNodeType::SimpleNoise));
        g.nodes.push_back(Node(4, GraphNodeType::Add));
        g.links.push_back(Link(2, 0, 4, 0));
        g.links.push_back(Link(3, 0, 4, 1));
        g.links.push_back(Link(4, 0, 1, 0));

        const GraphCodegenResult r = GenerateGraphSnippet(g);
        REQUIRE(r.Ok());
        auto countOf = [](const std::string& hay, const std::string& needle)
        {
            std::size_t c = 0, pos = 0;
            while ((pos = hay.find(needle, pos)) != std::string::npos)
            {
                ++c;
                pos += needle.size();
            }
            return c;
        };
        CHECK(countOf(r.snippet, "float _g_simple_noise(float2 uv)") == 1);
        CHECK(countOf(r.snippet, "float _g_hash21(float2 p)") == 1);
        // Both nodes call it (uv defaults v.uv, scale defaults 10).
        CHECK(countOf(r.snippet, "_g_simple_noise((v.uv) * 10.0)") == 2);
    }
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

TEST_CASE("Node preview codegen: adaptation wrap, stripping, contexts", "[material]")
{
    // The editor's per-node thumbnails: clone + rewire through a synthetic
    // Custom node whose width-4 pin makes the adaptation table do the work --
    // scalars splat to grayscale, and the body forces alpha opaque.
    MaterialGraph g;
    g.nodes.push_back(Node(1, GraphNodeType::Output));
    g.nodes.push_back(Node(2, GraphNodeType::Time));
    g.nodes.push_back(Node(3, GraphNodeType::Sin));
    g.links.push_back(Link(2, 0, 3, 0));
    g.links.push_back(Link(3, 0, 1, 0));

    SECTION("scalar target wraps to grayscale with opaque alpha")
    {
        const GraphCodegenResult r = GenerateNodePreviewSnippet(g, 3);
        REQUIRE(r.Ok());
        // Call-site adaptation splats the scalar to the width-4 pin; the wrap
        // body forces alpha 1 (opaque).
        CHECK(r.snippet.find(".xxxx") != std::string::npos);
        CHECK(r.snippet.find("float4(value.rgb, 1.0);") != std::string::npos);
        CHECK(r.snippet.find("float4 shade(Varyings v)") != std::string::npos);
        // Deterministic: same graph, same snippet.
        CHECK(GenerateNodePreviewSnippet(g, 3).snippet == r.snippet);
        // The original graph was not touched.
        CHECK(g.nodes.size() == 3);
        CHECK(g.links.size() == 2);
    }
    SECTION("previews reach nodes the Output cannot see")
    {
        g.nodes.push_back(Node(7, GraphNodeType::UV));   // disconnected island
        const GraphCodegenResult r = GenerateNodePreviewSnippet(g, 7);
        REQUIRE(r.Ok());
        CHECK(r.snippet.find("v.uv") != std::string::npos);
    }
    SECTION("Output and Vertex Output are not previewable; missing node errors")
    {
        CHECK_FALSE(GenerateNodePreviewSnippet(g, 1).Ok());
        g.nodes.push_back(Node(6, GraphNodeType::VertexOutput));
        CHECK_FALSE(GenerateNodePreviewSnippet(g, 6).Ok());
        CHECK_FALSE(GenerateNodePreviewSnippet(g, 42).Ok());
    }
    SECTION("Vertex Output nodes are stripped from the clone")
    {
        g.nodes.push_back(Node(6, GraphNodeType::VertexOutput));
        g.links.push_back(Link(3, 0, 6, 0));   // sine drives posOffset too
        const GraphCodegenResult r = GenerateNodePreviewSnippet(g, 3);
        REQUIRE(r.Ok());
        CHECK(r.vertexSnippet.empty());   // previews are pixel values only
    }
    SECTION("pass context: PassInput previews under wired slots only")
    {
        MaterialGraph pg;
        pg.nodes.push_back(Node(1, GraphNodeType::Output));
        GraphNode in = Node(2, GraphNodeType::PassInput);
        in.passInputSlot = 1;
        pg.nodes.push_back(in);
        pg.links.push_back(Link(2, 0, 1, 0));
        CHECK_FALSE(GenerateNodePreviewSnippet(pg, 2, 0).Ok());   // unwired
        const GraphCodegenResult r = GenerateNodePreviewSnippet(pg, 2, 2);
        REQUIRE(r.Ok());
        CHECK(r.snippet.find("InputTexture1") != std::string::npos);
    }
    SECTION("sprite-only nodes refuse (thumbnails render fullscreen)")
    {
        MaterialGraph sg;
        sg.nodes.push_back(Node(1, GraphNodeType::Output));
        sg.nodes.push_back(Node(2, GraphNodeType::VertexColor));
        sg.links.push_back(Link(2, 0, 1, 0));
        REQUIRE(GenerateGraphSnippet(sg, MaterialSurface::Sprite).Ok());
        CHECK_FALSE(GenerateNodePreviewSnippet(sg, 2).Ok());
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

TEST_CASE(".arcmat carries the graph; graph-only files self-heal a snippet", "[material]")
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

    const fs::path path = dir / "graphmat.arcmat";
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
        const fs::path bare = dir / "bare.arcmat";
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
        const fs::path inst = dir / "inst.arcmat";
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

    SECTION("fullscreen: texture + noise + Custom drive the VERTEX stage")
    {
        // The lifted bars end-to-end: displacement built from SimpleNoise (a
        // shared helper), a texture read (SampleLevel in VS), and a Custom
        // node used by BOTH walks -- proving %{MATERIAL_BODY}-before-
        // %{VERTEX_BODY} declaration order and the single _cf definition
        // survive real DXC on both targets.
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(TexNode(2, "HeightMap"));
        g.nodes.push_back(Node(3, GraphNodeType::SimpleNoise));
        GraphNode custom = Node(4, GraphNodeType::Custom);
        custom.customPins = { { "a", 1 }, { "b", 1 } };
        custom.customOutWidth = 2;
        custom.customBody = "return float2(a, b) * (0.5 + 0.5 * sin(Time));";
        g.nodes.push_back(custom);
        g.nodes.push_back(Node(5, GraphNodeType::VertexOutput));
        g.links.push_back(Link(2, 1, 4, 0));   // heightmap alpha -> custom.a
        g.links.push_back(Link(3, 0, 4, 1));   // noise -> custom.b
        g.links.push_back(Link(4, 0, 5, 0));   // -> posOffset (vertex walk)
        g.links.push_back(Link(2, 0, 1, 0));   // rgba also -> pixel Output

        const GraphCodegenResult gen = GenerateGraphSnippet(g, MaterialSurface::Fullscreen);
        REQUIRE(gen.Ok());
        REQUIRE_FALSE(gen.vertexSnippet.empty());
        CHECK(gen.vertexSnippet.find("SampleLevel") != std::string::npos);
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());
        const MaterialBuildResult build = BuildMaterialShaderSource(
            *templateText, gen.snippet, "graph_vtx_helpers",
            MaterialSurface::Fullscreen, gen.vertexSnippet);
        REQUIRE(build.errors.empty());
        compileBoth(build.hlsl, "graph_vtx_helpers.hlsl");
    }

    SECTION("fullscreen: every library-batch-1 kernel in one graph")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(2, GraphNodeType::UV));
        GraphNode tiling = Node(3, GraphNodeType::ConstFloat2);
        tiling.value[0] = 2.0f; tiling.value[1] = 2.0f;
        g.nodes.push_back(tiling);
        g.nodes.push_back(Node(4, GraphNodeType::TilingOffset));
        g.nodes.push_back(Node(5, GraphNodeType::Split));
        g.nodes.push_back(Node(6, GraphNodeType::Time));
        g.nodes.push_back(Node(7, GraphNodeType::Cos));
        GraphNode inRange = Node(8, GraphNodeType::ConstFloat2);
        inRange.value[0] = -1.0f; inRange.value[1] = 1.0f;
        g.nodes.push_back(inRange);
        GraphNode outRange = Node(9, GraphNodeType::ConstFloat2);
        outRange.value[0] = 0.0f; outRange.value[1] = 1.0f;
        g.nodes.push_back(outRange);
        g.nodes.push_back(Node(10, GraphNodeType::Remap));
        g.nodes.push_back(Node(11, GraphNodeType::Power));
        g.nodes.push_back(Node(12, GraphNodeType::Clamp));
        g.nodes.push_back(Node(13, GraphNodeType::Smoothstep));
        g.nodes.push_back(Node(14, GraphNodeType::Step));
        g.nodes.push_back(Node(15, GraphNodeType::Min));
        g.nodes.push_back(Node(16, GraphNodeType::Max));
        g.nodes.push_back(Node(17, GraphNodeType::Abs));
        g.nodes.push_back(Node(18, GraphNodeType::Combine));
        g.links.push_back(Link(2, 0, 4, 0));     // uv -> tiling.uv
        g.links.push_back(Link(3, 0, 4, 1));     // const2 -> tiling.tiling
        g.links.push_back(Link(4, 0, 5, 0));     // -> split
        g.links.push_back(Link(6, 0, 7, 0));     // time -> cos
        g.links.push_back(Link(7, 0, 10, 0));    // cos -> remap.x
        g.links.push_back(Link(8, 0, 10, 1));    // -> remap.inRange
        g.links.push_back(Link(9, 0, 10, 2));    // -> remap.outRange
        g.links.push_back(Link(10, 0, 11, 0));   // -> pow.a
        g.links.push_back(Link(5, 0, 11, 1));    // split.r -> pow.b
        g.links.push_back(Link(11, 0, 12, 0));   // -> clamp.x
        g.links.push_back(Link(12, 0, 13, 2));   // -> smoothstep.x
        g.links.push_back(Link(13, 0, 14, 0));   // -> step.edge
        g.links.push_back(Link(5, 1, 14, 1));    // split.g -> step.x
        g.links.push_back(Link(14, 0, 15, 0));   // -> min.a
        g.links.push_back(Link(7, 0, 15, 1));    // cos -> min.b
        g.links.push_back(Link(15, 0, 16, 0));   // -> max.a
        g.links.push_back(Link(12, 0, 16, 1));   // clamp -> max.b
        g.links.push_back(Link(16, 0, 17, 0));   // -> abs
        g.links.push_back(Link(17, 0, 18, 0));   // -> combine.r
        g.links.push_back(Link(10, 0, 18, 2));   // remap -> combine.b
        g.links.push_back(Link(18, 0, 1, 0));

        const GraphCodegenResult gen = GenerateGraphSnippet(g, MaterialSurface::Fullscreen);
        REQUIRE(gen.Ok());
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());
        const MaterialBuildResult build = BuildMaterialShaderSource(
            *templateText, gen.snippet, "graph_library1", MaterialSurface::Fullscreen);
        REQUIRE(build.errors.empty());
        compileBoth(build.hlsl, "graph_library1.hlsl");
    }

    SECTION("fullscreen: Swizzle (both forms) + SimpleNoise through the helper")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(Node(2, GraphNodeType::UV));
        g.nodes.push_back(Node(3, GraphNodeType::SimpleNoise));
        GraphNode scale = Node(4, GraphNodeType::ConstFloat);
        scale.value[0] = 20.0f;
        g.nodes.push_back(scale);
        GraphNode sw2 = Node(7, GraphNodeType::Swizzle);
        sw2.swizzleMask = "xw";   // scalar source -> constructor form
        g.nodes.push_back(sw2);
        g.nodes.push_back(Node(5, GraphNodeType::Combine));
        GraphNode sw4 = Node(6, GraphNodeType::Swizzle);
        sw4.swizzleMask = "yxzw";   // float4 source -> real swizzle
        g.nodes.push_back(sw4);
        g.links.push_back(Link(2, 0, 3, 0));   // uv -> noise.uv
        g.links.push_back(Link(4, 0, 3, 1));   // 20 -> noise.scale
        g.links.push_back(Link(3, 0, 7, 0));   // noise -> swizzle "xw"
        g.links.push_back(Link(3, 0, 5, 0));   // noise -> combine.r
        g.links.push_back(Link(7, 0, 5, 1));   // swizzle2 -> combine.g (.x adapt)
        g.links.push_back(Link(5, 0, 6, 0));   // combine -> swizzle "yxzw"
        g.links.push_back(Link(6, 0, 1, 0));

        const GraphCodegenResult gen = GenerateGraphSnippet(g, MaterialSurface::Fullscreen);
        REQUIRE(gen.Ok());
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());
        const MaterialBuildResult build = BuildMaterialShaderSource(
            *templateText, gen.snippet, "graph_library2", MaterialSurface::Fullscreen);
        REQUIRE(build.errors.empty());
        compileBoth(build.hlsl, "graph_library2.hlsl");
    }

    SECTION("fullscreen: a graph-generated vertex stage compiles")
    {
        MaterialGraph g;
        g.nodes.push_back(Node(1, GraphNodeType::Output));
        g.nodes.push_back(ParamNode(2, "Speed", MatParamType::Float,
                                    MatParamValue::MakeFloat(2.0f)));
        g.nodes.push_back(Node(3, GraphNodeType::Time));
        g.nodes.push_back(Node(4, GraphNodeType::Mul));
        g.nodes.push_back(Node(5, GraphNodeType::Sin));
        g.nodes.push_back(Node(6, GraphNodeType::VertexOutput));
        GraphNode c = Node(7, GraphNodeType::ConstColor);
        c.value[1] = 1.0f;
        c.value[3] = 1.0f;
        g.nodes.push_back(c);
        g.links.push_back(Link(3, 0, 4, 0));
        g.links.push_back(Link(2, 0, 4, 1));
        g.links.push_back(Link(4, 0, 5, 0));
        g.links.push_back(Link(5, 0, 6, 0));
        g.links.push_back(Link(7, 0, 1, 0));

        const GraphCodegenResult gen = GenerateGraphSnippet(g);
        REQUIRE(gen.Ok());
        REQUIRE_FALSE(gen.vertexSnippet.empty());
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());
        const MaterialBuildResult build = BuildMaterialShaderSource(
            *templateText, gen.snippet, "graph_vertex", MaterialSurface::Fullscreen,
            gen.vertexSnippet);
        REQUIRE(build.errors.empty());
        compileBoth(build.hlsl, "graph_vertex.hlsl");
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
