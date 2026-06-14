// Gjk.cpp -- 2D GJK distance + closest points, surface distance, and a
// conservative-advancement shape cast (port of GJK.lua + Cast.lua:advance).
//
// See Gjk.hpp for the contract. GjkDistance / ShapeDistance bit-match the
// captured oracle Arcane/Tests/data/physics_oracle/gjk.json (within f32
// tolerance). The simplex reduction, witness interpolation, the 64-iteration
// cap, and the 1e-9 / 1e-18 epsilons are carried over VERBATIM from GJK.lua so
// the branch selection + convergence reproduce the oracle.
//
// PRECISION: GJK.lua runs the distance core in Lua doubles. The stored shape
// geometry here is f32 (PhysicsTypes Real), but to track the f64 distance core
// (so witnesses/distance bit-match within the 1e-4 oracle margin) the simplex
// arithmetic is carried in `double` internally -- f32 core verts are widened on
// read; the GjkResult is narrowed back to Real on return. This keeps the
// convergence test and the closest-point reductions on the same numeric footing
// as the Lua oracle.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/Narrowphase/Gjk.hpp>

#include <array>
#include <cmath>
#include <limits>

namespace Arcane
{
    namespace Physics
    {
        namespace
        {
            // f64 vec2 scratch for the simplex (the Lua distance core is f64).
            struct V2
            {
                double x = 0.0;
                double y = 0.0;
            };

            // support(verts, dx, dy) (GJK.lua:10-17): farthest vert of the
            // f32 world-vert span along (dx,dy), returned widened to f64. The
            // ">" comparison + first-index tie-break match the Lua loop.
            V2 Support(const Vec2* verts, int n, double dx, double dy)
            {
                int bi = 0;
                double best = -std::numeric_limits<double>::infinity();
                for (int i = 0; i < n; ++i)
                {
                    const double d = static_cast<double>(verts[i].x) * dx +
                                     static_cast<double>(verts[i].y) * dy;
                    if (d > best)
                    {
                        best = d;
                        bi = i;
                    }
                }
                return V2{ static_cast<double>(verts[bi].x),
                           static_cast<double>(verts[bi].y) };
            }

            // supportMD (GJK.lua:22-26): Minkowski-difference support =
            // support(A,d) - support(B,-d). Returns the MD point and the two
            // witness points (pa on A, pb on B).
            struct MdSupport
            {
                V2 md; // support(A,d) - support(B,-d)
                V2 pa; // witness on A
                V2 pb; // witness on B
            };

            MdSupport SupportMd(const Vec2* va, int na, const Vec2* vb, int nb,
                                double dx, double dy)
            {
                const V2 pa = Support(va, na, dx, dy);
                const V2 pb = Support(vb, nb, -dx, -dy);
                return MdSupport{ V2{ pa.x - pb.x, pa.y - pb.y }, pa, pb };
            }
        } // namespace

