// Specialized.cpp -- round-shape contact manifolds (port of Manifold.lua's
// round paths + Geometry.lua round kernels).
//
// See Specialized.hpp for the contract. The margin==0 path is a faithful port
// of Manifold.lua genPair's round branches (roundView + roundVsPoly +
// round-round + poly-vs-round) and bit-matches round_manifold.json. The
// margin>0 speculative path is a strictly-additive MODERNIZATION that NEVER
// runs for margin==0, so parity is never compromised.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/Narrowphase/Specialized.hpp>

#include <cmath>

#include <Arcane/Physics/Narrowphase/GeometryKernel.hpp>

namespace Arcane
{
    namespace Physics
    {
        namespace
        {
            // Scratch world-vert capacity: AABB = 4, polys up to kMaxPolyVerts.
            constexpr int kMaxWorldVerts = static_cast<int>(kMaxPolyVerts);

            // roundView (Manifold.lua:42-48): the endpoint circles of a round
            // shape. count is 1 (circle) or 2 (capsule); c0/c1 are the circle
            // centers (c1 unused when count==1); r is the shared radius.
            struct RoundView
            {
                int  count = 1;
                Vec2 c0{ Real(0), Real(0) };
                Vec2 c1{ Real(0), Real(0) };
                Real r = Real(0);
            };

            RoundView MakeRoundView(const Shape& s, const Transform& xf)
            {
                RoundView v;
                const Real x = xf.position.x;
                const Real y = xf.position.y;
                if (s.kind == ShapeKind::Circle)
                {
                    v.count = 1;
                    v.c0 = Vec2(x, y);
                    v.c1 = Vec2(Real(0), Real(0));
                    v.r = s.radius;
                    return v;
                }
                // capsule: rotate the segment (rotation identity this phase, so
                // cos=1, sin=0 -> horizontal segment endpoints +/- halfLen).
                const Real ang = xf.rotation;
                const Real c = std::cos(ang);
                const Real sn = std::sin(ang);
                const Real hx = s.halfLen * c;
                const Real hy = s.halfLen * sn;
                v.count = 2;
                v.c0 = Vec2(x - hx, y - hy);
                v.c1 = Vec2(x + hx, y + hy);
                v.r = s.radius;
                return v;
            }

            // Push a contact onto the manifold (max 2 points; the Lua emit()).
            // Each point carries its OWN normal (round endpoints touching
            // different poly faces have different normals -- the Lua stores
            // nx,ny per row). Manifold::normal is the REPRESENTATIVE: the
            // DEEPEST point's normal, with a deterministic tiebreak on equal
            // separation (the FIRST emitted / lowest-key point keeps it). We
            // track that here as we emit so the result is order-independent of
            // any later compute pass.
            //
            // The tie comparison uses a small relative tolerance so an ANALYTIC
            // tie (e.g. a symmetric capsule grazing both mirror faces of a
            // diamond, where both endpoints share the same depth) resolves to
            // the first-emitted point deterministically rather than being
            // flipped by sub-ULP f32 noise. A genuinely deeper later point
            // (beyond the tolerance) still wins.
            void Emit(Manifold& m, const Vec2& point, const Vec2& normal,
                      Real depth, std::uint32_t key)
            {
                if (m.pointCount >= 2) return; // capacity guard (round = <=2)
                // First point seeds the representative; a meaningfully-deeper
                // later point replaces it (an equal-depth tie keeps the earlier
                // point, which has the lower key).
                if (m.pointCount == 0)
                {
                    m.normal = normal;
                }
                else
                {
                    const Real incumbent = m.points[0].separation;
                    const Real tol =
                        Real(1e-5) * (Real(1) + std::fabs(incumbent));
                    if (depth > incumbent + tol) m.normal = normal;
                }
                m.points[m.pointCount] =
                    ManifoldPoint{ point, depth, normal, key };
                ++m.pointCount;
            }
        } // namespace

