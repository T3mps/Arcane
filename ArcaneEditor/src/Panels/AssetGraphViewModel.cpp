#include <Panels/AssetGraphViewModel.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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

        std::string LowerCopy(std::string_view s)
        {
            std::string out(s);
            for (char& c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                s.remove_suffix(1);
            return s;
        }

        std::optional<AssetKind> KindFromFocusToken(std::string_view tok)
        {
            const std::string t = LowerCopy(tok);
            if (t == "source" || t == "src" || t == "cpp" || t == "hpp")
                return AssetKind::Source;
            if (t == "mesh")
                return AssetKind::Mesh;
            if (t == "scene")
                return AssetKind::Scene;
            if (t == "texture" || t == "tex")
                return AssetKind::Texture;
            if (t == "material" || t == "mat")
                return AssetKind::Material;
            if (t == "sprite")
                return AssetKind::Sprite;
            if (t == "model" || t == "glb")
                return AssetKind::Model;
            return std::nullopt;
        }

        // Sugiyama barycentric crossing reduction. Layers are already the
        // longest-path columns (sources left). Name-sort alone crosses wires
        // whenever two edges invert relative to alphabet order; a few
        // left-to-right / right-to-left sweeps pull connected nodes toward
        // each other's rows. Nodes with no neighbor in the compared layer
        // keep their current index so they do not all pile at the bottom.
        void ReduceCrossings(std::map<int, std::vector<Arcane::Guid>>& byLayer,
                             const std::vector<GraphEdge>& edges)
        {
            std::unordered_map<Arcane::Guid, std::vector<Arcane::Guid>> fwd, rev;
            for (const GraphEdge& e : edges)
            {
                fwd[e.from].push_back(e.to);
                rev[e.to].push_back(e.from);
            }

            std::vector<int> layers;
            layers.reserve(byLayer.size());
            for (const auto& [layer, _] : byLayer)
                layers.push_back(layer);

            auto orderAgainst = [&](int layer, int otherLayer, bool useFwd)
            {
                auto it = byLayer.find(layer);
                auto jt = byLayer.find(otherLayer);
                if (it == byLayer.end() || jt == byLayer.end())
                    return;
                std::unordered_map<Arcane::Guid, int> pos;
                pos.reserve(jt->second.size());
                for (int i = 0; i < static_cast<int>(jt->second.size()); ++i)
                    pos[jt->second[static_cast<std::size_t>(i)]] = i;

                const auto& nbrs = useFwd ? fwd : rev;
                std::vector<std::pair<float, Arcane::Guid>> scored;
                scored.reserve(it->second.size());
                for (int i = 0; i < static_cast<int>(it->second.size()); ++i)
                {
                    const Arcane::Guid& g = it->second[static_cast<std::size_t>(i)];
                    float sum = 0.0f;
                    int n = 0;
                    if (auto nit = nbrs.find(g); nit != nbrs.end())
                        for (const Arcane::Guid& nb : nit->second)
                            if (auto pit = pos.find(nb); pit != pos.end())
                            {
                                sum += static_cast<float>(pit->second);
                                ++n;
                            }
                    const float key = n > 0 ? sum / static_cast<float>(n)
                                            : static_cast<float>(i);
                    scored.push_back({ key, g });
                }
                std::stable_sort(scored.begin(), scored.end(),
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
                for (std::size_t i = 0; i < scored.size(); ++i)
                    it->second[i] = scored[i].second;
            };

            for (int sweep = 0; sweep < 4; ++sweep)
            {
                // Higher layer (referencers) looks at targets via outbound.
                for (std::size_t i = 0; i + 1 < layers.size(); ++i)
                    orderAgainst(layers[i + 1], layers[i], /*useFwd=*/true);
                // Lower layer (targets) looks at referencers via inbound.
                for (std::size_t i = layers.size(); i > 1; --i)
                    orderAgainst(layers[i - 2], layers[i - 1], /*useFwd=*/false);
            }
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

    GraphFocusQuery ParseGraphFocusQuery(std::string_view raw)
    {
        GraphFocusQuery q;
        std::string_view s = Trim(raw);
        const std::string lower = LowerCopy(s);
        if (lower.starts_with("focus:"))
        {
            s = Trim(s.substr(6));
        }
        if (s.empty() || LowerCopy(s) == "everything" || s == "*")
            return q;
        if (s.front() == '@')
        {
            const std::string_view rest = Trim(s.substr(1));
            if (auto k = KindFromFocusToken(rest))
            {
                q.mode = GraphFocusQuery::Mode::Kind;
                q.kind = *k;
                return q;
            }
            q.mode = GraphFocusQuery::Mode::KindPrefix;
            q.text = LowerCopy(rest);
            return q;
        }
        q.mode = GraphFocusQuery::Mode::Text;
        q.text = LowerCopy(s);
        return q;
    }

    bool MatchesGraphFocusQuery(const GraphFocusQuery& q, const AssetPanelEntry& e)
    {
        if (q.mode == GraphFocusQuery::Mode::Everything)
            return true;
        if (q.mode == GraphFocusQuery::Mode::Kind)
            return e.kind == q.kind;
        if (q.mode == GraphFocusQuery::Mode::KindPrefix)
            return false;   // '@' / '@s' lists keywords, not files
        const std::string name = LowerCopy(e.name);
        const std::string file = LowerCopy(e.fileName);
        return name.find(q.text) != std::string::npos || file.find(q.text) != std::string::npos;
    }

    std::optional<std::string_view> CompleteGraphFocusKindPrefix(std::string_view typed)
    {
        const std::string prefix = LowerCopy(Trim(typed));
        for (const GraphFocusKindKeyword& kw : kGraphFocusKindKeywords)
        {
            const std::string tok = LowerCopy(kw.token);
            if (prefix.empty() || tok.starts_with(prefix))
                return std::string_view{ kw.token };
        }
        if (auto k = KindFromFocusToken(prefix))
        {
            for (const GraphFocusKindKeyword& kw : kGraphFocusKindKeywords)
                if (kw.kind == *k)
                    return std::string_view{ kw.token };
        }
        return std::nullopt;
    }

    void AssetGraphViewModel::Clear()
    {
        nodes.clear();
        edges.clear();
        realNodeCount = 0;
        // buildEpoch is deliberately NOT reset -- see its declaration. Clear()
        // empties the projection; it does not un-count the work already done.
    }

    void AssetGraphViewModel::Build(const GraphBuildInput& in)
    {
        // FIRST, ahead of every early return below: the epoch counts CALLS,
        // not successful projections. A build that refuses its input still
        // consumed the call, and the whole point of the counter is to let a
        // caller's "I only rebuild when my inputs move" be MEASURED. See its
        // declaration in the header.
        ++buildEpoch;

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
        // measure from"): every CONTENT entry is in scope. Source files are
        // omitted unless kindFilter is Source (`@source`) -- an include graph
        // of every .cpp/.hpp in a game project is a hairball, not a map.
        // kindFilter on any other kind seeds that kind only; BFS still
        // discovers their edges (a mesh still shows its materials). A
        // breadth-capped expansion pass still runs so tombstones and overflow
        // accounting match focus-mode's -- the cap just can never evict a
        // seeded entry here.
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
                if (in.kindFilter)
                {
                    if (e.kind != *in.kindFilter)
                        continue;
                }
                else if (e.kind == AssetKind::Source)
                    continue;
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
            // A focus guid that names NOTHING at all -- never a real entry,
            // and no AssetReferenceIndex node either (e.g. stale after the
            // asset it named was deleted and its tombstone itself later
            // garbage-collected once nothing referenced it any more) -- has
            // nothing to show. Without this guard the guid below would be
            // seeded into scope unconditionally and rendered as a FALSE
            // tombstone by Step 4's classification, which only holds for a
            // guid the index actually explains (review round 1, finding 4).
            // A genuine tombstone always has an index Node; this guid does
            // not.
            if (!entries.count(in.focus) && !index.Find(in.focus))
                return;

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
        // Overflow accounting (review round 1, finding 3 -- controller
        // ruling): a candidate that survived THIS node's own per-direction
        // cap can still be vetoed below by the OTHER endpoint's cap (the
        // two-sided-conflict case -- e.g. a low-degree node's one outbound
        // edge, trivially under ITS OWN cap, pointed at a hub whose inbound
        // cap already dropped it). That is one more undrawn connection on
        // THIS node's side too, on top of whatever its own raw-count-minus-
        // cap already tallied at ProcessNode time -- the two can never
        // double-count the SAME candidate, since a candidate is either
        // excluded by this node's own cap (never reaches the loop below at
        // all) or survives it and is only ever evaluated for veto here,
        // never both. The vetoing side needs no matching increment: by
        // AssetReferenceIndex's own forward/inverse consistency, a rejected
        // candidate is ALWAYS one the vetoing side's own raw-cap overflow
        // already counted (that is precisely why it is not in the vetoing
        // side's own survivor list) -- see ProcessNode's own cap.
        std::set<std::pair<Arcane::Guid, Arcane::Guid>> seen;
        for (auto& [g, w] : work)
        {
            if (!w.processed || !scope.count(g))
                continue;
            for (const CandidateEdge& c : w.outboundSurvivors)
            {
                if (!scope.count(c.guid))
                    continue;   // depth-cut, not a breadth cut -- never overflow-accounted
                if (!TargetAcceptsInbound(c.guid, g, work))
                {
                    ++w.outboundOverflow;   // the target's OWN inbound cap dropped this one
                    continue;
                }
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
                    continue;   // depth-cut, not a breadth cut -- never overflow-accounted
                if (!SourceAcceptsOutbound(c.guid, g, work))
                {
                    ++w.inboundOverflow;   // the source's OWN outbound cap dropped this one
                    continue;
                }
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
        ReduceCrossings(byLayer, edges);

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
                    // A scope member with no entry is a tombstone
                    // (AssetReferenceIndex::Update never creates a node for
                    // a guid nothing pointed at -- see its own header
                    // comment) -- exists == false, inbound nonempty. This
                    // invariant depends on the focus-mode guard above
                    // (Step 1's `else` branch): without it, a stale focus
                    // guid absent from both entries and the index would be
                    // seeded into scope unconditionally and land here as a
                    // FALSE tombstone even though it names nothing the
                    // index can explain (review round 1, finding 4). Every
                    // OTHER way a guid enters scope is via a real edge the
                    // index itself produced, so this branch is always a
                    // genuine dangling target for them.
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

            if (w.outboundOverflow > 0)
            {
                GraphNode n;
                n.guid = g;
                n.label = "+" + std::to_string(w.outboundOverflow) + " more";
                n.kind = AssetKind::Other;   // never the anchor's kind (review round 1, finding 2/additional ruling)
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
                n.kind = AssetKind::Other;   // never the anchor's kind (review round 1, finding 2/additional ruling)
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
