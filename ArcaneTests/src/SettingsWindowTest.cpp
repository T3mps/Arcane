// Settings arc S3-10: the settings window through the REAL ImGui path --
// the tree, search, selection, window-local Ctrl+Z (focus-gated), the
// Restart bar, "Module unloaded", and debounced writes flushed on close.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include <Settings/SettingsWindow.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace Arcane;
using namespace Arcane::Editor;
using Arcane::Test::AddSetting;

namespace
{
    struct WindowHarness
    {
        Arcane::Test::SettingsImGuiHarness imgui;
        CVarRegistry reg;
        SettingsWindowState st;
        SettingsArchiveQueue archive;
        SettingsApplyTracker tracker;
        std::vector<std::pair<SetBy, std::vector<std::string>>> writes;
        double clock = 0.0;
        bool open = true;
        bool drawOther = false;
        int restarts = 0;
        SettingsWindowKind kind = SettingsWindowKind::Project;
        SettingsModuleRoles roles;

        WindowHarness() { st.grid.probe = &imgui.probe; }

        SettingsWindowEnv Env()
        {
            SettingsWindowEnv env;
            env.registry = &reg;
            env.kind = kind;
            env.title = "Settings Under Test";
            env.roles = roles;
            env.now = [this] { return clock; };
            env.archive = &archive;
            env.writeRung = [this](SetBy rung, const std::vector<std::string>& names) { writes.emplace_back(rung, names); return true; };
            env.tracker = &tracker;
            env.restartEditor = [this] { ++restarts; };
            env.projectOpen = true;
            return env;
        }
        void Frame()
        {
            imgui.Frame([&]
            {
                if (drawOther)
                {
                    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
                    ImGui::SetNextWindowSize(ImVec2(200.0f, 100.0f));
                    ImGui::Begin("Other");
                    ImGui::TextUnformatted("x");
                    ImGui::End();
                }
                DrawSettingsWindow(st, Env(), &open);
            });
        }
        ImVec2 At(const std::string& key) { INFO(key); REQUIRE(imgui.probe.count(key) == 1); return imgui.probe.at(key); }
        void Click(const std::string& key) { const ImVec2 p = At(key); Arcane::Test::ClickAt(p, [&] { Frame(); }); }
        void ClickPoint(ImVec2 p) { Arcane::Test::ClickAt(p, [&] { Frame(); }); }
        ImVec2 EmptyPageCorner()
        {
            const ImGuiWindow* w = ImGui::FindWindowByName("Settings Under Test");
            REQUIRE(w != nullptr);
            return ImVec2(w->Pos.x + w->Size.x - 30.0f, w->Pos.y + w->Size.y - 30.0f);
        }
        bool TreeHas(std::string_view path) const
        {
            return std::find(st.last.treePaths.begin(), st.last.treePaths.end(), path) != st.last.treePaths.end();
        }
    };
}

TEST_CASE("Settings window: the tree from category paths, a node's rows plus its children's, and search narrowing both", "[settings-ui]")
{
    WindowHarness h;
    h.kind = SettingsWindowKind::Preferences;
    REQUIRE_FALSE(AddSetting(h.reg, "editor.undo.maxSteps", { .type = CVarType::Int32, .def = CVarValue::Int32(100),
        .scope = SettingScope::PreferencesProject }).IsStale());
    REQUIRE_FALSE(AddSetting(h.reg, "editor.graph.fitMinZoom", { .type = CVarType::Float32, .def = CVarValue::Float32(0.5f),
        .scope = SettingScope::PreferencesMachine }).IsStale());
    REQUIRE_FALSE(AddSetting(h.reg, "editor.graph.wire.thickness", { .type = CVarType::Float32, .def = CVarValue::Float32(1.5f),
        .scope = SettingScope::PreferencesMachine }).IsStale());
    h.Frame();
    h.Frame();
    CHECK(h.TreeHas("Editor/Graph/Wire"));
    CHECK(h.TreeHas("Editor/Undo"));
    h.Click("tree:Editor/Graph");
    h.Frame();
    CHECK(h.st.last.page == SettingsWindowState::FrameFacts::Page::Rows);
    CHECK((h.st.last.rows == std::vector<std::string>{ "editor.graph.fitMinZoom", "editor.graph.wire.thickness" }));
    h.Click("##settings-search");
    Arcane::Test::TypeText("thick", [&] { h.Frame(); });
    h.Frame();
    CHECK(h.TreeHas("Editor/Graph/Wire"));
    CHECK_FALSE(h.TreeHas("Editor/Undo"));
    CHECK((h.st.last.rows == std::vector<std::string>{ "editor.graph.wire.thickness" }));
}