        // --------------------------------------------------------------------
        // round A vs round B (Manifold.lua:135-150).
        // --------------------------------------------------------------------
        Manifold CollideRoundRound(const Shape& a, const Transform& xfA,
                                   const Shape& b, const Transform& xfB,
                                   Real speculativeMargin,
                                   std::uint32_t keyBase)
        {
            Manifold m{};

            const RoundView va = MakeRoundView(a, xfA);
            const RoundView vb = MakeRoundView(b, xfB);

            // Faithful path: pairwise endpoint circles, keep the deepest.
            bool has = false;
            Real bestD = Real(0);
            Vec2 bn{ Real(0), Real(0) };
            Vec2 bp{ Real(0), Real(0) };
            for (int i = 0; i < va.count; ++i)
            {
                const Vec2 pa = (i == 0) ? va.c0 : va.c1;
                for (int j = 0; j < vb.count; ++j)
                {
                    const Vec2 pb = (j == 0) ? vb.c0 : vb.c1;
                    const Hit hit = CircleCircle(pa, va.r, pb, vb.r);
                    if (hit.hit && (!has || hit.depth > bestD))
                    {
                        has = true;
                        bestD = hit.depth;
                        bn = hit.normal;
                        // contact point: (pxb + nx*(rb - depth/2), ...)
                        bp = Vec2(pb.x + hit.normal.x * (vb.r - hit.depth * Real(0.5)),
                                  pb.y + hit.normal.y * (vb.r - hit.depth * Real(0.5)));
                    }
                }
            }
            if (has)
            {
                Emit(m, bp, bn, bestD, keyBase + 1);
                return m;
            }

            // --- Speculative path (MODERNIZATION; margin>0 only) ------------
            // Inflate each radius by half the margin and re-test; if the
            // inflated pair touches (but the faithful did not), emit a single
            // speculative contact whose separation is the negative gap. This
            // NEVER runs for margin==0.
            if (speculativeMargin > Real(0))
            {
                bool shas = false;
                Real bestSep = Real(0); // most-positive (smallest gap) wins
                Vec2 sn{ Real(0), Real(0) };
                Vec2 sp{ Real(0), Real(0) };
                for (int i = 0; i < va.count; ++i)
                {
                    const Vec2 pa = (i == 0) ? va.c0 : va.c1;
                    for (int j = 0; j < vb.count; ++j)
                    {
                        const Vec2 pb = (j == 0) ? vb.c0 : vb.c1;
                        const Real dx = pa.x - pb.x, dy = pa.y - pb.y;
                        const Real d = std::sqrt(dx * dx + dy * dy);
                        const Real gap = d - (va.r + vb.r); // >0 == separated
                        if (gap > Real(0) && gap <= speculativeMargin)
                        {
                            const Real sep = -gap;
                            if (!shas || sep > bestSep)
                            {
                                shas = true;
                                bestSep = sep;
                                const Real inv = (d > Real(1e-9)) ? (Real(1) / d) : Real(0);
                                sn = (d > Real(1e-9)) ? Vec2(dx * inv, dy * inv)
                                                      : Vec2(Real(1), Real(0));
                                // place on the mid-gap plane off pb along +n
                                sp = Vec2(pb.x + sn.x * (vb.r + gap * Real(0.5)),
                                          pb.y + sn.y * (vb.r + gap * Real(0.5)));
                            }
                        }
                    }
                }
                if (shas) Emit(m, sp, sn, bestSep, keyBase + 1);
            }
            return m;
        }

