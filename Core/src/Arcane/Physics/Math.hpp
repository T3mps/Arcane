#pragma once

// Pure 2D math kernels for the Arcane physics engine (M6).
//
// PORT NOTE: ported from Client/src/physics/Geometry.lua (the coordinate-
// agnostic kernel). The Lua module is the behavioral oracle; the reference
// outputs captured by Client/src/tests/physics_oracle_capture/ pin these
// formulas bit-for-bit (within f32 tolerance). Coordinate convention
// (matching the Lua source): y-down screen space, no rotation.
//
// PRESENTATION-FREE + C++20-clean: glm + std only. Header-only; functions are
// constexpr/inline where the math allows. Lives in namespace Arcane::Physics.

#include <cmath>

#include <glm/vec2.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>

namespace Arcane
{
    namespace Physics
    {
        namespace Math
        {
            // ------------------------------------------------------------
            // Scalar helpers
            // ------------------------------------------------------------

            // Clamp x into [lo, hi]. constexpr; branch form matches the Lua
            // "if t < 0 then 0 elseif t > 1 then 1" idiom used throughout
            // Geometry.lua (no NaN propagation guarantees, same as Lua).
            template <typename T>
            constexpr T Clamp(T x, T lo, T hi) noexcept
            {
                if (x < lo) return lo;
                if (x > hi) return hi;
                return x;
            }

            // ------------------------------------------------------------
            // Vector helpers
            // ------------------------------------------------------------

            // 2D cross product (scalar). cross2(a,b) = a.x*b.y - a.y*b.x.
            // The signed area of the parallelogram spanned by a and b; the
            // sign tells winding / which side of a a point lies.
            constexpr Real Cross2(const Vec2& a, const Vec2& b) noexcept
            {
                return a.x * b.y - a.y * b.x;
            }

            // Left-hand perpendicular of v: rotate +90 degrees (y-down). This
            // is the edge-normal form used by Geometry.lua's SAT axes
            // (nx,ny = (by-ay), (ax-bx) for edge a->b == perp of the edge).
            constexpr Vec2 Perp(const Vec2& v) noexcept
            {
                return Vec2(v.y, -v.x);
            }

            // ------------------------------------------------------------
            // Closest point on a segment
            // ------------------------------------------------------------

            // Result of ClosestOnSegment: the closest point plus the
            // parameter t in [0,1] along the segment a->b.
            struct ClosestPoint
            {
                Vec2 point{ Real(0), Real(0) };
                Real t = Real(0);
            };

            // Closest point on segment a->b to point p, with the clamped
            // parameter t. Direct port of Geometry.closestOnSegment:
            //   local abx, aby = bx - ax, by - ay
            //   local d = abx*abx + aby*aby
            //   local t = d > 0 and ((px-ax)*abx + (py-ay)*aby)/d or 0
            //   if t < 0 then t = 0 elseif t > 1 then t = 1 end
            //   return ax + abx*t, ay + aby*t, t
            // A degenerate (zero-length) segment yields t = 0 -> point a,
            // exactly as the Lua "or 0" branch does.
            inline ClosestPoint ClosestOnSegment(const Vec2& p,
                                                 const Vec2& a,
                                                 const Vec2& b) noexcept
            {
                const Vec2 ab = b - a;
                const Real d  = ab.x * ab.x + ab.y * ab.y;
                Real t = (d > Real(0))
                             ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / d
                             : Real(0);
                t = Clamp(t, Real(0), Real(1));
                return ClosestPoint{ Vec2(a.x + ab.x * t, a.y + ab.y * t), t };
            }

        } // namespace Math
    }     // namespace Physics
} // namespace Arcane
