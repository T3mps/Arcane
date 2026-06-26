// SoftStep.cpp -- Box2D-v3 TGS Soft constraint solver (M6, Task P2.2).
//
// See SoftStep.hpp for the algorithm overview + the PORT-vs-MODERNIZE framing.
// This TU implements the ISolver phases + the whole-Step Solve() driver. The
// soft coefficients follow Box2D v3 b2MakeSoft; the contact solve follows the
// v3 b2SolveContactsTask normal+friction form (separation re-evaluated each
// sub-step from per-body position deltas).
//
// PRESENTATION-FREE + C++20-clean: glm::vec2 + std + sibling Physics headers.

#include <Arcane/Physics/Solver/SoftStep.hpp>

#include <algorithm>
#include <cmath>

#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Joints/Joint.hpp>    // Joint (Prepare/SolveVelocity)
#include <Arcane/Physics/Solver/SoftCoeffs.hpp>   // shared MakeSoft + SoftCoeffs
#include <Arcane/Physics/Solver/BodyStateSoA.hpp> // SyncIn/SyncOut defs (SIMD solver Task 1)

namespace Arcane
{
    namespace Physics
    {
        namespace
        {
            // 2D cross products (port of the Lua applyAt / contact math).
            //   scalar(w) x vec(r) -> vec : (-w*r.y, w*r.x)
            //   vec(r)    x vec(p) -> scalar : r.x*p.y - r.y*p.x
            inline Vec2 CrossWR(Real w, const Vec2& r) noexcept
            {
                return Vec2(-w * r.y, w * r.x);
            }
            inline Real CrossRP(const Vec2& r, const Vec2& p) noexcept
            {
                return r.x * p.y - r.y * p.x;
            }
            inline Real Dot(const Vec2& a, const Vec2& b) noexcept
            {
                return a.x * b.x + a.y * b.y;
            }
        } // namespace

        void SoftStep::DropBody(std::uint32_t slot)
        {
            // Warm-start impulses now live on the persistent Contact, which the
            // broadphase destroys when a fixture/body is removed
            // (PhysicsWorld::DestroyContactsForBody/Fixture), so a recycled slot
            // inherits no stale impulse. The per-body sub-step scratch is re-zeroed
            // at the start of each Solve. Nothing slot-specific to do here; the
            // override stays only to honor the ISolver contract.
            (void)slot;
        }

        // ===================================================================
        // BodyStateSoA world<->solver sync (SIMD constraint-solver Task 1;
        // dense re-home Phase C, Task 2)
        // ===================================================================
        //
        // TWO INDEX SPACES live here. The SOLVER hot path uses the DENSE pair
        // SyncInCompacted / SyncOutCompacted (Phase C, Task 2): the scratch is sized
        // solverCount+1 (= AwakeCount()+KinematicCount()+1) and indexed by solverIndex
        // (awake dynamics at AwakeIndexOf(slot) in [0,awakeCount); kinematics at
        // awakeCount+KinematicIndexOf(slot); the dummy tail at solverCount). The
        // legacy SyncIn / SyncOut pair (below) is the original sparse world-slot
        // bridge, kept only for the standalone SyncIn/SyncOut round-trip contract
        // test -- it is NO LONGER on the solver path. Both pairs share the same
        // awake-dynamic predicate (Alive && Dynamic && Awake), differing only in the
        // index they write/read. These defs live here (not the header) because they
        // need the PhysicsWorld slot accessors.
        //
        // CONTRACT (legacy SyncIn): the caller Resize()s the SoA to world.Count()+1
        // (which zeroes every array; the "+1" tail is the scatter-safe dummy slot)
        // before SyncIn. SyncIn then OVERWRITES only the slots that satisfy the
        // awake-dynamic predicate. Non-matching slots (statics, kinematics, asleep/
        // dead dynamics) are LEFT AS-IS (zero after the caller's Resize). For matched
        // slots SyncIn also zeroes the TGS position-delta accumulators (dp/dq).
        // SyncIn additionally copies all Alive non-dynamic (Static/Kinematic) slots
        // so the solver's B-endpoint velocity reads see current values.

