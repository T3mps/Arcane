// PhysicsWorld.cpp -- the 2D-physics orchestration core, KINEMATIC subset
// (port of the kinematic path of PhysicsWorld.lua).
//
// See PhysicsWorld.hpp for the contract + the PORT BOUNDARY (what P1.8 ports
// vs what P2/P3 add). This TU implements Create/AddBody/RemoveBody/IsValid,
// the kinematic Step (prev snapshot + kinematic integration + broadphase
// update, then ContactManager::Step), QueryAABB, the two-granularity event
// gating glue (SetBodyEvents / SetEventsEnabled), and _staticCandidates.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/PhysicsWorld.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>

#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>
#include <Arcane/Physics/Broadphase/SpatialHash.hpp>
#include <Arcane/Physics/Broadphase/SweepAndPrune.hpp>
#include <Arcane/Physics/Narrowphase/GeometryKernel.hpp> // AabbOverlap (QueryAABB)
#include <Arcane/Physics/Narrowphase/Dispatch.hpp>       // CollideShapes (contact gen)
#include <Arcane/Physics/Solver/SoftStep.hpp>            // SoftStep solver impl
#include <Arcane/Physics/Solver/Baumgarte.hpp>           // Baumgarte oracle impl (A/B)
#include <Arcane/Physics/Island.hpp>                     // island sleep pass (stage 4)
#include <Arcane/Physics/Joints/Joints.hpp>              // joint set + MakeJoint factory (P2.5)

namespace Arcane
{
    namespace Physics
    {
        namespace
        {
            std::unique_ptr<IBroadphase> MakeBroadphase(const WorldDef& def)
            {
                // PORT + MODERNIZE: the Lua selected by string, defaulting to
                // SpatialHash; we default to DynamicTree (the P1.6 decision).
                switch (def.broadphase)
                {
                case BroadphaseKind::Hash:
                    return std::make_unique<SpatialHash>(def.hashCellSize);
                case BroadphaseKind::Sap:
                    return std::make_unique<SweepAndPrune>();
                case BroadphaseKind::Tree:
                default:
                    return std::make_unique<DynamicTree>();
                }
            }

            std::unique_ptr<ISolver> MakeSolver(const WorldDef& def)
            {
                // P2.3 A/B seam: pick the installed solver by WorldDef::solverKind
                // (default SoftStep -- the P2.2 TGS Soft solver; Baumgarte is the
                // retained PGS oracle that runs the same scenes for cross-check).
                switch (def.solverKind)
                {
                case SolverKind::Baumgarte:
                    return std::make_unique<Baumgarte>();
                case SolverKind::SoftStep:
                default:
                    return std::make_unique<SoftStep>();
                }
            }
        } // namespace

        PhysicsWorld::PhysicsWorld(const WorldDef& def)
            : m_moverBroadphase(MakeBroadphase(def))
            , m_gravityX(def.gravityX)
            , m_gravityY(def.gravityY)
            , m_substepCount(def.substepCount > 0u ? def.substepCount : 1u)
            , m_contactHertz(def.contactHertz)
            , m_contactDampingRatio(def.contactDampingRatio)
            , m_restitutionThreshold(def.restitutionThreshold)
            , m_contactPushMaxVelocity(def.contactPushMaxVelocity)
            , m_velIters(def.velIters > 0u ? def.velIters : 1u)
            , m_solver(MakeSolver(def))
        {
            // Optional tile statics: own a TileGrid over the passability seam if
            // one was provided (ports the Lua `tileGrid = map and TileGrid.new`).
            if (def.passability != nullptr)
            {
                m_tileGrid = std::make_unique<TileGrid>(*def.passability,
                                                        def.tileCellSize,
                                                        def.tileOrigin);
            }
        }

        PhysicsWorld::~PhysicsWorld() = default;

        void PhysicsWorld::EnsureCapacity(std::uint32_t n)
        {
            if (n <= m_posX.size())
            {
                return;
            }
            // Amortized growth: at least double the current capacity so that
            // a one-at-a-time fill (AddBody(count+1)) does not realloc all ~12
            // SoA vectors on every add.  Free-list / steady-state behaviour is
            // unchanged; only the initial fill is faster.
            const std::uint32_t next =
                std::max(n, static_cast<std::uint32_t>(m_posX.capacity() * 2));
            m_posX.resize(next);
            m_posY.resize(next);
            m_prevX.resize(next);
            m_prevY.resize(next);
            m_velX.resize(next);
            m_velY.resize(next);
            m_btype.resize(next);
            m_evtOn.resize(next);
            m_alive.resize(next);
            m_sensor.resize(next);
            m_gen.resize(next, 0u); // gen kept zero-filled (live slots start at 1)
            m_shape.resize(next);

            // Dynamics SoA (P2.1). Reals default-zero (no inverse mass/inertia ->
            // immovable like a static until AddBody fills a Dynamic slot); awake
            // defaults to 1 so an un-filled slot is never treated as a sleeper.
            // AddBody overwrites every field for the slot it returns, so these
            // fills only matter for the never-touched tail; keeping them
            // self-consistent avoids surprises if a future path scans the SoA.
            m_angle.resize(next, Real(0));
            m_angVel.resize(next, Real(0));
            m_invMass.resize(next, Real(0));
            m_invInertia.resize(next, Real(0));
            m_rest.resize(next, Real(0));
            m_fric.resize(next, Real(0));
            m_linDamp.resize(next, Real(0));
            m_sleepTimer.resize(next, Real(0));
            m_awake.resize(next, std::uint8_t(1));
            m_bullet.resize(next, std::uint8_t(0));
        }

