#pragma once

// AssetReferenceIndex (asset-manager arc, Plan 2 Task 3): the editor's pure,
// engine-facade-free inverted-reference index over ListAssetReferences
// answers (spec s9.1). Consumes only Arcane::AssetRef/AssetRefKind
// (Assets.hpp:47,53) -- no ImGui, no Arcane::Assets/Arcane::Project
// instance, no AssetRegistry. Task 4 (AssetPanelModel's rebuild) is the sole
// production caller: for every dirtied guid it calls Update with that
// guid's exists bit and its ListAssetReferences answer (nullopt when the
// file could not be read/parsed this walk), and this unit maintains the
// FORWARD graph (Node::outbound, last-known-good) and its INVERSE
// (Node::inbound, sorted-unique referencer guids) so InboundCount/
// DanglingTargets answer without a full registry walk per query.
//
// POLICY-FREE ON PURPOSE: "unused" (kind-gated -- a References-only orphan
// reads differently from a DerivesFrom orphan) is the model's rule to make,
// not this index's; this unit only ever answers "how many/which guids point
// at X", never "is X unused".
//
// THE TWO UE-DERIVED DISCIPLINES THIS UNIT EXISTS TO ENFORCE (spec s9.1):
//   1. REMOVE BEFORE RE-ADD. A re-walk of `id` must retract every inbound
//      edge `id` previously contributed before it adds the fresh set --
//      otherwise a target `id` used to reference but no longer does keeps
//      `id` in its `inbound` forever (the classic incremental-index
//      corruption bug). See Update's own doc comment for the exact order
//      that enforces this.
//   2. TOMBSTONES. A target named by a `References`/`DerivesFrom` edge that
//      this index has not itself walked yet (or that has since been
//      deleted) still needs a Node so its `inbound` list survives --
//      otherwise a dangling reference is silently invisible to the panel.
//      Such a node's `exists` stays false; it is garbage-collected the
//      instant its last referencer lets go, whether by a re-walk that
//      drops the edge or by the referencer itself being deleted.
#include <Arcane/Assets/Assets.hpp>   // AssetRef, AssetRefKind
#include <Arcane/Guid.hpp>

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Arcane::Editor
{
    // Pure reference topology over ListAssetReferences answers (spec s9.1).
    // Policy-free: "unused" (kind-gated) is the model's rule, not this
    // unit's.
    class AssetReferenceIndex
    {
    public:
        struct Node
        {
            std::vector<Arcane::AssetRef> outbound;  // last-known-good (spec s3.2)
            std::vector<Arcane::Guid>     inbound;   // referencers, sorted, unique
            bool exists = false;   // false + inbound nonempty == tombstone
        };

        // Drops every node -- the project-switch/full-rebuild reset.
        void Clear();

        // Re-walk one asset (`id`) with a fresh provider answer, in EXACTLY
        // this order (the discipline IS the order -- see the file header's
        // two UE-derived disciplines above):
        //   1. Fetch/create id's node; set node.exists = exists.
        //   2. !exists: retract every edge id's CURRENT outbound set
        //      contributes (RemoveOutboundEdges), clear id's own outbound,
        //      and erase id's node outright once nothing still references
        //      it (empty inbound) -- a deleted asset with no referencer
        //      leaves no trace at all. Returns immediately; `refs` is never
        //      consulted for a gone asset.
        //   3. !refs (id exists but could not be read/parsed this walk):
        //      return, keeping outbound/inbound exactly as they were --
        //      spec s3.2's last-known-good contract. A freshly-created
        //      node asked with nullopt (never parsed) is left at the
        //      fetch/create default (empty outbound, exists=true) and
        //      contributes no inbound edge anywhere, matching s3.2's
        //      "contributes nothing" sentence.
        //   4. RemoveOutboundEdges(id) again -- retracts id's OLD outbound
        //      set (from before this call) so the fresh set in step 5 is
        //      never additive. For each old target: `id` is removed from
        //      its `inbound` (binary search -- inbound is kept sorted); a
        //      target left with `!exists && inbound.empty()` afterward is
        //      a tombstone nobody references any more and is erased
        //      (garbage collection is always a side effect of a
        //      referencer letting go, never a separate scan).
        //   5. `node.outbound = *refs;` then for each ref: fetch/create the
        //      target's node (a freshly-created node starts exists=false --
        //      a target this index has not walked itself yet is
        //      indistinguishable from a dangling one until its own Update
        //      arrives, which a full build always eventually delivers for
        //      every real asset) and insert `id` into the target's
        //      `inbound`, sorted-unique.
        // RemoveOutboundEdges (both step 2 and step 4) reads a snapshot of
        // the node's PRE-CLEAR outbound -- step 5 overwrites node.outbound
        // in place, so retraction must always read the old set before that
        // assignment, never after.
        void Update(const Arcane::Guid& id, bool exists,
                    const std::optional<std::vector<Arcane::AssetRef>>& refs);

        // Referencer count for `id` -- Node::inbound.size(), 0 for a guid
        // this index has never seen at all (never walked and never named
        // as a target).
        [[nodiscard]] int InboundCount(const Arcane::Guid& id) const;

        // The node for `id`, or nullptr if this index has no entry for it
        // at all (never walked, never named as a target, or already
        // garbage-collected).
        [[nodiscard]] const Node* Find(const Arcane::Guid& id) const;

        // Every tombstone (exists == false) that still has at least one
        // referencer -- the panel's dangling-reference surface. A
        // tombstone with zero referencers cannot exist: Update's step 2/4
        // garbage-collects it the instant its last referencer lets go.
        // Iteration order is unspecified (an unordered_map) -- a consumer
        // that displays these MUST sort.
        [[nodiscard]] std::vector<Arcane::Guid> DanglingTargets() const;

        // Total node count, tombstones included -- a cheap health/debug
        // readout, not a production UI number.
        [[nodiscard]] std::size_t NodeCount() const;

    private:
        // Retracts every edge `source`'s CURRENT (pre-clear) outbound set
        // contributes: removes `source` from each target's sorted
        // `inbound` and erases a target left as an unreferenced tombstone.
        // Shared by Update's step 2 (asset gone) and step 4 (re-walk) --
        // see Update's own doc comment for why both call sites must read
        // outbound before it is cleared/overwritten.
        void RemoveOutboundEdges(const Arcane::Guid& source);

        std::unordered_map<Arcane::Guid, Node> m_nodes;
    };
}
