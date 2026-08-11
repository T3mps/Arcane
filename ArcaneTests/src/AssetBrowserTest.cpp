// Asset Browser (Slice 6): the PURE parts -- extension classification, entry
// building over a REAL scanned AssetRegistry, and filter/search -- headless.
// (DrawAssetBrowserPanel is ImGui and desk-verified.)

#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetBrowser.hpp"

#include <Arcane/Project/AssetRegistry.hpp>

#include <filesystem>
#include <fstream>

using namespace Arcane::Editor;

TEST_CASE("AssetKindOf classifies by extension, case-insensitive", "[editor]")
{
    CHECK(AssetKindOf("game://materials/glow.arcmat") == AssetKind::Material);
    CHECK(AssetKindOf("game://materials/old.armat") == AssetKind::Other);   // dead spelling
    CHECK(AssetKindOf("game://tex/hero.PNG") == AssetKind::Texture);
    CHECK(AssetKindOf("game://tex/sky.hdr") == AssetKind::Texture);
    CHECK(AssetKindOf("game://sfx/hit.wav") == AssetKind::Audio);
    CHECK(AssetKindOf("game://font/inter.ttf") == AssetKind::Font);
    CHECK(AssetKindOf("game://data/quests.json") == AssetKind::Data);
    CHECK(AssetKindOf("game://misc/readme.txt") == AssetKind::Other);
    CHECK(AssetKindOf("noextension") == AssetKind::Other);
}

TEST_CASE("AssetKindFilterForFieldName infers the kind from the field name", "[editor]")
{
    CHECK(AssetKindFilterForFieldName("material") == static_cast<int>(AssetKind::Material));
    CHECK(AssetKindFilterForFieldName("baseMaterial") == static_cast<int>(AssetKind::Material));
    CHECK(AssetKindFilterForFieldName("texture") == static_cast<int>(AssetKind::Texture));
    CHECK(AssetKindFilterForFieldName("normalTexture") == static_cast<int>(AssetKind::Texture));
    CHECK(AssetKindFilterForFieldName("target") == -1);
    CHECK(AssetKindFilterForFieldName("") == -1);
}

TEST_CASE("IsIdentityGuidFieldName separates identity guids from asset refs", "[editor]")
{
    // Arcane::Identity.id is the live case: every entity carries one, the
    // registry can never resolve it, and treating it as a dangling asset ref
    // painted three "(missing)" rows on a perfectly healthy scene.
    CHECK(IsIdentityGuidFieldName("id"));
    CHECK(IsIdentityGuidFieldName("Id"));
    CHECK(IsIdentityGuidFieldName("guid"));
    CHECK(IsIdentityGuidFieldName("GUID"));
    // Exact match only: substring would eat "textureId", which the kind
    // heuristic correctly claims as a texture reference.
    CHECK_FALSE(IsIdentityGuidFieldName("textureId"));
    CHECK_FALSE(IsIdentityGuidFieldName("sprite"));
    CHECK_FALSE(IsIdentityGuidFieldName("material"));
    CHECK_FALSE(IsIdentityGuidFieldName(""));
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
    std::ofstream(dir / "sub" / "alpha.arcmat", std::ios::binary)
        << R"({ "id": "bbbb2222-2222-4222-8222-222222222222", "snippet": "x" })";
    std::ofstream(dir / "hero.png", std::ios::binary) << "notapng";   // sidecar minted

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 3);

    const auto entries = BuildAssetEntries(registry);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].name == "alpha");
    CHECK(entries[0].kind == AssetKind::Material);
    CHECK(entries[0].mountPath == "game://sub/alpha.arcmat");
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
    mat.mountPath = "game://materials/glow_pulse.arcmat";
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

TEST_CASE("AssetKindOf classifies scenes", "[editor]")
{
    CHECK(AssetKindOf("game://scenes/main.arcscene") == AssetKind::Scene);
    CHECK(AssetKindOf("game://scenes/MAIN.ARCSCENE") == AssetKind::Scene);
    // Not the material kind, and not a generic data file.
    CHECK(AssetKindOf("game://scenes/main.arcscene") != AssetKind::Data);
    CHECK(AssetKindOf("game://mat/glow.arcmat") == AssetKind::Material);
}

TEST_CASE("AssetKindOf classifies sprites, heuristic keeps material/texture ordering", "[editor]")
{
    CHECK(AssetKindOf("game://sprites/hero.arcsprite") == AssetKind::Sprite);
    CHECK(AssetKindFilterForFieldName("sprite") == static_cast<int>(AssetKind::Sprite));
    // A plain "material" field name has no "sprite" substring, so it never
    // even reaches the sprite branch -- this pins that adding that branch
    // leaves an UNRELATED field name unaffected, but it cannot by itself
    // detect a reordering (see the spriteMaterial case right below, which
    // can).
    CHECK(AssetKindFilterForFieldName("material") == static_cast<int>(AssetKind::Material)); // order preserved
    // THIS is the order-sensitive case: "spriteMaterial" contains BOTH
    // substrings (a real field: SpriteRenderer::material, Components.hpp:88,
    // sits beside SpriteRenderer::sprite, Components.hpp:78) and must still
    // resolve Material -- a heuristic that checked "sprite" before
    // "material" would return Sprite here instead and this CHECK would fail.
    CHECK(AssetKindFilterForFieldName("spriteMaterial") == static_cast<int>(AssetKind::Material));
}

TEST_CASE("a .arcscene is a native JSON asset and gets a minted id", "[editor][project]")
{
    // Native-JSON rule: a top-level "id" is read, or minted and written back.
    // Without .arcscene on that list a scene would scan as an untracked file,
    // never get a Guid, and never appear in the browser or resolve as bootScene.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_scene_asset_scan";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "scenes", ec);

    std::ofstream(dir / "scenes" / "main.arcscene")
        << R"({"version":2,"entities":[]})";   // no id -- must be minted

    Arcane::AssetRegistry reg;
    const std::size_t n = reg.ScanContent(dir, "game");
    CHECK(n == 1);

    const auto all = reg.All();
    REQUIRE(all.size() == 1);
    CHECK(all[0].first.IsValid());
    CHECK(all[0].second == "game://scenes/main.arcscene");

    // Written back: a second scan resolves to the SAME Guid.
    Arcane::AssetRegistry again;
    again.ScanContent(dir, "game");
    const auto all2 = again.All();
    REQUIRE(all2.size() == 1);
    CHECK(all2[0].first == all[0].first);

    fs::remove_all(dir, ec);
}
