// Hud implementation (Task 8). See Hud.hpp for the section map + design notes.
//
// SCENE SWITCH / RESET ARE DEFERRED, NOT IMMEDIATE: Draw runs in the render phase
// (the host calls GamePlugin_DrawUI between ImGuiLayer BeginFrame/Render). A scene
// rebuild calls reg.Clear() + mints a fresh PhysicsResource, which must NOT happen
// mid-render. So the HUD requests a switch/reset through the SceneControl side channel
// (requestedScene / requestReset); SandboxApp::FixedUpdate consumes it next fixed step,
// on a clean registry -- the EXACT contract the SandboxSmokeTest already drives. The
// gravity slider rides the same path: SetGravityY then request a reset (the PhysicsWorld
// bakes gravity at construction, so a fresh world is the clean way to change it).

#include "Hud.hpp"
#include "SandboxApp.hpp"
#include "Scenes.hpp"

#include <Arcane/Physics/Narrowphase/NarrowphaseTrace.hpp>  // NarrowphaseTrace + snapshots (inspector)
#include <Arcane/Render/Batcher2D.hpp>      // Batcher2D (Minkowski inset draw lambda)
#include <Arcane/Render/OffscreenCanvas.hpp> // OffscreenCanvas (inset target)
#include <Arcane/Scene/PhysicsSystem.hpp>   // PhysicsResource (live body/contact counts)

#include <Astra/Registry/Registry.hpp>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>    // std::snprintf (partner-selector labels)
#include <span>
#include <vector>    // std::vector (the subject's contacts list)

namespace Arcane::Sandbox
{
    namespace
    {
        // Request a scene switch via the SceneControl side channel (consumed next
        // FixedUpdate on a clean registry). Idempotent; creates the resource if absent.
        void RequestScene(Astra::Registry& reg, int index)
        {
            SceneControl* ctrl = reg.GetResource<SceneControl>();
            if (!ctrl)
            {
                reg.SetResource(SceneControl{});
                ctrl = reg.GetResource<SceneControl>();
            }
            if (ctrl) ctrl->requestedScene = index;
        }

        // Request a rebuild of the current scene (same clean deferred path).
        void RequestReset(Astra::Registry& reg)
        {
            SceneControl* ctrl = reg.GetResource<SceneControl>();
            if (!ctrl)
            {
                reg.SetResource(SceneControl{});
                ctrl = reg.GetResource<SceneControl>();
            }
            if (ctrl) ctrl->requestReset = 1;
        }

        // ---- narrowphase-inspector helpers (Slice B, Task 5) --------------------

        const char* KindName(Arcane::Physics::NarrowphaseKind k)
        {
            using K = Arcane::Physics::NarrowphaseKind;
            switch (k)
            {
                case K::Separated:       return "Separated";
                case K::CircleCircle:    return "CircleCircle";
                case K::CircleVsPolygon: return "CircleVsPolygon";
                case K::Capsule:         return "Capsule";
                case K::SatPolygon:      return "SatPolygon (SAT)";
                case K::Epa:             return "Epa";
                case K::Mpr:             return "Mpr";
                default:                 return "?";
            }
        }

        // A Minkowski-space -> inset-pixel fit transform: maps an MD-space AABB
        // (padded) to the [0,w]x[0,h] inset, centered + uniformly scaled to fit.
        struct MinkowskiFit
        {
            glm::vec2 mn{0.0f, 0.0f};
            glm::vec2 scale{1.0f, 1.0f};   // uniform (x==y); pixels-per-MD-unit
            glm::vec2 origin{0.0f, 0.0f};  // pixel offset that centers the content

            glm::vec2 ToPx(glm::vec2 md) const noexcept
            {
                return glm::vec2((md.x - mn.x) * scale.x + origin.x,
                                 (md.y - mn.y) * scale.y + origin.y);
            }
        };

        // Accumulate one MD point into a running AABB.
        inline void Acc(glm::vec2& lo, glm::vec2& hi, glm::vec2 p)
        {
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y);
        }

