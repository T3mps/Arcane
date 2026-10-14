#include "Settings/EditorConsoleSettings.hpp"

#include "Settings/SettingsEdit.hpp"   // RungSource
#include "Settings/SettingsHost.hpp"   // NoteSettingEdited

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

#include <string>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(EditorConsoleSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.console", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(EditorConsoleSettings, ringLines)
            ARC_REFLECT_ATTR(DisplayName, "Log ring lines") ARC_REFLECT_ATTR(Category, "Log")
            ARC_REFLECT_ATTR(Range, 16.0, 1000000.0)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Keywords, "buffer capacity history scrollback log size")
            ARC_REFLECT_ATTR(Tooltip, "Log lines the Console's ring holds when the editor starts. While the editor "
                                      "runs, the ring follows Displayed lines.")
        ARC_REFLECT_FIELD(EditorConsoleSettings, displayLineCap)
            ARC_REFLECT_ATTR(DisplayName, "Displayed lines") ARC_REFLECT_ATTR(Category, "Log")
            ARC_REFLECT_ATTR(Range, 16.0, 1000000.0)
            ARC_REFLECT_ATTR(Keywords, "cap limit scrollback rows log size")
            ARC_REFLECT_ATTR(Tooltip, "Log lines the Console keeps while the editor runs; the oldest drop past it.")
        ARC_REFLECT_FIELD(EditorConsoleSettings, collapse)
            ARC_REFLECT_ATTR(DisplayName, "Collapse identical rows") ARC_REFLECT_ATTR(Category, "Toolbar")
            ARC_REFLECT_ATTR(Keywords, "duplicate fold group repeat")
            ARC_REFLECT_ATTR(Tooltip, "Fold identical consecutive log rows into one with a repeat count. The "
                                      "Console toolbar's Collapse toggle.")
        ARC_REFLECT_FIELD(EditorConsoleSettings, autoScroll)
            ARC_REFLECT_ATTR(DisplayName, "Auto-scroll") ARC_REFLECT_ATTR(Category, "Toolbar")
            ARC_REFLECT_ATTR(Keywords, "follow tail bottom scroll")
            ARC_REFLECT_ATTR(Tooltip, "Follow new log rows while the list is scrolled to the bottom. The Console "
                                      "toolbar's Scroll toggle.")
        ARC_REFLECT_FIELD(EditorConsoleSettings, wrap)
            ARC_REFLECT_ATTR(DisplayName, "Wrap long rows") ARC_REFLECT_ATTR(Category, "Toolbar")
            ARC_REFLECT_ATTR(Keywords, "word wrap line break")
            ARC_REFLECT_ATTR(Tooltip, "Wrap long log rows to the panel width. The Console toolbar's Wrap toggle.")
        ARC_REFLECT_FIELD(EditorConsoleSettings, replyLines)
            ARC_REFLECT_ATTR(DisplayName, "Command replies shown") ARC_REFLECT_ATTR(Category, "Command line")
            ARC_REFLECT_ATTR(Range, 0.0, 64.0)
            ARC_REFLECT_ATTR(Keywords, "cvar output result lines input")
            ARC_REFLECT_ATTR(Tooltip, "Lines of command-line output shown between the log rows and the input line.")
        ARC_REFLECT_FIELD(EditorConsoleSettings, categoryWidth)
            ARC_REFLECT_ATTR(DisplayName, "Category column width") ARC_REFLECT_ATTR(Category, "Log")
            ARC_REFLECT_ATTR(Range, 0.0, 64.0)
            ARC_REFLECT_ATTR(Keywords, "column align pad characters tag")
            ARC_REFLECT_ATTR(Tooltip, "Minimum width, in characters, of the category column in log rows and in "
                                      "copied text.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorConsoleSettings);

    ARC_REFLECT_TYPE(RecentsSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.recents", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(RecentsSettings, maxProjectsShown)
            ARC_REFLECT_ATTR(DisplayName, "Recent projects shown")
            ARC_REFLECT_ATTR(Range, 1.0, 50.0)
            ARC_REFLECT_ATTR(Keywords, "open recent menu history mru project")
            ARC_REFLECT_ATTR(Tooltip, "Entries File > Open Recent Project lists.")
        ARC_REFLECT_FIELD(RecentsSettings, maxScenes)
            ARC_REFLECT_ATTR(DisplayName, "Recent scenes kept")
            ARC_REFLECT_ATTR(Range, 1.0, 50.0)
            ARC_REFLECT_ATTR(Keywords, "open recent menu history mru scene level")
            ARC_REFLECT_ATTR(Tooltip, "Scenes the Open Recent Scene menu remembers, newest first. Applies the next "
                                      "time a scene is opened or saved.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(RecentsSettings);

    void SetConsoleToggle(std::string_view cvar, bool value)
    {
        // As SetViewportPref: tagged as the User file's own loader, so the click
        // replaces the record that file produced rather than stacking a second
        // User record, and queued for the archive so the choice outlives the
        // session (FlushSettingsArchives writes dirty-marked names only).
        // RefusedWeaker still holds the User record (a stronger rung wins for
        // now): it is archived all the same.
        CVarRegistry& reg = CVarRegistry::Get();
        const SetResult r = reg.Set(reg.Find(cvar), CVarValue::Bool(value), SetBy::User, RungSource(SetBy::User));
        if (r == SetResult::Applied || r == SetResult::RefusedWeaker)
            NoteSettingEdited(SetBy::User, std::string(cvar));
    }
}
