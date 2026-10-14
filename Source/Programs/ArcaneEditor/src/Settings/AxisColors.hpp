#pragma once

// Axis and outline colour roles (settings arc S4, inventory R1): ONE token per
// axis (editor.theme.axisX/Y/Z) and the amber token feed every consumer. Each
// consumer keeps its role (the gizmo's punchier display cousin, the grids'
// 0.9-alpha linear lines): exact legacy at the Dark default, the token's hue
// scaled by the role's gain otherwise.
//
// S5-2 option A: only the inspector bars bind to the tokens today. Grid and
// gizmo roles stay their legacy values (pending a unification re-bless).
// DeriveRole is the gain rule that unification will use; DeriveAxisRoles
// does not apply it to grid/gizmo until then.

#include "Widgets/EditorTheme.hpp"

#include <Arcane/Edit/Gizmo.hpp>

#include <glm/glm.hpp>
#include <imgui.h>

namespace Arcane::Editor
{
    struct AxisRoleColors
    {
        Arcane::GizmoAxisColors gizmo;
        glm::vec4 grid2D[2];        // X, Y (ViewportGrid, the 2D view)
        glm::vec4 grid3D[3];        // X, Y, Z (GridNode, Perspective)
        ImU32     inspectorBar[3];  // X, Y, Z (DrawAxisBar)
        glm::vec4 outlineSelect;    // OutlineNode: selected
        glm::vec4 outlineHover;     // OutlineNode: hovered
    };

    [[nodiscard]] glm::vec4 DeriveRole(const ImVec4& token, const ImVec4& darkToken, const glm::vec4& legacy) noexcept;
    [[nodiscard]] AxisRoleColors DeriveAxisRoles(const Theme::Palette& palette);
}
