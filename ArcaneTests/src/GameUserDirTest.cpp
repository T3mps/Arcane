// Settings arc S7 (spec s8.2, s11.0): where a shipped game keeps the player's
// settings. Dev and the editor: <project>/Saved (unchanged). Dist:
// %LOCALAPPDATA%/<Company>/<Game> on Windows, $XDG_CONFIG_HOME (else
// ~/.config)/<Company>/<Game> elsewhere. Company and game come from the project.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/PlayerSettings.hpp>
#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectManifest.hpp>
#include <Arcane/Project/ProjectPaths.hpp>

#include <Json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "Helpers/TestTypeContext.hpp"

namespace fs = std::filesystem;
using namespace Arcane;

namespace
{
    Paths::Config DistConfig(std::string company, std::string game)
    {
        Paths::Config c;
        c.engineDir = "E:/engine";
        c.projectDir = fs::path("P:/proj");
        c.companyName = std::move(company);
        c.gameName = std::move(game);
        c.dist = true;
        return c;
    }

    fs::path MakeProbeProject(const std::string& company)
    {
        const fs::path root = fs::temp_directory_path() / "S7PathsProbe";
        fs::remove_all(root);
        fs::create_directories(root / "Content");
        fs::create_directories(root / "Config");
        std::ofstream(root / "S7PathsProbe.arcproj")
            << R"({"formatVersion":2,"name":"S7PathsProbe","company":")" << company
            << R"(","engine":{"abi":)" << static_cast<int>(kGamePluginABIVersion) << "}}";
        return root;
    }
}

TEST_CASE("GameUserDir in Dist: %LOCALAPPDATA%/<Company>/<Game> on Windows; no company, no segment", "[paths]")
{
    Paths::PlatformDirs dirs;
    dirs.localAppData = "C:/Users/p/AppData/Local";
    CHECK(Paths::ResolveGameUserDir(DistConfig("Starworks", "Aphelyon"), Paths::HostPlatform::Windows, dirs)
          == fs::path("C:/Users/p/AppData/Local/Starworks/Aphelyon"));
    CHECK(Paths::ResolveGameUserDir(DistConfig("", "Aphelyon"), Paths::HostPlatform::Windows, dirs)
          == fs::path("C:/Users/p/AppData/Local/Aphelyon"));
    CHECK(Paths::ResolveGameUserDir(DistConfig("Starworks", "Aphelyon"), Paths::HostPlatform::Windows, {}).empty());
}

TEST_CASE("GameUserDir in Dist on Linux follows XDG: an absolute XDG_CONFIG_HOME, else ~/.config; a relative one is ignored", "[paths]")
{
    Paths::PlatformDirs dirs;
    dirs.home = "/home/p";
    dirs.xdgConfigHome = "/home/p/.cfg";
    CHECK(Paths::ResolveGameUserDir(DistConfig("Starworks", "Aphelyon"), Paths::HostPlatform::Posix, dirs)
          == fs::path("/home/p/.cfg/Starworks/Aphelyon"));
    dirs.xdgConfigHome = "relative/cfg";
    CHECK(Paths::ResolveGameUserDir(DistConfig("Starworks", "Aphelyon"), Paths::HostPlatform::Posix, dirs)
          == fs::path("/home/p/.config/Starworks/Aphelyon"));
    dirs.xdgConfigHome.clear();
    CHECK(Paths::ResolveGameUserDir(DistConfig("Starworks", "Aphelyon"), Paths::HostPlatform::Posix, dirs)
          == fs::path("/home/p/.config/Starworks/Aphelyon"));
}

TEST_CASE("GameUserDir outside Dist is <project>/Saved, and empty with no project", "[paths]")
{
    Paths::Config c = DistConfig("Starworks", "Aphelyon");
    c.dist = false;
    CHECK(Paths::ResolveGameUserDir(c, Paths::HostPlatform::Windows, {}) == fs::path("P:/proj/Saved"));
    c.projectDir.reset();
    CHECK(Paths::ResolveGameUserDir(c, Paths::HostPlatform::Windows, {}).empty());
}

TEST_CASE("SanitizePathSegment keeps a name a folder can carry", "[paths]")
{
    CHECK(Paths::SanitizePathSegment("Starworks") == "Starworks");
    CHECK(Paths::SanitizePathSegment("Star:works/<1>?") == "Star_works__1__");
    CHECK(Paths::SanitizePathSegment("Game. ") == "Game");
    CHECK(Paths::SanitizePathSegment("CON") == "_CON");
    CHECK(Paths::SanitizePathSegment("..").empty());
    Paths::Config c = DistConfig("", "");
    Paths::PlatformDirs dirs;
    dirs.localAppData = "C:/L";
    CHECK(Paths::ResolveGameUserDir(c, Paths::HostPlatform::Windows, dirs) == fs::path("C:/L/ArcaneGame"));
}

