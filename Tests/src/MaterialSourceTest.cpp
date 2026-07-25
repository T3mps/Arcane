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

TEST_CASE("GenerateMaterialBindings follows the sprite surface register map", "[material]")
{
    // Sprite surface (Slice 8): the batcher's push constants own b0 and the
    // sprite's own texture owns t0, so the material CB lands at b1, declared
    // textures at t1.., and the SAMPLER is the template's (not emitted here).
    MaterialSourceParse parsed = ParseMaterialSource(
        "//@param float Speed = 1.0\n"
        "//@param texture Noise\n");
    REQUIRE(parsed.errors.empty());
    const MaterialTemplate templ =
        MaterialTemplate::Build("sprite_binds", 1, std::move(parsed.decls));

    const std::string fullscreen = GenerateMaterialBindings(templ);
    CHECK(fullscreen.find("register(b0)") != std::string::npos);
    CHECK(fullscreen.find("register(t0)") != std::string::npos);
    CHECK(fullscreen.find("SamplerState MaterialSampler") != std::string::npos);

    const std::string sprite =
        GenerateMaterialBindings(templ, MaterialSurface::Sprite);
    CHECK(sprite.find("register(b1)") != std::string::npos);
    CHECK(sprite.find("register(t1)") != std::string::npos);
    CHECK(sprite.find("register(b0)") == std::string::npos);
    CHECK(sprite.find("register(t0)") == std::string::npos);
    CHECK(sprite.find("SamplerState") == std::string::npos);   // template owns s0

    CHECK(std::string(MaterialTemplateFile(MaterialSurface::Sprite)) ==
          "materials/sprite_material.hlsl");
    CHECK(MaterialSurfaceForKind("sprite") == MaterialSurface::Sprite);
    CHECK(MaterialSurfaceForKind("fullscreen") == MaterialSurface::Fullscreen);
    CHECK(MaterialSurfaceForKind("banana") == MaterialSurface::Fullscreen);
}

