// Dispatch.cpp -- top-level narrowphase shape-pair router (M6, Task P1.5).
//
// See Dispatch.hpp for the routing contract. The dispatch table mirrors
// Manifold.lua genPair (Client/src/physics/Manifold.lua:130-176):
//
//   isRound(A) && isRound(B)  -> CollideRoundRound
//   isRound(A) && poly(B)     -> CollideRoundPolygon
//   poly(A)    && isRound(B)  -> CollidePolygonRound  (flipped normal)
//   poly(A)    && poly(B)     -> CollidePolygons      (SAT, P1.2)
//
// All four combinations are now wired: the poly-poly stub from P1.3 is
// replaced with the real CollidePolygons call (P1.2).
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/Narrowphase/Dispatch.hpp>

#include <Arcane/Physics/Narrowphase/Specialized.hpp>
#include <Arcane/Physics/Narrowphase/Manifold.hpp>

namespace Arcane
{
    namespace Physics
    {
        Manifold CollideShapes(const Shape& a, const Transform& xfA,
                               const Shape& b, const Transform& xfB,
                               Real speculativeMargin,
                               std::uint32_t keyBase)
        {
            const bool ra = IsRound(a);
            const bool rb = IsRound(b);

            if (ra && rb)
            {
                // circle/capsule vs circle/capsule (Manifold.lua:133-150).
                return CollideRoundRound(a, xfA, b, xfB, speculativeMargin, keyBase);
            }
            if (ra) // A round, B poly/aabb (Manifold.lua:131-132).
            {
                return CollideRoundPolygon(a, xfA, b, xfB, speculativeMargin, keyBase);
            }
            if (rb) // A poly/aabb, B round (Manifold.lua:159-171).
            {
                return CollidePolygonRound(a, xfA, b, xfB, speculativeMargin, keyBase);
            }
            // Neither round: poly/aabb vs poly/aabb (Manifold.lua:173-174). P1.5
            // wires the real SAT collider instead of returning an empty stub.
            return CollidePolygons(a, xfA, b, xfB, speculativeMargin, keyBase);
        }

    } // namespace Physics
} // namespace Arcane
