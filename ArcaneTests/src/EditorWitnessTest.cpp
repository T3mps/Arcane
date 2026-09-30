// E1: the EDITOR witness for the play-mode topologies (Core-DLL split, plan 1
// Task 7). Same harness as the runtime/server witnesses (Helpers/HostWitness.hpp),
// same fresh-copy hygiene -- but [witness][gpu], not [witness][server]: this host
// builds a real graphics device even under --headless (the offscreen chrome
// vehicle), so it belongs with the other [gpu] scenarios and is INVISIBLE to a
// `~[gpu]` run by design. The unfiltered suite is where it runs.
#include "Helpers/HostWitness.hpp"
#include "Helpers/ReferenceProjectDir.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
using namespace Arcane::Test;
namespace
{
    std::string ReadAllBytes(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::filesystem::path StagedEditorDir()
    {
        const std::filesystem::path p = std::filesystem::absolute("../ArcaneEditor");
        {
            INFO("staged ArcaneEditor not found -- build Arcane.slnx first: " << p.string());
            REQUIRE(std::filesystem::exists(p / "ArcaneEditor.exe"));
        }

        // Every editor witness runs --headless, which pins the layout to the
        // STAGED Saved/verify-layout.ini. That copy is refreshed only when
        // ArcaneEditor's postbuild runs (or golden-gate.ps1 restages it), so a
        // seed edit made after the last editor link leaves the witnesses on
        // the OLD seed while they report green (inspector filters Task 10, fix
        // round 1: measured, the Debug witnesses ran the 2026-09-10 seed).
        // Refuse rather than test the wrong input.
        const std::filesystem::path sourceProject = Arcane::Test::FindReferenceProjectDir();
        REQUIRE_FALSE(sourceProject.empty());
        const std::filesystem::path sourceSeed = sourceProject / "Saved" / "verify-layout.ini";
        const std::filesystem::path stagedSeed = p / "ReferenceProject" / "Saved" / "verify-layout.ini";
        INFO("the staged layout seed is stale -- rebuild ArcaneEditor (its postbuild restages it) "
             "or run scripts/golden-gate.ps1: " << stagedSeed.string() << " != " << sourceSeed.string());
        REQUIRE(std::filesystem::exists(sourceSeed));
        REQUIRE(std::filesystem::exists(stagedSeed));
        const bool stagedSeedMatchesSource = ReadAllBytes(stagedSeed) == ReadAllBytes(sourceSeed);
        REQUIRE(stagedSeedMatchesSource);   // a bool, so a failure prints the message, not two whole files
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
// makes the new golden load-bearing -- two of golden-gate.ps1's lanes (dx12
// and vulkan) name it; that table is count-derived, so this comment names no
// total). Same [witness][gpu] posture as E1: outside ~[gpu], run unfiltered.
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

    // THE VISIBILITY BLOCK (F3 spec s9.2, plan 2 T6). s9.2 asks the witness
    // lanes to assert the block for ReferenceProject's 3D scene, and until
    // this task nothing did: a host could have reported zeros, or a
    // `gpuVisible` quietly copied from the coarse count, and every lane would
    // still have been green. This is the EDITOR half -- the runtime half is
    // WitnessScenariosTest.cpp's W5, against the F3 fixture scene.
    //
    // The numbers are the BOOT SCENE's, derived from its own content and not
    // from a previous run's output: main.arcscene carries two MeshRenderers
    // (MeshCube -> reference_cube.arcmesh, one section; GoldenProp ->
    // golden_prop.arcmesh, three sections), so four (entity, section) rows,
    // all four inside the perspective view, each with its own batch key
    // (distinct mesh/section pairs, all opaque) and therefore its own indirect
    // draw. No transparent material appears in that scene at all, which is
    // exactly why the F3 fixture scene exists beside it.
    REQUIRE(run.report.contains("visibility"));
    const auto& vis = run.report["visibility"];
    CHECK(vis.at("total") == 4);
    CHECK(vis.at("coarseVisible") == 4);
    CHECK(vis.at("batches") == 4);
    CHECK(vis.at("draws") == 4);
    CHECK(vis.at("transparentRows") == 0);
    // gpuVisible is the CULL PASS's OWN count, read back asynchronously and
    // `null` until one retires (schemaVersion 9). Asserting it is a NUMBER is
    // the load-bearing half: a delayed ring that never publishes inside this
    // run's --frames 60 / --settle 30 budget would leave null here, and the
    // right answer to that is a bigger budget, never a fabricated number.
    REQUIRE(vis.at("gpuVisible").is_number_unsigned());
    // ...and it agrees with the coarse count ON THIS SCENE, where every row is
    // opaque and inside the frustum. That identity is NOT general -- a scene
    // with transparent rows has fewer gpuVisible than coarseVisible, since
    // transparent keys are never emitted to the cull (W5 pins that side).
    CHECK(vis.at("gpuVisible") == vis.at("coarseVisible"));
}

// E3: the INPUT DOCUMENT witness (inspector-ownership spec s5 / input-editor
// spec s4). --open-asset puts Player.arcinput on screen, --select-in-document
// selects Player/Jump inside it, and the report's `inspector` block says the
// document -- not the scene -- owns the Inspector, with the breadcrumb a
// person would read. Compared against its own golden slot once blessed.
TEST_CASE("E3: an opened input document with a scripted selection owns the Inspector and matches the editor-input-doc golden", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e3-input-doc");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneEditor.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "dx12", "--frames", "90",
                 "--settle", "10", "--report", inv.reportPath.generic_string(),
                 "--open-asset", "97260310-8b35-4b29-b12f-1fd6f8e99071",
                 "--select-in-document", "Player/Jump", "--compare", "editor-input-doc" };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.contains("inspector"));
    CHECK(run.report["inspector"].at("source") == "Player.arcinput");
    CHECK(run.report["inspector"].at("breadcrumb") == "Player.arcinput > Player > Jump");
    // inspector.instances (schemaVersion 12, inspector filters s9): the default
    // layout's two instances -- the main one excludes only Assets, the second
    // is Assets-only (it excludes every other catalog kind, catalog order).
    REQUIRE(run.report["inspector"].contains("instances"));
    const auto& inst = run.report["inspector"]["instances"];
    REQUIRE(inst.size() == 2);
    CHECK(inst[0].at("id") == 0);
    CHECK(inst[0].at("excluded") == nlohmann::json::array({ "assets" }));
    CHECK(inst[1].at("id") == 1);
    CHECK(inst[1].at("excluded") == nlohmann::json::array({ "scene", "input-actions", "material", "sprite", "mesh" }));
    REQUIRE(run.report.contains("compare"));
    CHECK(run.report["compare"].at("passed") == true);
}

