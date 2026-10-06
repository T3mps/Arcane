#pragma once

// The Input Actions editor's and the crash viewer's tuning as cvars (settings
// sweep S6-40, inventory Part 3 "Documents"): editor.input.* (the rebind
// capture's timeout and the live-highlight wash a firing binding row paints)
// and editor.crash.* (the frame list's height cap). Per-machine editor
// preferences, Live.
//
// Plain structs, no settings machinery: ARC_SETTINGS and the reflection live in
// EditorDocumentUiSettings.cpp. Every default is the pre-sweep literal it
// replaced.

#include <algorithm>
#include <cstdint>

namespace Arcane::Editor
{
    // editor.input.*
    struct InputEditorSettings
    {
        float rebindTimeoutSeconds = 10.0f;   // a rebind / + Binding capture gives up after this long
        float liveHighlightBase    = 0.12f;   // the wash's alpha at the faintest live signal
        float liveHighlightGain    = 0.2f;    // alpha added at full signal (the signal saturates at 1)
    };

    // editor.crash.*
    struct CrashViewerSettings
    {
        std::int32_t maxRows = 24;   // text rows the stack-frame list grows to before it scrolls
    };

    // The live-highlight wash's alpha for a binding row whose live signal is v
    // (>= 0; saturates at 1). At the defaults: the pre-sweep 0.12f + 0.2f * min(v, 1).
    [[nodiscard]] inline float LiveHighlightAlpha(const InputEditorSettings& s, float v) noexcept
    {
        return s.liveHighlightBase + s.liveHighlightGain * std::min(v, 1.0f);
    }
}