        // --------------------------------------------------------------------
        // GjkDistance -- the simplex loop (GJK.lua:31-107), carried in f64.
        // --------------------------------------------------------------------
        GjkResult GjkDistance(const Vec2* va, int na, const Vec2* vb, int nb)
        {
            // Simplex scratch: MD points (smx/smy) + witnesses on A (sax/say)
            // and B (sbx/sby), mirroring the Lua module-local arrays. Capacity 3
            // (a 2D simplex never exceeds 3 verts under this reduction).
            std::array<double, 3> smx{}, smy{};
            std::array<double, 3> sax{}, say{}, sbx{}, sby{};

            int n = 1;
            {
                const MdSupport s0 = SupportMd(va, na, vb, nb, 1.0, 0.0);
                smx[0] = s0.md.x; smy[0] = s0.md.y;
                sax[0] = s0.pa.x; say[0] = s0.pa.y;
                sbx[0] = s0.pb.x; sby[0] = s0.pb.y;
            }

            // Closest point to origin (px,py) + barycentric weights (l1,l2).
            double px = smx[0], py = smy[0];
            double l1 = 1.0, l2 = 0.0;

            for (int iter = 0; iter < 64; ++iter)
            {
                // Closest point on the simplex to the origin, reducing simplex.
                if (n == 1)
                {
                    px = smx[0]; py = smy[0]; l1 = 1.0; l2 = 0.0;
                }
                else if (n == 2)
                {
                    const double ax_ = smx[0], ay_ = smy[0];
                    const double bx_ = smx[1], by_ = smy[1];
                    const double abx = bx_ - ax_, aby = by_ - ay_;
                    const double dd = abx * abx + aby * aby;
                    const double t =
                        (dd > 0.0) ? (-(ax_ * abx + ay_ * aby)) / dd : 0.0;
                    if (t <= 0.0)
                    {
                        n = 1; px = ax_; py = ay_; l1 = 1.0; l2 = 0.0;
                    }
                    else if (t >= 1.0)
                    {
                        smx[0] = smx[1]; smy[0] = smy[1];
                        sax[0] = sax[1]; say[0] = say[1];
                        sbx[0] = sbx[1]; sby[0] = sby[1];
                        n = 1; px = smx[0]; py = smy[0]; l1 = 1.0; l2 = 0.0;
                    }
                    else
                    {
                        px = ax_ + abx * t; py = ay_ + aby * t;
                        l1 = 1.0 - t; l2 = t;
                    }
                }
                else // n == 3: inside test, else reduce to the closest edge.
                {
                    const double x1 = smx[0], y1 = smy[0];
                    const double x2 = smx[1], y2 = smy[1];
                    const double x3 = smx[2], y3 = smy[2];
                    const double c1 = (x2 - x1) * (-y1) - (y2 - y1) * (-x1);
                    const double c2 = (x3 - x2) * (-y2) - (y3 - y2) * (-x2);
                    const double c3 = (x1 - x3) * (-y3) - (y1 - y3) * (-x3);
                    const bool hasNeg = (c1 < 0.0) || (c2 < 0.0) || (c3 < 0.0);
                    const bool hasPos = (c1 > 0.0) || (c2 > 0.0) || (c3 > 0.0);
                    if (!(hasNeg && hasPos))
                    {
                        // Origin inside the simplex -> the cores intersect.
                        return GjkResult{ Real(0), Vec2(Real(0), Real(0)),
                                          Vec2(Real(0), Real(0)) };
                    }

                    // Reduce to the closest of the three edges to the origin.
                    double bestD = std::numeric_limits<double>::infinity();
                    int e1 = 0, e2 = 1;
                    double bt = 0.0;
                    auto edge = [&](int i, int j)
                    {
                        const double ax_ = smx[i], ay_ = smy[i];
                        const double bx_ = smx[j], by_ = smy[j];
                        const double abx = bx_ - ax_, aby = by_ - ay_;
                        const double dd = abx * abx + aby * aby;
                        double t =
                            (dd > 0.0) ? (-(ax_ * abx + ay_ * aby)) / dd : 0.0;
                        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                        const double cx_ = ax_ + abx * t;
                        const double cy_ = ay_ + aby * t;
                        const double d2 = cx_ * cx_ + cy_ * cy_;
                        if (d2 < bestD) { bestD = d2; e1 = i; e2 = j; bt = t; }
                    };
                    edge(0, 1); edge(1, 2); edge(2, 0);

                    const double t1x = smx[e1], t1y = smy[e1];
                    const double t1ax = sax[e1], t1ay = say[e1];
                    const double t1bx = sbx[e1], t1by = sby[e1];
                    const double t2x = smx[e2], t2y = smy[e2];
                    const double t2ax = sax[e2], t2ay = say[e2];
                    const double t2bx = sbx[e2], t2by = sby[e2];

                    smx[0] = t1x; smy[0] = t1y;
                    sax[0] = t1ax; say[0] = t1ay; sbx[0] = t1bx; sby[0] = t1by;
                    smx[1] = t2x; smy[1] = t2y;
                    sax[1] = t2ax; say[1] = t2ay; sbx[1] = t2bx; sby[1] = t2by;
                    n = 2;
                    px = t1x + (t2x - t1x) * bt;
                    py = t1y + (t2y - t1y) * bt;
                    l1 = 1.0 - bt; l2 = bt;
                    if (bt <= 0.0)
                    {
                        n = 1; l1 = 1.0; l2 = 0.0;
                    }
                    else if (bt >= 1.0)
                    {
                        smx[0] = t2x; smy[0] = t2y;
                        sax[0] = t2ax; say[0] = t2ay;
                        sbx[0] = t2bx; sby[0] = t2by;
                        n = 1; l1 = 1.0; l2 = 0.0;
                    }
                }

                // Origin reached -> the cores intersect (witnesses meaningless).
                if (px * px + py * py < 1e-18)
                {
                    return GjkResult{ Real(0), Vec2(Real(0), Real(0)),
                                      Vec2(Real(0), Real(0)) };
                }

                const double dx_ = -px, dy_ = -py;
                const MdSupport s = SupportMd(va, na, vb, nb, dx_, dy_);
                // No closer support along the search direction -> converged.
                if ((s.md.x * dx_ + s.md.y * dy_) - (px * dx_ + py * dy_) < 1e-9)
                {
                    break;
                }
                // Append the new support (n was 1 or 2 here; index n).
                smx[n] = s.md.x; smy[n] = s.md.y;
                sax[n] = s.pa.x; say[n] = s.pa.y;
                sbx[n] = s.pb.x; sby[n] = s.pb.y;
                n = n + 1;
            }

            const double dist = std::sqrt(px * px + py * py);
            if (n == 1)
            {
                return GjkResult{
                    static_cast<Real>(dist),
                    Vec2(static_cast<Real>(sax[0]), static_cast<Real>(say[0])),
                    Vec2(static_cast<Real>(sbx[0]), static_cast<Real>(sby[0])) };
            }
            return GjkResult{
                static_cast<Real>(dist),
                Vec2(static_cast<Real>(sax[0] * l1 + sax[1] * l2),
                     static_cast<Real>(say[0] * l1 + say[1] * l2)),
                Vec2(static_cast<Real>(sbx[0] * l1 + sbx[1] * l2),
                     static_cast<Real>(sby[0] * l1 + sby[1] * l2)) };
        }

