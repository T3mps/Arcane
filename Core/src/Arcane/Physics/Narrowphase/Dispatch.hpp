#pragma once

// Top-level narrowphase shape-pair dispatch for the Arcane 2D physics engine
// (M6, Task P1.5).
//
// PORT NOTE: the routing table mirrors Manifold.lua genPair's outer dispatch
// (Client/src/physics/Manifold.lua lines 130-176):
//
//   isRound(A) && isRound(B)  -> CollideRoundRound
//   isRound(A) && poly(B)     -> CollideRoundPolygon
//   poly(A)    && isRound(B)  -> CollidePolygonRound  (flipped-normal path)
//   poly(A)    && poly(B)     -> CollidePolygons       (SAT, P1.2)
//
// "Round" = Circle or Capsule; "poly" = Polygon or Aabb (all four of P1.5).
// The four combinations are complete: no shape-pair combination returns an
// empty stub.
//
// CANONICAL HOME: CollideShapes lives HERE (Dispatch.hpp/.cpp). Callers that
// want the unified top-level router should include this header. The round
// building blocks (CollideRoundRound / CollideRoundPolygon / CollidePolygonRound)
// remain in Specialized.hpp for callers that want direct access.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/ImGui, no C++23-only features. namespace Arcane::Physics.

#include <cstdint>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Narrowphase/Manifold.hpp>

namespace Arcane
{
    namespace Physics
    {
        // Dispatch the contact manifold between any two shapes.
        //
        // Routes to the correct primitive collider based on shape kind:
        //   round vs round   -> CollideRoundRound    (Specialized.hpp, P1.3)
        //   round vs poly    -> CollideRoundPolygon  (Specialized.hpp, P1.3)
        //   poly  vs round   -> CollidePolygonRound  (Specialized.hpp, P1.3)
        //   poly  vs poly    -> CollidePolygons      (Manifold.hpp,    P1.2)
        //
        // "round" = Circle | Capsule; "poly" = Aabb | Polygon.
        //
        // speculativeMargin: if > 0, near-touching pairs within the skin are
        // emitted as speculative contacts (separation < 0). When 0 the result
        // is a faithful port of Manifold.lua (real penetration only).
        //
        // keyBase: per-pair warm-start key base (Lua genPair argument). Emitted
        // contact ids are keyBase + 1 (deepest) and keyBase + 2 (second).
        [[nodiscard]] Manifold CollideShapes(const Shape& a, const Transform& xfA,
                                             const Shape& b, const Transform& xfB,
                                             Real speculativeMargin = Real(0),
                                             std::uint32_t keyBase  = 0);

    } // namespace Physics
} // namespace Arcane
