// Settings arc S3-4: the settings windows' pure edit core -- which rung an
// edit writes, the All projects / This project switch, reset, Clear
// override, and the undo commands they return. No ImGui.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include <Settings/SettingsEdit.hpp>
#include <Arcane/Config/CVarFormat.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace Arcane;
using namespace Arcane::Editor;
using Arcane::Test::AddSetting;

namespace
{
    std::optional<CVarValue> V(CVarValue v) { return std::optional<CVarValue>(std::move(v)); }
}

TEST_CASE("Project Settings edits the Project rung; undo and redo restore it exactly", "[settings-ui]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    const CVarDescInfo d = *reg.Describe("render.vsync");
    std::vector<RungChange> seen;
    const SettingsEditSink sink = [&](const RungChange& c) { seen.push_back(c); };
    const RowFacts before = ComputeRowFacts(reg, d, SettingsWindowKind::Project);
    CHECK(before.target == SetBy::Project);
    CHECK_FALSE(before.modified);
    CHECK_FALSE(before.overridden);

    std::unique_ptr<SettingEditCommand> step = EditSetting(reg, d, SettingsWindowKind::Project, CVarValue::Bool(false), sink);
    REQUIRE(step);
    CHECK(reg.RungValue("render.vsync", SetBy::Project) == V(CVarValue::Bool(false)));
    CHECK(ComputeRowFacts(reg, d, SettingsWindowKind::Project).modified);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].rung == SetBy::Project);
    CHECK_FALSE(seen[0].before.has_value());
    CHECK(std::string(step->Label()) == "Edit Vsync");
    CHECK_FALSE(step->AffectsScene());
    step->Undo();
    CHECK_FALSE(reg.RungValue("render.vsync", SetBy::Project).has_value());
    step->Redo();
    CHECK(reg.RungValue("render.vsync", SetBy::Project) == V(CVarValue::Bool(false)));
    CHECK(EditSetting(reg, d, SettingsWindowKind::Project, CVarValue::Bool(false), sink) == nullptr);   // no change, no step
}

TEST_CASE("An overridden row refuses edits; Clear override pops the winning rung, undoably", "[settings-ui]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    const CVarDescInfo d = *reg.Describe("render.vsync");
    const SettingsEditSink sink = [](const RungChange&) {};
    REQUIRE(reg.Set(reg.Find("render.vsync"), CVarValue::Bool(false), SetBy::User, "user") == SetResult::Applied);
    reg.Publish();
    const RowFacts f = ComputeRowFacts(reg, d, SettingsWindowKind::Project);
    CHECK(f.overridden);
    CHECK(f.winner == SetBy::User);
    CHECK(f.effective == CVarValue::Bool(false));
    CHECK(EditSetting(reg, d, SettingsWindowKind::Project, CVarValue::Bool(true), sink) == nullptr);   // shown, not edited (spec s12)
    std::unique_ptr<SettingEditCommand> clear = ClearOverride(reg, d, SettingsWindowKind::Project, sink);
    REQUIRE(clear);
    CHECK_FALSE(reg.RungValue("render.vsync", SetBy::User).has_value());
    CHECK_FALSE(ComputeRowFacts(reg, d, SettingsWindowKind::Project).overridden);
    clear->Undo();
    CHECK(reg.RungValue("render.vsync", SetBy::User) == V(CVarValue::Bool(false)));
    CHECK(ClearOverride(reg, *reg.Describe("render.vsync"), SettingsWindowKind::Preferences, sink) == nullptr);   // a Preferences row targets User: nothing above it
}

TEST_CASE("Preferences rows target User or EditorUser by mode; values clamp to the declared range", "[settings-ui]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "editor.undo.maxSteps", { .type = CVarType::Int32, .def = CVarValue::Int32(100),
        .scope = SettingScope::PreferencesProject, .min = CVarValue::Int32(1), .max = CVarValue::Int32(10000) }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "editor.graph.fitMinZoom", { .type = CVarType::Float32, .def = CVarValue::Float32(0.5f),
        .scope = SettingScope::PreferencesMachine }).IsStale());
    const CVarDescInfo p = *reg.Describe("editor.undo.maxSteps");
    const CVarDescInfo m = *reg.Describe("editor.graph.fitMinZoom");
    const SettingsEditSink sink = [](const RungChange&) {};
    const RowFacts pf = ComputeRowFacts(reg, p, SettingsWindowKind::Preferences);
    CHECK(pf.mode == PrefMode::ThisProject);
    CHECK(pf.target == SetBy::User);
    const RowFacts mf = ComputeRowFacts(reg, m, SettingsWindowKind::Preferences);
    CHECK(mf.mode == PrefMode::AllProjects);
    CHECK(mf.target == SetBy::EditorUser);
    REQUIRE(EditSetting(reg, m, SettingsWindowKind::Preferences, CVarValue::Float32(0.8f), sink));
    CHECK(reg.RungValue("editor.graph.fitMinZoom", SetBy::EditorUser) == V(CVarValue::Float32(0.8f)));
    REQUIRE(EditSetting(reg, p, SettingsWindowKind::Preferences, CVarValue::Int32(99999), sink));
    CHECK(reg.RungValue("editor.undo.maxSteps", SetBy::User) == V(CVarValue::Int32(10000)));
}

