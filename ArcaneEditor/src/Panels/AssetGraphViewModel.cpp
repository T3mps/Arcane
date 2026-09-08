#include <Panels/AssetGraphViewModel.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    namespace
    {
        using Entries = std::unordered_map<Arcane::Guid, AssetPanelEntry>;

        // First 8 hex chars of the canonical 8-4-4-4-12 form -- exactly the
        // guid's first dash-delimited group, the git-short-hash convention.
        // The only label a tombstone (no AssetPanelEntry, so no name) can
        // ever carry (ruling 11).
        std::string ShortGuid(const Arcane::Guid& g)
        {
            return g.ToString().substr(0, 8);
        }

        // Deterministic total order for a guid within one column/one node's
        // capped edge list: the entry's own name when it has one (matching
        // every other name-sorted surface in this arc -- AssetPanelModel::
        // UnusedGuids's own precedent), the short-guid form otherwise (a
        // tombstone has no name). Never m_entries/unordered_map iteration
        // order.
        std::string SortKey(const Arcane::Guid& g, const Entries& entries)
        {
            auto it = entries.find(g);
            return it != entries.end() ? it->second.name : ShortGuid(g);
        }

        // The AssetRefKind of the edge `from` -> `to`, read off `from`'s own
        // outbound list. AssetReferenceIndex::Node::inbound is guid-only (no
        // kind), so this is the only way to recover a referencer's edge kind
        // for the inbound direction's cap-sort/label decisions. References
        // is the safe fallback for data the index cannot explain (should
        // never happen for consistent entries+index snapshots).
        Arcane::AssetRefKind EdgeKindBetween(const Arcane::Guid& from, const Arcane::Guid& to,
                                              const AssetReferenceIndex& index)
        {
            if (const AssetReferenceIndex::Node* node = index.Find(from))
                for (const Arcane::AssetRef& ref : node->outbound)
                    if (ref.target == to)
                        return ref.kind;
            return Arcane::AssetRefKind::References;
        }

        // Edge display label (ruling 9, spec s10): DerivesFrom -> "derives";
        // References -> "uses", EXCEPT a material-kind SOURCE's References
        // edge -> "samples" (a display label derived from the referencer's
        // own kind, never a third AssetRefKind).
        const char* LabelFor(const Arcane::Guid& from, Arcane::AssetRefKind kind, const Entries& entries)
        {
            if (kind == Arcane::AssetRefKind::DerivesFrom)
                return "derives";
            auto it = entries.find(from);
            if (it != entries.end() && it->second.kind == AssetKind::Material)
                return "samples";
            return "uses";
        }

        // One candidate neighbor edge before the breadth cut: the other
        // endpoint's guid and the edge's kind (needed for the DerivesFrom-
        // first sort).
        struct CandidateEdge
        {
            Arcane::Guid guid;
            Arcane::AssetRefKind kind = Arcane::AssetRefKind::References;
        };

        // Sorts most-important-first (DerivesFrom before References, ruling
        // 6/spec s10), then by SortKey for a fully deterministic order
        // within a kind, then by the raw guid string as a final tie-break
        // (two entries can share a name; SortKey alone is not a total
        // order).
        void SortByImportance(std::vector<CandidateEdge>& edges, const Entries& entries)
        {
            std::sort(edges.begin(), edges.end(),
                      [&](const CandidateEdge& a, const CandidateEdge& b)
                      {
                          if (a.kind != b.kind)
                              return a.kind == Arcane::AssetRefKind::DerivesFrom;
                          const std::string ka = SortKey(a.guid, entries);
                          const std::string kb = SortKey(b.guid, entries);
                          if (ka != kb)
                              return ka < kb;
                          return a.guid.ToString() < b.guid.ToString();
                      });
        }

        // Per-guid bookkeeping computed exactly once (idempotent via
        // `processed`): the breadth-capped survivor list for each direction
        // plus how many candidates that direction's cap dropped. Populated
        // lazily as the scope BFS (or the everything-mode all-entries pass)
        // visits each guid.
        struct NodeWork
        {
            bool processed = false;
            std::vector<CandidateEdge> outboundSurvivors;   // -> real edges FROM this guid
            std::vector<CandidateEdge> inboundSurvivors;    // -> real edges INTO this guid
            int outboundOverflow = 0;
            int inboundOverflow = 0;
        };

        // Computes (once) `g`'s capped outbound and inbound candidate lists
        // from the raw index data -- independent of scope/BFS order, so it
        // is safe to call the moment `g` is discovered, whichever direction
        // discovered it. A guid the index has never heard of (no Node at
        // all) simply has no edges either direction.
        void ProcessNode(const Arcane::Guid& g, const Entries& entries, const AssetReferenceIndex& index,
                          int breadthCap, std::unordered_map<Arcane::Guid, NodeWork>& work)
        {
            NodeWork& w = work[g];
            if (w.processed)
                return;
            w.processed = true;

            const AssetReferenceIndex::Node* node = index.Find(g);
            if (!node)
                return;

            std::vector<CandidateEdge> outbound;
            outbound.reserve(node->outbound.size());
            for (const Arcane::AssetRef& ref : node->outbound)
                outbound.push_back({ ref.target, ref.kind });
            SortByImportance(outbound, entries);
            if (static_cast<int>(outbound.size()) > breadthCap)
            {
                w.outboundOverflow = static_cast<int>(outbound.size()) - breadthCap;
                outbound.resize(static_cast<std::size_t>(breadthCap));
            }
            w.outboundSurvivors = std::move(outbound);

            std::vector<CandidateEdge> inbound;
            inbound.reserve(node->inbound.size());
            for (const Arcane::Guid& referencer : node->inbound)
                inbound.push_back({ referencer, EdgeKindBetween(referencer, g, index) });
            SortByImportance(inbound, entries);
            if (static_cast<int>(inbound.size()) > breadthCap)
            {
                w.inboundOverflow = static_cast<int>(inbound.size()) - breadthCap;
                inbound.resize(static_cast<std::size_t>(breadthCap));
            }
            w.inboundSurvivors = std::move(inbound);
        }

        // Does `target`'s OWN (already-capped) inbound view still include
        // `source`? Every entry can be independently a root in everything-
        // mode, so a low-degree source (its own outbound trivially under
        // cap) must not be able to force an edge the TARGET's own inbound
        // cap already dropped -- ruling 6's "per node per direction" is a
        // veto each endpoint holds over edges pointing at IT, not a
        // majority vote. `target` not (yet) processed -- a focus-mode
        // depth-boundary node whose own cap was never computed -- has no
        // veto to cast, so `source`'s proposal stands unchallenged.
        bool TargetAcceptsInbound(const Arcane::Guid& target, const Arcane::Guid& source,
                                   const std::unordered_map<Arcane::Guid, NodeWork>& work)
        {
            auto it = work.find(target);
            if (it == work.end() || !it->second.processed)
                return true;
            for (const CandidateEdge& c : it->second.inboundSurvivors)
                if (c.guid == source)
                    return true;
            return false;
        }

        // The mirror check for an edge discovered from the TARGET's inbound
        // side: does `source`'s own outbound view still include `target`?
        bool SourceAcceptsOutbound(const Arcane::Guid& source, const Arcane::Guid& target,
                                    const std::unordered_map<Arcane::Guid, NodeWork>& work)
        {
            auto it = work.find(source);
            if (it == work.end() || !it->second.processed)
                return true;
            for (const CandidateEdge& c : it->second.outboundSurvivors)
                if (c.guid == target)
                    return true;
            return false;
        }
    }

    void AssetGraphViewModel::Clear()
    {
        nodes.clear();
        edges.clear();
        realNodeCount = 0;
    }

    void AssetGraphViewModel::Build(const GraphBuildInput& in)
    {
        Clear();
        if (!in.entries || !in.index)
            return;

        const Entries& entries = *in.entries;
        const AssetReferenceIndex& index = *in.index;
        const int breadthCap = in.breadthCap;

        std::unordered_map<Arcane::Guid, NodeWork> work;
        std::unordered_set<Arcane::Guid> scope;

        // ---- Step 1: scope -- which guids become nodes at all. ----
        //
        // Everything-mode (nil focus, ruling 6: "there is no root to
        // measure from"): every entry is unconditionally in scope; a
        // breadth-capped expansion pass still runs (over every entry, then
        // over whatever it newly discovers) so tombstones are found and
        // every node's own overflow accounting is computed exactly like
        // focus-mode's -- the cap just can never evict an entry here, since
        // entries are roots by construction, not BFS discoveries.
        //
        // Focus-mode (non-nil): BFS from `focus`, both directions expanded
        // from the SAME frontier each hop, each hop's fan-out already
        // breadth-capped (ProcessNode) BEFORE anything is admitted to the
        // next frontier -- an over-cap neighbor is a discovery that never
        // happens, matching the UE Reference Viewer "+N more" precedent
        // (spec s10) rather than a node the cap merely disconnects.
        if (in.focus.IsNil())
        {
            std::vector<Arcane::Guid> frontier;
            frontier.reserve(entries.size());
            for (const auto& [g, e] : entries)
            {
                (void)e;
                scope.insert(g);
                frontier.push_back(g);
            }
            std::sort(frontier.begin(), frontier.end());

            while (!frontier.empty())
            {
                std::vector<Arcane::Guid> next;
                for (const Arcane::Guid& g : frontier)
                {
                    ProcessNode(g, entries, index, breadthCap, work);
                    const NodeWork& w = work[g];
                    for (const CandidateEdge& c : w.outboundSurvivors)
                        if (scope.insert(c.guid).second)
                            next.push_back(c.guid);
                    for (const CandidateEdge& c : w.inboundSurvivors)
                        if (scope.insert(c.guid).second)
                            next.push_back(c.guid);
                }
                std::sort(next.begin(), next.end());
                frontier = std::move(next);
            }
        }
        else
        {
            scope.insert(in.focus);
            std::vector<Arcane::Guid> frontier{ in.focus };

            for (int depth = 0; depth < in.depthLimit && !frontier.empty(); ++depth)
            {
                std::vector<Arcane::Guid> next;
                for (const Arcane::Guid& g : frontier)
                {
                    ProcessNode(g, entries, index, breadthCap, work);
                    const NodeWork& w = work[g];
                    for (const CandidateEdge& c : w.outboundSurvivors)
                        if (scope.insert(c.guid).second)
                            next.push_back(c.guid);
                    for (const CandidateEdge& c : w.inboundSurvivors)
                        if (scope.insert(c.guid).second)
                            next.push_back(c.guid);
                }
                std::sort(next.begin(), next.end());
                frontier = std::move(next);
            }
        }

        // ---- Step 2: edges -- deduped (from,to) pairs, both directions'
        // survivor lists can name the same logical edge twice. Dropped if
        // either endpoint fell outside scope (depth-cut, never overflow-
        // accounted -- only a BREADTH cut gets a synthetic node). ----
        std::set<std::pair<Arcane::Guid, Arcane::Guid>> seen;
        for (const auto& [g, w] : work)
        {
            if (!w.processed || !scope.count(g))
                continue;
            for (const CandidateEdge& c : w.outboundSurvivors)
            {
                if (!scope.count(c.guid))
                    continue;
                if (!TargetAcceptsInbound(c.guid, g, work))
                    continue;   // the target's OWN inbound cap dropped this one
                if (!seen.insert({ g, c.guid }).second)
                    continue;
                GraphEdge e;
                e.from = g;
                e.to = c.guid;
                e.kind = c.kind;
                e.label = LabelFor(g, c.kind, entries);
                edges.push_back(e);
            }
            for (const CandidateEdge& c : w.inboundSurvivors)
            {
                if (!scope.count(c.guid))
                    continue;
                if (!SourceAcceptsOutbound(c.guid, g, work))
                    continue;   // the source's OWN outbound cap dropped this one
                if (!seen.insert({ c.guid, g }).second)
                    continue;
                GraphEdge e;
                e.from = c.guid;
                e.to = g;
                e.kind = c.kind;
                e.label = LabelFor(c.guid, c.kind, entries);
                edges.push_back(e);
            }
        }
        std::sort(edges.begin(), edges.end(),
                  [](const GraphEdge& a, const GraphEdge& b)
                  {
                      if (a.from != b.from)
                          return a.from.ToString() < b.from.ToString();
                      return a.to.ToString() < b.to.ToString();
                  });

        // ---- Step 3: layer -- longest outbound-path length to a leaf,
        // memoized DFS over the surviving edges only, on-stack cycle guard.
        // Kicked off in guid-sorted order so a genuine cycle's traversal-
        // order-sensitive result (the guard skips a cyclic edge outright,
        // per the header's own doc comment) is still build-reproducible. ----
        std::unordered_map<Arcane::Guid, std::vector<Arcane::Guid>> outAdj;
        for (const GraphEdge& e : edges)
            outAdj[e.from].push_back(e.to);

        std::unordered_map<Arcane::Guid, int> layerOf;
        std::unordered_set<Arcane::Guid> onStack;

        std::function<int(const Arcane::Guid&)> ComputeLayer = [&](const Arcane::Guid& g) -> int
        {
            if (auto it = layerOf.find(g); it != layerOf.end())
                return it->second;

            onStack.insert(g);
            int best = 0;
            if (auto it = outAdj.find(g); it != outAdj.end())
                for (const Arcane::Guid& target : it->second)
                {
                    if (onStack.count(target))
                        continue;   // cycle guard: skip, no contribution (not even 0)
                    best = std::max(best, 1 + ComputeLayer(target));
                }
            onStack.erase(g);
            layerOf[g] = best;
            return best;
        };

        std::vector<Arcane::Guid> scopeSorted(scope.begin(), scope.end());
        std::sort(scopeSorted.begin(), scopeSorted.end());
        for (const Arcane::Guid& g : scopeSorted)
            ComputeLayer(g);

        // ---- Step 4: rows -- name-sorted stacking within each column. ----
        std::map<int, std::vector<Arcane::Guid>> byLayer;   // ordered by layer for a deterministic emission order
        for (const Arcane::Guid& g : scopeSorted)
            byLayer[layerOf[g]].push_back(g);
        for (auto& [layer, guids] : byLayer)
        {
            (void)layer;
            std::sort(guids.begin(), guids.end(),
                     [&](const Arcane::Guid& a, const Arcane::Guid& b)
                     {
                         const std::string ka = SortKey(a, entries);
                         const std::string kb = SortKey(b, entries);
                         return ka != kb ? ka < kb : a.ToString() < b.ToString();
                     });
        }

        std::unordered_map<int, int> nextRowInLayer;   // seeded below, then also used as the overflow placement cursor
        for (const auto& [layer, guids] : byLayer)
        {
            for (int row = 0; row < static_cast<int>(guids.size()); ++row)
            {
                const Arcane::Guid& g = guids[static_cast<std::size_t>(row)];
                GraphNode n;
                n.guid = g;
                n.layer = layer;
                n.row = row;
                if (auto it = entries.find(g); it != entries.end())
                {
                    n.label = it->second.name;
                    n.kind = it->second.kind;
                    n.isTombstone = false;
                }
                else
                {
                    // A scope member with no entry can only be a tombstone
                    // (AssetReferenceIndex::Update never creates a node for
                    // a guid nothing pointed at -- see its own header
                    // comment) -- exists == false, inbound nonempty.
                    n.label = ShortGuid(g);
                    n.kind = AssetKind::Other;
                    n.isTombstone = true;
                }
                nodes.push_back(n);
                if (!n.isTombstone)   // realNodeCount is "real ASSET nodes" (ruling 12) -- a
                    ++realNodeCount;  // tombstone is not an asset; that is the whole point of one.
            }
            nextRowInLayer[layer] = static_cast<int>(guids.size());
        }

        // ---- Step 5: overflow nodes -- one per (anchor, direction) with a
        // nonzero cap drop, guid = the anchor's own guid (ruling: "a nil
        // guid is NOT used for them"), row stacked beneath every real row
        // already placed in its column (ties broken by scope-sorted anchor
        // order, so repeat builds place them identically). ----
        for (const Arcane::Guid& g : scopeSorted)
        {
            auto it = work.find(g);
            if (it == work.end() || !it->second.processed)
                continue;
            const NodeWork& w = it->second;
            const int anchorLayer = layerOf[g];
            AssetKind anchorKind = AssetKind::Other;
            if (auto e = entries.find(g); e != entries.end())
                anchorKind = e->second.kind;

            if (w.outboundOverflow > 0)
            {
                GraphNode n;
                n.guid = g;
                n.label = "+" + std::to_string(w.outboundOverflow) + " more";
                n.kind = anchorKind;
                n.layer = std::max(0, anchorLayer - 1);
                n.row = nextRowInLayer[n.layer]++;
                n.isOverflow = true;
                n.overflowInbound = false;
                n.overflowCount = w.outboundOverflow;
                nodes.push_back(n);
            }
            if (w.inboundOverflow > 0)
            {
                GraphNode n;
                n.guid = g;
                n.label = "+" + std::to_string(w.inboundOverflow) + " more";
                n.kind = anchorKind;
                n.layer = anchorLayer + 1;
                n.row = nextRowInLayer[n.layer]++;
                n.isOverflow = true;
                n.overflowInbound = true;
                n.overflowCount = w.inboundOverflow;
                nodes.push_back(n);
            }
        }
    }
}
