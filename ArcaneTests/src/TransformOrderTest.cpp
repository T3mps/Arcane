// Task 4 (F1): the propagation rework.
//
// TransformPropagationSystem used to walk the scene through
// Astra::Relations::ForEachDescendant TWICE a frame -- once to materialise
// missing WorldTransforms, once to compose them. Each call copies the whole
// traversal cache by value under a shared_mutex (RelationshipGraph.hpp's
// GetDescendantsCached returns by value, and must: the caches live in a
// non-pointer-stable FlatMap). So a steady frame paid two mutex locks, two
// hash lookups and two full copies of every entity in the scene before any
// matrix work.
//
// This file pins the replacement. Four properties, in the order they matter:
//
//   1. STEADY STATE IS TRAVERSAL-FREE -- a frame with no structural change
//      makes ZERO ForEachDescendant calls. Measured, not eyeballed: see
//      DescendantCacheEntries below for the instrument.
//   2. THE ORDER IS TOPOLOGICALLY VALID -- every parent's index is strictly
//      less than its child's, over a deliberately awkward tree.
//   3. THE ORDER IS REBUILT ON STRUCTURE, NEVER ON VALUES -- counted.
//   4. THE ANSWER DOES NOT MOVE -- world matrices are ELEMENT-FOR-ELEMENT
//      identical to what Task 3's recursive composition produces, pinned
//      against a test-local recursive oracle written from the relationship
//      graph rather than from the new flat order.
//
// Plus the subtlety that makes a naive dirty flag wrong: dirtiness is
// INHERITED. Moving a parent leaves every descendant's world matrix stale even
// though their own local transforms did not change.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // ---- the instrument for property 1 -------------------------------------
    // Relations::ForEachDescendant's FIRST act is
    // m_relationsGraph->GetDescendantsCached(root), and on a miss that method
    // takes the exclusive lock and does `m_descendantCaches[root]` -- it
    // CREATES the map entry. So: clear the caches, run the frames under test,
    // and if the descendant-cache count is still zero, ForEachDescendant was
    // not called. There is no way to walk descendants through Relations without
    // leaving that fingerprint.
    std::size_t DescendantCacheEntries(const Astra::Registry& reg)
    {
        return reg.GetRelationshipGraph().GetCacheStats().descendantCacheCount;
    }

    void ClearTraversalCaches(Astra::Registry& reg)
    {
        reg.GetRelationshipGraph().ClearCaches();
    }

    // ---- the reference for property 4 --------------------------------------
    using WorldMap = std::unordered_map<Astra::Entity, glm::mat4>;

    // Task 3's propagation restated as a plain RECURSIVE walk of the
    // relationship graph: the root's world IS its local (no parent), every
    // other node's world is `parentWorld * local`, in that operand order.
    //
    // Written from Registry::GetChildren, deliberately NOT from the flat order
    // Task 4 builds -- an expectation derived from the thing under test proves
    // only that the implementation agrees with itself.
    //
    // Restriction, stated rather than hidden: this reproduces Task 3 exactly
    // for a FULLY SPATIAL subtree (every node carries a Transform), which is
    // what the cases below build. Task 3's handling of a Transform-less node in
    // the middle of a chain (its stale WorldTransform, if any, becomes the
    // child's parent matrix) is a separate contract, pinned by
    // TransformPropagationTest.cpp, not restated here.
    void RecurseWorld(Astra::Registry& reg, Astra::Entity e,
                      const glm::mat4* parentWorld, WorldMap& out)
    {
        glm::mat4 mine{1.0f};
        const glm::mat4* pass = parentWorld;
        if (const Arcane::Transform* local = reg.GetComponent<Arcane::Transform>(e))
        {
            mine = parentWorld ? (*parentWorld) * local->ToMatrix() : local->ToMatrix();
            out[e] = mine;
            pass = &mine;   // stack-stable for the whole subtree recursion
        }
        for (Astra::Entity c : reg.GetChildren(e))
            RecurseWorld(reg, c, pass, out);
    }

    WorldMap ReferenceWorlds(Astra::Registry& reg, Astra::Entity root)
    {
        WorldMap out;
        RecurseWorld(reg, root, nullptr, out);
        return out;
    }

    // EXACT, element by element. Not Approx: "identical to the recursive
    // implementation" is the requirement, and both sides run the same
    // `parent * local` product under /fp:strict, so the bits must agree.
    // `==` rather than memcmp so a legitimate +0.0f / -0.0f pair compares equal.
    bool ExactlyEqual(const glm::mat4& a, const glm::mat4& b)
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (a[c][r] != b[c][r])
                    return false;
        return true;
    }

    std::string Describe(const glm::mat4& m)
    {
        std::string s;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                s += std::to_string(m[c][r]) + (r == 3 ? " | " : ", ");
        return s;
    }

    // Compare every entity the reference computed against what the system left
    // in the registry. Reports the first divergence with both matrices.
    void CheckMatchesReference(Astra::Registry& reg, const WorldMap& reference)
    {
        for (const auto& [e, expected] : reference)
        {
            const Arcane::WorldTransform* w = reg.GetComponent<Arcane::WorldTransform>(e);
            REQUIRE(w != nullptr);
            INFO("entity " << e.GetID()
                 << "\n  actual   " << Describe(w->matrix)
                 << "\n  expected " << Describe(expected));
            CHECK(ExactlyEqual(w->matrix, expected));
        }
    }

    // ---- scene fixtures ----------------------------------------------------
    Astra::Entity Spatial(Astra::Registry& reg, glm::vec3 pos,
                          glm::quat rot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                          glm::vec3 scale = glm::vec3(1.0f))
    {
        Astra::Entity e = reg.CreateEntity();
        Arcane::Transform t;
        t.position = pos;
        t.rotation = rot;
        t.scale    = scale;
        reg.AddComponent<Arcane::Transform>(e, t);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
        return e;
    }

    glm::quat AxisAngle(glm::vec3 axis, float radians)
    {
        return glm::angleAxis(radians, glm::normalize(axis));
    }

    // A deliberately awkward subtree under `root`:
    //   * three sub-roots directly under the scene root (the "multiple roots"
    //     a scene actually has -- one SceneRoot, several top-level nodes),
    //   * a DEEP chain (6 links) under the first,
    //   * a WIDE fan (8 siblings) under the second,
    //   * entities WIRED OUT OF ORDER: the deep chain is parented deepest-first
    //     and two nodes are created after their eventual parent's other
    //     subtrees already exist,
    //   * non-trivial rotation and non-uniform scale at every level, so a
    //     wrong composition order cannot cancel out.
    struct AwkwardScene
    {
        Astra::Entity root{};
        std::vector<Astra::Entity> all;      // root + every descendant
        Astra::Entity deepLeaf{};
        Astra::Entity fanParent{};
        Astra::Entity lateChild{};
        Astra::Entity detachedRoot{};        // NOT under the scene root
    };

    AwkwardScene BuildAwkwardScene(Astra::Registry& reg)
    {
        AwkwardScene s;
        s.root = Spatial(reg, {1.0f, 2.0f, 3.0f}, AxisAngle({1.0f, 1.0f, 0.0f}, 0.7f),
                         {2.0f, 1.0f, 0.5f});

        // --- the deep chain, wired DEEPEST FIRST ---
        std::vector<Astra::Entity> chain;
        for (int i = 0; i < 6; ++i)
        {
            chain.push_back(Spatial(reg,
                {0.25f * (float)i, 7.0f - (float)i, -3.0f + 0.5f * (float)i},
                AxisAngle({0.0f, 1.0f, (float)i + 1.0f}, -0.9f + 0.2f * (float)i),
                {1.0f, 1.0f + 0.1f * (float)i, 2.0f}));
        }
        for (int i = (int)chain.size() - 1; i > 0; --i)
            reg.SetParent(chain[(std::size_t)i], chain[(std::size_t)i - 1]);
        reg.SetParent(chain[0], s.root);         // sub-root attached LAST
        s.deepLeaf = chain.back();

        // --- the wide fan ---
        s.fanParent = Spatial(reg, {-2.0f, 0.5f, 1.0f}, AxisAngle({1.0f, 0.0f, 0.0f}, 0.3f),
                              {0.5f, 0.5f, 0.5f});
        reg.SetParent(s.fanParent, s.root);
        std::vector<Astra::Entity> fan;
        for (int i = 0; i < 8; ++i)
        {
            Astra::Entity f = Spatial(reg,
                {(float)i, -(float)i, 0.5f * (float)i},
                AxisAngle({(float)i + 1.0f, 2.0f, 1.0f}, 0.11f * (float)i),
                {1.0f + 0.05f * (float)i, 1.0f, 1.0f});
            reg.SetParent(f, s.fanParent);
            fan.push_back(f);
        }

        // --- a third sub-root, plus a node created LATE and parented into the
        //     middle of an already-built subtree ---
        Astra::Entity third = Spatial(reg, {5.0f, -5.0f, 5.0f}, AxisAngle({0.0f, 0.0f, 1.0f}, 1.1f),
                                      {1.0f, 3.0f, 1.0f});
        reg.SetParent(third, s.root);
        s.lateChild = Spatial(reg, {0.75f, 0.25f, -1.5f}, AxisAngle({1.0f, 2.0f, 3.0f}, 0.42f),
                              {2.0f, 0.5f, 1.5f});
        reg.SetParent(s.lateChild, chain[2]);    // into the MIDDLE of the deep chain

        // --- a second, disconnected root: must not appear in the order at all ---
        s.detachedRoot = Spatial(reg, {100.0f, 100.0f, 100.0f});
        Astra::Entity detachedChild = Spatial(reg, {1.0f, 1.0f, 1.0f});
        reg.SetParent(detachedChild, s.detachedRoot);

        s.all.push_back(s.root);
        for (Astra::Entity e : chain) s.all.push_back(e);
        s.all.push_back(s.fanParent);
        for (Astra::Entity e : fan) s.all.push_back(e);
        s.all.push_back(third);
        s.all.push_back(s.lateChild);

        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{s.root});
        return s;
    }
}

