// Asset Browser (Slice 6): the PURE parts -- extension classification, entry
// building over a REAL scanned AssetRegistry, and filter/search -- headless.
// (DrawAssetBrowserPanel is ImGui and desk-verified.)

#include <catch2/catch_test_macros.hpp>

#include "AssetBrowser.hpp"

#include <Arcane/Project/AssetRegistry.hpp>

#include <filesystem>
#include <fstream>

using namespace Arcane::Editor;

TEST_CASE("AssetKindOf classifies by extension, case-insensitive", "[editor]")
{
    CHECK(AssetKindOf("game://materials/glow.armat") == AssetKind::Material);
    CHECK(AssetKindOf("game://tex/hero.PNG") == AssetKind::Texture);
    CHECK(AssetKindOf("game://tex/sky.hdr") == AssetKind::Texture);
    CHECK(AssetKindOf("game://sfx/hit.wav") == AssetKind::Audio);
    CHECK(AssetKindOf("game://font/inter.ttf") == AssetKind::Font);
    CHECK(AssetKindOf("game://data/quests.json") == AssetKind::Data);
    CHECK(AssetKindOf("game://misc/readme.txt") == AssetKind::Other);
    CHECK(AssetKindOf("noextension") == AssetKind::Other);
}

TEST_CASE("BuildAssetEntries snapshots a scanned registry, sorted by name", "[editor]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_browser_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sub");

    std::ofstream(dir / "zeta.json", std::ios::binary)
        << R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })";
    std::ofstream(dir / "sub" / "alpha.armat", std::ios::binary)
        << R"({ "id": "bbbb2222-2222-4222-8222-222222222222", "snippet": "x" })";
    std::ofstream(dir / "hero.png", std::ios::binary) << "notapng";   // sidecar minted

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 3);

    const auto entries = BuildAssetEntries(registry);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].name == "alpha");
    CHECK(entries[0].kind == AssetKind::Material);
    CHECK(entries[0].mountPath == "game://sub/alpha.armat");
    CHECK(entries[1].name == "hero");
    CHECK(entries[1].kind == AssetKind::Texture);
    CHECK(entries[2].name == "zeta");
    CHECK(entries[2].kind == AssetKind::Data);
    for (const AssetEntry& e : entries)
        CHECK(e.guid.IsValid());

    fs::remove_all(dir, ec);
}

TEST_CASE("MatchesFilter combines kind filter and case-insensitive search", "[editor]")
{
    AssetEntry mat;
    mat.name = "GlowPulse";
    mat.mountPath = "game://materials/glow_pulse.armat";
    mat.kind = AssetKind::Material;

    // Kind filter.
    CHECK(MatchesFilter(mat, -1, ""));
    CHECK(MatchesFilter(mat, static_cast<int>(AssetKind::Material), ""));
    CHECK_FALSE(MatchesFilter(mat, static_cast<int>(AssetKind::Texture), ""));

    // Search hits name (case-insensitive) and path.
    CHECK(MatchesFilter(mat, -1, "glow"));
    CHECK(MatchesFilter(mat, -1, "PULSE"));
    CHECK(MatchesFilter(mat, -1, "materials/"));
    CHECK_FALSE(MatchesFilter(mat, -1, "sprite"));

    // Both must pass.
    CHECK(MatchesFilter(mat, static_cast<int>(AssetKind::Material), "glow"));
    CHECK_FALSE(MatchesFilter(mat, static_cast<int>(AssetKind::Texture), "glow"));
}
