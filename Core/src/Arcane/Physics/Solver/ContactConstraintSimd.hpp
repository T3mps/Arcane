#pragma once

// ContactConstraintSimd.hpp -- the SoA contact-constraint BATCH + its packer +
// the lane-wide TGS-Soft solve passes.
//
// DATA + PACKER + LANE-WIDE SOLVE. This header owns three things:
//   1. ContactConstraintSimd -- the Structure-of-Arrays staging buffer (one
//      contact per SIMD lane, up to 2 points), the transposed layout the solve
//      gathers/scatters through.
//   2. Build -- the deterministic packer that fills a batch list from a color's
//      emitted `ContactConstraint`s (a pure COPY; it computes no masses/coeffs).
//   3. namespace SimdSolve -- the lane-wide `f32w` load / gather / select / solve
//      / scatter passes (WarmStart / SolveNormalAndFriction / ApplyRestitution +
//      StoreImpulses). SoftStep::Solve drives these per color per substep; the
//      scalar twin for the un-colorable overflow bucket lives in SoftStep.cpp
//      (Overflow{WarmStart/Solve/Restitution}), and the two MUST stay in lockstep.
//
// ----------------------------------------------------------------------------
// ONE CONTACT PER LANE (the Box2D-v3 layout)
// ----------------------------------------------------------------------------
// A batch packs `width` independent contacts -- one per SIMD lane -- where
// `width` is Arcane::Simd::f32w::width (8 on AVX2, 4 on NEON, 1 forced-scalar).
// A contact carries up to 2 manifold points, so the per-POINT fields are
// arrays-of-2-of-lane-arrays (points[0], points[1]); the per-CONTACT fields
// (normal, friction, masses, ...) are single lane arrays. We pack per-CONTACT
// (not per-point) so a contact's two points solve together in one lane with the
// same body pair -- the point-2 lane simply masks off (pointValid=0) when a
// contact has a single point. Graph coloring (Task 2) guarantees no two contacts
// in a batch share a dynamic body, so all `width` lanes are independent and the
// whole batch solves with no cross-lane read-modify-write hazard.
//
// ----------------------------------------------------------------------------
// PLAIN SCALAR LANE ARRAYS, alignas(32)
// ----------------------------------------------------------------------------
// The fields below are plain `alignas(32) float[width]` (and int[width] for the
// body indices), NOT `f32w`. We build the batch with ordinary scalar stores;
// the solve `load`s each array into an `f32w` once per sub-step. alignas(32)
// makes those loads/stores naturally aligned (a 256-bit AVX2 vector) so the
// solve can use aligned moves. Storing `f32w` directly here would force the SoA
// to carry the active backend's vector type into Core's data layout for no gain.
//
// ----------------------------------------------------------------------------
// FLOAT MASKS (dynB, pointValid), not bool/int
// ----------------------------------------------------------------------------
// `dynB` and `pointValid` are 1.0f / 0.0f FLOAT lane arrays, not a bool/int mask
// type. The solve turns them into a `b32w` predicate via
// `cmp_gt(load(mask), setzero())` and feeds that to `select(...)` -- the Simd
// wrapper's select path consumes a b32w derived from a float compare, so a float
// mask is exactly the shape it wants (and it `load`s with the same aligned move
// as every other lane array; no separate int mask plumbing). dynB gates whether
// B's velocity is mutated (B dynamic) or treated read-only (static/kinematic/
// span); pointValid gates whether a point contributes (a 1-point contact's
// second lane is a masked no-op).
//
// ----------------------------------------------------------------------------
// PADDING IS A LANE-WIDE NO-OP
// ----------------------------------------------------------------------------
// A color's contact count is rarely a multiple of `width`, so the final batch
// has padding lanes. Build masks them to a pure no-op: invMass*/invInertia* = 0
// (immovable -> zero effective impulse), both points invalid + their impulses 0,
// and bodyIndex = 0 (a safe in-range gather slot -- the masked impulse is 0 so
// the scatter writes nothing meaningful, but the gather must still hit a valid
// address). The solve therefore runs uniformly across all `width` lanes without
// a per-lane branch, and padding contributes nothing to any body.
//
// SCOPE: the struct + Build (a pure packer) + the lane-wide SimdSolve passes
// (further down this file) that consume a batch list. SoftStep::Solve drives the
// SimdSolve passes per color per substep. Build COPIES field values out of the
// source ContactConstraint/...Point (it does NOT compute effective masses or soft
// coefficients -- those are either already filled on the source cc or computed
// lane-wide at solve time).
//
// PRESENTATION-FREE + C++20-clean: std + the Simd width constant + Solver.hpp
// (for ContactConstraint). No SDL3/NVRHI/Batcher2D. namespace Arcane::Physics.

#include <cstdint>
#include <vector>

