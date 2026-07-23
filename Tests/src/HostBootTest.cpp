#include <LoomConfig.hpp>
#include <ProjectBoot.hpp>

#include <Arcane/Config/Config.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Input/InputActions.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>

namespace { namespace fs = std::filesystem; }

TEST_CASE("LoomConfig parses --project", "[loom]")
{
    const char* argv[] = { "loom", "--project", "MyGame" };
    auto out = LoomConfig::Parse(3, const_cast<char**>(argv));
    REQUIRE(out.config.has_value());
    REQUIRE(out.config->projectPath == "MyGame");
}

TEST_CASE("LoomConfig defaults --project to empty", "[loom]")
{
    const char* argv[] = { "loom" };
    auto out = LoomConfig::Parse(1, const_cast<char**>(argv));
    REQUIRE(out.config.has_value());
    REQUIRE(out.config->projectPath.empty());
}

TEST_CASE("HostBoot::GameModule falls back with no/empty gameModule", "[loom]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_hostboot_gm";
    std::error_code ec; fs::remove_all(dir, ec);
    REQUIRE(Arcane::Project::Create(dir, "G").has_value());   // Create writes gameModule ""
    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    REQUIRE(Arcane::HostBoot::GameModule(&*proj, "Sandbox.dll") == "Sandbox.dll");
    REQUIRE(Arcane::HostBoot::GameModule(nullptr, "Sandbox.dll") == "Sandbox.dll");
    // Empty fallback + no project -> empty: the editor's "start with no game" signal
    // (bare ArcaneEditor leaves pluginPath empty, so nothing is loaded).
    REQUIRE(Arcane::HostBoot::GameModule(nullptr, "").empty());
    REQUIRE(Arcane::HostBoot::GameModule(&*proj, "").empty());   // content-only project, no --plugin
    fs::remove_all(dir, ec);
}

TEST_CASE("HostBoot::GameModule returns the manifest gameModule when set", "[loom]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_hostboot_gm2";
    std::error_code ec; fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);
    std::ofstream(dir / "P.arcproj", std::ios::binary) <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":5},)"
        R"("gameModule":"Foo.dll","plugins":[],"bootScene":""})";
    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    REQUIRE(Arcane::HostBoot::GameModule(&*proj, "Sandbox.dll") == "Foo.dll");
    fs::remove_all(dir, ec);
}

TEST_CASE("HostBoot::GameModule resolves the project's Binaries/ copy when built", "[loom]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_hostboot_gm3";
    std::error_code ec; fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);
    fs::create_directories(dir / "Binaries", ec);
    std::ofstream(dir / "P.arcproj", std::ios::binary) <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":5},)"
        R"("gameModule":"Aphelyon.dll","plugins":[],"bootScene":""})";
    // The project has built its own module -> the host must load THIS copy, not a
    // same-named DLL beside the exe.
    std::ofstream(dir / "Binaries" / "Aphelyon.dll", std::ios::binary) << "MZ";  // presence is what matters

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    REQUIRE(fs::path(Arcane::HostBoot::GameModule(&*proj, "Sandbox.dll"))
            == dir / "Binaries" / "Aphelyon.dll");

    // Without the built copy, it stays a bare name (borrowing path, resolved beside exe).
    fs::remove(dir / "Binaries" / "Aphelyon.dll", ec);
    REQUIRE(Arcane::HostBoot::GameModule(&*proj, "Sandbox.dll") == "Aphelyon.dll");
    fs::remove_all(dir, ec);
}

TEST_CASE("HostBoot::LoadInputConfig loads the input category", "[loom]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_hostboot_input";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir, ec);
    std::ofstream(dir / "input.json", std::ios::binary) <<
        R"({"actionMaps":[{"name":"demo","actions":[{"name":"quit","type":"Button",)"
        R"("bindings":[{"path":"<Keyboard>/escape"}]}]}]})";

    Arcane::Config config;
    config.LoadEngineDefaults(dir);   // temp dir stands in as the engine-default config layer

    auto input = Arcane::InputActions::Create();
    REQUIRE(Arcane::HostBoot::LoadInputConfig(*input, config) == true);
    REQUIRE(input->ActiveContext() == "demo");
    fs::remove_all(dir, ec);
}
