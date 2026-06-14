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

#include <glm/vec2.hpp>

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
            std::uint32_t id         = 0;
        };

        // ----------------------------------------------------------------
        // Manifold: normal (shared) + up to 2 contact points.
        // ----------------------------------------------------------------
        //
        // normal     : unit contact normal, points from B toward A (push A out
        //              of B) -- the Geometry.polyPoly orientation, shared by all
        //              points (matching the Lua, which stores nx,ny per row but
        //              they are identical across the pair's rows).
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
