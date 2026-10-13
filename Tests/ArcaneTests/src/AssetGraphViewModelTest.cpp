// Asset-manager arc (Plan 3 Task 2): AssetGraphViewModel -- the Graph lens's
// pure, headless-testable projection of an injected entries map + a hand-
// built AssetReferenceIndex into a scoped, layered, breadth-capped node/edge
// set. Same posture as AssetReferenceIndexTest.cpp: no registry, no temp
// dirs, no filesystem at all -- every guid is a Guid::FromString literal (or
// GuidN's deterministic synthesis for the many-referencer cases), every
// entry is hand-built, every "refs answer" the local Refs() helper.

#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetGraphViewModel.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Arcane::Editor;

namespace
{
    Arcane::Guid ParseGuid(const char* s)
    {
        const auto g = Arcane::Guid::FromString(s);
        REQUIRE(g.has_value());
        return *g;
    }

    // Deterministic synthesis for cases that need many distinct guids (the
    // breadth-cap and everything-mode cases) -- `group` keeps different
    // TEST_CASEs' synthesized guids from colliding, `n` distinguishes within
    // one group.
    Arcane::Guid GuidN(int group, int n)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "7e5e%04x-0002-4002-8002-%012x",
                      static_cast<unsigned>(group), static_cast<unsigned>(n));
        return ParseGuid(buf);
    }

    std::optional<std::vector<Arcane::AssetRef>> Refs(std::vector<Arcane::AssetRef> refs)
    {
        return refs;
    }

    AssetPanelEntry MakeEntry(const Arcane::Guid& guid, std::string name, AssetKind kind)
    {
        AssetPanelEntry e;
        e.guid = guid;
        e.name = std::move(name);
        e.fileName = e.name;
        e.mountPath = "game://" + e.name;
        e.kind = kind;
        return e;
    }

    // A REAL (non-overflow) node with this guid, or nullptr.
    const GraphNode* FindReal(const std::vector<GraphNode>& nodes, const Arcane::Guid& guid)
    {
        for (const GraphNode& n : nodes)
            if (!n.isOverflow && n.guid == guid)
                return &n;
        return nullptr;
    }

    // The overflow node anchored at `guid` on the named side, or nullptr.
    // Overflow nodes reuse their anchor's guid (the header's own "a nil guid
    // is NOT used for them" rule), so isOverflow + overflowInbound together
    // are the only way to disambiguate from the anchor's own real node.
    const GraphNode* FindOverflow(const std::vector<GraphNode>& nodes, const Arcane::Guid& anchor, bool inbound)
    {
        for (const GraphNode& n : nodes)
            if (n.isOverflow && n.overflowInbound == inbound && n.guid == anchor)
                return &n;
        return nullptr;
    }

    const GraphEdge* FindEdge(const std::vector<GraphEdge>& edges, const Arcane::Guid& from, const Arcane::Guid& to)
    {
        for (const GraphEdge& e : edges)
            if (e.from == from && e.to == to)
                return &e;
        return nullptr;
    }
}

