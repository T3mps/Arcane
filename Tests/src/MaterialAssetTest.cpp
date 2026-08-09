// .arcmat material assets (Slice 5): save/load round-trip (values typed by the
// snippet's own //@param decls), saved-value application onto an instance, and
// the AssetRegistry treating .arcmat as a NATIVE asset (embedded "id"). CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Project/AssetRegistry.hpp>

#include <DiagnosticStore.hpp>

#include <Json.hpp>   // nlohmann::json -- the vendored single header lives at
                       // ThirdParty/nlohmann/Json.hpp, NOT <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

using namespace Arcane;

namespace
{
    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path d =
            std::filesystem::temp_directory_path() / "arcane_material_asset_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }

    constexpr const char* kSnippet =
        "//@param color Tint = (1, 1, 1, 1)\n"
        "//@param float Speed = 1.0 [0..4]\n"
        "//@param texture Noise\n"
        "float4 shade(Varyings v) { return Tint; }\n";
}

TEST_CASE("MaterialAsset save/load round-trips snippet and typed params", "[material]")
{
    const auto dir = TempDir("roundtrip");
    const auto file = dir / "glow.arcmat";

    MaterialAssetData data;
    data.id = Guid::Generate();
    data.name = "Glow";
    data.snippet = kSnippet;
    data.vertexSnippet = "Varyings displace(Varyings v) { v.pos.x += 0.1; return v; }\n";
    const Guid noiseTex = Guid::Generate();
    data.params.emplace_back("Tint", MatParamValue::MakeColor(0.25f, 0.5f, 0.75f, 1.0f));
    data.params.emplace_back("Speed", MatParamValue::MakeFloat(2.5f));
    data.params.emplace_back("Noise", MatParamValue::MakeTexture(noiseTex));

    REQUIRE(SaveMaterialAsset(file, data));

    const auto loaded = LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    CHECK(loaded->id == data.id);
    CHECK(loaded->name == "Glow");
    CHECK(loaded->kind == "fullscreen");
    CHECK(loaded->snippet == kSnippet);
    CHECK(loaded->vertexSnippet == data.vertexSnippet);
    REQUIRE(loaded->params.size() == 3);

    // Apply onto an instance over the snippet's own template.
    MaterialSourceParse parsed = ParseMaterialSource(loaded->snippet);
    auto templ = std::make_shared<MaterialTemplate>(
        MaterialTemplate::Build("glow", 1, std::move(parsed.decls)));
    MaterialInstance inst(templ);
    CHECK(ApplyMaterialParams(*loaded, inst) == 3);

    MatParamValue v;
    REQUIRE(inst.GetParam("Tint", v));
    CHECK(v.f[2] == 0.75f);
    REQUIRE(inst.GetParam("Speed", v));
    CHECK(v.f[0] == 2.5f);
    REQUIRE(inst.GetParam("Noise", v));
    CHECK(v.tex == noiseTex);
}

