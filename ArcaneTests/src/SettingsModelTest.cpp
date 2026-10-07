// Settings arc S3-3: the settings windows' PURE tree -- category paths
// derived from dotted names (or given), the root order, the Game/Plugins
// branches, custom page nodes, readable labels. No ImGui.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include <Settings/SettingsModel.hpp>
#include <Settings/SettingsEdit.hpp>
#include <Settings/LayoutSettings.hpp>
#include <Arcane/Project/ProjectManifest.hpp>

#include <algorithm>
#include <optional>
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

TEST_CASE("SettingsModel: Layout page owns both layout settings at the Preferences root", "[settings-ui][editor]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    SettingsModel m;
    m.SetPages({ SettingsPageRef{ SettingScope::PreferencesMachine, "Layout", "Layouts" } });
    m.Rebuild(reg, SettingScope::PreferencesMachine);

    const SettingsTreeNode* layout = m.Find("Layout");
    REQUIRE(layout != nullptr);
    CHECK((layout->cvars == std::vector<std::string>{ "editor.layout.default", "editor.layout.openPanelsAtStart" }));
    CHECK(m.Find("Editor/Layout") == nullptr);
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

namespace
{
    bool Has(const std::vector<std::string>& v, std::string_view n) { return std::find(v.begin(), v.end(), n) != v.end(); }
    const SettingsTreeNode* FindPath(const SettingsTreeNode& node, std::string_view path)
    {
        if (node.path == path) return &node;
        for (const SettingsTreeNode& c : node.children)
            if (const SettingsTreeNode* hit = FindPath(c, path)) return hit;
        return nullptr;
    }
}

TEST_CASE("SettingsModel::Search matches name, display name, help and keywords -- every token, any case", "[settings-ui]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "editor.graph.fitMinZoom", { .type = CVarType::Float32, .def = CVarValue::Float32(0.5f),
        .scope = SettingScope::PreferencesMachine, .keywords = "frame camera", .help = "Smallest zoom a frame-to-fit may pick." }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "editor.undo.maxSteps", { .type = CVarType::Int32, .def = CVarValue::Int32(100),
        .scope = SettingScope::PreferencesProject, .displayName = "History depth", .help = "Undo history depth in steps." }).IsStale());
    SettingsModel m;
    m.Rebuild(reg, SettingScope::PreferencesMachine);
    CHECK((m.Search("FITMIN") == std::vector<std::string>{ "editor.graph.fitMinZoom" }));      // the name, any case
    CHECK(Has(m.Search("history depth"), "editor.undo.maxSteps")); // display name and help; built-in settings can match too
    CHECK_FALSE(Has(m.Search("history depth"), "editor.graph.fitMinZoom"));
    CHECK((m.Search("camera") == std::vector<std::string>{ "editor.graph.fitMinZoom" }));      // keywords
    CHECK((m.Search("zoom pick") == std::vector<std::string>{ "editor.graph.fitMinZoom" }));   // tokens across fields
    CHECK(m.Search("zoom history").empty());                                                  // AND, not OR
    const std::vector<std::string> everything = m.Search("");
    CHECK(Has(everything, "editor.graph.fitMinZoom"));
    CHECK(Has(everything, "editor.undo.maxSteps"));
}

TEST_CASE("SettingsModel::Visible applies Modified, Overridden, Project overrides and the advanced gate", "[settings-ui]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "render.meshCull", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "render.debugMarkers", { .type = CVarType::Bool, .def = CVarValue::Bool(false), .flags = CVarFlags::Dev }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "render.secret", { .type = CVarType::Bool, .def = CVarValue::Bool(false), .flags = CVarFlags::Hidden }).IsStale());
    REQUIRE(reg.SetRung("render.vsync", SetBy::Project, CVarValue::Bool(false), "project"));               // modified
    REQUIRE(reg.Set(reg.Find("render.meshCull"), CVarValue::Bool(true), SetBy::Console) == SetResult::Applied);   // overridden, equal to its default
    reg.Publish();
    SettingsModel m;
    m.Rebuild(reg, SettingScope::Project);
    using F = SettingsModel::Filter;
    const std::vector<std::string> plain = m.Visible(reg, "", F::All, false);
    CHECK(Has(plain, "render.vsync"));
    CHECK(Has(plain, "render.meshCull"));
    CHECK_FALSE(Has(plain, "render.debugMarkers"));
    CHECK_FALSE(Has(plain, "render.secret"));
    const std::vector<std::string> advanced = m.Visible(reg, "", F::All, true);
    CHECK(Has(advanced, "render.debugMarkers"));
    CHECK(Has(advanced, "render.secret"));
    CHECK((m.Visible(reg, "render", F::Modified, false) == std::vector<std::string>{ "render.vsync" }));
    CHECK((m.Visible(reg, "render", F::Overridden, false) == std::vector<std::string>{ "render.meshCull" }));

    REQUIRE(reg.Set(reg.Find("render.debugMarkers"), CVarValue::Bool(true), SetBy::CommandLine) == SetResult::Applied);
    reg.Publish();
    CHECK(Has(m.Visible(reg, "", F::All, false), "render.debugMarkers"));
    CHECK_FALSE(Has(m.Visible(reg, "", F::All, false), "render.secret"));

    REQUIRE_FALSE(AddSetting(reg, "editor.undo.maxSteps", { .type = CVarType::Int32, .def = CVarValue::Int32(100),
        .scope = SettingScope::PreferencesProject }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "editor.graph.fitMinZoom", { .type = CVarType::Float32, .def = CVarValue::Float32(0.5f),
        .scope = SettingScope::PreferencesMachine }).IsStale());
    REQUIRE(reg.SetRung("editor.undo.maxSteps", SetBy::User, CVarValue::Int32(100), "user"));   // a project override equal to the default
    reg.Publish();
    SettingsModel p;
    p.Rebuild(reg, SettingScope::PreferencesMachine);
    CHECK((p.Visible(reg, "editor", F::ProjectOverrides, false) == std::vector<std::string>{ "editor.undo.maxSteps" }));
    CHECK(p.Visible(reg, "editor", F::Modified, false).empty());
}