// E4: the ASSET PAGE witness (inspector filters s6). --select-asset selects
// uv_marker.png in the Asset Browser the way a click does; the Assets-only
// Inspector shows its page while the main (All but Assets) one stays on the scene.
TEST_CASE("E4: a selected asset routes the Assets Inspector to its page and matches the editor-asset-page golden", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e4-asset-page");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneEditor.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    // --frames 60 --settle 30, E2's and the gate lane's values, NOT E3's 90/10:
    // the viewport is visible in this capture, and the frozen PulseBox phase
    // depends on the frame count, so the witness must render the frame the
    // shared editor-asset-page slot was blessed at (a 90-frame run converges
    // on a picture 4660 px off the golden, all of them the PulseBox).
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "dx12", "--frames", "60",
                 "--settle", "30", "--report", inv.reportPath.generic_string(),
                 "--select-asset", "d7f389fd-f687-407d-b9d7-9753eb6b0258",   // textures/uv_marker.png
                 "--compare", "editor-asset-page" };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.contains("inspector"));
    CHECK(run.report["inspector"].at("source") == "Assets");                  // Current(): the asset selection
    REQUIRE(run.report["inspector"].contains("instances"));
    CHECK(run.report["inspector"]["instances"][0].at("source") == "Scene");   // All but Assets: untouched
    CHECK(run.report["inspector"]["instances"][1].at("source") == "Assets");
    REQUIRE(run.report.contains("compare"));
    CHECK(run.report["compare"].at("passed") == true);
}

