#include <LoomConfig.hpp>
#include <ProjectBoot.hpp>

#include <Arcane/Config/Config.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // kGamePluginABIVersion (the probe tripwire)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Input/InputActions.hpp>

#include <Json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
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

// --- Engine probe (Arcane Hub slice 1) -----------------------------------------
// The Hub stamps `engine.abi` into every .arcproj it creates. If that number is
// ever sourced from anywhere but the engine itself, New Project silently mints
// stale-ABI projects and they crash on open -- the exact failure recorded in the
// shader-editor arc's ABI LESSON. This suite is the tripwire that keeps the probe
// pinned to the real constant.

TEST_CASE("EngineInfoJson reports the engine's real plugin ABI", "[loom]")
{
    const std::string s = Arcane::HostBoot::EngineInfoJson("C:/some/ArcaneEditor.exe");
    const nlohmann::json j = nlohmann::json::parse(s);

    REQUIRE(j.contains("engineAbi"));
    REQUIRE(j["engineAbi"].is_number_unsigned());
    // THE assertion: bump kGamePluginABIVersion without the probe following and
    // this fails.
    CHECK(j["engineAbi"].get<std::uint32_t>() == Arcane::kGamePluginABIVersion);
}

TEST_CASE("EngineInfoJson carries build + exe path and parses cleanly", "[loom]")
{
    const std::string s = Arcane::HostBoot::EngineInfoJson("C:/some dir/ArcaneEditor.exe");
    const nlohmann::json j = nlohmann::json::parse(s);   // must not throw

    REQUIRE(j.contains("build"));
    CHECK(j["build"].is_string());
    CHECK_FALSE(j["build"].get<std::string>().empty());

    REQUIRE(j.contains("exePath"));
    // Forward slashes: generic_string(), so the Hub never has to unescape
    // backslashes out of JSON.
    CHECK(j["exePath"].get<std::string>() == "C:/some dir/ArcaneEditor.exe");
}

TEST_CASE("EngineInfoJson is a single line", "[loom]")
{
    // The Hub reads one line from stdout. A pretty-printed payload would make it
    // guess where the object ends.
    const std::string s = Arcane::HostBoot::EngineInfoJson("x.exe");
    CHECK(s.find('\n') == std::string::npos);
}

TEST_CASE("LoomConfig parses --print-engine-info and defaults it off", "[loom]")
{
    {
        const char* argv[] = { "ArcaneEditor.exe" };
        auto out = LoomConfig::Parse(1, const_cast<char**>(argv));
        REQUIRE(out.config.has_value());
        CHECK_FALSE(out.config->printEngineInfo);
    }
    {
        const char* argv[] = { "ArcaneEditor.exe", "--print-engine-info" };
        auto out = LoomConfig::Parse(2, const_cast<char**>(argv));
        REQUIRE(out.config.has_value());
        CHECK(out.config->printEngineInfo);
    }
}
