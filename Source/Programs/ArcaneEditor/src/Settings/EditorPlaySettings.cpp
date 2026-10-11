#include "Settings/EditorPlaySettings.hpp"

#include "Settings/SettingsEdit.hpp"   // RungSource
#include "Settings/SettingsHost.hpp"   // NoteSettingEdited

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(EditorPlaySettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.play", SettingScope::PreferencesProject, ApplyMode::NextWorld, Audience::Editor)
        ARC_REFLECT_FIELD(EditorPlaySettings, launchMode)
            ARC_REFLECT_ATTR(DisplayName, "Play launch mode")
            ARC_REFLECT_ATTR(Tooltip, "Where the toolbar's Play button runs the game: in the viewport, in a separate "
                                      "window, as a listen or embedded server, or against a separate server process.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(LayoutFactorySettings)
        // Under the Layout page's node (the root "Layout", beside
        // editor.layout.default), not a derived Editor/Layout branch.
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.layout.factory", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor,
                              "Layout/Factory")
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(LayoutFactorySettings, inspectorWidth)
            ARC_REFLECT_ATTR(DisplayName, "Inspector width") ARC_REFLECT_ATTR(Range, 16.0, 8192.0)
            ARC_REFLECT_ATTR(Tooltip, "Width (pixels) of the main Inspector column in the default layout. Applies at the next Reset Layout.")
        ARC_REFLECT_FIELD(LayoutFactorySettings, outlinerWidth)
            ARC_REFLECT_ATTR(DisplayName, "Outliner width") ARC_REFLECT_ATTR(Range, 16.0, 8192.0)
            ARC_REFLECT_ATTR(Tooltip, "Width (pixels) of the Outliner in the default layout. Applies at the next Reset Layout.")
        ARC_REFLECT_FIELD(LayoutFactorySettings, bottomBand)
            ARC_REFLECT_ATTR(DisplayName, "Bottom band height") ARC_REFLECT_ATTR(Range, 16.0, 8192.0)
            ARC_REFLECT_ATTR(Tooltip, "Height (pixels) of the asset and console band in the default layout. Applies at the next Reset Layout.")
        ARC_REFLECT_FIELD(LayoutFactorySettings, centralMinFraction)
            ARC_REFLECT_ATTR(DisplayName, "Central minimum fraction") ARC_REFLECT_ATTR(Range, 0.1, 0.9)
            ARC_REFLECT_ATTR(Tooltip, "Share of each axis the viewport area keeps when the default layout is built in a small window; "
                                      "the side panels shrink to fit.")
        ARC_REFLECT_FIELD(LayoutFactorySettings, browserRefPx)
            ARC_REFLECT_ATTR(DisplayName, "Asset browser share") ARC_REFLECT_ATTR(Range, 1.0, 16384.0)
            ARC_REFLECT_ATTR(Tooltip, "The Asset Browser's part of the bottom band's browser : Inspector 2 proportion (reference pixels).")
        ARC_REFLECT_FIELD(LayoutFactorySettings, assetsInspectorRefPx)
            ARC_REFLECT_ATTR(DisplayName, "Assets Inspector share") ARC_REFLECT_ATTR(Range, 1.0, 16384.0)
            ARC_REFLECT_ATTR(Tooltip, "Inspector 2's part of the bottom band's browser : Inspector 2 proportion (reference pixels).")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorPlaySettings);
    ARC_SETTINGS(LayoutFactorySettings);

    namespace
    {
        constexpr const char* kLaunchModeCVar = "editor.play.launchMode";
    }

    bool ReadPlayModeIniLine(const char* line, std::optional<PlayLaunchMode>& out)
    {
        int mode = -1;
        // Malformed or out-of-range: refused -- never trust a line a hand edit
        // (or a future enumerator's rollback) could have left in a bogus state.
        if (std::sscanf(line, "Mode=%d", &mode) != 1 || mode < 0 ||
            mode > static_cast<int>(PlayLaunchMode::SeparateServerProcess))
            return false;
        out = static_cast<PlayLaunchMode>(mode);
        return true;
    }

    bool ImportLegacyPlayMode(CVarRegistry& reg, PlayLaunchMode mode)
    {
        const auto e = reg.Explain(kLaunchModeCVar);
        if (!e) return false;
        for (const CVarHistoryRecord& r : e->history)
            if (r.by == SetBy::User) return false;   // the user already chose: keep it
        return reg.Set(reg.Find(kLaunchModeCVar), CVarValue::Enum(static_cast<std::int32_t>(mode)),
                       SetBy::User, "playmode-ini-import") == SetResult::Applied;
    }

    void SetPlayLaunchMode(PlayLaunchMode mode)
    {
        // Tagged as the User file's own loader, so the edit replaces the record
        // that file produced (as SetViewportPref does). RefusedWeaker still
        // holds the User record: it is archived all the same.
        CVarRegistry& reg = CVarRegistry::Get();
        const SetResult r = reg.Set(reg.Find(kLaunchModeCVar), CVarValue::Enum(static_cast<std::int32_t>(mode)),
                                    SetBy::User, RungSource(SetBy::User));
        if (r == SetResult::Applied || r == SetResult::RefusedWeaker)
            NoteSettingEdited(SetBy::User, std::string(kLaunchModeCVar));
    }
}