namespace
{
    const Arcane::TransformOrder& OrderOf(Astra::Registry& reg)
    {
        const Arcane::TransformOrder* c = reg.GetResource<Arcane::TransformOrder>();
        REQUIRE(c != nullptr);
        return *c;
    }

    std::uint32_t RebuildCount(Astra::Registry& reg)
    {
        return OrderOf(reg).rebuilds;
    }
}

// ============================================================================
// Property 2: the order is topologically valid.
// ============================================================================
TEST_CASE("the transform order is topologically valid over an awkward tree",
          "[scene][transform-order]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    AwkwardScene s = BuildAwkwardScene(reg);

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);

    const Arcane::TransformOrder& order = OrderOf(reg);
    REQUIRE(order.order.size() == order.parentIndex.size());

    // THE property: a parent is always composed before its child.
    for (std::size_t i = 0; i < order.order.size(); ++i)
    {
        const std::uint32_t p = order.parentIndex[i];
        INFO("index " << i << " parentIndex " << p);
        if (p == Arcane::TransformOrder::kNoParent)
            CHECK(i == 0);                                   // only the root is parentless
        else
            CHECK((std::size_t)p < i);                       // strictly less
    }

    // The order is the scene root's subtree: exactly, no more, no less.
    CHECK(order.order[0] == s.root);
    std::unordered_set<Astra::Entity> inOrder(order.order.begin(), order.order.end());
    CHECK(inOrder.size() == order.order.size());             // no duplicates
    CHECK(order.order.size() == s.all.size());
    for (Astra::Entity e : s.all)
    {
        INFO("entity " << e.GetID() << " missing from the order");
        CHECK(inOrder.count(e) == 1);
    }
    // A second, disconnected root is NOT the scene -- it must not be walked.
    CHECK(inOrder.count(s.detachedRoot) == 0);

    // And parentIndex actually names the real parent, not just some earlier slot.
    for (std::size_t i = 1; i < order.order.size(); ++i)
        CHECK(reg.GetParent(order.order[i]) == order.order[order.parentIndex[i]]);
}

