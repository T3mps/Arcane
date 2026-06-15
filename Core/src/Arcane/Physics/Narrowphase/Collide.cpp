// Collide.cpp -- unified rotation-aware narrowphase entry (Physics v2, Task 3).
//
// See Collide.hpp for the full contract and algorithm description.
//
// Reference: Box2D v3 manifold.c (b2CollidePolygons, b2ClipSegments) +
//            the existing M6 Sat.cpp / Gjk.cpp for the local style.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics only.

#include <Arcane/Physics/Narrowphase/Collide.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <Arcane/Physics/Narrowphase/Gjk.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>

namespace Arcane
{
    namespace Physics
    {
        namespace
        {
            // ----------------------------------------------------------------
            // RotateInto: rotate + translate a shape's unified core verts
            // (Shape::verts, local space) into world space under xf.
            // xf.rotation is the angle in radians (cos/sin precomputed below).
            // Writes into `out` (caller provides buffer of at least verts.size()).
            // Returns the vertex count.
            // ----------------------------------------------------------------
            int RotateInto(Vec2* out, const Shape& s, const Transform& xf)
            {
                const float c = std::cos(xf.rotation);
                const float si = std::sin(xf.rotation);
                const float px = xf.position.x;
                const float py = xf.position.y;
                const int n = static_cast<int>(s.verts.size());
                for (int i = 0; i < n; ++i)
                {
                    const float lx = s.verts[i].x;
                    const float ly = s.verts[i].y;
                    out[i] = Vec2(lx * c - ly * si + px,
                                  lx * si + ly * c  + py);
                }
                return n;
            }

            // ----------------------------------------------------------------
            // Stable feature-id packing.
            //
            // Bits: [31]    = refIsA  (1 = reference face is on shape A)
            //       [30:16] = refEdgeIdx (15 bits)
            //       [15: 0] = incFeature (16 bits; vertex index or GJK feature)
            //
            // This is invariant to the depth of each frame's overlap: the same
            // physical feature pair always produces the same 32-bit id, enabling
            // the solver's warm-start cache to find the right impulse.
            // ----------------------------------------------------------------
            constexpr uint32_t MakeFeatureId(bool refIsA,
                                             uint32_t refEdge,
                                             uint32_t incFeature) noexcept
            {
                return (refIsA  ? 0x8000'0000u : 0u)
                     | ((refEdge   & 0x7FFFu) << 16)
                     |  (incFeature & 0xFFFFu);
            }

            // ----------------------------------------------------------------
            // SAT helper: find the minimum-separation axis for `va` (na verts)
            // against `vb` (nb verts), testing the CCW edge normals of `vsRef`.
            //
            // Returns (edgeIndex, separation) where separation > 0 means
            // separated (no overlap along this axis), separation < 0 means
            // the ref polygon's edge normal axis has that much penetration.
            //
            // For each CCW edge normal n of vsRef, the minimum support of vb
            // along -n gives the separation on that axis.  We pick the axis
            // with the LEAST penetration (maximum separation, i.e. closest to
            // touching).  Tie-break: lowest edge index wins (deterministic).
            //
            // When the function returns with separation > 0 the shapes are
            // already separated -- caller should not proceed to clipping.
            // ----------------------------------------------------------------
            struct SatAxis
            {
                int  edgeIdx   = 0;
                Real separation = Real(-1e30);
                // The outward edge normal on vsRef (world space, unit).
                Vec2 normal{ Real(0), Real(0) };
            };

            // Compute the CCW outward edge normal of edge i->(i+1) in `va`.
            // For CCW winding the outward normal is (dy, -dx) where d = v[i+1]-v[i].
            // (In y-down screen space CCW means shoelace positive with y increasing
            // downward; the outward normal for edge (a->b) is (b.y-a.y, a.x-b.x).)
            inline Vec2 EdgeNormalCCW(const Vec2* va, int na, int i)
            {
                const Vec2& v0 = va[i];
                const Vec2& v1 = va[(i + 1) % na];
                const float dx = v1.x - v0.x;
                const float dy = v1.y - v0.y;
                // Outward normal (CCW poly, y-down): (dy, -dx), then normalise.
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len < Real(1e-9f)) return Vec2(Real(1), Real(0));
                return Vec2(dy / len, -dx / len);
            }