TEST_CASE("AssetGraphViewModel layers by dependency depth: sources left, scenes right", "[editor]")
{
    // texture <- sprite <- scene (DerivesFrom chain): layer(texture)=0,
    // sprite=1, scene=2. material samples texture (References) -- its own
    // longest outbound chain is 1 hop to the same leaf, so it lands in
    // sprite's layer purely because the chain length says so, not because
    // of any special-casing. mesh references material (References) --
    // 1+layer(material) puts it in scene's layer. This single graph also
    // pins all three edge display labels (ruling 9): DerivesFrom always
    // "derives"; a material-kind SOURCE's References edge is "samples";
    // every other References edge is "uses" (mesh -> material here).
    const auto texture  = ParseGuid("7e5e0001-0001-4001-8001-000000000001");
    const auto sprite   = ParseGuid("7e5e0001-0001-4001-8001-000000000002");
    const auto scene    = ParseGuid("7e5e0001-0001-4001-8001-000000000003");
    const auto material = ParseGuid("7e5e0001-0001-4001-8001-000000000004");
    const auto mesh     = ParseGuid("7e5e0001-0001-4001-8001-000000000005");

    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;
    entries[texture]  = MakeEntry(texture, "texture", AssetKind::Texture);
    entries[sprite]   = MakeEntry(sprite, "sprite", AssetKind::Sprite);
    entries[scene]    = MakeEntry(scene, "scene", AssetKind::Scene);
    entries[material] = MakeEntry(material, "material", AssetKind::Material);
    entries[mesh]     = MakeEntry(mesh, "mesh", AssetKind::Mesh);

    AssetReferenceIndex index;
    index.Update(texture, true, Refs({}));
    index.Update(sprite, true, Refs({ { texture, Arcane::AssetRefKind::DerivesFrom } }));
    index.Update(scene, true, Refs({ { sprite, Arcane::AssetRefKind::DerivesFrom } }));
    index.Update(material, true, Refs({ { texture, Arcane::AssetRefKind::References } }));
    index.Update(mesh, true, Refs({ { material, Arcane::AssetRefKind::References } }));

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;   // focus left nil -> everything mode, no reachability restriction

    AssetGraphViewModel vm;
    vm.Build(in);

    CHECK(vm.realNodeCount == 5);
    REQUIRE(vm.nodes.size() == 5);

    const GraphNode* t = FindReal(vm.nodes, texture);
    const GraphNode* sp = FindReal(vm.nodes, sprite);
    const GraphNode* sc = FindReal(vm.nodes, scene);
    const GraphNode* ma = FindReal(vm.nodes, material);
    const GraphNode* me = FindReal(vm.nodes, mesh);
    REQUIRE((t && sp && sc && ma && me));

    CHECK(t->layer == 0);
    CHECK(sp->layer == 1);
    CHECK(ma->layer == 1);
    CHECK(sc->layer == 2);
    CHECK(me->layer == 2);

    // Rows: name-sorted within the column -- "material" < "sprite",
    // "mesh" < "scene".
    CHECK(ma->row == 0);
    CHECK(sp->row == 1);
    CHECK(me->row == 0);
    CHECK(sc->row == 1);

    REQUIRE(vm.edges.size() == 4);
    const GraphEdge* spToTex = FindEdge(vm.edges, sprite, texture);
    const GraphEdge* scToSp = FindEdge(vm.edges, scene, sprite);
    const GraphEdge* maToTex = FindEdge(vm.edges, material, texture);
    const GraphEdge* meToMa = FindEdge(vm.edges, mesh, material);
    REQUIRE((spToTex && scToSp && maToTex && meToMa));

    CHECK(spToTex->kind == Arcane::AssetRefKind::DerivesFrom);
    CHECK(std::string(spToTex->label) == "derives");
    CHECK(scToSp->kind == Arcane::AssetRefKind::DerivesFrom);
    CHECK(std::string(scToSp->label) == "derives");
    CHECK(maToTex->kind == Arcane::AssetRefKind::References);
    CHECK(std::string(maToTex->label) == "samples");   // material-kind source
    CHECK(meToMa->kind == Arcane::AssetRefKind::References);
    CHECK(std::string(meToMa->label) == "uses");       // mesh is not a material
}

