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
#include <Arcane/Physics/Joints/Joint.hpp> // Joint (Prepare/SolveVelocity)

namespace Arcane
{
    namespace Physics
    {
        namespace
        {
            // Steps a warm-start cache entry survives unused before eviction
            // (the Lua CACHE_LIFE = 2). Bounds the cache.
            constexpr std::uint32_t kCacheLife = 2u;

            // pi (f64 literal narrowed to Real; matches the workspace style).
            constexpr Real kPi = Real(3.14159265358979323846);

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

            // b2MakeSoft(hertz, zeta, h): the Box2D v3 soft-constraint
            // coefficients. hertz == 0 -> a hard (un-softened) constraint.
            struct Soft
            {
                Real biasRate     = Real(0);
                Real massScale    = Real(1);
                Real impulseScale = Real(0);
            };
            Soft MakeSoft(Real hertz, Real zeta, Real h) noexcept
            {
                if (hertz <= Real(0))
                {
                    return Soft{ Real(0), Real(1), Real(0) };
                }
                const Real omega = Real(2) * kPi * hertz;
                const Real a1 = Real(2) * zeta + h * omega;
                const Real a2 = h * omega * a1;
                const Real a3 = Real(1) / (Real(1) + a2);
                return Soft{ omega / a1, a2 * a3, a3 };
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
            // Stale impulses keyed by manifold ids that referenced this slot are
            // evicted by the normal stamp-based eviction; the per-body scratch is
            // re-zeroed at the start of each Solve, so a recycled slot inherits
            // nothing. Nothing slot-specific to do here today, but the hook keeps
            // the contract explicit for a future id-scheme that embeds the slot.
            (void)slot;
        }

        // ===================================================================
        // Prepare
        // ===================================================================

        void SoftStep::PrepareContacts(SolverContext& ctx)
        {
            PhysicsWorld& w = *ctx.world;
            ++m_stamp;

            // High contact hertz that, with the small sub-step, appears rigid --
            // but never out-running the sub-step solve rate (Box2D v3 clamps to
            // 0.25 * substepCount / dt == 0.25 / h).
            const Real h = ctx.subDt;
            const Real maxHertz = (h > Real(0)) ? (Real(0.25) / h) : w.ContactHertz();
            const Real contactHertz = std::min(w.ContactHertz(), maxHertz);
            const Soft contactSoft = MakeSoft(contactHertz, w.ContactDampingRatio(), h);

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

                    // Warm start: seed accumulators from the prior step's cache.
                    auto it = m_cache.find(cp.id);
                    if (it != m_cache.end())
                    {
                        cp.normalImpulse  = it->second.normalImpulse;
                        cp.tangentImpulse = it->second.tangentImpulse;
                    }
                    else
                    {
                        cp.normalImpulse  = Real(0);
                        cp.tangentImpulse = Real(0);
                    }
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
                const Vec2 p = w.PosSlot(i) + m_deltaPos[i];
                const Real a = w.GetAngle(w.HandleOf(i)) + m_deltaRot[i];
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
        // Whole-Step driver
        // ===================================================================

        void SoftStep::Solve(SolverContext& ctx)
        {
            PhysicsWorld& w = *ctx.world;
            const std::uint32_t count = w.Count();
            EnsureScratch(count);

            // Reset this step's per-body position deltas (awake dynamics only;
            // others stay irrelevant but are cheap to clear for determinism).
            for (std::uint32_t i = 0; i < count; ++i)
            {
                m_deltaPos[i] = Vec2(Real(0), Real(0));
                m_deltaRot[i] = Real(0);
            }

            const Real h = ctx.subDt;
            const int substeps = static_cast<int>(ctx.substepCount);

            // 1) Prepare (once): effective masses, soft coefficients, warm-start
            //    seeds, rest-relative velocity. Always run so warm-start impulses
            //    persist + the cache stamp advances even with zero contacts.
            PrepareContacts(ctx);
            PrepareJoints(ctx);

            // 2) Sub-step loop.
            //    Stage order per sub-step (matches Box2D v3 b2SolverStage sequence
            //    in solver.h / solver.c: b2_stageIntegrateVelocities ->
            //    b2_stageWarmStart -> b2_stageSolve -> b2_stageIntegratePositions ->
            //    b2_stageRelax). WarmStart is a PER-SUBSTEP stage in v3 (executed
            //    inside the substep loop, not once before it). The current placement
            //    is correct and intentional.
            for (int s = 0; s < substeps; ++s)
            {
                IntegrateVelocities(ctx, h);   // gravity + damping (per sub-step)
                WarmStart(ctx);                // apply accumulated impulses (per sub-step -- v3 stage order)
                SolveJoints(ctx);              // joints solve first each sub-step (Lua ordering)
                SolveContacts(ctx, h, /*useBias=*/true);
                IntegratePositions(ctx, h);    // accumulate deltaPos/deltaRot
                SolveJoints(ctx);              // relax-side joint pass (drives biased joints, keeps them tight)
                SolveContacts(ctx, h, /*useBias=*/false); // relax (no bias)
            }

            // 3) Restitution (once).
            ApplyRestitution(ctx);

            // 4) Store impulses to the warm-start cache (+ evict stale entries).
            // insert_or_assign avoids the default-construct+insert on NEW ids that
            // operator[] performs, eliminating the per-step heap allocation when
            // contact ids churn (e.g. bodies cycling in/out of sleep in P2.4).
            for (std::uint32_t c = 0; c < ctx.contactCount; ++c)
            {
                const ContactConstraint& cc = ctx.contacts[c];
                for (int p = 0; p < cc.pointCount; ++p)
                {
                    const ContactConstraintPoint& cp = cc.points[p];
                    m_cache.insert_or_assign(cp.id, CacheEntry{ cp.normalImpulse,
                                                                cp.tangentImpulse,
                                                                m_stamp });
                }
            }
            // Bounded cache: drop entries unused for more than kCacheLife stamps.
            // iteration order over m_cache is unobservable here (delete-only);
            // determinism is preserved (warm-start seeds are loaded by find(id),
            // not by iteration).
            for (auto it = m_cache.begin(); it != m_cache.end();)
            {
                if (m_stamp - it->second.stamp > kCacheLife)
                {
                    it = m_cache.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            // Commit accumulated position deltas onto the world (the solver owns
            // dynamic position integration; Step's old inline stage-4 is gone).
            FinalizePositions(ctx);
        }

    } // namespace Physics
} // namespace Arcane