        // Build the fit transform from the trace's Minkowski geometry for the current
        // step (epa polytope / gjk seed / mpr portal) + the ORIGIN, padded ~10%.
        MinkowskiFit BuildFit(const Arcane::Physics::NarrowphaseTrace& t, int step,
                              float w, float h)
        {
            glm::vec2 lo{0.0f, 0.0f}, hi{0.0f, 0.0f};   // always include the origin
            using K = Arcane::Physics::NarrowphaseKind;

            // GJK seed (single terminal simplex) -- always drawn alongside.
            for (const auto& s : t.gjkSnapshots)
                for (int i = 0; i < s.count; ++i)
                    Acc(lo, hi, glm::vec2(s.verts[i].x, s.verts[i].y));

            if (t.kind == K::Epa && !t.epaSnapshots.empty())
            {
                const int si = std::clamp(step, 0, (int)t.epaSnapshots.size() - 1);
                for (const auto& v : t.epaSnapshots[si].verts)
                    Acc(lo, hi, glm::vec2(v.x, v.y));
            }
            else if (t.kind == K::Mpr && !t.mprSnapshots.empty())
            {
                const int si = std::clamp(step, 0, (int)t.mprSnapshots.size() - 1);
                const auto& m = t.mprSnapshots[si];
                Acc(lo, hi, glm::vec2(m.v0.x, m.v0.y));
                Acc(lo, hi, glm::vec2(m.v1.x, m.v1.y));
                Acc(lo, hi, glm::vec2(m.v2.x, m.v2.y));
            }

            MinkowskiFit fit;
            glm::vec2 ext = hi - lo;
            // Pad ~10% (and guard a degenerate zero-extent so the scale is finite).
            const float pad = std::max(std::max(ext.x, ext.y) * 0.1f, 1.0f);
            lo -= glm::vec2(pad, pad);
            hi += glm::vec2(pad, pad);
            ext = hi - lo;
            const float sx = (ext.x > 1e-4f) ? (w / ext.x) : 1.0f;
            const float sy = (ext.y > 1e-4f) ? (h / ext.y) : 1.0f;
            const float s  = std::min(sx, sy);    // uniform: preserve MD aspect
            fit.mn    = lo;
            fit.scale = glm::vec2(s, s);
            // Center the (possibly non-square) content in the inset.
            fit.origin = glm::vec2((w - ext.x * s) * 0.5f, (h - ext.y * s) * 0.5f);
            return fit;
        }

        // Draw a 2-stroke arrowhead at `tip`, pointing along `dir` (unit), with the
        // given backsweep length (px) + half-spread angle. Used by the SAT chosen-axis
        // normal + the analytic normal so the direction reads unambiguously.
        void DrawArrowhead(Batcher2D& b, glm::vec2 tip, glm::vec2 dir, float thickness,
                           glm::vec4 color, float len = 7.0f)
        {
            const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (dl <= 1e-5f) return;
            const glm::vec2 u(dir.x / dl, dir.y / dl);   // forward
            const glm::vec2 p(-u.y, u.x);                // perpendicular
            // Two strokes sweeping back from the tip at +-30deg (cos/sin of 150deg).
            const glm::vec2 back = -u * (len * 0.866f);  // cos30
            const glm::vec2 side = p * (len * 0.5f);     // sin30
            b.Line(tip, tip + back + side, thickness, color);
            b.Line(tip, tip + back - side, thickness, color);
        }

