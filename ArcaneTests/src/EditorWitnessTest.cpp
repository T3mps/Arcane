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

// E2: the PERSPECTIVE editor witness (F4 plan 1 T12, spec s9). The editor
// booted with --view-mode perspective on ReferenceProject renders the cube on
// the XZ grid through the Orbit3D camera, settles, and compares against its
// OWN golden slot (editor-ui-perspective.png -- --compare takes any safe name,
// so the slot needs no registry beyond the PNG). Two facts are asserted, not
// one: the report's `viewMode` is what the host actually resolved the camera
// to (a "perspective" the seed failed to apply would still exit 0 with a 2D
// picture), and the compare PASSED against the blessed slot (which is what
// makes the new golden load-bearing -- golden-gate.ps1 runs six lanes now,
// and two of them (dx12 and vulkan) name it). Same [witness][gpu] posture as
// E1: outside ~[gpu], run unfiltered.
TEST_CASE("E2: the editor boots into perspective on --view-mode, reports viewMode, and matches the perspective golden", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e2-perspective");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneEditor.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "dx12", "--frames", "60",
                 "--settle", "30", "--report", inv.reportPath.generic_string(),
                 "--view-mode", "perspective", "--compare", "editor-ui-perspective" };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    CHECK(run.report.at("exitReason") == "frames-complete");
    REQUIRE(run.report.contains("viewMode"));           // schema 7: absent on a host that has no view mode
    CHECK(run.report.at("viewMode") == "perspective");
    REQUIRE(run.report.contains("compare"));
    CHECK(run.report["compare"].at("reference") == "editor-ui-perspective");
    CHECK(run.report["compare"].at("passed") == true);
    CHECK(run.report["compare"].at("diffCount") == 0);
}
