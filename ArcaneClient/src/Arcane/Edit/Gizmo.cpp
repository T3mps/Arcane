#include <Arcane/Edit/Gizmo.hpp>

#include <glm/gtc/matrix_access.hpp>     // glm::row
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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

        // Handle geometry in PIXELS at gizmo size 1, UNREAL'S PROPORTIONS
        // (Editor/UnrealEd/Private/UnrealWidgetRender.cpp, UnrealWidget.h):
        // the widget's UniformScale makes one widget unit two pixels, so every
        // number below is the UE constant times two. World lengths follow
        // from WorldUnitsPerPixel at the pivot (screen-constant size).
        constexpr float kAxisLenPx          = 70.0f;   // AXIS_LENGTH 35: the translate cylinder
        constexpr float kAxisTipPx          = 94.0f;   // cone apex: root at AXIS_LENGTH + ConeHeadOffset 12
        constexpr float kHeadLenPx          = 26.0f;   // DrawCone scaled -13
        constexpr float kHeadHalfPx         = 7.3f;    // 13 * tan(5 deg * pi): the cone's base radius
        constexpr float kShaftPx            = 5.0f;    // CylinderRadius 1.2 -> diameter 2.4 units
        constexpr float kScaleShaftFromPx   = 10.0f;   // scale mode: AXIS_LENGTH_SCALE_OFFSET 5 in ...
        constexpr float kScaleShaftToPx     = 60.0f;   // ... to AXIS_LENGTH - 5
        constexpr float kScaleCubeCentrePx  = 66.0f;   // Render_Cube at AxisLength + CubeHeadOffset 3 + offset 5
        constexpr float kScaleCubeHalfPx    = 4.0f;    // cube 4 units
        constexpr float kPlaneCornerPx      = 14.0f;   // CornerPos 7
        constexpr float kPlaneBarPx         = 24.0f;   // AxisSize 12 along each spanning axis
        constexpr float kPlaneBarWidthPx    = 3.0f;    // bar thickness 1.2 (rounded up so it survives AA)
        constexpr float kPlaneEdgeOnCos     = 0.2f;    // a corner within ~78 deg of edge-on is hidden (unusable as a target)
        constexpr float kCentrePx           = 8.0f;    // DrawSphere radius 4
        constexpr float kRingInnerPx        = 96.0f;   // INNER_AXIS_CIRCLE_RADIUS 48
        constexpr float kRingOuterPx        = 112.0f;  // OUTER_AXIS_CIRCLE_RADIUS 56
        constexpr float kScreenRingPx       = 140.0f;  // OUTER_AXIS_CIRCLE_RADIUS * 1.25
        constexpr float kScreenRingWidthPx  = 3.0f;    // 1.25 units
        constexpr float kHitThreshPx        = 8.0f;    // axis segment pick radius
        constexpr float kRingHitSlackPx     = 4.0f;    // the band half-width plus this is the ring pick radius
        constexpr float kMinQuadAreaPx2     = 4.0f;    // an edge-on plane corner is not a target
        constexpr int   kRingSegments       = 48;      // full ring
        constexpr int   kArcSegments        = 16;      // a quarter band

        // UE's axis colours (AxisDisplayInfo::GetAxisColor, LINEAR) converted
        // to display space, because the sink paints display-referred pixels;
        // the hot handle is FColor::Yellow (CurrentColor); the screen-space
        // ring is (196,196,196); the screen-axis rotate colour is
        // (0.76, 0.72, 0.14) linear.
        constexpr glm::vec4 kColorX      { 0.79f, 0.15f, 0.00f, 1.0f };   // (0.594, 0.0197, 0)
        constexpr glm::vec4 kColorY      { 0.40f, 0.66f, 0.00f, 1.0f };   // (0.1349, 0.3959, 0)
        constexpr glm::vec4 kColorZ      { 0.17f, 0.49f, 0.93f, 1.0f };   // (0.0251, 0.207, 0.85)
        constexpr glm::vec4 kColorHot    { 1.00f, 1.00f, 0.00f, 1.0f };
        constexpr glm::vec4 kColorScreen { 0.77f, 0.77f, 0.77f, 1.0f };
        constexpr glm::vec4 kColorScreenArc { 0.89f, 0.86f, 0.41f, 1.0f };
        constexpr glm::vec4 kColorCentre { 0.92f, 0.92f, 0.92f, 1.0f };

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

        // Is the point in FRONT of the eye? WorldToScreen NaNs only at |w| ~ 0;
        // a point BEHIND a perspective eye (w < 0) projects to a finite pixel
        // mirrored through the viewport centre, and a gizmo drawn there is a
        // phantom the user can grab. Orthographic w is 1: always visible.
        bool Visible(const ViewTransform& v, glm::vec3 world) noexcept
        {
            const glm::vec4 clip = v.projection * (v.view * glm::vec4(world, 1.0f));
            return std::isfinite(clip.w) && clip.w > 0.0f && Finite(glm::vec2(clip));
        }

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

        // The direction from the pivot TOWARD the eye: the eye position in
        // perspective, the (parallel) view direction reversed in orthographic.
        glm::vec3 ToEye(const ViewTransform& v, glm::vec3 pivot) noexcept
        {
            if (v.IsOrthographic()) return -ViewForward(v);
            const glm::vec3 eye = glm::vec3(glm::inverse(v.view)[3]);
            const glm::vec3 d = eye - pivot;
            const float len = glm::length(d);
            return len > kEps ? d / len : -ViewForward(v);
        }

        // World length of `px` screen pixels at the pivot for this gizmo size.
        float Metres(const ViewTransform& v, glm::vec3 pivot, float sizeScale, float px) noexcept
        {
            return px * sizeScale * WorldUnitsPerPixel(v, pivot);
        }

        // The plane corner's four world corners -- UE's CornerPos square, in
        // the QUADRANT THAT FACES THE CAMERA: each spanning axis is flipped
        // toward the eye, so orbiting never puts the corner behind the pivot or
        // under the arrows (Blender's rule; UE keeps +,+ and hides per ortho
        // view). False when the plane is close to edge-on -- a sliver is not a
        // target. sq[0] is the corner nearest the pivot, sq[1] along a, sq[3]
        // along b.
        bool PlaneCornerFacing(const ViewTransform& v, glm::vec3 pivot, float sizeScale, glm::vec3 a, glm::vec3 b,
                               std::array<glm::vec3, 4>& sq) noexcept
        {
            const glm::vec3 toEye = ToEye(v, pivot);
            const glm::vec3 n = glm::cross(a, b);
            const float nLen = glm::length(n);
            if (nLen < kEps || std::abs(glm::dot(n / nLen, toEye)) < kPlaneEdgeOnCos) return false;
            if (glm::dot(a, toEye) < 0.0f) a = -a;
            if (glm::dot(b, toEye) < 0.0f) b = -b;
            const float lo = Metres(v, pivot, sizeScale, kPlaneCornerPx);
            const float hi = Metres(v, pivot, sizeScale, kPlaneCornerPx + kPlaneBarPx);
            sq = { pivot + a * lo + b * lo, pivot + a * hi + b * lo, pivot + a * hi + b * hi, pivot + a * lo + b * hi };
            return true;
        }

        // UE's arc axes per ring (Render_Rotate): X arc spans (Z, Y), Y arc
        // (X, Z), Z arc (X, Y). The pair is what the quarter is drawn between.
        std::pair<GizmoAxis, GizmoAxis> ArcAxes(GizmoAxis ring) noexcept
        {
            switch (ring)
            {
            case GizmoAxis::X: return { GizmoAxis::Z, GizmoAxis::Y };
            case GizmoAxis::Y: return { GizmoAxis::X, GizmoAxis::Z };
            default:           return { GizmoAxis::X, GizmoAxis::Y };
            }
        }

        // A ring's world basis and angular span. FULL ring (0..tau in the
        // PlaneBasis frame) when the view looks straight down the ring's axis
        // in an orthographic view (UE's bIsOrthoDrawingFullRing) or the ring is
        // being dragged; otherwise UE's camera-facing QUARTER: each arc axis
        // is mirrored toward the eye (DrawRotationArc's bMirrorAxis0/1) and the
        // band runs from the first to the second.
        struct RingSpan { glm::vec3 u, w; float a0, a1; };
        RingSpan RingSpanFor(const ViewTransform& v, GizmoSpace space, const GizmoTransform& t, GizmoAxis ring, bool full) noexcept
        {
            const glm::vec3 n = AxisDir(space, t.rotation, ring);
            const glm::vec3 toEye = ToEye(v, t.position);
            const bool orthoDownAxis = v.IsOrthographic() && std::abs(glm::dot(n, toEye)) > 0.999f;
            if (full || orthoDownAxis)
            {
                const auto [u, w] = PlaneBasis(n);
                return { u, w, 0.0f, kTau };
            }
            const auto [ax0, ax1] = ArcAxes(ring);
            glm::vec3 r0 = AxisDir(space, t.rotation, ax0);
            glm::vec3 r1 = AxisDir(space, t.rotation, ax1);
            if (glm::dot(r0, toEye) < 0.0f) r0 = -r0;
            if (glm::dot(r1, toEye) < 0.0f) r1 = -r1;
            // (r0, r1) may be a left-handed pair after the mirroring; the band
            // is the quarter FROM r0 TO r1 either way.
            return { r0, r1, 0.0f, kPi * 0.5f };
        }

        glm::vec3 OnRing(const RingSpan& span, glm::vec3 pivot, float radius, float angle) noexcept
        {
            return pivot + (span.u * std::cos(angle) + span.w * std::sin(angle)) * radius;
        }

        glm::vec4 Brighten(glm::vec4 c) noexcept
        {
            return { std::min(c.x * 1.4f, 1.0f), std::min(c.y * 1.4f, 1.0f), std::min(c.z * 1.4f, 1.0f), c.w };
        }

        glm::vec4 Darken(glm::vec4 c) noexcept
        {
            return { c.x * 0.55f, c.y * 0.55f, c.z * 0.55f, c.w };
        }

        glm::vec4 AxisColor(GizmoAxis a) noexcept
        {
            switch (a)
            {
            case GizmoAxis::X: case GizmoAxis::YZ: return kColorX;
            case GizmoAxis::Y: case GizmoAxis::XZ: return kColorY;
            case GizmoAxis::Z: case GizmoAxis::XY: return kColorZ;
            case GizmoAxis::Center:                return kColorCentre;
            case GizmoAxis::Screen:                return kColorScreen;
            default:                               return kColorCentre;
            }
        }

        // UE: the hovered / dragged handle turns YELLOW (CurrentColor), every
        // other handle keeps its axis colour.
        glm::vec4 HandleColor(GizmoAxis a, GizmoAxis hovered, GizmoAxis active) noexcept
        {
            return (a == hovered || a == active) ? kColorHot : AxisColor(a);
        }

        // A shaded ROD between two pixels: the body in the handle colour with a
        // highlight strip on one side and a shadow strip on the other -- the
        // flat-shaded cylinder UE's ArrowMaterial gives, without a mesh pass.
        void Rod(GizmoDrawSink& b, glm::vec2 p0, glm::vec2 p1, float width, glm::vec4 c)
        {
            const glm::vec2 d = p1 - p0;
            const float len = glm::length(d);
            const glm::vec2 perp = len > kEps ? glm::vec2(-d.y, d.x) / len : glm::vec2(0.0f, 1.0f);
            b.Line(p0, p1, width, c);
            b.Line(p0 - perp * (width * 0.25f), p1 - perp * (width * 0.25f), width * 0.3f, Brighten(c));
            b.Line(p0 + perp * (width * 0.35f), p1 + perp * (width * 0.35f), width * 0.25f, Darken(c));
        }

        // A CONE head as two triangles split along the axis, one lit and one
        // in shadow.
        void Cone(GizmoDrawSink& b, glm::vec2 tip, glm::vec2 dir, float len, float halfWidth, glm::vec4 c)
        {
            const glm::vec2 perp(-dir.y, dir.x);
            const glm::vec2 base = tip - dir * len;
            b.Triangle(tip, base + perp * halfWidth, base, Brighten(c));
            b.Triangle(tip, base, base - perp * halfWidth, Darken(c));
        }

        // A thick ARC band between two radii in the ring's plane, projected
        // per segment (UE's DrawThickArc). Skips segments with a corner behind
        // the eye.
        void ArcBand(GizmoDrawSink& b, const ViewTransform& v, const RingSpan& span, glm::vec3 pivot,
                     float rIn, float rOut, int segments, glm::vec4 c)
        {
            for (int i = 0; i < segments; ++i)
            {
                const float a0 = span.a0 + (span.a1 - span.a0) * static_cast<float>(i) / static_cast<float>(segments);
                const float a1 = span.a0 + (span.a1 - span.a0) * static_cast<float>(i + 1) / static_cast<float>(segments);
                const glm::vec3 w0 = OnRing(span, pivot, rIn, a0), w1 = OnRing(span, pivot, rOut, a0);
                const glm::vec3 w2 = OnRing(span, pivot, rOut, a1), w3 = OnRing(span, pivot, rIn, a1);
                if (!Visible(v, w0) || !Visible(v, w1) || !Visible(v, w2) || !Visible(v, w3)) continue;
                const glm::vec2 q0 = Px(v, w0), q1 = Px(v, w1), q2 = Px(v, w2), q3 = Px(v, w3);
                if (!Finite(q0) || !Finite(q1) || !Finite(q2) || !Finite(q3)) continue;
                b.Triangle(q0, q1, q2, c);
                b.Triangle(q0, q2, q3, c);
            }
        }

        // The swept SECTOR of a rotate drag (UE's inner "pie" with the grid
        // material): a translucent fan from the pivot to the inner radius over
        // [start, start + delta].
        void Sector(GizmoDrawSink& b, const ViewTransform& v, const RingSpan& span, glm::vec3 pivot,
                    float radius, float start, float delta, glm::vec4 c)
        {
            const int segments = std::max(2, static_cast<int>(std::ceil(std::abs(delta) / (kTau / kRingSegments))));
            const glm::vec2 centre = Px(v, pivot);
            if (!Finite(centre)) return;
            for (int i = 0; i < segments; ++i)
            {
                const float a0 = start + delta * static_cast<float>(i) / static_cast<float>(segments);
                const float a1 = start + delta * static_cast<float>(i + 1) / static_cast<float>(segments);
                const glm::vec3 w0 = OnRing(span, pivot, radius, a0), w1 = OnRing(span, pivot, radius, a1);
                if (!Visible(v, w0) || !Visible(v, w1)) continue;
                const glm::vec2 q0 = Px(v, w0), q1 = Px(v, w1);
                if (Finite(q0) && Finite(q1)) b.Triangle(centre, q0, q1, c);
            }
        }

        // Distance from `p` to the projected centreline of an arc band.
        float DistToArc(const ViewTransform& v, const RingSpan& span, glm::vec3 pivot, float radius, int segments, glm::vec2 p) noexcept
        {
            float best = std::numeric_limits<float>::infinity();
            for (int i = 0; i < segments; ++i)
            {
                const float a0 = span.a0 + (span.a1 - span.a0) * static_cast<float>(i) / static_cast<float>(segments);
                const float a1 = span.a0 + (span.a1 - span.a0) * static_cast<float>(i + 1) / static_cast<float>(segments);
                const glm::vec3 w0 = OnRing(span, pivot, radius, a0), w1 = OnRing(span, pivot, radius, a1);
                if (!Visible(v, w0) || !Visible(v, w1)) continue;
                const glm::vec2 q0 = Px(v, w0), q1 = Px(v, w1);
                if (Finite(q0) && Finite(q1)) best = std::min(best, DistToSegment(p, q0, q1));
            }
            return best;
        }

        // The two angles a rotate drag reads in the ring's PlaneBasis frame --
        // the SAME frame Draw's sector uses, so the pie matches the turn.
        std::optional<std::pair<float, float>> RotateAngles(const ViewTransform& view, glm::vec3 n, glm::vec3 pivot,
                                                            glm::vec2 mouseStart, glm::vec2 mouseCur) noexcept
        {
            const auto p0 = RayPlane(view.ScreenToRay(mouseStart), pivot, n);
            const auto p1 = RayPlane(view.ScreenToRay(mouseCur), pivot, n);
            if (!p0 || !p1) return std::nullopt;
            const auto [u, w] = PlaneBasis(n);
            const glm::vec3 d0 = *p0 - pivot, d1 = *p1 - pivot;
            return std::make_pair(std::atan2(glm::dot(d0, w), glm::dot(d0, u)),
                                  std::atan2(glm::dot(d1, w), glm::dot(d1, u)));
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
        if (!Visible(view, t.position)) return GizmoAxis::None;   // behind the eye: no phantom to grab
        const glm::vec2 pivotPx = Px(view, t.position);
        if (!Finite(pivotPx)) return GizmoAxis::None;
        const GizmoSpace axisSpace = (mode == GizmoMode::Scale) ? GizmoSpace::Local : space;
        const auto M = [&](float px) { return Metres(view, t.position, sizeScale, px); };

        if (mode == GizmoMode::Rotate)
        {
            // The band's centreline, the most camera-facing ring first (an
            // edge-on band is a line through the pivot that would otherwise
            // steal every hit).
            const glm::vec3 fwd = ViewForward(view);
            std::array<GizmoAxis, 3> order{ GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z };
            std::sort(order.begin(), order.end(), [&](GizmoAxis a, GizmoAxis b)
            {
                return std::abs(glm::dot(AxisDir(space, t.rotation, a), fwd)) > std::abs(glm::dot(AxisDir(space, t.rotation, b), fwd));
            });
            const float band = (kRingOuterPx - kRingInnerPx) * 0.5f * sizeScale + kRingHitSlackPx;
            for (GizmoAxis a : order)
            {
                if (!handles.Has(a)) continue;
                const RingSpan span = RingSpanFor(view, space, t, a, /*full=*/false);
                const int segs = span.a1 > kPi ? kRingSegments : kArcSegments;
                if (DistToArc(view, span, t.position, M((kRingInnerPx + kRingOuterPx) * 0.5f), segs, mouse) <= band) return a;
            }
            if (handles.Has(GizmoAxis::Screen) &&
                std::abs(glm::length(mouse - pivotPx) - kScreenRingPx * sizeScale) <= kHitThreshPx)
                return GizmoAxis::Screen;
            return GizmoAxis::None;
        }

        // Centre wins on overlap.
        if (handles.Has(GizmoAxis::Center) && glm::length(mouse - pivotPx) <= kCentrePx * sizeScale)
            return GizmoAxis::Center;

        if (mode == GizmoMode::Translate)
        {
            for (GizmoAxis plane : { GizmoAxis::XY, GizmoAxis::YZ, GizmoAxis::XZ })
            {
                if (!handles.Has(plane)) continue;
                const auto [a, b] = PlaneAxes(plane);
                std::array<glm::vec3, 4> sq{};
                if (!PlaneCornerFacing(view, t.position, sizeScale, AxisDir(axisSpace, t.rotation, a), AxisDir(axisSpace, t.rotation, b), sq)) continue;
                // A corner behind the eye projects to a mirrored pixel: skip the
                // whole square rather than test a quad with a folded-back corner.
                if (!Visible(view, sq[0]) || !Visible(view, sq[1]) || !Visible(view, sq[2]) || !Visible(view, sq[3])) continue;
                const std::array<glm::vec2, 4> q{ Px(view, sq[0]), Px(view, sq[1]), Px(view, sq[2]), Px(view, sq[3]) };
                if (InsideQuad(mouse, q)) return plane;
            }
        }

        // The arrow (shaft + cone) or the scale rod + cube, as one segment.
        const float from = mode == GizmoMode::Scale ? kScaleShaftFromPx : 0.0f;
        const float to   = mode == GizmoMode::Scale ? kScaleCubeCentrePx + kScaleCubeHalfPx : kAxisTipPx;
        for (GizmoAxis a : { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z })
        {
            if (!handles.Has(a)) continue;
            const glm::vec3 dir = AxisDir(axisSpace, t.rotation, a);
            const glm::vec3 w0 = t.position + dir * M(from), w1 = t.position + dir * M(to);
            if (!Visible(view, w1) || !Visible(view, w0)) continue;   // behind the eye: no phantom axis to grab
            const glm::vec2 p0 = Px(view, w0), p1 = Px(view, w1);
            if (Finite(p0) && Finite(p1) && DistToSegment(mouse, p0, p1) <= kHitThreshPx) return a;
        }
        return GizmoAxis::None;
    }

    std::optional<GizmoRotateSweep> RotateSweep(GizmoSpace space, GizmoAxis axis, const GizmoTransform& start,
                                                const ViewTransform& view, glm::vec2 mouseStart, glm::vec2 mouseCur,
                                                const GizmoSnap& snap)
    {
        if (axis != GizmoAxis::X && axis != GizmoAxis::Y && axis != GizmoAxis::Z && axis != GizmoAxis::Screen)
            return std::nullopt;
        const glm::vec3 n = axis == GizmoAxis::Screen ? ViewForward(view) : AxisDir(space, start.rotation, axis);
        const auto angles = RotateAngles(view, n, start.position, mouseStart, mouseCur);
        if (!angles) return std::nullopt;
        float delta = std::remainder(angles->second - angles->first, kTau);
        if (snap.enabled) delta = SnapScalar(delta, snap.rotationDeg * kPi / 180.0f);
        return GizmoRotateSweep{ angles->first, delta };
    }

    // ---- Draw --------------------------------------------------------------------
    void Draw(GizmoDrawSink& sink, GizmoMode mode, GizmoSpace space, const GizmoTransform& t, const ViewTransform& view,
              GizmoHandleMask handles, float sizeScale, GizmoAxis hovered, GizmoAxis active,
              const GizmoRotateSweep* sweep)
    {
        // Into the host's FOREGROUND sink (GizmoDrawSink): over the finished
        // frame, no depth -- Unreal's SDPG_Foreground for its widget.
        if (!Visible(view, t.position)) return;   // behind the eye: no phantom to draw
        const glm::vec2 pivotPx = Px(view, t.position);
        if (!Finite(pivotPx)) return;
        const GizmoSpace axisSpace = (mode == GizmoMode::Scale) ? GizmoSpace::Local : space;
        const auto M = [&](float px) { return Metres(view, t.position, sizeScale, px); };

        if (mode == GizmoMode::Rotate)
        {
            // While a ring is being DRAGGED only that ring is drawn, as a full
            // band, with the swept sector inside it (UE's Render_Rotate under
            // bDragging). Otherwise the three camera-facing quarter bands.
            const bool dragging = sweep != nullptr && active != GizmoAxis::None && active != GizmoAxis::Screen;
            for (GizmoAxis a : { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z })
            {
                if (!handles.Has(a)) continue;
                if (dragging && a != active) continue;
                const RingSpan span = RingSpanFor(view, space, t, a, /*full=*/dragging && a == active);
                const int segs = span.a1 > kPi ? kRingSegments : kArcSegments;
                ArcBand(sink, view, span, t.position, M(kRingInnerPx), M(kRingOuterPx), segs, HandleColor(a, hovered, active));
                if (dragging && a == active)
                {
                    glm::vec4 fill = kColorHot; fill.w = 0.3f;
                    Sector(sink, view, span, t.position, M(kRingInnerPx), sweep->start, sweep->delta, fill);
                }
            }
            if (handles.Has(GizmoAxis::Screen) && !dragging)
            {
                const glm::vec4 c = (hovered == GizmoAxis::Screen || active == GizmoAxis::Screen) ? kColorHot : kColorScreenArc;
                const float r = kScreenRingPx * sizeScale;
                for (int i = 0; i < kRingSegments; ++i)
                {
                    const float a0 = kTau * static_cast<float>(i) / kRingSegments, a1 = kTau * static_cast<float>(i + 1) / kRingSegments;
                    sink.Line(pivotPx + glm::vec2(std::cos(a0), std::sin(a0)) * r, pivotPx + glm::vec2(std::cos(a1), std::sin(a1)) * r, kScreenRingWidthPx, c);
                }
            }
            if (sweep != nullptr && active == GizmoAxis::Screen)
            {
                // A screen-ring drag: the full ring in yellow with its sector.
                const glm::vec3 n = ViewForward(view);
                const auto [u, w] = PlaneBasis(n);
                const RingSpan span{ u, w, 0.0f, kTau };
                glm::vec4 fill = kColorHot; fill.w = 0.3f;
                Sector(sink, view, span, t.position, M(kScreenRingPx), sweep->start, sweep->delta, fill);
            }
            return;
        }

        if (mode == GizmoMode::Translate)
        {
            for (GizmoAxis plane : { GizmoAxis::XY, GizmoAxis::YZ, GizmoAxis::XZ })
            {
                if (!handles.Has(plane)) continue;
                const auto [a, b] = PlaneAxes(plane);
                std::array<glm::vec3, 4> sq{};
                if (!PlaneCornerFacing(view, t.position, sizeScale, AxisDir(axisSpace, t.rotation, a), AxisDir(axisSpace, t.rotation, b), sq)) continue;
                if (!Visible(view, sq[0]) || !Visible(view, sq[1]) || !Visible(view, sq[2]) || !Visible(view, sq[3])) continue;
                const std::array<glm::vec2, 4> q{ Px(view, sq[0]), Px(view, sq[1]), Px(view, sq[2]), Px(view, sq[3]) };
                if (!Finite(q[0]) || !Finite(q[1]) || !Finite(q[2]) || !Finite(q[3])) continue;
                // Unreal's DrawDualAxis: an L of two bars meeting at the corner
                // nearest the pivot (sq[0]), one along each spanning axis in
                // THAT axis's colour; both yellow when the plane is hot. The
                // square between the bars is the hit region, painted only
                // while hot so the grab area is visible exactly when it matters.
                const bool hot = (plane == hovered || plane == active);
                if (hot)
                {
                    glm::vec4 fill = kColorHot; fill.w = 0.3f;
                    sink.Triangle(q[0], q[1], q[2], fill);
                    sink.Triangle(q[0], q[2], q[3], fill);
                }
                sink.Line(q[0], q[1], kPlaneBarWidthPx, hot ? kColorHot : AxisColor(a));   // along a
                sink.Line(q[0], q[3], kPlaneBarWidthPx, hot ? kColorHot : AxisColor(b));   // along b
            }
        }

        for (GizmoAxis a : { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z })
        {
            if (!handles.Has(a)) continue;
            const glm::vec3 dir = AxisDir(axisSpace, t.rotation, a);
            const glm::vec4 c = HandleColor(a, hovered, active);
            if (mode == GizmoMode::Translate)
            {
                // Cylinder 0..35 units, cone 34..47 (Render_Axis).
                const glm::vec3 wShaft = t.position + dir * M(kAxisLenPx), wTip = t.position + dir * M(kAxisTipPx);
                if (!Visible(view, wShaft) || !Visible(view, wTip)) continue;   // behind the eye: no phantom axis
                const glm::vec2 shaft = Px(view, wShaft), tip = Px(view, wTip);
                if (!Finite(shaft) || !Finite(tip)) continue;
                Rod(sink, pivotPx, shaft, kShaftPx, c);
                const glm::vec2 d = tip - pivotPx;
                const float len = glm::length(d);
                if (len < 2.0f) continue;   // pointing at the camera: a dot, no head
                Cone(sink, tip, d / len, kHeadLenPx, kHeadHalfPx, c);
            }
            else
            {
                // Scale: the shorter rod (5..30 units) with a 4-unit cube at 33.
                const glm::vec3 w0 = t.position + dir * M(kScaleShaftFromPx), w1 = t.position + dir * M(kScaleShaftToPx);
                const glm::vec3 wc = t.position + dir * M(kScaleCubeCentrePx);
                if (!Visible(view, w0) || !Visible(view, w1) || !Visible(view, wc)) continue;
                const glm::vec2 p0 = Px(view, w0), p1 = Px(view, w1), pc = Px(view, wc);
                if (!Finite(p0) || !Finite(p1) || !Finite(pc)) continue;
                Rod(sink, p0, p1, kShaftPx, c);
                const glm::vec2 half(kScaleCubeHalfPx * sizeScale, kScaleCubeHalfPx * sizeScale);
                sink.Rect(pc - half, half * 2.0f, c);
                sink.Rect(pc - half, half, Brighten(c));   // the lit top-left face
            }
        }

        if (handles.Has(GizmoAxis::Center))
            sink.Circle(pivotPx, kCentrePx * sizeScale, HandleColor(GizmoAxis::Center, hovered, active));
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
                            if (std::abs(n[i]) < 0.5f)   // the components the plane spans -- for Center too, so the
                                                         // camera-plane normal (Z in the 2D view) stays untouched
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
            // The same angles RotateSweep hands Draw for the sector, so the pie
            // matches the turn to the bit.
            const glm::vec3 n = axis == GizmoAxis::Screen ? ViewForward(view) : AxisDir(space, start.rotation, axis);
            const auto angles = RotateAngles(view, n, start.position, mouseStart, mouseCur);
            if (!angles) break;
            float delta = std::remainder(angles->second - angles->first, kTau);   // world-sense: u x w == n, so + is a right-hand turn about n
            if (snap.enabled) delta = SnapScalar(delta, snap.rotationDeg * kPi / 180.0f);
            // n is already the WORLD direction of the chosen axis (local or not),
            // so the turn pre-multiplies in every case.
            r.rotation = glm::normalize(glm::angleAxis(delta, n) * start.rotation);
            break;
        }
        case GizmoMode::Scale:
        {
            if (!Visible(view, start.position)) break;   // behind the eye: no pixel geometry to measure against
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
                // Clamp the MAGNITUDE and keep the START's sign: a mirrored entity
                // (a negative authored scale) stays mirrored through a scale drag,
                // even when the snap lands on -0 (the snapped value has no sign to read).
                const float sign = start.scale[i] < 0.0f ? -1.0f : 1.0f;
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
