#include <Arcane/Physics/IslandManager.hpp>

#include <Arcane/Physics/PhysicsWorld.hpp> // the world SoA + awake-set mechanism (befriended)

namespace Arcane
{
    namespace Physics
    {
        // ---- registry lifecycle (Phase A) ----------------------------------

        std::uint32_t IslandManager::AllocIsland()
        {
            if (!m_islandFree.empty())
            {
                const std::uint32_t id = m_islandFree.back();
                m_islandFree.pop_back();
                m_islands[id].bodies.clear();      // capacity kept (zero re-alloc)
                m_islands[id].splitCandidate = false;
                return id;
            }
            const std::uint32_t id = static_cast<std::uint32_t>(m_islands.size());
            m_islands.emplace_back();
            return id;
        }

        void IslandManager::FreeIsland(std::uint32_t id) noexcept
        {
            // Defensive: never free an invalid/out-of-range id.
            if (id == Island::kInvalidIsland || id >= m_islands.size())
            {
                return;
            }
            m_islands[id].bodies.clear();
            m_islands[id].splitCandidate = false;
            m_islandFree.push_back(id);
        }

        std::uint32_t IslandManager::MergeIslands(std::uint32_t idA, std::uint32_t idB)
        {
            // Weighted union: keep the LARGER island; relabel the smaller's
            // members + append, then free the smaller id. Stable membership ->
            // fewer relabels -> the island id of a big pile is sticky.
            if (idA == idB)
            {
                return idA;
            }
            std::uint32_t big   = idA;
            std::uint32_t small = idB;
            if (m_islands[big].bodies.size() < m_islands[small].bodies.size())
            {
                std::swap(big, small);
            }
            for (const std::uint32_t s : m_islands[small].bodies)
            {
                m_islandId[s] = big;
                m_islands[big].bodies.push_back(s);
            }
            // The survivor inherits the UNION of both islands' pending split-candidate
            // state. If the absorbed island was flagged for a deferred split (a member
            // separated/was destroyed before this merge), that fracture need transfers
            // to the survivor -- dropping it could leave a genuinely disconnected pile
            // over-grouped until another edge re-marks it. big keeps its own flag.
            const bool absorbedSplit = m_islands[small].splitCandidate;
            FreeIsland(small);
            m_islands[big].splitCandidate = m_islands[big].splitCandidate || absorbedSplit;
            return big;
        }

        void IslandManager::MarkSplitCandidate(std::uint32_t islandId) noexcept
        {
            if (islandId == Island::kInvalidIsland || islandId >= m_islands.size())
            {
                return;
            }
            m_islands[islandId].splitCandidate = true;
        }

        void IslandManager::SplitIsland(PhysicsWorld& w, std::uint32_t islandId)
        {
            // Re-derive the connected components of one candidate island. Bodies
            // joined by a touching dyn-dyn contact share a component; the FIRST
            // component reuses islandId, others get fresh ids. The contact walk is
            // scoped to the island's OWN contacts via per-body adjacency
            // (w.m_bodyContacts) -> O(islandBodies + islandEdges), replacing the old
            // O(poolSize x islandSize) whole-pool scan. Byte-identical components.
            if (islandId == Island::kInvalidIsland || islandId >= m_islands.size())
            {
                return;
            }
            m_islands[islandId].splitCandidate = false;

            // Snapshot members (the rebuild reassigns m_islandId + may reuse this id).
            std::vector<std::uint32_t> members = m_islands[islandId].bodies;
            const std::uint32_t n = static_cast<std::uint32_t>(members.size());
            if (n <= 1)
            {
                return; // 0 or 1 member: nothing to fracture
            }

            // O(1) member-slot -> local index. Scratch is all-sentinel on entry.
            // members are live island body slots (< m_count) and EnsureCapacity
            // sizes m_splitLocalIndex >= m_count, so this write is always in-bounds.
            for (std::uint32_t i = 0; i < n; ++i)
            {
                m_splitLocalIndex[members[i]] = i;
            }

            std::vector<std::uint32_t> parent(n);
            for (std::uint32_t i = 0; i < n; ++i) { parent[i] = i; }
            auto find = [&](std::uint32_t x) -> std::uint32_t
            {
                while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
                return x;
            };

            // Union members joined by a TOUCHING dyn-dyn contact, walking only the
            // island's own contacts. Each edge is visited from both endpoints; the
            // union is idempotent and connected components are union-order-invariant,
            // so the resulting partition is identical to the old whole-pool walk.
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const std::uint32_t slot = members[i];
                for (const std::uint32_t cid : w.m_bodyContacts[slot])
                {
                    const Contact& c = w.m_contactPool.Get(cid);
                    if (!c.touching) { continue; }
                    const std::uint32_t other = (c.bodyA == slot) ? c.bodyB : c.bodyA;
                    if (other >= m_splitLocalIndex.size()) { continue; }
                    const std::uint32_t j = m_splitLocalIndex[other];
                    if (j == kSplitLocalNone) { continue; } // other body not in island
                    parent[find(i)] = find(j);
                }
            }