        // --------------------------------------------------------------------
        // BuildCore -- GJK.lua:buildCore (111-131). Returns the core radius.
        // --------------------------------------------------------------------
        Real BuildCore(const Shape& s, const Transform& xf, Vec2* out,
                       int& outCount)
        {
            const Real x = xf.position.x;
            const Real y = xf.position.y;
            switch (s.kind)
            {
            case ShapeKind::Circle:
                out[0] = Vec2(x, y);
                outCount = 1;
                return s.radius;
            case ShapeKind::Capsule:
                out[0] = Vec2(x - s.halfLen, y);
                out[1] = Vec2(x + s.halfLen, y);
                outCount = 2;
                return s.radius;
            case ShapeKind::Aabb:
                out[0] = Vec2(x - s.halfW, y - s.halfH);
                out[1] = Vec2(x + s.halfW, y - s.halfH);
                out[2] = Vec2(x + s.halfW, y + s.halfH);
                out[3] = Vec2(x - s.halfW, y + s.halfH);
                outCount = 4;
                return Real(0);
            case ShapeKind::Polygon:
            default:
            {
                const int n = static_cast<int>(s.verts.size());
                for (int i = 0; i < n; ++i)
                {
                    out[i] = Vec2(x + s.verts[i].x, y + s.verts[i].y);
                }
                outCount = n;
                return Real(0);
            }
            }
        }

        namespace
        {
            // Shared surface-distance assembly (GJK.lua:137-147 / 151-157): take
            // the core GJK result + the two radii and produce the surface
            // distance, surface witnesses, and the B->A normal.
            ShapeDistanceResult AssembleSurface(const GjkResult& g, Real ra,
                                                Real rb)
            {
                ShapeDistanceResult r{};
                Real nx = Real(0), ny = Real(0);
                if (g.distance > Real(1e-9))
                {
                    nx = (g.witnessA.x - g.witnessB.x) / g.distance;
                    ny = (g.witnessA.y - g.witnessB.y) / g.distance;
                }
                r.distance = g.distance - ra - rb;
                r.witnessA = Vec2(g.witnessA.x - nx * ra, g.witnessA.y - ny * ra);
                r.witnessB = Vec2(g.witnessB.x + nx * rb, g.witnessB.y + ny * rb);
                r.normal = Vec2(nx, ny);
                return r;
            }
        } // namespace

