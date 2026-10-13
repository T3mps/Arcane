#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Build/BuildToolSettings.hpp>
#include <Arcane/Build/Toolchain.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Project/IdeLaunch.hpp>   // the editor's DevenvCache (source-compiled into ArcaneTests)
#include <Request.hpp>   // arcbuild's Cli (source-compiled into ArcaneTests)
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
using namespace Arcane;

namespace
{
    // POSIX: an override must carry an executable bit to be a launchable
    // program (Toolchain's IsRunnableCandidate); Windows needs a regular file.
    void MakeLaunchable([[maybe_unused]] const std::filesystem::path& fake)
    {
#if !defined(_WIN32)
        std::filesystem::permissions(fake, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
#endif
    }
}

TEST_CASE("sweep: an explicit build.premakePath wins over discovery; empty discovers", "[sweep][build]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    CHECK(BuildToolSettings{}.premakePath.empty());
    Test::SkipIfCompiledOut("build.premakePath");
    Test::RequireDefault("build.premakePath", CVarValue::String(""));
    const auto fake = std::filesystem::temp_directory_path() / "fake-premake5.exe";
    std::ofstream(fake) << "x";
    MakeLaunchable(fake);
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("build.premakePath"), CVarValue::String(fake.string()), SetBy::Code);
    reg.PublishImmediate();
    CHECK(std::filesystem::equivalent(Toolchain::ResolvePremake("C:/no-such-sdk"), fake));
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
    std::filesystem::remove(fake);
}

TEST_CASE("sweep: build.msbuildPath/makePath/ninjaPath/ideExecutable win over discovery", "[sweep][build]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    CHECK(BuildToolSettings{}.msbuildPath.empty());
    CHECK(BuildToolSettings{}.makePath.empty());
    CHECK(BuildToolSettings{}.ninjaPath.empty());
    CHECK(BuildToolSettings{}.ideExecutable.empty());
    Test::RequireDefault("build.msbuildPath", CVarValue::String(""));
    Test::RequireDefault("build.makePath", CVarValue::String(""));
    Test::RequireDefault("build.ninjaPath", CVarValue::String(""));
    Test::RequireDefault("build.ideExecutable", CVarValue::String(""));

    // build.msbuildPath is not Dev, so Dist keeps it; the other three are
    // compiled out there (each proven absent: `&`, not `&&`).
    const bool devRows = Test::InThisBuild("build.makePath") & Test::InThisBuild("build.ninjaPath")
                       & Test::InThisBuild("build.ideExecutable");

    const auto fake = std::filesystem::temp_directory_path() / "fake-build-tool.exe";
    std::ofstream(fake) << "x";
    MakeLaunchable(fake);
    CVarRegistry& reg = CVarRegistry::Get();
    for (const char* name : { "build.msbuildPath", "build.makePath", "build.ninjaPath", "build.ideExecutable" })
        if (devRows || std::string_view(name) == "build.msbuildPath")
            reg.Set(reg.Find(name), CVarValue::String(fake.string()), SetBy::Code);
    reg.PublishImmediate();
    CHECK(std::filesystem::equivalent(Toolchain::ResolveMsBuild(), fake));
    if (devRows)
    {
        CHECK(std::filesystem::equivalent(Toolchain::ResolveMake(), fake));
        CHECK(std::filesystem::equivalent(Toolchain::ResolveNinja(), fake));
        CHECK(std::filesystem::equivalent(Toolchain::ResolveDevenv(), fake));
    }
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
    std::filesystem::remove(fake);
}

TEST_CASE("sweep: a changed build.ideExecutable is what the next IDE launch resolves (Live)", "[sweep][build]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    // S6-15 carried gap: the editor cached its devenv answer for the whole
    // session, so a changed setting reached "Open Visual Studio" only on the
    // next launch. Both values are real files, so no vswhere is spawned.
    Test::SkipIfCompiledOut("build.ideExecutable");
    const auto first  = std::filesystem::temp_directory_path() / "fake-devenv-a.exe";
    const auto second = std::filesystem::temp_directory_path() / "fake-devenv-b.exe";
    std::ofstream(first) << "x";
    std::ofstream(second) << "x";
    MakeLaunchable(first);
    MakeLaunchable(second);
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("build.ideExecutable");
    reg.Set(h, CVarValue::String(first.string()), SetBy::Code);
    reg.PublishImmediate();

    Editor::IdeLaunch::DevenvCache cache;
    CHECK(cache.Refresh());
    CHECK(std::filesystem::equivalent(cache.Path(), first));
    CHECK_FALSE(cache.Refresh());   // unchanged: the cached answer, no re-resolve

    reg.Set(h, CVarValue::String(second.string()), SetBy::Code);
    reg.PublishImmediate();
    CHECK(cache.Refresh());
    CHECK(std::filesystem::equivalent(cache.Path(), second));

    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

TEST_CASE("sweep: a build.ninjaPath that is not a file falls back to discovery", "[sweep][build]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    const std::filesystem::path discovered = Toolchain::ResolveNinja();
    CVarRegistry& reg = CVarRegistry::Get();
    const auto missing = std::filesystem::temp_directory_path() / "no-such-ninja.exe";
    std::filesystem::remove(missing);
    reg.Set(reg.Find("build.ninjaPath"), CVarValue::String(missing.string()), SetBy::Code);
    reg.PublishImmediate();
    CHECK(Toolchain::ResolveNinja() == discovered);
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
}

TEST_CASE("sweep: arcbuild carries repeatable --set to the CommandLine rung", "[sweep][build]")
{
    std::vector<std::string> words{ "build", "--project", "X",
                                    "--set", "build.ninjaPath=C:/tools/ninja.exe", "--set", "build.makePath=C:/tools/make.exe" };
    std::vector<char*> argv;
    for (std::string& w : words) argv.push_back(w.data());
    const Cli cli = arcbuild::MakeCli();
    const Cli::Result parsed = cli.Parse(static_cast<int>(argv.size()), argv.data());
    REQUIRE(parsed.ok);
    const arcbuild::Request req = arcbuild::RequestFromCli(arcbuild::Command::Build, parsed);
    CHECK(req.cvarSets == std::vector<std::string>{ "build.ninjaPath=C:/tools/ninja.exe", "build.makePath=C:/tools/make.exe" });

    // What arcbuild's Application does with them: the Editor context reaches
    // the Editor-audience build.* cvars (Dev, so not in a Dist build).
    Test::SkipIfCompiledOut("build.ninjaPath");
    Test::SkipIfCompiledOut("build.makePath");
    CVarRegistry& reg = CVarRegistry::Get();
    ApplyCVarCommandLine(reg, req.cvarSets, CVarContext::Editor);
    reg.PublishImmediate();
    CHECK(Settings<BuildToolSettings>().ninjaPath == "C:/tools/ninja.exe");
    CHECK(Settings<BuildToolSettings>().makePath == "C:/tools/make.exe");
    reg.RevertLayer(SetBy::CommandLine); reg.PublishImmediate();
    CHECK(Settings<BuildToolSettings>().ninjaPath.empty());
}
