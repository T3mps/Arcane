// Integration ruling I3: the staged runtime's --set uses Editor in Debug/Release
// and LocalHost in Dist. This witness checks the host wiring, beyond the helper
// contract tested in CVarAccessTest. The interactive overlay uses the session role.
#include "Helpers/HostWitness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace Arcane::Test;

namespace
{
    std::filesystem::path StagedRuntimeDir()
    {
        const std::filesystem::path p = std::filesystem::absolute("../ArcaneRuntime");
        INFO("staged ArcaneRuntime not found -- build Arcane.slnx first: " << p.string());
        REQUIRE(std::filesystem::exists(p / "ArcaneRuntime.exe"));
        return p;
    }

    std::string Slurp(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
}

TEST_CASE("PS-W1: Debug runtime --set accepts Server and Game settings through the command-line context",
          "[witness][gpu]")
{
    WitnessScratch scratch(StagedRuntimeDir(), "ps-w1-command-line-set");
    WitnessInvocation inv;
    inv.exePath    = scratch.Dir() / "ArcaneRuntime.exe";
    inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "vulkan",
                 "--frames", "10", "--report", inv.reportPath.generic_string(),
                 "--set", "server.allowClientSetServer=true",
                 "--set", "console.historySize=8",
                 "--set", "render.meshCull=false" };
    inv.hardCapMs = 120000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string());
    INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);

    const std::string log = Slurp(run.stdoutPath) + Slurp(run.stderrPath);
    CHECK(log.find("--set server.allowClientSetServer") == std::string::npos);
    CHECK(log.find("--set console.historySize") == std::string::npos);
    CHECK(log.find("--set render.meshCull") == std::string::npos);
}
