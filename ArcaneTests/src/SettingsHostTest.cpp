// Settings arc S3-11: the process-wide settings host -- a registered page
// draws in its window through CurrentSettingsGrid/SettingsRow, the focus
// signal the scene's Ctrl+Z stands down on, and the automation cvars that
// open a window at boot. Uses the PROCESS registry and host (no edits made,
// so nothing is written).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include <Settings/SettingsHost.hpp>
#include <Settings/SettingsWindow.hpp>
#include <imgui.h>
#include <imgui_internal.h>

using namespace Arcane;
using namespace Arcane::Editor;

namespace
{
    struct PageProbe { int calls = 0; bool rowDrawn = false; };
}

TEST_CASE("RegisterSettingsPage: the page draws in its window, through CurrentSettingsGrid and SettingsRow", "[settings-ui]")
{
    Arcane::Test::SettingsImGuiHarness imgui;
    PageProbe probe;
    RegisterSettingsPage(SettingScope::Project, "Test/Host Page", "Host Page", [](void* user)
    {
        auto& p = *static_cast<PageProbe*>(user);
        ++p.calls;
        PropertyGrid::Rows rows(CurrentSettingsGrid(), "##hostpage");
        if (rows) p.rowDrawn = SettingsRow("editor.settings.saveDebounceMs");
    }, &probe);
    SelectSettingsCategory(SettingsWindowKind::Project, "Test/Host Page");
    bool open = true;
    for (int i = 0; i < 3; ++i) imgui.Frame([&] { DrawProjectSettings(&open); });
    CHECK(probe.calls >= 2);
    CHECK(probe.rowDrawn);
    CHECK(SelectedSettingsCategory(SettingsWindowKind::Project) == "Test/Host Page");
    // The page's user pointer dies with this test: re-register the path with no draw function.
    RegisterSettingsPage(SettingScope::Project, "Test/Host Page", "Host Page", nullptr, nullptr);
    open = false;
    imgui.Frame([&] { DrawProjectSettings(&open); });
}

TEST_CASE("SettingsWindowFocused follows the settings windows' focus, so the scene's Ctrl+Z can stand down", "[settings-ui]")
{
    Arcane::Test::SettingsImGuiHarness imgui;
    bool open = true;
    auto frame = [&]
    {
        imgui.Frame([&]
        {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(200.0f, 100.0f));
            ImGui::Begin("Other");
            ImGui::TextUnformatted("x");
            ImGui::End();
            DrawProjectSettings(&open);
        });
    };
    frame();
    frame();
    const ImGuiWindow* w = ImGui::FindWindowByName("Project Settings");
    REQUIRE(w != nullptr);
    Arcane::Test::ClickAt(ImVec2(w->Pos.x + w->Size.x - 30.0f, w->Pos.y + w->Size.y - 30.0f), frame);
    CHECK(SettingsWindowFocused());
    Arcane::Test::ClickAt(ImVec2(100.0f, 50.0f), frame);
    CHECK_FALSE(SettingsWindowFocused());
    open = false;
    frame();
}

TEST_CASE("editor.settings.openAtBoot / openCategory open a window at boot and select a category", "[settings-ui]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    REQUIRE(reg.Set(reg.Find("editor.settings.openAtBoot"), CVarValue::String("project"), SetBy::Console) == SetResult::Applied);
    REQUIRE(reg.Set(reg.Find("editor.settings.openCategory"), CVarValue::String("Engine"), SetBy::Console) == SetResult::Applied);
    reg.Publish();
    const SettingsBootOpen boot = ConsumeSettingsOpenAtBoot();
    CHECK(boot.project);
    CHECK_FALSE(boot.preferences);
    CHECK(SelectedSettingsCategory(SettingsWindowKind::Project) == "Engine");
    REQUIRE(reg.RevertRung("editor.settings.openAtBoot", SetBy::Console));
    REQUIRE(reg.RevertRung("editor.settings.openCategory", SetBy::Console));
    reg.Publish();
    const SettingsBootOpen none = ConsumeSettingsOpenAtBoot();
    CHECK_FALSE(none.project);
    CHECK_FALSE(none.preferences);
}
