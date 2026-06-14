// Manifold.cpp -- contact manifold for convex polygons + AABBs.
//
// See Manifold.hpp for the contract. The margin==0 path is a faithful port of
// Manifold.lua polyVsPoly (SAT normal + contained-vertex points, 2 deepest, in
// the Lua's exact iteration order) + worldPoly. It bit-matches manifold.json.
// The margin>0 speculative path is a strictly-additive MODERNIZATION.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/Narrowphase/Manifold.hpp>

#include <limits>

#include <Arcane/Physics/Narrowphase/GeometryKernel.hpp>
#include <Arcane/Physics/Narrowphase/Sat.hpp>

namespace Arcane
{
    namespace Physics
    {
        namespace
        {
            constexpr Real kHuge = std::numeric_limits<Real>::infinity();
            // Scratch world-vert capacity: AABB = 4, polys up to kMaxPolyVerts.
            constexpr int kMaxWorldVerts = static_cast<int>(kMaxPolyVerts);

            // worldPoly + pointInPoly now live in GeometryKernel.hpp (shared
            // with Specialized.cpp, P1.3) -- this TU calls Physics::WorldPoly /
            // Physics::PointInPoly directly so both manifold paths use ONE impl.

            // The Lua `consider` shuffle: keep the 2 deepest (depth > 0), with
            // c1 = deepest, c2 = second. Order of calls matters for ties (the
            // Lua iterates A's verts first, then B's). Stored as plain fields so
            // the tie/replacement semantics match the Lua exactly.
            struct TwoDeepest
            {
                bool has1 = false, has2 = false;
                Vec2 p1{ Real(0), Real(0) }, p2{ Real(0), Real(0) };
                Real d1 = Real(0), d2 = Real(0);

                void Consider(const Vec2& p, Real depth)
                {
                    if (depth <= Real(0)) return;
                    if (!has1 || depth > d1)
                    {
                        // shift current c1 down into c2 (the Lua does this
                        // unconditionally, even if c2 was empty)
                        p2 = p1; d2 = d1; has2 = has1;
                        p1 = p;  d1 = depth; has1 = true;
                    }
                    else if (!has2 || depth > d2)
                    {
                        p2 = p; d2 = depth; has2 = true;
                    }
                }
            };

            // The faithful (margin==0) contained-vertex manifold. PORT of
            // Manifold.lua polyVsPoly lines 79-114. `nx,ny` is the SAT normal
            // (from B toward A). Fills `m` (normal already set by the caller).
            void ContainedVertexPoints(const Vec2* pa, int na,
                                       const Vec2* pb, int nb,
                                       Real nx, Real ny,
                                       std::uint32_t keyBase,
                                       Manifold& m)
            {
                TwoDeepest cc;

                // support of B along n: maxB = max over B verts of (vB . n).
                Real maxB = -kHuge;
                for (int i = 0; i < nb; ++i)
                {
                    const Real p = pb[i].x * nx + pb[i].y * ny;
                    if (p > maxB) maxB = p;
                }
                // verts of A inside B: depth = maxB - (vA . n).
                for (int i = 0; i < na; ++i)
                {
                    if (PointInPoly(pa[i], pb, nb))
                    {
                        const Real d = maxB - (pa[i].x * nx + pa[i].y * ny);
                        cc.Consider(pa[i], d);
                    }
                }

                // support of A along n: minA = min over A verts of (vA . n).
                // (Manifold.lua:103 `if p < minA then p = p end` is a harmless
                // no-op; line 104 does the real assignment. We port only line
                // 104's correct minA computation.)
                Real minA = kHuge;
                for (int i = 0; i < na; ++i)
                {
                    const Real p = pa[i].x * nx + pa[i].y * ny;
                    if (p < minA) minA = p;
                }
                // verts of B inside A: depth = (vB . n) - minA.
                for (int i = 0; i < nb; ++i)
                {
                    if (PointInPoly(pb[i], pa, na))
                    {
                        const Real d = (pb[i].x * nx + pb[i].y * ny) - minA;
                        cc.Consider(pb[i], d);
                    }
                }

                m.pointCount = 0;
                if (cc.has1)
                {
                    m.points[m.pointCount] =
                        ManifoldPoint{ cc.p1, cc.d1, keyBase + 1 };
                    ++m.pointCount;
                }
                if (cc.has2)
                {
                    m.points[m.pointCount] =
                        ManifoldPoint{ cc.p2, cc.d2, keyBase + 2 };
                    ++m.pointCount;
                }
            }
        } // namespace

        Manifold CollidePolygons(const Shape& a, const Transform& xfA,
                                 const Shape& b, const Transform& xfB,
                                 Real speculativeMargin,
                                 std::uint32_t keyBase)
        {
            Manifold m{};

            // Expand both shapes into world-space verts (AABB -> 4 corners).
            Vec2 wva[kMaxWorldVerts];
            Vec2 wvb[kMaxWorldVerts];
            const int na = WorldPoly(a, xfA, wva);
            const int nb = WorldPoly(b, xfB, wvb);

            // SAT (faithful path, margin 0 unless speculative requested). We run
            // it with margin == 0 first so the overlap result is the verbatim
            // Geometry.polyPoly verdict; the speculative branch is handled
            // separately below to keep parity byte-exact.
            const SatResult sat =
                CollidePolygonsSat(wva, na, wvb, nb, /*margin=*/Real(0));

            if (sat.hit)
            {
                // Real overlap: SAT normal + contained-vertex points (the Lua
                // path). depth (sat.depth) is unused here; per-point depths come
                // from the containment math, exactly as Manifold.lua does.
                m.normal = sat.normal;
                ContainedVertexPoints(wva, na, wvb, nb,
                                      sat.normal.x, sat.normal.y,
                                      keyBase, m);
                return m;
            }

            // --- Speculative path (MODERNIZATION; margin>0 only) ------------
            // Polygons do not overlap. If a skin margin is requested, report a
            // single speculative contact when the closest gap is within the
            // margin. This NEVER runs for margin==0, so the faithful oracle
            // path is untouched.
            if (speculativeMargin > Real(0))
            {
                const SatResult spec =
                    CollidePolygonsSat(wva, na, wvb, nb, speculativeMargin);
                if (spec.hit && spec.depth < Real(0))
                {
                    // spec.depth is the NEGATIVE gap (-separation). The contact
                    // point is the midpoint between the closest features along
                    // the separating normal -- approximated here as the closest
                    // vertex of A to B projected onto the contact plane. A
                    // simple, deterministic choice: the A vertex with the
                    // smallest projection beyond B's support, placed on the
                    // mid-gap plane.
                    m.normal = spec.normal;

                    // Closest A vertex along -normal (deepest toward B).
                    const Real nx = spec.normal.x, ny = spec.normal.y;
                    Real minProj = kHuge;
                    Vec2 best{ Real(0), Real(0) };
                    for (int i = 0; i < na; ++i)
                    {
                        const Real p = wva[i].x * nx + wva[i].y * ny;
                        if (p < minProj) { minProj = p; best = wva[i]; }
                    }
                    // Place the contact on the mid-gap plane: shift the closest
                    // A vertex back by half the gap along the normal.
                    const Real halfGap = Real(-0.5) * spec.depth; // depth<0
                    const Vec2 cp = Vec2(best.x - nx * halfGap,
                                         best.y - ny * halfGap);

                    m.points[0] =
                        ManifoldPoint{ cp, spec.depth, keyBase + 1 };
                    m.pointCount = 1;
                }
                return m;
            }

            return m; // separated, no skin: empty manifold
        }

    } // namespace Physics
} // namespace Arcane