        Aabb2 PhysicsWorld::SlotAabb(std::uint32_t i) const noexcept
        {
            Transform xf{ Vec2(m_posX[i], m_posY[i]), Real(0) };
            return m_shape[i].ComputeAABB(xf);
        }

        BodyHandle PhysicsWorld::AddBody(const BodyDef& def)
        {
            // Reuse a free slot or append (ports addBody's free-list pop).
            std::uint32_t idx;
            if (!m_free.empty())
            {
                idx = m_free.back();
                m_free.pop_back();
            }
            else
            {
                idx = m_count;
                EnsureCapacity(m_count + 1);
                ++m_count;
            }

            m_posX[idx]  = def.position.x;
            m_posY[idx]  = def.position.y;
            m_prevX[idx] = def.position.x;
            m_prevY[idx] = def.position.y;
            m_velX[idx]  = Real(0);
            m_velY[idx]  = Real(0);
            m_btype[idx] = static_cast<std::uint8_t>(def.type);
            m_evtOn[idx] = def.eventsEnabled ? std::uint8_t(1) : std::uint8_t(0);
            m_sensor[idx] = def.isSensor ? std::uint8_t(1) : std::uint8_t(0);
            m_alive[idx]  = 1;
            // Bump generation (live slots start at 1; bump on BOTH add + remove
            // so stale handles never match). Ports gen[idx] = (gen[idx] or 0)+1.
            m_gen[idx] += 1u;
            m_shape[idx] = def.shape;

            // ---- dynamics state (P2.1) -- ports addBody (PhysicsWorld.lua:
            // 229-250). Static/Kinematic get the zero defaults (no inverse
            // mass/inertia -> never integrated/solved). A Dynamic body derives
            // mass + rotational inertia from Shape::ComputeMass(density).
            m_angle[idx]      = Real(0);
            m_angVel[idx]     = Real(0);
            m_invMass[idx]    = Real(0);
            m_invInertia[idx] = Real(0);
            m_rest[idx]       = def.restitution;
            m_fric[idx]       = def.friction;
            m_linDamp[idx]    = def.linearDamping;
            m_sleepTimer[idx] = Real(0);
            m_awake[idx]      = 1;
            m_bullet[idx]     = def.bullet ? std::uint8_t(1) : std::uint8_t(0);

            if (def.type == BodyType::Dynamic)
            {
                // A dynamic AABB must be fixedRotation: an axis-aligned box has
                // no meaningful orientation in this fixed-rotation engine
                // (ports the Lua assert, PhysicsWorld.lua:238).
                assert((def.shape.kind != ShapeKind::Aabb || def.fixedRotation) &&
                       "dynamic AABB shapes must be fixedRotation "
                       "(axis-aligned by definition)");

                // Mass + rotational inertia from Shape::ComputeMass(density)
                // (the P1.1 MassData -- verified equivalent to the Lua massProps,
                // so we do NOT re-port massProps). md.inertia is about the
                // shape's centroid, which matches the Lua's per-shape inertia.
                const MassData md = def.shape.ComputeMass(def.density);
                Real m       = md.mass;
                Real inertia = md.inertia;

                // Optional mass override: scale inertia by (mass/computedMass)
                // then use the override mass (ports lines 241-243). Guard against
                // a zero computed mass (degenerate shape) so the scale is safe.
                if (def.mass > Real(0))
                {
                    if (m > Real(0))
                    {
                        inertia = inertia * (def.mass / m);
                    }
                    m = def.mass;
                }

                m_invMass[idx]    = m > Real(0) ? Real(1) / m : Real(0);
                m_invInertia[idx] = (def.fixedRotation || inertia <= Real(0))
                                        ? Real(0)
                                        : Real(1) / inertia;
            }

            if (def.type == BodyType::Static)
            {
                m_staticList.push_back(idx);
            }
            else
            {
                // Kinematic + Dynamic register in the mover broadphase.
                // Kinematic movers get begin/stay/end events via the broadphase
                // Pairs() stream AND via the kinematic-vs-static-body loop in
                // ContactManager::Step.  Dynamic movers get mover-mover events
                // via broadphase Pairs() only -- dynamic-vs-static-BODY events
                // are intentionally KINEMATIC-ONLY (faithful to
                // ContactManager.lua:150); the solver owns dynamic-vs-static
                // response, which arrives in P2.1.
                m_moverBroadphase->Update(idx, SlotAabb(idx));
            }
            return BodyHandle{ idx, m_gen[idx] };
        }

        void PhysicsWorld::RemoveBody(BodyHandle h)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t idx = h.index;
            m_alive[idx] = 0;
            m_gen[idx] += 1u; // invalidates the handle (stale-handle invariant)

            if (static_cast<BodyType>(m_btype[idx]) == BodyType::Static)
            {
                // staticList is an unsorted no-duplicate index bag -- swap-and-pop
                // is O(1) and order-independent (pair keys are canonically keyed
                // (min,max) so static-loop order does not affect determinism).
                for (std::size_t i = 0; i < m_staticList.size(); ++i)
                {
                    if (m_staticList[i] == idx)
                    {
                        m_staticList[i] = m_staticList.back();
                        m_staticList.pop_back();
                        break;
                    }
                }
            }
            else
            {
                m_moverBroadphase->Remove(idx);
            }