            // Signed support of `verts` along axis `n` (min projection).
            inline Real MinSupport(const Vec2* verts, int n, const Vec2& axis)
            {
                Real minProj = std::numeric_limits<Real>::infinity();
                for (int i = 0; i < n; ++i)
                {
                    const Real p = verts[i].x * axis.x + verts[i].y * axis.y;
                    if (p < minProj) minProj = p;
                }
                return minProj;
            }

            // MaxSupport of `verts` along axis `n` (max projection).
            inline Real MaxSupport(const Vec2* verts, int n, const Vec2& axis)
            {
                Real maxProj = -std::numeric_limits<Real>::infinity();
                for (int i = 0; i < n; ++i)
                {
                    const Real p = verts[i].x * axis.x + verts[i].y * axis.y;
                    if (p > maxProj) maxProj = p;
                }
                return maxProj;
            }

            // Find the SAT axis (most-penetrating = least-negative separation)
            // by testing all CCW edge normals of `vsRef` against `vsInc`.
            // Returns the edge index and separation (negative = penetration).
            // If separation >= 0 the shapes are separated along this polygon.
            SatAxis FindReferenceAxis(const Vec2* vsRef, int nRef,
                                      const Vec2* vsInc, int nInc)
            {
                SatAxis best{};
                best.separation = -std::numeric_limits<Real>::infinity();
                best.edgeIdx    = 0;

                for (int i = 0; i < nRef; ++i)
                {
                    const Vec2& refV = vsRef[i];
                    const Vec2 n = EdgeNormalCCW(vsRef, nRef, i);

                    // Support of vsRef along n (max projection from vsRef).
                    const Real maxRef = MaxSupport(vsRef, nRef, n);
                    // Support of vsInc along -n (min projection into vsRef).
                    const Real minInc = MinSupport(vsInc, nInc, n);
                    // Separation = minInc - maxRef.
                    // > 0 : separated.   < 0 : penetrating.
                    const Real sep = minInc - maxRef;

                    // Keep the axis with the LEAST penetration (highest sep).
                    // Tie-break: lower edge index wins (deterministic, per spec).
                    if (sep > best.separation)
                    {
                        best.separation = sep;
                        best.edgeIdx    = i;
                        best.normal     = n;
                    }
                    (void)refV; // suppress unused warning
                }
                return best;
            }

            // ----------------------------------------------------------------
            // ClipSegment: clip segment (p0, p1) against the half-plane
            // defined by the plane normal `n` and offset `offset`:
            //   n . x <= offset  (keep side)
            // Writes into `outP`, `outT` (parametric t) up to 2 results.
            // Returns the number of points kept (0, 1, or 2).
            // This is the Box2D v3 b2ClipSegments half-plane logic.
            // ----------------------------------------------------------------
            int ClipSegment(const Vec2& p0, const Vec2& p1,
                            const Vec2& n, Real offset,
                            Vec2 outP[2], float outT[2])
            {
                const Real d0 = p0.x * n.x + p0.y * n.y - offset;
                const Real d1 = p1.x * n.x + p1.y * n.y - offset;
                int count = 0;

                if (d0 <= Real(0))
                {
                    outP[count] = p0;
                    outT[count] = Real(0);
                    ++count;
                }
                if (d1 <= Real(0))
                {
                    outP[count] = p1;
                    outT[count] = Real(1);
                    ++count;
                }
                // If the segment crosses the plane, emit the crossing point.
                if ((d0 < Real(0)) != (d1 < Real(0)))
                {
                    const Real t = d0 / (d0 - d1);
                    outP[count] = Vec2(p0.x + t * (p1.x - p0.x),
                                       p0.y + t * (p1.y - p0.y));
                    outT[count] = t;
                    ++count;
                }
                return count;
            }

