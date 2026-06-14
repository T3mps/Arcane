#pragma once

// Round-shape contact manifolds for the Arcane 2D physics narrowphase (M6,
// Task P1.3): circle/capsule vs circle/capsule/polygon/aabb.
//
// PORT NOTE: a faithful port of the ROUND paths of Manifold.lua's genPair
// (Client/src/physics/Manifold.lua lines 42-71, 116-177) plus the round
// geometry kernels in Geometry.lua (circleCircle / circlePoly / capsulePoly --
// ported in GeometryKernel.hpp/.cpp). The Lua module is the behavioral oracle;
// the reference outputs captured by Client/src/tests/physics_oracle_capture/
// (round_manifold.json) pin the result bit-for-bit (within f32 tolerance).
//
// MODEL (Manifold.lua roundView): a round shape is viewed as a set of endpoint
// circles. A CIRCLE is one circle at its center; a CAPSULE is two circles at
// the rotated segment endpoints (this phase: rotation identity, so the segment
// is horizontal, endpoints (x-halfLen, y) and (x+halfLen, y)). A flat-resting
// capsule therefore yields TWO contacts (one per endpoint).
//
// CONVENTION: the stored Manifold.normal points from B toward A (push A out of
// B), exactly as the poly path (P1.2) and the Lua. circlePoly returns the
// push-out for the QUERIED circle, so the poly-vs-round path flips it.
//
// SCOPE (P1.3): every pair where AT LEAST ONE shape is round. The pure
// poly-poly / poly-aabb path is P1.2's CollidePolygons -- NOT duplicated here.
// Rotation is identity this phase (Transform.rotation unused, translate-only);
// the rotation plumbing in roundView is preserved but inactive.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/Batcher2D/ImGui, no C++23-only features (this module is also
// compiled static-CRT/C++20 in the server flavor). namespace Arcane::Physics.

#include <cstdint>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Narrowphase/Manifold.hpp>

namespace Arcane
{
    namespace Physics
    {
        // Circle/capsule A vs circle/capsule B. PORT of the round-round branch
        // of genPair (Manifold.lua:135-150): pairwise endpoint circles via
        // circleCircle, keep the single DEEPEST contact; contact point
        //   (pxb + nx*(rb - depth/2), pyb + ny*(rb - depth/2))
        // normal points from B toward A, key = keyBase + 1.
        //
        // Both shapes MUST be round (Circle or Capsule).
        [[nodiscard]] Manifold CollideRoundRound(const Shape& a, const Transform& xfA,
                                                 const Shape& b, const Transform& xfB,
                                                 Real speculativeMargin = Real(0),
                                                 std::uint32_t keyBase   = 0);

        // Round A vs polygon/aabb B. PORT of roundVsPoly (Manifold.lua:60-71):
        // for each endpoint circle of A, circlePoly against B's world verts;
        // emit a contact at (px - nx*(r-depth), py - ny*(r-depth)) with normal
        // (nx,ny) (push A out of B) and key = keyBase + k. A flat-resting
        // capsule yields TWO contacts (k=1 then k=2).
        //
        // A MUST be round; B MUST be Polygon or Aabb.
        [[nodiscard]] Manifold CollideRoundPolygon(const Shape& a, const Transform& xfA,
                                                   const Shape& b, const Transform& xfB,
                                                   Real speculativeMargin = Real(0),
                                                   std::uint32_t keyBase   = 0);

        // Polygon/aabb A vs round B. PORT of the poly-vs-round branch of genPair
        // (Manifold.lua:161-171): circles of B vs poly A via circlePoly, emit
        // with the FLIPPED normal (-nx,-ny) (circlePoly pushes the circle=B out
        // of A; we store the push-A-out-of-B convention), contact point
        // (px - nx*(r-depth), py - ny*(r-depth)), key = keyBase + k.
        //
        // A MUST be Polygon or Aabb; B MUST be round.
        [[nodiscard]] Manifold CollidePolygonRound(const Shape& a, const Transform& xfA,
                                                   const Shape& b, const Transform& xfB,
                                                   Real speculativeMargin = Real(0),
                                                   std::uint32_t keyBase   = 0);

        // Returns true iff the shape is round (Circle or Capsule). PORT of
        // Manifold.lua isRound(s).
        [[nodiscard]] inline bool IsRound(const Shape& s) noexcept
        {
            return s.kind == ShapeKind::Circle || s.kind == ShapeKind::Capsule;
        }

        // NOTE: the top-level router CollideShapes (dispatching all four shape-kind
        // combinations) lives in Dispatch.hpp/.cpp (P1.5). This header provides only
        // the round building blocks; include <Arcane/Physics/Narrowphase/Dispatch.hpp>
        // for the unified entry point.

    } // namespace Physics
} // namespace Arcane
