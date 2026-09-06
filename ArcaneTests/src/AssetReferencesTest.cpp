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