            // Union members joined by a JOINT edge too (Box2D treats a joint as an
            // island edge). A jointed dynamic pair stays in ONE component even when
            // it shares no touching contact -- so removing a contact never wrongly
            // splits a still-jointed pair. Iterate m_joints in index order
            // (deterministic); only a dyn-dyn joint whose BOTH endpoints are members
            // of THIS island (m_splitLocalIndex != kSplitLocalNone) unions. Resolve
            // via the stable HANDLE slots (Prepare-independent -- a joint whose
            // bodies are all asleep is not Prepared this Step, so BodyA()/BodyB()
            // may be stale).
            for (const std::unique_ptr<Joint>& jp : w.m_joints)
            {
                const Joint* jt = jp.get();
                if (jt == nullptr) { continue; }
                const BodyHandle ha = jt->HandleA();
                const BodyHandle hb = jt->HandleB();
                if (ha.generation == 0u || hb.generation == 0u) { continue; } // static-anchor / missing-body joint
                const std::uint32_t sa = ha.index;
                const std::uint32_t sb = hb.index;
                if (sa >= m_splitLocalIndex.size() || sb >= m_splitLocalIndex.size()) { continue; }
                if (w.TypeSlot(sa) != BodyType::Dynamic || w.TypeSlot(sb) != BodyType::Dynamic) { continue; }
                const std::uint32_t la = m_splitLocalIndex[sa];
                const std::uint32_t lb = m_splitLocalIndex[sb];
                if (la == kSplitLocalNone || lb == kSplitLocalNone) { continue; } // not both in this island
                parent[find(la)] = find(lb);
            }

            // Group by local root; FIRST component reuses islandId, others alloc a
            // fresh id. UNCHANGED from the original (byte-identical id assignment).
            // SAFETY: AllocIsland() may emplace_back + REALLOCATE m_islands -- never
            // hold an Island& across it; index m_islands[isl] freshly by id.
            m_islands[islandId].bodies.clear();
            std::vector<std::uint32_t> rootLocal;   // distinct local roots, first-seen order
            std::vector<std::uint32_t> rootIsland;  // parallel island id per root
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const std::uint32_t r = find(i);
                std::uint32_t ri = 0xFFFFFFFFu;
                for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(rootLocal.size()); ++k)
                {
                    if (rootLocal[k] == r) { ri = k; break; }
                }
                std::uint32_t isl;
                if (ri == 0xFFFFFFFFu)
                {
                    isl = rootLocal.empty() ? islandId : AllocIsland();
                    rootLocal.push_back(r);
                    rootIsland.push_back(isl);
                }
                else
                {
                    isl = rootIsland[ri];
                }
                const std::uint32_t slot = members[i];
                m_islandId[slot] = isl;
                m_islands[isl].bodies.push_back(slot);
            }

            // Reset only the touched scratch entries -> keep O(island), all-sentinel
            // between calls.
            for (std::uint32_t i = 0; i < n; ++i)
            {
                m_splitLocalIndex[members[i]] = kSplitLocalNone;
            }
        }

        void IslandManager::WakeIsland(PhysicsWorld& w, std::uint32_t slot) noexcept
        {
            const std::uint32_t isl = IslandOf(slot);
            if (isl == Island::kInvalidIsland)
            {
                return; // static/kinematic -> nothing to wake on itself
            }
            for (const std::uint32_t b : m_islands[isl].bodies)
            {
                w.m_awake[b]      = 1;
                w.m_sleepTimer[b] = Real(0);
                w.AddToAwakeSet(b); // Phase B: migrate every island member back into the awake-set
            }
        }

        std::uint32_t IslandManager::IslandRootOf(std::uint32_t i) const noexcept
        {
            // Phase A: the persistent island id IS the root (equal for all
            // co-island members after merge, distinct across islands). A non-member
            // (static/kinematic, or an un-assigned slot) has no island; return
            // a high-bit-tagged slot so it can never collide with a real island
            // id (ids are dense + small, < 2^31). Consumed by PhysicsDebugDraw
            // (color-by-island, Dynamic only) + the island tests.
            if (i >= m_islandId.size() || m_islandId[i] == Island::kInvalidIsland)
            {
                return i | 0x80000000u;
            }
            return m_islandId[i];
        }

        // ---- body-lifecycle seams (PhysicsWorld drives these) ---------------

        void IslandManager::Grow(std::uint32_t next)
        {
            // Phase A: new per-body island id column. A never-touched tail slot has
            // no island yet; it is assigned (or left kInvalidIsland for non-Dynamic)
            // in CreateSingletonIsland/ClearIsland at AddBody. SplitIsland scratch
            // column stays all-sentinel until SplitIsland writes its members.
            m_islandId.resize(next, Island::kInvalidIsland);
            m_splitLocalIndex.resize(next, kSplitLocalNone);
        }

        void IslandManager::CreateSingletonIsland(std::uint32_t slot)
        {
            const std::uint32_t isl = AllocIsland();
            m_islands[isl].bodies.push_back(slot);
            m_islandId[slot] = isl;
        }

        void IslandManager::ClearIsland(std::uint32_t slot) noexcept
        {
            m_islandId[slot] = Island::kInvalidIsland;
        }

        void IslandManager::RemoveBodyFromIsland(std::uint32_t slot) noexcept
        {
            const std::uint32_t isl = IslandOf(slot);
            if (isl != Island::kInvalidIsland)
            {
                auto& bodies = m_islands[isl].bodies;
                for (std::size_t i = 0; i < bodies.size(); ++i)
                {
                    if (bodies[i] == slot)
                    {
                        bodies[i] = bodies.back();
                        bodies.pop_back();
                        break;
                    }
                }
                if (bodies.empty())
                {
                    FreeIsland(isl);
                }
                else
                {
                    MarkSplitCandidate(isl);
                }
                m_islandId[slot] = Island::kInvalidIsland;
            }
        }

        void IslandManager::CollectSplitCandidates(std::vector<std::uint32_t>& out) const
        {
            // Collect candidates ascending-id (determinism), then the caller splits
            // the first quota; the rest carry their flag to the next Step.
            out.clear();
            for (std::uint32_t id = 0; id < static_cast<std::uint32_t>(m_islands.size()); ++id)
            {
                if (m_islands[id].splitCandidate && !m_islands[id].bodies.empty())
                {
                    out.push_back(id);
                }
            }
        }
    } // namespace Physics
} // namespace Arcane
