// Sat.cpp -- SAT convex-poly overlap (port of Geometry.polyPoly).
//
// See Sat.hpp for the contract. The margin==0 path is a verbatim port of
// Geometry.polyPoly and bit-matches the captured oracle. The margin>0 path is
// a strictly-additive MODERNIZATION (speculative skin) that only runs when the
// faithful path reports a separation; it never alters the overlap result.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/Narrowphase/Sat.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Arcane
{
    namespace Physics
    {
        namespace
        {
            // huge sentinel matching Lua math.huge in the SAT accumulators.
            constexpr Real kHuge = std::numeric_limits<Real>::infinity();

            // One side of the SAT axis loop. PORT of Geometry.polyPoly's local
            // `axes(verts, other, flip)`:
            //   - iterate `verts`' edges; edge i->j normal is
            //       nx,ny = verts[j].y - verts[i].y, verts[i].x - verts[j].x
            //     (the Lua spells it on the flat array as
            //       verts[j+1]-verts[i+1], verts[i]-verts[j]).
            //   - normalize (skip degenerate edges, len <= 1e-12).
            //   - project BOTH polygons onto the axis; overlap is
            //       min(maxA,maxB) - max(minA,minB).
            //   - overlap <= 0 => separated: return false immediately.
            //   - track the minimum overlap; orient via ca>=cb then flip.
            //
            // Returns false the moment a separating axis is found (overlap<=0),
            // exactly like the Lua early-out. Otherwise returns true and updates
            // bestDepth / bnormal in place.
            bool SatAxes(const Vec2* verts, int n,
                         const Vec2* other, int m,
                         bool flip,
                         Real& bestDepth, Vec2& bnormal)
            {
                for (int i = 0; i < n; ++i)
                {
                    const int j = (i + 1 < n) ? (i + 1) : 0;
                    // Edge normal (winding-agnostic, on the fly) -- matches the
                    // Lua exactly: (vj.y - vi.y, vi.x - vj.x).
                    Real nx = verts[j].y - verts[i].y;
                    Real ny = verts[i].x - verts[j].x;
                    const Real len = std::sqrt(nx * nx + ny * ny);
                    if (len > Real(1e-12))
                    {
                        nx /= len;
                        ny /= len;

                        Real minA = kHuge, maxA = -kHuge;
                        Real minB = kHuge, maxB = -kHuge;
                        for (int k = 0; k < n; ++k)
                        {
                            const Real p = verts[k].x * nx + verts[k].y * ny;
                            if (p < minA) minA = p;
                            if (p > maxA) maxA = p;
                        }
                        for (int k = 0; k < m; ++k)
                        {
                            const Real p = other[k].x * nx + other[k].y * ny;
                            if (p < minB) minB = p;
                            if (p > maxB) maxB = p;
                        }

                        const Real overlap =
                            std::min(maxA, maxB) - std::max(minA, minB);
                        if (overlap <= Real(0))
                        {
                            return false; // separating axis
                        }
                        if (overlap < bestDepth)
                        {
                            bestDepth = overlap;
                            // Orient from the axis-owner's counterpart toward
                            // the owner (Lua: ca>=cb keeps n, else negate; then
                            // flip inverts for the second call so the result
                            // always points from B toward A == push A out of B).
                            const Real ca = (minA + maxA) * Real(0.5);
                            const Real cb = (minB + maxB) * Real(0.5);
                            Real ox, oy;
                            if (ca >= cb) { ox = nx;  oy = ny; }
                            else          { ox = -nx; oy = -ny; }
                            if (flip)     { ox = -ox; oy = -oy; }
                            bnormal = Vec2(ox, oy);
                        }
                    }
                }
                return true;
            }

            // Speculative separation probe (margin>0 path only). When the
            // polygons do NOT overlap, the SEPARATING axis is the one with the
            // LARGEST signed gap (most positive `gap = max(minA,minB) -
            // min(maxA,maxB)`). We scan both polygons' edge axes (same axis SET
            // the Lua uses) and keep the max-gap axis, orienting the normal with
            // the same ca>=cb (+flip) rule so it points from B toward A.
            //
            // This NEVER runs for the faithful (margin==0) path and so cannot
            // affect oracle parity. It returns true iff the polygons are
            // separated AND the gap is within (0, margin]; depth is set to the
            // NEGATIVE gap (so callers see separation as a negative penetration).
            bool SatSeparation(const Vec2* va, int na,
                               const Vec2* vb, int nb,
                               Real margin,
                               Vec2& outNormal, Real& outDepth)
            {
                Real bestGap = -kHuge; // we want the maximum (largest) gap
                Vec2 bestNormal{ Real(0), Real(0) };

                auto scan = [&](const Vec2* verts, int n,
                                const Vec2* other, int m, bool flip)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        const int j = (i + 1 < n) ? (i + 1) : 0;
                        Real nx = verts[j].y - verts[i].y;
                        Real ny = verts[i].x - verts[j].x;
                        const Real len = std::sqrt(nx * nx + ny * ny);
                        if (len <= Real(1e-12)) continue;
                        nx /= len;
                        ny /= len;

                        Real minA = kHuge, maxA = -kHuge;
                        Real minB = kHuge, maxB = -kHuge;
                        for (int k = 0; k < n; ++k)
                        {
                            const Real p = verts[k].x * nx + verts[k].y * ny;
                            if (p < minA) minA = p;
                            if (p > maxA) maxA = p;
                        }
                        for (int k = 0; k < m; ++k)
                        {
                            const Real p = other[k].x * nx + other[k].y * ny;
                            if (p < minB) minB = p;
                            if (p > maxB) maxB = p;
                        }

                        // gap > 0 means this axis separates the polygons.
                        const Real gap =
                            std::max(minA, minB) - std::min(maxA, maxB);
                        if (gap > bestGap)
                        {
                            bestGap = gap;
                            const Real ca = (minA + maxA) * Real(0.5);
                            const Real cb = (minB + maxB) * Real(0.5);
                            Real ox, oy;
                            if (ca >= cb) { ox = nx;  oy = ny; }
                            else          { ox = -nx; oy = -ny; }
                            if (flip)     { ox = -ox; oy = -oy; }
                            bestNormal = Vec2(ox, oy);
                        }
                    }
                };

                scan(va, na, vb, nb, false);
                scan(vb, nb, va, na, true);

                if (bestGap > Real(0) && bestGap <= margin)
                {
                    outNormal = bestNormal;
                    outDepth  = -bestGap; // negative depth == separation gap
                    return true;
                }
                return false;
            }
        } // namespace

        SatResult CollidePolygonsSat(const Vec2* va, int na,
                                     const Vec2* vb, int nb,
                                     Real margin)
        {
            SatResult r{};

            // --- Faithful path (verbatim Geometry.polyPoly) -----------------
            Real bestDepth = kHuge;
            Vec2 bnormal{ Real(0), Real(0) };
            const bool passA = SatAxes(va, na, vb, nb, /*flip=*/false,
                                      bestDepth, bnormal);
            if (passA)
            {
                const bool passB = SatAxes(vb, nb, va, na, /*flip=*/true,
                                           bestDepth, bnormal);
                if (passB)
                {
                    // Overlap on every axis: real penetration.
                    r.hit    = true;
                    r.normal = bnormal;
                    r.depth  = bestDepth;
                    return r;
                }
            }

            // --- Speculative path (MODERNIZATION; margin>0 only) ------------
            if (margin > Real(0))
            {
                Vec2 sepNormal{ Real(0), Real(0) };
                Real sepDepth = Real(0);
                if (SatSeparation(va, na, vb, nb, margin, sepNormal, sepDepth))
                {
                    r.hit    = true;     // a speculative contact exists
                    r.normal = sepNormal;
                    r.depth  = sepDepth; // negative (gap within (-margin, 0))
                    return r;
                }
            }

            // Separated (and outside margin, or margin==0): no contact.
            return r;
        }

    } // namespace Physics
} // namespace Arcane
