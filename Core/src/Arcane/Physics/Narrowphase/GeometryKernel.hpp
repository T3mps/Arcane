#pragma once

// Shared pure geometry kernels for the Arcane 2D physics narrowphase (M6).
//
// PORT NOTE: a faithful port of the pure, coordinate-agnostic kernels in
// Client/src/physics/Geometry.lua. The Lua module is the behavioral oracle;
// the reference outputs captured by Client/src/tests/physics_oracle_capture/
// (geometry.json) pin these formulas bit-for-bit (within f32 tolerance).
//
// Convention (matching the Lua source): y-down screen space, no rotation.
// Normals point FROM the second/static shape TOWARD the first/queried shape --
// i.e. the direction that pushes the queried shape OUT. Polygons are flat
// world-vertex arrays (Vec2*, count), convex, ANY winding (outward edge
// normals are resolved against the centroid, so the result is winding-agnostic
// -- it does NOT depend on whether MakePolygon reordered the verts to CCW).
//
// DRY: these kernels are the single source of truth shared by BOTH the
// poly-path manifold (Manifold.cpp, P1.2 -- its file-local PointInPoly now
// delegates here) and the round-path manifold (Specialized.cpp, P1.3). The
// SAT poly-poly axis test lives separately in Sat.hpp (Geometry.polyPoly).
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/Batcher2D/ImGui, no C++23-only features (this module is also
// compiled static-CRT/C++20 in the server flavor). namespace Arcane::Physics.

#include <glm/vec2.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>

namespace Arcane
{
    namespace Physics
    {
        // ----------------------------------------------------------------
        // Hit: the (hit, normal, depth) tuple the Lua kernels return.
        // ----------------------------------------------------------------
        //
        // hit    : the shapes overlap (false => clear; normal/depth carry no
        //          meaning and are left at zero).
        // normal : unit push-out normal. For circle*/capsule* kernels it points
        //          FROM the polygon/other shape TOWARD the queried circle
        //          (push the circle out), matching the Lua convention.
        // depth  : penetration depth (positive when hit), the Lua's `depth`.
        struct Hit
        {
            bool hit   = false;
            Vec2 normal{ Real(0), Real(0) };
            Real depth = Real(0);
        };

        // ----------------------------------------------------------------
        // Boundary: closestOnPolyBoundary result.
        // ----------------------------------------------------------------
        //
        // point  : closest point on the polygon BOUNDARY to the query point.
        // dist   : distance from the query point to `point` (>= 0).
        // normal : the supporting edge's OUTWARD unit normal (resolved against
        //          the polygon centroid so it always points away from the
        //          interior, independent of winding).
        struct Boundary
        {
            Vec2 point{ Real(0), Real(0) };
            Real dist = Real(0);
            Vec2 normal{ Real(0), Real(0) };
        };

        // ----------------------------------------------------------------
        // ClosestSegSeg result: the closest pair of points (p on seg P,
        // q on seg Q). PORT of Geometry.closestSegSeg.
        // ----------------------------------------------------------------
        struct SegSeg
        {
            Vec2 p{ Real(0), Real(0) };
            Vec2 q{ Real(0), Real(0) };
        };

        // PORT of Manifold.lua worldPoly: expand a poly/aabb shape into
        // world-space verts under the (translation-only) transform xf. The Lua
        // polygon branch rotates by `ang`, but this phase is fixedRotation
        // (ang == 0 => cos=1, sin=0), so we translate only -- byte-identical to
        // the oracle capture, which passes angle = 0. The AABB branch emits the
        // four corners in the Lua's order:
        //   (x-hw,y-hh),(x+hw,y-hh),(x+hw,y+hh),(x-hw,y+hh).
        // `out` must have room for at least kMaxPolyVerts vertices. Returns the
        // vertex count written. The shape MUST be a Polygon or Aabb (callers
        // route round shapes elsewhere).
        [[nodiscard]] int WorldPoly(const Shape& s, const Transform& xf,
                                    Vec2* out);

        // PORT of Geometry.pointInPoly(px, py, verts): winding-agnostic
        // convex containment via consistent edge-cross signs. A point exactly
        // on an edge (cross == 0) does NOT flip the verdict (matches the Lua).
        [[nodiscard]] bool PointInPoly(const Vec2& p, const Vec2* verts, int n);

        // PORT of Geometry.polyCentroid(verts): the arithmetic mean of the
        // vertices (NOT the area centroid). closestOnPolyBoundary uses this to
        // orient the outward edge normal.
        [[nodiscard]] Vec2 PolyCentroid(const Vec2* verts, int n);

        // PORT of Geometry.closestSegSeg(p1,p2, q1,q2): the closest pair of
        // points between segments P(p1->p2) and Q(q1->q2). Exact for
        // non-degenerate segments (endpoint candidates + the proper-crossing
        // 2x2 solve). Returns the intersection point in both fields when the
        // segments properly cross.
        [[nodiscard]] SegSeg ClosestSegSeg(const Vec2& p1, const Vec2& p2,
                                           const Vec2& q1, const Vec2& q2);

        // PORT of Geometry.closestOnPolyBoundary(px,py,verts): the closest
        // point on the polygon boundary to p, the distance, and the supporting
        // edge's outward normal (resolved against the centroid).
        [[nodiscard]] Boundary ClosestOnPolyBoundary(const Vec2& p,
                                                     const Vec2* verts, int n);

        // PORT of Geometry.circleCircle(ax,ay,ar, bx,by,br): circle A queried
        // against circle B. Normal points from B toward A (push A out).
        // Degenerate (coincident centers) -> normal (1,0), the Lua fallback.
        [[nodiscard]] Hit CircleCircle(const Vec2& a, Real ar,
                                       const Vec2& b, Real br);

        // PORT of Geometry.circlePoly(cx,cy,r,verts): circle vs convex poly.
        // Normal pushes the circle out. Contained center -> push out along the
        // nearest edge's outward normal (depth = r + dist). Outside -> push
        // along the boundary-to-center direction (depth = r - dist). Center
        // exactly on the boundary (dist ~ 0) -> borrow the edge normal.
        [[nodiscard]] Hit CirclePoly(const Vec2& c, Real r,
                                     const Vec2* verts, int n);

        // PORT of Geometry.capsulePoly(ax,ay,bx,by,r,verts): capsule (segment
        // a-b inflated by r) vs convex poly. Endpoint-containment falls back to
        // CirclePoly at the contained endpoint; otherwise the closest pair
        // between the segment and each boundary edge (closestSegSeg) gives the
        // push-out. Normal pushes the capsule out.
        [[nodiscard]] Hit CapsulePoly(const Vec2& a, const Vec2& b, Real r,
                                      const Vec2* verts, int n);

    } // namespace Physics
} // namespace Arcane
