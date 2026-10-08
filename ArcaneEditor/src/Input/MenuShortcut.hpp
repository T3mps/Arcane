#pragma once

// What a menu item prints beside its label (settings arc S4, spec s7.2): the
// action's CURRENT chord, so the menus and the shortcuts page cannot drift.
// Holds the string for the MenuItem call: ImGui::MenuItem("Undo", MenuKey("edit.undo").c_str()).

#include "Input/EditorActions.hpp"

#include <string>
#include <string_view>

namespace Arcane::Editor
{
    class MenuKey
    {
    public:
        explicit MenuKey(std::string_view actionId) : m_text(EditorActions::Get().MenuShortcut(actionId)) {}
        [[nodiscard]] const char* c_str() const noexcept { return m_text.c_str(); }
    private:
        std::string m_text;
    };

    // A tooltip or label that names an action's chord reads it from the action
    // (S4-GATE), never a literal that goes stale on a rebind: "Undo (Ctrl+Z)",
    // or just "Undo" while the action is unbound.
    [[nodiscard]] inline std::string WithChord(std::string_view label, const EditorActions& actions, std::string_view actionId)
    {
        std::string out(label);
        const std::string chord = actions.MenuShortcut(actionId);
        if (!chord.empty()) out += " (" + chord + ")";
        return out;
    }
    [[nodiscard]] inline std::string WithChord(std::string_view label, std::string_view actionId)
    {
        return WithChord(label, EditorActions::Get(), actionId);
    }
}
