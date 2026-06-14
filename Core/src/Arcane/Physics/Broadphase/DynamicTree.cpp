// DynamicTree.cpp -- pooled dynamic AABB tree broadphase.
//
// See DynamicTree.hpp for the contract. This is a faithful port of
// Client/src/physics/AABBTree.lua with the GC-table nodes replaced by an
// index-based pool (zero steady-state heap) and the uniform deterministic
// output contract (sorted, true-overlap pairs / sorted query ids).
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/Broadphase/DynamicTree.hpp>

#include <algorithm>

namespace Arcane
{
    namespace Physics
    {
        namespace
        {
            // perim(x0,y0,x1,y1) = 2 * ((x1-x0) + (y1-y0)) -- the Lua's 2D SAH
            // analogue (box "cost" used to pick the cheaper child to grow).
            Real Perim(const Aabb2& b) noexcept
            {
                return Real(2) * ((b.max.x - b.min.x) + (b.max.y - b.min.y));
            }

            // Union of two boxes (the Lua nodeUnion / refit min-max).
            Aabb2 Union(const Aabb2& a, const Aabb2& b) noexcept
            {
                Aabb2 u;
                u.min.x = std::min(a.min.x, b.min.x);
                u.min.y = std::min(a.min.y, b.min.y);
                u.max.x = std::max(a.max.x, b.max.x);
                u.max.y = std::max(a.max.y, b.max.y);
                return u;
            }

            // Fat-box overlap test (descent predicate): the Lua query's
            // "n.x0 <= x1 and x0 <= n.x1 and n.y0 <= y1 and y0 <= n.y1".
            bool FatOverlap(const Aabb2& n, const Aabb2& q) noexcept
            {
                return n.min.x <= q.max.x && q.min.x <= n.max.x &&
                       n.min.y <= q.max.y && q.min.y <= n.max.y;
            }
        } // namespace

        // --------------------------------------------------------------------
        // Node pool (index-based free-list).
        // --------------------------------------------------------------------
        std::uint32_t DynamicTree::AllocNode()
        {
            if (m_freeList != kNull)
            {
                const std::uint32_t idx = m_freeList;
                m_freeList              = m_nodes[idx].parent; // next in chain
                m_nodes[idx]           = Node{};               // reset
                return idx;
            }
            const std::uint32_t idx =
                static_cast<std::uint32_t>(m_nodes.size());
            m_nodes.emplace_back();
            return idx;
        }

        void DynamicTree::FreeNode(std::uint32_t idx)
        {
            m_nodes[idx]        = Node{};
            m_nodes[idx].parent = m_freeList; // push onto free-list
            m_freeList          = idx;
        }

        // --------------------------------------------------------------------
        // id -> leaf slot map (the Lua `leaves` table). A simple dense vector
        // keyed by id; entry kNull means absent. Avoids unordered_map so the
        // Pairs() iteration order is deterministic (we iterate ids ascending).
        // --------------------------------------------------------------------
        std::uint32_t DynamicTree::LeafOf(std::uint32_t id) const noexcept
        {
            return id < m_leafOfId.size() ? m_leafOfId[id] : kNull;
        }

        void DynamicTree::SetLeafOf(std::uint32_t id, std::uint32_t leaf)
        {
            if (id >= m_leafOfId.size())
            {
                m_leafOfId.resize(static_cast<std::size_t>(id) + 1u, kNull);
            }
            m_leafOfId[id] = leaf;
        }

        // --------------------------------------------------------------------
        // refit: walk parents up, recomputing the union box (Lua refit).
        // --------------------------------------------------------------------
        void DynamicTree::Refit(std::uint32_t n)
        {
            while (n != kNull)
            {
                Node& node = m_nodes[n];
                if (!node.IsLeaf())
                {
                    node.fat = Union(m_nodes[node.left].fat,
                                     m_nodes[node.right].fat);
                }
                n = node.parent;
            }
        }

        // --------------------------------------------------------------------
        // _insertLeaf: descend by least combined-perimeter growth, split the
        // chosen leaf with a new internal node (faithful port).
        // --------------------------------------------------------------------
        void DynamicTree::InsertLeaf(std::uint32_t leaf)
        {
            if (m_root == kNull)
            {
                m_root                 = leaf;
                m_nodes[leaf].parent   = kNull;
                return;
            }

            const Aabb2 leafBox = m_nodes[leaf].fat;

            std::uint32_t n = m_root;
            while (!m_nodes[n].IsLeaf())
            {
                const std::uint32_t l = m_nodes[n].left;
                const std::uint32_t r = m_nodes[n].right;

                const Real growL =
                    Perim(Union(m_nodes[l].fat, leafBox)) - Perim(m_nodes[l].fat);
                const Real growR =
                    Perim(Union(m_nodes[r].fat, leafBox)) - Perim(m_nodes[r].fat);

                n = (growL <= growR) ? l : r; // Lua: growL <= growR ? l : r
            }

            // Split leaf n with a new internal node (Lua _insertLeaf tail).
            const std::uint32_t oldParent = m_nodes[n].parent;
            const std::uint32_t internal  = AllocNode();
            m_nodes[internal].parent = oldParent;
            m_nodes[internal].left   = n;
            m_nodes[internal].right  = leaf;

            if (oldParent != kNull)
            {
                if (m_nodes[oldParent].left == n)
                    m_nodes[oldParent].left = internal;
                else
                    m_nodes[oldParent].right = internal;
            }
            else
            {
                m_root = internal;
            }

            m_nodes[n].parent    = internal;
            m_nodes[leaf].parent = internal;
            Refit(internal);
        }

