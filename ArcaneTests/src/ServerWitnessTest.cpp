// The s6 witness: spawn the REAL staged ArcaneServer.exe and read its census. Same
// harness as the runtime witnesses (Helpers/HostWitness.hpp), same fresh-copy
// hygiene; [server], not [gpu] -- this host has no device (P14).
#include "Helpers/HostWitness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
using namespace Arcane::Test;
namespace
{
    std::filesystem::path StagedServerDir()
    {
        const std::filesystem::path p = std::filesystem::absolute("../ArcaneServer");
        INFO("staged ArcaneServer not found -- build Arcane.slnx first: " << p.string());
        REQUIRE(std::filesystem::exists(p / "ArcaneServer.exe"));
        return p;
    }
}
TEST_CASE("S1: ArcaneServer opens the project, loads the module, ticks N frames headless, and constructs no presentation", "[witness][server]")
{
    WitnessScratch scratch(StagedServerDir(), "s1-census");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneServer.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "server-report.json";
    inv.args = { "--project", "ReferenceProject", "--frames", "30", "--report", inv.reportPath.generic_string() };
    inv.hardCapMs = 60000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    CHECK(run.exitCode == 0);
    const auto& r = run.report;
    CHECK(r.at("schemaVersion") == 1);
    CHECK(r.at("host") == "ArcaneServer");
    CHECK(r.at("netMode") == "DedicatedServer");
    CHECK(r.at("isDedicatedServerProcess") == true);
    CHECK(r.at("project").at("opened") == true);
    CHECK(r.at("project").at("name") == "ReferenceProject");
    CHECK(r.at("module").at("loaded") == true);
    CHECK(r.at("framesTicked") == 30);
    CHECK(r.at("systems").at("hasPhysics") == true);
    CHECK(r.at("systems").at("hasPropagation") == true);
    CHECK(r.at("systems").at("hasRenderSubmission") == false);
    CHECK(r.at("systems").at("render") == 0);
    CHECK(r.at("presentation").at("clientAttached") == false);
    CHECK(r.at("presentation").at("clientDllLoadedAtBoot") == false);   // the exe's link line has no ArcaneClient (spec s6)
    CHECK(r.at("exitReason") == "frames-complete");
}
TEST_CASE("S2: ArcaneServer refuses a missing project with a report that says so", "[witness][server]")
{
    WitnessScratch scratch(StagedServerDir(), "s2-no-project");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneServer.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "server-report.json";
    inv.args = { "--project", "DoesNotExist", "--frames", "1", "--report", inv.reportPath.generic_string() };
    WitnessRun run = RunWitness(inv);
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    CHECK(run.exitCode != 0);
    CHECK(run.report.at("project").at("opened") == false);
    CHECK(run.report.at("exitReason") == "project-open-failed");
}