TEST_CASE("AssetGraphViewModel scopes to focus: BFS depth 2 both directions", "[editor]")
{
    // Chain Z->A->B->C->D->E->F (each references the next). Focused on C
    // with the default depthLimit=2: outbound reaches D(1)/E(2), inbound
    // (referencers) reaches B(1)/A(2) -- Z and F sit at hop 3 in their
    // respective direction and are excluded.
    const auto Z = ParseGuid("7e5e0002-0001-4001-8001-00000000005a");
    const auto A = ParseGuid("7e5e0002-0001-4001-8001-00000000000a");
    const auto B = ParseGuid("7e5e0002-0001-4001-8001-00000000000b");
    const auto C = ParseGuid("7e5e0002-0001-4001-8001-00000000000c");
    const auto D = ParseGuid("7e5e0002-0001-4001-8001-00000000000d");
    const auto E = ParseGuid("7e5e0002-0001-4001-8001-00000000000e");
    const auto F = ParseGuid("7e5e0002-0001-4001-8001-00000000005f");

    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;
    entries[Z] = MakeEntry(Z, "Z", AssetKind::Data);
    entries[A] = MakeEntry(A, "A", AssetKind::Data);
    entries[B] = MakeEntry(B, "B", AssetKind::Data);
    entries[C] = MakeEntry(C, "C", AssetKind::Data);
    entries[D] = MakeEntry(D, "D", AssetKind::Data);
    entries[E] = MakeEntry(E, "E", AssetKind::Data);
    entries[F] = MakeEntry(F, "F", AssetKind::Data);

    AssetReferenceIndex index;
    index.Update(F, true, Refs({}));
    index.Update(E, true, Refs({ { F, Arcane::AssetRefKind::References } }));
    index.Update(D, true, Refs({ { E, Arcane::AssetRefKind::References } }));
    index.Update(C, true, Refs({ { D, Arcane::AssetRefKind::References } }));
    index.Update(B, true, Refs({ { C, Arcane::AssetRefKind::References } }));
    index.Update(A, true, Refs({ { B, Arcane::AssetRefKind::References } }));
    index.Update(Z, true, Refs({ { A, Arcane::AssetRefKind::References } }));

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;
    in.focus = C;

    AssetGraphViewModel vm;
    vm.Build(in);

    CHECK(vm.realNodeCount == 5);
    for (const auto& g : { A, B, C, D, E })
        CHECK(FindReal(vm.nodes, g) != nullptr);
    CHECK(FindReal(vm.nodes, Z) == nullptr);
    CHECK(FindReal(vm.nodes, F) == nullptr);

    // No overflow anywhere -- the exclusion is a depth cut, not a breadth
    // cut, and only breadth cuts get a synthetic node.
    for (const GraphNode& n : vm.nodes)
        CHECK_FALSE(n.isOverflow);

    CHECK(FindEdge(vm.edges, A, B) != nullptr);
    CHECK(FindEdge(vm.edges, B, C) != nullptr);
    CHECK(FindEdge(vm.edges, C, D) != nullptr);
    CHECK(FindEdge(vm.edges, D, E) != nullptr);
    CHECK(FindEdge(vm.edges, Z, A) == nullptr);
    CHECK(FindEdge(vm.edges, E, F) == nullptr);
}

TEST_CASE("AssetGraphViewModel caps breadth with a synthetic overflow node, most-important edges first", "[editor]")
{
    // One texture, focused, with breadthCap(20)+5 = 25 inbound referencers:
    // 3 via DerivesFrom, 22 via References. The 3 DerivesFrom referencers
    // are named zzz00..zzz02 -- deliberately sorting AFTER every ref* name
    // (review round 1, finding 1: der00-02 sorted before ref00-21 by NAME
    // ALONE, so that construction could not actually discriminate the
    // DerivesFrom-first rule from a plain name sort -- deleting the kind
    // clause in SortByImportance would have passed it unnoticed). With
    // zzz-naming, a plain name sort would keep ref00..ref19 and drop EVERY
    // zzz* entry; DerivesFrom-first sorting keeps all 3 zzz* survivors
    // before a single ref* one, so only 17 of the 22 References referencers
    // (name-sorted: ref00..ref16) fill the remaining budget, leaving
    // ref17..ref21 as the "+5 more" overflow -- genuinely absent from the
    // graph (UE Reference Viewer precedent: never silent truncation, but
    // never a phantom node either).
    const auto texture = GuidN(3, 0);
    std::vector<Arcane::Guid> derives, refs;
    for (int i = 0; i < 3; ++i) derives.push_back(GuidN(3, 100 + i));
    for (int i = 0; i < 22; ++i) refs.push_back(GuidN(3, 200 + i));

    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;
    entries[texture] = MakeEntry(texture, "texture", AssetKind::Texture);
    char name[16];
    for (int i = 0; i < 3; ++i)
    {
        std::snprintf(name, sizeof(name), "zzz%02d", i);
        entries[derives[i]] = MakeEntry(derives[i], name, AssetKind::Sprite);
    }
    for (int i = 0; i < 22; ++i)
    {
        std::snprintf(name, sizeof(name), "ref%02d", i);
        entries[refs[i]] = MakeEntry(refs[i], name, AssetKind::Sprite);
    }

    AssetReferenceIndex index;
    index.Update(texture, true, Refs({}));
    for (const auto& g : derives)
        index.Update(g, true, Refs({ { texture, Arcane::AssetRefKind::DerivesFrom } }));
    for (const auto& g : refs)
        index.Update(g, true, Refs({ { texture, Arcane::AssetRefKind::References } }));

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;
    in.focus = texture;   // default depthLimit=2, default breadthCap=20

    AssetGraphViewModel vm;
    vm.Build(in);

    CHECK(vm.realNodeCount == 21);   // texture + 3 DerivesFrom + 17 References survivors

    for (const auto& g : derives)
        CHECK(FindReal(vm.nodes, g) != nullptr);
    for (int i = 0; i < 17; ++i)
        CHECK(FindReal(vm.nodes, refs[static_cast<std::size_t>(i)]) != nullptr);
    for (int i = 17; i < 22; ++i)
        CHECK(FindReal(vm.nodes, refs[static_cast<std::size_t>(i)]) == nullptr);   // collapsed into overflow

    const GraphNode* overflow = FindOverflow(vm.nodes, texture, /*inbound*/ true);
    REQUIRE(overflow);
    CHECK(overflow->overflowCount == 5);
    CHECK(overflow->label == "+5 more");
    CHECK(FindReal(vm.nodes, texture) != nullptr);   // texture itself is real, not the overflow node

    for (const auto& g : derives)
        REQUIRE(FindEdge(vm.edges, g, texture) != nullptr);
    int edgesIntoTexture = 0;
    for (const GraphEdge& e : vm.edges)
        if (e.to == texture) ++edgesIntoTexture;
    CHECK(edgesIntoTexture == 20);
}

