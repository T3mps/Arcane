// ContactManager.cpp -- persistent contact pairs + begin/stay/end + sensor
// events + two-granularity event gating (port of ContactManager.lua).
//
// See ContactManager.hpp for the contract + the (behavior-preserving)
// modernizations. The gating is a VERBATIM port of the Lua: per-body evtOn +
// world gate; gated -> begun=false (DROP, re-arm later); level-triggered
// re-arm emits a fresh begin for currently-overlapping pairs.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/ContactManager.hpp>

#include <algorithm>

#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Narrowphase/Dispatch.hpp>

namespace Arcane
{
    namespace Physics
    {
        bool ContactManager::ShapesOverlap(const PhysicsWorld& w,
                                           std::uint32_t a, std::uint32_t b)
        {
            // Reuse the narrowphase: margin 0 = exact overlap; a contact point
            // is emitted only for real penetration, matching the Lua
            // shapesOverlap for the tested configs (see header for the
            // edge-touch subtlety).
            Transform xfA{ w.PosSlot(a), Real(0) };
            Transform xfB{ w.PosSlot(b), Real(0) };
            const Manifold m = CollideShapes(w.ShapeSlot(a), xfA,
                                             w.ShapeSlot(b), xfB, Real(0));
            return m.pointCount > 0;
        }

        void ContactManager::Emit(PhysicsWorld& w, ContactEvent::Type type,
                                  std::uint32_t a, std::uint32_t b)
        {
            // Deferred: queue the row; do NOT call the listener here (the Lua
            // _emit only fills _queue; the flush is at the end of step). If no
            // listener is set we still skip queuing (matches the Lua guard).
            if (!m_listener)
            {
                return;
            }
            ContactEvent ev;
            ev.type   = type;
            ev.a      = w.HandleOf(a);
            ev.b      = w.HandleOf(b);
            ev.sensor = w.SensorSlot(a) || w.SensorSlot(b);
            m_queue.push_back(ev);
        }

        void ContactManager::Touch(PhysicsWorld& w, std::uint32_t a,
                                   std::uint32_t b, std::uint32_t stamp)
        {
            // Ports touch(a,b): guard alive, narrow to true overlap, then
            // get/create the pair, stamp it, gate, and queue begin/stay.
            if (!w.Alive(a) || !w.Alive(b))
            {
                return;
            }
            if (!ShapesOverlap(w, a, b))
            {
                return;
            }
            const std::uint64_t k = PairKey(a, b);
            auto it = m_pairs.find(k);
            if (it == m_pairs.end())
            {
                Pair p;
                p.a     = std::min(a, b);
                p.b     = std::max(a, b);
                p.begun = false;
                it      = m_pairs.emplace(k, p).first;
            }
            Pair& p = it->second;
            p.stamp = stamp;

            const bool gated = !w.EventsEnabled() || !w.EvtOn(p.a) || !w.EvtOn(p.b);
            if (gated)
            {
                p.begun = false; // dropped; the pair re-arms on enable
            }
            else if (!p.begun)
            {
                p.begun = true;
                Emit(w, ContactEvent::Type::Begin, p.a, p.b);
            }
            else
            {
                Emit(w, ContactEvent::Type::Stay, p.a, p.b);
            }
        }

        void ContactManager::Step(PhysicsWorld& w)
        {
            ++m_stamp;
            const std::uint32_t stamp = m_stamp;
            m_queue.clear();

            // Candidates: mover-mover (the broadphase Pairs() -- ALREADY SORTED
            // by (a,b) per P1.6, so NO re-sort needed here, unlike the Lua which
            // sorted the hash's bucket-order list) + each KINEMATIC body vs each
            // staticList body (index-ordered). Both candidate streams are
            // deterministic.
            w.MoverBroadphase().Pairs(m_pairScratch);
            for (const BroadphasePair& bp : m_pairScratch)
            {
                Touch(w, bp.a, bp.b, stamp);
            }

            const std::vector<std::uint32_t>& statics = w.StaticList();
            const std::uint32_t count = w.Count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (w.Alive(i) && w.TypeSlot(i) == BodyType::Kinematic)
                {
                    for (std::uint32_t s = 0; s < statics.size(); ++s)
                    {
                        Touch(w, i, statics[s], stamp);
                    }
                }
            }

