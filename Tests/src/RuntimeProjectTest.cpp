#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/Config.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Assets/Assets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
    namespace fs = std::filesystem;

    fs::path MakeTempDir(const char* tag)
    {
        std::error_code ec;
        fs::path d = fs::temp_directory_path() / (std::string("arcane_proj_") + tag);
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        return d;
    }

    void WriteFile(const fs::path& p, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary) << text;
    }
}

TEST_CASE("Runtime::OpenProject adopts a valid project", "[project]")
{
    const fs::path dir = MakeTempDir("valid");
    REQUIRE(Arcane::Project::Create(dir / "Game", "MyGame").has_value());  // abi == this engine

    Arcane::Runtime rt;
    REQUIRE(rt.CurrentProject() == nullptr);
    REQUIRE(rt.OpenProject(dir / "Game") == true);
    REQUIRE(rt.CurrentProject() != nullptr);
    REQUIRE(rt.CurrentProject()->Manifest().name == "MyGame");

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("Runtime::OpenProject refuses a mismatched engine ABI", "[project]")
{
    const fs::path dir = MakeTempDir("badabi");
    WriteFile(dir / "Bad.arcproj",
        R"({"formatVersion":1,"name":"Bad","engine":{"abi":9999},)"
        R"("gameModule":"","plugins":[],"bootScene":""})");
    std::error_code ec; fs::create_directories(dir / "Content", ec);

    Arcane::Runtime rt;
    REQUIRE(rt.OpenProject(dir) == false);
    REQUIRE(rt.CurrentProject() == nullptr);   // state untouched

    fs::remove_all(dir, ec);
}

TEST_CASE("Runtime::OpenProject sets the Assets content root", "[project]")
{
    const fs::path dir = MakeTempDir("content");
    REQUIRE(Arcane::Project::Create(dir / "Game", "G").has_value());
    WriteFile(dir / "Game" / "Content" / "probe.json", R"({"ok":true})");

    Arcane::Runtime rt;
    REQUIRE(rt.OpenProject(dir / "Game") == true);
    auto doc = rt.AssetsFacade().GetJson("probe.json");   // relative -> under Content/
    REQUIRE(doc != nullptr);
    REQUIRE((*doc)["ok"].get<bool>() == true);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("Runtime::OpenProject layers an enabled plugin's Config under the project", "[project]")
{
    const fs::path dir = MakeTempDir("plugincfg");
    const std::string abi = std::to_string(static_cast<int>(Arcane::kGamePluginABIVersion));

    // A project enabling one plugin "fx".
    WriteFile(dir / "Game.arcproj",
        std::string(R"({"formatVersion":1,"name":"G","engine":{"abi":)") + abi +
        R"(},"gameModule":"","plugins":[{"name":"fx","enabled":true}],"bootScene":""})");
    std::error_code ec; fs::create_directories(dir / "Content", ec);

    // Plugin fx: a valid descriptor + a Config/game.json contributing two keys.
    WriteFile(dir / "Plugins" / "fx" / "fx.arcplugin",
        std::string(R"({"formatVersion":1,"name":"fx","engine":{"abi":)") + abi +
        R"(},"gameModule":"","plugins":[],"bootScene":""})");
    WriteFile(dir / "Plugins" / "fx" / "Config" / "game.json",
        R"({"fromPlugin":true,"shared":"plugin"})");

    // The project's own Config/game.json overrides the shared key (project beats plugin).
    WriteFile(dir / "Config" / "game.json", R"({"shared":"project"})");

    Arcane::Runtime rt;
    REQUIRE(rt.OpenProject(dir) == true);
    REQUIRE(rt.CurrentProject()->ActivePluginRoots().size() == 1);

    const nlohmann::json& game = rt.Configuration().Category("game");
    CHECK(game.value("fromPlugin", false) == true);            // plugin-only key survives
    CHECK(game.value("shared", std::string()) == "project");   // engine->plugin->project: project wins

    fs::remove_all(dir, ec);
}

TEST_CASE("Runtime::OpenProject switches projects on re-open", "[project]")
{
    const fs::path dir = MakeTempDir("switch");
    REQUIRE(Arcane::Project::Create(dir / "A", "Alpha").has_value());
    REQUIRE(Arcane::Project::Create(dir / "B", "Beta").has_value());

    Arcane::Runtime rt;
    REQUIRE(rt.OpenProject(dir / "A") == true);
    REQUIRE(rt.CurrentProject()->Manifest().name == "Alpha");
    REQUIRE(rt.OpenProject(dir / "B") == true);
    REQUIRE(rt.CurrentProject()->Manifest().name == "Beta");

    std::error_code ec; fs::remove_all(dir, ec);
}
