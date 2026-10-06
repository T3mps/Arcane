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
}
