#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Render/Batcher2D.hpp>

#include <glm/gtc/matrix_access.hpp>     // glm::row
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace Arcane
{
    namespace
    {
        constexpr float kEps      = 1e-6f;
        constexpr float kMinScale = 0.01f;
        constexpr float kPi       = 3.14159265358979323846f;
        constexpr float kTau      = 2.0f * kPi;

        // Handle geometry in PIXELS at gizmo size 1 (the world radius follows
        // from WorldUnitsPerPixel at the pivot). Unreal's proportions.
        constexpr float kAxisLenPx          = 80.0f;   // arrow / scale-box reach
        constexpr float kRingFrac           = 0.8f;    // axis ring radius as a fraction of the reach (64 px)
        constexpr float kScreenRingRadiusPx = 76.0f;   // the camera-facing ring, a pixel circle
        constexpr float kPlaneMinFrac       = 0.35f;   // plane square from 0.35R to 0.65R along both axes
        constexpr float kPlaneMaxFrac       = 0.65f;
        constexpr float kHitThreshPx        = 8.0f;    // axis segment pick radius
        constexpr float kCenterHalfPx       = 8.0f;    // centre box half-extent
        constexpr float kRingBandPx         = 8.0f;    // ring pick band
        constexpr float kMinQuadAreaPx2     = 4.0f;    // an edge-on plane square is not a target
        constexpr float kShaftThicknessPx   = 2.0f;
        constexpr float kRingThicknessPx    = 2.0f;
        constexpr int   kRingSegments       = 48;
        constexpr float kArrowHeadLenPx     = 14.0f;
        constexpr float kArrowHeadHalfPx    = 6.0f;
        constexpr float kScaleBoxHalfPx     = 5.0f;

        glm::vec3 AxisUnit(GizmoAxis a) noexcept
        {
            switch (a)
            {
            case GizmoAxis::X: case GizmoAxis::YZ: return { 1.0f, 0.0f, 0.0f };   // a plane is named by its NORMAL here
            case GizmoAxis::Y: case GizmoAxis::XZ: return { 0.0f, 1.0f, 0.0f };
            case GizmoAxis::Z: case GizmoAxis::XY: return { 0.0f, 0.0f, 1.0f };
            default: return { 0.0f, 0.0f, 1.0f };
            }
        }

        // The axis (or plane normal) in WORLD space for the chosen frame.
        glm::vec3 AxisDir(GizmoSpace space, const glm::quat& rot, GizmoAxis a) noexcept
        {
            const glm::vec3 u = AxisUnit(a);
            return space == GizmoSpace::Local ? glm::normalize(rot * u) : u;
        }

        // The two axes a plane handle spans.
        std::pair<GizmoAxis, GizmoAxis> PlaneAxes(GizmoAxis plane) noexcept
        {
            switch (plane)
            {
            case GizmoAxis::XY: return { GizmoAxis::X, GizmoAxis::Y };
            case GizmoAxis::YZ: return { GizmoAxis::Y, GizmoAxis::Z };
            default:            return { GizmoAxis::X, GizmoAxis::Z };
            }
        }

        // The camera's forward in world space: the view matrix's rows are the
        // camera basis (right, up, BACK), so forward is minus the third row.
        glm::vec3 ViewForward(const ViewTransform& v) noexcept
        {
            return -glm::normalize(glm::vec3(glm::row(v.view, 2)));
        }

        // An orthonormal (u, v) in the plane normal to n with u x v == n, so a
        // positive angle from u toward v IS a right-hand turn about n.
        std::pair<glm::vec3, glm::vec3> PlaneBasis(glm::vec3 n) noexcept
        {
            const glm::vec3 a = std::abs(n.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 u = glm::normalize(glm::cross(n, a));
            return { u, glm::cross(n, u) };
        }

        bool Finite(glm::vec2 p) noexcept { return std::isfinite(p.x) && std::isfinite(p.y); }

        glm::vec2 Px(const ViewTransform& v, glm::vec3 world) noexcept
        {
            return glm::vec2(v.WorldToScreen(world));
        }

        float SnapScalar(float v, float step) noexcept
        {
            return step > kEps ? std::round(v / step) * step : v;
        }

        float DistToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b) noexcept
        {
            const glm::vec2 ab = b - a;
            const float len2 = glm::dot(ab, ab);
            const float u = len2 > kEps ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            return glm::length(p - (a + u * ab));
        }

        // Point-in-convex-quad in pixels (corners in order). A degenerate
        // (edge-on) quad is never inside.
        bool InsideQuad(glm::vec2 p, const std::array<glm::vec2, 4>& q) noexcept
        {
            float area = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                if (!Finite(q[i])) return false;
                const glm::vec2 a = q[i], b = q[(i + 1) % 4];
                area += a.x * b.y - b.x * a.y;
            }
            if (std::abs(area) * 0.5f < kMinQuadAreaPx2) return false;
            const float sign = area > 0.0f ? 1.0f : -1.0f;
            for (int i = 0; i < 4; ++i)
            {
                const glm::vec2 a = q[i], b = q[(i + 1) % 4];
                const glm::vec2 e = b - a, d = p - a;
                if ((e.x * d.y - e.y * d.x) * sign < 0.0f) return false;
            }
            return true;
        }

        // World radius of the handle set at the pivot.
        float Reach(const ViewTransform& v, glm::vec3 pivot, float sizeScale) noexcept
        {
            return kAxisLenPx * sizeScale * WorldUnitsPerPixel(v, pivot);
        }

        // The four world corners of a plane square (fractions of the reach).
        std::array<glm::vec3, 4> PlaneSquareWorld(glm::vec3 pivot, glm::vec3 a, glm::vec3 b, float R) noexcept
        {
            const float lo = kPlaneMinFrac * R, hi = kPlaneMaxFrac * R;
            return { pivot + a * lo + b * lo, pivot + a * hi + b * lo, pivot + a * hi + b * hi, pivot + a * lo + b * hi };
        }

        // The ring's world polyline (kRingSegments + 1 points, closed).
        std::array<glm::vec3, kRingSegments + 1> RingWorld(glm::vec3 pivot, glm::vec3 n, float radius) noexcept
        {
            const auto [u, w] = PlaneBasis(n);
            std::array<glm::vec3, kRingSegments + 1> pts{};
            for (int i = 0; i <= kRingSegments; ++i)
            {
                const float a = kTau * static_cast<float>(i) / static_cast<float>(kRingSegments);
                pts[static_cast<std::size_t>(i)] = pivot + (u * std::cos(a) + w * std::sin(a)) * radius;
            }
            return pts;
        }

        glm::vec4 Brighten(glm::vec4 c) noexcept
        {
            return { std::min(c.x * 1.4f, 1.0f), std::min(c.y * 1.4f, 1.0f), std::min(c.z * 1.4f, 1.0f), c.w };
        }

        // X red, Y green, Z blue; a plane takes the colour of its NORMAL axis
        // (Unreal / Blender); Center yellow; Screen light grey.
        glm::vec4 HandleColor(GizmoAxis a, GizmoAxis hovered, GizmoAxis active) noexcept
        {
            glm::vec4 base(0.85f, 0.2f, 0.2f, 1.0f);
            switch (a)
            {
            case GizmoAxis::Y: case GizmoAxis::XZ: base = { 0.2f, 0.85f, 0.2f, 1.0f }; break;
            case GizmoAxis::Z: case GizmoAxis::XY: base = { 0.25f, 0.4f, 0.95f, 1.0f }; break;
            case GizmoAxis::Center:                base = { 0.9f, 0.85f, 0.2f, 1.0f }; break;
            case GizmoAxis::Screen:                base = { 0.85f, 0.85f, 0.85f, 1.0f }; break;
            default: break;
            }
            return (a == hovered || a == active) ? Brighten(base) : base;
        }

        void Polyline(Batcher2D& b, const ViewTransform& v, std::span<const glm::vec3> pts, float thickness, glm::vec4 color)
        {
            for (std::size_t i = 0; i + 1 < pts.size(); ++i)
            {
                const glm::vec2 p0 = Px(v, pts[i]), p1 = Px(v, pts[i + 1]);
                if (Finite(p0) && Finite(p1)) b.Line(p0, p1, thickness, color);
            }
        }
    }

    // ---- GizmoHandleMask -------------------------------------------------------
    GizmoHandleMask GizmoHandleMask::All() noexcept { return {}; }

    GizmoHandleMask GizmoHandleMask::Planar(GizmoMode mode) noexcept
    {
        GizmoHandleMask m; m.bits = 0;
        switch (mode)
        {
        case GizmoMode::Translate: m.Set(GizmoAxis::X, true); m.Set(GizmoAxis::Y, true); m.Set(GizmoAxis::XY, true); m.Set(GizmoAxis::Center, true); break;
        case GizmoMode::Rotate:    m.Set(GizmoAxis::Z, true); break;
        case GizmoMode::Scale:     m.Set(GizmoAxis::X, true); m.Set(GizmoAxis::Y, true); m.Set(GizmoAxis::Center, true); break;
        }
        return m;
    }

    bool GizmoHandleMask::Has(GizmoAxis a) const noexcept { return ((bits >> static_cast<unsigned>(a)) & 1u) != 0u; }
    void GizmoHandleMask::Set(GizmoAxis a, bool on) noexcept
    {
        const std::uint16_t bit = static_cast<std::uint16_t>(1u << static_cast<unsigned>(a));
        bits = on ? static_cast<std::uint16_t>(bits | bit) : static_cast<std::uint16_t>(bits & ~bit);
    }

    // ---- geometry ----------------------------------------------------------------
    float WorldUnitsPerPixel(const ViewTransform& view, glm::vec3 worldPoint) noexcept
    {
        const glm::vec4 clip = view.projection * (view.view * glm::vec4(worldPoint, 1.0f));
        const float w  = std::max(std::abs(clip.w), 1e-4f);
        const float py = view.projection[1][1];
        if (view.viewport.y == 0u || std::abs(py) < kEps) return 1.0f;
        return (2.0f * w) / (py * static_cast<float>(view.viewport.y));
    }

    float ClosestLineParam(glm::vec3 lineOrigin, glm::vec3 lineDir, const Ray& ray) noexcept
    {
        // Two-lines closest points (Ericson 5.1.8) with both directions unit.
        const glm::vec3 w0 = lineOrigin - ray.origin;
        const float b  = glm::dot(lineDir, ray.direction);
        const float d0 = glm::dot(lineDir, w0);
        const float e0 = glm::dot(ray.direction, w0);
        const float denom = 1.0f - b * b;
        if (denom < 1e-5f)
            return -d0;   // parallel: the projection of the ray origin onto the line
        return (b * e0 - d0) / denom;
    }

    std::optional<glm::vec3> RayPlane(const Ray& ray, glm::vec3 planePoint, glm::vec3 planeNormal) noexcept
    {
        const float denom = glm::dot(planeNormal, ray.direction);
        if (std::abs(denom) < 1e-5f) return std::nullopt;                 // grazing
        const float s = glm::dot(planeNormal, planePoint - ray.origin) / denom;
        if (s < 0.0f) return std::nullopt;                                 // behind the ray
        return ray.origin + ray.direction * s;
    }

    // ---- HitTest ---------------------------------------------------------------
    GizmoAxis HitTest(GizmoMode mode, GizmoSpace space, const GizmoTransform& t, const ViewTransform& view,
                      GizmoHandleMask handles, float sizeScale, glm::vec2 mouse)
    {
        const glm::vec2 pivotPx = Px(view, t.position);
        if (!Finite(pivotPx)) return GizmoAxis::None;
        const float R = Reach(view, t.position, sizeScale);
        const GizmoSpace axisSpace = (mode == GizmoMode::Scale) ? GizmoSpace::Local : space;

        if (mode == GizmoMode::Rotate)
        {
            // Rings first, the most camera-facing first: an edge-on ring is a
            // line through the pivot that would otherwise steal every hit.
            const glm::vec3 fwd = ViewForward(view);
            std::array<GizmoAxis, 3> order{ GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z };
            std::sort(order.begin(), order.end(), [&](GizmoAxis a, GizmoAxis b)
            {
                return std::abs(glm::dot(AxisDir(space, t.rotation, a), fwd)) > std::abs(glm::dot(AxisDir(space, t.rotation, b), fwd));
            });
            for (GizmoAxis a : order)
            {
                if (!handles.Has(a)) continue;
                const auto ring = RingWorld(t.position, AxisDir(space, t.rotation, a), R * kRingFrac);
                for (int i = 0; i < kRingSegments; ++i)
                {
                    const glm::vec2 p0 = Px(view, ring[static_cast<std::size_t>(i)]), p1 = Px(view, ring[static_cast<std::size_t>(i + 1)]);
                    if (Finite(p0) && Finite(p1) && DistToSegment(mouse, p0, p1) <= kRingBandPx) return a;
                }
            }
            if (handles.Has(GizmoAxis::Screen) &&
                std::abs(glm::length(mouse - pivotPx) - kScreenRingRadiusPx * sizeScale) <= kRingBandPx)
                return GizmoAxis::Screen;
            return GizmoAxis::None;
        }

        // Centre wins on overlap.
        if (handles.Has(GizmoAxis::Center) &&
            std::abs(mouse.x - pivotPx.x) <= kCenterHalfPx && std::abs(mouse.y - pivotPx.y) <= kCenterHalfPx)
            return GizmoAxis::Center;

        if (mode == GizmoMode::Translate)
        {
            for (GizmoAxis plane : { GizmoAxis::XY, GizmoAxis::YZ, GizmoAxis::XZ })
            {
                if (!handles.Has(plane)) continue;
                const auto [a, b] = PlaneAxes(plane);
                const auto sq = PlaneSquareWorld(t.position, AxisDir(axisSpace, t.rotation, a), AxisDir(axisSpace, t.rotation, b), R);
                const std::array<glm::vec2, 4> q{ Px(view, sq[0]), Px(view, sq[1]), Px(view, sq[2]), Px(view, sq[3]) };
                if (InsideQuad(mouse, q)) return plane;
            }
        }

        for (GizmoAxis a : { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z })
        {
            if (!handles.Has(a)) continue;
            const glm::vec2 tip = Px(view, t.position + AxisDir(axisSpace, t.rotation, a) * R);
            if (Finite(tip) && DistToSegment(mouse, pivotPx, tip) <= kHitThreshPx) return a;
        }
        return GizmoAxis::None;
    }

    // ---- Draw --------------------------------------------------------------------
    void Draw(Batcher2D& batcher, GizmoMode mode, GizmoSpace space, const GizmoTransform& t, const ViewTransform& view,
              GizmoHandleMask handles, float sizeScale, GizmoAxis hovered, GizmoAxis active)
    {
        batcher.SetLayer(0xFFFF, 0xFFFF);   // on top of the scene (max layer/order); overlay pixels, no depth
        const glm::vec2 pivotPx = Px(view, t.position);
        if (!Finite(pivotPx)) return;
        const float R = Reach(view, t.position, sizeScale);
        const GizmoSpace axisSpace = (mode == GizmoMode::Scale) ? GizmoSpace::Local : space;

        if (mode == GizmoMode::Rotate)
        {
            for (GizmoAxis a : { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z })
            {
                if (!handles.Has(a)) continue;
                const auto ring = RingWorld(t.position, AxisDir(space, t.rotation, a), R * kRingFrac);
                Polyline(batcher, view, ring, kRingThicknessPx, HandleColor(a, hovered, active));
            }
            if (handles.Has(GizmoAxis::Screen))
            {
                const glm::vec4 c = HandleColor(GizmoAxis::Screen, hovered, active);
                const float r = kScreenRingRadiusPx * sizeScale;
                for (int i = 0; i < kRingSegments; ++i)
                {
                    const float a0 = kTau * static_cast<float>(i) / kRingSegments, a1 = kTau * static_cast<float>(i + 1) / kRingSegments;
                    batcher.Line(pivotPx + glm::vec2(std::cos(a0), std::sin(a0)) * r, pivotPx + glm::vec2(std::cos(a1), std::sin(a1)) * r, kRingThicknessPx, c);
                }
            }
            return;
        }

        if (mode == GizmoMode::Translate)
        {
            for (GizmoAxis plane : { GizmoAxis::XY, GizmoAxis::YZ, GizmoAxis::XZ })
            {
                if (!handles.Has(plane)) continue;
                const auto [a, b] = PlaneAxes(plane);
                const auto sq = PlaneSquareWorld(t.position, AxisDir(axisSpace, t.rotation, a), AxisDir(axisSpace, t.rotation, b), R);
                const std::array<glm::vec2, 4> q{ Px(view, sq[0]), Px(view, sq[1]), Px(view, sq[2]), Px(view, sq[3]) };
                if (!Finite(q[0]) || !Finite(q[1]) || !Finite(q[2]) || !Finite(q[3])) continue;
                glm::vec4 c = HandleColor(plane, hovered, active);
                c.w = (plane == hovered || plane == active) ? 0.6f : 0.35f;
                batcher.Triangle(q[0], q[1], q[2], c);
                batcher.Triangle(q[0], q[2], q[3], c);
            }
        }

        for (GizmoAxis a : { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z })
        {
            if (!handles.Has(a)) continue;
            const glm::vec2 tip = Px(view, t.position + AxisDir(axisSpace, t.rotation, a) * R);
            if (!Finite(tip)) continue;
            const glm::vec4 c = HandleColor(a, hovered, active);
            batcher.Line(pivotPx, tip, kShaftThicknessPx, c);
            const glm::vec2 d = tip - pivotPx;
            const float len = glm::length(d);
            if (len < 2.0f) continue;   // pointing at the camera: a dot, no head
            const glm::vec2 dir = d / len, perp(-dir.y, dir.x);
            if (mode == GizmoMode::Translate)
            {
                const glm::vec2 base = tip - dir * kArrowHeadLenPx;
                batcher.Triangle(tip, base + perp * kArrowHeadHalfPx, base - perp * kArrowHeadHalfPx, c);
            }
            else
            {
                const glm::vec2 half(kScaleBoxHalfPx, kScaleBoxHalfPx);
                batcher.Rect(tip - half, half * 2.0f, c);
            }
        }

        if (handles.Has(GizmoAxis::Center))
        {
            const glm::vec2 half(kCenterHalfPx, kCenterHalfPx);
            batcher.Rect(pivotPx - half, half * 2.0f, HandleColor(GizmoAxis::Center, hovered, active));
        }
    }

    // ---- ApplyDrag ---------------------------------------------------------------
    GizmoTransform ApplyDrag(GizmoMode mode, GizmoSpace space, GizmoAxis axis, const GizmoTransform& start,
                             const ViewTransform& view, glm::vec2 mouseStart, glm::vec2 mouseCur, const GizmoSnap& snap)
    {
        GizmoTransform r = start;
        const Ray ray0 = view.ScreenToRay(mouseStart);
        const Ray ray1 = view.ScreenToRay(mouseCur);
        const GizmoSpace axisSpace = (mode == GizmoMode::Scale) ? GizmoSpace::Local : space;

        switch (mode)
        {
        case GizmoMode::Translate:
        {
            if (axis == GizmoAxis::X || axis == GizmoAxis::Y || axis == GizmoAxis::Z)
            {
                // Closest point between the mouse ray and the axis LINE, before and after.
                const glm::vec3 dir = AxisDir(axisSpace, start.rotation, axis);
                const float d = ClosestLineParam(start.position, dir, ray1) - ClosestLineParam(start.position, dir, ray0);
                if (snap.enabled && axisSpace == GizmoSpace::Local)
                    r.position = start.position + SnapScalar(d, snap.translate) * dir;
                else
                {
                    r.position = start.position + d * dir;
                    if (snap.enabled)   // world axis: snap the moved COMPONENT so it lands on the grid
                    {
                        const int i = axis == GizmoAxis::X ? 0 : axis == GizmoAxis::Y ? 1 : 2;
                        r.position[i] = SnapScalar(r.position[i], snap.translate);
                    }
                }
            }
            else if (axis == GizmoAxis::XY || axis == GizmoAxis::YZ || axis == GizmoAxis::XZ || axis == GizmoAxis::Center)
            {
                // Ray-plane, before and after. Center is the CAMERA plane (in the
                // 2D view that is the XY plane, so it is the old free move).
                const glm::vec3 n = axis == GizmoAxis::Center ? ViewForward(view) : AxisDir(axisSpace, start.rotation, axis);
                const auto p0 = RayPlane(ray0, start.position, n);
                const auto p1 = RayPlane(ray1, start.position, n);
                if (!p0 || !p1) break;   // grazing / behind the eye: hold the start
                glm::vec3 delta = *p1 - *p0;
                delta -= n * glm::dot(delta, n);   // EXACTLY in the plane: a 2D drag leaves z untouched to the bit
                if (snap.enabled)
                {
                    if (axis == GizmoAxis::Center || axisSpace == GizmoSpace::World)
                    {
                        glm::vec3 p = start.position + delta;
                        for (int i = 0; i < 3; ++i)
                            if (axis == GizmoAxis::Center || std::abs(n[i]) < 0.5f)   // the components the plane spans
                                p[i] = SnapScalar(p[i], snap.translate);
                        r.position = p;
                    }
                    else
                    {
                        const auto [a, b] = PlaneAxes(axis);
                        const glm::vec3 da = AxisDir(axisSpace, start.rotation, a), db = AxisDir(axisSpace, start.rotation, b);
                        r.position = start.position + SnapScalar(glm::dot(delta, da), snap.translate) * da
                                                    + SnapScalar(glm::dot(delta, db), snap.translate) * db;
                    }
                }
                else
                    r.position = start.position + delta;
            }
            break;
        }
        case GizmoMode::Rotate:
        {
            const glm::vec3 n = axis == GizmoAxis::Screen ? ViewForward(view) : AxisDir(space, start.rotation, axis);
            const auto p0 = RayPlane(ray0, start.position, n);
            const auto p1 = RayPlane(ray1, start.position, n);
            if (!p0 || !p1) break;
            const auto [u, v] = PlaneBasis(n);
            const glm::vec3 d0 = *p0 - start.position, d1 = *p1 - start.position;
            const float a0 = std::atan2(glm::dot(d0, v), glm::dot(d0, u));
            const float a1 = std::atan2(glm::dot(d1, v), glm::dot(d1, u));
            float delta = a1 - a0;   // world-sense: u x v == n, so + is a right-hand turn about n
            if (snap.enabled) delta = SnapScalar(delta, snap.rotationDeg * kPi / 180.0f);
            // n is already the WORLD direction of the chosen axis -- AxisDir(Local,
            // rot, a) returns the local axis in world coordinates -- so the turn
            // PRE-multiplies in every case: there is no separate local-space form.
            r.rotation = glm::normalize(glm::angleAxis(delta, n) * start.rotation);
            break;
        }
        case GizmoMode::Scale:
        {
            const glm::vec2 pivotPx = Px(view, start.position);
            if (!Finite(pivotPx)) break;
            if (axis == GizmoAxis::Center)
            {
                const float l0 = glm::length(mouseStart - pivotPx), l1 = glm::length(mouseCur - pivotPx);
                const float f = l0 > kEps ? l1 / l0 : 1.0f;
                r.scale = start.scale * f;
            }
            else if (axis == GizmoAxis::X || axis == GizmoAxis::Y || axis == GizmoAxis::Z)
            {
                // Screen delta along the PROJECTED local axis, as a ratio of the
                // grab distance -- projection-independent, and the box the user
                // grabbed stays under the cursor along that axis.
                const glm::vec3 dir = AxisDir(GizmoSpace::Local, start.rotation, axis);
                const glm::vec2 tip = Px(view, start.position + dir * (10.0f * WorldUnitsPerPixel(view, start.position)));
                if (!Finite(tip)) break;
                const glm::vec2 d = tip - pivotPx;
                const float len = glm::length(d);
                if (len < kEps) break;   // the axis points at the camera: no screen direction to measure along
                const glm::vec2 dpx = d / len;
                const float s0 = glm::dot(mouseStart - pivotPx, dpx), s1 = glm::dot(mouseCur - pivotPx, dpx);
                const float f = std::abs(s0) > kEps ? s1 / s0 : 1.0f;
                const int i = axis == GizmoAxis::X ? 0 : axis == GizmoAxis::Y ? 1 : 2;
                r.scale[i] = start.scale[i] * f;
            }
            for (int i = 0; i < 3; ++i)
            {
                if (snap.enabled) r.scale[i] = SnapScalar(r.scale[i], snap.scale);
                // Clamp the MAGNITUDE and keep the sign: a mirrored entity (a
                // negative authored scale) stays mirrored through a scale drag.
                const float sign = r.scale[i] < 0.0f ? -1.0f : 1.0f;
                r.scale[i] = sign * std::max(std::abs(r.scale[i]), kMinScale);
            }
            break;
        }
        }
        return r;
    }

    // ---- group delta ---------------------------------------------------------------
    GizmoGroupDelta MakeGroupDelta(const GizmoTransform& start, const GizmoTransform& end)
    {
        GizmoGroupDelta d;
        d.translate = end.position - start.position;
        d.rotate    = glm::normalize(end.rotation * glm::inverse(start.rotation));   // the WORLD turn that takes start to end
        d.pivot     = start.position;
        for (int i = 0; i < 3; ++i)
            d.scale[i] = std::abs(start.scale[i]) > 1e-6f ? end.scale[i] / start.scale[i] : 1.0f;
        return d;
    }

    GizmoTransform ApplyGroupDelta(const GizmoTransform& t, const GizmoGroupDelta& d)
    {
        // Scale then turn the member's offset from the pivot, then shift: T*R*S about the pivot.
        const glm::vec3 rel = d.rotate * ((t.position - d.pivot) * d.scale);
        GizmoTransform r;
        r.position = d.pivot + rel + d.translate;
        r.rotation = glm::normalize(d.rotate * t.rotation);
        r.scale    = t.scale * d.scale;
        return r;
    }

    // ---- decompose / compose ------------------------------------------------------
    GizmoTransform DecomposeTRS(const glm::mat4& m)
    {
        GizmoTransform t;
        t.position = glm::vec3(m[3]);
        glm::mat3 basis{ glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2]) };   // braces: the paren form is a function declaration
        for (int i = 0; i < 3; ++i)
        {
            const float len = glm::length(basis[i]);
            t.scale[i] = len;
            basis[i] = len > kEps ? basis[i] / len : glm::vec3(i == 0, i == 1, i == 2);   // a dead axis keeps its identity direction
        }
        if (glm::determinant(basis) < 0.0f)
        {
            // A mirror. Negate X (UE's convention); the caller re-homes it if the
            // author put the mirror elsewhere (WithMirrorOn).
            t.scale.x = -t.scale.x;
            basis[0]  = -basis[0];
        }
        t.rotation = glm::normalize(glm::quat_cast(basis));
        return t;
    }

    glm::mat4 ComposeTRS(const GizmoTransform& t)
    {
        // translate * rotate * scale -- Transform::ToMatrix's order (pinned in GizmoTest.cpp).
        return glm::translate(glm::mat4(1.0f), t.position) * glm::mat4_cast(t.rotation) * glm::scale(glm::mat4(1.0f), t.scale);
    }

    GizmoTransform WithMirrorOn(const GizmoTransform& t, int axis)
    {
        if (t.scale.x >= 0.0f || axis < 1 || axis > 2) return t;
        // S' = S * D, R' = R * D, D = diag with -1 on X and on `axis`: a half turn
        // about the remaining axis, so R * D is still a rotation and the matrix is unchanged.
        GizmoTransform r = t;
        r.scale.x     = -t.scale.x;
        r.scale[axis] = -t.scale[axis];
        const glm::vec3 third = axis == 1 ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        r.rotation = glm::normalize(t.rotation * glm::angleAxis(kPi, third));
        return r;
    }
}
