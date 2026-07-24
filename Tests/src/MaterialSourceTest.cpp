// Material source pipeline (Slice 4): //@param parsing, cbuffer/bindings
// generation, %{slot} stitching, and the end-to-end snippet -> HLSL ->
// dual-target compile proof. CPU + dxc only, no GPU.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <string>

using namespace Arcane;

TEST_CASE("ParseMaterialSource reads the //@param grammar", "[material]")
{
    const char* snippet =
        "//@param color Tint = (1, 0.5, 0.25)\n"
        "//@param float Speed = 2.5 [0..4]\n"
        "//@param float2 Scale = (2, 3)\n"
        "//@param float4 Rect = (0, 1, 2, 3)\n"
        "//@param texture Noise\n"
        "//@param float Bare\n"
        "float4 shade(Varyings v) { return Tint; }\n";

    const MaterialSourceParse p = ParseMaterialSource(snippet);
    REQUIRE(p.errors.empty());
    REQUIRE(p.decls.size() == 6);
    REQUIRE(p.metas.size() == 6);

    CHECK(p.decls[0].type == MatParamType::Color);
    CHECK(p.decls[0].name == "Tint");
    CHECK(p.decls[0].def.f[1] == 0.5f);
    CHECK(p.decls[0].def.f[3] == 1.0f);   // 3-component color -> alpha 1

    CHECK(p.decls[1].type == MatParamType::Float);
    CHECK(p.decls[1].def.f[0] == 2.5f);
    CHECK(p.metas[1].sliderMin == 0.0f);
    CHECK(p.metas[1].sliderMax == 4.0f);

    CHECK(p.decls[2].def.f[1] == 3.0f);
    CHECK(p.decls[3].def.f[3] == 3.0f);

    CHECK(p.decls[4].type == MatParamType::Texture);
    CHECK(p.decls[4].def.tex == Guid::Nil());

    CHECK(p.decls[5].def.f[0] == 0.0f);   // no default -> zeroes
}

TEST_CASE("ParseMaterialSource reports bad lines and keeps going", "[material]")
{
    const char* snippet =
        "//@param nonsense X = 1\n"          // unknown type
        "//@param float 9bad = 1\n"          // invalid identifier
        "//@param float Time = 1\n"          // reserved by the template
        "//@param float Ok = 1\n"
        "//@param float Ok = 2\n"            // duplicate
        "//@param color C = (1, 2)\n"        // wrong tuple arity for color (needs 3-4)
        "//@param float R = 1 [4..0]\n"      // inverted range
        "//@param texture T = (1)\n";        // texture takes no default

    const MaterialSourceParse p = ParseMaterialSource(snippet);
    REQUIRE(p.decls.size() == 1);
    CHECK(p.decls[0].name == "Ok");
    REQUIRE(p.errors.size() == 7);
    CHECK(p.errors[0].find("line 1") != std::string::npos);
    CHECK(p.errors[2].find("reserved") != std::string::npos);
    CHECK(p.errors[3].find("duplicate") != std::string::npos);
}

TEST_CASE("GenerateMaterialBindings emits the b0 cbuffer + texture table", "[material]")
{
    const MaterialSourceParse p = ParseMaterialSource(
        "//@param color Tint = (1, 1, 1, 1)\n"
        "//@param float Speed = 1\n"
        "//@param texture Noise\n"
        "//@param texture Grain\n");
    REQUIRE(p.errors.empty());
    const auto t = MaterialTemplate::Build("m", 1, p.decls);

    const std::string text = GenerateMaterialBindings(t);
    CHECK(text.find("cbuffer Material : register(b0)") != std::string::npos);
    CHECK(text.find("float4 Tint;") != std::string::npos);
    CHECK(text.find("float Speed;") != std::string::npos);
    CHECK(text.find("Texture2D Noise : register(t0);") != std::string::npos);
    CHECK(text.find("Texture2D Grain : register(t1);") != std::string::npos);
    CHECK(text.find("SamplerState MaterialSampler : register(s0);") != std::string::npos);

    // No numeric params -> no cbuffer block; no textures -> no sampler.
    const auto texOnly = MaterialTemplate::Build("m", 1,
        ParseMaterialSource("//@param texture Noise\n").decls);
    const std::string texText = GenerateMaterialBindings(texOnly);
    CHECK(texText.find("cbuffer") == std::string::npos);
    CHECK(texText.find("Texture2D Noise") != std::string::npos);

    const auto empty = MaterialTemplate::Build("m", 1, {});
    CHECK(GenerateMaterialBindings(empty).empty());
}

TEST_CASE("StitchShaderTemplate substitutes slots and reports unresolved ones", "[material]")
{
    const std::pair<std::string_view, std::string_view> slots[] = {
        { "A", "alpha" },
        { "BODY", "float4 shade() { return 0; }" },
    };

    std::vector<std::string> unresolved;
    const std::string out = StitchShaderTemplate(
        "pre %{A} mid %{BODY} post %{MISSING} end", slots, &unresolved);

    CHECK(out == "pre alpha mid float4 shade() { return 0; } post %{MISSING} end");
    REQUIRE(unresolved.size() == 1);
    CHECK(unresolved[0] == "MISSING");
}

TEST_CASE("BuildMaterialShaderSource stitches a compilable dual-target shader", "[shadercompile]")
{
    // The REAL template file (copied beside the test exe by the postbuild),
    // through the same provider seam the editor uses.
    ShaderSourceProvider provider;
    provider.AddRoot("shaders");
    const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
    REQUIRE(templateText.has_value());

    const char* snippet =
        "//@param color Tint  = (1, 1, 1, 1)\n"
        "//@param float Speed = 1.0 [0..4]\n"
        "//@param texture Noise\n"
        "\n"
        "float4 shade(Varyings v)\n"
        "{\n"
        "    float w = 0.5 + 0.5 * sin(v.uv.x * 10.0 + Time * Speed);\n"
        "    return Tint * Noise.Sample(MaterialSampler, v.uv) * w;\n"
        "}\n";

    const MaterialBuildResult build =
        BuildMaterialShaderSource(*templateText, snippet, "spec_example");
    REQUIRE(build.errors.empty());
    CHECK(build.templ.CbSize() == 32);        // float4 Tint + float Speed
    CHECK(build.templ.TextureCount() == 1);
    CHECK(build.hlsl.find("%{") == std::string::npos);
    CHECK(build.hlsl.find(snippet) != std::string::npos);   // body rides verbatim

    ShaderCompiler sc;
    REQUIRE(sc.Initialize(0.0));

    ShaderCompileRequest ps;
    ps.debugName = "spec_example.hlsl";
    ps.sourceUtf8 = build.hlsl;
    ps.entry = kPsEntry;
    ps.profile = kPsProfile;
    const ShaderCompileResult psResult = sc.CompileNow(ps);
    for (const ShaderDiag& d : psResult.dxil.diags)
        INFO("dxil: " << d.file << ":" << d.line << ": " << d.message);
    for (const ShaderDiag& d : psResult.spirv.diags)
        INFO("spirv: " << d.file << ":" << d.line << ": " << d.message);
    CHECK(psResult.AllSucceeded());

    ShaderCompileRequest vs = ps;
    vs.entry = kVsEntry;
    vs.profile = kVsProfile;
    CHECK(sc.CompileNow(vs).AllSucceeded());

    sc.Shutdown();
}
