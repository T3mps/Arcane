// Asset-manager arc (ABI v22): Task 1 added MaterialSurfaceFor -- the material
// SUBKIND for a Guid, resolved through the installed AssetResolver and, for
// an instance, walked through its parent chain to the base's own "kind".
// Task 2 (below the MaterialSurfaceFor cases) adds ListAssetReferences -- the
// OUTGOING reference graph for a Guid: sprite/material/mesh extractors, plus
// the leaf-format and unresolvable-guid edge cases. Both are modeled on
// AssetsTest.cpp's Assets::Create() + SetAssetResolver pattern (real temp-dir
// files, a resolver lambda mapping test guids to them).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Material/MaterialSource.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path WriteFile(const fs::path& dir, const char* name, const std::string& text)
    {
        fs::path p = dir / name;
        std::ofstream(p) << text;
        return p;
    }

    // Task 2: field-wise membership check for a ListAssetReferences result --
    // AssetRef carries no operator== of its own (nothing else needs one yet;
    // adding one purely for test convenience would be scope creep), so the
    // test compares fields directly instead.
    bool ContainsRef(const std::vector<Arcane::AssetRef>& refs, const Arcane::Guid& target,
                      Arcane::AssetRefKind kind)
    {
        for (const auto& r : refs)
            if (r.target == target && r.kind == kind)
                return true;
        return false;
    }
}

TEST_CASE("MaterialSurfaceFor reads the kind string; instances resolve through parent", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_matsurface_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const auto base = WriteFile(dir, "base.arcmat",
        R"({"id":"7e5a0001-0001-4001-8001-000000000001","kind":"sprite","name":"B","params":{},"snippet":"","type":"material"})");
    const auto inst = WriteFile(dir, "inst.arcmat",
        R"({"id":"7e5a0001-0001-4001-8001-000000000002","parent":"7e5a0001-0001-4001-8001-000000000001","params":{},"type":"material"})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        const std::string g = id.Value().ToString();
        if (g == "7e5a0001-0001-4001-8001-000000000001") return base;
        if (g == "7e5a0001-0001-4001-8001-000000000002") return inst;
        return std::nullopt;
    });

    const auto baseIdOpt = Arcane::Guid::FromString("7e5a0001-0001-4001-8001-000000000001");
    const auto instIdOpt = Arcane::Guid::FromString("7e5a0001-0001-4001-8001-000000000002");
    REQUIRE(baseIdOpt.has_value());
    REQUIRE(instIdOpt.has_value());

    REQUIRE(assets->MaterialSurfaceFor(*baseIdOpt) == Arcane::MaterialSurface::Sprite);
    REQUIRE(assets->MaterialSurfaceFor(*instIdOpt) == Arcane::MaterialSurface::Sprite); // via parent
    REQUIRE_FALSE(assets->MaterialSurfaceFor(Arcane::Guid::Generate()).has_value()); // unresolvable

    fs::remove_all(dir, ec);
}

TEST_CASE("MaterialSurfaceFor refuses a cyclic parent chain without hanging", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_matsurface_cycle_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    // Two instances parenting each other -- no base ever terminates the walk.
    const auto a = WriteFile(dir, "a.arcmat",
        R"({"id":"7e5a0002-0001-4001-8001-000000000001","parent":"7e5a0002-0001-4001-8001-000000000002","type":"material"})");
    const auto b = WriteFile(dir, "b.arcmat",
        R"({"id":"7e5a0002-0001-4001-8001-000000000002","parent":"7e5a0002-0001-4001-8001-000000000001","type":"material"})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        const std::string g = id.Value().ToString();
        if (g == "7e5a0002-0001-4001-8001-000000000001") return a;
        if (g == "7e5a0002-0001-4001-8001-000000000002") return b;
        return std::nullopt;
    });

    const auto aId = Arcane::Guid::FromString("7e5a0002-0001-4001-8001-000000000001");
    REQUIRE(aId.has_value());
    REQUIRE_FALSE(assets->MaterialSurfaceFor(*aId).has_value()); // bounded depth: no hang

    fs::remove_all(dir, ec);
}

