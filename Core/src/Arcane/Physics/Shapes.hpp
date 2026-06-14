#pragma once

// Collider shapes for the Arcane 2D physics engine (M6, Task P1.1).
//
// PORT NOTE: the four constructors (Circle/Capsule/AABB/Polygon) and
// ComputeAABB port Client/src/physics/shapes.lua faithfully -- shapes.lua is
// the geometry oracle and ComputeAABB MUST bit-match the captured fixture
// Arcane/Tests/data/physics_oracle/shapes.json (within f32 tolerance). The Lua
// engine is a fixedRotation world: capsules are horizontal segments
// (-halfLen,0)-(+halfLen,0), polygons are baked in local space, no rotation.
//
// NEW (no Lua oracle): ComputeMass(density), centroid, polygon edge-normals,
// and CCW winding normalization are standard Box2D-style rigid-body
// computations that the Phase-2 dynamics solver and the P1.2 SAT port need.
// They are validated in the test against hand-derived analytic values, not
// against the Lua source (which has none).
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/Batcher2D/ImGui, no C++23-only features (this module is also
// compiled static-CRT/C++20 in the server flavor). Shapes are immutable
// value geometry -- multiple bodies can share one. Precomputed data (centroid,
// polygon normals) is baked at construction; copyable + cheap to share.

#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>

namespace Arcane
{
    namespace Physics
    {
        // ----------------------------------------------------------------
        // Transform: position + (forward-compat) rotation.
        // ----------------------------------------------------------------
        //
        // The Lua engine is fixedRotation: shapes.aabbOf(s, x, y) takes only a
        // position. We model that as a Vec2 position now. The rotation field is
        // present for forward-compat (later phases / non-fixedRotation worlds)
        // but is identity/unused in P1.1 -- ComputeAABB ignores it so the
        // result bit-matches the Lua aabbOf.
        struct Transform
        {
            Vec2 position{ Real(0), Real(0) };
            // Rotation in radians. Identity (0) in P1.1; reserved for later.
            Real rotation = Real(0);
        };

        // ----------------------------------------------------------------
        // AABB: world-space axis-aligned bounds (min/max corners).
        // ----------------------------------------------------------------
        //
        // Matches the Lua aabbOf return tuple (x0,y0,x1,y1) -> (min, max).
        struct Aabb
        {
            Vec2 min{ Real(0), Real(0) };
            Vec2 max{ Real(0), Real(0) };
        };

        // ----------------------------------------------------------------
        // MassData: result of ComputeMass(density).
        // ----------------------------------------------------------------
        //
        // Box2D-style mass properties. `inertia` is the rotational inertia
        // about the shape's centroid (NOT about the local origin); the solver
        // re-bases it onto the body center when bodies are built in Phase 2.
        struct MassData
        {
            Real mass = Real(0);                  // density * area
            Vec2 centroid{ Real(0), Real(0) };    // local-space area centroid
            Real inertia = Real(0);               // about the centroid
        };

        // ----------------------------------------------------------------
        // ShapeKind: tagged-union discriminator (Lua used a `kind` string).
        // ----------------------------------------------------------------
        enum class ShapeKind : std::uint8_t
        {
            Circle  = 0,
            Capsule = 1,
            Aabb    = 2,
            Polygon = 3,
        };

        // ----------------------------------------------------------------
        // Shape: immutable collider geometry, copyable + shareable.
        // ----------------------------------------------------------------
        //
        // A single struct carries all four kinds (data-oriented; matches the
        // Lua `kind`-tagged table). Use the named factory functions
        // (MakeCircle/MakeCapsule/MakeAabb/MakePolygon) to construct -- they
        // bake precomputed polygon data (CCW verts, outward normals, centroid)
        // so the SAT port in P1.2 reads it directly.
        struct Shape
        {
            ShapeKind kind = ShapeKind::Circle;

            // Circle / Capsule radius.
            Real radius = Real(0);
            // Capsule half-length: segment is (-halfLen,0)-(+halfLen,0).
            Real halfLen = Real(0);
            // AABB half-extents.
            Real halfW = Real(0);
            Real halfH = Real(0);

            // Polygon data (only populated for ShapeKind::Polygon). Stored CCW
            // with one outward unit normal per edge i (edge verts[i]->verts[i+1]).
            // verts.size() == normals.size() == vertex count (3..kMaxPolyVerts).
            std::vector<Vec2> verts;
            std::vector<Vec2> normals;
            // Polygon area centroid (local space). Baked at construction.
            Vec2 polyCentroid{ Real(0), Real(0) };

            // World AABB of this shape under transform xf. PORTS shapes.aabbOf
            // faithfully; in P1.1 xf is translation-only (xf.rotation ignored).
            Aabb ComputeAABB(const Transform& xf) const;

            // Standard Box2D-style mass properties for the given density.
            // NEW computation (no Lua oracle): validated against hand-derived
            // analytic values in the test. See Shapes.cpp for the per-kind
            // derivations.
            MassData ComputeMass(Real density) const;
        };

        // ----------------------------------------------------------------
        // Factory functions (port of shapes.circle/capsule/aabb/polygon).
        // ----------------------------------------------------------------

        // Circle of radius r. PORT: shapes.circle(r).
        Shape MakeCircle(Real r);

        // Horizontal capsule: segment (-halfLen,0)-(+halfLen,0) inflated by r.
        // PORT: shapes.capsule(halfLen, r).
        Shape MakeCapsule(Real halfLen, Real r);

        // Axis-aligned box by half-extents. PORT: shapes.aabb(hw, hh).
        Shape MakeAabb(Real hw, Real hh);

        // Convex polygon from a flat {x0,y0,x1,y1,...} vertex list in local
        // space. PORT: shapes.polygon(verts) -- requires >= 3 (x,y) verts and
        // <= kMaxPolyVerts; throws std::invalid_argument otherwise (the Lua
        // used assert). The input may be CW or CCW: the polygon is normalized
        // to CCW winding and outward unit edge normals + the area centroid are
        // baked. Geometry.polyPoly was winding-agnostic, but the C++ Polygon
        // stores CCW + precomputed normals for the SAT port (P1.2).
        Shape MakePolygon(const std::vector<Vec2>& verts);

    } // namespace Physics
} // namespace Arcane
