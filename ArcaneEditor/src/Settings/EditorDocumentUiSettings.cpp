#include "Settings/EditorDocumentUiSettings.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(InputEditorSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.input", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(InputEditorSettings, rebindTimeoutSeconds)
            ARC_REFLECT_ATTR(DisplayName, "Rebind timeout") ARC_REFLECT_ATTR(Category, "Rebinding")
            ARC_REFLECT_ATTR(Range, 1.0, 60.0)
            ARC_REFLECT_ATTR(Keywords, "capture listen seconds countdown binding")
            ARC_REFLECT_ATTR(Tooltip, "Seconds the Input Actions editor listens for a key, button or axis after "
                                      "Rebind... or + Binding before it gives up. Applies to the next capture.")
        ARC_REFLECT_FIELD(InputEditorSettings, liveHighlightBase)
            ARC_REFLECT_ATTR(DisplayName, "Live highlight base") ARC_REFLECT_ATTR(Category, "Live preview")
            ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Keywords, "glow wash alpha opacity firing preview")
            ARC_REFLECT_ATTR(Tooltip, "Opacity of the amber wash on a binding row that is firing in the live "
                                      "preview, at its faintest signal.")
        ARC_REFLECT_FIELD(InputEditorSettings, liveHighlightGain)
            ARC_REFLECT_ATTR(DisplayName, "Live highlight gain") ARC_REFLECT_ATTR(Category, "Live preview")
            ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Keywords, "glow wash alpha opacity firing preview strength")
            ARC_REFLECT_ATTR(Tooltip, "Opacity the amber wash gains as a binding row's live signal rises to full "
                                      "strength. Base plus gain is the wash at full signal.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(InputEditorSettings);

    ARC_REFLECT_TYPE(CrashViewerSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.crash", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(CrashViewerSettings, maxRows)
            ARC_REFLECT_ATTR(DisplayName, "Stack rows")
            ARC_REFLECT_ATTR(Range, 4.0, 200.0)
            ARC_REFLECT_ATTR(Keywords, "crash report frames call stack height lines scroll")
            ARC_REFLECT_ATTR(Tooltip, "Text rows the crash viewer's stack-frame list grows to before it scrolls.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(CrashViewerSettings);
}
