// Settings arc S3-12: the Project Settings "Project" page -- the .arcproj
// identity (read-only), the boot scene, and the gameplay input asset
// selector moved from the old read-out (spec s6.1, s6.6, s16.7).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include "Helpers/TestEnvironment.hpp"
#include <Settings/ProjectSettingsPage.hpp>
#include <Widgets/PropertyGrid.hpp>
#include <Arcane/Project/Project.hpp>
#include <imgui.h>

TEST_CASE("The Project page shows the .arcproj identity and raises the input-asset requests", "[settings-ui]")
{
    Arcane::Test::TempDir dir("project-page");
    std::optional<Arcane::Project> project = Arcane::Project::Create(dir.path / "PageProj", "PageProj");
    REQUIRE(project.has_value());
    Arcane::Test::SettingsImGuiHarness imgui;
    Arcane::Editor::PropertyGridState state;
    state.probe = &imgui.probe;
    Arcane::Editor::ProjectSettingsRequests req;
    const Arcane::Project* shown = &*project;
    auto frame = [&]
    {
        imgui.Frame([&]
        {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(900.0f, 900.0f));
            ImGui::Begin("Project");
            Arcane::Editor::PropertyGrid grid(state);
            req = {};
            Arcane::Editor::DrawProjectIdentityPage(shown, grid, nullptr, req);
            ImGui::End();
        });
    };
    frame();
    frame();
    for (const char* row : { "Name", "GUID", "Engine ABI", "Game module", "Boot scene", "Input asset" })
    {
        INFO(row);
        CHECK(imgui.probe.count(row) == 1);
    }
    REQUIRE(imgui.probe.count("Actions#Open Asset") == 1);
    Arcane::Test::ClickAt(imgui.probe.at("Actions#Open Asset"), frame);
    CHECK_FALSE(req.open);                                     // nothing selected: Open is disabled
    Arcane::Test::ClickAt(imgui.probe.at("Actions#Create Input Actions Asset"), frame);
    CHECK(req.create);
    shown = nullptr;                                           // no project: the hint, no rows
    frame();
    CHECK(imgui.probe.count("Name") == 0);
}
