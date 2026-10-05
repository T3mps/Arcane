#pragma once

// The Preferences window's per-row "All projects / This project" switch
// (settings arc S2, spec s3.3; S3 draws it). Every Preferences row has it
// (PreferencesMachine and PreferencesProject); Project rows do not:
//   All projects -> edits write the EditorUser rung (the machine folder);
//   This project -> edits write the User rung (this project's Saved/Config),
//                   which beats the machine value.
// Machine-wide rows default to All projects; per-project rows default to
// This project. Choosing All projects on a per-project row promotes the
// value to EditorUser. Switching back clears the other rung. The User
// archive then erases its key too (WriteCVarArchive, SetBy::User).

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Core/Api.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    enum class PreferenceTarget : std::uint8_t { AllProjects, ThisProject };

    ARC_CORE_API SetBy PreferenceRung(SettingScope scope, PreferenceTarget target) noexcept;

    // ThisProject when the User rung holds a value; else AllProjects when
    // EditorUser holds one; else the home-scope default (PreferencesProject
    // -> ThisProject, PreferencesMachine -> AllProjects).
    ARC_CORE_API PreferenceTarget PreferenceTargetOf(const CVarRegistry& registry, std::string_view name);

    // Flip the switch on a Preferences row.
    // - ThisProject copies the cvar's pending value into the User rung, so the row keeps its value.
    //   On PreferencesProject, also drops the EditorUser record.
    // - AllProjects drops the User record. PreferencesProject also promotes
    //   pending onto EditorUser first so the switch stays AllProjects (its
    //   home default is ThisProject). PreferencesMachine never promotes: a
    //   project override is cleared, not made machine-wide.
    // Returns Stale for an unknown name, and Denied for a Project-scope cvar.
    // Does not publish.
    ARC_CORE_API SetResult SetPreferenceTarget(CVarRegistry& registry, std::string_view name, PreferenceTarget target);

    // Write `value` on the rung this row's edits belong to:
    // PreferenceRung(scope, PreferenceTargetOf(name)), in the Editor context. Does not publish.
    ARC_CORE_API SetResult EditPreference(CVarRegistry& registry, std::string_view name, CVarValue value);

    // The Modified filter's "Project overrides": every Preferences cvar the User rung holds, sorted.
    ARC_CORE_API std::vector<std::string> ProjectOverrides(const CVarRegistry& registry);
}
