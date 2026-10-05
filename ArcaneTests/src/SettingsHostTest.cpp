// Settings arc S3-11: the process-wide settings host -- a registered page
// draws in its window through CurrentSettingsGrid/SettingsRow, the focus
// signal the scene's Ctrl+Z stands down on, and the automation cvars that
// open a window at boot. Uses the PROCESS registry and host (no edits made,
// so nothing is written).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include "App/DialogSlot.hpp"
#include "Documents/DocumentHost.hpp"
#include "Documents/EditorDocument.hpp"
#include "Scene/UndoGate.hpp"
#include <Settings/SettingsHost.hpp>
#include <Settings/SettingsWindow.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Astra/Registry/Registry.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string>

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

TEST_CASE("ApplySettingsPathPick: a Browse dialog's result lands on the cvar as an undoable edit; empty or unknown picks are ignored", "[settings-ui]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("editor.settings.openCategory");
    const std::string before = reg.Get(h)->AsString();

    ApplySettingsPathPick("editor.settings.openCategory", "");              // dialog cancelled: no edit
    CHECK(reg.Get(h)->AsString() == before);
    ApplySettingsPathPick("no.such.cvar", "C:/ignored");                    // unknown cvar: no crash, no edit
    CHECK(reg.Get(h)->AsString() == before);

    ApplySettingsPathPick("editor.settings.openCategory", "C:/picked/folder");
    reg.Publish();
    CHECK(reg.Get(h)->AsString() == "C:/picked/folder");

    // Put the process registry back; the archive is never ticked or flushed here, so nothing reaches disk.
    REQUIRE(reg.RevertRung("editor.settings.openCategory", SetBy::EditorUser));
    reg.Publish();
    CHECK(reg.Get(h)->AsString() == before);
}

namespace
{
    void RestoreOpenCategory(CVarRegistry& reg, const std::string& before)
    {
        (void)reg.RevertRung("editor.settings.openCategory", SetBy::EditorUser);
        reg.Publish();
        CHECK(reg.Get(reg.Find("editor.settings.openCategory"))->AsString() == before);
        CloseSettingsHostIfProjectSwitchAccepted(true);   // drop leftover dirty/undo; Dev cvar is not archived
    }

    struct DirtyDoc final : EditorDocument
    {
        std::string title = "dirty.arcmat";
        Arcane::Guid guid = Arcane::Guid::Generate();
        const std::string& Title() const override { return title; }
        Arcane::Guid AssetGuid() const override { return guid; }
        bool Dirty() const override { return true; }
        bool Save() override { return true; }
        void Draw(bool&) override {}
        void NoteMoved(const std::filesystem::path&) override {}
    };

    struct SceneDummy final : ICommand
    {
        int* n = nullptr;
        explicit SceneDummy(int* p) : n(p) {}
        void Undo() override { --*n; }
        void Redo() override { ++*n; }
        const char* Label() const override { return "Dummy"; }
    };
}

TEST_CASE("BrowseSettingsPath: OS picker PathPickedThunk lands on DialogInbox::settingsPath and the frame consume path edits the cvar",
          "[settings-ui]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("editor.settings.openCategory");
    const std::string before = reg.Get(h)->AsString();
    RestoreOpenCategory(reg, before);

    DialogInbox dialogs;
    std::string settingsPathCvar;

    // EditorApp::BrowseSettingsPath -> OS picker cancels (null path): no edit.
    LaunchSettingsPathBrowse(settingsPathCvar, dialogs.settingsPath, "editor.settings.openCategory", false,
        [](bool folder, PathDialogRequest* req)
        {
            CHECK_FALSE(folder);
            PathPickedThunk(nullptr, req);   // SDL cancel
        });
    CHECK(settingsPathCvar == "editor.settings.openCategory");
    ConsumeSettingsPathPick(settingsPathCvar, dialogs.settingsPath);   // EditorAppFrame consume
    CHECK(reg.Get(h)->AsString() == before);
    CHECK_FALSE(SettingsHostArchivePending());

    // Browse again: OS picker PathPickedThunk Stashes, next frame ApplySettingsPathPick.
    LaunchSettingsPathBrowse(settingsPathCvar, dialogs.settingsPath, "editor.settings.openCategory", true,
        [](bool folder, PathDialogRequest* req)
        {
            CHECK(folder);
            PathPickedThunk("C:/picked/from-os", req);
        });
    ConsumeSettingsPathPick(settingsPathCvar, dialogs.settingsPath);
    reg.Publish();
    CHECK(reg.Get(h)->AsString() == "C:/picked/from-os");
    CHECK(SettingsHostArchivePending());

    RestoreOpenCategory(reg, before);
}