TEST_CASE(".arcmat pass chains: round-trip, sprite/instance refusal", "[material]")
{
    const auto dir = TempDir("passes");

    MaterialAssetData data;
    data.id = Guid::Generate();
    data.name = "Chain";
    data.snippet = "float4 shade(Varyings v) { return float4(v.uv, 0.0, 1.0); }\n";
    data.passes.push_back({ "blur",
        "float4 shade(Varyings v) { return InputTexture.Sample(MaterialSampler, v.uv); }\n" });
    data.passes.push_back({ "composite",
        "float4 shade(Varyings v) { return InputTexture.Sample(MaterialSampler, v.uv) * 2.0; }\n" });

    SECTION("fullscreen base round-trips the pass list in order")
    {
        // DAG wiring + canvas layout ride each entry.
        data.passes[0].inputs = { 0 };
        data.passes[0].posX = 120.0f;
        data.passes[0].posY = 40.0f;
        data.passes[1].inputs = { 1, 0 };   // composite reads blur AND base

        const auto file = dir / "chain.arcmat";
        REQUIRE(SaveMaterialAsset(file, data));
        const auto loaded = LoadMaterialAsset(file);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->passes.size() == 2);
        CHECK(loaded->passes[0].name == "blur");
        CHECK(loaded->passes[1].name == "composite");
        CHECK(loaded->passes[0].snippet == data.passes[0].snippet);
        CHECK(loaded->passes[0].inputs == std::vector<std::uint32_t>{ 0 });
        CHECK(loaded->passes[1].inputs == std::vector<std::uint32_t>({ 1, 0 }));
        CHECK(loaded->passes[0].posX == 120.0f);
        CHECK(loaded->passes[0].posY == 40.0f);
    }

    SECTION("a graph-owned pass round-trips its graph and self-heals a snippet")
    {
        // Pass graph: PassInput(slot 0) -> Output.
        MaterialGraph pg;
        GraphNode out = { };
        out.id = 1;
        out.type = GraphNodeType::Output;
        GraphNode in = { };
        in.id = 2;
        in.type = GraphNodeType::PassInput;
        pg.nodes = { out, in };
        GraphLink l;
        l.fromNode = 2;
        l.toNode = 1;
        pg.links.push_back(l);
        pg.nextId = 3;

        MaterialAssetData chained = data;
        chained.passes[0].inputs = { 0 };
        chained.passes[0].graph = pg;
        chained.passes[0].snippet.clear();   // graph-only on disk

        const auto file = dir / "passgraph.arcmat";
        REQUIRE(SaveMaterialAsset(file, chained));
        const auto loaded = LoadMaterialAsset(file);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->passes.size() == 2);
        REQUIRE(loaded->passes[0].graph.has_value());
        CHECK(loaded->passes[0].graph->nodes.size() == 2);
        // Self-heal: the graph-only pass regenerated a working snippet with
        // the pass's OWN input count as context.
        CHECK(loaded->passes[0].snippet.find(
                  "InputTexture.Sample(MaterialSampler, v.uv)") != std::string::npos);
    }

    SECTION("an absent inputs key means NO inputs (no pre-DAG defaulting)")
    {
        const auto file = dir / "noinputs.arcmat";
        {
            std::ofstream out(file, std::ios::binary);
            out << R"({"id":"eeee5555-5555-4555-8555-555555555555",)"
                << R"("snippet":"float4 shade(Varyings v){return 0;}",)"
                << R"("passes":[{"name":"a","snippet":"s"}]})" << '\n';
        }
        const auto loaded = LoadMaterialAsset(file);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->passes.size() == 1);
        CHECK(loaded->passes[0].inputs.empty());
    }

    SECTION("absent passes key = single-pass (the pre-chain format)")
    {
        MaterialAssetData single = data;
        single.passes.clear();
        const auto file = dir / "single.arcmat";
        REQUIRE(SaveMaterialAsset(file, single));
        const auto loaded = LoadMaterialAsset(file);
        REQUIRE(loaded.has_value());
        CHECK(loaded->passes.empty());
    }

    SECTION("the sprite kind refuses passes at load")
    {
        MaterialAssetData sprite = data;
        sprite.kind = "sprite";
        const auto file = dir / "sprite.arcmat";
        REQUIRE(SaveMaterialAsset(file, sprite));
        const auto loaded = LoadMaterialAsset(file);
        REQUIRE(loaded.has_value());
        CHECK(loaded->passes.empty());
    }

    SECTION("instances refuse passes at load")
    {
        // Hand-authored shape: an instance file carrying a passes array.
        const auto file = dir / "inst.arcmat";
        {
            std::ofstream out(file, std::ios::binary);
            out << R"({"id":"aaaa1111-1111-4111-8111-111111111111",)"
                << R"("parent":"bbbb2222-2222-4222-8222-222222222222",)"
                << R"("passes":[{"name":"x","snippet":"float4 shade(Varyings v){return 0;}"}]})"
                << '\n';
        }
        const auto loaded = LoadMaterialAsset(file);
        REQUIRE(loaded.has_value());
        CHECK(loaded->IsInstance());
        CHECK(loaded->passes.empty());
    }

    SECTION("malformed pass entries drop; names default")
    {
        const auto file = dir / "malformed.arcmat";
        {
            std::ofstream out(file, std::ios::binary);
            out << R"({"id":"cccc3333-3333-4333-8333-333333333333",)"
                << R"("snippet":"float4 shade(Varyings v){return 0;}",)"
                << R"("passes":[{"snippet":"float4 shade(Varyings v){return 1;}"},)"
                << R"(17, {"name":"ok"}]})" << '\n';
        }
        const auto loaded = LoadMaterialAsset(file);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->passes.size() == 1);   // the two malformed entries drop
        CHECK(loaded->passes[0].name == "pass 1");
    }
}

TEST_CASE("MaterialAsset load drops stale or mismatched saved params", "[material]")
{
    const auto dir = TempDir("stale");
    const auto file = dir / "stale.arcmat";
    std::ofstream(file, std::ios::binary) << R"({
        "id": "c1e0c1de-3333-4444-8555-666677778888",
        "kind": "fullscreen",
        "name": "Stale",
        "snippet": "//@param float Speed = 1.0\nfloat4 shade(Varyings v) { return Speed; }\n",
        "params": { "Speed": [1, 2, 3, 4], "Gone": 5.0 }
    })";

    const auto loaded = LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());
    // "Speed" saved as a float4 (type changed) and "Gone" is undeclared: both drop.
    CHECK(loaded->params.empty());

    // Not-a-material JSON refuses to load.
    const auto bad = dir / "not_material.arcmat";
    std::ofstream(bad, std::ios::binary) << R"({ "id": "x", "whatever": 1 })";
    CHECK_FALSE(LoadMaterialAsset(bad).has_value());
}

