// Settings arc S4 (spec s7.3): the font family list (bundled + the user's
// fonts folder) and the editor.ui.* registration.
#include <catch2/catch_test_macros.hpp>
#include "Settings/EditorUiSettings.hpp"
#include "Widgets/EditorFonts.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <filesystem>
#include <fstream>

using namespace Arcane::Editor;

TEST_CASE("Editor fonts: bundled families first, then the user's .ttf/.otf; resolve falls back", "[settings-ui][editor]")
{
    const std::filesystem::path exe = "C:/fake/exe";
    const std::filesystem::path user = std::filesystem::temp_directory_path() / "s4-fonts";
    std::filesystem::create_directories(user);
    { std::ofstream(user / "Zed Mono.ttf") << "x"; std::ofstream(user / "Atkinson.otf") << "x"; std::ofstream(user / "notes.txt") << "x"; }
    const auto families = ListEditorFontFamilies(exe, user);
    REQUIRE(families.size() == 5);
    CHECK(families[0].name == "Inter");
    CHECK(families[1].name == "Roboto");
    CHECK(families[2].name == "JetBrains Mono");
    CHECK(families[3].name == "Atkinson");
    CHECK(families[4].name == "Zed Mono");
    CHECK_FALSE(families[3].bundled);
    CHECK(ResolveEditorFontFamily(families, "Zed Mono", "Inter") == user / "Zed Mono.ttf");
    CHECK(ResolveEditorFontFamily(families, "Missing", "Inter") == families[0].file);
    CHECK(DefaultEditorFontRequest(exe).uiFace == families[0].file);
    CHECK(DefaultEditorFontRequest(exe).sizePx == 16.0f);
    std::filesystem::remove_all(user);
}

TEST_CASE("EditorUiSettings registers editor.ui.* with today's defaults", "[settings-ui][editor]")
{
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    reg.PublishImmediate();
    const auto get = [&](std::string_view n) { const auto v = reg.Get(reg.Find(n)); REQUIRE(v.has_value()); return *v; };
    CHECK(get("editor.ui.fontFamily").AsString() == "Inter");
    CHECK(get("editor.ui.monoFontFamily").AsString() == "JetBrains Mono");
    CHECK(get("editor.ui.fontSize").AsFloat32() == 16.0f);
    CHECK(get("editor.ui.scale").AsFloat32() == 1.0f);
    CHECK(get("editor.ui.followDpi").AsBool() == false);
}