TEST_CASE("MaterialSurfaceFor returns nullopt for a non-material asset", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_matsurface_nonmaterial_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    // A ".png guid": the resolved file is not JSON at all, so GetJson fails to
    // parse it and MaterialSurfaceFor bails out at the "!json" branch.
    const auto png = WriteFile(dir, "marker.png", "not a real png, just bytes");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        const std::string g = id.Value().ToString();
        if (g == "7e5a0003-0001-4001-8001-000000000001") return png;
        return std::nullopt;
    });

    const auto pngId = Arcane::Guid::FromString("7e5a0003-0001-4001-8001-000000000001");
    REQUIRE(pngId.has_value());
    REQUIRE_FALSE(assets->MaterialSurfaceFor(*pngId).has_value());

    fs::remove_all(dir, ec);
}

TEST_CASE("MaterialSurfaceFor gates the fullscreen default on the material type discriminator", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_matsurface_typegate_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    // Valid JSON, neither "kind" nor "parent" -- but NOT a material ("type" is
    // something else). Must NOT default to Fullscreen the way a kind-less/
    // parent-less MATERIAL file would (MaterialAsset.cpp's own load default).
    const auto other = WriteFile(dir, "other.json",
        R"({"id":"7e5a0004-0001-4001-8001-000000000001","type":"texture"})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        const std::string g = id.Value().ToString();
        if (g == "7e5a0004-0001-4001-8001-000000000001") return other;
        return std::nullopt;
    });

    const auto otherId = Arcane::Guid::FromString("7e5a0004-0001-4001-8001-000000000001");
    REQUIRE(otherId.has_value());
    REQUIRE_FALSE(assets->MaterialSurfaceFor(*otherId).has_value());

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Task 2: ListAssetReferences -- sprite / material / mesh extractors
// ---------------------------------------------------------------------------

TEST_CASE("ListAssetReferences reads a plain sprite's texture as DerivesFrom", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_listrefs_sprite_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const auto sprite = WriteFile(dir, "plain.arcsprite",
        R"({"id":"7e5b0001-0001-4001-8001-000000000001","type":"sprite","name":"P",)"
        R"("texture":"7e5b0001-0001-4001-8001-000000000002","ppu":64.0})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value().ToString() == "7e5b0001-0001-4001-8001-000000000001") return sprite;
        return std::nullopt;
    });

    const auto spriteId = Arcane::Guid::FromString("7e5b0001-0001-4001-8001-000000000001");
    const auto texId    = Arcane::Guid::FromString("7e5b0001-0001-4001-8001-000000000002");
    REQUIRE(spriteId.has_value());
    REQUIRE(texId.has_value());

    const auto refs = assets->ListAssetReferences(*spriteId);
    REQUIRE(refs.has_value());
    REQUIRE(refs->size() == 1);
    CHECK((*refs)[0].target == *texId);
    CHECK((*refs)[0].kind == Arcane::AssetRefKind::DerivesFrom);

    fs::remove_all(dir, ec);
}

TEST_CASE("ListAssetReferences reads a sliced sprite's texture as References", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_listrefs_sprite_sliced_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    // SaveSpriteAsset (SpriteAsset.cpp) writes "sourceSize" only when it
    // differs from the (0,0) "whole texture" default -- a non-zero pair is
    // exactly what a hand-authored sliced sprite's file carries.
    const auto sprite = WriteFile(dir, "sliced.arcsprite",
        R"({"id":"7e5b0002-0001-4001-8001-000000000001","type":"sprite","name":"S",)"
        R"("texture":"7e5b0002-0001-4001-8001-000000000002","ppu":64.0,)"
        R"("sourcePos":[16.0,16.0],"sourceSize":[32.0,32.0]})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value().ToString() == "7e5b0002-0001-4001-8001-000000000001") return sprite;
        return std::nullopt;
    });

    const auto spriteId = Arcane::Guid::FromString("7e5b0002-0001-4001-8001-000000000001");
    const auto texId    = Arcane::Guid::FromString("7e5b0002-0001-4001-8001-000000000002");
    REQUIRE(spriteId.has_value());
    REQUIRE(texId.has_value());

    const auto refs = assets->ListAssetReferences(*spriteId);
    REQUIRE(refs.has_value());
    REQUIRE(refs->size() == 1);
    CHECK((*refs)[0].target == *texId);
    CHECK((*refs)[0].kind == Arcane::AssetRefKind::References);

    fs::remove_all(dir, ec);
}