            // Pairs not touched this step have separated. Collect the to-end
            // pairs, SORT by (a,b) for deterministic End order (the map's
            // iteration order is nondeterministic), then emit. Erase ALL
            // untouched pairs (begun or not).
            m_endScratch.clear();
            for (auto it = m_pairs.begin(); it != m_pairs.end();)
            {
                Pair& p = it->second;
                if (p.stamp != stamp)
                {
                    const bool gated = !w.EventsEnabled() ||
                                       !w.EvtOn(p.a) || !w.EvtOn(p.b);
                    if (p.begun && !gated)
                    {
                        m_endScratch.push_back(p);
                    }
                    it = m_pairs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            std::sort(m_endScratch.begin(), m_endScratch.end(),
                      [](const Pair& l, const Pair& r) noexcept
                      {
                          return l.a != r.a ? l.a < r.a : l.b < r.b;
                      });
            for (const Pair& p : m_endScratch)
            {
                Emit(w, ContactEvent::Type::End, p.a, p.b);
            }

            // Flush AFTER all state has settled (deferred delivery).
            if (m_listener)
            {
                for (const ContactEvent& ev : m_queue)
                {
                    m_listener(ev);
                }
            }
        }

        void ContactManager::Disarm()
        {
            for (auto& kv : m_pairs)
            {
                kv.second.begun = false;
            }
        }

        void ContactManager::Disarm(std::uint32_t idx)
        {
            for (auto& kv : m_pairs)
            {
                Pair& p = kv.second;
                if (p.a == idx || p.b == idx)
                {
                    p.begun = false;
                }
            }
        }

        void ContactManager::Rearm(PhysicsWorld& w)
        {
            RearmImpl(w, false, 0);
        }

        void ContactManager::Rearm(PhysicsWorld& w, std::uint32_t idx)
        {
            RearmImpl(w, true, idx);
        }

        void ContactManager::RearmImpl(PhysicsWorld& w, bool filterIdx,
                                       std::uint32_t idx)
        {
            // Level-triggered: for currently-overlapping !begun pairs that are
            // not gated (and, if filterIdx, involve idx), emit a fresh begin
            // IMMEDIATELY (synchronous; re-arm happens outside Step). Ports
            // rearm(w, idx|nil).
            //
            // DETERMINISM (modernize): the Lua iterated the pairs table (order
            // nondeterministic). We collect the to-rearm pairs, mark begun, SORT
            // by (a,b), then emit -- so the synchronous begin order is
            // deterministic too (matching Step's sorted-end discipline).
            m_endScratch.clear();
            for (auto& kv : m_pairs)
            {
                Pair& p = kv.second;
                if (p.begun)
                {
                    continue;
                }
                if (filterIdx && p.a != idx && p.b != idx)
                {
                    continue;
                }
                if (w.Alive(p.a) && w.Alive(p.b) && w.EventsEnabled() &&
                    w.EvtOn(p.a) && w.EvtOn(p.b) && ShapesOverlap(w, p.a, p.b))
                {
                    p.begun = true; // arm now; emission below
                    m_endScratch.push_back(p);
                }
            }
            std::sort(m_endScratch.begin(), m_endScratch.end(),
                      [](const Pair& l, const Pair& r) noexcept
                      {
                          return l.a != r.a ? l.a < r.a : l.b < r.b;
                      });
            if (!m_listener)
            {
                return;
            }
            for (const Pair& p : m_endScratch)
            {
                ContactEvent ev;
                ev.type   = ContactEvent::Type::Begin;
                ev.a      = w.HandleOf(p.a);
                ev.b      = w.HandleOf(p.b);
                ev.sensor = w.SensorSlot(p.a) || w.SensorSlot(p.b);
                m_listener(ev);
            }
        }

        void ContactManager::DropBody(std::uint32_t idx)
        {
            for (auto it = m_pairs.begin(); it != m_pairs.end();)
            {
                const Pair& p = it->second;
                if (p.a == idx || p.b == idx)
                {
                    it = m_pairs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

    } // namespace Physics
} // namespace Arcane
