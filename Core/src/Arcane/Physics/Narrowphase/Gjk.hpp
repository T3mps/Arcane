#pragma once

// 2D GJK distance + closest points, surface distance, and a conservative-
// advancement shape cast for the Arcane 2D physics narrowphase (M6, Task P1.4).
//
// PORT NOTE: a faithful port of Client/src/physics/GJK.lua (the distance core,
// shapeDistance, shapePolyDistance) plus the conservative-advancement `advance`
// primitive from Client/src/physics/Cast.lua. The Lua module is the behavioral
// oracle; GjkDistance / ShapeDistance bit-match the captured fixture
// Arcane/Tests/data/physics_oracle/gjk.json (within f32 tolerance). The
// standing NO-EPA decision holds: GJK serves DISTANCE (and the CCD/cast
// primitive); penetration manifolds stay SAT/specials (Sat.hpp / Specialized).
//
// CORES: GJK operates on convex CORES with the radius subtracted afterward
// (a circle is a 1-vert point of radius r; a capsule is a 2-vert segment of
// radius r; an AABB is its 4 corners with radius 0; a polygon is its world
// verts with radius 0). BuildCore (Gjk.cpp) ports GJK.lua:buildCore. The
// AABB-core radius (0) differs from nothing here, but note the core layout
// matches GeometryKernel::WorldPoly's corner order so the two stay consistent.
//
// CONVENTION (matching the Lua source): y-down screen space, no rotation.
// ShapeDistance's normal points FROM the second shape (B) TOWARD the first (A)
// -- i.e. the direction that pushes A out -- matching the Geometry-kernel and
// SAT convention used across the narrowphase. When the cores intersect the
// distance is 0 and the witnesses/normal are meaningless (returned as zeros),
// exactly as GJK.lua does.
//
// SHAPECAST (Cast.lua:advance, lines 20-29): a DECOUPLED conservative-advance
// primitive, NOT coupled to the world. Translation-only: advancing by
// dist/|translation| can never overshoot a convex obstacle. TOL = 0.05,
// MAX_ITER = 32 (ported verbatim). The world-level cast that collects tile /
// body candidates (Cast.shapeCast / rayVsBody) is P1.9 and will reuse this
// primitive; here we expose only the per-obstacle TOI.
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
        // GjkResult: GJK.distance return tuple.
        // ----------------------------------------------------------------
        //
        // distance : Euclidean distance between the two convex cores. 0 means
        //            the cores intersect -- the witnesses are then meaningless
        //            and returned as zeros (matching GJK.lua).
        // witnessA : closest point on core A.
        // witnessB : closest point on core B.
        struct GjkResult
        {
            Real distance = Real(0);
            Vec2 witnessA{ Real(0), Real(0) };
            Vec2 witnessB{ Real(0), Real(0) };
        };

        // ----------------------------------------------------------------
        // ShapeDistanceResult: GJK.shapeDistance / shapePolyDistance tuple.
        // ----------------------------------------------------------------
        //
        // distance : SURFACE distance (core distance minus both radii).
        //            Negative => the shapes overlap within their radii.
        // witnessA : surface witness on A (core witness pulled in by A's radius
        //            along the normal).
        // witnessB : surface witness on B (core witness pushed out by B's
        //            radius along the normal).
        // normal   : unit normal from B toward A (push A out). Zeros when the
        //            cores intersect (distance computed as 0 by GJK).
        struct ShapeDistanceResult
        {
            Real distance = Real(0);
            Vec2 witnessA{ Real(0), Real(0) };
            Vec2 witnessB{ Real(0), Real(0) };
            Vec2 normal{ Real(0), Real(0) };
        };

        // ----------------------------------------------------------------
        // ShapeCastResult: conservative-advancement shape-cast tuple.
        // ----------------------------------------------------------------
        //
        // hit      : the swept shape reached within TOL of the obstacle before
        //            the end of its travel (t in [0,1]).
        // t        : parametric time of impact along the cast translation. The
        //            shape CENTER at TOI is start + translation * t.
        // normal   : surface normal at the impact (from the obstacle toward the
        //            moving shape -- the push-back direction).
        // distance : surface distance at the stopping point (< TOL when hit).
        struct ShapeCastResult
        {
            bool hit = false;
            Real t = Real(0);
            Vec2 normal{ Real(0), Real(0) };
            Real distance = Real(0);
        };

        // ----------------------------------------------------------------
        // Conservative-advancement constants (Cast.lua:11-12, verbatim).
        // ----------------------------------------------------------------
        inline constexpr Real kShapeCastTol = Real(0.05);
        inline constexpr int  kShapeCastMaxIter = 32;

        // ----------------------------------------------------------------
        // GjkDistance: distance + closest points between two convex CORES.
        // ----------------------------------------------------------------
        //
        // PORT of GJK.distance(va, vb): va/vb are world-space vertex spans
        // (point=1, segment=2, polygon=n) -- the raw cores, radii NOT applied.
        // The simplex loop ports the Lua exactly (initial support along (1,0),
        // up to 64 iterations, the n==1/2/3 closest-point-to-origin reduction,
        // barycentric witness interpolation, the (s.d)-(p.d) < 1e-9 convergence
        // test, and the origin-reached early-out). The arithmetic is carried in
        // f64 internally to track the Lua's f64 distance core (the f32 core
        // inputs are widened on read); results are returned as f32. distance==0
        // means the cores intersect (witnesses returned as zeros).
        [[nodiscard]] GjkResult GjkDistance(const Vec2* va, int na,
                                            const Vec2* vb, int nb);

        // ----------------------------------------------------------------
        // BuildCore: PORT of GJK.lua:buildCore. Writes the convex core verts of
        // `s` at world transform `xf` (translation-only) into `out` and returns
        // the core RADIUS (circle/capsule -> s.radius; aabb/polygon -> 0).
        // Returns the vertex count via `outCount`. `out` must have room for at
        // least kMaxPolyVerts vertices.
        // ----------------------------------------------------------------
        [[nodiscard]] Real BuildCore(const Shape& s, const Transform& xf,
                                     Vec2* out, int& outCount);

        // ----------------------------------------------------------------
        // ShapeDistance: PORT of GJK.shapeDistance. Surface distance between two
        // shapes at world transforms. Subtracts both radii; the normal points
        // from B toward A. Negative distance => overlapping within radii.
        // ----------------------------------------------------------------
        [[nodiscard]] ShapeDistanceResult ShapeDistance(const Shape& a,
                                                        const Transform& xfA,
                                                        const Shape& b,
                                                        const Transform& xfB);

        // ----------------------------------------------------------------
        // ShapePolyDistance: PORT of GJK.shapePolyDistance. Surface distance
        // between shape `a` and a RAW world-space polygon span (e.g. a tile
        // span); the poly's radius is 0. Same return shape as ShapeDistance.
        // ----------------------------------------------------------------
        [[nodiscard]] ShapeDistanceResult ShapePolyDistance(const Shape& a,
                                                            const Transform& xfA,
                                                            const Vec2* poly,
                                                            int n);

        // ----------------------------------------------------------------
        // ShapeCast: conservative-advancement TOI of a moving shape against a
        // single static obstacle shape. PORT of Cast.lua:advance using
        // ShapeDistance as the per-step distance function. `start` is the
        // moving shape's transform at t=0; `translation` is its displacement
        // over the cast. Returns the center-at-TOI parametric t with the
        // surface normal/distance at the stopping point. No hit (zero/near-zero
        // translation, or the obstacle is never reached within travel) -> hit
        // == false. Deterministic: fixed iteration count, no wall-clock.
        // ----------------------------------------------------------------
        [[nodiscard]] ShapeCastResult ShapeCast(const Shape& moving,
                                                const Transform& start,
                                                const Vec2& translation,
                                                const Shape& obstacle,
                                                const Transform& obstacleXf);

        // ----------------------------------------------------------------
        // ShapeCastPoly: ShapeCast against a RAW world-space polygon span
        // (ShapePolyDistance as the distance function). Same semantics.
        // ----------------------------------------------------------------
        [[nodiscard]] ShapeCastResult ShapeCastPoly(const Shape& moving,
                                                    const Transform& start,
                                                    const Vec2& translation,
                                                    const Vec2* poly, int n);

    } // namespace Physics
} // namespace Arcane
