// Settings sweep S6-32 (inventory Part 3 EditorApp.hpp:943 / :1218,
// DeferredPick.hpp:205, EditorApp.cpp:2435-2436, DefaultLayout.hpp:41-47;
// "Persistence stores" [EditorPlayMode][State]): the Play launch mode is a
// Pref-P cvar imported once from an old ini section and never written there
// again; the viewport's misc tunables and the factory layout's geometry are
// cvars whose defaults are the pre-sweep literals.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/EditorPlaySettings.hpp"
#include "Settings/EditorViewportSettings.hpp"
#include "Panels/DefaultLayout.hpp"
#include <Arcane/Config/Settings.hpp>
using namespace Arcane;

namespace
{
    // Reverts the User rung the import writes even when a check fails midway.
    struct UserLayerReset
    {
        ~UserLayerReset()
        {
            CVarRegistry::Get().RevertLayer(SetBy::User);
            CVarRegistry::Get().PublishImmediate();
        }
    };
}

TEST_CASE("sweep: play and factory-layout defaults are the pre-sweep literals", "[sweep][play-layout]")
{
    CHECK(Editor::EditorPlaySettings{}.launchMode == Editor::PlayLaunchMode::Viewport);
    const Editor::LayoutFactorySettings f{};
    CHECK(f.inspectorWidth == 380.0f); CHECK(f.outlinerWidth == 270.0f); CHECK(f.bottomBand == 350.0f);
    CHECK(f.centralMinFraction == 0.40f); CHECK(f.browserRefPx == 1144.0f); CHECK(f.assetsInspectorRefPx == 392.0f);
    CHECK(Editor::DefaultAssetsInspectorBandFraction(f) == 392.0f / (1144.0f + 392.0f));
    Test::RequireDefault("editor.play.launchMode", CVarValue::Enum(static_cast<std::int32_t>(Editor::PlayLaunchMode::Viewport)));
    std::optional<Editor::PlayLaunchMode> legacy;
    CHECK(Editor::ReadPlayModeIniLine("Mode=1", legacy));
    REQUIRE(legacy.has_value());
}

TEST_CASE("sweep: the factory layout cvars and the viewport misc keep their literals", "[sweep][play-layout]")
{
    Test::RequireDefault("editor.layout.factory.inspectorWidth", CVarValue::Float32(380.0f));
    Test::RequireDefault("editor.layout.factory.outlinerWidth", CVarValue::Float32(270.0f));
    Test::RequireDefault("editor.layout.factory.bottomBand", CVarValue::Float32(350.0f));
    Test::RequireDefault("editor.layout.factory.centralMinFraction", CVarValue::Float32(0.40f));
    Test::RequireDefault("editor.layout.factory.browserRefPx", CVarValue::Float32(1144.0f));
    Test::RequireDefault("editor.layout.factory.assetsInspectorRefPx", CVarValue::Float32(392.0f));

    const Editor::EditorViewportSettings v{};
    CHECK_FALSE(v.physicsOverlay);
    CHECK(v.pickMaxFramesInFlight == 64u);
    CHECK(v.fallbackExtentW == 1280u);
    CHECK(v.fallbackExtentH == 720u);
    Test::RequireDefault("editor.viewport.physicsOverlay", CVarValue::Bool(false));
    Test::RequireDefault("editor.viewport.pickMaxFramesInFlight", CVarValue::UInt32(64u));
    Test::RequireDefault("editor.viewport.fallbackExtentW", CVarValue::UInt32(1280u));
    Test::RequireDefault("editor.viewport.fallbackExtentH", CVarValue::UInt32(720u));

    // The scopes the inventory rows name: the play mode is per project, the
    // factory layout and the pick/extent tunables are per machine (Dev).
    CVarRegistry& reg = CVarRegistry::Get();
    const auto play = reg.Describe("editor.play.launchMode");
    REQUIRE(play.has_value());
    CHECK(play->scope == SettingScope::PreferencesProject);
    CHECK(play->apply == ApplyMode::NextWorld);
    CHECK(play->audience == Audience::Editor);
    // The three Dev rows: compiled out of Dist (each proven absent: `&`, not `&&`).
    if (!(Test::InThisBuild("editor.layout.factory.inspectorWidth")
          & Test::InThisBuild("editor.viewport.pickMaxFramesInFlight")
          & Test::InThisBuild("editor.viewport.fallbackExtentW")))
        return;
    const auto inspector = reg.Describe("editor.layout.factory.inspectorWidth");
    REQUIRE(inspector.has_value());
    CHECK(inspector->scope == SettingScope::PreferencesMachine);
    CHECK(HasFlag(inspector->flags, CVarFlags::Dev));
    CHECK(inspector->categoryPath == "Layout/Factory");   // the Layout page's node, not a derived Editor/Layout
    const auto pick = reg.Describe("editor.viewport.pickMaxFramesInFlight");
    REQUIRE(pick.has_value());
    CHECK(pick->scope == SettingScope::PreferencesMachine);
    const auto extent = reg.Describe("editor.viewport.fallbackExtentW");
    REQUIRE(extent.has_value());
    CHECK(extent->scope == SettingScope::PreferencesMachine);
    CHECK(extent->apply == ApplyMode::Restart);
}

