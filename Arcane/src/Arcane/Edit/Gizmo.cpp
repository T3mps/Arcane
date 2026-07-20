#include <Arcane/Edit/Gizmo.hpp>

#include <algorithm>
#include <cmath>

namespace Arcane
{
    namespace
    {
        constexpr float kEps      = 1e-6f;
        constexpr float kMinScale = 0.01f;

        glm::vec2 WorldToScreen(const GizmoView& v, glm::vec2 world)
        {
            const float s = v.zoom * v.pixelsPerMeter;
            const glm::vec2 c = v.viewportOriginPx + v.viewportSizePx * 0.5f;
            const glm::vec2 d = (world - v.cameraOffset) * s;
            return glm::vec2(c.x + d.x, c.y - d.y);   // screen-Y down
        }

        glm::vec2 ScreenToWorld(const GizmoView& v, glm::vec2 screen)
        {
            const float s = v.zoom * v.pixelsPerMeter;
            const glm::vec2 c = v.viewportOriginPx + v.viewportSizePx * 0.5f;
            const glm::vec2 d(screen.x - c.x, -(screen.y - c.y));
            return v.cameraOffset + (s > kEps ? d / s : glm::vec2(0.0f));
        }

        glm::vec2 AxisDirWorld(GizmoAxis axis)
        {
            return axis == GizmoAxis::Y ? glm::vec2(0.0f, 1.0f) : glm::vec2(1.0f, 0.0f);
        }

        glm::vec2 AxisDirLocal(float rot, GizmoAxis axis)
        {
            const float c = std::cos(rot), s = std::sin(rot);
            return axis == GizmoAxis::Y ? glm::vec2(-s, c) : glm::vec2(c, s);
        }

        // Translate uses space; scale is always local.
        glm::vec2 AxisDir(GizmoSpace space, float rot, GizmoAxis axis)
        {
            return space == GizmoSpace::Local ? AxisDirLocal(rot, axis) : AxisDirWorld(axis);
        }

        float SnapScalar(float v, float step)
        {
            return step > kEps ? std::round(v / step) * step : v;
        }
    }

    GizmoTransform ApplyDrag(GizmoMode mode, GizmoSpace space, GizmoAxis axis,
                             const GizmoTransform& start, const GizmoView& view,
                             glm::vec2 mouseStartScreen, glm::vec2 mouseCurScreen,
                             const GizmoSnap& snap)
    {
        GizmoTransform r = start;
        const glm::vec2 pStart = ScreenToWorld(view, mouseStartScreen);
        const glm::vec2 pCur   = ScreenToWorld(view, mouseCurScreen);
        const glm::vec2 pivot  = start.position;

        switch (mode)
        {
            case GizmoMode::Translate:
            {
                const glm::vec2 delta = pCur - pStart;
                if (axis == GizmoAxis::Center)
                {
                    r.position = start.position + delta;
                    if (snap.enabled)
                    {
                        r.position.x = SnapScalar(r.position.x, snap.translate);
                        r.position.y = SnapScalar(r.position.y, snap.translate);
                    }
                }
                else
                {
                    const glm::vec2 dir = AxisDir(space, start.rotation, axis);
                    r.position = start.position + glm::dot(delta, dir) * dir;
                    if (snap.enabled)   // snap only the moved axis component
                    {
                        if (axis == GizmoAxis::X) r.position.x = SnapScalar(r.position.x, snap.translate);
                        else                      r.position.y = SnapScalar(r.position.y, snap.translate);
                    }
                }
                break;
            }
            case GizmoMode::Rotate:
            {
                const glm::vec2 d0 = pStart - pivot;
                const glm::vec2 d1 = pCur - pivot;
                const float a0 = std::atan2(d0.y, d0.x);
                const float a1 = std::atan2(d1.y, d1.x);
                r.rotation = start.rotation + (a1 - a0);
                if (snap.enabled)
                {
                    const float step = snap.rotationDeg * 3.14159265358979323846f / 180.0f;
                    r.rotation = SnapScalar(r.rotation, step);
                }
                break;
            }
            case GizmoMode::Scale:
            {
                if (axis == GizmoAxis::Center)
                {
                    const float l0 = glm::length(pStart - pivot);
                    const float l1 = glm::length(pCur - pivot);
                    const float f = (l0 > kEps) ? (l1 / l0) : 1.0f;
                    r.scale = start.scale * f;
                }
                else
                {
                    const glm::vec2 dir = AxisDirLocal(start.rotation, axis);   // scale is local
                    const float d0 = glm::dot(pStart - pivot, dir);
                    const float d1 = glm::dot(pCur - pivot, dir);
                    const float f = (std::fabs(d0) > kEps) ? (d1 / d0) : 1.0f;
                    if (axis == GizmoAxis::X) r.scale.x = start.scale.x * f;
                    else                      r.scale.y = start.scale.y * f;
                }
                if (snap.enabled)
                {
                    r.scale.x = SnapScalar(r.scale.x, snap.scale);
                    r.scale.y = SnapScalar(r.scale.y, snap.scale);
                }
                r.scale.x = std::max(r.scale.x, kMinScale);
                r.scale.y = std::max(r.scale.y, kMinScale);
                break;
            }
        }
        return r;
    }
}
