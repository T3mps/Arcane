#include "Settings/AxisColors.hpp"

#include "Viewport/ViewportGrid.hpp"
#include "Widgets/GraphCanvasStyle.hpp"

#include <Arcane/Render/Nri/nodes/GridNode.hpp>

#include <algorithm>

namespace Arcane::Editor
{
    glm::vec4 DeriveRole(const ImVec4& token, const ImVec4& dark, const glm::vec4& legacy) noexcept
    {
        if (token.x == dark.x && token.y == dark.y && token.z == dark.z)
            return legacy;   // the Dark token: today's value to the bit
        const auto channel = [](float t, float d, float l)
        { return std::clamp(d > 0.0f ? t * (l / d) : t, 0.0f, 1.0f); };
        return { channel(token.x, dark.x, legacy.x), channel(token.y, dark.y, legacy.y),
                 channel(token.z, dark.z, legacy.z), legacy.w };
    }

    AxisRoleColors DeriveAxisRoles(const Theme::Palette& p)
    {
        const Arcane::GizmoAxisColors gizmo{};
        AxisRoleColors r{};
        // S5-2 option A: grid and gizmo stay the legacy triples. Inspector
        // bars follow editor.theme.axis*; outline follows amber + graph hover.
        r.gizmo.x = gizmo.x;
        r.gizmo.y = gizmo.y;
        r.gizmo.z = gizmo.z;
        r.grid2D[0] = kGridAxisXColor;
        r.grid2D[1] = kGridAxisYColor;
        r.grid3D[0] = Arcane::GridSceneDesc::kAxisXColor;
        r.grid3D[1] = Arcane::GridSceneDesc::kAxisYColor;
        r.grid3D[2] = Arcane::GridSceneDesc::kAxisZColor;
        r.inspectorBar[0] = ImGui::ColorConvertFloat4ToU32(p.axisX);
        r.inspectorBar[1] = ImGui::ColorConvertFloat4ToU32(p.axisY);
        r.inspectorBar[2] = ImGui::ColorConvertFloat4ToU32(p.axisZ);
        r.outlineSelect = glm::vec4(p.amber.x, p.amber.y, p.amber.z, 1.0f);
        r.outlineHover  = glm::vec4(kGraphNodeHovBorderColor.x, kGraphNodeHovBorderColor.y,
                                    kGraphNodeHovBorderColor.z, kGraphNodeHovBorderColor.w);
        return r;
    }
}
