// Settings sweep S6-41: console.maxLines (the Core console's line cap, 0 =
// unbounded as before), the console.historySize shadow copy gone, and the
// editor's editor.console.* / editor.recents.* preferences. Defaults are the
// pre-sweep literals.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Config/ConsoleModel.hpp>
#include "Settings/EditorConsoleSettings.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

#include <optional>
#include <string>
#include <string_view>

using namespace Arcane;

TEST_CASE("sweep: console defaults; console.maxLines 0 keeps every line", "[sweep][console]")
{
    CHECK(Editor::EditorConsoleSettings{}.ringLines == 512);
    CHECK(Editor::EditorConsoleSettings{}.displayLineCap == 512);
    CHECK(Editor::EditorConsoleSettings{}.wrap);
    CHECK(Editor::RecentsSettings{}.maxProjectsShown == 10);
    Test::RequireDefault("console.maxLines", CVarValue::Int32(0));
    ConsoleModel m;
    for (int i = 0; i < 1000; ++i) m.AppendLine("line " + std::to_string(i));
    CHECK(m.Lines().size() == 1000u);
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("console.maxLines"), CVarValue::Int32(100), SetBy::Code); reg.PublishImmediate();
    m.AppendLine("one more");
    CHECK(m.Lines().size() == 100u);
    CHECK(m.Lines().back().text == "one more");
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
}

TEST_CASE("sweep: a submitted command's reply lines honour console.maxLines on the model's registry",
          "[sweep][console]")
{
    CVarRegistry reg;   // console.maxLines is a built-in on every registry, like console.historySize
    REQUIRE_FALSE(reg.Find("console.maxLines").IsStale());
    REQUIRE(reg.Set(reg.Find("console.maxLines"), CVarValue::Int32(3), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    ConsoleModel m;
    for (const char* line : { "cvarlist", "cvar_explain cheats", "console.maxLines" })
    {
        m.SetInput(line);
        m.Submit(reg, CVarContext::Editor);
    }
    REQUIRE(m.Lines().size() == 3u);
    // Six lines were appended ("> input" + reply per command); the oldest three dropped.
    CHECK(m.Lines()[1].text == "> console.maxLines");
    CHECK(m.Lines().back().text == "console.maxLines = 3");
}

TEST_CASE("sweep: editor.console / editor.recents register as per-machine editor preferences with their defaults",
          "[sweep][console]")
{
    Test::RequireDefault("editor.console.ringLines", CVarValue::Int32(512));
    Test::RequireDefault("editor.console.displayLineCap", CVarValue::Int32(512));
    Test::RequireDefault("editor.console.collapse", CVarValue::Bool(false));
    Test::RequireDefault("editor.console.autoScroll", CVarValue::Bool(true));
    Test::RequireDefault("editor.console.wrap", CVarValue::Bool(true));
    Test::RequireDefault("editor.console.replyLines", CVarValue::Int32(6));
    Test::RequireDefault("editor.console.categoryWidth", CVarValue::Int32(8));
    Test::RequireDefault("editor.recents.maxProjectsShown", CVarValue::Int32(10));
    Test::RequireDefault("editor.recents.maxScenes", CVarValue::Int32(10));

    CVarRegistry& reg = CVarRegistry::Get();
    for (const std::string_view n : { "editor.console.ringLines", "editor.console.displayLineCap",
                                      "editor.console.collapse", "editor.console.autoScroll", "editor.console.wrap",
                                      "editor.console.replyLines", "editor.console.categoryWidth",
                                      "editor.recents.maxProjectsShown", "editor.recents.maxScenes" })
    {
        const std::optional<CVarDescInfo> d = reg.Describe(n);
        INFO("cvar " << std::string(n));
        REQUIRE(d.has_value());
        CHECK(d->audience == Audience::Editor);
        CHECK(d->scope == SettingScope::PreferencesMachine);
        CHECK(d->apply == (n == "editor.console.ringLines" ? ApplyMode::Restart : ApplyMode::Live));
    }
    const std::optional<CVarDescInfo> maxLines = reg.Describe("console.maxLines");
    REQUIRE(maxLines.has_value());
    CHECK(maxLines->audience == Audience::Game);
    CHECK(maxLines->scope == SettingScope::PreferencesProject);
}
