#pragma once

// AssetGraphViewModel (asset-manager arc, Plan 3 Task 2): the Graph lens's
// brain -- a pure, headless-testable projection of an injected entries map
// (AssetPanelModel::Entries() shape) plus an AssetReferenceIndex into a
// scoped, layered, breadth-capped node/edge set. Tasks 3-6 draw and interact
// with exactly the structs below; nothing here touches ImGui, ax::NodeEditor,
// or AssetPanelModel itself -- the model type stays out of this unit on
// purpose so the [editor] tests drive it with a hand-built entries map +
// index, the same posture AssetReferenceIndexTest.cpp already established
// for the index this unit consumes.
//
// THE ALGORITHM, IN ONE PASS:
//   1. Scope (which guids become nodes): focus non-nil -> BFS from focus,
//      per-node-per-direction breadth-capped AT EACH HOP so an over-cap
//      neighbor never enters scope at all (the UE Reference Viewer
//      precedent the plan cites -- never silent truncation, but also never
//      a phantom node the cap was supposed to hide), capped at depthLimit
//      hops per direction (spec s10, ruling 6). focus nil ("everything") --
//      ruling 6 verbatim: "there is no root to measure from" -- every entry
//      is unconditionally in scope (no reachability gate), and the SAME
//      per-node breadth cap still runs over every entry's own edges so the
//      "+N more" accounting and edge trimming happen identically in both
//      modes; the only difference is that in everything-mode an over-cap
//      NEIGHBOR remains visible anyway (it is independently a root/entry),
//      so the cap trims which EDGES draw, not which entries appear. Tasks
//      3-6 render both modes off the same fields; this asymmetry is
//      intentional and covered by its own test case. Boundary case: two
//      nodes BOTH admitted at exactly depthLimit hops are visible, but an
//      edge between them draws no wire and gets no overflow accounting --
//      neither endpoint is ever processed, since the BFS stops admitting
//      before processing the last hop's frontier. Matches the UE Reference
//      Viewer; intentional.
//   2. Layer (the column): layer(n) = longest outbound-path length to a
//      leaf, a memoized DFS over the FINAL surviving edge set (post-scope,
//      post-cap) with an on-stack cycle guard -- a cycle member's layer is
//      the max of its non-cycle continuations (the cyclic edge itself is
//      skipped, not treated as a zero-length leaf). DFS is kicked off in a
//      fixed guid-sorted order so a genuine cycle's result (which the
//      guard makes traversal-order-sensitive by construction) is still
//      reproducible build-to-build, never an unordered_map artifact.
//   3. Row (the stacking index): real nodes group by layer, name-sorted
//      (short-guid-sorted for a tombstone, which has no name) within the
//      column. A synthetic overflow node's row stacks beneath every real
//      row already placed in its column.
// See AssetGraphViewModel.cpp's anonymous namespace for the per-step
// helpers (SortKey/ShortGuid/EdgeKindBetween/CapEdges/LabelFor).
#include "Panels/AssetPanelModel.hpp"        // AssetPanelEntry, AssetKind (entries map value type)
#include "Panels/AssetReferenceIndex.hpp"    // AssetReferenceIndex

