#include <Panels/AssetReferenceIndex.hpp>

#include <algorithm>

namespace Arcane::Editor
{
    void AssetReferenceIndex::Clear()
    {
        m_nodes.clear();
    }

    void AssetReferenceIndex::RemoveOutboundEdges(const Arcane::Guid& source)
    {
        auto it = m_nodes.find(source);
        if (it == m_nodes.end())
            return;

        const std::vector<Arcane::AssetRef> outboundSnapshot = it->second.outbound;
        for (const Arcane::AssetRef& ref : outboundSnapshot)
        {
            auto targetIt = m_nodes.find(ref.target);
            if (targetIt == m_nodes.end())
                continue;

            Node& target = targetIt->second;
            auto pos = std::lower_bound(target.inbound.begin(), target.inbound.end(), source);
            if (pos != target.inbound.end() && *pos == source)
                target.inbound.erase(pos);

            if (!target.exists && target.inbound.empty())
                m_nodes.erase(targetIt);
        }
    }

    void AssetReferenceIndex::Update(const Arcane::Guid& id, bool exists,
                                      const std::optional<std::vector<Arcane::AssetRef>>& refs)
    {
        // Step 1: fetch/create id's node; set node.exists. A reference into
        // m_nodes is never held across a call that can mutate the map
        // (RemoveOutboundEdges can erase entries, including -- for a
        // self-referencing asset -- `id`'s own node) -- every step below
        // re-looks-up its node fresh instead, per the header's own warning.
        {
            Node& node = m_nodes[id];
            node.exists = exists;
        }

        // Step 2: asset gone. Retract everything id used to point at, drop
        // its own outbound, and erase it outright once nothing references
        // it any more. refs is never consulted past this point.
        if (!exists)
        {
            RemoveOutboundEdges(id);
            auto it = m_nodes.find(id);
            if (it == m_nodes.end())
                return;   // a self-loop's own GC already erased it (see above)
            it->second.outbound.clear();
            if (it->second.inbound.empty())
                m_nodes.erase(it);
            return;
        }

        // Step 3: unreadable/unparsed this walk -- keep last-known-good.
        if (!refs)
            return;

        // Step 4: retract id's OLD outbound set (the discipline this class
        // exists to enforce -- see the header's "REMOVE BEFORE RE-ADD"
        // paragraph) before step 5 installs the fresh one.
        RemoveOutboundEdges(id);

        // Step 5: install the fresh outbound set and add id to every
        // target's inbound, sorted-unique. Re-fetch id's node (never the
        // stale reference from step 1/before RemoveOutboundEdges) --
        // RemoveOutboundEdges can only ever erase a !exists node, and id's
        // own exists is true at this point, so m_nodes[id] here always
        // finds the same node it created/kept, never a fresh empty one.
        Node& self = m_nodes[id];
        self.outbound = *refs;
        for (const Arcane::AssetRef& ref : self.outbound)
        {
            Node& target = m_nodes[ref.target];   // fetch/create; new nodes start exists=false
            auto pos = std::lower_bound(target.inbound.begin(), target.inbound.end(), id);
            if (pos == target.inbound.end() || *pos != id)
                target.inbound.insert(pos, id);
        }
    }

    int AssetReferenceIndex::InboundCount(const Arcane::Guid& id) const
    {
        auto it = m_nodes.find(id);
        return it == m_nodes.end() ? 0 : static_cast<int>(it->second.inbound.size());
    }

    const AssetReferenceIndex::Node* AssetReferenceIndex::Find(const Arcane::Guid& id) const
    {
        auto it = m_nodes.find(id);
        return it == m_nodes.end() ? nullptr : &it->second;
    }

    std::vector<Arcane::Guid> AssetReferenceIndex::DanglingTargets() const
    {
        std::vector<Arcane::Guid> out;
        for (const auto& [guid, node] : m_nodes)
            if (!node.exists && !node.inbound.empty())
                out.push_back(guid);
        return out;
    }

    std::size_t AssetReferenceIndex::NodeCount() const
    {
        return m_nodes.size();
    }
}