TEST_CASE("sweep: the factory layout at its defaults is the pre-sweep geometry", "[sweep][play-layout]")
{
    const Editor::LayoutFactorySettings f{};
    // 1920 x 954: nothing clamps -- the literal pixel targets.
    const Editor::DefaultLayoutPixels full = Editor::ComputeDefaultLayoutPixels(1920.0f, 954.0f, f);
    CHECK(Test::SameBits(full.inspector, 380.0f));
    CHECK(Test::SameBits(full.outliner, 270.0f));
    CHECK(Test::SameBits(full.bottomBand, 350.0f));
    // 800 x 500: both sides share (1 - 0.40) of the width, the band (1 - 0.40) of the height.
    const Editor::DefaultLayoutPixels small = Editor::ComputeDefaultLayoutPixels(800.0f, 500.0f, f);
    const float sideScale = (800.0f * (1.0f - 0.40f)) / (380.0f + 270.0f);
    CHECK(Test::SameBits(small.inspector, 380.0f * sideScale));
    CHECK(Test::SameBits(small.outliner, 270.0f * sideScale));
    CHECK(Test::SameBits(small.bottomBand, 500.0f * (1.0f - 0.40f)));
    // A changed factory value reaches the geometry.
    Editor::LayoutFactorySettings wide = f;
    wide.inspectorWidth = 500.0f;
    CHECK(Editor::ComputeDefaultLayoutPixels(1920.0f, 954.0f, wide).inspector == 500.0f);
}

TEST_CASE("sweep: an old [EditorPlayMode][State] line is validated as before", "[sweep][play-layout]")
{
    std::optional<Editor::PlayLaunchMode> legacy;
    CHECK(Editor::ReadPlayModeIniLine("Mode=4", legacy));
    REQUIRE(legacy.has_value());
    CHECK(*legacy == Editor::PlayLaunchMode::SeparateServerProcess);
    legacy.reset();
    CHECK_FALSE(Editor::ReadPlayModeIniLine("Mode=5", legacy));    // past the last enumerator
    CHECK_FALSE(Editor::ReadPlayModeIniLine("Mode=-1", legacy));
    CHECK_FALSE(Editor::ReadPlayModeIniLine("Mode=", legacy));
    CHECK_FALSE(Editor::ReadPlayModeIniLine("Speed=1", legacy));
    CHECK_FALSE(legacy.has_value());                                // a refused line leaves it untouched
}

TEST_CASE("sweep: an old play mode is imported once, and never over the user's own choice", "[sweep][play-layout]")
{
    UserLayerReset reset;
    CVarRegistry& reg = CVarRegistry::Get();
    CHECK(Editor::ImportLegacyPlayMode(reg, Editor::PlayLaunchMode::ListenServer));
    reg.PublishImmediate();
    CHECK(Settings<Editor::EditorPlaySettings>().launchMode == Editor::PlayLaunchMode::ListenServer);
    // Once: the User rung now holds a record, so a second (stale) section is ignored.
    CHECK_FALSE(Editor::ImportLegacyPlayMode(reg, Editor::PlayLaunchMode::SeparateWindow));
    reg.PublishImmediate();
    CHECK(Settings<Editor::EditorPlaySettings>().launchMode == Editor::PlayLaunchMode::ListenServer);
}