#include <Arcane/Assets/Assets.hpp>   // AssetRefKind
#include <Arcane/Guid.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Arcane::Editor
{
    // One node in the built graph. Synthetic overflow nodes ("+N more") have
    // isOverflow=true, a nil guid is NOT used for them -- they carry the guid
    // of the node they overflow FROM plus a direction tag, so ids stay stable.
    struct GraphNode
    {
        Arcane::Guid guid;                 // real asset, or overflow anchor
        std::string  label;                // name, or "+N more"
        // AssetKind::Other for an overflow node, ALWAYS -- never the anchor's
        // own kind (review round 1, finding 2/additional ruling). A "+N
        // more" node has no accent color of its own to wear; Task 3's kind-
        // color table must not paint it as if it were one more instance of
        // whatever it overflowed from.
        AssetKind    kind = AssetKind::Other;
        int          layer = 0;            // column, 0 = left (sources)
        int          row = 0;              // stacking index within the column
        bool         isOverflow = false;
        bool         overflowInbound = false; // which side it truncates
        // PINNED SEMANTIC (review round 1, finding 3 -- controller ruling):
        // "N undrawn connections on THIS node's (guid's) side, in the
        // `overflowInbound` direction." Every candidate edge that did not
        // get drawn counts here, for EITHER reason a candidate can fail to
        // become a GraphEdge -- this node's own per-direction breadth cap,
        // OR the OTHER endpoint's own cap vetoing an edge this node's cap
        // would otherwise have kept. This is NOT a claim that N *nodes* are
        // hidden: in everything-mode every entry is unconditionally a node
        // regardless of the cap, so the guid(s) on the other end of those N
        // undrawn edges may be perfectly visible elsewhere in the graph --
        // the count is scoped to CONNECTIONS on this node's side, never to
        // the visibility of whatever is on the other end of them.
        int          overflowCount = 0;
        bool         isTombstone = false;  // dangling target (exists=false)
    };

    // Edge with its display label already decided (ruling 9).
    struct GraphEdge
    {
        Arcane::Guid from;                 // referencer
        Arcane::Guid to;                   // target
        Arcane::AssetRefKind kind = Arcane::AssetRefKind::References;
        const char* label = "uses";        // "derives" | "uses" | "samples"
    };

    struct GraphBuildInput
    {
        // Injected snapshots -- the unit never touches AssetPanelModel.
        const std::unordered_map<Arcane::Guid, AssetPanelEntry>* entries = nullptr;
        const AssetReferenceIndex* index = nullptr;
        Arcane::Guid focus;                // nil = "everything" (ruling 6)
        int depthLimit = 2;                // per direction, from focus
        int breadthCap = 20;               // per node per direction
    };

    struct AssetGraphViewModel
    {
        std::vector<GraphNode> nodes;      // real + overflow, layer/row assigned
        // Between REAL nodes only -- "real" here means non-overflow (never
        // touches a synthetic "+N more" node). A tombstone DOES count as
        // real for this purpose: ruling 11 wants a dangling reference's edge
        // to have pixels, so an edge into/out of a tombstone renders like
        // any other. Do not confuse this "real" with realNodeCount's below
        // -- the two words exclude DIFFERENT sets.
        std::vector<GraphEdge> edges;
        // "N of M"'s N (ruling 12) -- real ASSET nodes: excludes BOTH
        // synthetic overflow nodes AND tombstones (a tombstone is not an
        // asset -- that is the entire point of one). See `edges`' own
        // comment just above for why this is a narrower "real" than that
        // one.
        int realNodeCount = 0;

        // How many times Build() has run on THIS object, ever. Not display
        // data and not part of the projection -- it exists so a consumer's
        // "I do not rebuild this per frame" claim is MEASURABLE rather than
        // merely asserted. The Graph lens rebuilds only when its inputs move
        // (AssetPanelModel::entriesStamp or the focus guid), and the
        // device-less canvas test pins that by counting builds across a fixed
        // number of frames -- an assertion that goes red the moment the
        // panel's guard is removed, which a "the stamps agree afterwards"
        // check cannot do (every rebuild makes them agree).
        //
        // MONOTONIC: bumped on EVERY entry to Build(), including the early
        // returns (a refused build still consumed the call, and the caller
        // still decided to make it), and deliberately NOT reset by Clear() --
        // Clear() empties the projection, it does not un-count the work
        // already done. Same reasoning as AssetPanelModel::entriesStamp:
        // a counter that only ever increases cannot accidentally compare
        // equal to a stale reading.
        std::uint32_t buildEpoch = 0;

        void Build(const GraphBuildInput& in);
        void Clear();
    };
}
