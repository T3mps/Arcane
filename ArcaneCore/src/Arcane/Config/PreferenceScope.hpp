#pragma once

// The Preferences window's per-row "All projects / This project" switch
// (settings arc S2, spec s3.3; S3 draws it). Only PreferencesMachine settings
// have it:
//   All projects -> edits write the EditorUser rung (the machine folder);
//   This project -> edits write the User rung (this project's Saved/Config),
//                   which beats the machine value.
// Switching back to All projects clears the project value. The User archive
// then erases its key too (WriteCVarArchive, SetBy::User).
// PreferencesProject settings always edit the User rung, and Project settings
// edit the Project rung.

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

    // ThisProject when `name` is a PreferencesMachine cvar the User rung holds a value for; else AllProjects.
    ARC_CORE_API PreferenceTarget PreferenceTargetOf(const CVarRegistry& registry, std::string_view name);

    // Flip the switch.
    // - ThisProject copies the cvar's pending value into the User rung, so the row keeps its value.
    // - AllProjects drops the User record.
    // Returns Stale for an unknown name, and Denied for a cvar that is not PreferencesMachine.
    // Does not publish.
    ARC_CORE_API SetResult SetPreferenceTarget(CVarRegistry& registry, std::string_view name, PreferenceTarget target);

    // Write `value` on the rung this row's edits belong to:
    // PreferenceRung(scope, PreferenceTargetOf(name)), in the Editor context. Does not publish.
    ARC_CORE_API SetResult EditPreference(CVarRegistry& registry, std::string_view name, CVarValue value);

    // The Modified filter's "Project overrides": every PreferencesMachine cvar the User rung holds, sorted.
    ARC_CORE_API std::vector<std::string> ProjectOverrides(const CVarRegistry& registry);
}