TEST_CASE("AssetGraphViewModel caps breadth on the OUTBOUND direction with its own overflow node", "[editor]")
{
    // Review round 1, finding 2: the test above only ever exercised the
    // INBOUND side of the per-node-per-direction cap (a hub too many things
    // point AT). This case exercises the OTHER side -- one node, X, that
    // itself points at breadthCap(20)+5 = 25 distinct leaf targets. 20
    // survive by name (t00..t19); the other 5 (t20..t24) collapse into an
    // outboundOverflow (overflowInbound=false) "+5 more" node.
    const auto X = GuidN(8, 0);
    std::vector<Arcane::Guid> targets;
    for (int i = 0; i < 25; ++i)
        targets.push_back(GuidN(8, 100 + i));

    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;
    entries[X] = MakeEntry(X, "X", AssetKind::Material);
    char name[16];
    for (int i = 0; i < 25; ++i)
    {
        std::snprintf(name, sizeof(name), "t%02d", i);
        entries[targets[static_cast<std::size_t>(i)]] = MakeEntry(targets[static_cast<std::size_t>(i)], name, AssetKind::Texture);
    }

    AssetReferenceIndex index;
    for (const auto& t : targets)
        index.Update(t, true, Refs({}));
    std::vector<Arcane::AssetRef> refs;
    refs.reserve(targets.size());
    for (const auto& t : targets)
        refs.push_back({ t, Arcane::AssetRefKind::References });
    index.Update(X, true, Refs(refs));

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;
    in.focus = X;   // default depthLimit=2, default breadthCap=20

    AssetGraphViewModel vm;
    vm.Build(in);

    for (int i = 0; i < 20; ++i)
        CHECK(FindReal(vm.nodes, targets[static_cast<std::size_t>(i)]) != nullptr);
    for (int i = 20; i < 25; ++i)
        CHECK(FindReal(vm.nodes, targets[static_cast<std::size_t>(i)]) == nullptr);   // collapsed into overflow

    const GraphNode* overflow = FindOverflow(vm.nodes, X, /*inbound*/ false);
    REQUIRE(overflow);
    CHECK(overflow->overflowCount == 5);
    CHECK(overflow->label == "+5 more");
    CHECK_FALSE(overflow->overflowInbound);
    CHECK(overflow->kind == AssetKind::Other);   // never the anchor's own kind (additional ruling)
    CHECK_FALSE(overflow->isTombstone);

    int overflowNodesInResult = 0;
    for (const GraphNode& n : vm.nodes)
        if (n.isOverflow) ++overflowNodesInResult;
    CHECK(overflowNodesInResult == 1);
    CHECK(vm.realNodeCount == 21);   // X + 20 surviving targets, the overflow node excluded

    // Placement: X's own longest-outbound-to-leaf chain is 1 hop (every
    // surviving target is itself a leaf), so X sits at layer 1; its
    // outbound overflow node sits one column TOWARD THE SOURCES (layer 0)
    // -- the opposite direction from an inbound overflow node (test above),
    // which sits one column AWAY from the sources.
    const GraphNode* xNode = FindReal(vm.nodes, X);
    REQUIRE(xNode);
    CHECK(xNode->layer == 1);
    CHECK(overflow->layer == 0);
}