#include <Arcane/Math/Simd.hpp>                  // Arcane::Simd::f32w::width + ops
#include <Arcane/Physics/Solver/BodyStateSoA.hpp> // gather/scatter target (T5 solve)
#include <Arcane/Physics/Solver/Solver.hpp>       // ContactConstraint / ...Point

namespace Arcane
{
    namespace Physics
    {
        // SoA batch of `kWidth` independent contacts -- one contact per lane,
        // up to 2 points per contact. Built scalar by Build(); loaded into f32w
        // at solve time (Task 5). All float lane arrays are alignas(32) for
        // aligned 256-bit loads; body-index arrays are std::int32_t for the i32w
        // gather of body velocities.
        struct ContactConstraintSimd
        {
            // Lane width == the active SIMD float width (8 AVX2 / 4 NEON / 1
            // scalar). A batch holds exactly this many contacts (the tail batch
            // pads the unused lanes -- see `count` + Build's padding path).
            static constexpr int kWidth = static_cast<int>(Arcane::Simd::f32w::width);

            // Per-POINT lane arrays (a contact has up to 2 of these). Mirrors
            // ContactConstraintPoint, transposed to SoA. pointValid is the float
            // mask (1.0f if this point is live on this lane, else 0.0f).
            struct Point
            {
                alignas(32) float anchorAx[kWidth];        // anchorA.x
                alignas(32) float anchorAy[kWidth];        // anchorA.y
                alignas(32) float anchorBx[kWidth];        // anchorB.x
                alignas(32) float anchorBy[kWidth];        // anchorB.y
                alignas(32) float baseSep[kWidth];         // baseSeparation
                alignas(32) float normalMass[kWidth];      // effective normal mass
                alignas(32) float tangentMass[kWidth];     // effective tangent mass
                alignas(32) float normalImpulse[kWidth];   // accumulated normal impulse
                alignas(32) float tangentImpulse[kWidth];  // accumulated tangent impulse
                alignas(32) float relVel[kWidth];          // relativeVelocity (restitution)
                alignas(32) float pointValid[kWidth];      // float mask: live point?
            };

            // Per-CONTACT lane arrays (transposed ContactConstraint).
            alignas(32) float normalX[kWidth];      // normal.x (B -> A)
            alignas(32) float normalY[kWidth];      // normal.y
            alignas(32) float friction[kWidth];     // combined friction
            alignas(32) float restitution[kWidth];  // combined restitution
            alignas(32) float biasRate[kWidth];     // soft bias rate
            alignas(32) float massScale[kWidth];    // soft mass scale
            alignas(32) float impulseScale[kWidth]; // soft impulse scale
            alignas(32) float invMassA[kWidth];     // body A inverse mass
            alignas(32) float invInertiaA[kWidth];  // body A inverse inertia
            alignas(32) float invMassB[kWidth];     // body B inverse mass
            alignas(32) float invInertiaB[kWidth];  // body B inverse inertia

            // Body index lanes -> i32w gather/scatter of BodyStateSoA velocities.
            alignas(32) std::int32_t bodyIndexA[kWidth];   // A world slot
            // B world slot, OR 0 when dynB==0 (read-only B: static/kinematic/span)
            // so the unconditional T5 gather stays in-range; the value is
            // discarded by the dynB mask.
            alignas(32) std::int32_t bodyIndexB[kWidth];

            // Float mask: 1.0f iff B is a DYNAMIC body whose velocity the solve
            // mutates (bodyBIsBody && invMassB > 0); 0.0f for a static/kinematic
            // body or a tile-span virtual fixture (B is read-only -> no scatter).
            alignas(32) float dynB[kWidth];

            // Two manifold points; a 1-point contact leaves points[1] masked off.
            Point points[2]{};

            // Live lanes in this batch (<= kWidth). The lanes [count, kWidth)
            // are padding (masked no-ops). Lets the consumer know the live prefix
            // without re-deriving it (the solve still runs all kWidth lanes; this
            // is for write-back / inspection).
            int count = 0;