TEST_CASE("ListAssetReferences reads a base material's texture params as References", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_listrefs_material_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const auto mat = WriteFile(dir, "mat.arcmat",
        R"({"id":"7e5b0003-0001-4001-8001-000000000001","type":"material","kind":"fullscreen",)"
        R"("name":"M","snippet":"","params":{)"
        R"("albedo":{"type":"texture","value":"7e5b0003-0001-4001-8001-000000000002"},)"
        R"("normal":{"type":"texture","value":"7e5b0003-0001-4001-8001-000000000003"},)"
        R"("amount":{"type":"float","value":0.5}}})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value().ToString() == "7e5b0003-0001-4001-8001-000000000001") return mat;
        return std::nullopt;
    });

    const auto matId    = Arcane::Guid::FromString("7e5b0003-0001-4001-8001-000000000001");
    const auto albedoId = Arcane::Guid::FromString("7e5b0003-0001-4001-8001-000000000002");
    const auto normalId = Arcane::Guid::FromString("7e5b0003-0001-4001-8001-000000000003");
    REQUIRE(matId.has_value());
    REQUIRE(albedoId.has_value());
    REQUIRE(normalId.has_value());

    const auto refs = assets->ListAssetReferences(*matId);
    REQUIRE(refs.has_value());
    REQUIRE(refs->size() == 2);   // the float param contributes no ref
    CHECK(ContainsRef(*refs, *albedoId, Arcane::AssetRefKind::References));
    CHECK(ContainsRef(*refs, *normalId, Arcane::AssetRefKind::References));

    fs::remove_all(dir, ec);
}

TEST_CASE("ListAssetReferences reads an instance's parent as DerivesFrom plus its own texture overrides as References", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_listrefs_material_instance_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const auto inst = WriteFile(dir, "inst.arcmat",
        R"({"id":"7e5b0004-0001-4001-8001-000000000001","type":"material",)"
        R"("parent":"7e5b0004-0001-4001-8001-000000000002","params":{)"
        R"("albedo":{"type":"texture","value":"7e5b0004-0001-4001-8001-000000000003"}}})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value().ToString() == "7e5b0004-0001-4001-8001-000000000001") return inst;
        return std::nullopt;
    });

    const auto instId   = Arcane::Guid::FromString("7e5b0004-0001-4001-8001-000000000001");
    const auto parentId = Arcane::Guid::FromString("7e5b0004-0001-4001-8001-000000000002");
    const auto texId    = Arcane::Guid::FromString("7e5b0004-0001-4001-8001-000000000003");
    REQUIRE(instId.has_value());
    REQUIRE(parentId.has_value());
    REQUIRE(texId.has_value());

    const auto refs = assets->ListAssetReferences(*instId);
    REQUIRE(refs.has_value());
    REQUIRE(refs->size() == 2);
    CHECK(ContainsRef(*refs, *parentId, Arcane::AssetRefKind::DerivesFrom));
    CHECK(ContainsRef(*refs, *texId, Arcane::AssetRefKind::References));

    fs::remove_all(dir, ec);
}