TEST_CASE("AssetGraphViewModel renders tombstones", "[editor]")
{
    // A derives from M, but M has no entry and was never itself walked --
    // AssetReferenceIndex auto-creates M's node with exists=false the
    // moment A's Update names it (AssetReferenceIndexTest.cpp's "tombstones
    // an unresolvable target" case is the same construction). M must still
    // appear -- as a ghost, not silently invisible (ruling 11) -- with a
    // short-guid label since it has no name to show.
    const auto A = ParseGuid("7e5e0004-0001-4001-8001-00000000000a");
    const auto M = ParseGuid("7e5e0004-0001-4001-8001-0000000000dd");

    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;
    entries[A] = MakeEntry(A, "A", AssetKind::Sprite);

    AssetReferenceIndex index;
    index.Update(A, true, Refs({ { M, Arcane::AssetRefKind::DerivesFrom } }));

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;   // everything mode

    AssetGraphViewModel vm;
    vm.Build(in);

    CHECK(vm.realNodeCount == 1);   // M is not a real asset -- it does not exist
    REQUIRE(vm.nodes.size() == 2);

    const GraphNode* aNode = FindReal(vm.nodes, A);
    REQUIRE(aNode);
    CHECK_FALSE(aNode->isTombstone);

    const GraphNode* mNode = nullptr;
    for (const GraphNode& n : vm.nodes)
        if (n.guid == M && !n.isOverflow)
            mNode = &n;
    REQUIRE(mNode);
    CHECK(mNode->isTombstone);
    CHECK(mNode->label == M.ToString().substr(0, 8));

    const GraphEdge* e = FindEdge(vm.edges, A, M);
    REQUIRE(e);
    CHECK(e->kind == Arcane::AssetRefKind::DerivesFrom);
    CHECK(std::string(e->label) == "derives");
}

TEST_CASE("AssetGraphViewModel everything-mode applies no depth cut but keeps breadth caps", "[editor]")
{
    // A 5-node reference chain with depthLimit deliberately set to 1 (which
    // would truncate hard in focus mode) proves "everything" really does
    // skip the depth cut entirely (ruling 6: "there is no root to measure
    // from"). A hub H with breadthCap(20)+3=23 inbound referencers proves
    // the breadth cap still runs: unlike focus mode, the 3 excess
    // referencers are NOT evicted (every entry is unconditionally in scope
    // here), only the EDGE into H and the overflow accounting are capped.
    //
    // Review round 1, finding 3 (controller ruling): each of the 3 excess
    // referencers (h20..h22, name-sorted last) ALSO gets its own outbound
    // "+1 more" overflow node -- the "+N more" node's pinned semantic is "N
    // undrawn connections on THIS node's side", so h2x's one dropped edge
    // counts on h2x's own side too, even though h2x itself remains a
    // perfectly visible node here (everything-mode never evicts an entry).
    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;

    std::vector<Arcane::Guid> chain;
    char name[16];
    for (int i = 0; i < 5; ++i)
    {
        chain.push_back(GuidN(5, i));
        std::snprintf(name, sizeof(name), "chain%d", i);
        entries[chain.back()] = MakeEntry(chain.back(), name, AssetKind::Data);
    }

    const auto hub = GuidN(5, 100);
    entries[hub] = MakeEntry(hub, "hub", AssetKind::Texture);
    std::vector<Arcane::Guid> referencers;
    for (int i = 0; i < 23; ++i)
    {
        referencers.push_back(GuidN(5, 200 + i));
        std::snprintf(name, sizeof(name), "h%02d", i);
        entries[referencers.back()] = MakeEntry(referencers.back(), name, AssetKind::Sprite);
    }

    AssetReferenceIndex index;
    for (std::size_t i = 0; i + 1 < chain.size(); ++i)
        index.Update(chain[i], true, Refs({ { chain[i + 1], Arcane::AssetRefKind::References } }));
    index.Update(chain.back(), true, Refs({}));
    index.Update(hub, true, Refs({}));
    for (const auto& g : referencers)
        index.Update(g, true, Refs({ { hub, Arcane::AssetRefKind::References } }));

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;
    in.depthLimit = 1;   // everything mode ignores this entirely

    AssetGraphViewModel vm;
    vm.Build(in);

    CHECK(vm.realNodeCount == static_cast<int>(entries.size()));   // every entry, no eviction

    for (const auto& g : chain)
        CHECK(FindReal(vm.nodes, g) != nullptr);   // no depth cut despite depthLimit=1
    for (const auto& g : referencers)
        CHECK(FindReal(vm.nodes, g) != nullptr);   // no eviction despite the breadth cap

    const GraphNode* overflow = FindOverflow(vm.nodes, hub, /*inbound*/ true);
    REQUIRE(overflow);
    CHECK(overflow->overflowCount == 3);
    CHECK(overflow->label == "+3 more");
    CHECK(overflow->kind == AssetKind::Other);   // never the anchor's own kind (additional ruling)

    int edgesIntoHub = 0;
    for (const GraphEdge& e : vm.edges)
        if (e.to == hub) ++edgesIntoHub;
    CHECK(edgesIntoHub == 20);   // capped even though every referencer is still a node

    // h00..h19 (the 20 survivors, name-sorted) drew their edge -- no
    // outbound overflow node of their own.
    for (int i = 0; i < 20; ++i)
    {
        const GraphNode* o = FindOverflow(vm.nodes, referencers[static_cast<std::size_t>(i)], /*inbound*/ false);
        CHECK(o == nullptr);
    }
    // h20..h22 (name-sorted last) lost their sole edge to hub's inbound cap
    // -- each gets its OWN "+1 more" outbound overflow node.
    for (int i = 20; i < 23; ++i)
    {
        const GraphNode* o = FindOverflow(vm.nodes, referencers[static_cast<std::size_t>(i)], /*inbound*/ false);
        REQUIRE(o);
        CHECK(o->overflowCount == 1);
        CHECK(o->label == "+1 more");
    }
}