            // ----------------------------------------------------------------
            // Build -- pack `count` contacts (the color's `refs`, indexing into
            // `ccs`) into ceil(count / kWidth) SoA batches.
            // ----------------------------------------------------------------
            //
            // Pure deterministic packer: for lane L of batch B, if the global
            // index g = B*kWidth + L is < count, copy ccs[refs[g]]'s fields into
            // lane L (computing dynB + per-point pointValid, and zeroing a point
            // whose index >= pointCount); otherwise lane L is PADDING -> a
            // lane-wide no-op (zero inv-mass/inertia on both bodies, both points
            // invalid + impulses 0). It COPIES values only -- it does not compute
            // effective masses or soft coefficients.
            //
            // SCATTER-SAFE DUMMY SLOT (Task 5). The lane-wide solve scatters body
            // velocities back UNMASKED (AVX2 scatter serializes -> last write wins).
            // A padding lane, or a read-only-B lane (static/kinematic/span), must
            // NOT scatter to a real dynamic body's slot or it could clobber that
            // body's freshly-solved velocity with a stale value. So the caller
            // sizes BodyStateSoA to world.Count()+1 and passes dummyIndex =
            // world.Count(): a zeroed throwaway slot that ONLY padding + read-only-B
            // lanes target. The gather/scatter then run unconditionally and only the
            // dummy slot ever takes a redundant write -- no real body is corrupted.
            //
            // dummyIndex DEFAULTS to 0 so existing Task-4 packer tests (which assert
            // read-only-B / padding bodyIndexB == 0) keep their contract; the solver
            // always passes a real dummy index. bodyIndexA is never dummied (A is
            // always a dynamic body in the solver feed).
            //
            // refs may be null iff count == 0.
            static std::vector<ContactConstraintSimd>
            Build(const ContactConstraint* ccs, const std::uint32_t* refs, int count,
                  std::int32_t dummyIndex = 0)
            {
                std::vector<ContactConstraintSimd> batches;
                if (count <= 0)
                {
                    return batches;
                }

                const int batchCount = (count + kWidth - 1) / kWidth;
                batches.resize(static_cast<std::size_t>(batchCount));

                for (int b = 0; b < batchCount; ++b)
                {
                    ContactConstraintSimd& dst = batches[static_cast<std::size_t>(b)];
                    const int base = b * kWidth;
                    const int live = (count - base) < kWidth ? (count - base) : kWidth;
                    dst.count = live;

                    for (int L = 0; L < kWidth; ++L)
                    {
                        const int g = base + L;
                        if (g < count)
                        {
                            PackLane(dst, L, ccs[refs[g]], dummyIndex);
                        }
                        else
                        {
                            PadLane(dst, L, dummyIndex);
                        }
                    }
                }

                return batches;
            }

        private:
            // Copy one source ContactConstraint into lane L (the live path).
            // dummyIndex is the scatter-safe throwaway slot a read-only B points at.
            static void PackLane(ContactConstraintSimd& dst, int L,
                                 const ContactConstraint& cc,
                                 std::int32_t dummyIndex)
            {
                dst.normalX[L]      = static_cast<float>(cc.normal.x);
                dst.normalY[L]      = static_cast<float>(cc.normal.y);
                dst.friction[L]     = static_cast<float>(cc.friction);
                dst.restitution[L]  = static_cast<float>(cc.restitution);
                dst.biasRate[L]     = static_cast<float>(cc.biasRate);
                dst.massScale[L]    = static_cast<float>(cc.massScale);
                dst.impulseScale[L] = static_cast<float>(cc.impulseScale);
                dst.invMassA[L]     = static_cast<float>(cc.invMassA);
                dst.invInertiaA[L]  = static_cast<float>(cc.invInertiaA);
                dst.invMassB[L]     = static_cast<float>(cc.invMassB);
                dst.invInertiaB[L]  = static_cast<float>(cc.invInertiaB);

                dst.bodyIndexA[L]   = static_cast<std::int32_t>(cc.bodyA);

                // dynB: B's velocity is MUTATED (scattered back) only if B is a real
                // dynamic body. A static/kinematic body (invMassB == 0) or a tile
                // span (bodyBIsBody == false) is read-only -> mask off (no scatter).
                const bool dyn = cc.bodyBIsBody && cc.invMassB > Real(0);
                dst.dynB[L]         = dyn ? 1.0f : 0.0f;

                // bodyIndexB must be an in-range gather slot on EVERY lane (the T5
                // read of B's velocity is an UNMASKED gather) AND must not let a
                // read-only B's UNMASKED scatter clobber a real DYNAMIC body's
                // freshly-solved velocity. Two cases for a read-only B:
                //   * a REAL body (kinematic/static, bodyBIsBody && invMassB==0):
                //     point at its REAL slot cc.bodyB. The solver MUST gather its
                //     velocity (a kinematic plate's authored +X feeds the relative-
                //     velocity term that drives the push -- the solve reads vB for any
                //     bodyBIsBody endpoint). The scatter writes back
                //     the UNCHANGED gathered value (select(dynB=0, new, gathered)),
                //     so the kinematic/static slot is preserved -- idempotent even if
                //     several read-only-B lanes in a color share it, and a kinematic
                //     body is never a dynamic A, so it can never collide with an
                //     updated A slot.
                //   * NOT a real body (tile span, bodyBIsBody == false, cc.bodyB ==
                //     kInvalidSlot -> -1): point at the scatter-safe DUMMY slot (the
                //     gather must hit a valid address; the value is discarded by the
                //     dynB mask and the scatter only touches the throwaway dummy).
                // A is always dynamic in the solver feed, so bodyIndexA needs no
                // clamp. dummyIndex defaults to 0 (the Task-4 packer contract).
                dst.bodyIndexB[L]   = cc.bodyBIsBody
                                          ? static_cast<std::int32_t>(cc.bodyB)
                                          : dummyIndex;

                for (int p = 0; p < 2; ++p)
                {
                    Point& pt = dst.points[p];
                    if (p < cc.pointCount)
                    {
                        const ContactConstraintPoint& cp = cc.points[p];
                        pt.anchorAx[L]       = static_cast<float>(cp.anchorA.x);
                        pt.anchorAy[L]       = static_cast<float>(cp.anchorA.y);
                        pt.anchorBx[L]       = static_cast<float>(cp.anchorB.x);
                        pt.anchorBy[L]       = static_cast<float>(cp.anchorB.y);
                        pt.baseSep[L]        = static_cast<float>(cp.baseSeparation);
                        pt.normalMass[L]     = static_cast<float>(cp.normalMass);
                        pt.tangentMass[L]    = static_cast<float>(cp.tangentMass);
                        pt.normalImpulse[L]  = static_cast<float>(cp.normalImpulse);
                        pt.tangentImpulse[L] = static_cast<float>(cp.tangentImpulse);
                        pt.relVel[L]         = static_cast<float>(cp.relativeVelocity);
                        pt.pointValid[L]     = 1.0f;
                    }
                    else
                    {
                        // Point absent on this contact (pointCount < 2): a masked
                        // no-op lane. Zero its masses + impulses so even if the
                        // mask were ignored the contribution is identically zero.
                        ZeroPointLane(pt, L);
                    }
                }
            }