// ============================================================================
// Property 3: rebuilt on structure, never on values. This is the property the
// whole task exists for.
// ============================================================================
TEST_CASE("the transform order is rebuilt on structure and not on values",
          "[scene][transform-order]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    AwkwardScene s = BuildAwkwardScene(reg);

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);
    const std::uint32_t base = RebuildCount(reg);
    REQUIRE(base >= 1);                                      // the first frame has to build one

    SECTION("idle frames rebuild nothing")
    {
        for (int i = 0; i < 16; ++i)
            propagate(reg);
        CHECK(RebuildCount(reg) == base);
    }

    SECTION("component VALUE changes rebuild nothing")
    {
        for (int i = 0; i < 4; ++i)
        {
            reg.GetComponent<Arcane::Transform>(s.fanParent)->position += glm::vec3(1.0f, 0.0f, 0.0f);
            reg.GetComponent<Arcane::Transform>(s.deepLeaf)->scale     *= 1.01f;
            propagate(reg);
        }
        // Adding an unrelated component is a value/archetype change, not a
        // hierarchy change -- it must not cost a rebuild either.
        reg.AddComponent<Arcane::SpriteRenderer>(s.lateChild, Arcane::SpriteRenderer{});
        propagate(reg);
        CHECK(RebuildCount(reg) == base);
    }

    SECTION("a reparent rebuilds exactly once")
    {
        reg.SetParent(s.lateChild, s.fanParent);
        propagate(reg);
        CHECK(RebuildCount(reg) == base + 1);
        propagate(reg);
        propagate(reg);
        CHECK(RebuildCount(reg) == base + 1);                // and not again
    }

    SECTION("a detach rebuilds")
    {
        reg.RemoveParent(s.lateChild);
        propagate(reg);
        CHECK(RebuildCount(reg) == base + 1);
    }

    SECTION("destroying an entity in the tree rebuilds")
    {
        reg.DestroyEntity(s.deepLeaf);
        propagate(reg);
        CHECK(RebuildCount(reg) == base + 1);
    }

    SECTION("changing the scene root rebuilds")
    {
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{s.detachedRoot});
        propagate(reg);
        CHECK(RebuildCount(reg) == base + 1);
        propagate(reg);
        CHECK(RebuildCount(reg) == base + 1);
    }
}