TEST_CASE("AssetGraphViewModel guards a reference cycle without hanging, layer = max of non-cycle continuations", "[editor]")
{
    // C1 <-> C2 (a 2-cycle) plus C2 -> Leaf. Kickoff order for the layer DFS
    // is guid-sorted (never unordered_map order), so this is reproducible:
    // DFS(C1) recurses into DFS(C2), which skips the C2->C1 back-edge
    // (C1 is on-stack) and takes only its non-cycle continuation C2->Leaf,
    // giving layer(C2)=1; layer(C1)=1+layer(C2)=2. Without the on-stack
    // guard this would recurse C1->C2->C1->C2->... forever -- this case's
    // real job is proving Build() returns at all.
    const auto C1 = ParseGuid("7e5e0006-0001-4001-8001-000000000001");
    const auto C2 = ParseGuid("7e5e0006-0001-4001-8001-000000000002");
    const auto Leaf = ParseGuid("7e5e0006-0001-4001-8001-000000000003");

    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;
    entries[C1] = MakeEntry(C1, "C1", AssetKind::Data);
    entries[C2] = MakeEntry(C2, "C2", AssetKind::Data);
    entries[Leaf] = MakeEntry(Leaf, "Leaf", AssetKind::Data);

    AssetReferenceIndex index;
    index.Update(Leaf, true, Refs({}));
    index.Update(C2, true, Refs({ { C1, Arcane::AssetRefKind::References }, { Leaf, Arcane::AssetRefKind::References } }));
    index.Update(C1, true, Refs({ { C2, Arcane::AssetRefKind::References } }));

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;

    AssetGraphViewModel vm;
    vm.Build(in);   // must return -- the whole point of this case

    CHECK(vm.realNodeCount == 3);
    const GraphNode* c1 = FindReal(vm.nodes, C1);
    const GraphNode* c2 = FindReal(vm.nodes, C2);
    const GraphNode* leaf = FindReal(vm.nodes, Leaf);
    REQUIRE((c1 && c2 && leaf));

    CHECK(leaf->layer == 0);
    CHECK(c2->layer == 1);
    CHECK(c1->layer == 2);
}