        void BodyStateSoA::SyncIn(const PhysicsWorld& world)
        {
            // Two-pass design (Phase B, Task 3):
            //
            // (a) Awake dynamics: mirror velocity + reset the TGS position-delta
            //     accumulators. Only awake dynamics are integrated, so only they need
            //     a fresh dp/dq=0 at the start of the sub-step loop.
            world.ForEachAwake([&](std::uint32_t i)
            {
                const Vec2 v = world.VelSlot(i);
                vx[i] = v.x;
                vy[i] = v.y;
                w[i]  = world.AngVelSlot(i);
                dpx[i] = 0.f;
                dpy[i] = 0.f;
                dq[i]  = 0.f;
            });

            // (b) Static / kinematic slots are gathered as contact B-endpoints, so
            //     their velocity MUST stay correct in the SoA every step (statics = 0;
            //     kinematics = their set velocity). This also handles the recycled-slot
            //     hole: a slot that was a moving Dynamic, removed, then recycled as a
            //     Static would keep a stale non-zero velocity in the SoA if SyncIn
            //     never copies it. Non-dynamics do not integrate, so dp/dq stay zero
            //     (they arrive zero from the caller's Resize; we never touch them).
            //
            //     Sleeping dynamics are visited by NEITHER pass -- safe because no
            //     emitted constraint references a sleeping dynamic: EmitContactConstraints
            //     gates on awake-A, and a touching dynamic-dynamic pair shares one
            //     island (islands sleep as a unit), so awake-A implies awake-B.
            //     This is asserted in EmitContactConstraints (Debug builds only).
            const std::uint32_t count = world.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!world.Alive(i) || world.TypeSlot(i) == BodyType::Dynamic)
                {
                    continue; // dead or dynamic (handled by ForEachAwake above)
                }
                const Vec2 v = world.VelSlot(i);
                vx[i] = v.x;
                vy[i] = v.y;
                w[i]  = world.AngVelSlot(i);
                // dp/dq intentionally left at zero (non-dynamics never integrate).
            }
        }

        void BodyStateSoA::SyncOut(PhysicsWorld& world) const
        {
            // Phase B, Task 3: iterate the awake-set directly -- it IS the gate.
            // Only awake dynamics were SyncIn'd and integrated; only they get written
            // back. Non-dynamics and sleeping dynamics are never disturbed.
            world.ForEachAwake([&](std::uint32_t i)
            {
                world.SetVelSlot(i, Vec2(vx[i], vy[i]));
                world.SetAngVelSlot(i, w[i]);
            });
        }

        void BodyStateSoA::SyncInCompacted(const PhysicsWorld& world)
        {
            // DENSE fill (Phase C, Task 2). The caller Resize()d to solverCount+1, so
            // every row -- including the dummy tail at solverCount -- starts at zero.
            //
            // (a) Awake dynamics -> dense row AwakeIndexOf(slot) in [0, awakeCount):
            //     mirror velocity + reset the TGS position-delta accumulators (only
            //     awake dynamics integrate, so only they need a fresh dp/dq = 0).
            world.ForEachAwake([&](std::uint32_t s)
            {
                const std::uint32_t i = world.AwakeIndexOf(s);
                const Vec2 v = world.VelSlot(s);
                vx[i] = v.x;
                vy[i] = v.y;
                w[i]  = world.AngVelSlot(s);
                dpx[i] = 0.f;
                dpy[i] = 0.f;
                dq[i]  = 0.f;
            });

            // (b) Kinematics -> dense row awakeCount + KinematicIndexOf(slot) in
            //     [awakeCount, solverCount): mirror velocity ONLY. A kinematic B is a
            //     contact endpoint whose authored velocity drives the relative-velocity
            //     push term, so its dense row MUST carry the current velocity. dp/dq
            //     stay at zero (kinematics never integrate -> the Resize zero stands).
            //     Sleeping dynamics get NO row -- safe because no emitted constraint
            //     references one (EmitContactConstraints gates on awake-A, and a
            //     touching dyn-dyn pair shares one island, so awake-A => awake-B).
            //     Statics likewise get no row (they map to the zero dummy tail).
            const std::uint32_t awakeCount = world.AwakeCount();
            world.ForEachKinematic([&](std::uint32_t s)
            {
                const std::uint32_t i = awakeCount + world.KinematicIndexOf(s);
                const Vec2 v = world.VelSlot(s);
                vx[i] = v.x;
                vy[i] = v.y;
                w[i]  = world.AngVelSlot(s);
                // dp/dq intentionally left at zero (kinematics never integrate).
            });
        }

        void BodyStateSoA::SyncOutCompacted(PhysicsWorld& world) const
        {
            // DENSE write-back (Phase C, Task 2). Read each awake dynamic's velocity
            // from its dense row AwakeIndexOf(slot) and push it to the world. Only
            // awake dynamics were integrated; kinematics are read-only (their dense
            // rows are never written back), and statics/sleeping dynamics have no row.
            world.ForEachAwake([&](std::uint32_t s)
            {
                const std::uint32_t i = world.AwakeIndexOf(s);
                world.SetVelSlot(s, Vec2(vx[i], vy[i]));
                world.SetAngVelSlot(s, w[i]);
            });
        }

        // ===================================================================
        // Prepare
        // ===================================================================

        void SoftStep::PrepareContacts(SolverContext& ctx)
        {
            PhysicsWorld& w = *ctx.world;

            // High contact hertz that, with the small sub-step, appears rigid --
            // but never out-running the sub-step solve rate (Box2D v3 clamps to
            // 0.25 * substepCount / dt == 0.25 / h).
            const Real h = ctx.subDt;
            const Real maxHertz = (h > Real(0)) ? (Real(0.25) / h) : w.ContactHertz();
            const Real contactHertz = std::min(w.ContactHertz(), maxHertz);
            const SoftCoeffs contactSoft = MakeSoft(contactHertz, w.ContactDampingRatio(), h);

            for (std::uint32_t c = 0; c < ctx.contactCount; ++c)
            {
                ContactConstraint& cc = ctx.contacts[c];

                cc.biasRate     = contactSoft.biasRate;
                cc.massScale    = contactSoft.massScale;
                cc.impulseScale = contactSoft.impulseScale;

                const Vec2 n = cc.normal;
                const Vec2 tangent(-n.y, n.x);

                const Real iMa = cc.invMassA, iIa = cc.invInertiaA;
                const Real iMb = cc.invMassB, iIb = cc.invInertiaB;

                const Vec2 vA = w.VelSlot(cc.bodyA);
                const Real wA = w.AngVelSlot(cc.bodyA);
                Vec2 vB(Real(0), Real(0));
                Real wB = Real(0);
                if (cc.bodyBIsBody)
                {
                    vB = w.VelSlot(cc.bodyB);
                    wB = w.AngVelSlot(cc.bodyB);
                }

                for (int p = 0; p < cc.pointCount; ++p)
                {
                    ContactConstraintPoint& cp = cc.points[p];

                    const Vec2 rA = cp.anchorA;
                    const Vec2 rB = cp.anchorB;

                    // Effective normal mass: 1/(iMa+iMb + iIa*(rA x n)^2 + iIb*(rB x n)^2).
                    const Real rnA = CrossRP(rA, n);
                    const Real rnB = CrossRP(rB, n);
                    const Real kNormal = iMa + iMb + iIa * rnA * rnA + iIb * rnB * rnB;
                    cp.normalMass = (kNormal > Real(0)) ? (Real(1) / kNormal) : Real(0);

                    const Real rtA = CrossRP(rA, tangent);
                    const Real rtB = CrossRP(rB, tangent);
                    const Real kTangent = iMa + iMb + iIa * rtA * rtA + iIb * rtB * rtB;
                    cp.tangentMass = (kTangent > Real(0)) ? (Real(1) / kTangent) : Real(0);

                    // Relative normal velocity at the contact (for restitution),
                    // measured as A-relative-to-B along the B->A normal. When A
                    // approaches B this is NEGATIVE (the closing speed).
                    const Vec2 dv = (vA + CrossWR(wA, rA)) - (vB + CrossWR(wB, rB));
                    cp.relativeVelocity = Dot(dv, n);

                    // Warm start: the accumulators ARRIVE already seeded on cp
                    // (EmitContactConstraints copied them off the persistent
                    // Contact's manifold point; 0 for a fresh feature or a
                    // transient tile span). Prepare must NOT reset them -- doing so
                    // would discard the carried-forward impulse history.
                }
            }
        }

        void SoftStep::PrepareJoints(SolverContext& ctx)
        {
            // Prepare each joint with the SUB-STEP dt so its Baumgarte bias
            // (BETA/subDt) is per-substep-correct: the bias is applied once per
            // sub-step's velocity solve, summed over substepCount sub-steps. This
            // is the "soft-constraint formulation" the plan means -- sub-stepping
            // softens the Baumgarte joints. The joints read the committed (start-
            // of-step) positions; SoftStep tracks in-flight motion in deltaPos/
            // deltaRot (committed only at FinalizePositions), so a single Prepare
            // with subDt is correct (re-Preparing each sub-step would recompute
            // the same start-relative bias). Index-ordered (determinism).
            PhysicsWorld& w = *ctx.world;
            for (std::uint32_t k = 0; k < ctx.jointCount; ++k)
            {
                if (ctx.joints[k].joint != nullptr)
                {
                    ctx.joints[k].joint->Prepare(w, ctx.subDt);
                }
            }
        }

        void SoftStep::SolveJoints(SolverContext& ctx)
        {
            // One velocity-constraint pass over the joints (per sub-step). Joints
            // solve alongside contacts in the sub-step velocity solve. Index-
            // ordered (determinism).
            PhysicsWorld& w = *ctx.world;
            for (std::uint32_t k = 0; k < ctx.jointCount; ++k)
            {
                if (ctx.joints[k].joint != nullptr)
                {
                    ctx.joints[k].joint->SolveVelocity(w);
                }
            }
        }

        // ===================================================================
        // SIMD lane-wide solve helpers (Part 1) -- integrate / bridge / overflow
        // ===================================================================
        //
        // These operate on m_bodyState (the packed SoA the lane-wide passes
        // gather/scatter through), NOT the world velocity SoA. The world<->SoA
        // sync happens at the Step boundary (SyncIn/SyncOut) + around joint passes
        // (SyncVelTo/FromWorld). The integrate loops stay scalar O(n) (not the hot
        // path); the WIN is the lane-wide colored contact solve.

        void SoftStep::IntegrateVelocitiesSoA(SolverContext& ctx, Real h)
        {
            // Phase B, Task 3: iterate the awake-set directly -- the set guarantees
            // Alive + Dynamic + Awake. Only the InvMassSlot<=0 guard is retained
            // (a degenerate zero-invMass dynamic is valid; it never integrates).
            // Phase C, Task 2: index m_bodyState by the DENSE solverIndex
            // AwakeIndexOf(slot), NOT the world slot.
            PhysicsWorld& w = *ctx.world;
            const Vec2 g = ctx.gravity;
            w.ForEachAwake([&](std::uint32_t s)
            {
                if (w.InvMassSlot(s) <= Real(0))
                {
                    return; // pinned dynamic -- never integrates
                }
                const std::uint32_t i = w.AwakeIndexOf(s);
                float vx = m_bodyState.vx[i] + static_cast<float>(g.x * h);
                float vy = m_bodyState.vy[i] + static_cast<float>(g.y * h);
                float wv = m_bodyState.w[i];
                const Real d = w.LinDampSlot(s);
                if (d > Real(0))
                {
                    const float f = static_cast<float>(Real(1) / (Real(1) + d * h));
                    vx *= f; vy *= f; wv *= f;
                }
                m_bodyState.vx[i] = vx;
                m_bodyState.vy[i] = vy;
                m_bodyState.w[i]  = wv;
            });
        }

        void SoftStep::IntegratePositionsSoA(SolverContext& ctx, Real h)
        {
            // Phase B, Task 3: iterate the awake-set directly -- Alive+Dynamic+Awake
            // is guaranteed. dp/dq += v*h for the TGS separation re-evaluation.
            // Phase C, Task 2: index by the DENSE solverIndex AwakeIndexOf(slot).
            PhysicsWorld& w = *ctx.world;
            const float fh = static_cast<float>(h);
            w.ForEachAwake([&](std::uint32_t s)
            {
                const std::uint32_t i = w.AwakeIndexOf(s);
                m_bodyState.dpx[i] += m_bodyState.vx[i] * fh;
                m_bodyState.dpy[i] += m_bodyState.vy[i] * fh;
                m_bodyState.dq[i]  += m_bodyState.w[i]  * fh;
            });
        }

        void SoftStep::FinalizePositionsSoA(SolverContext& ctx)
        {
            // Phase B, Task 3: iterate the awake-set directly -- Alive+Dynamic+Awake
            // is guaranteed. Commit the compound-COM position from SoA dp/dq.
            // For localCenter==(0,0) this reduces to p=p0+dp, a=a0+dq.
            // Phase C, Task 2: index dp/dq by the DENSE solverIndex AwakeIndexOf(slot);
            // CommitSlotPosition still takes the WORLD slot `s` (unchanged).
            PhysicsWorld& w = *ctx.world;
            w.ForEachAwake([&](std::uint32_t s)
            {
                const std::uint32_t i = w.AwakeIndexOf(s);
                const Vec2 dp(static_cast<Real>(m_bodyState.dpx[i]),
                              static_cast<Real>(m_bodyState.dpy[i]));
                const Real dr = static_cast<Real>(m_bodyState.dq[i]);
                const Vec2 p0 = w.PosSlot(s);
                const Real a0 = w.GetAngle(w.HandleOf(s));
                const Vec2 lc = w.LocalCenterSlot(s);
                const Vec2 c0 = p0 + RotateVec(a0, lc);
                const Vec2 c  = c0 + dp;
                const Real a  = a0 + dr;
                const Vec2 p  = c - RotateVec(a, lc);
                w.CommitSlotPosition(s, p, a);
            });
        }

        void SoftStep::SyncVelToWorld(SolverContext& ctx)
        {
            // Push the SoA velocities back to the world so the scalar joint passes
            // read the in-flight (contact-solved) velocities. Awake dynamics only.
            // Phase C, Task 2: index m_bodyState by the DENSE solverIndex; iterate
            // the awake-set (it IS the Alive+Dynamic+Awake gate).
            PhysicsWorld& w = *ctx.world;
            w.ForEachAwake([&](std::uint32_t s)
            {
                const std::uint32_t i = w.AwakeIndexOf(s);
                w.SetVelSlot(s, Vec2(static_cast<Real>(m_bodyState.vx[i]),
                                     static_cast<Real>(m_bodyState.vy[i])));
                w.SetAngVelSlot(s, static_cast<Real>(m_bodyState.w[i]));
            });
        }

        void SoftStep::SyncVelFromWorld(SolverContext& ctx)
        {
            // Pull the world velocities back into the SoA after a joint pass mutated
            // them. dp/dq untouched (joints do not read them). Awake dynamics only.
            // Phase C, Task 2: index m_bodyState by the DENSE solverIndex.
            PhysicsWorld& w = *ctx.world;
            w.ForEachAwake([&](std::uint32_t s)
            {
                const std::uint32_t i = w.AwakeIndexOf(s);
                const Vec2 v = w.VelSlot(s);
                m_bodyState.vx[i] = static_cast<float>(v.x);
                m_bodyState.vy[i] = static_cast<float>(v.y);
                m_bodyState.w[i]  = static_cast<float>(w.AngVelSlot(s));
            });
        }

        // ----- Overflow: width-1 SCALAR solve over the SoA (scatter-safe) ------
        //
        // Phase C, Task 5: the overflow set is m_overflowRefs -- the emitted
        // constraints whose PERSISTENT contact color is kInvalidColor (an overflow
        // contact whose source Contact found no free color among kColorCount at
        // create -- a hub body in > kColorCount contacts -- OR a transient span with
        // no pool home). They CANNOT solve lane-wide (an overflow hub shares a body
        // with a colored contact), so we solve them one-at-a-time, scalar, over the
        // SAME BodyStateSoA -- sequential => no scatter conflict. They read/write the
        // SoA velocity + dp/dq (so overflow composes with the colored result in the
        // same velocity store). One contact per call iter.
        //
        // LOCKSTEP: this overflow trio (OverflowWarmStart / OverflowSolve /
        // OverflowRestitution) is the WIDTH-1 SEQUENTIAL version of the same TGS
        // step the lane-wide SimdSolve::WarmStart / SolveNormalAndFriction /
        // ApplyRestitution passes run (ContactConstraintSimd.hpp). The two paths
        // share one m_bodyState and compose in one velocity store, so they MUST stay
        // numerically in lockstep lane-for-scalar. Keep the math here identical to
        // the SimdSolve::* passes (and to the plain-float ScalarRef oracle in
        // PhysicsSimdSolverTest.cpp); change one and you change all three.

        namespace
        {
            // Per-overflow-constraint scalar helpers (SoA-resident). cc is the
            // emitted ContactConstraint; bs holds vel + dp/dq by DENSE solverIndex
            // (Phase C, Task 2).
            //   bIsBody : B is a real body (read its velocity for the relative-
            //             velocity term -- a kinematic plate's velocity drives the
            //             push; SyncInCompacted gives kinematics a real dense row).
            //   dynB    : B is a real DYNAMIC body whose velocity is MUTATED.
            //   ia, ib  : DENSE solverIndex of A and B (NOT world slots).
            struct OverflowBodies
            {
                std::uint32_t ia, ib;
                bool bIsBody, dynB;
                Real iMa, iIa, iMb, iIb;
            };

            // Map a contact B-endpoint world slot to its dense solverIndex, matching
            // the packer's kinematic-vs-static gate exactly: a dynamic B -> its awake
            // row; a kinematic B -> awakeCount + its kinematic row (real velocity ->
            // push); a static B or a span -> the zero dummy tail at solverCount.
            inline std::uint32_t DenseB(const ContactConstraint& cc, const PhysicsWorld& w,
                                        std::uint32_t awakeCount, std::uint32_t solverCount,
                                        bool dynB)
            {
                if (!cc.bodyBIsBody)
                {
                    return solverCount;                 // tile span -> dummy tail
                }
                if (dynB)
                {
                    return w.AwakeIndexOf(cc.bodyB);     // dynamic B -> its awake row
                }
                const std::uint32_t ki = w.KinematicIndexOf(cc.bodyB);
                if (ki != kNotKinematic)
                {
                    return awakeCount + ki;             // kinematic B -> its dense row
                }
                // static B -> dummy tail. NOTE: a non-idiomatic moving ZERO-invMass
                // *dynamic* B (BodyType::Dynamic with invMass==0) is not dynB and not
                // in the kinematic set, so it falls HERE -> the zero dummy tail; its
                // velocity is NOT gathered and its push is dropped BY DESIGN -- use a
                // Kinematic body for an infinite-mass mover.
                return solverCount;
            }

            // ia/ib are DENSE solverIndices. awakeCount/solverCount come from the
            // world's per-step dense index space; A is ALWAYS an awake dynamic.
            inline OverflowBodies OverflowSetup(const ContactConstraint& cc, const PhysicsWorld& w,
                                                std::uint32_t awakeCount, std::uint32_t solverCount)
            {
                OverflowBodies ob;
                ob.bIsBody = cc.bodyBIsBody;
                ob.dynB    = cc.bodyBIsBody && cc.invMassB > Real(0);
                ob.ia      = w.AwakeIndexOf(cc.bodyA);
                ob.ib      = DenseB(cc, w, awakeCount, solverCount, ob.dynB);
                ob.iMa     = cc.invMassA; ob.iIa = cc.invInertiaA;
                ob.iMb     = cc.invMassB; ob.iIb = cc.invInertiaB;
                return ob;
            }
        } // namespace

        void SoftStep::OverflowWarmStart(SolverContext& ctx)
        {
            const PhysicsWorld& w = *ctx.world;
            const std::uint32_t awakeCount = w.AwakeCount();
            const std::uint32_t solverCount = awakeCount + w.KinematicCount();
            for (std::uint32_t ref : m_overflowRefs)
            {
                ContactConstraint& cc = ctx.contacts[ref];
                const OverflowBodies ob = OverflowSetup(cc, w, awakeCount, solverCount);
                const Vec2 n = cc.normal;
                const Vec2 tangent(-n.y, n.x);

                Vec2 vA(m_bodyState.vx[ob.ia], m_bodyState.vy[ob.ia]);
                Real wA = m_bodyState.w[ob.ia];
                Vec2 vB(Real(0), Real(0)); Real wB = Real(0);
                if (ob.bIsBody) { vB = Vec2(m_bodyState.vx[ob.ib], m_bodyState.vy[ob.ib]); wB = m_bodyState.w[ob.ib]; }

                for (int p = 0; p < cc.pointCount; ++p)
                {
                    const ContactConstraintPoint& cp = cc.points[p];
                    const Vec2 P = n * cp.normalImpulse + tangent * cp.tangentImpulse;
                    vA += P * ob.iMa;
                    wA += ob.iIa * CrossRP(cp.anchorA, P);
                    if (ob.dynB) { vB -= P * ob.iMb; wB -= ob.iIb * CrossRP(cp.anchorB, P); }
                }

                m_bodyState.vx[ob.ia] = static_cast<float>(vA.x);
                m_bodyState.vy[ob.ia] = static_cast<float>(vA.y);
                m_bodyState.w[ob.ia]  = static_cast<float>(wA);
                if (ob.dynB)
                {
                    m_bodyState.vx[ob.ib] = static_cast<float>(vB.x);
                    m_bodyState.vy[ob.ib] = static_cast<float>(vB.y);
                    m_bodyState.w[ob.ib]  = static_cast<float>(wB);
                }
            }
        }

        void SoftStep::OverflowSolve(SolverContext& ctx, Real h, bool useBias)
        {
            const Real invH = (h > Real(0)) ? (Real(1) / h) : Real(0);
            const Real maxBiasVel = ctx.world->ContactPushMaxVelocity();
            const PhysicsWorld& w = *ctx.world;
            const std::uint32_t awakeCount = w.AwakeCount();
            const std::uint32_t solverCount = awakeCount + w.KinematicCount();

            for (std::uint32_t ref : m_overflowRefs)
            {
                ContactConstraint& cc = ctx.contacts[ref];
                const OverflowBodies ob = OverflowSetup(cc, w, awakeCount, solverCount);
                const Vec2 n = cc.normal;
                const Vec2 tangent(-n.y, n.x);

                Vec2 vA(m_bodyState.vx[ob.ia], m_bodyState.vy[ob.ia]);
                Real wA = m_bodyState.w[ob.ia];
                Vec2 vB(Real(0), Real(0)); Real wB = Real(0);
                if (ob.bIsBody) { vB = Vec2(m_bodyState.vx[ob.ib], m_bodyState.vy[ob.ib]); wB = m_bodyState.w[ob.ib]; }

                const Vec2 dpA(m_bodyState.dpx[ob.ia], m_bodyState.dpy[ob.ia]);
                const Real drA = m_bodyState.dq[ob.ia];
                Vec2 dpB(Real(0), Real(0)); Real drB = Real(0);
                if (ob.dynB) { dpB = Vec2(m_bodyState.dpx[ob.ib], m_bodyState.dpy[ob.ib]); drB = m_bodyState.dq[ob.ib]; }

                for (int p = 0; p < cc.pointCount; ++p)
                {
                    ContactConstraintPoint& cp = cc.points[p];
                    const Vec2 rA = cp.anchorA, rB = cp.anchorB;
                    const Vec2 prA = dpA + CrossWR(drA, rA);
                    const Vec2 prB = dpB + CrossWR(drB, rB);
                    const Real s = cp.baseSeparation + Dot(prA - prB, n);

                    Real bias = Real(0), massScale = Real(1), impulseScale = Real(0);
                    if (s > Real(0))            { bias = s * invH; }
                    else if (useBias)           { bias = std::max(cc.biasRate * s, -maxBiasVel); massScale = cc.massScale; impulseScale = cc.impulseScale; }

                    const Vec2 dv = (vA + CrossWR(wA, rA)) - (vB + CrossWR(wB, rB));
                    const Real vn = Dot(dv, n);
                    Real impulse = -cp.normalMass * massScale * (vn + bias) - impulseScale * cp.normalImpulse;
                    const Real newImpulse = std::max(cp.normalImpulse + impulse, Real(0));
                    impulse = newImpulse - cp.normalImpulse;
                    cp.normalImpulse = newImpulse;

                    const Vec2 P = n * impulse;
                    vA += P * ob.iMa; wA += ob.iIa * CrossRP(rA, P);
                    if (ob.dynB) { vB -= P * ob.iMb; wB -= ob.iIb * CrossRP(rB, P); }
                }

                for (int p = 0; p < cc.pointCount; ++p)
                {
                    ContactConstraintPoint& cp = cc.points[p];
                    const Vec2 rA = cp.anchorA, rB = cp.anchorB;
                    const Vec2 dv = (vA + CrossWR(wA, rA)) - (vB + CrossWR(wB, rB));
                    const Real vt = Dot(dv, tangent);
                    Real impulse = -cp.tangentMass * vt;
                    const Real maxFriction = cc.friction * cp.normalImpulse;
                    const Real newImpulse = std::clamp(cp.tangentImpulse + impulse, -maxFriction, maxFriction);
                    impulse = newImpulse - cp.tangentImpulse;
                    cp.tangentImpulse = newImpulse;

                    const Vec2 P = tangent * impulse;
                    vA += P * ob.iMa; wA += ob.iIa * CrossRP(rA, P);
                    if (ob.dynB) { vB -= P * ob.iMb; wB -= ob.iIb * CrossRP(rB, P); }
                }

                m_bodyState.vx[ob.ia] = static_cast<float>(vA.x);
                m_bodyState.vy[ob.ia] = static_cast<float>(vA.y);
                m_bodyState.w[ob.ia]  = static_cast<float>(wA);
                if (ob.dynB)
                {
                    m_bodyState.vx[ob.ib] = static_cast<float>(vB.x);
                    m_bodyState.vy[ob.ib] = static_cast<float>(vB.y);
                    m_bodyState.w[ob.ib]  = static_cast<float>(wB);
                }
            }
        }

        void SoftStep::OverflowRestitution(SolverContext& ctx)
        {
            const Real threshold = ctx.world->RestitutionThreshold();
            const PhysicsWorld& w = *ctx.world;
            const std::uint32_t awakeCount = w.AwakeCount();
            const std::uint32_t solverCount = awakeCount + w.KinematicCount();
            for (std::uint32_t ref : m_overflowRefs)
            {
                ContactConstraint& cc = ctx.contacts[ref];
                if (cc.restitution <= Real(0)) { continue; }
                const OverflowBodies ob = OverflowSetup(cc, w, awakeCount, solverCount);
                const Vec2 n = cc.normal;

                Vec2 vA(m_bodyState.vx[ob.ia], m_bodyState.vy[ob.ia]);
                Real wA = m_bodyState.w[ob.ia];
                Vec2 vB(Real(0), Real(0)); Real wB = Real(0);
                if (ob.bIsBody) { vB = Vec2(m_bodyState.vx[ob.ib], m_bodyState.vy[ob.ib]); wB = m_bodyState.w[ob.ib]; }

                for (int p = 0; p < cc.pointCount; ++p)
                {
                    ContactConstraintPoint& cp = cc.points[p];
                    if (cp.relativeVelocity > -threshold || cp.normalImpulse <= Real(0)) { continue; }
                    const Vec2 rA = cp.anchorA, rB = cp.anchorB;
                    const Vec2 dv = (vA + CrossWR(wA, rA)) - (vB + CrossWR(wB, rB));
                    const Real vn = Dot(dv, n);
                    Real impulse = -cp.normalMass * (vn + cc.restitution * cp.relativeVelocity);
                    const Real newImpulse = std::max(cp.normalImpulse + impulse, Real(0));
                    impulse = newImpulse - cp.normalImpulse;
                    cp.normalImpulse = newImpulse;

                    const Vec2 P = n * impulse;
                    vA += P * ob.iMa; wA += ob.iIa * CrossRP(rA, P);
                    if (ob.dynB) { vB -= P * ob.iMb; wB -= ob.iIb * CrossRP(rB, P); }
                }

                m_bodyState.vx[ob.ia] = static_cast<float>(vA.x);
                m_bodyState.vy[ob.ia] = static_cast<float>(vA.y);
                m_bodyState.w[ob.ia]  = static_cast<float>(wA);
                if (ob.dynB)
                {
                    m_bodyState.vx[ob.ib] = static_cast<float>(vB.x);
                    m_bodyState.vy[ob.ib] = static_cast<float>(vB.y);
                    m_bodyState.w[ob.ib]  = static_cast<float>(wB);
                }
            }
        }

        // ===================================================================
        // Whole-Step driver (lane-wide colored SoA contact solve, Part 1)
        // ===================================================================

        void SoftStep::Solve(SolverContext& ctx)
        {
            PhysicsWorld& w = *ctx.world;

            // Phase C, Task 2: the body-state scratch is now DENSE, sized by the
            // per-step solver index space (awake dynamics + kinematics + 1 dummy),
            // NOT the sparse world slot count. awakeCount = the awake-dynamic count
            // (solverIndex [0, awakeCount)); solverCount = awakeCount + kinematicCount
            // (kinematics occupy [awakeCount, solverCount)); the dummy tail is at
            // solverCount (statics/spans/padding gather/scatter through it).
            const std::uint32_t awakeCount  = w.AwakeCount();
            const std::uint32_t solverCount = awakeCount + w.KinematicCount();

            const Real h = ctx.subDt;
            const int substeps = static_cast<int>(ctx.substepCount);
            const bool hasJoints = ctx.jointCount > 0;
            const float maxBiasVel = static_cast<float>(w.ContactPushMaxVelocity());
            const float threshold = static_cast<float>(w.RestitutionThreshold());

            // 1) Prepare (once), SCALAR on ctx.contacts -- effective masses, soft
            //    coefficients, rest-relative velocity. Warm-start impulses arrive
            //    already seeded on each ContactConstraintPoint (emit). We run Prepare
            //    BEFORE building the SoA batches so Build packs the PREPARED values
            //    (lower churn than a lane-wide Prepare; the per-step b2MakeSoft is
            //    scalar regardless). Joints prepare as before (Part 2 leaves joints
            //    scalar).
            PrepareContacts(ctx);
            PrepareJoints(ctx);

            // 2) Body-state SoA: size to solverCount+1 (the extra slot = the
            //    scatter-safe DUMMY) and sync world velocities into the DENSE rows
            //    (zeroes dp/dq for awake dynamics; kinematics get a real velocity row;
            //    the dummy tail stays zero from Resize). The dummy slot is index
            //    `solverCount`; a redundant scatter there is harmless.
            const std::int32_t dummyIndex = static_cast<std::int32_t>(solverCount);
            m_bodyState.Resize(solverCount + 1u);
            m_bodyState.SyncInCompacted(w);

            // 3) Group the emitted constraints by their PERSISTENT contact color
            //    (Phase C, Task 4/5). The per-step greedy recolor is GONE: every
            //    solver-relevant body-body contact was colored once at create
            //    (PhysicsWorld::AssignContactColor) and EmitContactConstraints copied
            //    that color onto cc.color. A kInvalidColor constraint -- an OVERFLOW
            //    contact (the source Contact found no free color at create) OR a span
            //    (no pool home) -- goes to the scalar overflow path; everything else
            //    buckets into its color. The colored solve is within-color order-
            //    independent (a valid coloring has one contact per dynamic body per
            //    color), so bucket order does not affect the result.
            //
            //    WHY THIS IS SCATTER-SAFE: the persistent coloring is valid w.r.t.
            //    WORLD SLOTS (no two same-color contacts share a dynamic world-slot
            //    body). Each awake dynamic world slot maps to a UNIQUE awake index
            //    (AwakeIndexOf is a bijection), so world-slot-disjoint <=> awake-index-
            //    disjoint -> the coloring is also valid for the dense awake-index space
            //    PackLane/ScatterBody rely on. A color's EMITTED subset (awake +
            //    touching + solver-relevant) is a SUBSET of its persistent members, so
            //    it is still a valid (disjoint) coloring.
            for (auto& bucket : m_colorRefs) { bucket.clear(); }
            m_overflowRefs.clear();
            for (std::uint32_t c = 0; c < ctx.contactCount; ++c)
            {
                const std::uint8_t col = ctx.contacts[c].color;
                if (col < kColorCount) { m_colorRefs[col].push_back(c); }
                else                   { m_overflowRefs.push_back(c); }
            }

#ifndef NDEBUG
            // Debug-only: PROVE the active emitted subset of the persistent coloring is
            // still a valid coloring -- within each color no awake-dynamic body index
            // appears twice. This is WHY grouping-by-persistent-color is scatter-safe:
            // PackLane/ScatterBody require no two constraints in a lane-wide batch to
            // share a dynamic awake-index. A is always an awake dynamic; B is dynamic
            // iff it is a real body with positive inverse mass (statics/kinematics/spans
            // are read-only, never block a color). Reuses awakeCount as the index bound.
            {
                std::vector<std::uint8_t> seen(static_cast<std::size_t>(awakeCount), 0u);
                for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(kColorCount); ++k)
                {
                    std::fill(seen.begin(), seen.end(), 0u);
                    for (const std::uint32_t ref : m_colorRefs[k])
                    {
                        const ContactConstraint& cc = ctx.contacts[ref];
                        const std::uint32_t ia = w.AwakeIndexOf(cc.bodyA);
                        assert(ia < awakeCount && seen[ia] == 0u &&
                               "within-color clash: awake-dynamic A twice in one color");
                        seen[ia] = 1u;
                        if (cc.bodyBIsBody && cc.invMassB > Real(0))
                        {
                            const std::uint32_t ib = w.AwakeIndexOf(cc.bodyB);
                            assert(ib < awakeCount && seen[ib] == 0u &&
                                   "within-color clash: awake-dynamic B twice in one color");
                            seen[ib] = 1u;
                        }
                    }
                }
            }