TEST_CASE("ListAssetReferences reads a mesh's material as References; a nil material yields an empty list", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_listrefs_mesh_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const auto mesh = WriteFile(dir, "mesh.arcmesh",
        R"({"id":"7e5b0005-0001-4001-8001-000000000001","type":"mesh","name":"Mesh",)"
        R"("source":"cube","rings":16,"segments":32,"subdivisions":1,)"
        R"("capsuleLengthRatio":2.0,"material":"7e5b0005-0001-4001-8001-000000000002"})");
    // SaveMeshAsset writes EVERY field unconditionally, including a nil
    // material as the literal nil-guid string -- exactly this shape.
    const auto meshNil = WriteFile(dir, "meshNil.arcmesh",
        R"({"id":"7e5b0005-0001-4001-8001-000000000003","type":"mesh","name":"MeshNil",)"
        R"("source":"cube","rings":16,"segments":32,"subdivisions":1,)"
        R"("capsuleLengthRatio":2.0,"material":"00000000-0000-0000-0000-000000000000"})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        const std::string g = id.Value().ToString();
        if (g == "7e5b0005-0001-4001-8001-000000000001") return mesh;
        if (g == "7e5b0005-0001-4001-8001-000000000003") return meshNil;
        return std::nullopt;
    });

    const auto meshId     = Arcane::Guid::FromString("7e5b0005-0001-4001-8001-000000000001");
    const auto materialId = Arcane::Guid::FromString("7e5b0005-0001-4001-8001-000000000002");
    const auto meshNilId  = Arcane::Guid::FromString("7e5b0005-0001-4001-8001-000000000003");
    REQUIRE(meshId.has_value());
    REQUIRE(materialId.has_value());
    REQUIRE(meshNilId.has_value());

    const auto refs = assets->ListAssetReferences(*meshId);
    REQUIRE(refs.has_value());
    REQUIRE(refs->size() == 1);
    CHECK((*refs)[0].target == *materialId);
    CHECK((*refs)[0].kind == Arcane::AssetRefKind::References);

    const auto nilRefs = assets->ListAssetReferences(*meshNilId);
    REQUIRE(nilRefs.has_value());
    CHECK(nilRefs->empty());

    fs::remove_all(dir, ec);
}

TEST_CASE("ListAssetReferences returns an empty list, not nullopt, for a leaf .png guid", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_listrefs_leaf_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const auto png = WriteFile(dir, "marker.png", "not a real png, just bytes");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value().ToString() == "7e5b0006-0001-4001-8001-000000000001") return png;
        return std::nullopt;
    });

    const auto pngId = Arcane::Guid::FromString("7e5b0006-0001-4001-8001-000000000001");
    REQUIRE(pngId.has_value());

    const auto refs = assets->ListAssetReferences(*pngId);
    REQUIRE(refs.has_value());
    CHECK(refs->empty());

    fs::remove_all(dir, ec);
}

TEST_CASE("ListAssetReferences returns nullopt for an unresolvable guid", "[assets]")
{
    auto assets = Arcane::Assets::Create();   // no resolver installed at all
    CHECK_FALSE(assets->ListAssetReferences(Arcane::Guid::Generate()).has_value());
}

// ---------------------------------------------------------------------------
// Task 3: scene structural reference scan + format coverage
// ---------------------------------------------------------------------------

TEST_CASE("ListAssetReferences scans a scene: reports a real ref, skips identity/nil/"
          "unresolvable, and dedups repeated mentions", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_listrefs_scene_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    // {"hi":u64,"lo":u64} is EXACTLY Components.hpp's ASTRA_REFLECT_TYPE(Guid)
    // wire shape -- verified against the real ReferenceProject/Content/
    // scenes/main.arcscene fixture ("material": {"hi": <u64>, "lo": <u64>}).
    // Two entities:
    //   entity 0: an identity "id" (must NOT be reported even though it IS
    //     resolvable -- proves the exclusion is the KEY-NAME rule, not the
    //     resolvability filter), a resolvable material named from TWO
    //     different component fields (dedup), and a nil sprite ({0,0}).
    //   entity 1: an identity "guid" (the OTHER spelling the rule accepts)
    //     and a nonzero, resolver-unknown mesh guid (the resolvability
    //     filter must drop it).
    const auto scene = WriteFile(dir, "scene.arcscene", R"({
        "version": 3,
        "entities": [
            {
                "components": {
                    "Arcane::Identity": {
                        "id": { "hi": 111111111111111111, "lo": 222222222222222222 },
                        "name": "A"
                    },
                    "Arcane::SpriteRenderer": {
                        "material": { "hi": 333333333333333333, "lo": 444444444444444444 },
                        "sprite": { "hi": 0, "lo": 0 }
                    },
                    "Arcane::PostProcess": {
                        "material": { "hi": 333333333333333333, "lo": 444444444444444444 }
                    }
                },
                "parent": -1
            },
            {
                "components": {
                    "Arcane::Identity": {
                        "guid": { "hi": 111111111111111111, "lo": 222222222222222222 },
                        "name": "B"
                    },
                    "Arcane::MeshRenderer": {
                        "mesh": { "hi": 555555555555555555, "lo": 666666666666666666 }
                    }
                },
                "parent": -1
            }
        ]
    })");

    const auto sceneId = Arcane::Guid::FromString("7e5c0001-0001-4001-8001-000000000001");
    REQUIRE(sceneId.has_value());

    // Reconstructed the SAME way GuidFromHiLo does (Guid{hi, lo} -- direct
    // field construction, no packing) so the test and the production code
    // agree on what these numbers mean.
    const Arcane::Guid identityGuid{ 111111111111111111ull, 222222222222222222ull };
    const Arcane::Guid materialGuid{ 333333333333333333ull, 444444444444444444ull };
    const Arcane::Guid unresolvableGuid{ 555555555555555555ull, 666666666666666666ull };

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value() == *sceneId)      return scene;
        if (id.Value() == identityGuid)  return fs::path("identity-marker");   // resolvable, but excluded BY NAME
        if (id.Value() == materialGuid)  return fs::path("material-marker");
        return std::nullopt;   // unresolvableGuid stays deliberately unresolved
    });

    const auto refs = assets->ListAssetReferences(*sceneId);
    REQUIRE(refs.has_value());
    REQUIRE(refs->size() == 1);   // materialGuid only -- deduped from its two mentions
    CHECK((*refs)[0].target == materialGuid);
    CHECK((*refs)[0].kind == Arcane::AssetRefKind::References);
    CHECK_FALSE(ContainsRef(*refs, identityGuid, Arcane::AssetRefKind::References));
    CHECK_FALSE(ContainsRef(*refs, unresolvableGuid, Arcane::AssetRefKind::References));

    fs::remove_all(dir, ec);
}