        // Draw the Minkowski geometry of the current step into the inset Batcher2D.
        // Homogenized: ONLY Batcher2D primitives, no ImDrawList. Coordinates are inset
        // pixels (y down -- the OffscreenCanvas convention).
        void DrawMinkowskiInset(Batcher2D& b,
                                const Arcane::Physics::NarrowphaseTrace& t,
                                int step, float w, float h)
        {
            using K = Arcane::Physics::NarrowphaseKind;
            const MinkowskiFit fit = BuildFit(t, step, w, h);

            constexpr glm::vec4 kCrosshair{0.80f, 0.80f, 0.88f, 1.0f};  // origin (brighter)
            constexpr glm::vec4 kSeed     {0.30f, 1.00f, 0.55f, 0.9f};  // GJK seed green
            constexpr glm::vec4 kPoly     {0.40f, 0.70f, 1.00f, 1.0f};  // EPA polytope blue
            constexpr glm::vec4 kEdge     {1.00f, 0.85f, 0.20f, 1.0f};  // closest edge gold
            constexpr glm::vec4 kNormal   {1.00f, 0.25f, 1.00f, 1.0f};  // edge normal magenta
            constexpr glm::vec4 kPortal   {0.80f, 0.45f, 1.00f, 1.0f};  // MPR portal violet
            constexpr glm::vec4 kVert     {1.00f, 1.00f, 1.00f, 1.0f};  // vertex discs
            constexpr glm::vec4 kAxisDim  {0.45f, 0.45f, 0.52f, 0.7f};  // SAT candidate axes
            constexpr glm::vec4 kAxisCur  {0.65f, 0.80f, 0.95f, 0.9f};  // SAT current-step axis
            constexpr glm::vec4 kChosen   {1.00f, 0.80f, 0.20f, 1.0f};  // SAT chosen axis gold
            constexpr glm::vec4 kClosest  {0.30f, 1.00f, 0.55f, 1.0f};  // analytic closest MD pt

            // ---- SAT (poly-poly) + analytic kinds draw the family of SEPARATING
            //      AXES / the closest-point construction around a FIXED, origin-
            //      centered radius. The MD-AABB fit is meaningless for these (the
            //      satAxes `dir` are UNIT directions; analytic data lives in the
            //      manifold), so we center the origin and pick a fixed radius. ----
            const bool directional =
                (t.kind == K::SatPolygon) ||
                (t.kind == K::CircleCircle) ||
                (t.kind == K::CircleVsPolygon) ||
                (t.kind == K::Capsule);

            // Centered origin + a fixed inset radius for the directional kinds.
            const glm::vec2 oc(w * 0.5f, h * 0.5f);
            const float     rad = 0.40f * std::min(w, h);

            // Origin crosshair (the Minkowski origin -- overlap iff it is enclosed).
            // Directional kinds center it; algorithmic kinds use the fit transform.
            const glm::vec2 o = directional ? oc : fit.ToPx(glm::vec2(0.0f, 0.0f));
            b.Line(glm::vec2(o.x - 9.0f, o.y), glm::vec2(o.x + 9.0f, o.y), 1.5f, kCrosshair);
            b.Line(glm::vec2(o.x, o.y - 9.0f), glm::vec2(o.x, o.y + 9.0f), 1.5f, kCrosshair);
            b.Circle(o, 2.0f, kCrosshair);

            // ---- SatPolygon: the family of tested separating axes ----------------
            if (t.kind == K::SatPolygon && !t.satAxes.empty())
            {
                const int cur = std::clamp(step, 0, (int)t.satAxes.size() - 1);
                int chosenIdx = -1;

                // Pass 1: every candidate axis as a full grey line through the origin
                // (so the family of tested axes is visible). The slider-current axis
                // gets a slightly brighter tint so the step control reads.
                for (std::size_t i = 0; i < t.satAxes.size(); ++i)
                {
                    const auto& ax = t.satAxes[i];
                    if (ax.chosen) chosenIdx = (int)i;
                    const float dl =
                        std::sqrt(ax.dir.x * ax.dir.x + ax.dir.y * ax.dir.y);
                    if (dl <= 1e-5f) continue;
                    const glm::vec2 u(ax.dir.x / dl, ax.dir.y / dl);
                    const glm::vec2 e = u * rad;
                    const bool isCur = ((int)i == cur);
                    b.Line(o - e, o + e, isCur ? 1.6f : 1.0f,
                           isCur ? kAxisCur : kAxisDim);
                }

                // Pass 2: the CHOSEN axis (min-penetration reference axis == contact
                // normal) BOLD gold with an arrowhead along the penetration normal.
                if (chosenIdx >= 0)
                {
                    const auto& ax = t.satAxes[(std::size_t)chosenIdx];
                    const float dl =
                        std::sqrt(ax.dir.x * ax.dir.x + ax.dir.y * ax.dir.y);
                    if (dl > 1e-5f)
                    {
                        const glm::vec2 u(ax.dir.x / dl, ax.dir.y / dl);
                        const glm::vec2 tip = o + u * rad;
                        b.Line(o - u * rad, tip, 3.0f, kChosen);
                        DrawArrowhead(b, tip, u, 3.0f, kChosen, 11.0f);

                        // Projection-interval overlap on the chosen axis: the gap
                        // between the reference max and the incident min along `dir`
                        // IS the penetration depth. Draw both intervals as short
                        // parallel bars offset to either side of the axis, scaled so
                        // the wider interval spans the inset (legible, not metric).
                        const float lenRef = (float)(ax.maxRef - ax.minRef);
                        const float lenInc = (float)(ax.maxInc - ax.minInc);
                        const float span   = std::max(std::max(lenRef, lenInc), 1e-3f);
                        const float k      = (rad * 1.6f) / span;   // px per world unit
                        // Anchor both intervals around the axis center so the overlap
                        // is centered on the inset. Use the interval midpoints.
                        const float midRef = 0.5f * (float)(ax.minRef + ax.maxRef);
                        const float midInc = 0.5f * (float)(ax.minInc + ax.maxInc);
                        const float mid    = 0.5f * (midRef + midInc);
                        const glm::vec2 p(-u.y, u.x);   // perpendicular offset dir
                        auto Bar = [&](float lo, float hi, float side, glm::vec4 col)
                        {
                            const glm::vec2 a = o + u * ((lo - mid) * k) + p * side;
                            const glm::vec2 c = o + u * ((hi - mid) * k) + p * side;
                            b.Line(a, c, 2.5f, col);
                            // End caps so the interval extent reads.
                            b.Line(a - p * 3.0f, a + p * 3.0f, 1.5f, col);
                            b.Line(c - p * 3.0f, c + p * 3.0f, 1.5f, col);
                        };
                        Bar((float)ax.minRef, (float)ax.maxRef, -8.0f, kPoly);   // reference
                        Bar((float)ax.minInc, (float)ax.maxInc, +8.0f, kSeed);   // incident
                    }
                }
                return;   // SAT fully drawn; no MD polytope/seed for this kind.
            }

            // ---- Analytic (CircleCircle / CircleVsPolygon / Capsule): a basic
            //      closest-point / normal construction from the FINAL manifold. The
            //      MD-difference closest point to the origin lies at -normal*depth
            //      (B->A normal, depth = max separation). ----
            if (t.kind == K::CircleCircle || t.kind == K::CircleVsPolygon ||
                t.kind == K::Capsule)
            {
                const glm::vec2 nrm(t.manifold.normal.x, t.manifold.normal.y);
                const float nl = std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y);
                if (nl > 1e-5f)
                {
                    const glm::vec2 u(nrm.x / nl, nrm.y / nl);   // B->A, unit
                    // Contact normal as an arrow from the origin (length = fixed).
                    const glm::vec2 tip = o + u * rad;
                    b.Line(o, tip, 2.5f, kNormal);
                    DrawArrowhead(b, tip, u, 2.5f, kNormal, 10.0f);

                    // Closest MD point to the origin: -normal * depth. Place it at a
                    // fraction of the inset radius opposite the normal so it is always
                    // visible (depth is metric; this is a schematic, not to scale).
                    float depth = 0.0f;
                    for (int pi = 0; pi < t.manifold.pointCount; ++pi)
                        depth = std::max(depth,
                                         (float)t.manifold.points[pi].separation);
                    const float frac = (depth > 1e-4f) ? 0.5f : 0.0f;
                    const glm::vec2 cp = o - u * (rad * frac);
                    b.Line(o, cp, 1.5f, kClosest);
                    b.Circle(cp, 3.5f, kClosest);
                }
                return;   // analytic fully drawn.
            }

