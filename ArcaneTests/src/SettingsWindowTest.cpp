// Settings arc S3-10: the settings window through the REAL ImGui path --
// the tree, search, selection, window-local Ctrl+Z (focus-gated), the
// Restart bar, "Module unloaded", and debounced writes flushed on close.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include <Input/EditorActions.hpp>
#include <Settings/SettingsWindow.hpp>
#include <Widgets/EditorFonts.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <functional>
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
        CVarRegistry keysReg;              // the shortcut cvars, apart from the window's own tree
        EditorActions keys{ keysReg };
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
        std::vector<SettingsPageRef> pages;
        std::function<void(const std::string&)> drawPage;

        WindowHarness()
        {
            st.grid.probe = &imgui.probe;
            RegisterEditorActions(keys);
            keysReg.Publish();
            keys.RefreshBindings();
        }
        void Rebind(std::string_view action, const char* chord)
        {
            REQUIRE(keysReg.Set(keys.HandleOf(action), CVarValue::String(chord), SetBy::EditorUser, "editor", CVarContext::Editor) == SetResult::Applied);
            keysReg.Publish();
            keys.RefreshBindings();
        }

        SettingsWindowEnv Env()
        {
            SettingsWindowEnv env;
            env.registry = &reg;
            env.kind = kind;
            env.title = "Settings Under Test";
            env.roles = roles;
            env.pages = pages;
            env.drawPage = drawPage;
            env.now = [this] { return clock; };
            env.archive = &archive;
            env.writeRung = [this](SetBy rung, const std::vector<std::string>& names) { writes.emplace_back(rung, names); return true; };
            env.tracker = &tracker;
            env.restartEditor = [this] { ++restarts; };
            env.projectOpen = true;
            env.actions = &keys;
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

TEST_CASE("Settings window: its undo/redo keys are the editor's edit.undo / edit.redo / edit.redoAlt bindings -- a rebind moves them", "[settings-ui][shortcuts]")
{
    // S4-GATE: the window-local undo read raw Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z, so
    // rebinding Undo on the Keyboard page left this window on the old chords.
    WindowHarness h;
    REQUIRE_FALSE(AddSetting(h.reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    h.st.selected = "Engine/Render";
    h.Frame();
    h.Frame();
    const auto edited = [&] { return h.reg.RungValue("render.vsync", SetBy::Project).has_value(); };

    SECTION("the defaults: Ctrl+Z undoes, Ctrl+Y and Ctrl+Shift+Z redo")
    {
        h.Click("Vsync");
        Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Z, [&] { h.Frame(); });
        CHECK_FALSE(edited());
        Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Y, [&] { h.Frame(); });
        CHECK(edited());
        Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Z, [&] { h.Frame(); });
        CHECK_FALSE(edited());
        ImGuiIO& io = ImGui::GetIO();   // Ctrl+Shift+Z: one AddKeyEvent per modifier (PressChord takes one)
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiMod_Shift, true); io.AddKeyEvent(ImGuiKey_Z, true); h.Frame();
        io.AddKeyEvent(ImGuiKey_Z, false); io.AddKeyEvent(ImGuiMod_Shift, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); h.Frame();
        CHECK(edited());
    }
    SECTION("rebound: Ctrl+U undoes and Ctrl+R redoes; the old chords do nothing here")
    {
        h.Rebind("edit.undo", "Ctrl+U");
        h.Rebind("edit.redo", "Ctrl+R");
        h.Click("Vsync");
        Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Z, [&] { h.Frame(); });
        CHECK(edited());
        Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_U, [&] { h.Frame(); });
        CHECK_FALSE(edited());
        Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Y, [&] { h.Frame(); });
        CHECK_FALSE(edited());
        Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_R, [&] { h.Frame(); });
        CHECK(edited());
    }
    SECTION("unbound: no key undoes")
    {
        h.Rebind("edit.undo", "");
        h.Click("Vsync");
        Arcane::Test::PressChord(ImGuiMod_Ctrl, ImGuiKey_Z, [&] { h.Frame(); });
        CHECK(edited());
    }
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

TEST_CASE("Settings window: Rebuild picks up a same-size page swap and a roles change with no revision bump", "[settings-ui]")
{
    WindowHarness h;
    h.pages = { SettingsPageRef{ SettingScope::Project, "Project", "Project" } };
    REQUIRE_FALSE(AddSetting(h.reg, "speed.max", { .type = CVarType::Float32, .def = CVarValue::Float32(5.0f),
                                                  .module = "TestGame" }).IsStale());
    h.Frame();
    h.Frame();
    CHECK(h.TreeHas("Project"));
    CHECK(h.TreeHas("Engine/Speed"));
    CHECK_FALSE(h.TreeHas("Identity"));
    CHECK_FALSE(h.TreeHas("Game/TestGame/Speed"));

    h.pages = { SettingsPageRef{ SettingScope::Project, "Identity", "Identity" } };
    h.Frame();
    CHECK(h.TreeHas("Identity"));
    CHECK_FALSE(h.TreeHas("Project"));

    h.roles.gameModule = "TestGame";
    h.Frame();
    CHECK(h.TreeHas("Game/TestGame/Speed"));
    CHECK_FALSE(h.TreeHas("Engine/Speed"));
}