TEST_CASE("ListAssetReferences never returns nullopt for any AssetKindOf-recognized "
          "extension (format coverage)", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_listrefs_coverage_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    // Every extension ArcaneEditor's AssetKindOf (Panels/AssetPanelModel.hpp:75)
    // explicitly classifies -- hardcoded here, WITH this comment pointing
    // back at the real switch, per the brief's documented fallback (AssetKindOf
    // switches on a runtime string_view; there is no case list to introspect
    // and walk). This is the spec s3.2 guarantee pinned as data: a future
    // extension AssetKindOf grows without a matching row here will still
    // classify correctly upstream, but ListAssetReferences's own switch
    // (Assets.cpp) has no entry for it either until someone adds one -- and
    // the day they add a row HERE for it is the day this test actually
    // exercises the gap.
    //
    // Content: formats ListAssetReferences routes through GetJson (must
    // parse) get a minimal "{}"; formats it short-circuits on extension
    // alone (Assets.cpp's kLeaf/kOpaque tables) get arbitrary non-empty
    // bytes, matching the existing leaf-.png test's own fixture above.
    struct Case { const char* ext; const char* content; };
    static constexpr Case kCases[] = {
        { ".arcmat",    "{}" },
        { ".arcscene",  "{}" },
        { ".arcsprite", "{}" },
        { ".arcmesh",   "{}" },
        { ".arcdiag",   "not a real diag, just bytes" },
        { ".png",       "not a real png, just bytes" },
        { ".jpg",       "not a real jpg, just bytes" },
        { ".jpeg",      "not a real jpeg, just bytes" },
        { ".tga",       "not a real tga, just bytes" },
        { ".bmp",       "not a real bmp, just bytes" },
        { ".hdr",       "not a real hdr, just bytes" },
        { ".wav",       "not a real wav, just bytes" },
        { ".ogg",       "not a real ogg, just bytes" },
        { ".mp3",       "not a real mp3, just bytes" },
        { ".flac",      "not a real flac, just bytes" },
        { ".ttf",       "not a real ttf, just bytes" },
        { ".otf",       "not a real otf, just bytes" },
        { ".json",      "{}" },
    };

    std::vector<std::pair<Arcane::Guid, fs::path>> mapping;
    int n = 0;
    for (const Case& c : kCases)
    {
        const std::string name = "min" + std::to_string(++n) + c.ext;
        mapping.emplace_back(Arcane::Guid::Generate(), WriteFile(dir, name.c_str(), c.content));
    }

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        for (const auto& [guid, path] : mapping)
            if (id.Value() == guid) return path;
        return std::nullopt;
    });

    for (size_t i = 0; i < mapping.size(); ++i)
    {
        INFO("extension: " << kCases[i].ext);
        const auto refs = assets->ListAssetReferences(mapping[i].first);
        REQUIRE(refs.has_value());   // never nullopt for a readable file (spec s3.2)
    }

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Final fix wave (C1): the two panel queries are PARSE-ON-CALL.
//
// Spec s3 pins them "parse-on-call, no engine-side cache -- the editor's index
// and model are the caches". They used to route through this facade's cached
// JSON loader, which keys on the canonical path, never consults the mtime, and
// memoizes failures for the process lifetime (nothing evicts it). Every case
// below FAILS against that older path -- it answers with the FIRST parse
// forever -- and passes only once both queries read the file on each call.
//
// Each case rewrites a file IN PLACE and re-asks through the SAME Assets
// instance and the SAME resolver: a fresh instance would prove nothing,
// because a fresh cache is empty by construction.
// ---------------------------------------------------------------------------