            // GJK seed simplex (point / segment / triangle) -- drawn alongside.
            for (const auto& s : t.gjkSnapshots)
            {
                if (s.count <= 0) continue;
                for (int i = 0; i < s.count; ++i)
                {
                    const glm::vec2 a = fit.ToPx(glm::vec2(s.verts[i].x, s.verts[i].y));
                    const glm::vec2 c = fit.ToPx(glm::vec2(s.verts[(i + 1) % s.count].x,
                                                           s.verts[(i + 1) % s.count].y));
                    if (s.count >= 2) b.Line(a, c, 1.5f, kSeed);
                    b.Circle(a, 3.0f, kVert);
                }
            }

            if (t.kind == K::Epa && !t.epaSnapshots.empty())
            {
                const int si = std::clamp(step, 0, (int)t.epaSnapshots.size() - 1);
                const auto& poly = t.epaSnapshots[si];
                const std::size_t vc = poly.verts.size();
                for (std::size_t i = 0; i < vc; ++i)
                {
                    const glm::vec2 a = fit.ToPx(glm::vec2(poly.verts[i].x, poly.verts[i].y));
                    const glm::vec2 c = fit.ToPx(glm::vec2(poly.verts[(i + 1) % vc].x,
                                                           poly.verts[(i + 1) % vc].y));
                    b.Line(a, c, 1.5f, kPoly);
                    b.Circle(a, 2.5f, kVert);
                }
                // Highlight the closest edge + its outward normal (from the edge midpoint).
                if (vc >= 2 && poly.edgeA >= 0 && poly.edgeB >= 0 &&
                    (std::size_t)poly.edgeA < vc && (std::size_t)poly.edgeB < vc)
                {
                    const glm::vec2 ea = fit.ToPx(glm::vec2(poly.verts[poly.edgeA].x,
                                                            poly.verts[poly.edgeA].y));
                    const glm::vec2 eb = fit.ToPx(glm::vec2(poly.verts[poly.edgeB].x,
                                                            poly.verts[poly.edgeB].y));
                    b.Line(ea, eb, 3.0f, kEdge);
                    const glm::vec2 mid = (ea + eb) * 0.5f;
                    const glm::vec2 nrm(poly.edgeNormal.x, poly.edgeNormal.y);
                    const float nl = std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y);
                    if (nl > 1e-5f)
                    {
                        // The MD normal is a direction; draw a fixed-px ray (the inset is
                        // not metric-uniform with world). y down -> use the raw normal.
                        const glm::vec2 tip = mid + glm::vec2(nrm.x / nl, nrm.y / nl) * 24.0f;
                        b.Line(mid, tip, 2.0f, kNormal);
                    }
                }
            }
            else if (t.kind == K::Mpr && !t.mprSnapshots.empty())
            {
                const int si = std::clamp(step, 0, (int)t.mprSnapshots.size() - 1);
                const auto& m = t.mprSnapshots[si];
                const glm::vec2 v0 = fit.ToPx(glm::vec2(m.v0.x, m.v0.y));
                const glm::vec2 v1 = fit.ToPx(glm::vec2(m.v1.x, m.v1.y));
                const glm::vec2 v2 = fit.ToPx(glm::vec2(m.v2.x, m.v2.y));
                // Portal triangle: v0 (interior seed) + the v1-v2 portal edge.
                b.Line(v1, v2, 3.0f, kEdge);       // the portal edge (gold)
                b.Line(v0, v1, 1.5f, kPortal);
                b.Line(v0, v2, 1.5f, kPortal);
                b.Circle(v0, 3.0f, kVert);
                b.Circle(v1, 2.5f, kVert);
                b.Circle(v2, 2.5f, kVert);
                // Search ray from the interior seed along rayDir.
                const glm::vec2 rd(m.rayDir.x, m.rayDir.y);
                const float rl = std::sqrt(rd.x * rd.x + rd.y * rd.y);
                if (rl > 1e-5f)
                    b.Line(v0, v0 + glm::vec2(rd.x / rl, rd.y / rl) * 24.0f, 2.0f, kNormal);
            }
        }
    }

    void Hud::Draw(SandboxApp& app, Astra::Registry& reg)
    {
        const std::span<const SceneDef> scenes = SceneRegistry();
        const int sceneCount = static_cast<int>(scenes.size());
        const int current    = static_cast<int>(app.CurrentScene());

        ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Sandbox"))
        {
            ImGui::End();
            return;
        }

        // ---- Scene ----------------------------------------------------------------
        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* curName =
                (current >= 0 && current < sceneCount) ? scenes[current].name : "?";
            if (ImGui::BeginCombo("##scene", curName))
            {
                for (int i = 0; i < sceneCount; ++i)
                {
                    const bool selected = (i == current);
                    if (ImGui::Selectable(scenes[i].name, selected) && i != current)
                        RequestScene(reg, i);
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            if (ImGui::Button("Prev") && sceneCount > 0)
                RequestScene(reg, (current - 1 + sceneCount) % sceneCount);
            ImGui::SameLine();
            if (ImGui::Button("Next") && sceneCount > 0)
                RequestScene(reg, (current + 1) % sceneCount);
            ImGui::SameLine();
            if (ImGui::Button("Reset"))
                RequestReset(reg);
        }

        // ---- Sim ------------------------------------------------------------------
        if (ImGui::CollapsingHeader("Sim", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool paused = app.IsPaused();
            if (ImGui::Checkbox("Paused", &paused))
                app.SetPaused(paused);

            ImGui::SameLine();
            // Single-step is only meaningful while paused (a running sim steps anyway).
            ImGui::BeginDisabled(!paused);
            if (ImGui::Button("Step"))
                app.RequestSingleStep();
            ImGui::EndDisabled();

            float timeScale = app.TimeScale();
            if (ImGui::SliderFloat("Time scale", &timeScale, 0.05f, 4.0f, "%.2fx"))
                app.SetTimeScale(timeScale);

            // Gravity: PhysicsWorld bakes gravity at construction (no runtime setter),
            // so a change rebuilds the current scene with a fresh world at the new value.
            float gravityY = app.GravityY();
            if (ImGui::SliderFloat("Gravity Y", &gravityY, 0.0f, 2400.0f, "%.0f"))
            {
                app.SetGravityY(gravityY);
                RequestReset(reg);   // mint a fresh world at the new gravity (deferred)
            }
        }

        // ---- Spawn ----------------------------------------------------------------
        // Unified spawn section: one segmented shape-button row selects the active shape
        // (Box / Circle / Capsule / Polygon). Box/Circle/Capsule spawn on LMB-press over
        // empty space; Polygon enters vertex-collection mode (click to add, Enter/KP_Enter
        // to commit, Backspace to undo last, Esc to clear). The old separate Polygon
        // header is removed -- shape is the single knob.
        if (ImGui::CollapsingHeader("Spawn", ImGuiTreeNodeFlags_DefaultOpen))
        {
            Sandbox::SpawnConfig& cfg = app.SpawnConfigMut();

            // Segmented shape buttons: highlight the active shape with an accent color.
            static const char* kShapeNames[] = { "Box", "Circle", "Capsule", "Polygon" };
            static constexpr int kShapeCount = 4;
            const ImVec4 kAccent{0.95f, 0.55f, 0.25f, 1.0f};   // orange accent

            for (int i = 0; i < kShapeCount; ++i)
            {
                const bool active = (static_cast<int>(cfg.shape) == i);
                if (active)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button,        kAccent);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1.0f, 0.70f, 0.45f, 1.0f});
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.80f, 0.40f, 0.15f, 1.0f});
                }
                if (ImGui::Button(kShapeNames[i]))
                    cfg.shape = static_cast<SpawnShape>(i);
                if (active)
                    ImGui::PopStyleColor(3);

                if (i < kShapeCount - 1)
                    ImGui::SameLine();
            }

            // Density applies to every shape.
            ImGui::SliderFloat("Density", &cfg.density, 0.1f, 10.0f, "%.1f");

            // Adaptive per-shape controls.
            if (cfg.shape == SpawnShape::Polygon)
            {
                const std::size_t pts = app.PolygonPointCount();
                ImGui::Text("Points: %zu", pts);

                ImGui::BeginDisabled(pts == 0);
                if (ImGui::Button("Clear"))
                    app.ClearPolygonPoints();
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(pts < 3);   // need >= 3 verts
                // "##polygon" gives a unique ID: the visible label is still "Spawn",
                // but the bare "Spawn" string collides with the "Spawn" CollapsingHeader
                // (a header opens no ID scope for its contents), which ImGui flags.
                if (ImGui::Button("Spawn##polygon"))
                    app.RequestPolygonSpawn();
                ImGui::EndDisabled();

                ImGui::TextDisabled(
                    "Click to add a vertex - Enter spawn / Backspace undo / Esc clear");
            }
            else
            {
                ImGui::SliderFloat("Size", &cfg.size, 4.0f, 80.0f, "%.0f");
                ImGui::TextDisabled("Left-click empty space to spawn");
            }
        }

        // ---- Debug draw -----------------------------------------------------------
        if (ImGui::CollapsingHeader("Debug draw", ImGuiTreeNodeFlags_DefaultOpen))
        {
            SandboxDebugDraw& dbg = app.DebugOptionsMut();
            ImGui::Checkbox("Contacts", &dbg.drawContacts);
            ImGui::Checkbox("AABBs", &dbg.drawAabbs);
            ImGui::SliderFloat("Line thickness", &dbg.lineThickness, 1.0f, 4.0f, "%.1f");

            ImGui::Separator();

            // Rich per-body overlays (each with its scalar; disabled when its
            // flag is off so the slider only edits a live overlay).
            ImGui::Checkbox("Velocities", &dbg.drawVelocities);
            ImGui::BeginDisabled(!dbg.drawVelocities);
            ImGui::SliderFloat("Vel scale", &dbg.velocityScale, 0.02f, 0.5f, "%.2fs");
            ImGui::EndDisabled();

            ImGui::Checkbox("COM markers", &dbg.drawComMarkers);
            ImGui::BeginDisabled(!dbg.drawComMarkers);
            ImGui::SliderFloat("COM size", &dbg.comMarkerSize, 2.0f, 16.0f, "%.0f");
            ImGui::EndDisabled();

            ImGui::Checkbox("Orientation ticks", &dbg.drawOrientations);
            ImGui::BeginDisabled(!dbg.drawOrientations);
            ImGui::SliderFloat("Tick length", &dbg.orientationTickLen, 4.0f, 48.0f, "%.0f");
            ImGui::EndDisabled();

            ImGui::Separator();

            // Slice A broadphase + manifold overlays (default off). "Contact
            // manifolds" is ADDED ALONGSIDE the legacy "Contacts" center-to-center
            // line above -- both can be on at once.
            ImGui::Checkbox("Fixture tree", &dbg.drawFixtureTree);
            ImGui::Checkbox("Static tree", &dbg.drawStaticGrid);
            ImGui::Checkbox("Residency grid", &dbg.drawResidencyGrid);
            ImGui::Checkbox("Contact manifolds", &dbg.drawManifolds);

            ImGui::Separator();

            // ---- narrowphase inspector (subject model) ------------------------
            // No toggle: the inspector is ALWAYS on. The subject is the body you GRAB
            // to drag (a side effect of the normal grab). When a subject exists, the
            // "Narrowphase Inspector" window opens below + the world overlay draws ALL
            // its contacts. The clear affordance lives in that window.
            if (app.HasSubject())
                ImGui::TextDisabled("Inspector subject set (grab a shape to switch)");
            else
                ImGui::TextDisabled("Grab a shape to inspect its contacts");
        }

        // ---- Stats ----------------------------------------------------------------
        if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen))
        {
            std::uint32_t bodies   = 0;
            std::size_t   contacts = 0;
            if (const PhysicsResource* phys = reg.GetResource<PhysicsResource>();
                phys && phys->world)
            {
                bodies   = phys->world->Count();
                contacts = phys->world->ActiveContactCount();
            }

            const ImGuiIO& io = ImGui::GetIO();
            const float fps   = io.Framerate;
            const float stepMs = (fps > 0.0f) ? (1000.0f / fps) : 0.0f;

            ImGui::Text("Bodies:   %u", bodies);
            ImGui::Text("Contacts: %zu", contacts);
            ImGui::Text("FPS:      %.1f  (%.2f ms)", fps, stepMs);
        }

        ImGui::End();

        // ---- Slice B: the Narrowphase Inspector window (only when a subject is set) ----------
        DrawInspectorWindow(app);
    }

    // -------------------------------------------------------------------------
    // DrawInspectorWindow (subject model): a separate ImGui window showing the
    // SUBJECT's header (ids + active-contact count + "Clear subject") + a partner
    // SELECTOR (defaults to the deepest contact) + the SELECTED contact's narrowphase
    // header + step slider/play/step + the Minkowski inset. Drawn only when a subject
    // exists. The OffscreenCanvas::Draw runs HERE, right before the ImGui::Image samples
    // its texture: Draw self-executes synchronously on the graphics queue, and the host
    // renders the ImGui draw data AFTER this DrawUI phase (same queue, ordered), so the
    // texture is valid when sampled.
    // -------------------------------------------------------------------------
    void Hud::DrawInspectorWindow(SandboxApp& app)
    {
        if (!app.HasSubject())
            return;

        ImGui::SetNextWindowSize(ImVec2(380.0f, 460.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(370.0f, 16.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Narrowphase Inspector"))
        {
            ImGui::End();
            return;
        }

        const std::vector<Arcane::Sandbox::ContactView>& contacts = app.Contacts();
        const int contactCount = static_cast<int>(contacts.size());

        // ---- subject header: ids + contact count + Clear subject ---------------
        ImGui::Text("Subject fixture: #%u.g%u   body #%u",
                    app.Subject().index, app.Subject().generation,
                    app.SubjectBody().index);
        ImGui::Text("Active contacts: %d", contactCount);
        ImGui::SameLine();
        if (ImGui::Button("Clear subject"))
            app.ClearSubject();

        ImGui::Separator();

        // ---- 0-contact early-out: subject is touching nothing ------------------
        if (contactCount == 0)
        {
            ImGui::TextDisabled("subject not touching anything");
            // Still draw an empty/idle inset (fit-to-bounds handles no geometry).
            ImGui::TextUnformatted("Minkowski space (origin enclosed == overlap):");
            const ImVec2 availE = ImGui::GetContentRegionAvail();
            const uint32_t iwE = static_cast<uint32_t>(std::max(64.0f, availE.x));
            const uint32_t ihE = static_cast<uint32_t>(std::max(64.0f, availE.y));
            if (Arcane::OffscreenCanvas* ocE = app.EnsureInspectorCanvas(iwE, ihE))
            {
                const Arcane::Physics::NarrowphaseTrace& te = app.SelectedTrace();
                const float fwE = static_cast<float>(ocE->Width());
                const float fhE = static_cast<float>(ocE->Height());
                ocE->Draw(
                    [&te, fwE, fhE](Batcher2D& b) { DrawMinkowskiInset(b, te, 0, fwE, fhE); },
                    glm::vec4(0.03f, 0.03f, 0.05f, 1.0f));
                ImGui::Image(static_cast<ImTextureID>(ocE->TextureId()),
                             ImVec2(static_cast<float>(ocE->Width()),
                                    static_cast<float>(ocE->Height())));
            }
            else
            {
                ImGui::TextDisabled("(inset unavailable: no render device)");
            }
            ImGui::End();
            return;
        }

        // Max penetration depth over a trace's manifold (the shared "depth" label value).
        auto MaxDepth = [](const Arcane::Physics::NarrowphaseTrace& t) {
            float d = 0.0f;
            for (int pi = 0; pi < t.manifold.pointCount; ++pi)
                d = std::max(d, static_cast<float>(t.manifold.points[pi].separation));
            return d;
        };

        // ---- partner selector (defaults to the deepest; re-points inset/step) --
        // Selection is kept stable across frames by partner id inside SandboxApp; this
        // combo just lets the user re-point it. Each entry is labelled with the partner
        // body id, the narrowphase kind, and the max penetration depth.
        const int sel = std::clamp(app.SelectedIndex(), 0, contactCount - 1);

        char curLabel[96];
        {
            const Arcane::Physics::NarrowphaseTrace& ts = contacts[static_cast<std::size_t>(sel)].trace;
            std::snprintf(curLabel, sizeof(curLabel), "vs body #%u - %s - depth %.2f",
                          contacts[static_cast<std::size_t>(sel)].partnerBodyId,
                          KindName(ts.kind), static_cast<double>(MaxDepth(ts)));
        }

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##partner", curLabel))
        {
            for (int i = 0; i < contactCount; ++i)
            {
                const Arcane::Physics::NarrowphaseTrace& ti = contacts[static_cast<std::size_t>(i)].trace;
                char lbl[96];
                std::snprintf(lbl, sizeof(lbl), "vs body #%u - %s - depth %.2f",
                              contacts[static_cast<std::size_t>(i)].partnerBodyId,
                              KindName(ti.kind), static_cast<double>(MaxDepth(ti)));
                const bool selected = (i == sel);
                if (ImGui::Selectable(lbl, selected) && i != sel)
                    app.SetSelectedIndex(i);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // ---- SELECTED contact: kind, normal, depth, pointCount -----------------
        const Arcane::Physics::NarrowphaseTrace& t = app.SelectedTrace();
        ImGui::Text("Kind: %s", KindName(t.kind));
        ImGui::Text("Normal (B->A): (%.3f, %.3f)",
                    static_cast<double>(t.manifold.normal.x),
                    static_cast<double>(t.manifold.normal.y));
        ImGui::Text("Points: %d   Max depth: %.3f", t.manifold.pointCount,
                    static_cast<double>(MaxDepth(t)));

        ImGui::Separator();

        // ---- step control over the SELECTED contact ----------------------------
        const int stepCount = app.RecordedStepCount();
        if (stepCount <= 0)
        {
            ImGui::TextDisabled("analytic -- no iterations");
        }
        else
        {
            int step = app.StepIndex();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt("##step", &step, 0, stepCount - 1, "step %d"))
                app.SetStepIndex(step);
            ImGui::SameLine();
            if (ImGui::Button("<") && step > 0)
                app.SetStepIndex(step - 1);
            ImGui::SameLine();
            if (ImGui::Button(">") && step < stepCount - 1)
                app.SetStepIndex(step + 1);
            ImGui::SameLine();
            // "Play" advances one step per frame (wraps), so the polytope/portal
            // animates expanding. The flag lives on SandboxApp so it resets on
            // ClearSubject / a partner switch rather than persisting like a static.
            bool play = app.InspectorPlay();
            if (ImGui::Checkbox("Play", &play))
                app.SetInspectorPlay(play);
            if (play)
                app.SetStepIndex((app.StepIndex() + 1) % stepCount);
        }

        ImGui::Separator();

        // ---- Minkowski inset (the SELECTED contact's trace) --------------------
        ImGui::TextUnformatted("Minkowski space (origin enclosed == overlap):");
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const uint32_t iw = static_cast<uint32_t>(std::max(64.0f, avail.x));
        const uint32_t ih = static_cast<uint32_t>(std::max(64.0f, avail.y));

        Arcane::OffscreenCanvas* oc = app.EnsureInspectorCanvas(iw, ih);
        if (oc)
        {
            const int   step = app.StepIndex();
            const float fw   = static_cast<float>(oc->Width());
            const float fh   = static_cast<float>(oc->Height());
            // Self-executes synchronously; the texture is shader-readable on return.
            oc->Draw(
                [&t, step, fw, fh](Batcher2D& b)
                {
                    DrawMinkowskiInset(b, t, step, fw, fh);
                },
                glm::vec4(0.03f, 0.03f, 0.05f, 1.0f));
            ImGui::Image(static_cast<ImTextureID>(oc->TextureId()),
                         ImVec2(static_cast<float>(oc->Width()),
                                static_cast<float>(oc->Height())));
        }
        else
        {
            // Headless / no device: the inset is unavailable; the world overlay + the
            // header/step still work (the inset is a render-only nicety).
            ImGui::TextDisabled("(inset unavailable: no render device)");
        }

        ImGui::End();
    }
}
