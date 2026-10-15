// S6-5: the asset tunables are settings -- assets.cache.byteBudget (the
// facade's byte budget, read when a Runtime creates its Assets),
// assets.material.maxParentDepth (the parent-chain walk bound),
// assets.sprite.defaultPixelsPerUnit (what a newly minted sprite is seeded
// with; the editor declares it) and assets.cook.pendingRepollInterval (the
// texture cache's PendingCook re-poll cadence, pinned in
// NriTextureCacheArtifactTest.cpp).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Assets/AssetsSettings.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Render/Nri/AssetsCookSettings.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include "Documents/SpriteDocument.hpp"
#include "Settings/AssetsSpriteSettings.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace Arcane;
namespace fs = std::filesystem;

namespace
{
    void CheckMeta(const char* name, Audience audience, SettingScope scope, ApplyMode apply, bool dev)
    {
        INFO("cvar " << name);
        if (!Test::InThisBuild(name)) { CHECK(dev); return; }   // Dist: a Dev row is compiled out
        const auto e = CVarRegistry::Get().Explain(name);
        REQUIRE(e.has_value());
        CHECK(e->audience == audience);
        CHECK(e->scope == scope);
        CHECK(e->apply == apply);
        CHECK(HasFlag(e->flags, CVarFlags::Dev) == dev);
        CHECK_FALSE(e->help.empty());
    }

    fs::path FreshDir(const char* leaf)
    {
        const fs::path dir = fs::temp_directory_path() / leaf;
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        return dir;
    }

    fs::path WriteFile(const fs::path& dir, const char* name, const std::string& text)
    {
        const fs::path p = dir / name;
        std::ofstream(p) << text;
        return p;
    }
}

TEST_CASE("sweep: asset defaults are the pre-sweep literals", "[sweep][assets]")
{
    STATIC_REQUIRE(std::is_same_v<decltype(AssetsCacheSettings{}.byteBudget), std::uint64_t>);
    CHECK(AssetsCacheSettings{}.byteBudget == 256ull * 1024 * 1024);
    CHECK(AssetsDesc{}.byteBudget == AssetsCacheSettings{}.byteBudget);
    CHECK(AssetsMaterialSettings{}.maxParentDepth == 8);
    CHECK(Test::SameBits(Editor::AssetsSpriteSettings{}.defaultPixelsPerUnit, 100.0f));
    // The absent-key fallback stays the struct default (inventory row 420),
    // equal to the setting's default, so a default project mints and loads
    // exactly the sprites it did before.
    CHECK(Test::SameBits(SpriteAssetData{}.ppu, Editor::AssetsSpriteSettings{}.defaultPixelsPerUnit));
    CHECK(AssetsCookSettings{}.pendingRepollInterval == 32u);
    Test::RequireDefault("assets.cache.byteBudget", CVarValue::UInt64(256ull << 20));
    Test::RequireDefault("assets.material.maxParentDepth", CVarValue::Int32(8));
    Test::RequireDefault("assets.sprite.defaultPixelsPerUnit", CVarValue::Float32(100.0f));
    Test::RequireDefault("assets.cook.pendingRepollInterval", CVarValue::UInt32(32u));
}

TEST_CASE("sweep: asset settings carry the inventory's audience, scope, apply and Dev flag", "[sweep][assets]")
{
    CheckMeta("assets.cache.byteBudget", Audience::Game, SettingScope::Project, ApplyMode::Restart, false);
    // "Editor Dev" rows declared in Core: Game + Dev (plan Open questions S6).
    CheckMeta("assets.material.maxParentDepth", Audience::Game, SettingScope::Project, ApplyMode::Live, true);
    CheckMeta("assets.sprite.defaultPixelsPerUnit", Audience::Editor, SettingScope::Project, ApplyMode::Live, false);
    CheckMeta("assets.cook.pendingRepollInterval", Audience::Game, SettingScope::Project, ApplyMode::Live, true);
}

TEST_CASE("sweep: a newly minted sprite is seeded from assets.sprite.defaultPixelsPerUnit", "[sweep][assets]")
{
    const Guid texture = Guid::Generate();
    {
        const SpriteAssetData d = Editor::SpriteDocument::NewSpriteData(texture, "fresh");
        CHECK(Test::SameBits(d.ppu, 100.0f));
        CHECK(d.texture == texture);
        CHECK(d.name == "fresh");
        CHECK(d.id.IsValid());
    }

    const fs::path dir = FreshDir("arc_sweep_sprite_ppu");
    const Test::ScopedCodeRung ppu("assets.sprite.defaultPixelsPerUnit", CVarValue::Float32(32.0f));

    // The seed survives a save/load: the writer always writes ppu.
    const SpriteAssetData minted = Editor::SpriteDocument::NewSpriteData(texture, "pixel-art");
    CHECK(minted.ppu == 32.0f);
    REQUIRE(SaveSpriteAsset(dir / "minted.arcsprite", minted));
    const auto reloaded = LoadSpriteAsset(dir / "minted.arcsprite");
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->ppu == 32.0f);
    CHECK(ComputeSpriteGeom(*reloaded, 64, 64).sizeMeters == glm::vec2(2.0f, 2.0f));

    // It seeds NEW sprites only: an existing file without the key keeps the
    // struct default, and a non-positive ppu resolves through it too.
    WriteFile(dir, "legacy.arcsprite",
              R"({"id":"7e5a0005-0001-4001-8001-000000000001","type":"sprite","name":"legacy"})");
    const auto legacy = LoadSpriteAsset(dir / "legacy.arcsprite");
    REQUIRE(legacy.has_value());
    CHECK(legacy->ppu == SpriteAssetData{}.ppu);
    SpriteAssetData zero = *legacy;
    zero.ppu = 0.0f;
    CHECK(ComputeSpriteGeom(zero, 200, 100).sizeMeters == glm::vec2(2.0f, 1.0f));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// S6-5 fix round 1: SpritePixelsPerUnitRange reads the setting's DECLARED