// ============================================================================
// Property 1: a steady-state frame makes ZERO ForEachDescendant calls.
// ============================================================================
TEST_CASE("a steady-state transform frame walks no descendants", "[scene][transform-order]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    AwkwardScene s = BuildAwkwardScene(reg);

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);                       // frame 1: builds whatever it needs

    // Everything Astra cached up to here is fair game -- wipe it, so the only
    // thing that can put an entry back is a descendant walk from now on.
    ClearTraversalCaches(reg);
    REQUIRE(DescendantCacheEntries(reg) == 0);

    for (int frame = 0; frame < 8; ++frame)
        propagate(reg);
    CHECK(DescendantCacheEntries(reg) == 0);

    // A pure VALUE change is still steady state as far as structure goes.
    reg.GetComponent<Arcane::Transform>(s.fanParent)->position = glm::vec3(-9.0f, 4.0f, 2.0f);
    propagate(reg);
    CHECK(DescendantCacheEntries(reg) == 0);
}

// ============================================================================
// Property 4: the answer does not move.
// ============================================================================
TEST_CASE("propagated world matrices are identical to the recursive reference",
          "[scene][transform-order]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    AwkwardScene s = BuildAwkwardScene(reg);

    Arcane::TransformPropagationSystem propagate;

    SECTION("first pass")
    {
        const WorldMap reference = ReferenceWorlds(reg, s.root);
        propagate(reg);
        CheckMatchesReference(reg, reference);
    }

    SECTION("after local values change")
    {
        propagate(reg);
        reg.GetComponent<Arcane::Transform>(s.fanParent)->position = glm::vec3(11.0f, -3.0f, 0.25f);
        reg.GetComponent<Arcane::Transform>(s.fanParent)->scale    = glm::vec3(1.5f, 2.5f, 0.75f);
        reg.GetComponent<Arcane::Transform>(s.deepLeaf)->rotation  = AxisAngle({1.0f, 1.0f, 1.0f}, 2.2f);

        const WorldMap reference = ReferenceWorlds(reg, s.root);
        propagate(reg);
        CheckMatchesReference(reg, reference);
    }

    SECTION("after a reparent")
    {
        propagate(reg);
        reg.SetParent(s.lateChild, s.fanParent);   // out of the chain, into the fan

        const WorldMap reference = ReferenceWorlds(reg, s.root);
        propagate(reg);
        CheckMatchesReference(reg, reference);
    }

    SECTION("after the scene root itself changes")
    {
        propagate(reg);
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{s.detachedRoot});

        const WorldMap reference = ReferenceWorlds(reg, s.detachedRoot);
        propagate(reg);
        CheckMatchesReference(reg, reference);
    }
}