            // ----------------------------------------------------------------
            // Round-shape fast path (circle / capsule).
            //
            // When at least one core is 1 or 2 verts (circle or capsule), the
            // GJK witness pair IS the contact (no face-clip needed).  After
            // computing via GjkDistanceCore:
            //   - If coreDist >= rA+rB  -> separated or speculative.
            //   - If coreDist == 0      -> deep overlap: use the direction
            //     from B's centroid to A's centroid as a fallback normal (rare).
            //   - Otherwise             -> coreDist < rA+rB -> penetration via
            //     the witness pair.
            // Returns a 1-point manifold (for circle-vs-* the cores are both
            // single points or short segments; the closest feature pair is the
            // contact).
            //
            // Feature id for the speculative/round path: pack featureA (from A's
            // core) and featureB (from B's core) from GjkDistanceCore directly
            // into the stable id scheme (refIsA=true, refEdge=featureA & 0x7FFF,
            // incFeature=featureB & 0xFFFF). This gives a stable key as long as
            // the same core features are in contact, independent of depth.
            // ----------------------------------------------------------------
            Manifold CollideRound(const Vec2* va, int na, Real rA,
                                  const Vec2* vb, int nb, Real rB,
                                  Real speculativeMargin)
            {
                Manifold m{};
                const GjkCoreResult g = GjkDistanceCore(va, na, vb, nb);

                // Build a stable id from the GJK feature pair.
                // We use refIsA=true, encode featureA in bits [30:16] and
                // featureB in bits [15:0].
                const uint32_t id = MakeFeatureId(
                    true,
                    g.featureA & 0x7FFFu,
                    g.featureB & 0xFFFFu);

                const Real totalR = rA + rB;

                // Separated beyond speculative margin: no contact.
                // Use strict ">" so coreDist == totalR (surface touch) still
                // generates a contact.
                if (g.distance > totalR + speculativeMargin)
                {
                    return m;
                }

                Vec2 normal{ Real(-1), Real(0) }; // default fallback
                Vec2 ptA = g.pointA;
                Vec2 ptB = g.pointB;

                if (g.distance > Real(1e-6f))
                {
                    // Normal from B's witness toward A's witness (B->A convention).
                    const Real inv = Real(1) / g.distance;
                    normal = Vec2((g.pointA.x - g.pointB.x) * inv,
                                  (g.pointA.y - g.pointB.y) * inv);
                }
                else
                {
                    // Cores touching/overlapping: derive from centroid offset.
                    // Fallback: use +x if cores coincide.
                    Real cx = Real(0), cy = Real(0);
                    for (int i = 0; i < na; ++i) { cx += va[i].x; cy += va[i].y; }
                    cx /= (na > 0 ? Real(na) : Real(1));
                    cy /= (na > 0 ? Real(na) : Real(1));
                    Real dx_ = Real(0), dy_ = Real(0);
                    for (int i = 0; i < nb; ++i) { dx_ += vb[i].x; dy_ += vb[i].y; }
                    dx_ = cx - dx_ / (nb > 0 ? Real(nb) : Real(1));
                    dy_ = cy - dy_ / (nb > 0 ? Real(nb) : Real(1));
                    const Real len2 = dx_ * dx_ + dy_ * dy_;
                    if (len2 > Real(1e-12f))
                    {
                        const Real inv = Real(1) / std::sqrt(len2);
                        normal = Vec2(dx_ * inv, dy_ * inv);
                    }
                    // With deep overlap, witnesses from GJK are zero; place them
                    // at the respective centroids.
                    ptA = Vec2(cx, cy);
                    Real bcx = Real(0), bcy = Real(0);
                    for (int i = 0; i < nb; ++i) { bcx += vb[i].x; bcy += vb[i].y; }
                    ptB = Vec2(bcx / (nb > 0 ? Real(nb) : Real(1)),
                               bcy / (nb > 0 ? Real(nb) : Real(1)));
                }

                // Surface witnesses: offset the core witnesses outward by their radii.
                // surfaceA = ptA - normal*rA  (A's surface toward B)
                // surfaceB = ptB + normal*rB  (B's surface toward A)
                const Vec2 surfaceA = Vec2(ptA.x - normal.x * rA,
                                           ptA.y - normal.y * rA);
                const Vec2 surfaceB = Vec2(ptB.x + normal.x * rB,
                                           ptB.y + normal.y * rB);

                // Contact point: midpoint of the two surface witnesses.
                const Vec2 cp = Vec2((surfaceA.x + surfaceB.x) * Real(0.5f),
                                     (surfaceA.y + surfaceB.y) * Real(0.5f));

                const Real separation = totalR - g.distance; // positive = penetration

                if (g.distance > totalR)
                {
                    // Speculative gap: coreDist > totalR but within margin.
                    // separation = totalR - coreDist (negative = gap).
                    m.normal = normal;
                    m.points[0] = ManifoldPoint{ cp, totalR - g.distance, normal, id };
                    m.pointCount = 1;
                }
                else
                {
                    // Real penetration (coreDist <= totalR).
                    m.normal = normal;
                    m.points[0] = ManifoldPoint{ cp, separation, normal, id };
                    m.pointCount = 1;
                }

                return m;
            }

        } // namespace

