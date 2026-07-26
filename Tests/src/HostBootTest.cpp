#include <LoomConfig.hpp>
#include <ProjectBoot.hpp>

#include <Arcane/Base/Engine.hpp>        // ExecutablePathUtf8 (the argv[0] replacement)
#include <Arcane/Config/Config.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // kGamePluginABIVersion (the probe tripwire)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Input/InputActions.hpp>

#include <Json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
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
// shader-editor arc's ABI LESSON.
//
// WHAT THIS SUITE CANNOT DO (corrected after the 2026-07-26 review found two
// reviewers flagging it independently): it does NOT catch an ABI bump.
// EngineInfoJson is an inline header function reading kGamePluginABIVersion and
// the assertion below compares against kGamePluginABIVersion, so both sides
// expand to the same constant in this one TU -- bumping it moves both and the
// test stays green by construction.
//
// What it DOES catch: a refactor that hardcodes a literal instead of the
// constant, a renamed/dropped JSON key, a type change, a payload that stops
// being one parseable line. What it structurally CANNOT catch is a STALE BUILT
// BINARY, which is the failure the design actually cares about -- only spawning
// the built exe and parsing its stdout catches that, which is what the Hub does
// at registration time.

TEST_CASE("EngineInfoJson sources the ABI from the engine constant", "[loom]")
{
    const std::string s = Arcane::HostBoot::EngineInfoJson("C:/some/ArcaneEditor.exe");
    const nlohmann::json j = nlohmann::json::parse(s);

    REQUIRE(j.contains("engineAbi"));
    REQUIRE(j["engineAbi"].is_number_unsigned());
    // Pins the key, the type, and the fact that the value comes from the constant
    // rather than a literal. NOT an ABI-bump tripwire -- see the note above.
    CHECK(j["engineAbi"].get<std::uint32_t>() == Arcane::kGamePluginABIVersion);
}

TEST_CASE("EngineInfoJson carries build + exe path and parses cleanly", "[loom]")
{
    // Fed BACKSLASHES on purpose: the previous fixture passed an
    // already-forward-slashed path, so the normalisation assertion below held
    // trivially and could not have failed. This is the Win32 shape
    // Arcane::ExecutablePathUtf8 actually produces before normalisation.
    const std::string s = Arcane::HostBoot::EngineInfoJson("C:\\some dir\\ArcaneEditor.exe");
    const nlohmann::json j = nlohmann::json::parse(s);   // must not throw

    REQUIRE(j.contains("build"));
    CHECK(j["build"].is_string());
    CHECK_FALSE(j["build"].get<std::string>().empty());

    REQUIRE(j.contains("exePath"));
    // Forward slashes so the Hub never has to unescape backslashes out of JSON.
    CHECK(j["exePath"].get<std::string>() == "C:/some dir/ArcaneEditor.exe");
}

TEST_CASE("EngineInfoJson survives a non-ASCII install path", "[loom]")
{
    // Regression for MAJOR 5 of the 2026-07-26 review. The probe used to be fed
    // argv[0], which MSVC hands to main() in the ACTIVE ANSI CODEPAGE. Under an
    // install path like C:\Users\Jose\... those bytes are not valid UTF-8, and
    // nlohmann's dump() is strict-UTF-8 with no JSON_NOEXCEPTION -- it threw
    // type_error.316 straight out of main and terminated. The probe failing in
    // exactly the way it exists to prevent.
    //
    // Two belts are under test: callers now pass Arcane::ExecutablePathUtf8()
    // (well-formed UTF-8 from the wide Win32 path), and dump() runs with
    // error_handler_t::replace so even malformed input degrades instead of
    // throwing.
    SECTION("well-formed UTF-8 round-trips byte for byte")
    {
        // "C:/Users/José/Arcane/ArcaneEditor.exe" -- e-acute as UTF-8 (C3 A9).
        const std::string path = "C:/Users/Jos\xC3\xA9/Arcane/ArcaneEditor.exe";
        std::string s;
        REQUIRE_NOTHROW(s = Arcane::HostBoot::EngineInfoJson(path));
        const nlohmann::json j = nlohmann::json::parse(s);
        CHECK(j["exePath"].get<std::string>() == path);
        CHECK(s.find('\n') == std::string::npos);
    }

    SECTION("a raw ANSI byte does not throw out of the probe")
    {
        // 0xE9 alone is e-acute in CP-1252 and an INVALID UTF-8 lead byte -- i.e.
        // literally what argv[0] used to hand over. Must degrade, never throw.
        const std::string ansi = "C:/Users/Jos\xE9/ArcaneEditor.exe";
        std::string s;
        REQUIRE_NOTHROW(s = Arcane::HostBoot::EngineInfoJson(ansi));
        REQUIRE_NOTHROW(nlohmann::json::parse(s));   // still valid JSON
        CHECK(s.find('\n') == std::string::npos);
    }
}

TEST_CASE("ExecutablePathUtf8 reports this test exe, absolute and forward-slashed", "[loom]")
{
    // The replacement for argv[0]. Asserted against the real running process, so
    // it also proves the export crosses the Arcane.dll boundary.
    const std::string exe = Arcane::ExecutablePathUtf8();
    REQUIRE_FALSE(exe.empty());
    CHECK(exe.find('\\') == std::string::npos);        // normalised
    CHECK(std::filesystem::path(exe).is_absolute());   // NOT a bare argv[0] name
    // Case-insensitively ends with the test exe's name (Win32 paths vary in case).
    std::string lower = exe;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    CHECK(lower.find("arcanetests.exe") != std::string::npos);
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