// ============================================================================
// The subtlety: dirtiness is INHERITED.
// ============================================================================
TEST_CASE("moving a parent updates its grandchild's world matrix", "[scene][transform-order]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);

    Astra::Entity root  = Spatial(reg, {0.0f, 0.0f, 0.0f});
    Astra::Entity mid   = Spatial(reg, {1.0f, 0.0f, 0.0f});
    Astra::Entity child = Spatial(reg, {2.0f, 0.0f, 0.0f});
    Astra::Entity grand = Spatial(reg, {4.0f, 0.0f, 0.0f});
    reg.SetParent(grand, child);
    reg.SetParent(child, mid);
    reg.SetParent(mid, root);
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);
    REQUIRE(reg.GetComponent<Arcane::WorldTransform>(grand)->matrix[3].x == Catch::Approx(7.0f));

    // Move ONLY `mid`. Neither child's nor grandchild's LOCAL transform
    // changed, so a dirty scheme that only re-composes entities whose own
    // local moved leaves both of them at their old world position -- the
    // exact bug this case exists to catch.
    reg.GetComponent<Arcane::Transform>(mid)->position = glm::vec3(101.0f, 0.0f, 0.0f);
    propagate(reg);

    CHECK(reg.GetComponent<Arcane::WorldTransform>(mid)->matrix[3].x   == Catch::Approx(101.0f));
    CHECK(reg.GetComponent<Arcane::WorldTransform>(child)->matrix[3].x == Catch::Approx(103.0f));
    CHECK(reg.GetComponent<Arcane::WorldTransform>(grand)->matrix[3].x == Catch::Approx(107.0f));

    // ...and a ROTATION on the root, which changes every descendant's world
    // matrix without touching a single descendant translation.
    reg.GetComponent<Arcane::Transform>(root)->rotation = Arcane::RotationAboutZ(1.57079632679f);
    const WorldMap reference = ReferenceWorlds(reg, root);
    propagate(reg);
    CheckMatchesReference(reg, reference);
}

