#pragma once

// GizmoOverlay: the editor's FOREGROUND sink for Arcane::Draw -- ImGui draw-
// list primitives painted over the viewport image, after the scene render
// (F4 plan 2, desk finding 2026-09-18). The gizmo used to go into the scene
// batch, which the mesh pass overpaints (F5's compositing contract), so any
// mesh whose silhouette covered the handles hid them. Unreal draws its widget
// in SDPG_Foreground; ImGuizmo draws into ImGui. This is the same posture:
// viewport-local pixels + the image origin -> the Viewport window's draw list,
// clipped to the image, drawn before the tool overlay so the buttons stay on
// top of the gizmo lines.

#include <Arcane/Edit/Gizmo.hpp>

#include <imgui.h>

#include <glm/glm.hpp>

namespace Arcane::Editor
{
    class ImGuiGizmoSink final : public GizmoDrawSink
    {
    public:
        ImGuiGizmoSink(ImDrawList& list, ImVec2 origin) : m_list(list), m_origin(origin) {}

        void Line(glm::vec2 a, glm::vec2 b, float thickness, glm::vec4 rgba) override
        {
            // Dark halo under every stroke so axis colours still read on a
            // matching mesh (red on the cube, blue on the sky). Draw's own
            // primitive counts stay the same -- this is paint, not geometry.
            m_list.AddLine(At(a), At(b), kHalo, thickness + 2.2f);
            m_list.AddLine(At(a), At(b), Col(rgba), thickness);
        }
        void Triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec4 rgba) override
        {
            const ImVec2 p0 = At(a), p1 = At(b), p2 = At(c);
            m_list.AddTriangleFilled(p0, p1, p2, Col(rgba));
            const ImVec2 ring[3] = { p0, p1, p2 };
            m_list.AddPolyline(ring, 3, kHalo, ImDrawFlags_Closed, 1.6f);
        }
        void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 rgba) override
        {
            const ImVec2 p0 = At(pos), p1 = At(pos + size);
            m_list.AddRectFilled(ImVec2(p0.x - 1.2f, p0.y - 1.2f),
                                 ImVec2(p1.x + 1.2f, p1.y + 1.2f), kHalo);
            m_list.AddRectFilled(p0, p1, Col(rgba));
        }
        void Circle(glm::vec2 center, float radius, glm::vec4 rgba) override
        {
            const ImVec2 c = At(center);
            m_list.AddCircleFilled(c, radius + 1.6f, kHalo);
            m_list.AddCircleFilled(c, radius, Col(rgba));
        }

    private:
        static constexpr ImU32 kHalo = IM_COL32(8, 10, 14, 200);

        [[nodiscard]] ImVec2 At(glm::vec2 p) const noexcept { return ImVec2(m_origin.x + p.x, m_origin.y + p.y); }
        [[nodiscard]] static ImU32 Col(glm::vec4 c) noexcept { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w)); }

        ImDrawList& m_list;
        ImVec2      m_origin;
    };
}
