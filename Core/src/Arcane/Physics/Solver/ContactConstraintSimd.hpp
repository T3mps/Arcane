#pragma once

// ContactConstraintSimd.hpp -- the SoA contact-constraint BATCH + its packer.
//
// PURE DATA + PACKER. This header holds NO solve math: it is the Structure-of-
// Arrays staging buffer the lane-wide TGS-Soft solve (a later task, SoftStep)
// reads, plus the deterministic packer (`Build`) that fills it from a color's
// emitted `ContactConstraint`s. The actual `f32w` load / gather / select / solve
// / scatter lives in SoftStep -- this file is just the transposed layout.
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
// SCOPE (Task 4): the struct + Build + its unit tests. NOTHING consumes a batch
// yet (Task 5 wires it into the SoftStep lane-wide solve). Build is a pure
// packer: it COPIES field values out of the source ContactConstraint/...Point
// (it does NOT compute effective masses or soft coefficients -- those are either
// already filled on the source cc or computed lane-wide at solve time).
//
// PRESENTATION-FREE + C++20-clean: std + the Simd width constant + Solver.hpp
// (for ContactConstraint). No SDL3/NVRHI/Batcher2D. namespace Arcane::Physics.

#include <cstdint>
#include <vector>

#include <Arcane/Math/Simd.hpp>              // Arcane::Simd::f32w::width
#include <Arcane/Physics/Solver/Solver.hpp>  // ContactConstraint / ...Point

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
            // invalid + impulses 0, bodyIndex 0). It COPIES values only -- it
            // does not compute effective masses or soft coefficients.
            //
            // refs may be null iff count == 0.
            static std::vector<ContactConstraintSimd>
            Build(const ContactConstraint* ccs, const std::uint32_t* refs, int count)
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
                            PackLane(dst, L, ccs[refs[g]]);
                        }
                        else
                        {
                            PadLane(dst, L);
                        }
                    }
                }

                return batches;
            }

        private:
            // Copy one source ContactConstraint into lane L (the live path).
            static void PackLane(ContactConstraintSimd& dst, int L,
                                 const ContactConstraint& cc)
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

                // dynB: B's velocity is mutated only if B is a real dynamic body.
                // A static/kinematic body (invMassB == 0) or a tile span
                // (bodyBIsBody == false) is read-only -> mask off (no scatter).
                const bool dyn = cc.bodyBIsBody && cc.invMassB > Real(0);
                dst.dynB[L]         = dyn ? 1.0f : 0.0f;

                // bodyIndexB must be an in-range gather slot on EVERY lane (the
                // T5 read of B's velocity is an UNMASKED gather). For read-only B
                // (dynB==0: static/kinematic body OR a tile span whose
                // cc.bodyB == kInvalidSlot casts to -1) clamp to 0 -- slot 0
                // always exists if any body exists, and the gathered value is
                // discarded by the dynB mask anyway. Mirrors the padding-lane
                // rationale. A is always dynamic in the solver feed, so bodyIndexA
                // needs no such clamp.
                dst.bodyIndexB[L]   = dyn ? static_cast<std::int32_t>(cc.bodyB) : 0;

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
            // invalid, bodyIndex a safe in-range 0 (the gather must hit a valid
            // slot; the masked impulse is 0 so the scatter writes nothing).
            static void PadLane(ContactConstraintSimd& dst, int L)
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
                dst.bodyIndexA[L]   = 0;
                dst.bodyIndexB[L]   = 0;
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

    } // namespace Physics
} // namespace Arcane
