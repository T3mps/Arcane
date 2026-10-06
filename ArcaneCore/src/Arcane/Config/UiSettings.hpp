#pragma once

// ui.* (settings arc S6-4; inventory R1): UiSettings' reflection block. The
// plain struct is in UiSettingsData.hpp (ArcaneCrashReporter reads its
// default without the Astra include path). copyFlashSeconds is one value for
// the editor's two Copy buttons and the crash reporter's.

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Config/UiSettingsData.hpp>

namespace Arcane
{
    ARC_REFLECT_TYPE(UiSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "ui", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(UiSettings, copyFlashSeconds)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 0.0, 5.0)
            ARC_REFLECT_ATTR(Tooltip, "How long (seconds) a Copy button reads \"Copied\" after a copy. "
                                         "The crash reporter receives it when it is launched.")
    ARC_END_REFLECT_TYPE()
}