            m_contacts.DropBody(idx);
            m_solver->DropBody(idx); // drop warm-start state for the recycled slot

            // Drop joints referencing the destroyed body (ports PhysicsWorld.lua
            // 281-286: `j.a.idx == idx or j.b.idx == idx`). Match by HANDLE index
            // (stable from construction, independent of Prepare). Iterate in
            // reverse so the swap-free erase keeps the surviving joints' order.
            for (std::size_t i = m_joints.size(); i-- > 0;)
            {
                const Joint* j = m_joints[i].get();
                const BodyHandle ha = j->HandleA();
                const BodyHandle hb = j->HandleB();
                if ((ha.generation != 0u && ha.index == idx) ||
                    (hb.generation != 0u && hb.index == idx))
                {
                    m_joints.erase(m_joints.begin() + static_cast<std::ptrdiff_t>(i));
                }
            }

            m_shape[idx] = Shape{}; // release polygon storage
            m_free.push_back(idx);
        }

        Joint* PhysicsWorld::AddJoint(const JointDef& def)
        {
            // Build the concrete joint (the Lua Joints.make factory). Unknown
            // kinds return nullptr (never thrown).
            std::unique_ptr<Joint> j = MakeJoint(*this, def);
            if (!j)
            {
                return nullptr;
            }
            Joint* raw = j.get();
            m_joints.push_back(std::move(j));

            // Wake the jointed bodies so a sleeping captive rejoins the solve
            // (ports addJoint's def.a:wake() / def.b:wake()).
            Wake(def.a);
            Wake(def.b);
            return raw;
        }

        void PhysicsWorld::RemoveJoint(Joint* j)
        {
            if (j == nullptr)
            {
                return;
            }
            for (std::size_t i = 0; i < m_joints.size(); ++i)
            {
                if (m_joints[i].get() == j)
                {
                    m_joints.erase(m_joints.begin() + static_cast<std::ptrdiff_t>(i));
                    return;
                }
            }
        }

        bool PhysicsWorld::IsValid(BodyHandle h) const noexcept
        {
            // Ports handleValid: in-range index, matching generation, alive.
            // gen==0 is NEVER a live slot (AddBody bumps 0->1 on first use), so
            // the sentinel kInvalidBody{0,0} can never match a live body.
            return h.index < m_count && m_gen[h.index] == h.generation &&
                   m_alive[h.index] != 0;
        }