        // --------------------------------------------------------------------
        // ShapeDistance -- GJK.lua:shapeDistance (137-147).
        // --------------------------------------------------------------------
        ShapeDistanceResult ShapeDistance(const Shape& a, const Transform& xfA,
                                          const Shape& b, const Transform& xfB)
        {
            Vec2 coreA[kMaxPolyVerts];
            Vec2 coreB[kMaxPolyVerts];
            int na = 0, nb = 0;
            const Real ra = BuildCore(a, xfA, coreA, na);
            const Real rb = BuildCore(b, xfB, coreB, nb);
            const GjkResult g = GjkDistance(coreA, na, coreB, nb);
            return AssembleSurface(g, ra, rb);
        }

        // --------------------------------------------------------------------
        // ShapePolyDistance -- GJK.lua:shapePolyDistance (151-157). B is a raw
        // world poly (rb = 0).
        // --------------------------------------------------------------------
        ShapeDistanceResult ShapePolyDistance(const Shape& a,
                                              const Transform& xfA,
                                              const Vec2* poly, int n)
        {
            Vec2 coreA[kMaxPolyVerts];
            int na = 0;
            const Real ra = BuildCore(a, xfA, coreA, na);
            const GjkResult g = GjkDistance(coreA, na, poly, n);
            return AssembleSurface(g, ra, Real(0));
        }

        namespace
        {
            // advance (Cast.lua:20-29): march the moving shape's center from
            // `start` along `translation` (length `len`), querying `distFn` at
            // each step center. Stop when the surface distance drops below TOL
            // (hit) or t exceeds 1 (miss). distFn yields (distance, normal).
            template <typename DistFn>
            ShapeCastResult Advance(const Vec2& start, const Vec2& translation,
                                    Real len, DistFn&& distFn)
            {
                ShapeCastResult res{};
                Real t = Real(0);
                for (int i = 0; i < kShapeCastMaxIter; ++i)
                {
                    const Vec2 c(start.x + translation.x * t,
                                 start.y + translation.y * t);
                    const ShapeDistanceResult d = distFn(c);
                    if (d.distance < kShapeCastTol)
                    {
                        res.hit = true;
                        res.t = t;
                        res.normal = d.normal;
                        res.distance = d.distance;
                        return res;
                    }
                    t = t + d.distance / len;
                    if (t > Real(1))
                    {
                        return res; // miss
                    }
                }
                return res; // miss (iteration budget exhausted)
            }
        } // namespace

        // --------------------------------------------------------------------
        // ShapeCast -- conservative advancement vs one obstacle shape.
        // --------------------------------------------------------------------
        ShapeCastResult ShapeCast(const Shape& moving, const Transform& start,
                                  const Vec2& translation, const Shape& obstacle,
                                  const Transform& obstacleXf)
        {
            const Real len = std::sqrt(translation.x * translation.x +
                                       translation.y * translation.y);
            if (len < Real(1e-9))
            {
                return ShapeCastResult{}; // no travel -> no hit (Cast.lua guard)
            }
            return Advance(
                start.position, translation, len, [&](const Vec2& c)
                {
                    const Transform xf{ c, start.rotation };
                    return ShapeDistance(moving, xf, obstacle, obstacleXf);
                });
        }

        // --------------------------------------------------------------------
        // ShapeCastPoly -- conservative advancement vs a raw world poly span.
        // --------------------------------------------------------------------
        ShapeCastResult ShapeCastPoly(const Shape& moving,
                                      const Transform& start,
                                      const Vec2& translation, const Vec2* poly,
                                      int n)
        {
            const Real len = std::sqrt(translation.x * translation.x +
                                       translation.y * translation.y);
            if (len < Real(1e-9))
            {
                return ShapeCastResult{};
            }
            return Advance(
                start.position, translation, len, [&](const Vec2& c)
                {
                    const Transform xf{ c, start.rotation };
                    return ShapePolyDistance(moving, xf, poly, n);
                });
        }

    } // namespace Physics
} // namespace Arcane
