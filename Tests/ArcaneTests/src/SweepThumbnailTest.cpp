// Settings arc S6-39: editor.thumbnail.* -- the Asset Browser thumbnails'
// size, clock, failure budget, mesh camera FOV and framing margin (Project
// scope, latched when the harvester is created). The thumbnail golden set
// ("[thumbs][golden]") is bound to these defaults.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/EditorThumbnailSettings.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

#include <optional>

using namespace Arcane;

TEST_CASE("sweep: thumbnail defaults are the golden-bound literals", "[sweep][thumbnail]")
{
    const Editor::EditorThumbnailSettings t{};
    CHECK(t.size == 64u);
    CHECK(Test::SameBits(t.time, 0.35f));
    CHECK(t.maxRetries == 3);
    CHECK(Test::SameBits(t.meshFovDegrees, 35.0f));
    CHECK(Test::SameBits(t.framingMargin, 0.15f));
    CHECK(Editor::ThumbnailCheckerCell(t) == 16.0f);
    Test::RequireDefault("editor.thumbnail.size", CVarValue::UInt32(64u));
    Test::RequireDefault("editor.thumbnail.time", CVarValue::Float32(0.35f));
    Test::RequireDefault("editor.thumbnail.maxRetries", CVarValue::Int32(3));
    Test::RequireDefault("editor.thumbnail.meshFovDegrees", CVarValue::Float32(35.0f));
    Test::RequireDefault("editor.thumbnail.framingMargin", CVarValue::Float32(0.15f));

    if (!Test::InThisBuild("editor.thumbnail.size")) return;   // Dev: compiled out of Dist
    const std::optional<CVarDescInfo> d = CVarRegistry::Get().Describe("editor.thumbnail.size");
    REQUIRE(d.has_value());
    CHECK(d->scope == SettingScope::Project);
    CHECK(d->apply == ApplyMode::Restart);
    CHECK(d->audience == Audience::Editor);
}

TEST_CASE("sweep: the thumbnail cache name is keyed by the picture-changing settings", "[sweep][thumbnail]")
{
    const Editor::EditorThumbnailSettings def{};
    CHECK(Editor::ThumbnailCacheSuffix(def).empty());   // the default cache stays "<guid>.png"

    Editor::EditorThumbnailSettings big = def;   big.size = 128;
    Editor::EditorThumbnailSettings later = def; later.time = 1.0f;
    Editor::EditorThumbnailSettings wide = def;  wide.meshFovDegrees = 60.0f;
    Editor::EditorThumbnailSettings loose = def; loose.framingMargin = 0.5f;
    Editor::EditorThumbnailSettings retry = def; retry.maxRetries = 10;

    CHECK_FALSE(Editor::ThumbnailCacheSuffix(big).empty());
    CHECK_FALSE(Editor::ThumbnailCacheSuffix(later).empty());
    CHECK_FALSE(Editor::ThumbnailCacheSuffix(wide).empty());
    CHECK_FALSE(Editor::ThumbnailCacheSuffix(loose).empty());
    CHECK(Editor::ThumbnailCacheSuffix(big) != Editor::ThumbnailCacheSuffix(later));
    CHECK(Editor::ThumbnailCacheSuffix(wide) != Editor::ThumbnailCacheSuffix(loose));
    CHECK(Editor::ThumbnailCacheSuffix(retry).empty());   // the failure budget does not change the picture

    CHECK(Editor::ThumbnailCheckerCell(big) == 32.0f);
}
