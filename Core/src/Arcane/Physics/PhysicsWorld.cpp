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

#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>
#include <Arcane/Physics/Broadphase/SpatialHash.hpp>
#include <Arcane/Physics/Broadphase/SweepAndPrune.hpp>

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
            // setVelocity). The Dynamic wake side-effect is P2.
            if (bt == BodyType::Kinematic || bt == BodyType::Dynamic)
            {
                m_velX[i] = v.x;
                m_velY[i] = v.y;
            }
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
            // Stage 1 (kinematic subset) + stage 7: prev snapshot + KINEMATIC
            // velocity integration + mover-broadphase update, by INDEX order
            // (deterministic; no map iteration, no wall-clock).
            //
            // Static: no integrate. Dynamic: NOT integrated in P1.8 (the
            // gravity/damping/solver/position-integration/sleep/CCD stages are
            // P2/P3 -- see PORT BOUNDARY).
            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0)
                {
                    continue;
                }
                // [P2.1] dynamic velocity integrate (gravity/damping) goes here,
                // before broadphase update
                m_prevX[i] = m_posX[i];
                m_prevY[i] = m_posY[i];
                if (static_cast<BodyType>(m_btype[i]) == BodyType::Kinematic)
                {
                    m_posX[i] += m_velX[i] * dt;
                    m_posY[i] += m_velY[i] * dt;
                    m_moverBroadphase->Update(i, SlotAabb(i));
                }
            }

            // Events + gating + deferred flush (ports the trailing
            // contacts:step). prev/curr are published by the snapshot +
            // integration above; DrawPosition lerps between them.
            m_contacts.Step(*this);
            // [P2.1] dynamic stages go here: solver (contacts+joints) ->
            // dynamic position integrate -> island/sleep
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

        void PhysicsWorld::StaticCandidates(const Aabb2& box,
                                            std::vector<Aabb2>& spansOut,
                                            std::vector<std::uint32_t>& staticsOut) const
        {
            // Merged tile spans (TileGrid) + overlapping static-body slots
            // (staticList, index-ordered). Ports _staticCandidates. With no
            // TileGrid spansOut is empty.
            spansOut.clear();
            if (m_tileGrid)
            {
                m_tileGrid->Query(box, spansOut);
            }
            staticsOut.clear();
            for (std::uint32_t i = 0; i < m_staticList.size(); ++i)
            {
                const std::uint32_t idx = m_staticList[i];
                if (AabbOverlap(box, SlotAabb(idx)))
                {
                    staticsOut.push_back(idx);
                }
            }
        }

        Body PhysicsWorld::GetBody(BodyHandle h) noexcept
        {
            return Body(this, h);
        }

    } // namespace Physics
} // namespace Arcane
