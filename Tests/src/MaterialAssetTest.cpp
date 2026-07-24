// .armat material assets (Slice 5): save/load round-trip (values typed by the
// snippet's own //@param decls), saved-value application onto an instance, and
// the AssetRegistry treating .armat as a NATIVE asset (embedded "id"). CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Project/AssetRegistry.hpp>

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
    const auto file = dir / "glow.armat";

    MaterialAssetData data;
    data.id = Guid::Generate();
    data.name = "Glow";
    data.snippet = kSnippet;
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

TEST_CASE("MaterialAsset load drops stale or mismatched saved params", "[material]")
{
    const auto dir = TempDir("stale");
    const auto file = dir / "stale.armat";
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
    const auto bad = dir / "not_material.armat";
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
    REQUIRE(SaveMaterialAsset(dir / "base.armat", base));

    // Instance: parent + ONE sparse override (Speed 4). No snippet on disk.
    MaterialAssetData inst;
    inst.id = Guid::Generate();
    inst.parent = base.id;
    inst.name = "Fast";
    inst.params.emplace_back("Speed", MatParamValue::MakeFloat(4.0f));
    REQUIRE(SaveMaterialAsset(dir / "fast.armat", inst));

    const auto loadedBase = LoadMaterialAsset(dir / "base.armat");
    const auto loadedInst = LoadMaterialAsset(dir / "fast.armat");
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

TEST_CASE("AssetRegistry scans .armat as a native asset", "[material][project]")
{
    const auto dir = TempDir("registry");

    // One with an embedded id, one without (gets minted + written back).
    MaterialAssetData withId;
    withId.id = Guid::Generate();
    withId.name = "A";
    withId.snippet = "float4 shade(Varyings v) { return 1; }\n";
    REQUIRE(SaveMaterialAsset(dir / "a.armat", withId));

    std::ofstream(dir / "b.armat", std::ios::binary)
        << R"({ "kind": "fullscreen", "name": "B", "snippet": "x", "params": {} })";

    AssetRegistry registry;
    CHECK(registry.ScanContent(dir, "game") == 2);
    const auto resolved = registry.Resolve(withId.id);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == "game://a.armat");

    // The minted id was written back into b.armat.
    const auto b = LoadMaterialAsset(dir / "b.armat");
    REQUIRE(b.has_value());
    CHECK(b->id.IsValid());
    CHECK(registry.Resolve(b->id).has_value());
}