// range (no second literal), and a project default above 4096 mints a sprite
// carrying it. The Sprite document's row no longer clamps to this range
// (S6-42 fix round 1): it clamps to editor.sprite.ppuMin/ppuMax widened only
// to the sprite's current value -- the drag halves (a 5000 sprite re-edits, a
// non-default ppuMax caps) are in SpriteDocumentUndoTest.cpp.
TEST_CASE("sweep: SpritePixelsPerUnitRange is assets.sprite.defaultPixelsPerUnit's declared range", "[sweep][assets][sprite]")
{
    const auto meta = CVarRegistry::Get().Metadata(CVarRegistry::Get().Find("assets.sprite.defaultPixelsPerUnit"));
    REQUIRE(meta.has_value());
    REQUIRE(meta->min.has_value());
    REQUIRE(meta->max.has_value());
    REQUIRE(meta->max->type == CVarType::Float32);

    const auto row = Editor::SpritePixelsPerUnitRange();
    REQUIRE(row.has_value());
    CHECK(row->min == static_cast<double>(meta->min->AsFloat32()));
    CHECK(row->max == static_cast<double>(meta->max->AsFloat32()));
    CHECK(row->max == 10000.0);   // inventory row 420: [1, 10000]

    // A registry that never registered it falls back to the declaration: the
    // same range, still not a literal of the row's own.
    const CVarRegistry bare;
    const auto declared = Editor::SpritePixelsPerUnitRange(bare);
    REQUIRE(declared.has_value());
    CHECK(declared->min == row->min);
    CHECK(declared->max == row->max);

    // A default above the old 4096 cap seeds a sprite whose value the row holds.
    const Test::ScopedCodeRung ppu("assets.sprite.defaultPixelsPerUnit", CVarValue::Float32(5000.0f));
    const SpriteAssetData minted = Editor::SpriteDocument::NewSpriteData(Guid::Generate(), "big");
    CHECK(minted.ppu == 5000.0f);
    CHECK(static_cast<double>(minted.ppu) >= row->min);
    CHECK(static_cast<double>(minted.ppu) <= row->max);
}

TEST_CASE("sweep: assets.material.maxParentDepth bounds the parent-chain walk live", "[sweep][assets]")
{
    Test::SkipIfCompiledOut("assets.material.maxParentDepth");
    const fs::path dir = FreshDir("arc_sweep_matdepth");
    const auto base = WriteFile(dir, "base.arcmat",
        R"({"id":"7e5a0006-0001-4001-8001-000000000001","kind":"sprite","name":"B","params":{},"snippet":"","type":"material"})");
    const auto inst = WriteFile(dir, "inst.arcmat",
        R"({"id":"7e5a0006-0001-4001-8001-000000000002","parent":"7e5a0006-0001-4001-8001-000000000001","params":{},"type":"material"})");

    auto assets = Assets::Create();
    assets->SetAssetResolver([&](const AssetId& id) -> std::optional<fs::path>
    {
        const std::string g = id.Value().ToString();
        if (g == "7e5a0006-0001-4001-8001-000000000001") return base;
        if (g == "7e5a0006-0001-4001-8001-000000000002") return inst;
        return std::nullopt;
    });
    const auto baseId = Guid::FromString("7e5a0006-0001-4001-8001-000000000001");
    const auto instId = Guid::FromString("7e5a0006-0001-4001-8001-000000000002");
    REQUIRE(baseId.has_value());
    REQUIRE(instId.has_value());

    // Default (8): the one-hop instance resolves through its parent.
    CHECK(assets->MaterialSurfaceFor(*instId) == MaterialSurface::Sprite);
    {
        // One step reads only the instance itself: its parent is past the bound.
        const Test::ScopedCodeRung depth("assets.material.maxParentDepth", CVarValue::Int32(1));
        CHECK(assets->MaterialSurfaceFor(*baseId) == MaterialSurface::Sprite);
        CHECK_FALSE(assets->MaterialSurfaceFor(*instId).has_value());
    }
    CHECK(assets->MaterialSurfaceFor(*instId) == MaterialSurface::Sprite);

    std::error_code ec;
    fs::remove_all(dir, ec);
}