        // --------------------------------------------------------------------
        // round A vs poly/aabb B (Manifold.lua:60-71, roundVsPoly).
        // --------------------------------------------------------------------
        Manifold CollideRoundPolygon(const Shape& a, const Transform& xfA,
                                     const Shape& b, const Transform& xfB,
                                     Real speculativeMargin,
                                     std::uint32_t keyBase)
        {
            Manifold m{};

            const RoundView va = MakeRoundView(a, xfA);
            Vec2 poly[kMaxWorldVerts];
            const int n = WorldPoly(b, xfB, poly);

            for (int k = 0; k < va.count; ++k)
            {
                const Vec2 p = (k == 0) ? va.c0 : va.c1;
                const Hit hit = CirclePoly(p, va.r, poly, n);
                if (hit.hit)
                {
                    const Real nx = hit.normal.x, ny = hit.normal.y;
                    const Vec2 cp(p.x - nx * (va.r - hit.depth),
                                  p.y - ny * (va.r - hit.depth));
                    Emit(m, cp, hit.normal, hit.depth,
                         keyBase + static_cast<std::uint32_t>(k + 1));
                }
                else if (speculativeMargin > Real(0))
                {
                    // Speculative (MODERNIZATION): the circle is clear of the
                    // poly but its skin may reach. circlePoly reports clear when
                    // dist >= r; re-test against the inflated radius (r+margin)
                    // and, if THAT hits while the faithful did not, emit a
                    // speculative contact with negative separation. NEVER runs
                    // for margin==0.
                    const Hit infl = CirclePoly(p, va.r + speculativeMargin, poly, n);
                    if (infl.hit)
                    {
                        // depth against inflated radius = (r+margin) - dist, so
                        // dist = (r+margin) - infl.depth; gap = dist - r.
                        const Real dist = (va.r + speculativeMargin) - infl.depth; // boundary distance
                        const Real gap  = dist - va.r;                             // geometric gap (>0 => separated)
                        if (gap > Real(0) && gap <= speculativeMargin)
                        {
                            const Real nx = infl.normal.x, ny = infl.normal.y;
                            // contact on the mid-gap plane: push the circle
                            // center back along -n by (r + gap/2).
                            const Vec2 cp(p.x - nx * (va.r + gap * Real(0.5)),
                                          p.y - ny * (va.r + gap * Real(0.5)));
                            Emit(m, cp, infl.normal, -gap,
                                 keyBase + static_cast<std::uint32_t>(k + 1));
                        }
                    }
                }
            }
            return m;
        }

        // --------------------------------------------------------------------
        // poly/aabb A vs round B (Manifold.lua:161-171), FLIPPED normal.
        // --------------------------------------------------------------------
        Manifold CollidePolygonRound(const Shape& a, const Transform& xfA,
                                     const Shape& b, const Transform& xfB,
                                     Real speculativeMargin,
                                     std::uint32_t keyBase)
        {
            Manifold m{};

            Vec2 poly[kMaxWorldVerts];
            const int n = WorldPoly(a, xfA, poly);
            const RoundView vb = MakeRoundView(b, xfB);

            for (int k = 0; k < vb.count; ++k)
            {
                const Vec2 p = (k == 0) ? vb.c0 : vb.c1;
                const Hit hit = CirclePoly(p, vb.r, poly, n);
                if (hit.hit)
                {
                    // nx,ny push the CIRCLE (B) out of A; flip for our
                    // push-A-out-of-B convention. The contact POINT uses the
                    // unflipped circlePoly normal (matches Manifold.lua:167).
                    const Real nx = hit.normal.x, ny = hit.normal.y;
                    const Vec2 cp(p.x - nx * (vb.r - hit.depth),
                                  p.y - ny * (vb.r - hit.depth));
                    Emit(m, cp, Vec2(-nx, -ny), hit.depth,
                         keyBase + static_cast<std::uint32_t>(k + 1));
                }
                else if (speculativeMargin > Real(0))
                {
                    const Hit infl = CirclePoly(p, vb.r + speculativeMargin, poly, n);
                    if (infl.hit)
                    {
                        const Real dist = (vb.r + speculativeMargin) - infl.depth; // boundary distance
                        const Real gap  = dist - vb.r;                             // geometric gap (>0 => separated)
                        if (gap > Real(0) && gap <= speculativeMargin)
                        {
                            const Real nx = infl.normal.x, ny = infl.normal.y;
                            const Vec2 cp(p.x - nx * (vb.r + gap * Real(0.5)),
                                          p.y - ny * (vb.r + gap * Real(0.5)));
                            Emit(m, cp, Vec2(-nx, -ny), -gap,
                                 keyBase + static_cast<std::uint32_t>(k + 1));
                        }
                    }
                }
            }
            return m;
        }

    } // namespace Physics
} // namespace Arcane
