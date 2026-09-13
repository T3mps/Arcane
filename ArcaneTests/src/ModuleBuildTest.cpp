// ModuleBuild's PURE halves ([editor]) after the arcbuild driver arc: the
// SDK-root walk, the driver-exe candidate list, and THE ONE COMMAND LINE the
// Runner now executes -- arcbuild itself. Composition of premake/msbuild
// lines, solution discovery and the s4.3 rule left the editor for the
// driver (BuildDriverTest.cpp, ToolchainTest.cpp -- [build]). The Runner and
// RunCapture spawn processes and stay desk-verify territory.

#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <Project/ModuleBuild.hpp>

namespace
{
    namespace fs = std::filesystem;
    using namespace Arcane::Editor;

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
        fs::create_directories(p.parent_path());
        std::ofstream(p.string()) << "x";
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

TEST_CASE("DriverCandidates: packaged beside the editor first, dev bin layout second", "[editor]")
{
    // The RuntimeLaunch::ExeCandidates rule, applied to arcbuild.exe.
    const auto c = ModuleBuild::DriverCandidates("D:/sdk/bin/Debug-windows-x86_64-md/ArcaneEditor");
    REQUIRE(c.size() == 2);
    CHECK(c[0] == fs::path("D:/sdk/bin/Debug-windows-x86_64-md/ArcaneEditor") / "arcbuild.exe");
    CHECK(c[1] == fs::path("D:/sdk/bin/Debug-windows-x86_64-md/ArcaneEditor") / ".." / "arcbuild" / "arcbuild.exe");
}

TEST_CASE("ResolveDriver returns the first candidate that exists, else empty", "[editor]")
{
    TempDir bin("driver");
    const fs::path editorDir = bin.path / "ArcaneEditor";
    fs::create_directories(editorDir);
    SECTION("neither -> empty (StartModuleRebuild refuses with a Console error)")
    {
        CHECK(ModuleBuild::ResolveDriver(editorDir).empty());
    }
    SECTION("dev layout only -> ../arcbuild/arcbuild.exe")
    {
        Touch(bin.path / "arcbuild" / "arcbuild.exe");
        CHECK(ModuleBuild::ResolveDriver(editorDir).lexically_normal() ==
              (bin.path / "arcbuild" / "arcbuild.exe").lexically_normal());
    }
    SECTION("packaged beside wins over the dev neighbour")
    {
        Touch(bin.path / "arcbuild" / "arcbuild.exe");
        Touch(editorDir / "arcbuild.exe");
        CHECK(ModuleBuild::ResolveDriver(editorDir) == editorDir / "arcbuild.exe");
    }
}

TEST_CASE("ComposeDriverCommand is arcbuild + the three flags, parenthesised, stderr-folded", "[editor]")
{
    // THE LINE THE EDITOR SPAWNS. Pinned exactly: the Runner executes this
    // through cmd, and arcbuild's own [build] tests pin what these flags
    // mean on the other side (spec s5.1).
    ModuleBuild::DriverInputs in;
    in.driverExe     = "D:/sdk/bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe";
    in.projectRoot   = "D:/dev/starworks/Gacha/Game";
    in.sdkRoot       = "D:/sdk";
    in.command       = "build";
    in.configuration = "Debug";

    CHECK(ModuleBuild::ComposeDriverCommand(in) ==
          "( \"D:/sdk/bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe\" build"
          " --project \"D:/dev/starworks/Gacha/Game\" --config Debug --sdk \"D:/sdk\" ) 2>&1");

    // RegenerateSolution's spelling: same shape, `generate`.
    in.command = "generate";
    const std::string gen = ModuleBuild::ComposeDriverCommand(in);
    CHECK(gen.find("arcbuild.exe\" generate --project") != std::string::npos);
    CHECK(gen.front() == '(');
    CHECK(gen.rfind(") 2>&1") == gen.size() - 6);

    // A space-laden install path survives inside quotes.
    in.driverExe = "C:/Program Files/Arcane/arcbuild.exe";
    CHECK(ModuleBuild::ComposeDriverCommand(in).find("( \"C:/Program Files/Arcane/arcbuild.exe\" generate") == 0);
}

TEST_CASE("Configuration matches the editor's own build flavor", "[editor]")
{
#ifdef _DEBUG
    CHECK(std::string(ModuleBuild::Configuration()) == "Debug");
#else
    CHECK(std::string(ModuleBuild::Configuration()) == "Release");
#endif
}
