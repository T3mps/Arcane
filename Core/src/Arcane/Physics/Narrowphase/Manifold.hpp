#pragma once

// Contact manifold generation for convex polygons + AABBs (M6, Task P1.2).
//
// PORT NOTE: a faithful port of the polygon path of Manifold.lua's polyVsPoly
// (Client/src/physics/Manifold.lua lines 73-114) plus worldPoly's aabb branch.
// The Lua module is the behavioral oracle; the reference outputs captured by
// Client/src/tests/physics_oracle_capture/ (manifold.json) pin the result
// bit-for-bit (within f32 tolerance).
//
// ALGORITHM (NOT incident-face clipping): the Lua does NOT clip an incident
// face. It takes the SAT normal from Geometry.polyPoly and finds contact points
// as the CONTAINED VERTICES -- verts of A inside B (depth = maxB - vA.n) and
// verts of B inside A (depth = vB.n - minA) -- keeping the 2 DEEPEST with the
// Lua's exact iteration order (A's verts first, then B's verts; the `consider`
// shuffle keeps c1=deepest, c2=second). We reproduce that order so the emitted
// point SET + order match the oracle. (Box2D-style face clipping would NOT
// bit-match; we follow the Lua where it decides the algorithm.)
//
// SCOPE (P1.2): polygon<->polygon and polygon<->AABB ONLY. Round shapes
// (circle/capsule) are P1.3. An AABB is treated as a 4-vertex polygon via the
// worldPoly corner expansion: (x-hw,y-hh),(x+hw,y-hh),(x+hw,y+hh),(x-hw,y+hh).
// Rotation is identity in this phase (Transform.rotation unused, translate-only).
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/Batcher2D/ImGui, no C++23-only features.

#include <cstdint>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>

namespace Arcane
{
    namespace Physics
    {
        // ----------------------------------------------------------------
        // ManifoldPoint: one contact point.
        // ----------------------------------------------------------------
        //
        // point      : world-space contact position (a contained vertex).
        // separation : penetration depth. POSITIVE when overlapping (the Lua's
        //              per-point `depth`); NEGATIVE for a speculative contact
        //              (a gap, within [-margin, 0)) when a skin margin is used.
        // normal     : THIS contact's own unit normal, points from B toward A
        //              (push A out of B) -- the Lua contact-row `nx,ny`. Each
        //              point carries its own normal because round-vs-poly
        //              endpoint contacts (e.g. a capsule whose two ends touch
        //              DIFFERENT polygon faces) can have DIFFERENT normals per
        //              point. For poly-poly (single SAT axis) every point's
        //              normal equals Manifold::normal.
        // id          : the Lua `key` analog -- a stable per-pair feature key the
        //              solver uses for warm-starting. We reproduce the Lua's
        //              scheme exactly: id = keyBase + slot, where slot is 1 for
        //              the deepest point and 2 for the second-deepest (so the
        //              first manifold point carries keyBase+1, the second
        //              keyBase+2). This matches manifold.json's `key` field.
        struct ManifoldPoint
        {
            Vec2          point{ Real(0), Real(0) };
            Real          separation = Real(0);
            Vec2          normal{ Real(0), Real(0) };
            std::uint32_t id         = 0;
        };

        // ----------------------------------------------------------------
        // Manifold: representative normal + up to 2 contact points (each with
        // its own normal).
        // ----------------------------------------------------------------
        //
        // normal     : a REPRESENTATIVE unit contact normal, points from B
        //              toward A (push A out of B). It is the Box2D-v3-style
        //              single-axis representative -- the normal of the DEEPEST
        //              contact point (deterministic tiebreak: on equal
        //              separation the first-emitted / lowest-key point wins).
        //              For poly-poly (one SAT axis) every point shares this
        //              normal, so it is exactly the SAT axis. It is kept for
        //              convenience and for poly-poly consumers; the per-point
        //              ManifoldPoint::normal preserves the Lua's per-contact
        //              fidelity (round endpoints touching different faces) that
        //              the future solver needs.
        // pointCount : 0, 1, or 2.
        struct Manifold
        {
            Vec2          normal{ Real(0), Real(0) };
            int           pointCount = 0;
            ManifoldPoint points[2]{};
        };

        // Generate the contact manifold between two polygon/AABB shapes under
        // their (translation-only) transforms. PORT of Manifold.lua polyVsPoly
        // (the poly path of genPair) + worldPoly. AABB shapes are expanded to
        // their 4 world corners before SAT.
        //
        // keyBase is the per-pair base for the warm-start ids (the Lua genPair
        // arg): emitted points carry id = keyBase + 1 (deepest) and
        // keyBase + 2 (second). Defaults to 0 (matching the oracle capture,
        // which passes keyBase = 0).
        //
        // speculativeMargin (MODERNIZATION, kSkin):
        //   - == 0: bit-match Manifold.lua. Only contacts with depth > 0 are
        //     emitted (real penetration), exactly as the Lua's `consider`.
        //   - > 0 : ALSO report a near-touching pair (separation in [-margin, 0))
        //     when the polygons do not overlap but are within the skin -- a
        //     single speculative contact so the solver sees it pre-penetration.
        //     The faithful path is unchanged when margin == 0, so parity holds.
        [[nodiscard]] Manifold CollidePolygons(const Shape& a, const Transform& xfA,
                                               const Shape& b, const Transform& xfB,
                                               Real speculativeMargin = Real(0),
                                               std::uint32_t keyBase   = 0);

    } // namespace Physics
} // namespace Arcane