            // Mask a whole lane to a no-op (a padding lane in the tail batch).
            // Zero inv-mass/inertia (immovable -> zero impulse), both points
            // invalid, BOTH body indices the scatter-safe dummy slot (the gather
            // must hit a valid slot; the impulse is 0 so the scatter writes 0 to
            // the throwaway dummy -- never a real body). dummyIndex defaults to 0
            // (the Task-4 padding contract) and is the world.Count() throwaway in
            // the solver feed.
            static void PadLane(ContactConstraintSimd& dst, int L,
                                std::int32_t dummyIndex)
            {
                dst.normalX[L]      = 0.0f;
                dst.normalY[L]      = 0.0f;
                dst.friction[L]     = 0.0f;
                dst.restitution[L]  = 0.0f;
                dst.biasRate[L]     = 0.0f;
                dst.massScale[L]    = 0.0f;
                dst.impulseScale[L] = 0.0f;
                dst.invMassA[L]     = 0.0f;
                dst.invInertiaA[L]  = 0.0f;
                dst.invMassB[L]     = 0.0f;
                dst.invInertiaB[L]  = 0.0f;
                dst.bodyIndexA[L]   = dummyIndex;
                dst.bodyIndexB[L]   = dummyIndex;
                dst.dynB[L]         = 0.0f;
                for (int p = 0; p < 2; ++p)
                {
                    ZeroPointLane(dst.points[p], L);
                }
            }

            // Zero one point's lane: invalidate it + null its masses/impulses so
            // it cannot contribute (the shared "absent / padding point" reset).
            static void ZeroPointLane(Point& pt, int L)
            {
                pt.anchorAx[L]       = 0.0f;
                pt.anchorAy[L]       = 0.0f;
                pt.anchorBx[L]       = 0.0f;
                pt.anchorBy[L]       = 0.0f;
                pt.baseSep[L]        = 0.0f;
                pt.normalMass[L]     = 0.0f;
                pt.tangentMass[L]    = 0.0f;
                pt.normalImpulse[L]  = 0.0f;
                pt.tangentImpulse[L] = 0.0f;
                pt.relVel[L]         = 0.0f;
                pt.pointValid[L]     = 0.0f;
            }
        };

