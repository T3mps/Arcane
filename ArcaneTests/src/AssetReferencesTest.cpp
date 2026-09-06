// Asset-manager arc (ABI v22), Task 1: MaterialSurfaceFor -- the material
// SUBKIND for a Guid, resolved through the installed AssetResolver and, for
// an instance, walked through its parent chain to the base's own "kind".
// Modeled on AssetsTest.cpp's Assets::Create() + SetAssetResolver pattern
// (real temp-dir files, a resolver lambda mapping test guids to them).

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