TEST_CASE("Material instance assets round-trip the override chain", "[material]")
{
    const auto dir = TempDir("instances");

    // Base: snippet + saved values (Tint blue-ish, Speed 2).
    MaterialAssetData base;
    base.id = Guid::Generate();
    base.name = "Base";
    base.snippet = kSnippet;
    base.params.emplace_back("Tint", MatParamValue::MakeColor(0.0f, 0.0f, 1.0f, 1.0f));
    base.params.emplace_back("Speed", MatParamValue::MakeFloat(2.0f));
    REQUIRE(SaveMaterialAsset(dir / "base.arcmat", base));

    // Instance: parent + ONE sparse override (Speed 4). No snippet on disk.
    MaterialAssetData inst;
    inst.id = Guid::Generate();
    inst.parent = base.id;
    inst.name = "Fast";
    inst.params.emplace_back("Speed", MatParamValue::MakeFloat(4.0f));
    REQUIRE(SaveMaterialAsset(dir / "fast.arcmat", inst));

    const auto loadedBase = LoadMaterialAsset(dir / "base.arcmat");
    const auto loadedInst = LoadMaterialAsset(dir / "fast.arcmat");
    REQUIRE(loadedBase.has_value());
    REQUIRE(loadedInst.has_value());
    CHECK_FALSE(loadedBase->IsInstance());
    CHECK(loadedInst->IsInstance());
    CHECK(loadedInst->parent == base.id);
    CHECK(loadedInst->snippet.empty());   // instances never persist a snippet
    REQUIRE(loadedInst->params.size() == 1);

    // Layer the chain exactly like the editor does: template <- base values <-
    // instance overrides. Resolution: my override -> parent -> //@param default.
    MaterialSourceParse parsed = ParseMaterialSource(loadedBase->snippet);
    auto templ = std::make_shared<MaterialTemplate>(
        MaterialTemplate::Build("base", 1, std::move(parsed.decls)));
    auto baseLayer = std::make_shared<MaterialInstance>(templ);
    CHECK(ApplyMaterialParams(*loadedBase, *baseLayer) == 2);
    MaterialInstance child(baseLayer);
    CHECK(ApplyMaterialParams(*loadedInst, child) == 1);

    MatParamValue v;
    REQUIRE(child.GetParam("Speed", v));
    CHECK(v.f[0] == 4.0f);            // instance override wins
    REQUIRE(child.GetParam("Tint", v));
    CHECK(v.f[2] == 1.0f);            // base's saved value shows through
    REQUIRE(child.GetParam("Noise", v));
    CHECK(v.tex == Guid::Nil());      // untouched -> declaration default

    // Clearing the instance override re-exposes the base value.
    CHECK(child.ClearOverride("Speed"));
    REQUIRE(child.GetParam("Speed", v));
    CHECK(v.f[0] == 2.0f);
}

TEST_CASE("AssetRegistry scans .arcmat as a native asset", "[material][project]")
{
    const auto dir = TempDir("registry");

    // One with an embedded id, one without (gets minted + written back).
    MaterialAssetData withId;
    withId.id = Guid::Generate();
    withId.name = "A";
    withId.snippet = "float4 shade(Varyings v) { return 1; }\n";
    REQUIRE(SaveMaterialAsset(dir / "a.arcmat", withId));

    std::ofstream(dir / "b.arcmat", std::ios::binary)
        << R"({ "kind": "fullscreen", "name": "B", "snippet": "x", "params": {} })";

    // The pre-rename ".armat" spelling is DEAD (no shipped content): not
    // scanned, not an asset.
    std::ofstream(dir / "old.armat", std::ios::binary)
        << R"({ "id": "abab1212-1212-4212-8212-121212121212", "snippet": "x" })";

    AssetRegistry registry;
    CHECK(registry.ScanContent(dir, "game") == 2);
    const auto resolved = registry.Resolve(withId.id);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == "game://a.arcmat");

    // The minted id was written back into b.arcmat.
    const auto b = LoadMaterialAsset(dir / "b.arcmat");
    REQUIRE(b.has_value());
    CHECK(b->id.IsValid());
    CHECK(registry.Resolve(b->id).has_value());
}