        // ====================================================================
        // Lane-wide contact solve passes (Task 5) -- the TGS-Soft math ported
        // from SoftStep.cpp, per lane, over Arcane::Simd.
        // ====================================================================
        //
        // Each pass takes the per-color batch vector + the solver's BodyStateSoA
        // and processes ALL kWidth lanes of every batch (padding/read-only-B lanes
        // are masked no-ops by construction -- zero inv-mass and the dummy slot).
        // Graph coloring guarantees no two LIVE lanes in a batch share a dynamic
        // body, so the gather -> math -> scatter is hazard-free; the only redundant
        // scatter lands on the dummy slot (see Build's SCATTER-SAFE DUMMY note).
        //
        // LOCKSTEP: this SimdSolve trio (WarmStart / SolveNormalAndFriction /
        // ApplyRestitution) must match the scalar overflow trio Overflow{WarmStart/
        // Solve/Restitution} in SoftStep.cpp lane-for-scalar. The colored batches and
        // the overflow constraints share ONE BodyStateSoA and compose into ONE
        // velocity store within a single substep, so the lane-wide math here and the
        // width-1 sequential overflow math MUST stay numerically identical (they are
        // the same TGS normal+friction+restitution step at two widths). The plain-
        // float ScalarRef oracle in PhysicsSimdSolverTest.cpp is the third copy and
        // the bit-match gate -- change one, change all three.
        //
        // The math mirrors the scalar SoftStep TGS step EXACTLY (per lane):
        //   WarmStart        <-> SoftStep.cpp Overflow{WarmStart} normal+tangent apply
        //   SolveNormalAndFriction(useBias) <-> Overflow{Solve} (normal solve w/ the
        //                      s>0 / penetration / relax bias branch + the friction
        //                      Coulomb-cone clamp + the per-substep separation re-eval)
        //   ApplyRestitution <-> Overflow{Restitution} (rebound for points that
        //                      approached fast enough AND took a normal impulse)
        // 2D cross helpers inline: CrossWR(w,r) = (-w*r.y, w*r.x);
        // CrossRP(r,p) = r.x*p.y - r.y*p.x; Dot(a,b) = a.x*b.x + a.y*b.y.
        //
        // FP NOTE: the wrapper's plain `* + -` operators round per-op (only
        // mul_add/mul_sub fuse under /fp:strict). The scalar SoftStep.cpp uses
        // plain `*`/`+` everywhere (no std::fma), so to BIT-MATCH the scalar oracle
        // these passes ALSO use plain operators -- NOT mul_add -- for the impulse
        // accumulation. (mul_add would fuse and diverge from the scalar reference,
        // breaking lane-width invariance against the forced-scalar oracle.) The
        // scalar backend's plain operators are exactly C++ float arithmetic, so the
        // 1-wide path reproduces SoftStep.cpp's scalar result bit-for-bit, and the
        // 8-wide path differs only by the same per-lane float rounding the scalar
        // path has -> lane-width invariance holds to <1e-5.

        namespace SimdSolve
        {
            using namespace Arcane::Simd;

            // Turn a 1.0f/0.0f float mask lane array into a b32w predicate.
            ARCANE_SIMD_INLINE b32w MaskOf(const float* maskArr) noexcept
            {
                return cmp_gt(load(maskArr), setzero());
            }

            // Gather a body-state array (vx/vy/w/dpx/dpy/dq) by an i32w slot index.
            ARCANE_SIMD_INLINE f32w GatherBody(const std::vector<float>& arr,
                                               i32w idx) noexcept
            {
                return gather(arr.data(), idx);
            }
            ARCANE_SIMD_INLINE void ScatterBody(std::vector<float>& arr, i32w idx,
                                                f32w v) noexcept
            {
                scatter(arr.data(), idx, v);
            }

            // ---- WarmStart: apply accumulated n*nImp + t*tImp to body vels -----
            // Lane-wide TGS warm-start; the scalar twin is SoftStep::OverflowWarmStart
            // (SoftStep.cpp). LOCKSTEP -- keep identical lane-for-scalar.
            inline void WarmStart(std::vector<ContactConstraintSimd>& batches,
                                  BodyStateSoA& bs)
            {
                for (ContactConstraintSimd& cc : batches)
                {
                    const i32w ia = iload(cc.bodyIndexA);
                    const i32w ib = iload(cc.bodyIndexB);
                    const b32w dyn = MaskOf(cc.dynB);

                    const f32w nx = load(cc.normalX);
                    const f32w ny = load(cc.normalY);
                    const f32w tx = -ny;            // tangent = (-n.y, n.x)
                    const f32w ty = nx;

                    const f32w iMa = load(cc.invMassA);
                    const f32w iIa = load(cc.invInertiaA);
                    const f32w iMb = load(cc.invMassB);
                    const f32w iIb = load(cc.invInertiaB);

                    f32w vAx = GatherBody(bs.vx, ia), vAy = GatherBody(bs.vy, ia);
                    f32w wA  = GatherBody(bs.w,  ia);
                    f32w vBx = GatherBody(bs.vx, ib), vBy = GatherBody(bs.vy, ib);
                    f32w wB  = GatherBody(bs.w,  ib);

                    for (int p = 0; p < 2; ++p)
                    {
                        const ContactConstraintSimd::Point& pt = cc.points[p];
                        const f32w nImp = load(pt.normalImpulse);
                        const f32w tImp = load(pt.tangentImpulse);
                        const f32w rAx = load(pt.anchorAx), rAy = load(pt.anchorAy);
                        const f32w rBx = load(pt.anchorBx), rBy = load(pt.anchorBy);

                        // P = n*nImp + tangent*tImp (an inactive point has both
                        // impulses 0 -> contributes nothing, no mask needed).
                        const f32w Px = nx * nImp + tx * tImp;
                        const f32w Py = ny * nImp + ty * tImp;

                        // A += P*iMa ; wA += iIa*(rA x P)
                        vAx = vAx + Px * iMa;
                        vAy = vAy + Py * iMa;
                        wA  = wA  + iIa * (rAx * Py - rAy * Px);
                        // B -= P*iMb ; wB -= iIb*(rB x P)  (gated by dynB)
                        vBx = vBx - Px * iMb;
                        vBy = vBy - Py * iMb;
                        wB  = wB  - iIb * (rBx * Py - rBy * Px);
                    }

                    ScatterBody(bs.vx, ia, vAx);
                    ScatterBody(bs.vy, ia, vAy);
                    ScatterBody(bs.w,  ia, wA);
                    // B writes only where dynB; non-dyn lanes write back the
                    // unchanged gathered value (-> the dummy slot, harmless).
                    ScatterBody(bs.vx, ib, select(dyn, vBx, GatherBody(bs.vx, ib)));
                    ScatterBody(bs.vy, ib, select(dyn, vBy, GatherBody(bs.vy, ib)));
                    ScatterBody(bs.w,  ib, select(dyn, wB,  GatherBody(bs.w,  ib)));
                }
            }