// E5: DOCUMENT PAGES (inspector filters s6a). Opening a material, a sprite or a
// mesh selects that document's page: the main Inspector routes to it, and the
// Assets-only Inspector is untouched.
TEST_CASE("E5: opening a material, a sprite or a mesh routes the main Inspector to that document's page", "[witness][gpu]")
{
    struct Doc { const char* guid; const char* title; const char* compare; };
    const Doc docs[] = {
        { "7e5a0010-0010-4010-8010-000000000010", "ReferenceCubeMaterial", "editor-material-page" },   // materials/reference_mesh.arcmat
        { "87bd4fd3-c9e6-4fc8-a8e0-cf799378f049", "UvMarkerSprite",        nullptr },                  // sprites/uv_marker.arcsprite
        { "7e5a0011-0011-4011-8011-000000000011", "ReferenceCube",         nullptr },                  // meshes/reference_cube.arcmesh
    };
    for (const Doc& d : docs)
    {
        DYNAMIC_SECTION(d.title)
        {
            WitnessScratch scratch(StagedEditorDir(), std::string("e5-") + d.title);
            WitnessInvocation inv;
            inv.exePath = scratch.Dir() / "ArcaneEditor.exe"; inv.workingDir = scratch.Dir();
            inv.reportPath = scratch.Dir() / "witness-report.json";
            inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "dx12", "--frames", "90",
                         "--settle", "10", "--report", inv.reportPath.generic_string(),
                         "--open-asset", d.guid };
            if (d.compare) { inv.args.push_back("--compare"); inv.args.push_back(d.compare); }
            inv.hardCapMs = 180000;
            WitnessRun run = RunWitness(inv);
            INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
            REQUIRE_FALSE(GradeProcessFacts(run).has_value());
            REQUIRE(run.exitCode == 0);
            REQUIRE(run.report.contains("inspector"));
            REQUIRE(run.report["inspector"].contains("instances"));
            CHECK(run.report["inspector"]["instances"][0].at("source") == d.title);
            CHECK(run.report["inspector"]["instances"][1].at("source") == "Assets");   // untouched by a document open
            if (d.compare)
            {
                REQUIRE(run.report.contains("compare"));
                CHECK(run.report["compare"].at("passed") == true);
            }
        }
    }
}

// E3b: an unresolvable --select-in-document is loud (ERROR on stderr) but not
// fatal; the Inspector keeps the document's opening selection (first map +
// first action of Player.arcinput). No --compare: no golden slot for this state.
TEST_CASE("E3b: an unresolvable --select-in-document is a loud ERROR, the run completes, and the Inspector keeps the document's opening selection", "[witness][gpu]")
{
    WitnessScratch scratch(StagedEditorDir(), "e3b-input-doc-unresolved");
    WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneEditor.exe"; inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "dx12", "--frames", "60",
                 "--report", inv.reportPath.generic_string(),
                 "--open-asset", "97260310-8b35-4b29-b12f-1fd6f8e99071",
                 "--select-in-document", "Player/NoSuchAction" };
    inv.hardCapMs = 180000;
    WitnessRun run = RunWitness(inv);
    INFO("host stdout: " << run.stdoutPath.string()); INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    CHECK(run.report.at("exitReason") == "frames-complete");
    REQUIRE(run.report.contains("inspector"));
    CHECK(run.report["inspector"].at("source") == "Player.arcinput");
    CHECK(run.report["inspector"].at("breadcrumb") == "Player.arcinput > Player > Move");
    CHECK_FALSE(run.report.contains("compare"));
    std::ifstream err(run.stderrPath);
    const std::string all((std::istreambuf_iterator<char>(err)), std::istreambuf_iterator<char>());
    CHECK(all.find("--select-in-document 'Player/NoSuchAction': the opened document has no such path") != std::string::npos);
}