TEST_CASE("Settings window: a custom page owns its node's keychord rows; the other rows still draw", "[settings-ui]")
{
    WindowHarness h;
    h.kind = SettingsWindowKind::Preferences;
    REQUIRE_FALSE(AddSetting(h.reg, "editor.keys.edit.undo", { .type = CVarType::String, .def = CVarValue::String("Ctrl+Z"),
        .scope = SettingScope::PreferencesMachine, .widget = "keychord", .categoryPath = "Keyboard" }).IsStale());
    REQUIRE_FALSE(AddSetting(h.reg, "editor.keys.repeatDelay", { .type = CVarType::Int32, .def = CVarValue::Int32(300),
        .scope = SettingScope::PreferencesMachine, .categoryPath = "Keyboard" }).IsStale());
    h.st.selected = "Keyboard";
    h.Frame();
    h.Frame();
    const auto drew = [&](std::string_view name)
    {
        return std::find(h.st.last.rows.begin(), h.st.last.rows.end(), name) != h.st.last.rows.end();
    };
    CHECK(h.st.last.page == SettingsWindowState::FrameFacts::Page::Rows);   // no page: the generic chord row draws
    CHECK(h.st.last.rows.size() == 2);
    CHECK(drew("editor.keys.edit.undo"));
    CHECK(drew("editor.keys.repeatDelay"));

    int pageDraws = 0;
    h.pages = { SettingsPageRef{ SettingScope::PreferencesMachine, "Keyboard", "Keyboard Shortcuts" } };
    h.drawPage = [&](const std::string& path) { if (path == "Keyboard") ++pageDraws; };
    h.Frame();
    h.Frame();
    CHECK(pageDraws >= 1);
    CHECK(h.st.last.page == SettingsWindowState::FrameFacts::Page::Custom);
    CHECK((h.st.last.rows == std::vector<std::string>{ "editor.keys.repeatDelay" }));
}

TEST_CASE("Settings window: the Fonts and Scale page feeds its node's font rows; a family pick is a standard row edit (undo, reset, provenance)", "[settings-ui]")
{
    WindowHarness h;
    h.kind = SettingsWindowKind::Preferences;
    REQUIRE_FALSE(AddSetting(h.reg, "editor.ui.fontFamily", { .type = CVarType::String, .def = CVarValue::String("Inter"),
        .scope = SettingScope::PreferencesMachine, .widget = "font", .categoryPath = "Appearance/Fonts and Scale" }).IsStale());
    REQUIRE_FALSE(AddSetting(h.reg, "editor.ui.scale", { .type = CVarType::Float32, .def = CVarValue::Float32(1.0f),
        .scope = SettingScope::PreferencesMachine, .categoryPath = "Appearance/Fonts and Scale" }).IsStale());
    h.st.selected = "Appearance/Fonts and Scale";
    h.Frame();
    h.Frame();
    CHECK(h.st.last.page == SettingsWindowState::FrameFacts::Page::Rows);   // no page: the font row is a text box
    CHECK(h.st.last.rows.size() == 2);

    const std::vector<EditorFontFamily> families{ { "Inter", {}, true }, { "JetBrains Mono", {}, true }, { "Roboto", {}, false } };
    h.pages = { SettingsPageRef{ SettingScope::PreferencesMachine, "Appearance/Fonts and Scale", "Fonts and Scale" } };
    h.drawPage = [&](const std::string&) { SetSettingsFontFamilies(&families); };
    h.Frame();
    h.Frame();
    CHECK(h.st.last.page == SettingsWindowState::FrameFacts::Page::Custom);
    // The page does not own the font rows: both still draw as standard rows.
    CHECK((h.st.last.rows == std::vector<std::string>{ "editor.ui.fontFamily", "editor.ui.scale" }));

    h.Click("Font Family");                     // opens the family combo
    h.Frame();
    h.Frame();
    h.Click("Font Family#font:Roboto  (user)");
    CHECK(h.reg.RungValue("editor.ui.fontFamily", SetBy::EditorUser) == std::optional<CVarValue>(CVarValue::String("Roboto")));
    CHECK(h.archive.Dirty());
    REQUIRE(SettingsUndo(h.st).CanUndo());      // the window's own undo stack
    SettingsUndo(h.st).Undo();
    CHECK_FALSE(h.reg.RungValue("editor.ui.fontFamily", SetBy::EditorUser).has_value());

    // --set editor.ui.fontFamily=Roboto: the row shows the CommandLine rung wins (provenance + Clear override).
    REQUIRE(h.reg.Set(h.reg.Find("editor.ui.fontFamily"), CVarValue::String("Roboto"), SetBy::CommandLine) == SetResult::Applied);
    h.reg.PublishImmediate();
    h.Frame();
    h.Frame();
    CHECK((h.st.last.overridden == std::vector<std::string>{ "editor.ui.fontFamily" }));
}