            // ---- Normal + friction solve --------------------------------------
            // Lane-wide TGS normal+friction; the scalar twin is SoftStep::OverflowSolve
            // (SoftStep.cpp). LOCKSTEP -- keep identical lane-for-scalar.
            // invH = 1/h, maxBiasVel = world.ContactPushMaxVelocity().
            inline void SolveNormalAndFriction(std::vector<ContactConstraintSimd>& batches,
                                               BodyStateSoA& bs, float h, bool useBias,
                                               float maxBiasVel)
            {
                const f32w invH = (h > 0.0f) ? splat(1.0f / h) : setzero();
                const f32w vMaxNeg = splat(-maxBiasVel);
                const f32w one = splat(1.0f);
                const f32w zero = setzero();

                for (ContactConstraintSimd& cc : batches)
                {
                    const i32w ia = iload(cc.bodyIndexA);
                    const i32w ib = iload(cc.bodyIndexB);
                    const b32w dyn = MaskOf(cc.dynB);

                    const f32w nx = load(cc.normalX);
                    const f32w ny = load(cc.normalY);
                    const f32w tx = -ny;
                    const f32w ty = nx;

                    const f32w iMa = load(cc.invMassA);
                    const f32w iIa = load(cc.invInertiaA);
                    const f32w iMb = load(cc.invMassB);
                    const f32w iIb = load(cc.invInertiaB);

                    const f32w ccBias = load(cc.biasRate);
                    const f32w ccMassScale = load(cc.massScale);
                    const f32w ccImpScale = load(cc.impulseScale);
                    const f32w ccFriction = load(cc.friction);

                    f32w vAx = GatherBody(bs.vx, ia), vAy = GatherBody(bs.vy, ia);
                    f32w wA  = GatherBody(bs.w,  ia);
                    f32w vBx = GatherBody(bs.vx, ib), vBy = GatherBody(bs.vy, ib);
                    f32w wB  = GatherBody(bs.w,  ib);

                    const f32w dpAx = GatherBody(bs.dpx, ia), dpAy = GatherBody(bs.dpy, ia);
                    const f32w drA  = GatherBody(bs.dq,  ia);
                    const f32w dpBx = GatherBody(bs.dpx, ib), dpBy = GatherBody(bs.dpy, ib);
                    const f32w drB  = GatherBody(bs.dq,  ib);

                    // ---- normal solve (per point) -----------------------------
                    for (int p = 0; p < 2; ++p)
                    {
                        ContactConstraintSimd::Point& pt = cc.points[p];
                        const f32w rAx = load(pt.anchorAx), rAy = load(pt.anchorAy);
                        const f32w rBx = load(pt.anchorBx), rBy = load(pt.anchorBy);
                        const f32w baseSep = load(pt.baseSep);
                        const f32w nMass = load(pt.normalMass);
                        f32w nImp = load(pt.normalImpulse);

                        // s = baseSep + dot((dpA + drA x rA) - (dpB + drB x rB), n).
                        // CrossWR(dr, r) = (-dr*r.y, dr*r.x).
                        const f32w prAx = dpAx + (-drA * rAy);
                        const f32w prAy = dpAy + ( drA * rAx);
                        const f32w prBx = dpBx + (-drB * rBy);
                        const f32w prBy = dpBy + ( drB * rBx);
                        const f32w s = baseSep + ((prAx - prBx) * nx + (prAy - prBy) * ny);

                        // bias / massScale / impulseScale branch (SoftStep:383-400).
                        //   s > 0  : speculative gap -> bias = s*invH, scale 1/0
                        //   s <= 0 & useBias : bias = max(ccBias*s, -maxBiasVel),
                        //                      massScale/impulseScale from contact
                        //   s <= 0 & !useBias: bias 0, scale 1/0 (relax)
                        const b32w gap = cmp_gt(s, zero);
                        const f32w biasGap = s * invH;
                        const f32w biasPen = max(ccBias * s, vMaxNeg);
                        f32w bias, massScale, impScale;
                        if (useBias)
                        {
                            bias      = select(gap, biasGap, biasPen);
                            massScale = select(gap, one, ccMassScale);
                            impScale  = select(gap, zero, ccImpScale);
                        }
                        else
                        {
                            // Relax pass: penetration branch is bias 0, scale 1/0.
                            bias      = select(gap, biasGap, zero);
                            massScale = one;
                            impScale  = zero;
                        }

                        // vn = dot((vA + wA x rA) - (vB + wB x rB), n).
                        const f32w dvx = (vAx + (-wA * rAy)) - (vBx + (-wB * rBy));
                        const f32w dvy = (vAy + ( wA * rAx)) - (vBy + ( wB * rBx));
                        const f32w vn = dvx * nx + dvy * ny;

                        // impulse = -nMass*massScale*(vn+bias) - impScale*nImp.
                        f32w impulse = -nMass * massScale * (vn + bias) - impScale * nImp;
                        // accumulated clamp >= 0.
                        const f32w newI = max(nImp + impulse, zero);
                        impulse = newI - nImp;
                        nImp = newI;
                        store(pt.normalImpulse, nImp);

                        // P = n*impulse ; apply to A (+) and B (-, gated).
                        const f32w Px = nx * impulse, Py = ny * impulse;
                        vAx = vAx + Px * iMa;
                        vAy = vAy + Py * iMa;
                        wA  = wA  + iIa * (rAx * Py - rAy * Px);
                        vBx = vBx - Px * iMb;
                        vBy = vBy - Py * iMb;
                        wB  = wB  - iIb * (rBx * Py - rBy * Px);
                    }

                    // ---- friction solve (per point) ---------------------------
                    for (int p = 0; p < 2; ++p)
                    {
                        ContactConstraintSimd::Point& pt = cc.points[p];
                        const f32w rAx = load(pt.anchorAx), rAy = load(pt.anchorAy);
                        const f32w rBx = load(pt.anchorBx), rBy = load(pt.anchorBy);
                        const f32w tMass = load(pt.tangentMass);
                        const f32w nImp = load(pt.normalImpulse);
                        f32w tImp = load(pt.tangentImpulse);

                        const f32w dvx = (vAx + (-wA * rAy)) - (vBx + (-wB * rBy));
                        const f32w dvy = (vAy + ( wA * rAx)) - (vBy + ( wB * rBx));
                        const f32w vt = dvx * tx + dvy * ty;

                        f32w impulse = -tMass * vt;

                        // clamp to [-friction*nImp, +friction*nImp] (Coulomb cone).
                        const f32w maxFric = ccFriction * nImp;
                        const f32w cand = tImp + impulse;
                        const f32w newI = max(-maxFric, min(cand, maxFric));
                        impulse = newI - tImp;
                        tImp = newI;
                        store(pt.tangentImpulse, tImp);

                        const f32w Px = tx * impulse, Py = ty * impulse;
                        vAx = vAx + Px * iMa;
                        vAy = vAy + Py * iMa;
                        wA  = wA  + iIa * (rAx * Py - rAy * Px);
                        vBx = vBx - Px * iMb;
                        vBy = vBy - Py * iMb;
                        wB  = wB  - iIb * (rBx * Py - rBy * Px);
                    }

                    ScatterBody(bs.vx, ia, vAx);
                    ScatterBody(bs.vy, ia, vAy);
                    ScatterBody(bs.w,  ia, wA);
                    ScatterBody(bs.vx, ib, select(dyn, vBx, GatherBody(bs.vx, ib)));
                    ScatterBody(bs.vy, ib, select(dyn, vBy, GatherBody(bs.vy, ib)));
                    ScatterBody(bs.w,  ib, select(dyn, wB,  GatherBody(bs.w,  ib)));
                }
            }