        // ====================================================================
        // Collide -- the unified v2 narrowphase entry.
        // ====================================================================
        Manifold Collide(const Shape& a, const Transform& xfA,
                         const Shape& b, const Transform& xfB,
                         Real speculativeMargin)
        {
            Manifold m{};

            // Stack scratch: kMaxPolyVerts = 128, but the common cases (circle,
            // capsule, aabb) are 1-4 verts. 128 * sizeof(Vec2) = 1 KB -- fine.
            Vec2 va[kMaxPolyVerts];
            Vec2 vb[kMaxPolyVerts];

            const int na = RotateInto(va, a, xfA);
            const int nb = RotateInto(vb, b, xfB);

            const Real rA = a.radius;
            const Real rB = b.radius;

            // ----------------------------------------------------------------
            // Fast path: at least one shape is round (circle or capsule core).
            // Circle = 1 vert, capsule = 2 verts. When either core is "short"
            // (<=2 verts) the reference-face clip doesn't apply -- use the
            // GJK witness pair directly (one contact point per closest feature).
            // ----------------------------------------------------------------
            if (na <= 2 || nb <= 2)
            {
                return CollideRound(va, na, rA, vb, nb, rB, speculativeMargin);
            }

            // ----------------------------------------------------------------
            // Both cores have >= 3 verts (polygon / AABB). Use GJK to test
            // separation / speculative, then SAT + reference-face clip for
            // deep overlap.
            // ----------------------------------------------------------------
            const GjkCoreResult g = GjkDistanceCore(va, na, vb, nb);
            const Real totalR = rA + rB;

            if (g.distance > totalR + speculativeMargin)
            {
                // Fully separated beyond the speculative margin -- no contact.
                return m;
            }

            if (g.distance > totalR)
            {
                // Speculative gap: surface distance in (0, margin].
                // Emit one speculative contact point.
                const Real dist = g.distance;
                Vec2 normal{ Real(-1), Real(0) };
                if (dist > Real(1e-6f))
                {
                    const Real inv = Real(1) / dist;
                    normal = Vec2((g.pointA.x - g.pointB.x) * inv,
                                  (g.pointA.y - g.pointB.y) * inv);
                }
                const Vec2 cp = Vec2((g.pointA.x + g.pointB.x) * Real(0.5f),
                                     (g.pointA.y + g.pointB.y) * Real(0.5f));

                const uint32_t id = MakeFeatureId(
                    true,
                    g.featureA & 0x7FFFu,
                    g.featureB & 0xFFFFu);

                m.normal = normal;
                m.points[0] = ManifoldPoint{ cp, totalR - dist, normal, id };
                m.pointCount = 1;
                return m;
            }

            // ----------------------------------------------------------------
            // Cores overlap (GJK returned distance 0). Use SAT to find the
            // reference face (minimum-penetration axis), then clip the incident
            // edge to produce up to 2 contact points.
            //
            // We test all edge normals of both polygons and take the axis that
            // produces the LEAST penetration (Box2D v3 style).
            // ----------------------------------------------------------------

            // Test A's edge normals as reference axes against B.
            const SatAxis axisA = FindReferenceAxis(va, na, vb, nb);
            // Test B's edge normals as reference axes against A.
            const SatAxis axisB = FindReferenceAxis(vb, nb, va, na);

            // Pick the less-penetrating (higher separation) axis.
            // Tie-break: if equal, prefer A as reference (refIsA=true).
            // Both separations are negative (we're in the overlap case).
            bool refIsA;
            SatAxis refAxis;
            if (axisA.separation >= axisB.separation)
            {
                refIsA  = true;
                refAxis = axisA;
            }
            else
            {
                refIsA  = false;
                refAxis = axisB;
            }

            // Reference face: vertices of the reference polygon.
            const Vec2* vsRef = refIsA ? va : vb;
            const int   nRef  = refIsA ? na : nb;
            const Vec2* vsInc = refIsA ? vb : va;
            const int   nInc  = refIsA ? nb : na;

            // The reference edge index.
            const int refEdge = refAxis.edgeIdx;

            // The reference face outward normal (world space, unit).
            // This points AWAY from the reference shape (outward).
            // We need it to point B->A: if refIsA the outward normal of A
            // already points away from A (outward), and since A is the body
            // being pushed out is NOT the goal -- actually:
            //   normal convention: B -> A (push A out of B).
            // If refIsA (reference face on A): the outward normal of A's edge
            // points AWAY from A -- that is, toward B. So the B->A direction
            // is the NEGATION of A's outward normal.
            // If refIsB (reference face on B): the outward normal of B's edge
            // points AWAY from B -- that is, toward A. So the B->A direction
            // IS B's outward normal.
            Vec2 contactNormal;
            if (refIsA)
            {
                // Reference is A; its outward normal points toward B (away from A).
                // B->A = opposite direction.
                contactNormal = Vec2(-refAxis.normal.x, -refAxis.normal.y);
            }
            else
            {
                // Reference is B; its outward normal points toward A (B->A).
                contactNormal = refAxis.normal;
            }

            // Reference face verts: edge refEdge -> refEdge+1.
            const Vec2& refV0 = vsRef[refEdge];
            const Vec2& refV1 = vsRef[(refEdge + 1) % nRef];

            // Reference face tangent (direction along the face).
            const Vec2 refTangent = Vec2(refV1.x - refV0.x, refV1.y - refV0.y);

            // Find the incident edge on the incident polygon: the edge whose
            // outward normal is most anti-parallel to the REFERENCE face's
            // outward normal (refAxis.normal). This selects the face of the
            // incident shape that is most facing the reference face, i.e.,
            // the face being "pushed into" the reference shape.
            // (Most anti-parallel = dot product with refAxis.normal most negative.)
            int incEdge = 0;
            {
                Real minDot = std::numeric_limits<Real>::infinity();
                for (int i = 0; i < nInc; ++i)
                {
                    const Vec2 n = EdgeNormalCCW(vsInc, nInc, i);
                    // Dot product with the reference face outward normal:
                    // the incident face that faces the reference face has the
                    // most negative dot product with refAxis.normal.
                    const Real d = n.x * refAxis.normal.x + n.y * refAxis.normal.y;
                    if (d < minDot)
                    {
                        minDot  = d;
                        incEdge = i;
                    }
                }
            }

            // Incident edge verts (the two endpoints to clip).
            const Vec2& incV0 = vsInc[incEdge];
            const Vec2& incV1 = vsInc[(incEdge + 1) % nInc];

            // ----------------------------------------------------------------
            // Clip the incident segment against the two side planes of the
            // reference face (Box2D v3 b2ClipSegments).
            //
            // Side plane 1: the "left" side of the reference edge (along -tangent).
            //   plane normal = -refTangent (normalised), offset = -tangent . refV0
            // Side plane 2: the "right" side (along +tangent).
            //   plane normal = +refTangent (normalised), offset = +tangent . refV1
            // ----------------------------------------------------------------
            const Real tangentLen = std::sqrt(refTangent.x * refTangent.x +
                                               refTangent.y * refTangent.y);
            Vec2 tangentN{ Real(1), Real(0) };
            if (tangentLen > Real(1e-9f))
            {
                tangentN = Vec2(refTangent.x / tangentLen,
                                refTangent.y / tangentLen);
            }

            // Side clip 1: keep points where dot(p, -tangentN) <= -dot(refV0, tangentN)
            //   <=> dot(p, tangentN) >= dot(refV0, tangentN)
            // Rewrite as: dot(p, -tangentN) <= dot(refV0, -tangentN)
            const Vec2 negTangentN = Vec2(-tangentN.x, -tangentN.y);
            const Real offset1 = refV0.x * negTangentN.x + refV0.y * negTangentN.y;

            Vec2 clipBuf1[2];
            float clipT1[2]{};
            int nc1 = ClipSegment(incV0, incV1, negTangentN, offset1,
                                   clipBuf1, clipT1);

            if (nc1 == 0) return m; // degenerate: nothing survives clip 1

            // Side clip 2: keep points where dot(p, tangentN) <= dot(refV1, tangentN)
            const Real offset2 = refV1.x * tangentN.x + refV1.y * tangentN.y;

            Vec2 clipBuf2[2];
            float clipT2[2]{};
            int nc2;
            if (nc1 >= 2)
            {
                // Normal 2-point case: Sutherland-Hodgman clip of the surviving segment.
                nc2 = ClipSegment(clipBuf1[0], clipBuf1[1],
                                   tangentN, offset2, clipBuf2, clipT2);
            }
            else
            {
                // nc1 == 1: only a single point survived clip 1.
                // Do NOT pass (p, p) to ClipSegment — the zero-length segment
                // would fire the crossing logic and could emit the same point twice.
                // Instead, just apply a single half-plane keep/drop on the one point.
                const Real d = clipBuf1[0].x * tangentN.x
                             + clipBuf1[0].y * tangentN.y - offset2;
                if (d <= Real(0))
                {
                    clipBuf2[0] = clipBuf1[0];
                    clipT2[0]   = clipT1[0];
                    nc2 = 1;
                }
                else
                {
                    nc2 = 0;
                }
            }

            if (nc2 == 0) return m;

            // ----------------------------------------------------------------
            // Keep only the clipped points that are on the "inside" of the
            // reference face (behind its plane, i.e. penetrating the reference
            // shape). Compute the signed depth per point.
            //
            // The reference face plane: normal = refAxis.normal (outward from
            // reference shape), offset = dot(refV0, refAxis.normal).
            // A point p is inside (penetrating) when:
            //   dot(p, refAxis.normal) < dot(refV0, refAxis.normal)
            // i.e. depth = dot(refV0, refAxis.normal) - dot(p, refAxis.normal) > 0
            // ----------------------------------------------------------------
            const Real refFaceOffset = refV0.x * refAxis.normal.x +
                                       refV0.y * refAxis.normal.y;

            m.normal = contactNormal;
            m.pointCount = 0;

            for (int i = 0; i < nc2 && m.pointCount < 2; ++i)
            {
                const Vec2& cp = clipBuf2[i];
                const Real  proj = cp.x * refAxis.normal.x + cp.y * refAxis.normal.y;
                const Real  depth = refFaceOffset - proj; // positive = penetrating

                // Only keep penetrating points (depth > 0).
                // A point exactly on the face (depth==0) is a degenerate touch;
                // we discard it for stability (the speculative path handles gaps).
                if (depth <= Real(0)) continue;

                // Stable feature id: pack (refIsA, refEdge, incEdge + sub-index).
                // For the two clipped points we use incEdge itself and incEdge+1
                // as the "incident vertex" sub-index so the two ids differ:
                //   point 0: incFeature = incEdge
                //   point 1: incFeature = incEdge+1  (wraps mod nInc)
                // This is stable as long as the same incident edge is selected.
                const uint32_t incFeature = static_cast<uint32_t>(
                    (incEdge + i) % nInc);
                const uint32_t id = MakeFeatureId(
                    refIsA,
                    static_cast<uint32_t>(refEdge),
                    incFeature);

                ManifoldPoint mp{};
                mp.point      = cp;
                mp.separation = depth;
                mp.normal     = contactNormal;
                mp.id         = id;
                m.points[m.pointCount++] = mp;
            }

            return m;
        }

    } // namespace Physics
} // namespace Arcane