TEST_CASE("MaterialSurfaceFor re-parses a rewritten material (parse-on-call)", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_matsurface_freshness_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    const char* kGuid = "7e5a0009-0001-4001-8001-000000000001";
    const auto mat = WriteFile(dir, "shifting.arcmat",
        R"({"id":"7e5a0009-0001-4001-8001-000000000001","kind":"sprite","name":"S","params":{},"type":"material"})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        return id.Value().ToString() == kGuid ? std::optional<fs::path>(mat) : std::nullopt;
    });

    const auto idOpt = Arcane::Guid::FromString(kGuid);
    REQUIRE(idOpt.has_value());

    REQUIRE(assets->MaterialSurfaceFor(*idOpt) == Arcane::MaterialSurface::Sprite);

    // The user edits the material's kind and saves; the panel model marks the
    // guid dirty and re-asks. It must get the NEW answer, not the first one --
    // otherwise the subkind pill is frozen for the whole session.
    WriteFile(dir, "shifting.arcmat",
        R"({"id":"7e5a0009-0001-4001-8001-000000000001","kind":"mesh","name":"S","params":{},"type":"material"})");
    REQUIRE(assets->MaterialSurfaceFor(*idOpt) == Arcane::MaterialSurface::Mesh);

    // ...and again, to "post": a third distinct answer proves the second was
    // not merely a one-shot cache miss.
    WriteFile(dir, "shifting.arcmat",
        R"({"id":"7e5a0009-0001-4001-8001-000000000001","kind":"fullscreen","name":"S","params":{},"type":"material"})");
    REQUIRE(assets->MaterialSurfaceFor(*idOpt) == Arcane::MaterialSurface::Fullscreen);

    fs::remove_all(dir, ec);
}

TEST_CASE("A parse failure is not memoized -- the next call retries (spec s3.2)", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_parse_retry_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    // Caught MID-SAVE: a truncated file that parses as nothing. Both queries
    // must say nullopt now and answer for real once the save completes -- s3.2's
    // "keeps the last-known refs and retries on the next change event" is
    // unimplementable above a facade that latches the failure permanently.
    const char* kMatGuid = "7e5a000a-0001-4001-8001-000000000001";
    const char* kTexGuid = "7e5a000a-0001-4001-8001-000000000002";
    const auto mat = WriteFile(dir, "midsave.arcmat", R"({"id":"7e5a000a-0001-4001-80)");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        return id.Value().ToString() == kMatGuid ? std::optional<fs::path>(mat) : std::nullopt;
    });

    const auto matId = Arcane::Guid::FromString(kMatGuid);
    const auto texId = Arcane::Guid::FromString(kTexGuid);
    REQUIRE(matId.has_value());
    REQUIRE(texId.has_value());

    REQUIRE_FALSE(assets->MaterialSurfaceFor(*matId).has_value());    // unreadable -> nullopt
    REQUIRE_FALSE(assets->ListAssetReferences(*matId).has_value());   // could-not-read -> nullopt

    // The save completes.
    WriteFile(dir, "midsave.arcmat",
        std::string(R"({"id":")") + kMatGuid + R"(","kind":"mesh","name":"M","type":"material",)" +
        R"("params":{"BaseTexture":{"type":"texture","value":")" + kTexGuid + R"("}}})");

    REQUIRE(assets->MaterialSurfaceFor(*matId) == Arcane::MaterialSurface::Mesh);
    const auto refs = assets->ListAssetReferences(*matId);
    REQUIRE(refs.has_value());
    REQUIRE(ContainsRef(*refs, *texId, Arcane::AssetRefKind::References));

    fs::remove_all(dir, ec);
}