TEST_CASE("BuildMaterialShaderSource stitches a compilable dual-target SPRITE shader", "[shadercompile]")
{
    // The REAL sprite template through the provider seam: proves the register
    // maps (push constants b0 / material b1 / globals b2 / SpriteTexture t0 /
    // params t1..) coexist on BOTH targets, and that a snippet can use the
    // sprite texture + vertex color + params + Time together.
    ShaderSourceProvider provider;
    provider.AddRoot("shaders");
    const auto templateText = provider.Get("materials/sprite_material.hlsl");
    REQUIRE(templateText.has_value());

    const char* snippet =
        "//@param color Tint  = (1, 1, 1, 1)\n"
        "//@param float Speed = 1.0 [0..4]\n"
        "//@param texture Noise\n"
        "\n"
        "float4 shade(Varyings v)\n"
        "{\n"
        "    float w = 0.5 + 0.5 * sin(v.uv.x * 10.0 + Time * Speed);\n"
        "    float4 base = SpriteTexture.Sample(MaterialSampler, v.uv) * v.color;\n"
        "    return base * Tint * Noise.Sample(MaterialSampler, v.uv) * w;\n"
        "}\n";

    const MaterialBuildResult build = BuildMaterialShaderSource(
        *templateText, snippet, "sprite_example", MaterialSurface::Sprite);
    REQUIRE(build.errors.empty());
    CHECK(build.hlsl.find("%{") == std::string::npos);

    ShaderCompiler sc;
    REQUIRE(sc.Initialize(0.0));
    for (const char* entry : { kPsEntry, kVsEntry })
    {
        ShaderCompileRequest req;
        req.debugName = "sprite_example.hlsl";
        req.sourceUtf8 = build.hlsl;
        req.entry = entry;
        req.profile = entry == kPsEntry ? kPsProfile : kVsProfile;
        const ShaderCompileResult r = sc.CompileNow(req);
        for (const ShaderDiag& d : r.dxil.diags)
            INFO("dxil " << entry << ": " << d.line << ": " << d.message);
        for (const ShaderDiag& d : r.spirv.diags)
            INFO("spirv " << entry << ": " << d.line << ": " << d.message);
        CHECK(r.AllSucceeded());
    }
    sc.Shutdown();
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

TEST_CASE("The vertex stage stitches %{VERTEX_BODY} (passthrough by default)", "[material]")
{
    const char* templ = "%{MATERIAL_CBUFFER}\n%{VERTEX_BODY}\n%{MATERIAL_BODY}\n";
    const char* snippet = "float4 shade(Varyings v) { return 1; }\n";

    const MaterialBuildResult plain =
        BuildMaterialShaderSource(templ, snippet, "vs_default");
    REQUIRE(plain.errors.empty());
    CHECK(plain.hlsl.find("Varyings displace(Varyings v) { return v; }") !=
          std::string::npos);

    const char* wobble =
        "Varyings displace(Varyings v)\n"
        "{\n"
        "    v.pos.x += sin(Time) * 0.01;\n"
        "    return v;\n"
        "}\n";
    const MaterialBuildResult custom = BuildMaterialShaderSource(
        templ, snippet, "vs_custom", MaterialSurface::Fullscreen, wobble);
    REQUIRE(custom.errors.empty());
    CHECK(custom.hlsl.find(wobble) != std::string::npos);
    CHECK(custom.hlsl.find("Varyings displace(Varyings v) { return v; }") ==
          std::string::npos);
    // The vertex snippet participates in the compile-cache hash.
    CHECK(custom.templ.SourceHash() != plain.templ.SourceHash());

    // The hook's name is reserved for params.
    const MaterialSourceParse p = ParseMaterialSource("//@param float displace = 1\n");
    REQUIRE(p.errors.size() == 1);
    CHECK(p.errors[0].find("reserved") != std::string::npos);
}

TEST_CASE("Vertex-stage sources compile on both targets and surfaces", "[shadercompile]")
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

    const char* snippet =
        "//@param float Wobble = 0.02 [0..0.2]\n"
        "float4 shade(Varyings v) { return float4(v.uv, 0.0, 1.0); }\n";
    const char* displace =
        "Varyings displace(Varyings v)\n"
        "{\n"
        "    v.pos.x += sin(Time * 3.0 + v.pos.y * 8.0) * Wobble;\n"
        "    return v;\n"
        "}\n";

    SECTION("fullscreen: displace reads a param + Time")
    {
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());
        const MaterialBuildResult build = BuildMaterialShaderSource(
            *templateText, snippet, "vs_fullscreen", MaterialSurface::Fullscreen,
            displace);
        REQUIRE(build.errors.empty());
        compileBoth(build.hlsl, "vs_fullscreen.hlsl");
    }

    SECTION("sprite: the same displace body under the sprite register map")
    {
        const auto templateText = provider.Get("materials/sprite_material.hlsl");
        REQUIRE(templateText.has_value());
        const char* spriteSnippet =
            "//@param float Wobble = 0.02 [0..0.2]\n"
            "float4 shade(Varyings v)\n"
            "{ return SpriteTexture.Sample(MaterialSampler, v.uv) * v.color; }\n";
        const MaterialBuildResult build = BuildMaterialShaderSource(
            *templateText, spriteSnippet, "vs_sprite", MaterialSurface::Sprite,
            displace);
        REQUIRE(build.errors.empty());
        compileBoth(build.hlsl, "vs_sprite.hlsl");
    }

    SECTION("chains stitch the ONE vertex body into every pass")
    {
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());
        const std::uint32_t p1in[] = { 0 };
        const MaterialChainPassDesc passes[] = {
            { snippet, {} },
            { "float4 shade(Varyings v)\n"
              "{ return InputTexture.Sample(MaterialSampler, v.uv); }\n", p1in },
        };
        const MaterialChainBuildResult r = BuildMaterialChainSource(
            *templateText, passes, "vs_chain", displace);
        REQUIRE(r.Ok());
        for (const std::string& h : r.hlsl)
            CHECK(h.find("sin(Time * 3.0") != std::string::npos);
        for (std::size_t p = 0; p < r.hlsl.size(); ++p)
            compileBoth(r.hlsl[p], "vs_chain.hlsl");
    }

    sc.Shutdown();
}