TEST_CASE("ProjectManifest: company is optional identity; absent reads as empty", "[paths]")
{
    const auto with = ProjectManifest::FromJson(nlohmann::json::parse(
        R"({"formatVersion":2,"name":"G","company":"Starworks","engine":{"abi":1}})"));
    REQUIRE(with);
    CHECK(with->company == "Starworks");
    const auto without = ProjectManifest::FromJson(nlohmann::json::parse(
        R"({"formatVersion":2,"name":"G","engine":{"abi":1}})"));
    REQUIRE(without);
    CHECK(without->company.empty());
    CHECK_FALSE(ProjectManifest::FromJson(nlohmann::json::parse(
        R"({"formatVersion":2,"name":"G","company":7,"engine":{"abi":1}})")).has_value());
}

TEST_CASE("PathsConfigFor takes company and game from the project", "[paths]")
{
    const fs::path root = MakeProbeProject("Starworks QA");
    auto project = Project::Open(root);
    REQUIRE(project);
    const Paths::Config c = PathsConfigFor(*project, "E:/engine", true);
    CHECK(c.companyName == "Starworks QA");
    CHECK(c.gameName == "S7PathsProbe");
    REQUIRE(c.projectDir);
    CHECK(fs::equivalent(*c.projectDir, root));
    CHECK(c.dist);
    CHECK(c.engineDir == fs::path("E:/engine"));
    project.reset();
    fs::remove_all(root);
}

TEST_CASE("Runtime: the User rung is GameUserDir/Config -- a player's PlayerSafe choice lands there on save", "[paths]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    CVarDesc d;
    d.name = "test.s7paths.persisted";
    d.type = CVarType::Int32;
    d.defaultValue = CVarValue::Int32(1);
    d.flags = CVarFlags::Archive;
    d.help = "S7 paths probe.";
    d.module = "test-s7-paths";
    d.audience = Audience::PlayerSafe;
    REQUIRE_FALSE(reg.Register(d).IsStale());

    const fs::path root = MakeProbeProject("Starworks QA");
    {
        Runtime runtime(Test::Process());
        REQUIRE(runtime.OpenProject(root));
#if defined(ARC_BUILD_DIST)
        CHECK(Paths::Get(Paths::Location::GameUserDir)
              == Paths::ResolveGameUserDir(PathsConfigFor(*runtime.CurrentProject(), "", true),
                                           Paths::kHostPlatform, Paths::CurrentPlatformDirs()));
#else
        CHECK(Paths::Get(Paths::Location::GameUserDir) == root / "Saved");
        runtime.SetUserCVarArchiving(true);
        REQUIRE(PlayerSettings::Set(reg, "test.s7paths.persisted", CVarValue::Int32(7), CVarContext::LocalHost)
                == SetResult::Applied);
        reg.PublishImmediate();
        REQUIRE(runtime.SaveUserCVars());
        std::ifstream in(root / "Saved" / "Config" / "test.json", std::ios::binary);
        REQUIRE(in.good());
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("persisted") != std::string::npos);
        CHECK(text.find('7') != std::string::npos);
        runtime.SetUserCVarArchiving(false);
#endif
        runtime.CloseProject();
        CHECK(Paths::Get(Paths::Location::GameUserDir).empty() == !kDistBuild);   // dev: no project, no dir
    }
    reg.UnregisterModule("test-s7-paths");
    fs::remove_all(root);
}

// S7-GATE (the S7-9 deferred minor): a `dist` a host or test preset survives
// OpenProject and CloseProject, as it survives the EngineConfig rung
// (ApplyEngineDirDefaults). Otherwise a preset run splits the User rung across
// two folders in one boot: HostBoot's early rung under the per-user OS dir
// (Paths::ForProject(Current())), the project's live rung under Saved/Config.
// Nothing is saved, so nothing is written under the per-user dir.
TEST_CASE("Runtime keeps a preset Paths dist across OpenProject and CloseProject; the User rung follows Paths", "[paths]")
{
    const Paths::Config saved = Paths::Current();
    struct RestorePaths
    {
        const Paths::Config& config;
        ~RestorePaths() { Paths::Configure(config); }
    } restore{ saved };

    Paths::Config preset = saved;
    preset.dist = true;
    Paths::Configure(preset);

    const fs::path root = MakeProbeProject("Starworks QA");
    {
        Runtime runtime(Test::Process());
        REQUIRE(runtime.OpenProject(root));
        CHECK(Paths::Current().dist);

        const fs::path userDir = Paths::Get(Paths::Location::GameUserDir);
        CHECK_FALSE(userDir.empty());
        CHECK(userDir != root / "Saved");                                       // the per-user OS dir, not Saved/
        const LayerSources layers = runtime.CVarLayerSources();
        const auto user = std::find_if(layers.dirs.begin(), layers.dirs.end(),
                                       [](const CVarLayerDir& d) { return d.by == SetBy::User; });
        REQUIRE(user != layers.dirs.end());
        CHECK(user->dir == userDir / "Config");                                 // one User folder per boot

        runtime.CloseProject();
        CHECK(Paths::Current().dist);
    }
    fs::remove_all(root);
}