TEST_CASE("Settings window: Category attribute groups are collapsible within a category, not tree nodes", "[settings-ui]")
{
    WindowHarness h;
    REQUIRE_FALSE(AddSetting(h.reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    REQUIRE_FALSE(AddSetting(h.reg, "render.exposure", { .type = CVarType::Float32, .def = CVarValue::Float32(1.0f),
        .group = "Color" }).IsStale());
    REQUIRE_FALSE(AddSetting(h.reg, "render.gamma", { .type = CVarType::Float32, .def = CVarValue::Float32(2.2f),
        .group = "Color" }).IsStale());
    REQUIRE_FALSE(AddSetting(h.reg, "render.shadow", { .type = CVarType::Bool, .def = CVarValue::Bool(true),
        .group = "Lighting" }).IsStale());
    REQUIRE_FALSE(AddSetting(h.reg, "render.wire.thickness", { .type = CVarType::Float32, .def = CVarValue::Float32(1.5f) }).IsStale());
    h.st.selected = "Engine/Render";
    h.Frame();
    h.Frame();
    CHECK(h.TreeHas("Engine/Render/Wire"));
    CHECK_FALSE(h.TreeHas("Engine/Render/Color"));
    CHECK_FALSE(h.TreeHas("Engine/Render/Lighting"));
    CHECK((h.st.last.groups == std::vector<std::string>{ "Color", "Lighting" }));
    CHECK((h.st.last.rows == std::vector<std::string>{
        "render.vsync", "render.exposure", "render.gamma", "render.shadow", "render.wire.thickness" }));
    h.Click("group:Engine/Render/Color");
    h.Frame();
    CHECK((h.st.last.groups == std::vector<std::string>{ "Color", "Lighting" }));
    CHECK((h.st.last.rows == std::vector<std::string>{ "render.vsync", "render.shadow", "render.wire.thickness" }));
}

TEST_CASE("Settings window: Ctrl+Z undoes the window's own last edit, and only while the window has focus", "[settings-ui]")
{
    WindowHarness h;
    h.drawOther = true;
    REQUIRE_FALSE(AddSetting(h.reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    h.st.selected = "Engine/Render";
    h.Frame();
    h.Frame();
    h.Click("Vsync");
    CHECK(h.reg.RungValue("render.vsync", SetBy::Project) == std::optional<CVarValue>(CVarValue::Bool(false)));
    Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Z, [&] { h.Frame(); });
    CHECK_FALSE(h.reg.RungValue("render.vsync", SetBy::Project).has_value());
    h.Click("Vsync");
    h.ClickPoint(ImVec2(100.0f, 50.0f));                               // focus moves to "Other"
    CHECK_FALSE(h.st.focused);
    Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Z, [&] { h.Frame(); });
    CHECK(h.reg.RungValue("render.vsync", SetBy::Project) == std::optional<CVarValue>(CVarValue::Bool(false)));   // not ours to undo
    h.ClickPoint(h.EmptyPageCorner());                                 // focus back
    CHECK(h.st.focused);
    Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Z, [&] { h.Frame(); });
    CHECK_FALSE(h.reg.RungValue("render.vsync", SetBy::Project).has_value());
}

TEST_CASE("Settings window: the Restart bar counts settings changed since boot and its button restarts", "[settings-ui]")
{
    WindowHarness h;
    REQUIRE_FALSE(AddSetting(h.reg, "render.backend", { .type = CVarType::Int32, .def = CVarValue::Int32(0), .apply = ApplyMode::Restart }).IsStale());
    REQUIRE_FALSE(AddSetting(h.reg, "physics.substeps", { .type = CVarType::Int32, .def = CVarValue::Int32(4), .apply = ApplyMode::NextWorld }).IsStale());
    h.Frame();                                                          // the first rebuild baselines them (boot)
    CHECK(h.st.last.restartPending == 0);
    REQUIRE(h.reg.SetRung("render.backend", SetBy::Project, CVarValue::Int32(1), "project"));
    REQUIRE(h.reg.SetRung("physics.substeps", SetBy::Project, CVarValue::Int32(8), "project"));
    h.reg.Publish();
    h.Frame();
    CHECK(h.st.last.restartPending == 1);
    CHECK(h.st.last.nextWorldPending == 1);
    h.Click("Restart editor");
    CHECK(h.restarts == 1);
    REQUIRE(h.reg.RevertRung("render.backend", SetBy::Project));
    h.reg.Publish();
    h.Frame();
    CHECK(h.st.last.restartPending == 0);                              // reverted before restarting: no bar
    CHECK(h.imgui.probe.count("Restart editor") == 0);
}

TEST_CASE("Settings window: a game module's page shows 'Module unloaded' while it is gone, and repopulates on reload", "[settings-ui]")
{
    WindowHarness h;
    h.roles.gameModule = "TestGame";
    const Arcane::Test::SettingSpec spec{ .type = CVarType::Float32, .def = CVarValue::Float32(5.0f), .module = "TestGame" };
    REQUIRE_FALSE(AddSetting(h.reg, "speed.max", spec).IsStale());
    h.st.selected = "Game/TestGame/Speed";
    h.Frame();
    h.Frame();
    CHECK((h.st.last.rows == std::vector<std::string>{ "speed.max" }));
    h.reg.UnregisterModule("TestGame");
    h.Frame();
    CHECK(h.st.last.page == SettingsWindowState::FrameFacts::Page::ModuleUnloaded);
    CHECK(h.st.selected == "Game/TestGame/Speed");                     // the selection waits for the reload
    REQUIRE_FALSE(AddSetting(h.reg, "speed.max", spec).IsStale());
    h.Frame();
    CHECK(h.st.last.page == SettingsWindowState::FrameFacts::Page::Rows);
    CHECK((h.st.last.rows == std::vector<std::string>{ "speed.max" }));
}

TEST_CASE("Settings window: edits are written once quiet for the debounce, and at once when the window closes", "[settings-ui]")
{
    WindowHarness h;
    REQUIRE_FALSE(AddSetting(h.reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    h.st.selected = "Engine/Render";
    h.Frame();
    h.Frame();
    h.Click("Vsync");                                                   // at clock 0
    const RungWriter write = [&](SetBy rung, const std::vector<std::string>& names) { h.writes.emplace_back(rung, names); return true; };
    CHECK_FALSE(h.archive.Tick(0.3, 500, write));
    CHECK(h.writes.empty());
    CHECK(h.archive.Tick(0.6, 500, write));
    REQUIRE(h.writes.size() == 1);
    CHECK(h.writes[0].first == SetBy::Project);
    CHECK((h.writes[0].second == std::vector<std::string>{ "render.vsync" }));
    h.clock = 1.0;
    h.Click("Vsync");
    h.open = false;                                                     // the window closes
    h.Frame();
    CHECK(h.writes.size() == 2);                                        // flushed at once, no debounce
    CHECK_FALSE(h.archive.Dirty());
}
