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

#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>
#include <Arcane/Physics/Broadphase/SpatialHash.hpp>
#include <Arcane/Physics/Broadphase/SweepAndPrune.hpp>
#include <Arcane/Physics/Narrowphase/GeometryKernel.hpp> // AabbOverlap (QueryAABB)

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
        } // namespace

        PhysicsWorld::PhysicsWorld(const WorldDef& def)
            : m_moverBroadphase(MakeBroadphase(def))
            , m_gravityX(def.gravityX)
            , m_gravityY(def.gravityY)
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
            m_shape[idx] = Shape{}; // release polygon storage
            m_free.push_back(idx);
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
            // PORT of PhysicsWorld.lua step() ORDER:
            //   stage 1: prev snapshot + velocity integrate (dynamic gravity +
            //            damping; kinematic move) + mover-broadphase update
            //   stage 2: dynamics contact generation         (P2.2)
            //   stage 3: SOLVE velocities (contacts + joints) (P2.2 / P2.3)
            //   stage 4: dynamic POSITION integrate + broadphase update
            //   stage 5: island sleep bookkeeping            (P2.4)
            //   stage 6: contacts:step (events + gating + deferred flush)
            //
            // P2.1 wires stages 1 + 4 + 6 only (no solver yet -> a dynamic body
            // free-falls under gravity per SEMI-IMPLICIT EULER: velocity is
            // integrated first, then position uses the NEW velocity). Stages 2,
            // 3 (solver) and 5 (islands/sleep) land in P2.2-P2.4 at the seams
            // marked below. Index-ordered, no wall-clock, no fast-math.

            // ---- stage 1: prev snapshot + velocity integrate + kinematic move
            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0)
                {
                    continue;
                }
                m_prevX[i] = m_posX[i];
                m_prevY[i] = m_posY[i];

                const BodyType bt = static_cast<BodyType>(m_btype[i]);
                if (bt == BodyType::Dynamic)
                {
                    if (m_awake[i] != 0)
                    {
                        // Gravity then linear damping (ports lines 302-310). The
                        // NEW velocity drives this Step's position integrate ->
                        // semi-implicit (symplectic) Euler.
                        m_velX[i] += m_gravityX * dt;
                        m_velY[i] += m_gravityY * dt;
                        const Real d = m_linDamp[i];
                        if (d > Real(0))
                        {
                            const Real f = Real(1) / (Real(1) + d * dt);
                            m_velX[i]  *= f;
                            m_velY[i]  *= f;
                            m_angVel[i] *= f;
                        }
                    }
                }
                else if (bt == BodyType::Kinematic)
                {
                    m_posX[i] += m_velX[i] * dt;
                    m_posY[i] += m_velY[i] * dt;
                    m_moverBroadphase->Update(i, SlotAabb(i));
                }
            }

            // ---- stage 2-3: dynamics contact generation + velocity solve.
            // [P2.2 solver] manifold generation (dynamic-vs-static + mover-mover)
            // and ISolver::PrepareContacts/SolveVelocity/Relax/ApplyRestitution
            // (Soft Step) land here, between velocity-integrate and
            // position-integrate. [P2.3 baumgarte] position correction too.
            // Nothing runs in P2.1 -> dynamic bodies move purely ballistically.

            // ---- stage 4: dynamic POSITION integrate + broadphase update.
            // Uses the velocity produced by stage 1 (and, once it exists, solved
            // by stages 2-3) -> ports lines 393-401. Awake dynamics only.
            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0 ||
                    static_cast<BodyType>(m_btype[i]) != BodyType::Dynamic ||
                    m_awake[i] == 0)
                {
                    continue;
                }
                m_posX[i]  += m_velX[i] * dt;
                m_posY[i]  += m_velY[i] * dt;
                m_angle[i] += m_angVel[i] * dt;
                m_moverBroadphase->Update(i, SlotAabb(i));
            }

            // ---- stage 5: island sleep bookkeeping.
            // [P2.4 islands/sleep] union-find over awake dynamics via this step's
            // contacts + joints; an island sleeps only when every member is past
            // the sleep threshold (ports lines 403-452). Not in P2.1 -> dynamic
            // bodies never sleep yet (m_awake stays 1, m_sleepTimer unused).

            // ---- stage 6: events + gating + deferred flush (ports the trailing
            // contacts:step). prev/curr are published above; DrawPosition lerps.
            m_contacts.Step(*this);
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
