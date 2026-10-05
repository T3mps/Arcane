// The Preferences row switch (settings arc S2, spec s3.3; the UI is S3):
// "All projects" edits a machine-wide preference on the EditorUser rung, "This
// project" on the User rung, and switching back clears the project value.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/PreferenceScope.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace Arcane;

namespace
{
    CVarHandle RegisterPref(CVarRegistry& reg, std::string_view name, SettingScope scope, std::int32_t def)
    {
        CVarDesc d;
        d.name = name;
        d.type = CVarType::Int32;
        d.defaultValue = CVarValue::Int32(def);
        d.flags = CVarFlags::Archive;
        d.help = "Preference switch probe.";
        d.module = "preference-test";
        d.audience = Audience::Editor;
        d.scope = scope;
        return reg.Register(d);
    }

    bool HoldsAt(const CVarRegistry& reg, std::string_view name, SetBy rung, std::int32_t value)
    {
        const auto e = reg.Explain(name);
        if (!e) return false;
        for (const CVarHistoryRecord& h : e->history)
            if (h.by == rung && h.value == CVarValue::Int32(value))
                return true;
        return false;
    }
}

TEST_CASE("PreferenceRung maps scope x target onto the rung an edit writes", "[settings][cvar]")
{
    CHECK(PreferenceRung(SettingScope::PreferencesMachine, PreferenceTarget::AllProjects) == SetBy::EditorUser);
    CHECK(PreferenceRung(SettingScope::PreferencesMachine, PreferenceTarget::ThisProject) == SetBy::User);
    CHECK(PreferenceRung(SettingScope::PreferencesProject, PreferenceTarget::AllProjects) == SetBy::User);
    CHECK(PreferenceRung(SettingScope::PreferencesProject, PreferenceTarget::ThisProject) == SetBy::User);
    CHECK(PreferenceRung(SettingScope::Project, PreferenceTarget::AllProjects) == SetBy::Project);
    CHECK(PreferenceRung(SettingScope::Project, PreferenceTarget::ThisProject) == SetBy::Project);
}

TEST_CASE("All projects / This project moves a machine-wide preference's edits between EditorUser and User", "[settings][cvar]")
{
    CVarRegistry reg;
    const CVarHandle h = RegisterPref(reg, "pref.theme", SettingScope::PreferencesMachine, 1);
    REQUIRE_FALSE(h.IsStale());
    CHECK(PreferenceTargetOf(reg, "pref.theme") == PreferenceTarget::AllProjects);
    REQUIRE(EditPreference(reg, "pref.theme", CVarValue::Int32(2)) == SetResult::Applied);
    reg.Publish();
    CHECK(reg.Explain("pref.theme")->setBy == SetBy::EditorUser);

    REQUIRE(SetPreferenceTarget(reg, "pref.theme", PreferenceTarget::ThisProject) == SetResult::Applied);
    CHECK(PreferenceTargetOf(reg, "pref.theme") == PreferenceTarget::ThisProject);
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 2);                       // the row keeps its value when it switches
    CHECK(reg.Explain("pref.theme")->setBy == SetBy::User);

    REQUIRE(EditPreference(reg, "pref.theme", CVarValue::Int32(5)) == SetResult::Applied);
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 5);
    CHECK(HoldsAt(reg, "pref.theme", SetBy::EditorUser, 2));   // the machine value is untouched underneath
    CHECK(ProjectOverrides(reg) == std::vector<std::string>{ "pref.theme" });

    REQUIRE(SetPreferenceTarget(reg, "pref.theme", PreferenceTarget::AllProjects) == SetResult::Applied);
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 2);
    CHECK(PreferenceTargetOf(reg, "pref.theme") == PreferenceTarget::AllProjects);
    CHECK(ProjectOverrides(reg).empty());
}

TEST_CASE("Only machine-wide preferences have the switch; per-project and Project rows edit their own rung", "[settings][cvar]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(RegisterPref(reg, "pref.undo", SettingScope::PreferencesProject, 1).IsStale());
    REQUIRE_FALSE(RegisterPref(reg, "pref.shared", SettingScope::Project, 1).IsStale());
    CHECK(SetPreferenceTarget(reg, "pref.undo", PreferenceTarget::ThisProject) == SetResult::Denied);
    REQUIRE(EditPreference(reg, "pref.undo", CVarValue::Int32(9)) == SetResult::Applied);
    REQUIRE(EditPreference(reg, "pref.shared", CVarValue::Int32(3)) == SetResult::Applied);
    reg.Publish();
    CHECK(reg.Explain("pref.undo")->setBy == SetBy::User);
    CHECK(reg.Explain("pref.shared")->setBy == SetBy::Project);
    CHECK(SetPreferenceTarget(reg, "pref.missing", PreferenceTarget::ThisProject) == SetResult::Stale);
    CHECK(EditPreference(reg, "pref.missing", CVarValue::Int32(1)) == SetResult::Stale);
}