        Vec2 PhysicsWorld::Position(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return Vec2(Real(0), Real(0));
            }
            return Vec2(m_posX[h.index], m_posY[h.index]);
        }

        void PhysicsWorld::SetPosition(BodyHandle h, Vec2 p)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            // Teleport: prev snaps too (no lerp smear) -- ports setPosition.
            m_posX[i]  = p.x;
            m_posY[i]  = p.y;
            m_prevX[i] = p.x;
            m_prevY[i] = p.y;
            if (static_cast<BodyType>(m_btype[i]) != BodyType::Static)
            {
                m_moverBroadphase->Update(i, SlotAabb(i));
            }
        }

        void PhysicsWorld::MovePosition(BodyHandle h, Vec2 p)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            // Step-managed-prev move: write pos + the mover-broadphase AABB but
            // leave prev untouched (Step owns prev). Ports the Lua slideMove
            // write-back: w.posX[i] = x; w.posY[i] = y; moverHash:update(...).
            m_posX[i] = p.x;
            m_posY[i] = p.y;
            if (static_cast<BodyType>(m_btype[i]) != BodyType::Static)
            {
                m_moverBroadphase->Update(i, SlotAabb(i));
            }
        }

        Vec2 PhysicsWorld::Velocity(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return Vec2(Real(0), Real(0));
            }
            return Vec2(m_velX[h.index], m_velY[h.index]);
        }

        void PhysicsWorld::SetVelocity(BodyHandle h, Vec2 v)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            const BodyType bt = static_cast<BodyType>(m_btype[i]);
            // Kinematic + Dynamic accept velocity; Static ignores (ports
            // setVelocity). Setting a Dynamic body's velocity WAKES it (line
            // 102) so a sleeping body re-enters integration next Step.
            if (bt == BodyType::Kinematic || bt == BodyType::Dynamic)
            {
                m_velX[i] = v.x;
                m_velY[i] = v.y;
                if (bt == BodyType::Dynamic)
                {
                    m_awake[i]      = 1;
                    m_sleepTimer[i] = Real(0);
                }
            }
        }

        void PhysicsWorld::ApplyImpulse(BodyHandle h, Vec2 impulse)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            // Dynamic only; wakes (ports applyImpulse, the no-point branch).
            if (static_cast<BodyType>(m_btype[i]) != BodyType::Dynamic)
            {
                return;
            }
            m_awake[i]      = 1;
            m_sleepTimer[i] = Real(0);
            m_velX[i] += impulse.x * m_invMass[i];
            m_velY[i] += impulse.y * m_invMass[i];
        }

        void PhysicsWorld::ApplyImpulse(BodyHandle h, Vec2 impulse, Vec2 worldPoint)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            // Dynamic only; wakes (ports applyImpulse, the px,py branch). Linear
            // part as above, plus an angular term from the lever arm about the
            // body center: angVel += cross(p - center, i) * invInertia.
            if (static_cast<BodyType>(m_btype[i]) != BodyType::Dynamic)
            {
                return;
            }
            m_awake[i]      = 1;
            m_sleepTimer[i] = Real(0);
            m_velX[i] += impulse.x * m_invMass[i];
            m_velY[i] += impulse.y * m_invMass[i];
            const Real rx = worldPoint.x - m_posX[i];
            const Real ry = worldPoint.y - m_posY[i];
            m_angVel[i] += (rx * impulse.y - ry * impulse.x) * m_invInertia[i];
        }

        void PhysicsWorld::Wake(BodyHandle h)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            // Only Dynamic bodies have a sleep state to clear (ports Body:wake).
            if (static_cast<BodyType>(m_btype[i]) == BodyType::Dynamic)
            {
                m_awake[i]      = 1;
                m_sleepTimer[i] = Real(0);
            }
        }

        bool PhysicsWorld::IsAwake(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return false;
            }
            // Static/Kinematic never sleep -> always report awake (their m_awake
            // stays 1). Ports Body:isAwake.
            return m_awake[h.index] != 0;
        }

        Real PhysicsWorld::GetAngle(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return Real(0);
            }
            return m_angle[h.index];
        }

        void PhysicsWorld::SetAngle(BodyHandle h, Real angle)
        {
            if (!IsValid(h))
            {
                return;
            }
            m_angle[h.index] = angle;
        }

        Vec2 PhysicsWorld::DrawPosition(BodyHandle h, Real alpha) const noexcept
        {
            if (!IsValid(h))
            {
                return Vec2(Real(0), Real(0));
            }
            const std::uint32_t i = h.index;
            // Render-boundary lerp prev..curr (ports drawPosition).
            return Vec2(m_prevX[i] + (m_posX[i] - m_prevX[i]) * alpha,
                        m_prevY[i] + (m_posY[i] - m_prevY[i]) * alpha);
        }

        const Shape* PhysicsWorld::GetShape(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return nullptr;
            }
            return &m_shape[h.index];
        }

        BodyType PhysicsWorld::GetType(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return BodyType::Static;
            }
            return static_cast<BodyType>(m_btype[h.index]);
        }

        bool PhysicsWorld::IsSensor(BodyHandle h) const noexcept
        {
            return IsValid(h) && m_sensor[h.index] != 0;
        }

        void PhysicsWorld::OnContact(ContactManager::Listener fn)
        {
            m_contacts.SetListener(std::move(fn));
        }

        void PhysicsWorld::SetBodyEvents(BodyHandle h, bool on)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            const bool was = m_evtOn[i] != 0;
            m_evtOn[i] = on ? std::uint8_t(1) : std::uint8_t(0);
            // true->false: Disarm (drop, no synthetic end). false->true: Rearm
            // (fresh begin for overlapping). Ports _setBodyEvents.
            if (was && !on)
            {
                m_contacts.Disarm(i);
            }
            else if (on && !was)
            {
                m_contacts.Rearm(*this, i);
            }
        }

        void PhysicsWorld::SetEventsEnabled(bool on)
        {
            if (m_eventsEnabled == on)
            {
                return;
            }
            m_eventsEnabled = on;
            // on->off: Disarm all. off->on: Rearm all overlapping. Ports
            // setEventsEnabled.
            if (on)
            {
                m_contacts.Rearm(*this);
            }
            else
            {
                m_contacts.Disarm();
            }
        }

        void PhysicsWorld::Step(Real dt)
        {
            // STEP ORDER (P2.2 -- Box2D v3 TGS Soft restructure of the P1.8/P2.1
            // pipeline). The P2.1 INLINE dynamic gravity-integrate (old stage 1)
            // and dynamic position-integrate (old stage 4) are SUPERSEDED: the
            // Soft Step solver now OWNS dynamic velocity + position integration,
            // folded INTO its sub-step loop (the proven-stable v3 form). Dynamics
            // are therefore integrated EXACTLY ONCE per Step -- by the solver --
            // with no double-integration.
            //
            //   stage 1: prev snapshot (ALL) + KINEMATIC integrate (once/step) +
            //            kinematic mover-broadphase update.
            //   stage 2: solver contact generation (Part A) -- manifolds for
            //            awake-dynamic pairs (vs tile spans + static bodies via
            //            StaticCandidates; vs mover-mover pairs involving a
            //            dynamic, narrowed). Builds m_contactConstraints.
            //   stage 3: SOFT STEP SOLVE (Part B) -- the solver runs Prepare ->
            //            [per sub-step: integrate vel, warm-start, solve(bias),
            //            integrate pos, relax] -> restitution -> store impulses
            //            -> commit dynamic positions + mover-broadphase update.
            //   stage 4: bullet GJK-TOI clamp (P3.1 CCD) -- sweep each isBullet
            //            body prev->curr vs statics; clamp to time-of-impact so a
            //            thin static wall cannot be tunneled. (The speculative
            //            margin in stage 2 is the inline CCD for fast dynamics;
            //            this is the discrete backup, primarily for kinematics.)
            //            CCD runs after the solver commits positions, before
            //            island/events.
            //   stage 5: island sleep bookkeeping            (P2.4 -- deferred)
            //   stage 6: contacts:step (events + gating + deferred flush)
            //
            // Free-fall parity: with NO contacts the solver's sub-step loop is a
            // pure semi-implicit integrate (gravity per sub-step, position per
            // sub-step). Summed over substepCount sub-steps of length h = dt/N
            // this equals the P2.1 single-step semi-implicit Euler to f32
            // tolerance (the PhysicsDynamics free-fall test's margins absorb the
            // sub-step regrouping). Index-ordered, no wall-clock, no fast-math.

            // ---- stage 1: prev snapshot (all) + kinematic integrate ----------
            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0)
                {
                    continue;
                }
                m_prevX[i] = m_posX[i];
                m_prevY[i] = m_posY[i];

                // Kinematic bodies integrate ONCE per Step (unchanged from P2.1).
                // Dynamic gravity/damping + position now live in the solver's
                // sub-step loop (stage 3); a dynamic body with no contacts still
                // gets the same semi-implicit fall, just regrouped over sub-steps.
                if (static_cast<BodyType>(m_btype[i]) == BodyType::Kinematic)
                {
                    m_posX[i] += m_velX[i] * dt;
                    m_posY[i] += m_velY[i] * dt;
                    m_moverBroadphase->Update(i, SlotAabb(i));
                }
            }

            // ---- stage 2: solver contact generation (Part A) -----------------
            GenerateContacts(dt);

            // ---- stage 3: Soft Step solve (Part B) ---------------------------
            // The solver integrates dynamic velocity + position across sub-steps,
            // solves soft contacts, applies restitution, and commits positions +
            // the mover broadphase. Run it whenever there are dynamics to move
            // (constraints OR free-falling bodies); the solver is a no-op for a
            // scene with no awake dynamics.
            // Rebuild the pooled JointConstraint array (Joint* views into the
            // world-owned m_joints) so the SolverContext keeps its P2.1 shape.
            // clear() preserves capacity -> zero steady-state alloc.
            m_jointConstraints.clear();
            for (std::size_t i = 0; i < m_joints.size(); ++i)
            {
                JointConstraint jc;
                jc.joint = m_joints[i].get();
                m_jointConstraints.push_back(jc);
            }

            {
                SolverContext ctx;
                ctx.world        = this;
                ctx.contacts     = m_contactConstraints.empty()
                                       ? nullptr
                                       : m_contactConstraints.data();
                ctx.contactCount = static_cast<std::uint32_t>(m_contactConstraints.size());
                ctx.joints       = m_jointConstraints.empty()
                                       ? nullptr
                                       : m_jointConstraints.data();
                ctx.jointCount   = static_cast<std::uint32_t>(m_jointConstraints.size());
                ctx.dt           = dt;
                ctx.substepCount = m_substepCount;
                ctx.invDt        = dt > Real(0) ? Real(1) / dt : Real(0);
                ctx.subDt        = dt / static_cast<Real>(m_substepCount);
                ctx.invSubDt     = ctx.subDt > Real(0) ? Real(1) / ctx.subDt : Real(0);
                ctx.gravity      = Vec2(m_gravityX, m_gravityY);
                m_solver->Solve(ctx);
            }

            // ---- stage 4: bullet GJK-TOI clamp (P3.1 CCD) --------------------
            // The discrete CCD backup for `isBullet` bodies vs statics. Runs
            // AFTER the solver commits dynamic positions (so a dynamic bullet's
            // post-solve sweep is correct) and BEFORE island sleep + events (so
            // they observe the clamped position). Kinematic bullets -- which the
            // solver never touches -- are clamped from their stage-1-integrated
            // position; the speculative margin (stage 2) handles fast dynamics
            // inline, with this as the safety net. CCD runs after the solver
            // commits positions, before island/events.
            BulletSweep();

            // ---- stage 5: island sleep bookkeeping (P2.4) --------------------
            // Build the per-Step constraint graph (bodies = nodes, THIS step's
            // contacts = edges), advance per-body sleep timers, and sleep any
            // island whose every member is idle past the threshold. Runs AFTER
            // the solve using this step's contacts; sleeping bodies are then
            // skipped by the next Step's GenerateContacts + solver (the awake
            // gate), freezing their positions. Ports PhysicsWorld.lua:403-452.
            Island::UpdateSleep(
                *this,
                m_contactConstraints.empty() ? nullptr : m_contactConstraints.data(),
                static_cast<std::uint32_t>(m_contactConstraints.size()),
                m_jointConstraints.empty() ? nullptr : m_jointConstraints.data(),
                static_cast<std::uint32_t>(m_jointConstraints.size()),
                dt);

            // ---- stage 6: events + gating + deferred flush -------------------
            m_contacts.Step(*this);
        }

        void PhysicsWorld::GenerateContacts(Real dt)
        {
            // Part A: build the dynamics ContactConstraint array (the Lua step()
            // stage 2 "solver contact generation"). For each awake non-sensor
            // DYNAMIC body, generate manifolds vs static candidates (tile spans +
            // static bodies) and vs mover-mover pairs involving a dynamic. A is
            // ALWAYS the dynamic body (Lua lines 377-378); B may be dynamic,
            // kinematic, static, or a tile-span virtual fixture (invMass = 0).
            //
            // Index-ordered over dynamic bodies, then the broadphase's SORTED
            // mover pairs -> deterministic. The pool only grows (clear preserves
            // capacity) -> zero steady-state allocation.
            m_contactConstraints.clear();

            // ---- P3.1 speculative-contact CCD (the PRIMARY fast-mover CCD) ---
            //
            // MODERNIZE: the speculative margin (the kSkin skin from P1.2) is
            // VELOCITY-SCALED for fast movers so the solver sees an impending wall
            // BEFORE the body reaches it. The margin is the distance the body
            // would travel this Step (|v| * dt), floored at kSkin: a SLOW body's
            // margin is just kSkin (resting/settling behavior UNCHANGED), while a
            // FAST body's margin grows to cover its full sweep so a contact is
            // generated while it is still on the near side of the wall. The Soft
            // Step solver's speculative bias (the s > 0 case: bias = s * invSubDt,
            // which caps the closing velocity to exactly close the gap in ONE
            // sub-step) then prevents the body from advancing more than the gap
            // per sub-step -- stopping tunneling WITHOUT a discrete clamp. This is
            // the modern speculative-contact CCD; the bullet GJK-TOI clamp
            // (Step stage 6) is the discrete backup for flagged bodies vs statics.
            //
            // Per-body margin: max(kSkin, |v| * dt). The query AABB pad and the
            // CollideShapes speculativeMargin both use it (the AABB must be
            // expanded by AT LEAST the margin so the wall candidate is FOUND
            // before geometric overlap, and CollideShapes must report the
            // near-touching contact within that distance).
            const Real moveDt = dt > Real(0) ? dt : Real(0);
            // Threshold for the sqrt skip (Fix 3): if speedSq <= threshSq then
            // speed * moveDt <= kSkin, so margin == kSkin (the floor) regardless.
            // Hoisted once out of both loops; guard moveDt==0 -> threshold 0 so
            // the sqrt is always taken (but moveDt==0 means dt==0 -> no motion).
            const Real threshSq = (moveDt > Real(0))
                                      ? (kSkin / moveDt) * (kSkin / moveDt)
                                      : Real(0);

            // ---- helper: append a manifold as a ContactConstraint -----------
            // A is dynamic (aIdx); bIdx is the slot for a real body or
            // kInvalidSlot for a span. invMassB/invInertiaB come from the slot
            // (0 for static/kinematic/span -> push, not pushed).
            // centerB: for a real body this is the body's world position; for a
            // tile-span virtual fixture it is the span's geometric center
            // (span.min+span.max)*0.5. anchorB = mp.point - centerB in both cases.
            // (Passing it explicitly avoids the old Vec2(0,0) fallback that made
            // anchorB wrong-looking for spans -- currently harmless because
            // invInertiaB==0 zeros the lever arm, but would break if spans ever
            // gained DOF.)
            auto emit = [&](std::uint32_t aIdx, std::uint32_t bIdx, bool bIsBody,
                            const Vec2& centerB, const Manifold& m)
            {
                if (m.pointCount <= 0)
                {
                    return;
                }
                ContactConstraint cc;
                cc.bodyA       = aIdx;
                cc.bodyB       = bIsBody ? bIdx : kInvalidSlot;
                cc.bodyBIsBody = bIsBody;
                cc.invMassA    = m_invMass[aIdx];
                cc.invInertiaA = m_invInertia[aIdx];
                cc.invMassB    = bIsBody ? m_invMass[bIdx] : Real(0);
                cc.invInertiaB = bIsBody ? m_invInertia[bIdx] : Real(0);
                cc.normal      = m.normal;

                // Combined material coefficients. Friction is the geometric mean
                // (the Lua sqrt(fricA*fricB)); restitution is the max (Box2D v3).
                const Real fricA = m_fric[aIdx];
                const Real fricB = bIsBody ? m_fric[bIdx] : fricA;
                cc.friction = std::sqrt(fricA * fricB);
                const Real restA = m_rest[aIdx];
                const Real restB = bIsBody ? m_rest[bIdx] : Real(0);
                cc.restitution = std::max(restA, restB);

                const Vec2 cA(m_posX[aIdx], m_posY[aIdx]);

                cc.pointCount = m.pointCount;
                for (int p = 0; p < m.pointCount; ++p)
                {
                    const ManifoldPoint& mp = m.points[p];
                    ContactConstraintPoint& cp = cc.points[p];
                    cp.anchorA = mp.point - cA;
                    cp.anchorB = mp.point - centerB;
                    // Manifold separation is POSITIVE for penetration; Box2D's
                    // signed separation is negative for penetration -> negate.
                    cp.baseSeparation = -mp.separation;
                    cp.id = mp.id;
                }
                m_contactConstraints.push_back(cc);
            };

            // ---- per dynamic body: vs spans + static bodies -----------------
            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0 ||
                    static_cast<BodyType>(m_btype[i]) != BodyType::Dynamic ||
                    m_awake[i] == 0 || m_sensor[i] != 0)
                {
                    continue;
                }

                // Per-body speculative margin (P3.1): max(kSkin, |v| * dt). For a
                // slow body this is kSkin (resting unchanged); for a fast body it
                // grows to cover this Step's sweep so the wall is seen pre-overlap.
                // Exact sqrt skip (Fix 3): if speedSq <= threshSq the margin is
                // just kSkin, so a slow/resting body never pays the sqrt.
                const Real speedSqA = m_velX[i] * m_velX[i] + m_velY[i] * m_velY[i];
                const Real specMargin = (speedSqA > threshSq)
                                            ? std::sqrt(speedSqA) * moveDt
                                            : kSkin;

                const Transform xfA{ Vec2(m_posX[i], m_posY[i]), Real(0) };
                const Aabb box = SlotAabb(i);
                // Query pad: at least the legacy +/-2 broadphase skin, expanded to
                // the speculative margin so a fast mover's wall candidate is FOUND
                // before geometric overlap (otherwise CollideShapes never sees it).
                const Real pad = std::max(Real(2), specMargin);
                Aabb2 query;
                query.min = Vec2(box.min.x - pad, box.min.y - pad);
                query.max = Vec2(box.max.x + pad, box.max.y + pad);
                StaticCandidates(query, m_genSpans, m_genStatics);

                // tile spans (Aabb2 rects) -> collide dynamic shape vs span-AABB.
                for (std::size_t s = 0; s < m_genSpans.size(); ++s)
                {
                    const Aabb2& span = m_genSpans[s];
                    const Vec2 c = (span.min + span.max) * Real(0.5);
                    const Vec2 he = (span.max - span.min) * Real(0.5);
                    const Shape spanShape = MakeAabb(he.x, he.y);
                    const Transform xfB{ c, Real(0) };
                    const Manifold m =
                        CollideShapes(m_shape[i], xfA, spanShape, xfB, specMargin);
                    emit(i, kInvalidSlot, /*bIsBody=*/false, /*centerB=*/c, m);
                }

                // static bodies (non-sensor).
                for (std::size_t s = 0; s < m_genStatics.size(); ++s)
                {
                    const std::uint32_t idx = m_genStatics[s];
                    if (m_sensor[idx] != 0)
                    {
                        continue;
                    }
                    const Vec2 posB(m_posX[idx], m_posY[idx]);
                    const Transform xfB{ posB, Real(0) };
                    const Manifold m =
                        CollideShapes(m_shape[i], xfA, m_shape[idx], xfB, specMargin);
                    emit(i, idx, /*bIsBody=*/true, /*centerB=*/posB, m);
                }
            }

            // ---- mover-mover pairs involving a dynamic ----------------------
            // The broadphase emits SORTED pairs (a < b). For each pair where at
            // least one body is dynamic, narrow to true AABB overlap, orient A =
            // dynamic, and generate. Sleeping dynamics are woken by an awake
            // mover touch (ports lines 369-381).
            m_moverBroadphase->Pairs(m_genPairs);
            for (std::size_t k = 0; k < m_genPairs.size(); ++k)
            {
                std::uint32_t a = m_genPairs[k].a;
                std::uint32_t b = m_genPairs[k].b;
                if (m_alive[a] == 0 || m_alive[b] == 0 ||
                    m_sensor[a] != 0 || m_sensor[b] != 0)
                {
                    continue;
                }
                const bool da = static_cast<BodyType>(m_btype[a]) == BodyType::Dynamic;
                const bool db = static_cast<BodyType>(m_btype[b]) == BodyType::Dynamic;
                if (!da && !db)
                {
                    continue; // kinematic-kinematic: no dynamic response
                }

                // Per-pair speculative margin (P3.1): the larger of the two
                // bodies' velocity-scaled margins (the relative approach speed is
                // bounded by their sum, but using the max keeps it cheap and still
                // catches a fast mover closing on a slow/resting one), floored at
                // kSkin. Expand the tight-AABB overlap test by it so a fast pair is
                // not rejected here before CollideShapes can emit the speculative
                // contact (the solver's s > 0 bias then caps the closing velocity).
                // Exact sqrt skip (Fix 3): use the faster of the pair; only a fast
                // pair pays the sqrt (slow pairs collapse to the kSkin floor).
                const Real speedSqAm = m_velX[a] * m_velX[a] + m_velY[a] * m_velY[a];
                const Real speedSqBm = m_velX[b] * m_velX[b] + m_velY[b] * m_velY[b];
                const Real maxSpeedSq = std::max(speedSqAm, speedSqBm);
                const Real pairMargin = (maxSpeedSq > threshSq)
                                            ? std::sqrt(maxSpeedSq) * moveDt
                                            : kSkin;
                Aabb2 boxA = SlotAabb(a);
                Aabb2 boxB = SlotAabb(b);
                boxA.min -= Vec2(pairMargin, pairMargin);
                boxA.max += Vec2(pairMargin, pairMargin);
                if (!AabbOverlap(boxA, boxB))
                {
                    continue; // broadphase candidate that is not a true overlap
                }

                // Wake a sleeping dynamic touched by an awake mover (ports the
                // Lua wake rules, PhysicsWorld.lua:369-382). P2.4's island pass
                // now sleeps idle dynamics, so this wake path is live: a sleeping
                // dynamic that an awake mover moves into must rejoin the solve.
                if (da && m_awake[a] == 0 && (!db || m_awake[b] != 0))
                {
                    m_awake[a] = 1;
                    m_sleepTimer[a] = Real(0);
                }
                if (db && m_awake[b] == 0 && (!da || m_awake[a] != 0))
                {
                    m_awake[b] = 1;
                    m_sleepTimer[b] = Real(0);
                }

                // Orient A = dynamic (lines 377-378). If both dynamic, keep the
                // sorted (a,b) order so A is the lower index (deterministic).
                std::uint32_t ia = a, ib = b;
                if (!da)
                {
                    ia = b;
                    ib = a;
                }
                if (m_awake[ia] == 0)
                {
                    continue; // A (dynamic) asleep -> no constraint
                }
                const Vec2 posA(m_posX[ia], m_posY[ia]);
                const Vec2 posB(m_posX[ib], m_posY[ib]);
                const Transform xfA{ posA, Real(0) };
                const Transform xfB{ posB, Real(0) };
                const Manifold m =
                    CollideShapes(m_shape[ia], xfA, m_shape[ib], xfB, pairMargin);
                emit(ia, ib, /*bIsBody=*/true, /*centerB=*/posB, m);
            }
        }

        void PhysicsWorld::BulletSweep()
        {
            // P3.1 CCD bullet clamp (port of PhysicsWorld.lua:313-320). For each
            // alive `isBullet` body, cast the swept shape from its start-of-step
            // position (prev, snapshotted in Step stage 1) along this Step's net
            // displacement vs STATICS ONLY, and clamp the position to the time of
            // impact so a thin static wall cannot be tunneled.
            //
            // The Lua epsilon: clamp = max(0, hit.t - 0.001). Pulling the body a
            // hair short of the surface keeps it just OUTSIDE the wall (no
            // depenetration churn next Step).
            constexpr Real kBulletEpsilon = Real(0.001);

            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0 || m_bullet[i] == 0)
                {
                    continue;
                }

                const Vec2 prev(m_prevX[i], m_prevY[i]);
                const Vec2 curr(m_posX[i], m_posY[i]);
                const Vec2 delta = curr - prev;
                // No net travel this Step -> nothing to clamp (ShapeCast also
                // guards a near-zero delta, but skip the candidate gather too).
                if (delta.x == Real(0) && delta.y == Real(0))
                {
                    continue;
                }

                // INVARIANT: bullets are always movers (Kinematic or Dynamic);
                // a Static body is never flagged isBullet so this branch is
                // unreachable for statics.
                assert(static_cast<BodyType>(m_btype[i]) != BodyType::Static &&
                       "bullets are never static");

                // STATICS ONLY: default ShapeCastOpts (movers = false) casts vs
                // tile spans + non-sensor static bodies. The bullet body is a
                // mover, so it is never a self-obstacle here (no exclude needed).
                const std::optional<ShapeCastHit> hit =
                    ShapeCast(m_shape[i], prev, delta, ShapeCastOpts{});
                if (!hit || hit->t >= Real(1))
                {
                    continue; // clear sweep -> the integrated position stands
                }

                // Clamp to TOI (a hair short, the Lua's hit.t - 0.001).
                const Real clamp = std::max(Real(0), hit->t - kBulletEpsilon);
                const Vec2 clamped = prev + delta * clamp;
                m_posX[i] = clamped.x;
                m_posY[i] = clamped.y;
                // Unconditional: the assert above guarantees this is always a
                // mover (never Static), so the broadphase update always applies.
                m_moverBroadphase->Update(i, SlotAabb(i));
            }
        }

        // ---- pull API for debug draw / inspection (P3.6) -------------------

        void PhysicsWorld::ForEachContact(
            const std::function<void(std::uint32_t, std::uint32_t)>& fn) const
        {
            m_contacts.ForEachBegunPair(fn);
        }

        std::uint32_t PhysicsWorld::IslandRootOf(std::uint32_t i) const noexcept
        {
            // m_uf[i] is the union-find parent for slot i from the last Step's
            // island pass (Island::UpdateSleep writes it via UnionFindScratch()).
            // If the island pass has not run or i is beyond the filled range,
            // return i itself (every un-unioned body is its own island root).
            //
            // IMPORTANT: Island::UpdateSleep uses PATH-HALVING when unioning, so
            // m_uf[i] is a halved parent, NOT necessarily the root. Two bodies in
            // the same island at depth > 1 would yield DIFFERENT m_uf[i] values
            // if we return the one-hop parent directly. Walk to the root
            // non-destructively (world is const here -- do NOT mutate m_uf).
            // Termination: the root satisfies m_uf[root] == root (self-pointing);
            // path-halving never reparents a root, so the walk always terminates.
            if (i >= m_uf.size())
                return i;
            std::uint32_t x = i;
            while (m_uf[x] != x)
                x = m_uf[x];
            return x;
        }

        int PhysicsWorld::QueryAABB(const Aabb2& box,
                                    std::vector<BodyHandle>& out) const
        {
            // Linear scan over alive slots, index-ordered (deterministic).
            // Ports queryAABB (the richer query suite is P1.9).
            out.clear();
            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0)
                {
                    continue;
                }
                if (AabbOverlap(box, SlotAabb(i)))
                {
                    out.push_back(BodyHandle{ i, m_gen[i] });
                }
            }
            return static_cast<int>(out.size());
        }

        // NOTE: the spatial-query surface (Raycast / RayVsBody / LineOfSight /
        // ShapeCast / OverlapShape / StaticCandidates) lives in Queries.cpp
        // (same class, separate TU -- split out in P2.1 to keep this
        // orchestration TU readable once the dynamics stages landed). No API
        // change; PhysicsWorld.hpp still declares them.
        Body PhysicsWorld::GetBody(BodyHandle h) noexcept
        {
            return Body(this, h);
        }

    } // namespace Physics
} // namespace Arcane