TEST_CASE("AssetGraphViewModel yields an empty result for a focus guid absent from both entries and the index", "[editor]")
{
    // Review round 1, finding 4: a stale focus (e.g. the survivor of a
    // delete+GC that removed the asset's entry AND, once nothing
    // referenced it any more, its tombstone too) names nothing the index
    // can explain at all -- Build() must yield the empty graph, never a
    // FALSE tombstone rendered for a guid nobody can vouch for. The entries
    // map and index below are populated (proving the guard checks the
    // FOCUS specifically, not "is the whole input empty"), but the focus
    // guid itself appears in neither.
    const auto A = ParseGuid("7e5e0009-0001-4001-8001-00000000000a");
    const auto B = ParseGuid("7e5e0009-0001-4001-8001-00000000000b");
    const auto ghost = ParseGuid("7e5e0009-0001-4001-8001-0000000000ff");

    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;
    entries[A] = MakeEntry(A, "A", AssetKind::Data);
    entries[B] = MakeEntry(B, "B", AssetKind::Data);

    AssetReferenceIndex index;
    index.Update(B, true, Refs({}));
    index.Update(A, true, Refs({ { B, Arcane::AssetRefKind::References } }));

    REQUIRE_FALSE(entries.count(ghost));
    REQUIRE(index.Find(ghost) == nullptr);

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;
    in.focus = ghost;

    AssetGraphViewModel vm;
    vm.Build(in);

    CHECK(vm.nodes.empty());
    CHECK(vm.edges.empty());
    CHECK(vm.realNodeCount == 0);
}

TEST_CASE("AssetGraphViewModel Clear() empties nodes, edges and realNodeCount", "[editor]")
{
    const auto A = ParseGuid("7e5e0007-0001-4001-8001-00000000000a");
    const auto B = ParseGuid("7e5e0007-0001-4001-8001-00000000000b");

    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;
    entries[A] = MakeEntry(A, "A", AssetKind::Data);
    entries[B] = MakeEntry(B, "B", AssetKind::Data);

    AssetReferenceIndex index;
    index.Update(B, true, Refs({}));
    index.Update(A, true, Refs({ { B, Arcane::AssetRefKind::References } }));

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;

    AssetGraphViewModel vm;
    vm.Build(in);
    REQUIRE_FALSE(vm.nodes.empty());
    REQUIRE_FALSE(vm.edges.empty());
    REQUIRE(vm.realNodeCount > 0);

    vm.Clear();
    CHECK(vm.nodes.empty());
    CHECK(vm.edges.empty());
    CHECK(vm.realNodeCount == 0);
}

TEST_CASE("AssetGraphViewModel: a Model node wears a distinct accent, not grab-gray",
          "[editor]")
{
    // F2c Plan 2 Task 8: KindAccentRgb extends §11.3 with Model #5b7fb0.
    // The graph panel's KindAccentColor maps 0 to Theme::kGrab; a non-zero
    // value distinct from Mesh and Sprite is what keeps a Model node from
    // collapsing into the unnamed-kind gray.
    CHECK(KindAccentRgb(AssetKind::Model) == 0x5b7fb0u);
    CHECK(KindAccentRgb(AssetKind::Model) != 0);
    CHECK(KindAccentRgb(AssetKind::Model) != KindAccentRgb(AssetKind::Mesh));
    CHECK(KindAccentRgb(AssetKind::Model) != KindAccentRgb(AssetKind::Sprite));
    CHECK(KindAccentRgb(AssetKind::Data) == 0);   // still the grab-gray fallback

    const auto model = ParseGuid("7e5e0008-0001-4001-8001-000000000001");
    const auto mesh  = ParseGuid("7e5e0008-0001-4001-8001-000000000002");

    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;
    entries[model] = MakeEntry(model, "prop", AssetKind::Model);
    entries[mesh]  = MakeEntry(mesh, "prop-mesh", AssetKind::Mesh);

    AssetReferenceIndex index;
    index.Update(model, true, Refs({}));
    index.Update(mesh, true, Refs({ { model, Arcane::AssetRefKind::DerivesFrom } }));

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;

    AssetGraphViewModel vm;
    vm.Build(in);

    const GraphNode* n = FindReal(vm.nodes, model);
    REQUIRE(n != nullptr);
    CHECK(n->kind == AssetKind::Model);
}