TEST_CASE("SettingsModel::Pruned keeps hit rows with their ancestors and every page, and drops empty branches", "[settings-ui]")
{
    CVarRegistry reg;
    for (const char* n : { "editor.graph.fitMinZoom", "editor.graph.wire.thickness", "editor.undo.maxSteps" })
        REQUIRE_FALSE(AddSetting(reg, n, { .type = CVarType::Float32, .def = CVarValue::Float32(1.0f),
                                           .scope = SettingScope::PreferencesMachine }).IsStale());
    SettingsModel m;
    m.SetPages({ SettingsPageRef{ SettingScope::PreferencesMachine, "Appearance/Theme", "Theme" } });
    m.Rebuild(reg, SettingScope::PreferencesMachine);
    const SettingsTreeNode t = m.Pruned({ "editor.graph.wire.thickness" });
    REQUIRE(FindPath(t, "Editor/Graph/Wire") != nullptr);
    CHECK((FindPath(t, "Editor/Graph/Wire")->cvars == std::vector<std::string>{ "editor.graph.wire.thickness" }));
    REQUIRE(FindPath(t, "Editor/Graph") != nullptr);
    CHECK(FindPath(t, "Editor/Graph")->cvars.empty());        // fitMinZoom was not kept
    CHECK(FindPath(t, "Editor/Undo") == nullptr);
    CHECK(FindPath(t, "Appearance/Theme") != nullptr);         // a page always stays
}

TEST_CASE("SettingCategoryPath: a slash-only categoryPath is General, not UB", "[settings-ui]")
{
    CVarDescInfo d;
    d.name = "orphan";
    d.categoryPath = "/";
    CHECK(SettingCategoryPath(d, {}) == "Engine/General");
    d.categoryPath = "///";
    CHECK(SettingCategoryPath(d, {}) == "Engine/General");
    d.module = "TestGame";
    CHECK(SettingCategoryPath(d, { .gameModule = "TestGame" }) == "Game/TestGame/General");
}

TEST_CASE("SettingsModel::Rebuild applies a same-size page swap and a roles change with no revision bump", "[settings-ui]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "speed.max", { .type = CVarType::Float32, .def = CVarValue::Float32(5.0f),
                                                .module = "TestGame" }).IsStale());
    SettingsModel m;
    m.SetPages({ SettingsPageRef{ SettingScope::Project, "Project", "Project" } });
    m.Rebuild(reg, SettingScope::Project);
    CHECK(m.Find("Project") != nullptr);
    CHECK(m.Find("Engine/Speed") != nullptr);
    CHECK(m.Find("Identity") == nullptr);
    const std::uint64_t rev = m.BuiltRevision();

    m.SetPages({ SettingsPageRef{ SettingScope::Project, "Identity", "Identity" } });
    m.Rebuild(reg, SettingScope::Project);
    CHECK(m.BuiltRevision() == rev);
    CHECK(m.Find("Identity") != nullptr);
    CHECK(m.Find("Project") == nullptr);

    m.SetModuleRoles({ .gameModule = "TestGame" });
    m.Rebuild(reg, SettingScope::Project);
    CHECK(m.BuiltRevision() == rev);
    CHECK(m.Find("Game/TestGame/Speed") != nullptr);
    CHECK(m.Find("Engine/Speed") == nullptr);
}

// S6-GATE (controller verification, user try-out 2026-10-07): the live
// astra.snapshot.compression row reaches the Preferences window, filed under
// Engine/Astra/Snapshot (its dotted name; "astra" is no known root). It is a
// Dev row, so the default view hides it until Show advanced is on (spec s6.2).
TEST_CASE("SettingsModel: astra.snapshot.compression is a Preferences row under Engine/Astra/Snapshot, behind Show advanced", "[settings-ui][editor]")
{
    const CVarRegistry& reg = CVarRegistry::Get();
    const std::optional<CVarDescInfo> d = reg.Describe("astra.snapshot.compression");
    REQUIRE(d.has_value());
    CHECK(d->scope == SettingScope::PreferencesProject);
    CHECK(HasFlag(d->flags, CVarFlags::Dev));

    SettingsModel prefs;
    prefs.Rebuild(reg, SettingScope::PreferencesMachine);
    const SettingsTreeNode* node = prefs.Find("Engine/Astra/Snapshot");
    REQUIRE(node != nullptr);
    CHECK((node->cvars == std::vector<std::string>{ "astra.snapshot.compression" }));
    REQUIRE(prefs.Desc("astra.snapshot.compression") != nullptr);
    CHECK(SettingDisplayName(*prefs.Desc("astra.snapshot.compression")) == "Snapshot compression");
    using F = SettingsModel::Filter;
    CHECK(Has(prefs.Visible(reg, "snapshot", F::All, /*showAdvanced=*/true), "astra.snapshot.compression"));
    if (!ComputeRowFacts(reg, *d, SettingsWindowKind::Preferences).overridden)   // the Dev gate hides a row nobody overrides
        CHECK_FALSE(Has(prefs.Visible(reg, "snapshot", F::All, /*showAdvanced=*/false), "astra.snapshot.compression"));

    SettingsModel project;
    project.Rebuild(reg, SettingScope::Project);
    CHECK(project.Desc("astra.snapshot.compression") == nullptr);
}
