#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/Config.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Assets/Assets.hpp>

#include <catch2/catch_test_macros.hpp>

#include "Helpers/TestTypeContext.hpp"

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

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
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

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.OpenProject(dir) == false);
    REQUIRE(rt.CurrentProject() == nullptr);   // state untouched

    fs::remove_all(dir, ec);
}

TEST_CASE("Runtime::OpenProject sets the Assets content root", "[project]")
{
    const fs::path dir = MakeTempDir("content");
    REQUIRE(Arcane::Project::Create(dir / "Game", "G").has_value());
    WriteFile(dir / "Game" / "Content" / "probe.json", R"({"ok":true})");

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
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

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
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

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.OpenProject(dir / "A") == true);
    REQUIRE(rt.CurrentProject()->Manifest().name == "Alpha");
    REQUIRE(rt.OpenProject(dir / "B") == true);
    REQUIRE(rt.CurrentProject()->Manifest().name == "Beta");

    std::error_code ec; fs::remove_all(dir, ec);
}

// Task 9 (async-boot-corestages, project-switch overlay): the editor's
// SwitchProject failure fallback calls Runtime::CloseProject() to converge on
// the SAME project-less state the boot path itself uses when there is no
// project -- not a second, ad hoc definition. This pins that CloseProject()
// actually discards all three things OpenProject installed (the Project
// itself, the Assets content root, and the Config plugin/project layers),
// not just CurrentProject().
TEST_CASE("Runtime::CloseProject returns to the fresh-Runtime no-project state", "[project]")
{
    const fs::path dir = MakeTempDir("close");
    const std::string abi = std::to_string(static_cast<int>(Arcane::kGamePluginABIVersion));

    // Project A: a real project with a Content/ asset resolvable by relative
    // path, and one enabled plugin contributing a Config/ layer -- so
    // CloseProject has all three things (project, content root, config
    // layers) to actually discard, not just the trivial "nothing was ever
    // opened" case.
    WriteFile(dir / "A.arcproj",
        std::string(R"({"formatVersion":1,"name":"A","engine":{"abi":)") + abi +
        R"(},"gameModule":"","plugins":[{"name":"fx","enabled":true}],"bootScene":""})");
    WriteFile(dir / "Content" / "close_project_probe.json", R"({"ok":true})");
    WriteFile(dir / "Plugins" / "fx" / "fx.arcplugin",
        std::string(R"({"formatVersion":1,"name":"fx","engine":{"abi":)") + abi +
        R"(},"gameModule":"","plugins":[],"bootScene":""})");
    WriteFile(dir / "Plugins" / "fx" / "Config" / "game.json", R"({"fromPlugin":true})");

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.OpenProject(dir) == true);
    REQUIRE(rt.CurrentProject() != nullptr);
    REQUIRE(rt.AssetsFacade().GetJson("close_project_probe.json") != nullptr);
    REQUIRE(rt.Configuration().Category("game").value("fromPlugin", false) == true);

    rt.CloseProject();

    // No stale project reference.
    REQUIRE(rt.CurrentProject() == nullptr);
    // Content root cleared -- the SAME relative path no longer resolves under
    // the torn-down project's Content/ (ResolveAssetPath falls back to
    // exe-relative, which has no such file, and the cache key changes with
    // the resolved path so this cannot be a stale hit).
    REQUIRE(rt.AssetsFacade().GetJson("close_project_probe.json") == nullptr);
    // Config rebuilt from engine defaults only -- the plugin/project layer is
    // gone, not merely shadowed by nothing.
    REQUIRE(rt.Configuration().Category("game").value("fromPlugin", false) == false);

    // Reopening a DIFFERENT project afterward behaves exactly like a fresh
    // Runtime's first open -- CloseProject must not leave any residue that
    // could bleed into (or block) the next OpenProject.
    REQUIRE(Arcane::Project::Create(dir / "B", "Beta").has_value());
    REQUIRE(rt.OpenProject(dir / "B") == true);
    REQUIRE(rt.CurrentProject()->Manifest().name == "Beta");

    std::error_code ec; fs::remove_all(dir, ec);
}