TEST_CASE("BuildMaterialChainSource merges params and binds InputTexture", "[material]")
{
    const std::string_view pass0 =
        "//@param float Speed = 2 [0..4]\n"
        "//@param texture Noise\n"
        "float4 shade(Varyings v) { return Noise.Sample(MaterialSampler, v.uv) * Speed; }\n";
    const std::string_view pass1 =
        "//@param float Speed = 9\n"   // shared decl: the FIRST default wins
        "//@param float2 Dir = (1, 0)\n"
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv + Dir * Speed); }\n";
    const std::uint32_t p1in[] = { 0 };
    const MaterialChainPassDesc passes[] = { { pass0, {} }, { pass1, p1in } };

    const MaterialChainBuildResult r = BuildMaterialChainSource(
        "%{MATERIAL_CBUFFER}\n%{MATERIAL_BODY}\n", passes, "chain");
    REQUIRE(r.Ok());
    REQUIRE(r.hlsl.size() == 2);
    CHECK(r.chainInputSlots == 1);
    REQUIRE(r.passInputs.size() == 2);
    CHECK(r.passInputs[1] == std::vector<std::uint32_t>{ 0 });

    // ONE merged surface: Speed (default 2), Noise, Dir -- declaration order.
    REQUIRE(r.templ.Params().size() == 3);
    CHECK(r.templ.Params()[0].name == "Speed");
    CHECK(r.templ.Params()[0].def.f[0] == 2.0f);
    CHECK(r.templ.TextureCount() == 1);

    // EVERY pass carries the same binding block: union cbuffer + Noise t0 +
    // InputTexture after the material's own textures + the sampler.
    for (const std::string& h : r.hlsl)
    {
        CHECK(h.find("float Speed;") != std::string::npos);
        CHECK(h.find("float2 Dir;") != std::string::npos);
        CHECK(h.find("Texture2D Noise : register(t0);") != std::string::npos);
        CHECK(h.find("Texture2D InputTexture : register(t1);") != std::string::npos);
        CHECK(h.find("SamplerState MaterialSampler : register(s0);") != std::string::npos);
    }

    SECTION("conflicting types across passes are chain errors")
    {
        const std::uint32_t in0[] = { 0 };
        const MaterialChainPassDesc bad[] = {
            { "//@param float Speed = 1\nfloat4 shade(Varyings v) { return Speed; }\n", {} },
            { "//@param color Speed = (1, 1, 1, 1)\nfloat4 shade(Varyings v) { return Speed; }\n",
              in0 },
        };
        const MaterialChainBuildResult c = BuildMaterialChainSource(
            "%{MATERIAL_CBUFFER}\n%{MATERIAL_BODY}\n", bad, "conflict");
        REQUIRE_FALSE(c.Ok());
        REQUIRE_FALSE(c.errors.empty());
        CHECK(c.errors[0].find("Speed") != std::string::npos);
    }

    SECTION("a textureless chain still gets InputTexture t0 + the sampler")
    {
        const MaterialChainPassDesc lone[] = {
            { "float4 shade(Varyings v) { return float4(v.uv, 0.0, 1.0); }\n", {} },
        };
        const MaterialChainBuildResult c = BuildMaterialChainSource(
            "%{MATERIAL_CBUFFER}\n%{MATERIAL_BODY}\n", lone, "lone");
        REQUIRE(c.Ok());
        REQUIRE(c.hlsl.size() == 1);
        CHECK(c.hlsl[0].find("Texture2D InputTexture : register(t0);") != std::string::npos);
        CHECK(c.hlsl[0].find("SamplerState MaterialSampler : register(s0);") != std::string::npos);
    }

    SECTION("multi-input passes widen the uniform decl set (the DAG)")
    {
        // p2 reads BOTH p1 and p0 (bloom's composite shape): every pass's
        // source carries InputTexture + InputTexture1, slots t0/t1.
        const std::uint32_t p1in[] = { 0 };
        const std::uint32_t p2in[] = { 1, 0 };
        const MaterialChainPassDesc dag[] = {
            { "float4 shade(Varyings v) { return float4(v.uv, 0.0, 1.0); }\n", {} },
            { "float4 shade(Varyings v)\n"
              "{ return InputTexture.Sample(MaterialSampler, v.uv); }\n", p1in },
            { "float4 shade(Varyings v)\n"
              "{ return InputTexture.Sample(MaterialSampler, v.uv) +\n"
              "         InputTexture1.Sample(MaterialSampler, v.uv); }\n", p2in },
        };
        const MaterialChainBuildResult c = BuildMaterialChainSource(
            "%{MATERIAL_CBUFFER}\n%{MATERIAL_BODY}\n", dag, "dag");
        REQUIRE(c.Ok());
        CHECK(c.chainInputSlots == 2);
        CHECK(c.passInputs[2] == std::vector<std::uint32_t>({ 1, 0 }));
        for (const std::string& h : c.hlsl)
        {
            CHECK(h.find("Texture2D InputTexture : register(t0);") != std::string::npos);
            CHECK(h.find("Texture2D InputTexture1 : register(t1);") != std::string::npos);
        }
    }

    SECTION("DAG violations are chain errors")
    {
        const std::uint32_t self[] = { 1 };      // pass 1 reading itself
        const MaterialChainPassDesc selfRef[] = {
            { "float4 shade(Varyings v) { return 1; }\n", {} },
            { "float4 shade(Varyings v) { return 1; }\n", self },
        };
        CHECK_FALSE(BuildMaterialChainSource(
            "%{MATERIAL_CBUFFER}\n%{MATERIAL_BODY}\n", selfRef, "s").Ok());

        const std::uint32_t fwd[] = { 2 };       // pass 1 reading a later pass
        const MaterialChainPassDesc forward[] = {
            { "float4 shade(Varyings v) { return 1; }\n", {} },
            { "float4 shade(Varyings v) { return 1; }\n", fwd },
            { "float4 shade(Varyings v) { return 1; }\n", {} },
        };
        CHECK_FALSE(BuildMaterialChainSource(
            "%{MATERIAL_CBUFFER}\n%{MATERIAL_BODY}\n", forward, "f").Ok());

        const std::uint32_t baseIn[] = { 0 };    // the base reading anything
        const MaterialChainPassDesc basePass[] = {
            { "float4 shade(Varyings v) { return 1; }\n", baseIn },
        };
        CHECK_FALSE(BuildMaterialChainSource(
            "%{MATERIAL_CBUFFER}\n%{MATERIAL_BODY}\n", basePass, "b").Ok());

        const std::uint32_t five[] = { 0, 0, 0, 0, 0 };   // over kMaxPassInputs
        const MaterialChainPassDesc tooMany[] = {
            { "float4 shade(Varyings v) { return 1; }\n", {} },
            { "float4 shade(Varyings v) { return 1; }\n", five },
        };
        CHECK_FALSE(BuildMaterialChainSource(
            "%{MATERIAL_CBUFFER}\n%{MATERIAL_BODY}\n", tooMany, "m").Ok());
    }

    SECTION("the InputTexture names are reserved in snippets")
    {
        for (const char* line : { "//@param texture InputTexture\n",
                                  "//@param float InputTexture1 = 0\n" })
        {
            const MaterialSourceParse p = ParseMaterialSource(line);
            REQUIRE(p.errors.size() == 1);
            CHECK(p.errors[0].find("reserved") != std::string::npos);
        }
    }
}