        // --------------------------------------------------------------------
        // _removeLeaf: promote the sibling into the parent's slot (Lua port).
        // The freed internal parent goes back on the pool free-list.
        // --------------------------------------------------------------------
        void DynamicTree::RemoveLeaf(std::uint32_t leaf)
        {
            const std::uint32_t p = m_nodes[leaf].parent;
            if (p == kNull)
            {
                m_root = kNull; // leaf was the root
                return;
            }

            const std::uint32_t sib =
                (m_nodes[p].left == leaf) ? m_nodes[p].right : m_nodes[p].left;
            const std::uint32_t gp = m_nodes[p].parent;

            m_nodes[sib].parent = gp;
            if (gp != kNull)
            {
                if (m_nodes[gp].left == p)
                    m_nodes[gp].left = sib;
                else
                    m_nodes[gp].right = sib;
                Refit(gp);
            }
            else
            {
                m_root = sib;
            }
            FreeNode(p); // recycle the now-orphaned internal node
        }

        // --------------------------------------------------------------------
        // update (upsert): fat-box-contains fast path, else remove + reinsert
        // with a fresh fat box (Lua AABBTree:update).
        // --------------------------------------------------------------------
        void DynamicTree::Update(std::uint32_t id, const Aabb2& box)
        {
            std::uint32_t leaf = LeafOf(id);
            if (leaf != kNull)
            {
                // Still inside the fat box: only refresh the tight box.
                const Aabb2& fat = m_nodes[leaf].fat;
                if (box.min.x >= fat.min.x && box.min.y >= fat.min.y &&
                    box.max.x <= fat.max.x && box.max.y <= fat.max.y)
                {
                    m_nodes[leaf].tight = box;
                    return;
                }
                RemoveLeaf(leaf);
            }
            else
            {
                leaf = AllocNode();
                m_nodes[leaf].left  = kNull; // explicit leaf
                m_nodes[leaf].right = kNull;
                m_nodes[leaf].id    = id;
                SetLeafOf(id, leaf);
            }

            m_nodes[leaf].tight   = box;
            m_nodes[leaf].fat.min = Vec2(box.min.x - kMargin, box.min.y - kMargin);
            m_nodes[leaf].fat.max = Vec2(box.max.x + kMargin, box.max.y + kMargin);
            InsertLeaf(leaf);
        }

        void DynamicTree::Remove(std::uint32_t id)
        {
            const std::uint32_t leaf = LeafOf(id);
            if (leaf == kNull)
            {
                return; // Lua guard: not present
            }
            RemoveLeaf(leaf);
            FreeNode(leaf);
            SetLeafOf(id, kNull);
        }

        // --------------------------------------------------------------------
        // queryAABB: stack-based descent on the FAT boxes, narrow to the TIGHT
        // box (Lua query + queryAABB). Output is collected then SORTED ascending
        // for the deterministic contract.
        // --------------------------------------------------------------------
        int DynamicTree::QueryAABB(const Aabb2& box,
                                   std::vector<std::uint32_t>& out) const
        {
            out.clear();
            if (m_root == kNull)
            {
                return 0;
            }

            m_stack.clear();
            m_stack.push_back(m_root);
            while (!m_stack.empty())
            {
                const std::uint32_t ni = m_stack.back();
                m_stack.pop_back();
                const Node& n = m_nodes[ni];
                if (!FatOverlap(n.fat, box))
                {
                    continue;
                }
                if (!n.IsLeaf())
                {
                    m_stack.push_back(n.left);
                    m_stack.push_back(n.right);
                }
                else if (AabbOverlap(n.tight, box)) // narrow against tight box
                {
                    out.push_back(n.id);
                }
            }

            std::sort(out.begin(), out.end());
            return static_cast<int>(out.size());
        }

        // --------------------------------------------------------------------
        // pairs: for each leaf, descend querying its TIGHT box; emit (id,other)
        // when other.id > id (the Lua's dup/self guard) AND the tight boxes
        // overlap. Iterate leaves by id ASCENDING (deterministic) then sort the
        // final output by (a,b). The result is the canonical true-overlap set.
        // --------------------------------------------------------------------
        int DynamicTree::Pairs(std::vector<BroadphasePair>& out) const
        {
            out.clear();
            if (m_root == kNull)
            {
                return 0;
            }

            // Iterate ids ascending so the (smaller-id) leaf drives each pair;
            // m_leafOfId is keyed by id, giving a deterministic order without
            // any hash-map iteration.
            for (std::uint32_t id = 0;
                 id < static_cast<std::uint32_t>(m_leafOfId.size()); ++id)
            {
                const std::uint32_t leaf = m_leafOfId[id];
                if (leaf == kNull)
                {
                    continue;
                }
                const Aabb2 tight = m_nodes[leaf].tight;

                m_stack.clear();
                m_stack.push_back(m_root);
                while (!m_stack.empty())
                {
                    const std::uint32_t ni = m_stack.back();
                    m_stack.pop_back();
                    const Node& n = m_nodes[ni];
                    if (!FatOverlap(n.fat, tight))
                    {
                        continue;
                    }
                    if (!n.IsLeaf())
                    {
                        m_stack.push_back(n.left);
                        m_stack.push_back(n.right);
                    }
                    else if (n.id > id && AabbOverlap(n.tight, tight))
                    {
                        out.push_back(BroadphasePair{ id, n.id });
                    }
                }
            }

            std::sort(out.begin(), out.end());
            return static_cast<int>(out.size());
        }

    } // namespace Physics
} // namespace Arcane
