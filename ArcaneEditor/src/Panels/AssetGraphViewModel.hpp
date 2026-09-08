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
//      intentional and covered by its own test case.
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
        AssetKind    kind = AssetKind::Other;
        int          layer = 0;            // column, 0 = left (sources)
        int          row = 0;              // stacking index within the column
        bool         isOverflow = false;
        bool         overflowInbound = false; // which side it truncates
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
        std::vector<GraphEdge> edges;      // between real nodes only
        int realNodeCount = 0;             // "N of M"'s N (ruling 12)

        void Build(const GraphBuildInput& in);
        void Clear();
    };
}