TEST_CASE("SwitchPrefMode: This project copies the shown value; All projects clears it, promoting only when nothing is shared", "[settings-ui]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "editor.graph.fitMinZoom", { .type = CVarType::Float32, .def = CVarValue::Float32(0.5f),
        .scope = SettingScope::PreferencesMachine }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "editor.undo.maxSteps", { .type = CVarType::Int32, .def = CVarValue::Int32(100),
        .scope = SettingScope::PreferencesProject }).IsStale());
    const CVarDescInfo m = *reg.Describe("editor.graph.fitMinZoom");
    const CVarDescInfo p = *reg.Describe("editor.undo.maxSteps");
    const SettingsEditSink sink = [](const RungChange&) {};
    REQUIRE(reg.SetRung("editor.graph.fitMinZoom", SetBy::EditorUser, CVarValue::Float32(0.8f), "editor-user"));

    REQUIRE(SwitchPrefMode(reg, m, PrefMode::ThisProject, sink));
    CHECK(reg.RungValue("editor.graph.fitMinZoom", SetBy::User) == V(CVarValue::Float32(0.8f)));
    CHECK(ComputeRowFacts(reg, m, SettingsWindowKind::Preferences).mode == PrefMode::ThisProject);
    CHECK(ComputeRowFacts(reg, m, SettingsWindowKind::Preferences).projectOverride);
    REQUIRE(reg.SetRung("editor.graph.fitMinZoom", SetBy::User, CVarValue::Float32(1.5f), "user"));
    REQUIRE(SwitchPrefMode(reg, m, PrefMode::AllProjects, sink));
    CHECK_FALSE(reg.RungValue("editor.graph.fitMinZoom", SetBy::User).has_value());
    CHECK(reg.RungValue("editor.graph.fitMinZoom", SetBy::EditorUser) == V(CVarValue::Float32(0.8f)));   // cleared, not promoted

    REQUIRE(reg.SetRung("editor.undo.maxSteps", SetBy::User, CVarValue::Int32(42), "user"));
    std::unique_ptr<SettingEditCommand> promote = SwitchPrefMode(reg, p, PrefMode::AllProjects, sink);
    REQUIRE(promote);
    CHECK(reg.RungValue("editor.undo.maxSteps", SetBy::EditorUser) == V(CVarValue::Int32(42)));
    CHECK_FALSE(reg.RungValue("editor.undo.maxSteps", SetBy::User).has_value());
    CHECK(ComputeRowFacts(reg, p, SettingsWindowKind::Preferences).mode == PrefMode::AllProjects);   // the switch stays put
    promote->Undo();
    CHECK(reg.RungValue("editor.undo.maxSteps", SetBy::User) == V(CVarValue::Int32(42)));
    CHECK_FALSE(reg.RungValue("editor.undo.maxSteps", SetBy::EditorUser).has_value());
    CHECK(SwitchPrefMode(reg, p, PrefMode::ThisProject, sink) == nullptr);   // already This project
}

