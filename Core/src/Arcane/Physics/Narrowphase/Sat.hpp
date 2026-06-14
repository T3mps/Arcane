#pragma once

// SAT (Separating Axis Theorem) overlap for convex polygons (M6, Task P1.2).
//
// PORT NOTE: a faithful port of Geometry.polyPoly from
// Client/src/physics/Geometry.lua (lines ~157-199). The Lua module is the
// behavioral oracle; the reference outputs captured by
// Client/src/tests/physics_oracle_capture/ pin the result bit-for-bit (within
// f32 tolerance). There is NO SAT.lua -- SAT lives inside Geometry.lua.
//
// Convention (matching the Lua source): y-down screen space, no rotation. The
// returned normal points FROM polygon B toward polygon A -- i.e. the direction
// that pushes A out of B. polyPoly is winding-AGNOSTIC: it recomputes each edge
// normal on the fly (nx,ny = vj.y-vi.y, vi.x-vj.x) and resolves the push
// direction via the projection-center comparison (ca >= cb) plus the per-call
// flip flag, so the result is independent of how the caller wound the polygon.
// We port that on-the-fly computation VERBATIM (rather than reuse the Shape's
// precomputed CCW normals) so the normal selection + orientation reproduce the
// oracle exactly.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/Batcher2D/ImGui, no C++23-only features (this module is also
// compiled static-CRT/C++20 in the server flavor).

#include <glm/vec2.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>

namespace Arcane
{
    namespace Physics
    {
        // ----------------------------------------------------------------
        // SatResult: the Geometry.polyPoly return tuple.
        // ----------------------------------------------------------------
        //
        // hit    : the polygons overlap (false => separated; normal/depth are
        //          left at zero and carry no meaning).
        // normal : unit axis of minimum overlap, oriented from B toward A
        //          (push A out of B), exactly as the Lua resolves it.
        // depth  : the minimum overlap magnitude (always > 0 when hit). When
        //          the polygons are separated this is the closest-axis value
        //          only if `separated` semantics are requested (see below);
        //          otherwise it is left at 0.
        struct SatResult
        {
            bool hit    = false;
            Vec2 normal{ Real(0), Real(0) };
            Real depth  = Real(0);
        };

        // SAT convex-poly vs convex-poly over WORLD-space vertices. Direct port
        // of Geometry.polyPoly(va, vb): tests both polygons' edge-normal axes,
        // picks the minimum-overlap axis, and orients the normal from B toward A
        // (push A out of B). `va`/`vb` are arrays of `na`/`nb` world vertices in
        // any winding (the algorithm is winding-agnostic). Returns hit=false the
        // moment any axis separates the polygons (matching the Lua early-out).
        //
        // With `margin == 0` (the default) this bit-matches the Lua oracle:
        // hit is true iff the polygons strictly overlap on every axis.
        //
        // MODERNIZATION (kSkin speculative margin): with `margin > 0` the result
        // also reports a near-touching pair whose minimum separation gap is
        // within (0, margin]. In that case hit=true, depth is NEGATIVE (the gap,
        // i.e. -separation), and normal still points from B toward A. The
        // faithful overlap path is unchanged when margin == 0, so parity is
        // never compromised. (Manifold.cpp converts this negative depth into a
        // speculative ManifoldPoint with separation in [-margin, 0).)
        [[nodiscard]] SatResult CollidePolygonsSat(const Vec2* va, int na,
                                                   const Vec2* vb, int nb,
                                                   Real margin = Real(0));

    } // namespace Physics
} // namespace Arcane