TEST_CASE("SwitchProject: SettingsHostOnProjectSwitch flushes only on Accepted, including later StageTableMismatch refusals",
          "[settings-ui]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("editor.settings.openCategory");
    const std::string before = reg.Get(h)->AsString();
    RestoreOpenCategory(reg, before);

    ApplySettingsPathPick("editor.settings.openCategory", "C:/pending-outgoing");
    reg.Publish();
    REQUIRE(reg.Get(h)->AsString() == "C:/pending-outgoing");
    REQUIRE(SettingsHostArchivePending());

    DocumentHost docs;
    docs.Add(std::make_unique<DirtyDoc>());
    REQUIRE(docs.AnyDirty());
    // SwitchProject's every session-untouched exit, including the two later
    // stage-table refusals that used to sit AFTER the accepted close.
    SettingsHostOnProjectSwitch(ProjectSwitchPreTeardown::RivalLock);
    SettingsHostOnProjectSwitch(ProjectSwitchPreTeardown::InvalidProject);
    SettingsHostOnProjectSwitch(docs.AnyDirty() ? ProjectSwitchPreTeardown::DirtyDocuments
                                                : ProjectSwitchPreTeardown::Accepted);
    SettingsHostOnProjectSwitch(ProjectSwitchPreTeardown::StageTableMismatch);
    CHECK(SettingsHostArchivePending());
    CHECK(reg.Get(h)->AsString() == "C:/pending-outgoing");

    docs.CloseAll();
    CHECK_FALSE(docs.AnyDirty());
    SettingsHostOnProjectSwitch(ProjectSwitchPreTeardown::Accepted);
    CHECK_FALSE(SettingsHostArchivePending());
    CHECK(reg.Get(h)->AsString() == "C:/pending-outgoing");   // flush keeps the value; undo is gone

    Arcane::Test::SettingsImGuiHarness imgui;
    bool open = true;
    auto frame = [&]
    {
        imgui.Frame([&]
        {
            DrawEditorPreferences(&open);
        });
    };
    frame();
    frame();
    const ImGuiWindow* w = ImGui::FindWindowByName("Editor Preferences");
    REQUIRE(w != nullptr);
    Arcane::Test::ClickAt(ImVec2(w->Pos.x + w->Size.x - 30.0f, w->Pos.y + w->Size.y - 30.0f), frame);
    CHECK(SettingsWindowFocused());
    Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Z, frame);
    CHECK(reg.Get(h)->AsString() == "C:/pending-outgoing");   // undo cleared with the outgoing project
    open = false;
    frame();

    RestoreOpenCategory(reg, before);
}

TEST_CASE("HandleUndoRedoAndSceneShortcuts: a focused settings window owns Ctrl+Z/Y via DispatchSceneUndoKeys",
          "[settings-ui]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("editor.settings.openCategory");
    const std::string before = reg.Get(h)->AsString();
    RestoreOpenCategory(reg, before);

    Astra::Registry registry;
    CommandStack scene{[&registry]() -> Astra::Registry& { return registry; }};
    int n = 1;
    scene.Push(std::make_unique<SceneDummy>(&n));
    REQUIRE(scene.CanUndo());
    REQUIRE(n == 1);

    ApplySettingsPathPick("editor.settings.openCategory", "C:/settings-edit");
    reg.Publish();
    REQUIRE(reg.Get(h)->AsString() == "C:/settings-edit");

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
            DrawEditorPreferences(&open);
        });
    };
    frame();
    frame();
    const ImGuiWindow* w = ImGui::FindWindowByName("Editor Preferences");
    REQUIRE(w != nullptr);
    Arcane::Test::ClickAt(ImVec2(w->Pos.x + w->Size.x - 40.0f, w->Pos.y + w->Size.y - 40.0f), frame);
    CHECK(SettingsWindowFocused());

    // Production Ctrl+Z/Y path (HandleUndoRedoAndSceneShortcuts -> DispatchSceneUndoKeys).
    DispatchSceneUndoKeys(scene, true, false, scene.InTransaction(), SettingsWindowFocused(), true, false);
    CHECK(n == 1);
    CHECK(scene.CanUndo());

    Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Z, frame);
    CHECK(reg.Get(h)->AsString() == before);                 // settings undid
    CHECK(n == 1);                                           // scene did not
    CHECK(scene.CanUndo());

    DispatchSceneUndoKeys(scene, true, false, scene.InTransaction(), SettingsWindowFocused(), false, true);
    CHECK(n == 1);
    Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Y, frame);
    CHECK(reg.Get(h)->AsString() == "C:/settings-edit");     // settings redid
    CHECK(n == 1);

    Arcane::Test::ClickAt(ImVec2(100.0f, 50.0f), frame);
    CHECK_FALSE(SettingsWindowFocused());
    Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Z, frame);
    CHECK(reg.Get(h)->AsString() == "C:/settings-edit");     // settings stand down
    DispatchSceneUndoKeys(scene, true, false, scene.InTransaction(), SettingsWindowFocused(), true, false);
    CHECK(n == 0);
    CHECK_FALSE(scene.CanUndo());
    CHECK(scene.CanRedo());
    DispatchSceneUndoKeys(scene, true, false, scene.InTransaction(), SettingsWindowFocused(), false, true);
    CHECK(n == 1);

    DispatchSceneUndoKeys(scene, true, true, false, false, true, false);   // Play
    CHECK(n == 1);
    DispatchSceneUndoKeys(scene, true, false, true, false, true, false);   // open transaction
    CHECK(n == 1);
    DispatchSceneUndoKeys(scene, true, false, false, true, true, false);   // focused settings
    CHECK(n == 1);

    open = false;
    frame();
    RestoreOpenCategory(reg, before);
}
