// Settings sweep S6-40: editor.input.* (the Input Actions editor's rebind
// timeout and live-highlight wash) and editor.crash.maxRows (the crash
// viewer's frame list). Defaults are the pre-sweep literals; the existing
// [editor][input] rebind cases are the binding proof at the defaults.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/EditorDocumentUiSettings.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

#include <optional>
#include <string>
#include <string_view>

using namespace Arcane;

TEST_CASE("sweep: input-editor and crash-viewer defaults are the pre-sweep literals", "[sweep][document-ui]")
{
    CHECK(Editor::InputEditorSettings{}.rebindTimeoutSeconds == 10.0f);
    CHECK(Editor::InputEditorSettings{}.liveHighlightBase == 0.12f);
    CHECK(Editor::InputEditorSettings{}.liveHighlightGain == 0.2f);
    CHECK(Editor::CrashViewerSettings{}.maxRows == 24);
    Test::RequireDefault("editor.input.rebindTimeoutSeconds", CVarValue::Float32(10.0f));
}

TEST_CASE("sweep: every editor.input / editor.crash field registers as a per-machine, Live editor preference",
          "[sweep][document-ui]")
{
    Test::RequireDefault("editor.input.liveHighlightBase", CVarValue::Float32(0.12f));
    Test::RequireDefault("editor.input.liveHighlightGain", CVarValue::Float32(0.2f));
    Test::RequireDefault("editor.crash.maxRows", CVarValue::Int32(24));

    CVarRegistry& reg = CVarRegistry::Get();
    for (const std::string_view n : { "editor.input.rebindTimeoutSeconds", "editor.input.liveHighlightBase",
                                      "editor.input.liveHighlightGain", "editor.crash.maxRows" })
    {
        if (!Test::InThisBuild(n)) continue;   // Dev: compiled out of Dist
        const std::optional<CVarDescInfo> d = reg.Describe(n);
        INFO("cvar " << std::string(n));
        REQUIRE(d.has_value());
        CHECK(d->audience == Audience::Editor);
        CHECK(d->scope == SettingScope::PreferencesMachine);
        CHECK(d->apply == ApplyMode::Live);
    }
}

TEST_CASE("sweep: the live-highlight wash is the pre-sweep alpha at the defaults and follows the settings",
          "[sweep][document-ui]")
{
    const Editor::InputEditorSettings s{};
    for (const float v : { 0.0f, 0.25f, 0.5f, 1.0f, 3.0f })
    {
        INFO("v " << v);
        CHECK(Test::SameBits(Editor::LiveHighlightAlpha(s, v), 0.12f + 0.2f * (v < 1.0f ? v : 1.0f)));
    }
    Editor::InputEditorSettings custom{};
    custom.liveHighlightBase = 0.3f;
    custom.liveHighlightGain = 0.5f;
    CHECK(Editor::LiveHighlightAlpha(custom, 0.0f) == 0.3f);
    CHECK(Editor::LiveHighlightAlpha(custom, 2.0f) == 0.8f);   // the signal saturates at 1
}

// S6-45: the crash viewer's first-use window size and log-tail rows
// (inventory "CrashReportDocument.cpp:168" / ":387", S5-2 review).
TEST_CASE("sweep: editor.crash.initialSize and editor.crash.textRows are the pre-sweep literals", "[sweep][documents]")
{
    const Editor::CrashViewerSettings c{};
    CHECK(c.initialSize.x == 760.0f); CHECK(c.initialSize.y == 760.0f);
    CHECK(c.textRows == 16);
    Test::RequireDefault("editor.crash.initialSize", CVarValue::Vec2({ 760.0f, 760.0f }));
    Test::RequireDefault("editor.crash.textRows", CVarValue::Int32(16));
    const std::optional<CVarDescInfo> d = CVarRegistry::Get().Describe("editor.crash.textRows");
    REQUIRE(d.has_value());
    CHECK(d->scope == SettingScope::PreferencesMachine);
    CHECK(d->apply == ApplyMode::Live);
    REQUIRE(d->min.has_value()); REQUIRE(d->max.has_value());
    CHECK(*d->min == CVarValue::Int32(4)); CHECK(*d->max == CVarValue::Int32(200));
}