// ============================================================================
// A clean subtree is SKIPPED -- not merely recomputed to the same value.
// ============================================================================
TEST_CASE("a clean leaf is not rewritten by the pass", "[scene][transform-order]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);

    Astra::Entity root = Spatial(reg, {0.0f, 0.0f, 0.0f});
    Astra::Entity a    = Spatial(reg, {1.0f, 0.0f, 0.0f});
    Astra::Entity b    = Spatial(reg, {0.0f, 1.0f, 0.0f});
    reg.SetParent(a, root);
    reg.SetParent(b, root);
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);

    // Stamp a sentinel over a CLEAN leaf's derived matrix. Nothing in the scene
    // changes afterward, so a pass that skips clean work leaves the sentinel
    // standing; a pass that re-composes every entity every frame stomps it.
    const glm::mat4 sentinel = glm::mat4(7.0f);
    reg.GetComponent<Arcane::WorldTransform>(b)->matrix = sentinel;
    propagate(reg);
    CHECK(ExactlyEqual(reg.GetComponent<Arcane::WorldTransform>(b)->matrix, sentinel));

    // ...and the skip is not a leak: touching the leaf's local brings it back.
    reg.GetComponent<Arcane::Transform>(b)->position = glm::vec3(0.0f, 5.0f, 0.0f);
    propagate(reg);
    CHECK(reg.GetComponent<Arcane::WorldTransform>(b)->matrix[3].y == Catch::Approx(5.0f));
}

// ============================================================================
// Structural edits the flat order has to survive.
// ============================================================================
TEST_CASE("an entity inserted mid-hierarchy is picked up", "[scene][transform-order]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);

    Astra::Entity root  = Spatial(reg, {10.0f, 0.0f, 0.0f});
    Astra::Entity mid   = Spatial(reg, {1.0f, 0.0f, 0.0f});
    Astra::Entity leaf  = Spatial(reg, {2.0f, 0.0f, 0.0f});
    reg.SetParent(leaf, mid);
    reg.SetParent(mid, root);
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);

    // Splice a new node BETWEEN mid and leaf.
    Astra::Entity spliced = Spatial(reg, {100.0f, 0.0f, 0.0f});
    reg.SetParent(spliced, mid);
    reg.SetParent(leaf, spliced);

    const WorldMap reference = ReferenceWorlds(reg, root);
    propagate(reg);
    CheckMatchesReference(reg, reference);
    CHECK(reg.GetComponent<Arcane::WorldTransform>(leaf)->matrix[3].x == Catch::Approx(113.0f));
}

TEST_CASE("a WorldTransform is materialised on demand", "[scene][transform-order]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);

    Astra::Entity root = Spatial(reg, {5.0f, 0.0f, 0.0f});
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    Arcane::TransformPropagationSystem propagate;
    propagate(reg);

    SECTION("an entity attached with a Transform but no WorldTransform")
    {
        Astra::Entity fresh = reg.CreateEntity();
        Arcane::Transform t;
        t.position = glm::vec3(2.0f, 3.0f, 0.0f);
        reg.AddComponent<Arcane::Transform>(fresh, t);
        reg.SetParent(fresh, root);
        REQUIRE(reg.GetComponent<Arcane::WorldTransform>(fresh) == nullptr);

        propagate(reg);
        Arcane::WorldTransform* w = reg.GetComponent<Arcane::WorldTransform>(fresh);
        REQUIRE(w != nullptr);
        CHECK(w->matrix[3].x == Catch::Approx(7.0f));
        CHECK(w->matrix[3].y == Catch::Approx(3.0f));
    }

    SECTION("an entity already in the hierarchy that GAINS a Transform later")
    {
        // The Inspector's Add Component can put a Transform on an entity that
        // has been sitting in the scene without one (Transform is deliberately
        // NOT structure-locked -- see ComponentCatalog.hpp). No structural
        // change accompanies it, so a pass that only materialises on rebuild
        // would leave this entity without a WorldTransform forever.
        Astra::Entity bare = reg.CreateEntity();
        reg.SetParent(bare, root);
        propagate(reg);
        REQUIRE(reg.GetComponent<Arcane::WorldTransform>(bare) == nullptr);

        Arcane::Transform t;
        t.position = glm::vec3(0.0f, 4.0f, 0.0f);
        reg.AddComponent<Arcane::Transform>(bare, t);
        propagate(reg);

        Arcane::WorldTransform* w = reg.GetComponent<Arcane::WorldTransform>(bare);
        REQUIRE(w != nullptr);
        CHECK(w->matrix[3].x == Catch::Approx(5.0f));
        CHECK(w->matrix[3].y == Catch::Approx(4.0f));
    }
}