            // ---- Restitution ---------------------------------------------------
            // Lane-wide TGS restitution; the scalar twin is SoftStep::OverflowRestitution
            // (SoftStep.cpp). LOCKSTEP -- keep identical lane-for-scalar.
            // A whole contact with restitution <= 0 is skipped scalar-side; here we
            // mask it lane-wise (restitution<=0 -> no rebound impulse). A point only
            // rebounds when relVel <= -threshold AND nImp > 0.
            inline void ApplyRestitution(std::vector<ContactConstraintSimd>& batches,
                                         BodyStateSoA& bs, float threshold)
            {
                const f32w negThresh = splat(-threshold);
                const f32w zero = setzero();

                for (ContactConstraintSimd& cc : batches)
                {
                    const i32w ia = iload(cc.bodyIndexA);
                    const i32w ib = iload(cc.bodyIndexB);
                    const b32w dyn = MaskOf(cc.dynB);

                    const f32w rest = load(cc.restitution);
                    const b32w ccHasRest = cmp_gt(rest, zero);
                    // SoftStep skips a contact with restitution<=0 entirely; if NO
                    // lane in this batch has restitution, the whole batch is a no-op.
                    if (none(ccHasRest))
                    {
                        continue;
                    }

                    const f32w nx = load(cc.normalX);
                    const f32w ny = load(cc.normalY);
                    const f32w iMa = load(cc.invMassA);
                    const f32w iIa = load(cc.invInertiaA);
                    const f32w iMb = load(cc.invMassB);
                    const f32w iIb = load(cc.invInertiaB);

                    f32w vAx = GatherBody(bs.vx, ia), vAy = GatherBody(bs.vy, ia);
                    f32w wA  = GatherBody(bs.w,  ia);
                    f32w vBx = GatherBody(bs.vx, ib), vBy = GatherBody(bs.vy, ib);
                    f32w wB  = GatherBody(bs.w,  ib);

                    for (int p = 0; p < 2; ++p)
                    {
                        ContactConstraintSimd::Point& pt = cc.points[p];
                        const f32w rAx = load(pt.anchorAx), rAy = load(pt.anchorAy);
                        const f32w rBx = load(pt.anchorBx), rBy = load(pt.anchorBy);
                        const f32w nMass = load(pt.normalMass);
                        const f32w relVel = load(pt.relVel);
                        f32w nImp = load(pt.normalImpulse);

                        // Active lane: ccHasRest AND relVel <= -threshold AND nImp>0.
                        const b32w fast = cmp_le(relVel, negThresh);
                        const b32w took = cmp_gt(nImp, zero);
                        // combine masks: ccHasRest & fast & took. b32w has no & op,
                        // so AND via select (true-lane keeps next mask, else false).
                        const f32w fastF = select(fast, splat(1.0f), zero);
                        const f32w tookF = select(took, splat(1.0f), zero);
                        const f32w restF = select(ccHasRest, splat(1.0f), zero);
                        const b32w active = cmp_gt(restF * fastF * tookF, zero);

                        const f32w dvx = (vAx + (-wA * rAy)) - (vBx + (-wB * rBy));
                        const f32w dvy = (vAy + ( wA * rAx)) - (vBy + ( wB * rBx));
                        const f32w vn = dvx * nx + dvy * ny;

                        // impulse = -nMass*(vn + restitution*relVel); accumulated >=0.
                        f32w impulse = -nMass * (vn + rest * relVel);
                        const f32w newI = max(nImp + impulse, zero);
                        impulse = newI - nImp;
                        // gate the impulse to active lanes only (inactive -> 0).
                        impulse = select(active, impulse, zero);
                        // nImp only updates on active lanes (matches scalar skip).
                        store(pt.normalImpulse, select(active, newI, nImp));

                        const f32w Px = nx * impulse, Py = ny * impulse;
                        vAx = vAx + Px * iMa;
                        vAy = vAy + Py * iMa;
                        wA  = wA  + iIa * (rAx * Py - rAy * Px);
                        vBx = vBx - Px * iMb;
                        vBy = vBy - Py * iMb;
                        wB  = wB  - iIb * (rBx * Py - rBy * Px);
                    }

                    ScatterBody(bs.vx, ia, vAx);
                    ScatterBody(bs.vy, ia, vAy);
                    ScatterBody(bs.w,  ia, wA);
                    ScatterBody(bs.vx, ib, select(dyn, vBx, GatherBody(bs.vx, ib)));
                    ScatterBody(bs.vy, ib, select(dyn, vBy, GatherBody(bs.vy, ib)));
                    ScatterBody(bs.w,  ib, select(dyn, wB,  GatherBody(bs.w,  ib)));
                }
            }

