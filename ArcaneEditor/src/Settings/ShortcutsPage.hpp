#pragma once

// The shortcuts page (settings arc S4, spec s7.2): Preferences > Keyboard.
// One row per editor action: Action / Context / Shortcut / Type / Default.
// Clicking a Shortcut cell listens for the next chord (every action is
// silenced meanwhile); writes land on the EditorUser rung (keys are Pref-M)
// and reach <EditorUserDir>/Config/editor.json through the settings archive.

#include "Input/EditorActions.hpp"

#include <Arcane/Config/CVarTypes.hpp>   // SetBy

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    // Told the rung and cvar name (editor.keys.<id>) of every record a page
    // write changed: EditorUser for a binding, User (or a session rung) for a
    // cleared override. DrawShortcutsPage passes SettingsHost's
    // NoteSettingEdited for the two archived rungs, so the debounced archive
    // writes (or, after a clear, removes) the key; null leaves the change in
    // memory only.
    using ShortcutWriteSink = std::function<void(Arcane::SetBy rung, const std::string& cvar)>;

    struct ShortcutsPageState
    {
        char search[128] = {};
        bool conflictsOnly = false;   // the "Conflicts only" filter (S4-GATE)
        std::string listeningId;   // the row waiting for a chord; empty = none
        std::string refusal;       // why the last captured chord (or clear) did not take effect
        int drawnFrame = -1;       // the ImGui frame the page last drew on
    };

    struct ShortcutRow
    {
        std::string id, action, context, chord, defaultChord;   // chord / defaultChord: DisplayKeyChord text
        KeyType type = KeyType::Labelled;
        bool conflict = false;
        bool isDefault = true;
        std::string tooltip;                                    // the conflict, or the cvar name
        // A rung above EditorUser (the page's own) holds the binding: the
        // per-project User file, --set or the console. The row is marked
        // "Overridden by" with Clear override; Default = not overridden.
        Arcane::SetBy overriddenBy = Arcane::SetBy::Default;
    };

    // Registration order; `query` (case-insensitive) matches the action name,
    // id, context and chord. `conflictsOnly` keeps the rows drawn red.
    [[nodiscard]] std::vector<ShortcutRow> BuildShortcutRows(const EditorActions& actions, std::string_view query,
                                                             bool conflictsOnly = false);

    enum class ListenOutcome : std::uint8_t { Waiting, Bound, Cancelled, Cleared, Refused };
    // Reads this frame's CapturedChord for state.listeningId. Esc cancels,
    // Backspace clears, a reserved chord is refused; listening ends on any
    // outcome but Waiting. The chord binds with the row's current Type. A
    // write that does not take effect (a stronger rung still wins, or the
    // registry refused it) is Refused, with state.refusal saying why.
    ListenOutcome FeedListen(EditorActions& actions, ShortcutsPageState& state, const ShortcutWriteSink& sink = {});

    // Called every frame after the Preferences window: a page that was not
    // drawn this frame (window closed, another page selected) stops
    // listening, so the editor's shortcuts never stay silenced.
    void EndListenIfPageHidden(EditorActions& actions, ShortcutsPageState& state, int frame);

    // EditorUser + Publish; `sink` hears the cvar written.
    bool SetActionChord(EditorActions& actions, std::string_view id, const KeyChord& chord, const ShortcutWriteSink& sink = {});
    bool FlipKeyType(EditorActions& actions, std::string_view id, const ShortcutWriteSink& sink = {});
    // Removes the value of the rung that overrides `id` (row.overriddenBy);
    // false when nothing above EditorUser holds it. `sink` hears that rung.
    bool ClearShortcutOverride(EditorActions& actions, std::string_view id, const ShortcutWriteSink& sink = {});
    // Drops every saved binding: the EditorUser records (the file stops
    // pinning defaults) and this project's User overrides. `sink` hears each
    // cvar cleared on each rung, so the archive erases its key. Session rungs
    // (--set, console) stay; their rows keep Clear override.
    void ResetAllShortcuts(EditorActions& actions, const ShortcutWriteSink& sink = {});

    void DrawShortcutsPage(void* user);   // RegisterSettingsPage callback; user = ShortcutsPageState*
}