TEST_CASE("ParseGraphFocusQuery: everything, @kind, and substring", "[editor]")
{
    auto q = ParseGraphFocusQuery("");
    CHECK(q.mode == GraphFocusQuery::Mode::Everything);
    q = ParseGraphFocusQuery("  everything  ");
    CHECK(q.mode == GraphFocusQuery::Mode::Everything);
    q = ParseGraphFocusQuery("focus: @source");
    CHECK(q.mode == GraphFocusQuery::Mode::Kind);
    CHECK(q.kind == AssetKind::Source);
    q = ParseGraphFocusQuery("@mesh");
    CHECK(q.mode == GraphFocusQuery::Mode::Kind);
    CHECK(q.kind == AssetKind::Mesh);
    q = ParseGraphFocusQuery("@");
    CHECK(q.mode == GraphFocusQuery::Mode::KindPrefix);
    CHECK(q.text == "");
    q = ParseGraphFocusQuery("@s");
    CHECK(q.mode == GraphFocusQuery::Mode::KindPrefix);
    CHECK(q.text == "s");
    q = ParseGraphFocusQuery("Player");
    CHECK(q.mode == GraphFocusQuery::Mode::Text);
    CHECK(q.text == "player");
    CHECK(CompleteGraphFocusKindPrefix("s") == "source");
    CHECK(CompleteGraphFocusKindPrefix("sp") == "sprite");
    CHECK(CompleteGraphFocusKindPrefix("e") == "everything");
    q = ParseGraphFocusQuery("@everything");
    CHECK(q.mode == GraphFocusQuery::Mode::Everything);
    CHECK_FALSE(CompleteGraphFocusKindPrefix("z").has_value());

    AssetPanelEntry src = MakeEntry(GuidN(9, 1), "NetClient", AssetKind::Source);
    src.fileName = "NetClient.cpp";
    CHECK(MatchesGraphFocusQuery(ParseGraphFocusQuery("@source"), src));
    CHECK(MatchesGraphFocusQuery(ParseGraphFocusQuery("net"), src));
    CHECK_FALSE(MatchesGraphFocusQuery(ParseGraphFocusQuery("@mesh"), src));
}

TEST_CASE("AssetGraphViewModel everything-mode omits Source unless @source", "[editor]")
{
    const auto tex = GuidN(10, 1);
    const auto src = GuidN(10, 2);
    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;
    entries[tex] = MakeEntry(tex, "albedo", AssetKind::Texture);
    entries[src] = MakeEntry(src, "Main", AssetKind::Source);

    AssetReferenceIndex index;
    index.Update(tex, true, Refs({}));
    index.Update(src, true, Refs({}));

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;

    AssetGraphViewModel vm;
    vm.Build(in);
    CHECK(FindReal(vm.nodes, tex) != nullptr);
    CHECK(FindReal(vm.nodes, src) == nullptr);

    in.kindFilter = AssetKind::Source;
    vm.Build(in);
    CHECK(FindReal(vm.nodes, src) != nullptr);
    CHECK(FindReal(vm.nodes, tex) == nullptr);
}

TEST_CASE("AssetGraphViewModel barycentric uncrosses inverted edges", "[editor]")
{
    // A->Y and B->X cross under name-sort (A,B / X,Y). After ReduceCrossings
    // the two columns share an order so the wires do not invert.
    const auto A = GuidN(11, 1);
    const auto B = GuidN(11, 2);
    const auto X = GuidN(11, 3);
    const auto Y = GuidN(11, 4);
    std::unordered_map<Arcane::Guid, AssetPanelEntry> entries;
    entries[A] = MakeEntry(A, "A", AssetKind::Scene);
    entries[B] = MakeEntry(B, "B", AssetKind::Scene);
    entries[X] = MakeEntry(X, "X", AssetKind::Texture);
    entries[Y] = MakeEntry(Y, "Y", AssetKind::Texture);

    AssetReferenceIndex index;
    index.Update(A, true, Refs({ { Y, Arcane::AssetRefKind::References } }));
    index.Update(B, true, Refs({ { X, Arcane::AssetRefKind::References } }));
    index.Update(X, true, Refs({}));
    index.Update(Y, true, Refs({}));

    GraphBuildInput in;
    in.entries = &entries;
    in.index = &index;

    AssetGraphViewModel vm;
    vm.Build(in);
    const GraphNode* nA = FindReal(vm.nodes, A);
    const GraphNode* nB = FindReal(vm.nodes, B);
    const GraphNode* nX = FindReal(vm.nodes, X);
    const GraphNode* nY = FindReal(vm.nodes, Y);
    REQUIRE(nA); REQUIRE(nB); REQUIRE(nX); REQUIRE(nY);
    CHECK((nA->row < nB->row) == (nY->row < nX->row));
}