            // ---- Store accumulated impulses back onto ctx.contacts (Hazard 3) --
            // After the solve, the converged impulses live in the SoA batches. Copy
            // them back into ctx.contacts[ref].points[p].normalImpulse/tangentImpulse
            // so the world's stage-3b pool write-back persists warm-start. Keyed by
            // the color's refs (same order Build packed them: lane L of batch b is
            // global index b*kWidth+L = refs index, EXCLUDING padding lanes).
            inline void StoreImpulses(const std::vector<ContactConstraintSimd>& batches,
                                      ContactConstraint* ccs, const std::uint32_t* refs)
            {
                constexpr int W = ContactConstraintSimd::kWidth;
                for (std::size_t b = 0; b < batches.size(); ++b)
                {
                    const ContactConstraintSimd& batch = batches[b];
                    for (int L = 0; L < batch.count; ++L)   // live lanes only
                    {
                        const std::uint32_t ref = refs[b * W + static_cast<std::size_t>(L)];
                        ContactConstraint& cc = ccs[ref];
                        for (int p = 0; p < cc.pointCount; ++p)
                        {
                            cc.points[p].normalImpulse =
                                static_cast<Real>(batch.points[p].normalImpulse[L]);
                            cc.points[p].tangentImpulse =
                                static_cast<Real>(batch.points[p].tangentImpulse[L]);
                        }
                    }
                }
            }
        } // namespace SimdSolve

    } // namespace Physics
} // namespace Arcane
