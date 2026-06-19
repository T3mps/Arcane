// Baumgarte.cpp -- the retained sequential-impulse PGS oracle (M6, Task P2.3).
//
// Faithful port of Client/src/physics/SequentialImpulse.lua. See Baumgarte.hpp
// for the algorithm overview + the PORT-vs-MODERNIZE framing and the port map
// of the Lua module's constants. This TU implements ISolver::Solve as the
// single-step solve that owns dynamic integration (the world no longer
// integrates dynamics inline -- the solver does, see Solver.hpp).
//
// PRESENTATION-FREE + C++20-clean: glm::vec2 + std + sibling Physics headers.

#include <Arcane/Physics/Solver/Baumgarte.hpp>

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
            // Lua SequentialImpulse constants (lines 7-10).
            constexpr Real          kBeta      = Real(0.2);  // positional-correction factor
            constexpr Real          kSlop      = Real(0.5);  // penetration tolerance
            constexpr Real          kRestVel   = Real(20);   // restitution above this approach speed
            constexpr std::uint32_t kCacheLife = 2u;         // steps an unused entry survives

            // 2D cross helpers (mirror the Lua applyAt / contact math).
            inline Real CrossRP(const Vec2& r, const Vec2& p) noexcept
            {
                return r.x * p.y - r.y * p.x;
            }
            inline Real Dot(const Vec2& a, const Vec2& b) noexcept
            {
                return a.x * b.x + a.y * b.y;
            }
        } // namespace

        void Baumgarte::DropBody(std::uint32_t slot)
        {
            // Warm-start entries keyed by manifold ids referencing this slot are
            // evicted by the normal stamp-based eviction (kCacheLife); a recycled
            // slot therefore inherits nothing. Nothing slot-specific to do today;
            // the hook keeps the contract explicit (parity with SoftStep).
            (void)slot;
        }

        void Baumgarte::IntegrateVelocities(SolverContext& ctx)
        {
            // Ports the Lua world stage-1 dynamic branch (gravity + linear
            // damping), now owned by the solver, integrated over the FULL dt.
            PhysicsWorld& w = *ctx.world;
            const Vec2 g = ctx.gravity;
            const Real dt = ctx.dt;

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
                v += g * dt; // gravity (gravityScale = 1)
                Real wv = w.AngVelSlot(i);
                const Real d = w.LinDampSlot(i);
                if (d > Real(0))
                {
                    const Real f = Real(1) / (Real(1) + d * dt);
                    v  *= f;
                    wv *= f; // linDamp decays linear AND angular velocity, matching PhysicsWorld.lua:305-310 (world stage-1, now solver-owned).
                }
                w.SetVelSlot(i, v);
                w.SetAngVelSlot(i, wv);
            }
        }

        void Baumgarte::IntegratePositions(SolverContext& ctx)
        {
            // Integrate the COM by dt*v + dt*angVel and reconstruct the origin
            // (compound-COM: bodies rotate about their COM; see the per-body math
            // below -- byte-identical to the old pos += dt*v / angle += dt*angVel
            // for localCenter == 0). Awake dynamics only, then refresh the mover
            // broadphase AABB. Full-dt semi-implicit Euler (the solver already
            // advanced velocities this Step).
            PhysicsWorld& w = *ctx.world;
            const Real dt = ctx.dt;

            const std::uint32_t count = w.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!w.Alive(i) || w.TypeSlot(i) != BodyType::Dynamic || !w.AwakeSlot(i))
                {
                    continue;
                }
                // Compound-COM dynamics (cross-check oracle parity with
                // SoftStep::FinalizePositions): integrate the CENTER OF MASS by
                // the (COM) linear velocity, advance the angle, then reconstruct
                // the ORIGIN from the new COM + new angle. VelSlot is the COM
                // linear velocity (Box2D body-origin/COM model). For
                // localCenter == (0,0) this is byte-identical to the old
                //   p = PosSlot + VelSlot*dt; a = GetAngle + AngVel*dt.
                const Vec2 p0 = w.PosSlot(i);
                const Real a0 = w.GetAngle(w.HandleOf(i));
                const Vec2 lc = w.LocalCenterSlot(i);
                const Vec2 c0 = p0 + RotateVec(a0, lc);     // start-of-step world COM
                const Vec2 c  = c0 + w.VelSlot(i) * dt;     // integrate COM by linear vel
                const Real a  = a0 + w.AngVelSlot(i) * dt;  // integrate angle
                const Vec2 p  = c - RotateVec(a, lc);       // origin from new COM + angle
                w.CommitSlotPosition(i, p, a);
            }
        }

        void Baumgarte::Solve(SolverContext& ctx)
        {
            PhysicsWorld& w = *ctx.world;
            const Real dt = ctx.dt;
            const Real invDt = (dt > Real(0)) ? (Real(1) / dt) : Real(0);
            const std::uint32_t iters = w.VelIters();
            const std::uint32_t n = ctx.contactCount;

            ++m_stamp;
            const std::uint32_t stamp = m_stamp;

            // 1) Integrate dynamic velocities over the full dt (the solver owns
            //    dynamic integration -- the world no longer does it inline).
            IntegrateVelocities(ctx);

            // 2) Prepare per contact point: effective masses, the restitution
            //    target, the Baumgarte bias, + warm start. Ports the Lua
            //    precompute loop (SequentialImpulse.lua:39-83) FAITHFULLY: the
            //    warm-start impulse is applied to the body velocities INLINE
            //    (per contact), so contact k's warm-start affects the velocities
            //    contact k+1 reads for its restitution target -- exactly the Lua
            //    Gauss-Seidel ordering. Per-point prepared scalars live in m_prep
            //    (reused scratch); the shared ContactConstraint geometry stays
            //    untouched so the world pool remains solver-agnostic.
            m_prep.clear();
            m_prep.resize(n);

            for (std::uint32_t c = 0; c < n; ++c)
            {
                const ContactConstraint& cc = ctx.contacts[c];
                const Vec2 nrm = cc.normal;
                const Vec2 tangent(-nrm.y, nrm.x);

                const Real iMa = cc.invMassA, iIa = cc.invInertiaA;
                const Real iMb = cc.invMassB, iIb = cc.invInertiaB;
                const bool dynB = cc.bodyBIsBody && iMb > Real(0);

                // Read current velocities (after IntegrateVelocities + any prior
                // contact's warm-start application this loop -- Lua ordering).
                Vec2 vA = w.VelSlot(cc.bodyA);
                Real wA = w.AngVelSlot(cc.bodyA);
                Vec2 vB(Real(0), Real(0));
                Real wB = Real(0);
                if (cc.bodyBIsBody)
                {
                    // Kinematic/dynamic B has a velocity; static B is all zeros.
                    vB = w.VelSlot(cc.bodyB);
                    wB = w.AngVelSlot(cc.bodyB);
                }

                // Combined restitution: max of A,B (Lua used max(rest[a],rest[b])).
                // GenerateContacts already stored cc.restitution = max(restA,restB).
                const Real e = cc.restitution;

                PointPrep* prep = &m_prep[c].pt[0];
                bool wroteVel = false;

                for (int p = 0; p < cc.pointCount; ++p)
                {
                    const ContactConstraintPoint& cp = cc.points[p];
                    PointPrep& pr = prep[p];

                    const Vec2 rA = cp.anchorA;
                    const Vec2 rB = cp.anchorB;

                    // Effective normal mass: 1/(iMa+iMb + iIa*(rA x n)^2 + iIb*(rB x n)^2).
                    const Real rnA = CrossRP(rA, nrm);
                    const Real rnB = CrossRP(rB, nrm);
                    const Real kN = iMa + iMb + rnA * rnA * iIa + rnB * rnB * iIb;
                    pr.massN = (kN > Real(0)) ? (Real(1) / kN) : Real(0);

                    const Real rtA = CrossRP(rA, tangent);
                    const Real rtB = CrossRP(rB, tangent);
                    const Real kT = iMa + iMb + rtA * rtA * iIa + rtB * rtB * iIb;
                    pr.massT = (kT > Real(0)) ? (Real(1) / kT) : Real(0);

                    // Relative normal velocity at the contact (A relative to B
                    // along the B->A normal). Approaching -> vn < 0. The Lua used
                    // the point-velocity form v + cross(w, r).
                    const Vec2 dv = (vA + Vec2(-wA * rA.y, wA * rA.x))
                                  - (vB + Vec2(-wB * rB.y, wB * rB.x));
                    const Real vn = Dot(dv, nrm);

                    // Restitution target only above the approach-speed threshold.
                    // Lua: restVel = (vn < -REST_VEL) and (-e*vn) or 0.
                    pr.restVel = (vn < -kRestVel) ? (-e * vn) : Real(0);

                    // Baumgarte positional bias. Lua: bias = BETA/dt *
                    // max(depth - SLOP, 0). depth (positive penetration) ==
                    // -baseSeparation (baseSeparation < 0 means penetration).
                    const Real depth = -cp.baseSeparation;
                    pr.bias = kBeta * invDt * std::max(depth - kSlop, Real(0));

                    // Warm start: seed accumulators from the prior step's cache,
                    // keyed by the stable manifold-point id.
                    auto it = m_cache.find(cp.id);
                    if (it != m_cache.end())
                    {
                        pr.jn = it->second.normalImpulse;
                        pr.jt = it->second.tangentImpulse;

                        // Apply the cached impulse to the local velocities (Lua
                        // applyAt on both bodies). Normal points B->A: A gets +P,
                        // B gets -P. applyAt no-ops on invMass == 0 (immovable B).
                        const Vec2 P = nrm * pr.jn + tangent * pr.jt;
                        vA += P * iMa;
                        wA += iIa * CrossRP(rA, P);
                        if (dynB)
                        {
                            vB -= P * iMb;
                            wB -= iIb * CrossRP(rB, P);
                        }
                        wroteVel = true;
                    }
                    else
                    {
                        pr.jn = Real(0);
                        pr.jt = Real(0);
                    }
                }

                m_prep[c].pointCount = cc.pointCount;

                // Commit the warm-started velocities so the next contact (and the
                // iteration loop) sees them (Lua applied them in place on w.*).
                if (wroteVel)
                {
                    w.SetVelSlot(cc.bodyA, vA);
                    w.SetAngVelSlot(cc.bodyA, wA);
                    if (dynB)
                    {
                        w.SetVelSlot(cc.bodyB, vB);
                        w.SetAngVelSlot(cc.bodyB, wB);
                    }
                }
            }

            // 3) velIters Gauss-Seidel velocity passes. Joints are prepared ONCE
            //    before the loop (the Lua :init, full dt -- Baumgarte is single-
            //    step) then solved FIRST each iteration (the Lua SequentialImpulse
            //    order, line 88: joints solve before contacts each iter). Then per
            //    contact-point: normal impulse with the accumulated >=0 clamp,
            //    friction in the Coulomb cone. Ports SequentialImpulse.lua:87-128.
            for (std::uint32_t k = 0; k < ctx.jointCount; ++k)
            {
                if (ctx.joints[k].joint != nullptr)
                {
                    ctx.joints[k].joint->Prepare(w, dt);
                }
            }
            for (std::uint32_t iter = 0; iter < iters; ++iter)
            {
                // Joints solve first each iteration (Lua ordering).
                for (std::uint32_t k = 0; k < ctx.jointCount; ++k)
                {
                    if (ctx.joints[k].joint != nullptr)
                    {
                        ctx.joints[k].joint->SolveVelocity(w);
                    }
                }

                for (std::uint32_t c = 0; c < n; ++c)
                {
                    const ContactConstraint& cc = ctx.contacts[c];
                    const Vec2 nrm = cc.normal;
                    const Vec2 tangent(-nrm.y, nrm.x);
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

                    PointPrep* prep = &m_prep[c].pt[0];
                    for (int p = 0; p < cc.pointCount; ++p)
                    {
                        const ContactConstraintPoint& cp = cc.points[p];
                        PointPrep& pr = prep[p];
                        const Vec2 rA = cp.anchorA;
                        const Vec2 rB = cp.anchorB;

                        // ---- normal impulse (accumulated clamp >= 0) ----------
                        Vec2 dv = (vA + Vec2(-wA * rA.y, wA * rA.x))
                                - (vB + Vec2(-wB * rB.y, wB * rB.x));
                        Real vn = Dot(dv, nrm);
                        // Lua: dJn = -(vn - restVel - bias) * massN.
                        Real dJn = -(vn - pr.restVel - pr.bias) * pr.massN;
                        const Real jn0 = pr.jn;
                        pr.jn = std::max(Real(0), jn0 + dJn);
                        dJn = pr.jn - jn0;
                        {
                            const Vec2 P = nrm * dJn;
                            vA += P * iMa;
                            wA += iIa * CrossRP(rA, P);
                            if (dynB)
                            {
                                vB -= P * iMb;
                                wB -= iIb * CrossRP(rB, P);
                            }
                        }

                        // ---- friction impulse (|jt| <= mu * jn) ---------------
                        dv = (vA + Vec2(-wA * rA.y, wA * rA.x))
                           - (vB + Vec2(-wB * rB.y, wB * rB.x));
                        const Real vt = Dot(dv, tangent);
                        Real dJt = -vt * pr.massT;
                        const Real maxF = cc.friction * pr.jn;
                        const Real jt0 = pr.jt;
                        pr.jt = std::clamp(jt0 + dJt, -maxF, maxF);
                        dJt = pr.jt - jt0;
                        {
                            const Vec2 P = tangent * dJt;
                            vA += P * iMa;
                            wA += iIa * CrossRP(rA, P);
                            if (dynB)
                            {
                                vB -= P * iMb;
                                wB -= iIb * CrossRP(rB, P);
                            }
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

            // 4) Integrate dynamic positions over the full dt (solver owns it).
            IntegratePositions(ctx);

            // 5) Persist warm-start impulses + evict stale entries. Ports
            //    SequentialImpulse.lua:130-139 (CACHE_LIFE = 2). Keyed by id.
            // insert_or_assign avoids the default-construct+insert on NEW ids that
            // operator[] performs, eliminating the per-step heap allocation when
            // contact ids churn (e.g. bodies cycling in/out of sleep in P2.4).
            for (std::uint32_t c = 0; c < n; ++c)
            {
                const ContactConstraint& cc = ctx.contacts[c];
                const PointPrep* prep = &m_prep[c].pt[0];
                for (int p = 0; p < cc.pointCount; ++p)
                {
                    const ContactConstraintPoint& cp = cc.points[p];
                    m_cache.insert_or_assign(cp.id, CacheEntry{ prep[p].jn,
                                                                prep[p].jt,
                                                                stamp });
                }
            }
            // Bounded cache: drop entries unused for more than kCacheLife stamps.
            // Delete-only iteration order is unobservable (warm-start seeds load
            // via find(id), not iteration) -> determinism preserved.
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
        }

    } // namespace Physics
} // namespace Arcane
