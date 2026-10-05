// Settings arc S3-6: the settings windows' debounced writes (spec s6.3) and
// the Restart / next-world trackers behind the bar and the note (s6.5, s12).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include <Settings/SettingsApply.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace Arcane;
using namespace Arcane::Editor;
using Arcane::Test::AddSetting;

TEST_CASE("SettingsArchiveQueue writes each dirty rung once the edits go quiet; Flush writes at once", "[settings-ui]")
{
    std::vector<std::pair<SetBy, std::vector<std::string>>> writes;
    const RungWriter write = [&](SetBy rung, const std::vector<std::string>& names) { writes.emplace_back(rung, names); };
    SettingsArchiveQueue q;
    CHECK_FALSE(q.Tick(10.0, 500, write));                                // nothing dirty
    q.MarkDirty(SetBy::Project, "render.vsync", 0.0);
    q.MarkDirty(SetBy::Project, "render.vsync", 0.1);                     // the same key twice: written once
    q.MarkDirty(SetBy::EditorUser, "editor.theme.accent", 0.2);
    CHECK_FALSE(q.Tick(0.6, 500, write));                                 // 400 ms since the last edit
    CHECK(writes.empty());
    CHECK(q.Tick(0.71, 500, write));
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].first == SetBy::Project);
    CHECK((writes[0].second == std::vector<std::string>{ "render.vsync" }));
    CHECK(writes[1].first == SetBy::EditorUser);
    CHECK_FALSE(q.Dirty());
    CHECK_FALSE(q.Tick(5.0, 500, write));
    q.MarkDirty(SetBy::User, "editor.undo.maxSteps", 6.0);
    q.Flush(write);                                                       // window close / exit
    CHECK(writes.size() == 3);
    q.Flush(write);
    CHECK(writes.size() == 3);                                            // nothing left to write
    q.MarkDirty(SetBy::User, "editor.undo.maxSteps", 7.0);
    CHECK(q.Tick(7.0, 0, write));                                         // a 0 ms debounce writes on the same frame
}

TEST_CASE("SettingsApplyTracker: Restart rows count against boot, NextWorld rows against the last world", "[settings-ui]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "render.backend", { .type = CVarType::Int32, .def = CVarValue::Int32(0), .apply = ApplyMode::Restart }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "physics.substeps", { .type = CVarType::Int32, .def = CVarValue::Int32(4), .apply = ApplyMode::NextWorld }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "render.meshCull", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    SettingsApplyTracker t;
    t.Observe(reg);
    CHECK(t.PendingRestart(reg).empty());
    CHECK(t.PendingNextWorld(reg).empty());
    REQUIRE(reg.SetRung("render.backend", SetBy::Project, CVarValue::Int32(1), "project"));
    REQUIRE(reg.SetRung("physics.substeps", SetBy::Project, CVarValue::Int32(8), "project"));
    REQUIRE(reg.SetRung("render.meshCull", SetBy::Project, CVarValue::Bool(false), "project"));
    reg.Publish();
    CHECK((t.PendingRestart(reg) == std::vector<std::string>{ "render.backend" }));
    CHECK((t.PendingNextWorld(reg) == std::vector<std::string>{ "physics.substeps" }));   // Live rows are never tracked
    t.Observe(reg);                                                        // re-observing never moves a baseline
    CHECK(t.PendingRestart(reg).size() == 1);
    t.WorldCreated(reg);
    CHECK(t.PendingNextWorld(reg).empty());
    CHECK(t.PendingRestart(reg).size() == 1);                              // only a restart clears a restart
    REQUIRE(reg.RevertRung("render.backend", SetBy::Project));
    reg.Publish();
    CHECK(t.PendingRestart(reg).empty());                                  // reverted before restarting: the bar goes (spec s12)
    REQUIRE(reg.SetRung("physics.substeps", SetBy::Project, CVarValue::Int32(2), "project"));
    reg.Publish();
    reg.UnregisterModule("test");
    CHECK(t.PendingNextWorld(reg).empty());                                // an unregistered cvar is dropped, not reported
}

TEST_CASE("editor.settings.saveDebounceMs is a 500 ms machine-wide editor preference", "[settings-ui]")
{
    CHECK(cvar_settingsSaveDebounceMs.Get() == 500);
    const std::optional<CVarDescInfo> d = CVarRegistry::Get().Describe("editor.settings.saveDebounceMs");
    REQUIRE(d.has_value());
    CHECK(d->audience == Audience::Editor);
    CHECK(d->scope == SettingScope::PreferencesMachine);
    CHECK(d->apply == ApplyMode::Live);
    CHECK(d->min == std::optional<CVarValue>(CVarValue::Int32(0)));
    CHECK(d->max == std::optional<CVarValue>(CVarValue::Int32(10000)));
}
