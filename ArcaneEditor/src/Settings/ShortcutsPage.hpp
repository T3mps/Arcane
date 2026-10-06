#pragma once

// The shortcuts page (settings arc S4, spec s7.2): Preferences > Keyboard.
// One row per editor action: Action / Context / Shortcut / Type / Default.
// Clicking a Shortcut cell listens for the next chord (every action is
// silenced meanwhile); writes land on the EditorUser rung (keys are Pref-M).

#include "Input/EditorActions.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    struct ShortcutsPageState
    {
        char search[128] = {};
        std::string listeningId;   // the row waiting for a chord; empty = none
        std::string refusal;       // why the last captured chord was refused
        int drawnFrame = -1;       // the ImGui frame the page last drew on
    };

    struct ShortcutRow
    {
        std::string id, action, context, chord, defaultChord;   // chord / defaultChord: DisplayKeyChord text
        KeyType type = KeyType::Labelled;
        bool conflict = false;
        bool isDefault = true;
        std::string tooltip;                                    // the conflict, or the cvar name
    };

    // Registration order; `query` (case-insensitive) matches the action name,
    // id, context and chord.
    [[nodiscard]] std::vector<ShortcutRow> BuildShortcutRows(const EditorActions& actions, std::string_view query);

    enum class ListenOutcome : std::uint8_t { Waiting, Bound, Cancelled, Cleared, Refused };
    // Reads this frame's CapturedChord for state.listeningId. Esc cancels,
    // Backspace clears, a reserved chord is refused; listening ends on any
    // outcome but Waiting. The chord binds with the row's current Type.
    ListenOutcome FeedListen(EditorActions& actions, ShortcutsPageState& state);

    // Called every frame after the Preferences window: a page that was not
    // drawn this frame (window closed, another page selected) stops
    // listening, so the editor's shortcuts never stay silenced.
    void EndListenIfPageHidden(EditorActions& actions, ShortcutsPageState& state, int frame);

    bool SetActionChord(EditorActions& actions, std::string_view id, const KeyChord& chord);   // EditorUser + Publish
    bool FlipKeyType(EditorActions& actions, std::string_view id);
    void ResetAllShortcuts(EditorActions& actions);   // drops every EditorUser binding

    void DrawShortcutsPage(void* user);   // RegisterSettingsPage callback; user = ShortcutsPageState*
}
