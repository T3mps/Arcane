// Settings arc S4 (spec s7.4): named layouts are files; the session layout is
// seeded once; editor.layout.openPanelsAtStart speaks the panel vocabulary.
#include <catch2/catch_test_macros.hpp>
#include "Panels/LayoutLibrary.hpp"
#include "Panels/PanelRegistry.hpp"
#include "Settings/LayoutSettings.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace Arcane::Editor;

namespace
{
    std::filesystem::path Fresh(const char* name)
    {
        const std::filesystem::path d = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(d);
        std::filesystem::create_directories(d);
        return d;
    }
    std::string Read(const std::filesystem::path& p) { std::ifstream in(p); std::stringstream s; s << in.rdbuf(); return s.str(); }
}

TEST_CASE("LayoutLibrary: save, list (files only, sorted), load, delete", "[settings-ui][editor]")
{
    const std::filesystem::path dir = Fresh("s4-layouts");
    std::filesystem::create_directories(dir / "Session");
    const LayoutLibrary lib(dir);
    std::string error;
    REQUIRE(lib.Save("Wide", "[Window][A]\nPos=0,0\n", &error));
    REQUIRE(lib.Save("compact", "[Window][B]\n", &error));
    CHECK(lib.List() == std::vector<std::string>{ "compact", "Wide" });
    REQUIRE(lib.Load("Wide").has_value());
    CHECK(*lib.Load("Wide") == "[Window][A]\nPos=0,0\n");
    CHECK_FALSE(lib.Load("missing").has_value());
    CHECK(lib.Delete("compact"));
    CHECK(lib.List() == std::vector<std::string>{ "Wide" });
    CHECK_FALSE(lib.Save("default", "x", &error));
    CHECK_FALSE(error.empty());
    std::filesystem::remove_all(dir);
}

TEST_CASE("LayoutLibrary: pre-S4 session files left in the same folder are not listed as named layouts", "[settings-ui][editor]")
{
    // On Windows the pre-S4 session folder <EditorUserDir>\layouts IS the named
    // folder <EditorUserDir>\Layouts (case-insensitive): its default.ini and
    // <guid>.ini stay there (seeding copies), and must not show as layouts.
    const std::filesystem::path dir = Fresh("s4-layouts-legacy");
    { std::ofstream(dir / "default.ini") << "[old]\n"; std::ofstream(dir / "0f8fad5b-d9cb-469f-a165-70867728950e.ini") << "[old]\n"; }
    const LayoutLibrary lib(dir);
    std::string error;
    REQUIRE(lib.Save("Wide", "[w]\n", &error));
    CHECK(lib.List() == std::vector<std::string>{ "Wide" });
    std::filesystem::remove_all(dir);
}

TEST_CASE("Layout names: refused with a reason when they cannot be a file or would collide", "[settings-ui][editor]")
{
    CHECK_FALSE(ValidateLayoutName("My Layout 2").has_value());
    CHECK(ValidateLayoutName("").has_value());
    CHECK(ValidateLayoutName("a/b").has_value());
    CHECK(ValidateLayoutName("what?").has_value());
    CHECK(ValidateLayoutName("CON").has_value());
    CHECK(ValidateLayoutName("trailing.").has_value());
    CHECK(ValidateLayoutName("default").has_value());
    CHECK(ValidateLayoutName("Session").has_value());
    CHECK(ValidateLayoutName("0f8fad5b-d9cb-469f-a165-70867728950e").has_value());
    CHECK(ValidateLayoutName(std::string(65, 'x')).has_value());
}

TEST_CASE("SeedSessionLayout: the pre-S4 file first, then the default named layout, then the exe-dir imgui.ini", "[settings-ui][editor]")
{
    const std::filesystem::path root = Fresh("s4-seed");
    const LayoutLibrary named(root / "Layouts");
    std::string error;
    REQUIRE(named.Save("Studio", "[studio]\n", &error));
    { std::ofstream(root / "old.ini") << "[old]\n"; std::ofstream(root / "imgui.ini") << "[legacy]\n"; }
    const std::filesystem::path target = root / "Layouts" / "Session" / "k.ini";
    CHECK(SeedSessionLayout(target, root / "old.ini", named, "Studio", root / "imgui.ini") == LayoutSeed::PreS4File);
    CHECK(Read(target) == "[old]\n");
    CHECK(std::filesystem::exists(root / "old.ini"));                 // copied, never moved
    std::filesystem::remove(target);
    CHECK(SeedSessionLayout(target, root / "absent.ini", named, "Studio", root / "imgui.ini") == LayoutSeed::NamedDefault);
    CHECK(Read(target) == "[studio]\n");
    std::filesystem::remove(target);
    CHECK(SeedSessionLayout(target, root / "absent.ini", named, "", root / "imgui.ini") == LayoutSeed::LegacyExeIni);
    std::filesystem::remove(target);
    CHECK(SeedSessionLayout(target, root / "absent.ini", named, "Gone", root / "absent2.ini") == LayoutSeed::None);
    std::filesystem::remove_all(root);
}

TEST_CASE("openPanelsAtStart: * is every panel (today's default); a list hides the rest; round trip", "[settings-ui][editor]")
{
    const PanelVisibility all = ParseOpenPanels("*");
    for (const PanelInfo& p : kPanels) CHECK(all.IsVisible(p.id));
    CHECK(FormatOpenPanels(all) == "*");
    const PanelVisibility some = ParseOpenPanels(" outliner , Console,Nope ");
    CHECK(some.IsVisible(PanelId::Outliner));
    CHECK(some.IsVisible(PanelId::Console));
    CHECK(some.IsVisible(PanelId::Viewport));            // permanent
    CHECK_FALSE(some.IsVisible(PanelId::AssetGraph));
    CHECK(FormatOpenPanels(some) == "Outliner,Console");
    CHECK(cvar_layoutOpenPanelsAtStart.Get() == "*");
    CHECK(cvar_layoutDefault.Get().empty());
}