TEST_CASE(".arcmat round-trips base scene inputs (the post-material shape)", "[material]")
{
    const auto dir = TempDir("baseinputs");
    const auto file = dir / "post.arcmat";

    MaterialAssetData data;
    data.id = Guid::Generate();
    data.name = "Post";
    data.snippet = "float4 shade(Varyings v)\n"
                   "{ return 1.0 - InputTexture.Sample(MaterialSampler, v.uv); }\n";
    data.baseInputs = { kSceneInput };
    data.chainSceneX = -180.0f;
    data.chainSceneY = 40.0f;
    REQUIRE(SaveMaterialAsset(file, data));

    const auto back = LoadMaterialAsset(file);
    REQUIRE(back.has_value());
    REQUIRE(back->baseInputs.size() == 1);
    CHECK(back->baseInputs[0] == kSceneInput);       // sentinel survives exactly
    CHECK(back->chainSceneX == -180.0f);
    CHECK(back->chainSceneY == 40.0f);
    CHECK(back->passes.empty());                     // base-only post material

    // Garbage in the sentinel half-space clamps to the sentinel.
    MaterialAssetData weird = data;
    weird.baseInputs = { 0x80000001u };
    REQUIRE(SaveMaterialAsset(file, weird));
    const auto clamped = LoadMaterialAsset(file);
    REQUIRE(clamped.has_value());
    REQUIRE(clamped->baseInputs.size() == 1);
    CHECK(clamped->baseInputs[0] == kSceneInput);
}

// A hand-authored graph-only file (no snippet keys) must self-heal BOTH stages:
// the loader regenerates the pixel snippet (MaterialAsset.cpp's documented
// self-heal) AND the vertex snippet -- GraphCodegenResult carries displace()
// for a wired Vertex Output, and dropping it would silently lose the vertex
// stage until the editor's next save regenerates both.
TEST_CASE("graph-only material self-heals pixel AND vertex snippets", "[material]")
{
    const auto dir = TempDir("graph_selfheal");
    {
        std::ofstream out(dir / "g.arcmat", std::ios::binary);
        out << R"({"id":"11112222333344445555666677778888","type":"material",)"
               R"("name":"g","kind":"sprite","graph":{"nextId":5,"nodes":[)"
               R"({"id":1,"type":"output","pos":[400.0,100.0]},)"
               R"({"id":2,"type":"const_color","pos":[100.0,100.0],"value":[1.0,0.5,0.25,1.0]},)"
               R"({"id":3,"type":"vertex_output","pos":[400.0,300.0]},)"
               R"({"id":4,"type":"const_float2","pos":[100.0,300.0],"value":[0.01,0.0]}],)"
               R"("links":[{"from":2,"fromPin":0,"to":1,"toPin":0},)"
               R"({"from":4,"fromPin":0,"to":3,"toPin":0}]}})";
    }
    const auto back = LoadMaterialAsset(dir / "g.arcmat");
    REQUIRE(back.has_value());
    REQUIRE(back->graph.has_value());
    CHECK_FALSE(back->snippet.empty());
    CHECK_FALSE(back->vertexSnippet.empty());
    CHECK(back->vertexSnippet.find("displace") != std::string::npos);
}

// Task 10: LoadMaterialAsset's dropped-entry diagnostics (material.param.dropped
// here; material.pass.malformed/material.graph.invalid are the sibling codes at
// the other drop sites in MaterialAsset.cpp). On-disk shape verified against
// SaveMaterialAsset/LoadMaterialAsset above: top-level "id"/"name"/"snippet",
// "params" is an object keyed by param name, each entry self-typed as
// {"type","value"} (MatParamValueToJson/MatParamValueFromJson).
TEST_CASE("A malformed material param is dropped WITH a diagnostic", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const auto dir = TempDir("bad_param");
    const auto file = dir / "bad.arcmat";

    // Hand-written .arcmat: valid envelope, one param whose "type" is nonsense.
    // The loader drops it -- today silently, after this task with a diagnostic.
    {
        nlohmann::json j;
        j["id"]      = Guid::Generate().ToString();
        j["name"]    = "Bad";
        j["snippet"] = "float4 shade(Varyings v) { return 1; }\n";
        j["params"]["Tint"]["type"]  = "not-a-real-type";
        j["params"]["Tint"]["value"] = 1.0f;
        std::ofstream out(file);
        REQUIRE(out.good());
        out << j.dump(2);
    }

    const auto loaded = LoadMaterialAsset(file);
    REQUIRE(loaded.has_value());   // the asset still loads; the param is dropped

    const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].code == "material.param.dropped");
    CHECK(rows[0].scope == Arcane::DiagScope::Material);
    CHECK(rows[0].locator.kind == Arcane::DiagLocator::Kind::Asset);

    store.UninstallEngineSink();
}
