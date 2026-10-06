// Settings arc S4 (spec s7.1): the three shipped presets, the .arctheme
// round trip, and the contrast bars every preset meets.
#include <catch2/catch_test_macros.hpp>
#include "Settings/EditorThemeSettings.hpp"
#include "Settings/ThemePresets.hpp"
#include "Widgets/EditorTheme.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Arcane::Editor;

namespace
{
    ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
    std::filesystem::path Preset(std::string_view name) { return ThemePresetDir() / (std::string(name) + ".arctheme"); }
}

TEST_CASE("Theme presets: Dark.arctheme is today's palette, to the 8-bit colour", "[theme][editor]")
{
    const auto file = ReadThemeFile(Preset("Dark"));
    REQUIRE(file.has_value());
    CHECK(file->unknownKeys.empty());
    CHECK(file->colors.size() == kThemeTokens.size());
    const Theme::Palette p = ToPalette(ApplyThemeFileTo(*file, EditorThemeSettings{}));
    for (const ThemeToken& t : kThemeTokens)
    {
        INFO(t.field);
        CHECK(U32(p.*(t.palette)) == U32(Theme::kDarkPalette.*(t.palette)));
    }
}

TEST_CASE("Theme presets: every shipped preset sets every token and meets every contrast bar", "[theme][editor]")
{
    for (std::string_view name : kThemePresetNames)
    {
        INFO(name);
        const auto file = ReadThemeFile(Preset(name));
        REQUIRE(file.has_value());
        CHECK(file->colors.size() == kThemeTokens.size());
        CHECK(file->unknownKeys.empty());
        const Theme::Palette p = ToPalette(ApplyThemeFileTo(*file, EditorThemeSettings{}));
        for (const ContrastRow& row : ContrastReport(p))
        {
            INFO(row.label << " " << row.ratio << " (min " << row.minRatio << ")");
            CHECK(row.ok);
        }
    }
}

TEST_CASE("Theme files: export then import returns every colour", "[theme][editor]")
{
    Theme::Palette mine = Theme::kDarkPalette;
    mine.text = ImVec4(1.0f, 0.5f, 0.25f, 1.0f);
    mine.rowStripe = ImVec4(0.0f, 0.0f, 0.0f, 0.2f);
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "s4-roundtrip.arctheme";
    std::string error;
    REQUIRE(WriteThemeFile(path, "Mine", mine, &error));
    const auto file = ReadThemeFile(path);
    REQUIRE(file.has_value());
    CHECK(file->name == "Mine");
    const Theme::Palette back = ToPalette(ApplyThemeFileTo(*file, EditorThemeSettings{}));
    for (const ThemeToken& t : kThemeTokens)
    {
        INFO(t.field);
        CHECK(U32(back.*(t.palette)) == U32(mine.*(t.palette)));
    }
    std::filesystem::remove(path);
}

TEST_CASE("Theme files: an unknown key is reported; a bad colour is refused naming its key", "[theme][editor]")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "s4-bad.arctheme";
    {
        std::ofstream out(path);
        out << R"({ "format": "arctheme", "version": 1, "name": "x", "colors": { "editor.theme.text": "#ff0000ff", "editor.theme.nope": "#000000" } })";
    }
    auto file = ReadThemeFile(path);
    REQUIRE(file.has_value());
    REQUIRE(file->unknownKeys.size() == 1);
    CHECK(file->unknownKeys[0] == "editor.theme.nope");
    REQUIRE(file->colors.size() == 1);
    CHECK(file->colors[0].first == "text");
    {
        std::ofstream out(path);
        out << R"({ "format": "arctheme", "version": 1, "colors": { "editor.theme.panel": "red" } })";
    }
    file = ReadThemeFile(path);
    REQUIRE_FALSE(file.has_value());
    CHECK(file.error().find("editor.theme.panel") != std::string::npos);
    std::filesystem::remove(path);
    CHECK_FALSE(ParseHexColor("#12345").has_value());
    CHECK(U32(*ParseHexColor("#102030")) == IM_COL32(0x10, 0x20, 0x30, 0xff));
    CHECK(FormatHexColor(Theme::kDarkPalette.warning) == "#f2c44dff");   // ImGui's rounding: 0.3f -> 0x4d
}

TEST_CASE("Theme presets: applying Light writes the EditorUser rung for every token and Settings<> follows", "[theme][editor]")
{
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    const auto file = ReadThemeFile(Preset("Light"));
    REQUIRE(file.has_value());
    CHECK(ApplyThemeToRegistry(*file, reg) == kThemeTokens.size());
    reg.PublishImmediate();
    CHECK(U32(ToPalette(Arcane::Settings<EditorThemeSettings>()).panel) == IM_COL32(0xf0, 0xf0, 0xf0, 0xff));
    const auto explain = reg.Explain("editor.theme.panel");
    REQUIRE(explain.has_value());
    CHECK(explain->setBy == Arcane::SetBy::EditorUser);
    reg.RevertLayer(Arcane::SetBy::EditorUser);   // leave the global registry as found
    reg.PublishImmediate();
    CHECK(SameThemeSettings(Arcane::Settings<EditorThemeSettings>(), EditorThemeSettings{}));
}