#endif

            // 4) Build one SoA batch list per color (warm-start already on the
            //    prepared ctx.contacts points). Build re-homes each body index onto
            //    the dense solverIndex space via the world's awake/kinematic index
            //    maps; padding + static/span B lanes point at the dummy slot
            //    (scatter-safe), and a kinematic B reads its real dense row.
            m_colorBatches.assign(static_cast<std::size_t>(kColorCount), {});
            for (std::size_t k = 0; k < static_cast<std::size_t>(kColorCount); ++k)
            {
                const std::vector<std::uint32_t>& color = m_colorRefs[k];
                if (color.empty()) { continue; }
                m_colorBatches[k] = ContactConstraintSimd::Build(
                    ctx.contacts, color.data(), static_cast<int>(color.size()), dummyIndex,
                    w.AwakeIndexData(), w.KinematicIndexData(), awakeCount,
                    kNotKinematic);
            }

            // 5) Sub-step loop. Stage order matches the scalar driver (Box2D v3
            //    b2SolverStage sequence): integrate-vel -> warm-start -> solve(bias)
            //    -> integrate-pos -> relax(no-bias). Joints solve scalar against the
            //    world; bridge velocities SoA<->world only around the joint passes
            //    (skipped entirely when there are no joints -> pure-SoA hot path).
            for (int s = 0; s < substeps; ++s)
            {
                IntegrateVelocitiesSoA(ctx, h);

                // Warm start (per sub-step -- v3 stage order): all colors + overflow.
                for (auto& batches : m_colorBatches) { SimdSolve::WarmStart(batches, m_bodyState); }
                OverflowWarmStart(ctx);

                if (hasJoints) { SyncVelToWorld(ctx); SolveJoints(ctx); SyncVelFromWorld(ctx); }

                // Biased solve: all colors + overflow.
                for (auto& batches : m_colorBatches)
                {
                    SimdSolve::SolveNormalAndFriction(batches, m_bodyState, static_cast<float>(h), /*useBias=*/true, maxBiasVel);
                }
                OverflowSolve(ctx, h, /*useBias=*/true);

                IntegratePositionsSoA(ctx, h);

                if (hasJoints) { SyncVelToWorld(ctx); SolveJoints(ctx); SyncVelFromWorld(ctx); }

                // Relax (no bias): all colors + overflow.
                for (auto& batches : m_colorBatches)
                {
                    SimdSolve::SolveNormalAndFriction(batches, m_bodyState, static_cast<float>(h), /*useBias=*/false, maxBiasVel);
                }
                OverflowSolve(ctx, h, /*useBias=*/false);
            }

            // 6) Restitution (once): all colors + overflow.
            for (auto& batches : m_colorBatches) { SimdSolve::ApplyRestitution(batches, m_bodyState, threshold); }
            OverflowRestitution(ctx);

            // 7) Store the converged impulses back onto ctx.contacts so the world's
            //    stage-3b pool write-back persists warm-start (HAZARD 3). Overflow
            //    constraints already carry their accumulated impulses on
            //    ctx.contacts (the scalar overflow path wrote them in place); only
            //    the colored batches need this copy-back.
            for (std::size_t k = 0; k < static_cast<std::size_t>(kColorCount); ++k)
            {
                const std::vector<std::uint32_t>& color = m_colorRefs[k];
                if (color.empty()) { continue; }
                SimdSolve::StoreImpulses(m_colorBatches[k], ctx.contacts, color.data());
            }

            // 8) Push final velocities to the world (DENSE write-back), then commit
            //    positions (compound-COM) from the SoA dp/dq.
            m_bodyState.SyncOutCompacted(w);
            FinalizePositionsSoA(ctx);
        }

    } // namespace Physics
} // namespace Arcane
