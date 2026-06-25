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

        // ===================================================================
        // Scratch sizing
        // ===================================================================

        void SoftStep::EnsureScratch(std::uint32_t n)
        {
            if (n > m_deltaPos.size())
            {
                m_deltaPos.resize(n, Vec2(Real(0), Real(0)));
                m_deltaRot.resize(n, Real(0));
            }
        }

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
        // BodyStateSoA world<->solver sync (SIMD constraint-solver Task 1)
        // ===================================================================
        //
        // CONTRACT: the caller Resize()s the SoA to world.Count() (which zeroes
        // every array) before SyncIn. SyncIn then OVERWRITES only the slots that
        // satisfy the awake-dynamic predicate -- the SAME predicate the solver's
        // IntegrateVelocities / IntegratePositions / FinalizePositions use:
        //   Alive(i) && TypeSlot(i) == BodyType::Dynamic && AwakeSlot(i).
        // Non-matching slots (statics, kinematics, asleep/dead dynamics) are
        // LEFT AS-IS (zero after the caller's Resize). For matched slots SyncIn
        // also zeroes the TGS position-delta accumulators (dp/dq), since they
        // start each Step's sub-step loop at zero.
        //
        // SyncOut writes the packed velocities BACK to the world for the SAME
        // predicate, and ONLY the velocities -- positions are committed by the
        // solver's FinalizePositions, never here. Nothing consumes this bridge
        // yet (the solver wiring is a later task); these defs live here (not the
        // header) because they need the PhysicsWorld slot accessors.

        void BodyStateSoA::SyncIn(const PhysicsWorld& world)
        {
            const std::uint32_t count = world.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!world.Alive(i))
                {
                    continue; // dead slot: leave as the caller Resize'd it (zero)
                }

                // ALL alive bodies' velocities are mirrored so the lane-wide solve
                // can GATHER a read-only B's velocity (a kinematic plate's authored
                // +X, a static body's 0). The scalar SolveContacts reads vB/wB for
                // ANY bodyBIsBody endpoint (its velocity feeds the relative-velocity
                // term that drives the push), even when B is not MUTATED -- so the
                // SoA must carry that velocity too, else a kinematic-pushes-dynamic
                // contact would see B at rest and never push. dp/dq stay 0 for a
                // non-awake-dynamic body (it never integrates).
                const Vec2 v = world.VelSlot(i);
                vx[i] = v.x;
                vy[i] = v.y;
                w[i]  = world.AngVelSlot(i);

                if (world.TypeSlot(i) == BodyType::Dynamic && world.AwakeSlot(i))
                {
                    // Awake dynamic: zero the TGS position-delta accumulators (they
                    // start each Step's sub-step loop at zero).
                    dpx[i] = 0.f;
                    dpy[i] = 0.f;
                    dq[i]  = 0.f;
                }
            }
        }

        void BodyStateSoA::SyncOut(PhysicsWorld& world) const
        {
            const std::uint32_t count = world.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!world.Alive(i) ||
                    world.TypeSlot(i) != BodyType::Dynamic ||
                    !world.AwakeSlot(i))
                {
                    continue; // never disturb non-synced slots
                }
                world.SetVelSlot(i, Vec2(vx[i], vy[i]));
                world.SetAngVelSlot(i, w[i]);
            }
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

        // ISolver phase entry points. The whole-Step Solve() driver above calls
        // the lower-level SolveContacts directly (it interleaves the velocity +
        // position integration the v3 algorithm folds into the sub-step loop),
        // but these honor the ISolver contract so a profiler / oracle / future
        // caller can drive the phases individually. SolveVelocity = biased soft
        // solve; Relax = the no-bias pass that removes the bias-injected energy.
        void SoftStep::SolveVelocity(SolverContext& ctx, int /*substep*/)
        {
            SolveContacts(ctx, ctx.subDt, /*useBias=*/true);
        }

        void SoftStep::Relax(SolverContext& ctx, int /*substep*/)
        {
            SolveContacts(ctx, ctx.subDt, /*useBias=*/false);
        }

        // ===================================================================
        // Sub-step phases
        // ===================================================================

        void SoftStep::IntegrateVelocities(SolverContext& ctx, Real h)
        {
            PhysicsWorld& w = *ctx.world;
            const Vec2 g = ctx.gravity;

            const std::uint32_t count = w.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!w.Alive(i) || w.TypeSlot(i) != BodyType::Dynamic || !w.AwakeSlot(i))
                {
                    continue;
                }
                if (w.InvMassSlot(i) <= Real(0))
                {
                    continue; // pinned dynamic (mass override 0) -- never integrates
                }
                Vec2 v = w.VelSlot(i);
                v += g * h;                 // gravity (gravityScale = 1)
                const Real d = w.LinDampSlot(i);
                Real wv = w.AngVelSlot(i);
                if (d > Real(0))
                {
                    const Real f = Real(1) / (Real(1) + d * h);
                    v  *= f;
                    wv *= f;
                }
                w.SetVelSlot(i, v);
                w.SetAngVelSlot(i, wv);
            }
        }

        void SoftStep::WarmStart(SolverContext& ctx)
        {
            PhysicsWorld& w = *ctx.world;
            for (std::uint32_t c = 0; c < ctx.contactCount; ++c)
            {
                ContactConstraint& cc = ctx.contacts[c];
                const Vec2 n = cc.normal;
                const Vec2 tangent(-n.y, n.x);

                Vec2 vA = w.VelSlot(cc.bodyA);
                Real wA = w.AngVelSlot(cc.bodyA);
                Vec2 vB(Real(0), Real(0));
                Real wB = Real(0);
                const bool dynB = cc.bodyBIsBody && cc.invMassB > Real(0);
                if (cc.bodyBIsBody)
                {
                    vB = w.VelSlot(cc.bodyB);
                    wB = w.AngVelSlot(cc.bodyB);
                }

                for (int p = 0; p < cc.pointCount; ++p)
                {
                    const ContactConstraintPoint& cp = cc.points[p];
                    // Normal points B->A (push A out of B): A gets +P, B gets -P
                    // (matches the Lua applyAt(a, +..) / applyAt(b, -..)).
                    const Vec2 P = n * cp.normalImpulse + tangent * cp.tangentImpulse;
                    vA += P * cc.invMassA;
                    wA += cc.invInertiaA * CrossRP(cp.anchorA, P);
                    if (dynB)
                    {
                        vB -= P * cc.invMassB;
                        wB -= cc.invInertiaB * CrossRP(cp.anchorB, P);
                    }
                }

                w.SetVelSlot(cc.bodyA, vA);
                w.SetAngVelSlot(cc.bodyA, wA);
                if (dynB)
                {
                    w.SetVelSlot(cc.bodyB, vB);
                    w.SetAngVelSlot(cc.bodyB, wB);
                }
            }
        }

        void SoftStep::SolveContacts(SolverContext& ctx, Real h, bool useBias)
        {
            PhysicsWorld& w = *ctx.world;
            const Real invH = (h > Real(0)) ? (Real(1) / h) : Real(0);
            const Real maxBiasVel = w.ContactPushMaxVelocity();

            for (std::uint32_t c = 0; c < ctx.contactCount; ++c)
            {
                ContactConstraint& cc = ctx.contacts[c];
                const Vec2 n = cc.normal;
                const Vec2 tangent(-n.y, n.x);

                const Real iMa = cc.invMassA, iIa = cc.invInertiaA;
                const Real iMb = cc.invMassB, iIb = cc.invInertiaB;
                const bool dynB = cc.bodyBIsBody && iMb > Real(0);

                Vec2 vA = w.VelSlot(cc.bodyA);
                Real wA = w.AngVelSlot(cc.bodyA);
                Vec2 vB(Real(0), Real(0));
                Real wB = Real(0);
                if (cc.bodyBIsBody)
                {
                    vB = w.VelSlot(cc.bodyB);
                    wB = w.AngVelSlot(cc.bodyB);
                }

                // Per-body accumulated position deltas (TGS separation tracking).
                const Vec2 dpA = m_deltaPos[cc.bodyA];
                const Real drA = m_deltaRot[cc.bodyA];
                Vec2 dpB(Real(0), Real(0));
                Real drB = Real(0);
                if (cc.bodyBIsBody)
                {
                    dpB = m_deltaPos[cc.bodyB];
                    drB = m_deltaRot[cc.bodyB];
                }

                // ---- normal solve (per point) ------------------------------
                for (int p = 0; p < cc.pointCount; ++p)
                {
                    ContactConstraintPoint& cp = cc.points[p];
                    const Vec2 rA = cp.anchorA;
                    const Vec2 rB = cp.anchorB;

                    // Current separation: base + the change from this step's
                    // accumulated position deltas (incl. the small anchor-rotation
                    // term). With the B->A normal, A moving along +n INCREASES the
                    // separation, so ds = dot((dpA + drA x rA) - (dpB + drB x rB), n).
                    // Separation s > 0 == a gap; s < 0 == penetration.
                    const Vec2 prA = dpA + CrossWR(drA, rA);
                    const Vec2 prB = dpB + CrossWR(drB, rB);
                    const Real s = cp.baseSeparation + Dot(prA - prB, n);

                    Real bias = Real(0);
                    Real massScale = Real(1);
                    Real impulseScale = Real(0);
                    if (s > Real(0))
                    {
                        // Speculative gap: allow the bodies to approach only fast
                        // enough to close the gap in one sub-step (a hard upper
                        // bound on the closing velocity, NOT a soft push).
                        bias = s * invH;
                    }
                    else if (useBias)
                    {
                        // Penetration (s < 0): a soft push-OUT. Clamp the push
                        // velocity so a deep overlap recovers without exploding.
                        bias = std::max(cc.biasRate * s, -maxBiasVel);
                        massScale = cc.massScale;
                        impulseScale = cc.impulseScale;
                    }

                    // Relative normal velocity (A relative to B, along B->A n).
                    // Approaching (penetrating further) -> vn < 0.
                    const Vec2 dv = (vA + CrossWR(wA, rA)) - (vB + CrossWR(wB, rB));
                    const Real vn = Dot(dv, n);

                    // Soft TGS normal impulse. (vn + bias): for penetration bias
                    // is negative -> a larger positive impulse that pushes apart.
                    Real impulse = -cp.normalMass * massScale * (vn + bias)
                                 - impulseScale * cp.normalImpulse;

                    // Accumulated clamp >= 0 (contacts only push).
                    const Real newImpulse = std::max(cp.normalImpulse + impulse, Real(0));
                    impulse = newImpulse - cp.normalImpulse;
                    cp.normalImpulse = newImpulse;

                    // Normal points B->A: A gets +P, B gets -P.
                    const Vec2 P = n * impulse;
                    vA += P * iMa;
                    wA += iIa * CrossRP(rA, P);
                    if (dynB)
                    {
                        vB -= P * iMb;
                        wB -= iIb * CrossRP(rB, P);
                    }
                }

                // ---- friction solve (per point) ----------------------------
                for (int p = 0; p < cc.pointCount; ++p)
                {
                    ContactConstraintPoint& cp = cc.points[p];
                    const Vec2 rA = cp.anchorA;
                    const Vec2 rB = cp.anchorB;

                    const Vec2 dv = (vA + CrossWR(wA, rA)) - (vB + CrossWR(wB, rB));
                    const Real vt = Dot(dv, tangent);

                    Real impulse = -cp.tangentMass * vt;

                    // |tangent| <= friction * normal (Coulomb cone).
                    const Real maxFriction = cc.friction * cp.normalImpulse;
                    const Real newImpulse =
                        std::clamp(cp.tangentImpulse + impulse, -maxFriction, maxFriction);
                    impulse = newImpulse - cp.tangentImpulse;
                    cp.tangentImpulse = newImpulse;

                    // Same B->A convention: A gets +P, B gets -P.
                    const Vec2 P = tangent * impulse;
                    vA += P * iMa;
                    wA += iIa * CrossRP(rA, P);
                    if (dynB)
                    {
                        vB -= P * iMb;
                        wB -= iIb * CrossRP(rB, P);
                    }
                }

                w.SetVelSlot(cc.bodyA, vA);
                w.SetAngVelSlot(cc.bodyA, wA);
                if (dynB)
                {
                    w.SetVelSlot(cc.bodyB, vB);
                    w.SetAngVelSlot(cc.bodyB, wB);
                }
            }
        }

        void SoftStep::IntegratePositions(SolverContext& ctx, Real h)
        {
            PhysicsWorld& w = *ctx.world;
            const std::uint32_t count = w.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!w.Alive(i) || w.TypeSlot(i) != BodyType::Dynamic || !w.AwakeSlot(i))
                {
                    continue;
                }
                m_deltaPos[i] += w.VelSlot(i) * h;
                m_deltaRot[i] += w.AngVelSlot(i) * h;
            }
        }

        void SoftStep::FinalizePositions(SolverContext& ctx)
        {
            PhysicsWorld& w = *ctx.world;
            const std::uint32_t count = w.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!w.Alive(i) || w.TypeSlot(i) != BodyType::Dynamic || !w.AwakeSlot(i))
                {
                    continue;
                }
                // Compound-COM dynamics: integrate the body's CENTER OF MASS
                // along its inertial path (deltaPos = sum of VelSlot*h is the
                // COM linear displacement), advance the angle, then reconstruct
                // the ORIGIN from the new COM + new angle. PosSlot/GetAngle here
                // are the START-OF-STEP origin/angle (p0/a0): deltaPos/deltaRot
                // were accumulated across the sub-step loop and committed only
                // here, so the world position is untouched until this point.
                //
                // For localCenter == (0,0) this is byte-identical to the old
                //   p = PosSlot + deltaPos; a = GetAngle + deltaRot
                // because c0 == p0, c == p0 + deltaPos, and p == c (R*lc == 0).
                const Vec2 p0 = w.PosSlot(i);
                const Real a0 = w.GetAngle(w.HandleOf(i));
                const Vec2 lc = w.LocalCenterSlot(i);
                const Vec2 c0 = p0 + RotateVec(a0, lc);   // start-of-step world COM
                const Vec2 c  = c0 + m_deltaPos[i];       // integrate COM by linear vel
                const Real a  = a0 + m_deltaRot[i];       // integrate angle
                const Vec2 p  = c - RotateVec(a, lc);     // origin from new COM + angle
                w.CommitSlotPosition(i, p, a);
            }
        }

        // ===================================================================
        // Restitution
        // ===================================================================

        void SoftStep::ApplyRestitution(SolverContext& ctx)
        {
            PhysicsWorld& w = *ctx.world;
            const Real threshold = w.RestitutionThreshold();

            for (std::uint32_t c = 0; c < ctx.contactCount; ++c)
            {
                ContactConstraint& cc = ctx.contacts[c];
                if (cc.restitution <= Real(0))
                {
                    continue;
                }
                const Vec2 n = cc.normal;
                const Real iMa = cc.invMassA, iIa = cc.invInertiaA;
                const Real iMb = cc.invMassB, iIb = cc.invInertiaB;
                const bool dynB = cc.bodyBIsBody && iMb > Real(0);

                Vec2 vA = w.VelSlot(cc.bodyA);
                Real wA = w.AngVelSlot(cc.bodyA);
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
                    // Only points that approached fast enough AND took a normal
                    // impulse (i.e. genuinely collided) rebound.
                    if (cp.relativeVelocity > -threshold || cp.normalImpulse <= Real(0))
                    {
                        continue;
                    }
                    const Vec2 rA = cp.anchorA;
                    const Vec2 rB = cp.anchorB;
                    const Vec2 dv = (vA + CrossWR(wA, rA)) - (vB + CrossWR(wB, rB));
                    const Real vn = Dot(dv, n);

                    // Target separating speed = -e * approachSpeed. relativeVelocity
                    // is the (negative) approach speed; the new vn should be at
                    // least -e*relativeVelocity (positive == separating).
                    Real impulse = -cp.normalMass * (vn + cc.restitution * cp.relativeVelocity);
                    const Real newImpulse = std::max(cp.normalImpulse + impulse, Real(0));
                    impulse = newImpulse - cp.normalImpulse;
                    cp.normalImpulse = newImpulse;

                    const Vec2 P = n * impulse;
                    vA += P * iMa;
                    wA += iIa * CrossRP(rA, P);
                    if (dynB)
                    {
                        vB -= P * iMb;
                        wB -= iIb * CrossRP(rB, P);
                    }
                }

                w.SetVelSlot(cc.bodyA, vA);
                w.SetAngVelSlot(cc.bodyA, wA);
                if (dynB)
                {
                    w.SetVelSlot(cc.bodyB, vB);
                    w.SetAngVelSlot(cc.bodyB, wB);
                }
            }
        }

        void SoftStep::SolvePosition(SolverContext& ctx)
        {
            // Soft contacts + the speculative bias handle penetration recovery;
            // no separate NGS pass is needed for the Soft Step solver. (The
            // Baumgarte oracle in P2.3 leans on its own position bias instead.)
            (void)ctx;
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
            // SoA-resident port of IntegrateVelocities: gravity + linear damping
            // for awake dynamics. Reads world type/awake/invMass/linDamp (immutable
            // this Step) but writes the packed SoA velocities. Same predicate +
            // math as IntegrateVelocities so the SoA path is behavior-equivalent.
            PhysicsWorld& w = *ctx.world;
            const Vec2 g = ctx.gravity;
            const std::uint32_t count = w.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!w.Alive(i) || w.TypeSlot(i) != BodyType::Dynamic || !w.AwakeSlot(i))
                {
                    continue;
                }
                if (w.InvMassSlot(i) <= Real(0))
                {
                    continue; // pinned dynamic -- never integrates
                }
                float vx = m_bodyState.vx[i] + static_cast<float>(g.x * h);
                float vy = m_bodyState.vy[i] + static_cast<float>(g.y * h);
                float wv = m_bodyState.w[i];
                const Real d = w.LinDampSlot(i);
                if (d > Real(0))
                {
                    const float f = static_cast<float>(Real(1) / (Real(1) + d * h));
                    vx *= f; vy *= f; wv *= f;
                }
                m_bodyState.vx[i] = vx;
                m_bodyState.vy[i] = vy;
                m_bodyState.w[i]  = wv;
            }
        }

        void SoftStep::IntegratePositionsSoA(SolverContext& ctx, Real h)
        {
            // dp/dq += v*h for awake dynamics (SoA-resident port of
            // IntegratePositions). The lane-wide contact solve re-reads dp/dq next
            // sub-step to re-evaluate separation (the TGS heart).
            PhysicsWorld& w = *ctx.world;
            const float fh = static_cast<float>(h);
            const std::uint32_t count = w.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!w.Alive(i) || w.TypeSlot(i) != BodyType::Dynamic || !w.AwakeSlot(i))
                {
                    continue;
                }
                m_bodyState.dpx[i] += m_bodyState.vx[i] * fh;
                m_bodyState.dpy[i] += m_bodyState.vy[i] * fh;
                m_bodyState.dq[i]  += m_bodyState.w[i]  * fh;
            }
        }

        void SoftStep::FinalizePositionsSoA(SolverContext& ctx)
        {
            // SAME compound-COM commit as FinalizePositions (SoftStep.cpp:483-513),
            // but reading the per-body deltas from m_bodyState.dp*/dq instead of
            // m_deltaPos/m_deltaRot. Integrate the COM along its inertial path, then
            // reconstruct the origin from new COM + new angle.
            PhysicsWorld& w = *ctx.world;
            const std::uint32_t count = w.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!w.Alive(i) || w.TypeSlot(i) != BodyType::Dynamic || !w.AwakeSlot(i))
                {
                    continue;
                }
                const Vec2 dp(static_cast<Real>(m_bodyState.dpx[i]),
                              static_cast<Real>(m_bodyState.dpy[i]));
                const Real dr = static_cast<Real>(m_bodyState.dq[i]);
                const Vec2 p0 = w.PosSlot(i);
                const Real a0 = w.GetAngle(w.HandleOf(i));
                const Vec2 lc = w.LocalCenterSlot(i);
                const Vec2 c0 = p0 + RotateVec(a0, lc);
                const Vec2 c  = c0 + dp;
                const Real a  = a0 + dr;
                const Vec2 p  = c - RotateVec(a, lc);
                w.CommitSlotPosition(i, p, a);
            }
        }

        void SoftStep::SyncVelToWorld(SolverContext& ctx)
        {
            // Push the SoA velocities back to the world so the scalar joint passes
            // read the in-flight (contact-solved) velocities. Awake dynamics only.
            PhysicsWorld& w = *ctx.world;
            const std::uint32_t count = w.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!w.Alive(i) || w.TypeSlot(i) != BodyType::Dynamic || !w.AwakeSlot(i))
                {
                    continue;
                }
                w.SetVelSlot(i, Vec2(static_cast<Real>(m_bodyState.vx[i]),
                                     static_cast<Real>(m_bodyState.vy[i])));
                w.SetAngVelSlot(i, static_cast<Real>(m_bodyState.w[i]));
            }
        }

        void SoftStep::SyncVelFromWorld(SolverContext& ctx)
        {
            // Pull the world velocities back into the SoA after a joint pass mutated
            // them. dp/dq untouched (joints do not read them). Awake dynamics only.
            PhysicsWorld& w = *ctx.world;
            const std::uint32_t count = w.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!w.Alive(i) || w.TypeSlot(i) != BodyType::Dynamic || !w.AwakeSlot(i))
                {
                    continue;
                }
                const Vec2 v = w.VelSlot(i);
                m_bodyState.vx[i] = static_cast<float>(v.x);
                m_bodyState.vy[i] = static_cast<float>(v.y);
                m_bodyState.w[i]  = static_cast<float>(w.AngVelSlot(i));
            }
        }

        // ----- Overflow: width-1 SCALAR solve over the SoA (scatter-safe) ------
        //
        // The colorer spills constraints that found no free color into m_coloring.
        // overflow (a hub body in > kColorCount contacts). They CANNOT solve lane-
        // wide (they share bodies), so we solve them one-at-a-time, scalar, over the
        // SAME BodyStateSoA -- sequential => no scatter conflict. The math mirrors
        // the scalar SoftStep WarmStart/SolveContacts/ApplyRestitution exactly, but
        // reads/writes the SoA velocity + dp/dq (so overflow composes with the
        // colored result in the same velocity store). One contact per call iter.

        namespace
        {
            // Per-overflow-constraint scalar helpers (SoA-resident). cc is the
            // emitted ContactConstraint; bs holds vel + dp/dq by world slot.
            //   bIsBody : B is a real body (read its velocity for the relative-
            //             velocity term -- a kinematic plate's velocity drives the
            //             push; SyncIn mirrors all alive bodies for this).
            //   dynB    : B is a real DYNAMIC body whose velocity is MUTATED.
            struct OverflowBodies
            {
                std::uint32_t ia, ib;
                bool bIsBody, dynB;
                Real iMa, iIa, iMb, iIb;
            };

            inline OverflowBodies OverflowSetup(const ContactConstraint& cc)
            {
                OverflowBodies ob;
                ob.ia      = cc.bodyA;
                ob.ib      = cc.bodyB;
                ob.bIsBody = cc.bodyBIsBody;
                ob.dynB    = cc.bodyBIsBody && cc.invMassB > Real(0);
                ob.iMa     = cc.invMassA; ob.iIa = cc.invInertiaA;
                ob.iMb     = cc.invMassB; ob.iIb = cc.invInertiaB;
                return ob;
            }
        } // namespace

        void SoftStep::OverflowWarmStart(SolverContext& ctx)
        {
            for (std::uint32_t ref : m_coloring.overflow)
            {
                ContactConstraint& cc = ctx.contacts[ref];
                const OverflowBodies ob = OverflowSetup(cc);
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

            for (std::uint32_t ref : m_coloring.overflow)
            {
                ContactConstraint& cc = ctx.contacts[ref];
                const OverflowBodies ob = OverflowSetup(cc);
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
            for (std::uint32_t ref : m_coloring.overflow)
            {
                ContactConstraint& cc = ctx.contacts[ref];
                if (cc.restitution <= Real(0)) { continue; }
                const OverflowBodies ob = OverflowSetup(cc);
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
            const std::uint32_t count = w.Count();

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

            // 2) Body-state SoA: size to count+1 (the extra slot = the scatter-safe
            //    DUMMY) and sync world velocities in (zeroes dp/dq for awake
            //    dynamics). The dummy slot is index `count`; Resize zeroes it and
            //    SyncIn leaves it zero (not an awake dynamic), so a redundant scatter
            //    there is harmless.
            const std::int32_t dummyIndex = static_cast<std::int32_t>(count);
            m_bodyState.Resize(count + 1u);
            m_bodyState.SyncIn(w);

            // 3) Per-step greedy coloring of the solver-relevant touching contacts.
            //    A is already the dynamic orientation (always aDyn). B is dynamic iff
            //    it is a real body with positive inverse mass. Static/kinematic/span
            //    endpoints are read-only -> not marked dynamic (do not constrain
            //    coloring). ref = the constraint index into ctx.contacts.
            m_edges.clear();
            m_edges.reserve(ctx.contactCount);
            for (std::uint32_t c = 0; c < ctx.contactCount; ++c)
            {
                const ContactConstraint& cc = ctx.contacts[c];
                const bool bDyn = cc.bodyBIsBody && cc.invMassB > Real(0);
                ColorEdge e;
                e.a    = cc.bodyA;
                e.b    = bDyn ? cc.bodyB : cc.bodyA; // b unused for coloring when !bDyn
                e.aDyn = true;                       // A is always dynamic in the feed
                e.bDyn = bDyn;
                e.ref  = c;
                m_edges.push_back(e);
            }
            m_coloring = ColorConstraints(m_edges, count);

            // 4) Build one SoA batch list per color (warm-start already on the
            //    prepared ctx.contacts points). Padding + read-only-B lanes point at
            //    the dummy slot (scatter-safe).
            m_colorBatches.assign(m_coloring.colors.size(), {});
            for (std::size_t k = 0; k < m_coloring.colors.size(); ++k)
            {
                const std::vector<std::uint32_t>& color = m_coloring.colors[k];
                if (color.empty()) { continue; }
                m_colorBatches[k] = ContactConstraintSimd::Build(
                    ctx.contacts, color.data(), static_cast<int>(color.size()), dummyIndex);
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
            for (std::size_t k = 0; k < m_coloring.colors.size(); ++k)
            {
                const std::vector<std::uint32_t>& color = m_coloring.colors[k];
                if (color.empty()) { continue; }
                SimdSolve::StoreImpulses(m_colorBatches[k], ctx.contacts, color.data());
            }

            // 8) Push final velocities to the world, then commit positions (compound-
            //    COM) from the SoA dp/dq.
            m_bodyState.SyncOut(w);
            FinalizePositionsSoA(ctx);
        }

    } // namespace Physics
} // namespace Arcane
