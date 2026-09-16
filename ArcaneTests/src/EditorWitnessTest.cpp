// E1: the EDITOR witness for the play-mode topologies (Core-DLL split, plan 1
// Task 7). Same harness as the runtime/server witnesses (Helpers/HostWitness.hpp),
// same fresh-copy hygiene -- but [witness][gpu], not [witness][server]: this host
// builds a real graphics device even under --headless (the offscreen chrome
// vehicle), so it belongs with the other [gpu] scenarios and is INVISIBLE to a
// `~[gpu]` run by design. The unfiltered suite is where it runs.
#include "Helpers/HostWitness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
using namespace Arcane::Test;
namespace
{
    std::filesystem::path StagedEditorDir()
    {
        const std::filesystem::path p = std::filesystem::absolute("../ArcaneEditor");
        INFO("staged ArcaneEditor not found -- build Arcane.slnx first: " << p.string());
        REQUIRE(std::filesystem::exists(p / "ArcaneEditor.exe"));
        return p;
    }
}
TEST_CASE("E1: the editor stands up client + embedded server through --play-as and reports both worlds", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e1-embedded-server");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneEditor.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "vulkan", "--frames", "60",
                 "--report", inv.reportPath.generic_string(), "--play-as", "embedded-server" };
    inv.hardCapMs = 120000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    const auto& worlds = run.report.at("worlds");
    REQUIRE(worlds.size() == 2);
    CHECK(worlds[0].at("role") == "Client");           CHECK(worlds[0].at("hasAuthority") == false);
    CHECK(worlds[1].at("role") == "DedicatedServer");  CHECK(worlds[1].at("hasAuthority") == true);
    CHECK(worlds[0].at("entities") == worlds[1].at("entities"));   // the same scene in both
}