TEST_CASE("ListAssetReferences re-parses a rewritten sprite (parse-on-call)", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_refs_freshness_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    const char* kSpriteGuid = "7e5a000b-0001-4001-8001-000000000001";
    const char* kTexGuid    = "7e5a000b-0001-4001-8001-000000000002";
    const auto sprite = WriteFile(dir, "shifting.arcsprite",
        std::string(R"({"id":")") + kSpriteGuid + R"(","texture":")" + kTexGuid + R"("})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        return id.Value().ToString() == kSpriteGuid ? std::optional<fs::path>(sprite)
                                                    : std::nullopt;
    });

    const auto spriteId = Arcane::Guid::FromString(kSpriteGuid);
    const auto texId    = Arcane::Guid::FromString(kTexGuid);
    REQUIRE(spriteId.has_value());
    REQUIRE(texId.has_value());

    // Whole-texture wrap: the DerivesFrom edge Browse folds on.
    {
        const auto refs = assets->ListAssetReferences(*spriteId);
        REQUIRE(refs.has_value());
        REQUIRE(ContainsRef(*refs, *texId, Arcane::AssetRefKind::DerivesFrom));
    }

    // The sprite gains a sub-rect (the "sliced" pill's own datum). The SAME
    // guid must now report References, or the pill and the fold stay frozen at
    // whatever they were the first time anyone asked.
    WriteFile(dir, "shifting.arcsprite",
        std::string(R"({"id":")") + kSpriteGuid + R"(","texture":")" + kTexGuid +
        R"(","sourceSize":[32,32]})");   // non-zero sourceSize IS the sliced discriminator
    {
        const auto refs = assets->ListAssetReferences(*spriteId);
        REQUIRE(refs.has_value());
        REQUIRE(ContainsRef(*refs, *texId, Arcane::AssetRefKind::References));
    }

    fs::remove_all(dir, ec);
}

TEST_CASE("MaterialSurfaceFor rejects a file whose type contradicts 'material'", "[assets]")
{
    // The hoisted "type" gate (Task 1's deferred minor, taken in this same fix
    // wave): the "kind" branch used to answer before confirming the file is a
    // material at all. A non-material JSON that happens to carry a "kind"
    // string must not read as a material subkind. The gate rejects a
    // CONTRADICTING type only -- a hand-authored .arcmat with "kind" and no
    // "type" (ReferenceProject's own three) still resolves, and that asymmetry
    // is exactly what the second half of this case pins.
    const fs::path dir = fs::temp_directory_path() / "arc_matsurface_typegate_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    const char* kImposterGuid = "7e5a000c-0001-4001-8001-000000000001";
    const char* kBareGuid     = "7e5a000c-0001-4001-8001-000000000002";
    const auto imposter = WriteFile(dir, "imposter.json",
        std::string(R"({"id":")") + kImposterGuid + R"(","type":"prefab","kind":"sprite"})");
    const auto bare = WriteFile(dir, "bare.arcmat",
        std::string(R"({"id":")") + kBareGuid + R"(","kind":"sprite","params":{}})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        const std::string g = id.Value().ToString();
        if (g == kImposterGuid) return imposter;
        if (g == kBareGuid)     return bare;
        return std::nullopt;
    });

    const auto imposterId = Arcane::Guid::FromString(kImposterGuid);
    const auto bareId     = Arcane::Guid::FromString(kBareGuid);
    REQUIRE(imposterId.has_value());
    REQUIRE(bareId.has_value());

    REQUIRE_FALSE(assets->MaterialSurfaceFor(*imposterId).has_value());
    REQUIRE(assets->MaterialSurfaceFor(*bareId) == Arcane::MaterialSurface::Sprite);

    fs::remove_all(dir, ec);
}
