// ModuleBuild's PURE halves: solution discovery, the SDK-root walk, and the
// composed premake+msbuild command line ([editor]). The Runner and the
// resolution probes (vswhere, premake, _wpopen) spawn processes and are
// desk-verify territory -- the same "no spawn test" split RuntimeLaunch's
// SpawnDetached already draws.

#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <Project/ModuleBuild.hpp>

namespace
{
    namespace fs = std::filesystem;
    using namespace Arcane::Editor;

    // Unique temp dir per SECTION run; removed by the guard so a failing
    // assertion cannot strand files for the next run to trip on.
    struct TempDir
    {
        fs::path path;
        explicit TempDir(const char* tag)
        {
            path = fs::temp_directory_path() /
                   (std::string("arcane_modulebuild_") + tag + "_" +
                    std::to_string(static_cast<unsigned>(
                        std::hash<const void*>{}(this))));
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    void Touch(const fs::path& p)
    {
        std::ofstream(p.string()) << "x";
    }
}

TEST_CASE("DiscoverSolution prefers .slnx over .sln, first lexicographic within a bucket",
          "[editor]")
{
    TempDir dir("discover");
    Touch(dir.path / "Zeta.sln");
    Touch(dir.path / "beta.slnx");
    Touch(dir.path / "Alpha.slnx");
    Touch(dir.path / "notes.txt");

    const fs::path found = ModuleBuild::DiscoverSolution(dir.path);
    CHECK(found.filename() == "Alpha.slnx");
}

TEST_CASE("DiscoverSolution falls back to .sln, and to empty when neither exists",
          "[editor]")
{
    TempDir dir("fallback");
    SECTION("only a .sln")
    {
        Touch(dir.path / "Game.sln");
        CHECK(ModuleBuild::DiscoverSolution(dir.path).filename() == "Game.sln");
    }
    SECTION("neither")
    {
        CHECK(ModuleBuild::DiscoverSolution(dir.path).empty());
    }
    SECTION("a directory that does not exist")
    {
        CHECK(ModuleBuild::DiscoverSolution(dir.path / "missing").empty());
    }
}

TEST_CASE("SdkRootFromExeDir inverts the bin/<cfg>/<project> targetdir rule", "[editor]")
{
    CHECK(ModuleBuild::SdkRootFromExeDir(
              "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneEditor") ==
          fs::path("D:/dev/starworks/Gacha/Arcane"));
    // A trailing separator must not eat one of the three parent steps.
    CHECK(ModuleBuild::SdkRootFromExeDir(
              "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneEditor/") ==
          fs::path("D:/dev/starworks/Gacha/Arcane"));
}

TEST_CASE("ComposeRebuildCommands is one premake-first cmd chain with folded stderr",
          "[editor]")
{
    ModuleBuild::ComposeInputs in;
    in.projectRoot   = "D:/dev/starworks/Aphelyon";
    in.premakeExe    = "D:/dev/starworks/ThirdParty/premake5/premake5.exe";
    in.msbuildExe    = "C:/Program Files/Microsoft Visual Studio/18/MSBuild.exe";
    in.solution      = "D:/dev/starworks/Aphelyon/Aphelyon.slnx";
    in.configuration = "Debug";

    const std::string cmd = ModuleBuild::ComposeRebuildCommands(in);

    // cd into the project root first (premake reads ./premake5.lua from cwd).
    const std::size_t cdPos      = cmd.find("cd /d \"D:/dev/starworks/Aphelyon\"");
    const std::size_t premakePos = cmd.find("premake5.exe\" vs2026");
    const std::size_t msbuildPos = cmd.find("MSBuild.exe\"");
    REQUIRE(cdPos != std::string::npos);
    REQUIRE(premakePos != std::string::npos);
    REQUIRE(msbuildPos != std::string::npos);
    // Premake FIRST, every build (the arc's stale-.sln decision), then msbuild.
    CHECK(cdPos < premakePos);
    CHECK(premakePos < msbuildPos);

    // The msbuild half names the solution and the configuration.
    CHECK(cmd.find("\"D:/dev/starworks/Aphelyon/Aphelyon.slnx\"") != std::string::npos);
    CHECK(cmd.find("/p:Configuration=Debug") != std::string::npos);
    CHECK(cmd.find("/m /nologo") != std::string::npos);

    // A space-laden exe path survives inside quotes.
    CHECK(cmd.find("\"C:/Program Files/Microsoft Visual Studio/18/MSBuild.exe\"") !=
          std::string::npos);

    // THE LINK IS FORCED, ALWAYS -- and this is the assertion, not a detail.
    //
    // A game project's Binaries\ is a SINGLE SHARED SLOT: Debug and Release
    // write the same <Project>.dll there, while their object trees live apart
    // under Intermediate\<Config>\. So when a Release DLL is sitting in
    // Binaries\ and the editor (a Debug build) asks for a Debug module, MSBuild
    // compares the Debug objects against the Debug link stamp, finds both
    // current, relinks NOTHING, and reports success in a fraction of a second --
    // leaving the WRONG-CONFIG DLL in place for the host to load and refuse.
    // Observed live at NRI Phase 5a, desk checkpoint D5a-1: a 0.24s "All
    // outputs are up-to-date" immediately followed by "the rebuilt module still
    // failed to load".
    //
    // An incremental build therefore CANNOT be trusted to heal a cross-config
    // module -- the state it reasons about is per-config, the artifact it
    // guards is not. The worker must force the link every time; a few seconds
    // per rebuild is the whole cost, and Rebuild Game Module is a deliberate
    // user action, not a hot loop.
    CHECK(cmd.find("/t:Rebuild") != std::string::npos);

    // Parenthesized so the trailing 2>&1 folds EVERY member's stderr into the
    // captured stdout -- unparenthesized it would bind to msbuild alone.
    CHECK(cmd.front() == '(');
    CHECK(cmd.rfind(") 2>&1") == cmd.size() - 6);
}

TEST_CASE("Configuration matches the editor's own build flavor", "[editor]")
{
#ifdef _DEBUG
    CHECK(std::string(ModuleBuild::Configuration()) == "Debug");
#else
    CHECK(std::string(ModuleBuild::Configuration()) == "Release");
#endif
}
