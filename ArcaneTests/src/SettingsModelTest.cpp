// Settings arc S3-3: the settings windows' PURE tree -- category paths
// derived from dotted names (or given), the root order, the Game/Plugins
// branches, custom page nodes, readable labels. No ImGui.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include <Settings/SettingsModel.hpp>
#include <Arcane/Project/ProjectManifest.hpp>

#include <string>
#include <vector>

using namespace Arcane;
using namespace Arcane::Editor;
using Arcane::Test::AddSetting;

TEST_CASE("SettingsModel: the Project window's tree from category paths -- Project, Engine, Game, Plugins in order", "[settings-ui]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "physics.solver.substeps", { .type = CVarType::Int32, .def = CVarValue::Int32(4) }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true), .order = 1 }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "render.meshCull", { .type = CVarType::Bool, .def = CVarValue::Bool(true), .order = 0 }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "speed.max", { .type = CVarType::Float32, .def = CVarValue::Float32(5.0f), .module = "TestGame" }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "foo.bar", { .type = CVarType::Int32, .def = CVarValue::Int32(1), .module = "MyPlugin" }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "editor.graph.fitMinZoom", { .type = CVarType::Float32, .def = CVarValue::Float32(0.5f),
                                                              .scope = SettingScope::PreferencesMachine }).IsStale());
    SettingsModel m;
    m.SetModuleRoles({ .gameModule = "TestGame", .plugins = { "MyPlugin" } });
    m.SetPages({ SettingsPageRef{ SettingScope::Project, "Project", "Project" } });
    m.Rebuild(reg, SettingScope::Project);

    std::vector<std::string> roots;
    for (const SettingsTreeNode& c : m.Tree().children) roots.push_back(c.label);
    CHECK((roots == std::vector<std::string>{ "Project", "Engine", "Game", "Plugins" }));
    const SettingsTreeNode* solver = m.Find("Engine/Physics/Solver");
    REQUIRE(solver != nullptr);
    CHECK(solver->label == "Solver");
    CHECK((solver->cvars == std::vector<std::string>{ "physics.solver.substeps" }));
    const SettingsTreeNode* render = m.Find("Engine/Render");
    REQUIRE(render != nullptr);
    CHECK((render->cvars == std::vector<std::string>{ "render.meshCull", "render.vsync" }));   // by order, then name
    REQUIRE(m.Find("Game/TestGame/Speed") != nullptr);
    CHECK((m.Find("Game/TestGame/Speed")->cvars == std::vector<std::string>{ "speed.max" }));
    REQUIRE(m.Find("Plugins/MyPlugin/Foo") != nullptr);
    REQUIRE(m.Find("Project") != nullptr);
    CHECK(m.Find("Project")->cvars.empty());                   // a page node: no rows of its own
    CHECK(m.Find("Editor/Graph") == nullptr);                  // a Preferences setting is not in this window
    CHECK(m.Desc("render.vsync") != nullptr);
    CHECK(m.BuiltRevision() == reg.Revision());
}

TEST_CASE("SettingsModel: the Preferences window takes both Preferences scopes, and an explicit categoryPath wins", "[settings-ui]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "editor.undo.maxSteps", { .type = CVarType::Int32, .def = CVarValue::Int32(100),
                                                           .scope = SettingScope::PreferencesProject }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "editor.graph.fitMinZoom", { .type = CVarType::Float32, .def = CVarValue::Float32(0.5f),
                                                              .scope = SettingScope::PreferencesMachine }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "editor.theme.accent", { .type = CVarType::Color, .def = CVarValue::Color(CVarColor{ 1.0f, 0.5f, 0.0f, 1.0f }),
                                                          .scope = SettingScope::PreferencesMachine, .categoryPath = "Appearance/Theme" }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    SettingsModel m;
    m.Rebuild(reg, SettingScope::PreferencesProject);   // either Preferences scope names the same window
    CHECK(m.Find("Editor/Undo") != nullptr);
    CHECK(m.Find("Editor/Graph") != nullptr);
    REQUIRE(m.Find("Appearance/Theme") != nullptr);
    CHECK((m.Find("Appearance/Theme")->cvars == std::vector<std::string>{ "editor.theme.accent" }));
    CHECK(m.Find("Engine/Render") == nullptr);
    CHECK(m.Tree().children.front().label == "Appearance");
}

TEST_CASE("DisplayWord and SettingDisplayName derive readable labels", "[settings-ui]")
{
    CHECK(DisplayWord("maxSteps") == "Max Steps");
    CHECK(DisplayWord("undo") == "Undo");
    CHECK(DisplayWord("meshCull") == "Mesh Cull");
    CHECK(DisplayWord("grid3D") == "Grid3D");
    CHECK(DisplayWord("hdr_enabled") == "Hdr Enabled");
    CVarDescInfo d;
    d.name = "editor.graph.fitMinZoom";
    CHECK(SettingDisplayName(d) == "Fit Min Zoom");
    d.displayName = "Fit zoom floor";
    CHECK(SettingDisplayName(d) == "Fit zoom floor");
}

TEST_CASE("RolesForManifest takes the game module's stem and the enabled plugins", "[settings-ui]")
{
    ProjectManifest m;
    m.gameModule = "ReferenceGame.dll";
    m.plugins = { ProjectManifest::PluginRef{ "Alpha", true }, ProjectManifest::PluginRef{ "Beta", false } };
    const SettingsModuleRoles r = RolesForManifest(m);
    CHECK(r.gameModule == "ReferenceGame");
    CHECK((r.plugins == std::vector<std::string>{ "Alpha" }));
    CHECK(RolesForManifest(ProjectManifest{}).gameModule.empty());
}