TEST_CASE("Pass-chain sources compile on both targets", "[shadercompile]")
{
    ShaderSourceProvider provider;
    provider.AddRoot("shaders");
    const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
    REQUIRE(templateText.has_value());

    const std::string_view pass0 =
        "//@param float Speed = 1.0 [0..4]\n"
        "float4 shade(Varyings v)\n"
        "{\n"
        "    float w = 0.5 + 0.5 * sin(v.uv.x * 10.0 + Time * Speed);\n"
        "    return float4(w, w, w, 1.0);\n"
        "}\n";
    const std::string_view pass1 =
        "//@param color Tint = (1, 0.5, 0.25, 1)\n"
        "float4 shade(Varyings v)\n"
        "{\n"
        "    return InputTexture.Sample(MaterialSampler, v.uv) * Tint +\n"
        "           InputTexture1.Sample(MaterialSampler, v.uv) * 0.25;\n"
        "}\n";
    const std::uint32_t p1in[] = { 0, 0 };   // both slots fed by the base
    const MaterialChainPassDesc passes[] = { { pass0, {} }, { pass1, p1in } };

    const MaterialChainBuildResult r =
        BuildMaterialChainSource(*templateText, passes, "chain_compile");
    REQUIRE(r.Ok());
    REQUIRE(r.hlsl.size() == 2);
    CHECK(r.chainInputSlots == 2);

    ShaderCompiler sc;
    REQUIRE(sc.Initialize(0.0));
    for (std::size_t p = 0; p < r.hlsl.size(); ++p)
    {
        for (const char* entry : { kPsEntry, kVsEntry })
        {
            ShaderCompileRequest req;
            req.debugName = "chain_pass" + std::to_string(p) + ".hlsl";
            req.sourceUtf8 = r.hlsl[p];
            req.entry = entry;
            req.profile = entry == kPsEntry ? kPsProfile : kVsProfile;
            const ShaderCompileResult cr = sc.CompileNow(req);
            for (const ShaderDiag& d : cr.dxil.diags)
                INFO("dxil p" << p << ": " << d.line << ": " << d.message);
            for (const ShaderDiag& d : cr.spirv.diags)
                INFO("spirv p" << p << ": " << d.line << ": " << d.message);
            CHECK(cr.AllSucceeded());
        }
    }
    sc.Shutdown();
}
