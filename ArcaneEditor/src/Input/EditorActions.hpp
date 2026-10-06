#pragma once

// Editor actions (settings arc S4, spec s7.2): every editor keyboard command is
// a named action whose chord is the String cvar editor.keys.<id> (widget
// "keychord", PreferencesMachine). "Ctrl+Z" is LABELLED (the key that prints Z
// on the user's layout, the default); "[KeyW]" is PHYSICAL (the key in W's
// QWERTY position: the fly camera, the QWER tool row). Display always uses the
// current layout's labels.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Arcane { class KeyLayout; }

namespace Arcane::Editor
{
    enum class KeyType : std::uint8_t { Labelled, Physical };

    struct KeyChord
    {
        bool ctrl = false, shift = false, alt = false, super = false;
        KeyType type = KeyType::Labelled;
        std::int32_t key = 0;   // Labelled: an SDL keycode. Physical: an SDL scancode. 0 = unbound.
        bool operator==(const KeyChord&) const = default;
        [[nodiscard]] bool Bound() const noexcept { return key != 0; }
    };

    [[nodiscard]] std::optional<KeyChord> ParseKeyChord(std::string_view text);   // "" -> an unbound chord; nullopt = not a chord
    [[nodiscard]] std::string FormatKeyChord(const KeyChord& chord);              // the canonical cvar text
    [[nodiscard]] std::string DisplayKeyChord(const KeyChord& chord);             // the active layout's labels

    void SetActiveKeyLayout(const Arcane::KeyLayout* layout);   // null = QWERTY (headless tests)
    [[nodiscard]] const Arcane::KeyLayout& ActiveKeyLayout();

    [[nodiscard]] std::optional<std::string> ReservedChordReason(const KeyChord& chord);   // why the OS/ImGui owns it
    [[nodiscard]] KeyChord ToKeyType(const KeyChord& chord, KeyType type);                  // unchanged when no key maps
    [[nodiscard]] bool SameKey(const KeyChord& a, const KeyChord& b);                       // same modifiers, same key on this layout

    // ---- the action registry (S4-7) ----
}