TEST_CASE("ResetSetting drops the target rung, and pins the default there when a lower rung would show through", "[settings-ui]")
{
    CVarRegistry reg;
    const SettingsEditSink sink = [](const RungChange&) {};
    REQUIRE_FALSE(AddSetting(reg, "render.lod", { .type = CVarType::Int32, .def = CVarValue::Int32(1) }).IsStale());
    REQUIRE(reg.SetRung("render.lod", SetBy::EngineConfig, CVarValue::Int32(3), "engine-config"));
    REQUIRE(reg.SetRung("render.lod", SetBy::Project, CVarValue::Int32(5), "project"));
    reg.Publish();
    const CVarDescInfo d = *reg.Describe("render.lod");
    std::unique_ptr<SettingEditCommand> r = ResetSetting(reg, d, SettingsWindowKind::Project, sink);
    REQUIRE(r);
    CHECK(reg.RungValue("render.lod", SetBy::Project) == V(CVarValue::Int32(1)));
    CHECK_FALSE(ComputeRowFacts(reg, d, SettingsWindowKind::Project).modified);
    r->Undo();
    CHECK(reg.RungValue("render.lod", SetBy::Project) == V(CVarValue::Int32(5)));

    REQUIRE_FALSE(AddSetting(reg, "render.plain", { .type = CVarType::Int32, .def = CVarValue::Int32(1) }).IsStale());
    REQUIRE(reg.SetRung("render.plain", SetBy::Project, CVarValue::Int32(7), "project"));
    const CVarDescInfo plain = *reg.Describe("render.plain");
    REQUIRE(ResetSetting(reg, plain, SettingsWindowKind::Project, sink));
    CHECK_FALSE(reg.RungValue("render.plain", SetBy::Project).has_value());   // nothing underneath: the record just goes
    CHECK(ResetSetting(reg, plain, SettingsWindowKind::Project, sink) == nullptr);   // already the default
}

TEST_CASE("Rung labels and sources, value formatting, and a step expiring with its module", "[settings-ui]")
{
    CHECK(std::string(RungLabel(SetBy::EditorUser)) == "Editor (all projects)");
    CHECK(std::string(RungLabel(SetBy::User)) == "User (this project)");
    CHECK(std::string(RungLabel(SetBy::CommandLine)) == "Command line");
    CHECK(RungSource(SetBy::Project) == "project");
    CHECK(RungSource(SetBy::User) == "user");
    CHECK(RungSource(SetBy::EditorUser) == "editor-user");
    CHECK(FormatSettingValue(CVarValue::Enum(1), { "Orbit", "Fly" }) == "Fly");
    CHECK(FormatSettingValue(CVarValue::Enum(7), { "Orbit" }) == "7");
    CHECK(FormatSettingValue(CVarValue::Bool(true), {}) == "true");

    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "speed.max", { .type = CVarType::Float32, .def = CVarValue::Float32(5.0f), .module = "TestGame" }).IsStale());
    std::unique_ptr<SettingEditCommand> step = EditSetting(reg, *reg.Describe("speed.max"), SettingsWindowKind::Project,
                                                           CVarValue::Float32(6.0f), [](const RungChange&) {});
    REQUIRE(step);
    CHECK_FALSE(step->IsExpired());
    reg.UnregisterModule("TestGame");
    CHECK(step->IsExpired());   // the stack skips it rather than spend a Ctrl+Z on it
}

TEST_CASE("a Color at its hex-round-tripped default is not modified", "[settings-ui]")
{
    // S1-6 ruling: "is default" uses CVarColorNearlyEqual (Review Focus 3).
    CVarRegistry reg;
    const CVarColor def{ 0.45f, 0.7f, 0.8f, 1.0f };
    REQUIRE_FALSE(AddSetting(reg, "look.light", { .type = CVarType::Color, .def = CVarValue::Color(def) }).IsStale());
    const CVarDescInfo d = *reg.Describe("look.light");
    const SettingsEditSink sink = [](const RungChange&) {};
    const CVarColor reloaded = *CVarColorFromHex(CVarColorToHex(def));
    REQUIRE(reg.SetRung("look.light", SetBy::Project, CVarValue::Color(reloaded), "project"));
    CHECK_FALSE(ComputeRowFacts(reg, d, SettingsWindowKind::Project).modified);
    CHECK(ResetSetting(reg, d, SettingsWindowKind::Project, sink) == nullptr);

    std::string hex = CVarColorToHex(def);
    REQUIRE(hex.size() >= 3);
    const unsigned r = (static_cast<unsigned>(std::stoul(hex.substr(1, 2), nullptr, 16)) + 2u) & 0xFFu;
    static constexpr char kDigits[] = "0123456789ABCDEF";
    hex[1] = kDigits[r >> 4];
    hex[2] = kDigits[r & 15u];
    REQUIRE(reg.SetRung("look.light", SetBy::Project, CVarValue::Color(*CVarColorFromHex(hex)), "project"));
    CHECK(ComputeRowFacts(reg, d, SettingsWindowKind::Project).modified);
}
