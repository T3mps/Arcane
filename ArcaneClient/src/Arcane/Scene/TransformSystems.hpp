#pragma once

// TransformPropagationSystem: world-matrix propagation over the scene root's
// subtree, as a LINEAR pass over a flat topologically sorted array.
//
// Task 4 (F1) replaced the walk. It used to call
// Astra::Relations::ForEachDescendant TWICE per frame -- once to materialise
// missing WorldTransforms, once to compose them. That call copies the entire
// traversal cache BY VALUE under a shared_mutex, and it must:
// RelationshipGraph::GetDescendantsCached returns by value because the caches
// live in a non-pointer-stable FlatMap, so a reference escaping the lock can
// dangle after a rehash. Correct, and not something to argue with -- but it
// meant a steady frame paid two mutex locks, two hash lookups and two heap
// allocations plus full copies of every entity in the scene, then four random
// component/relation lookups per entity, before any matrix work.
//
// The fix is to decouple STRUCTURE from VALUES:
//
//   * TransformOrder (below) owns a flat `order` array in topological (BFS)
//     order plus a `parentIndex` array indexing into it. It is rebuilt ONLY
//     when Astra's RelationshipGraph::StructureVersion() moves (or the scene
//     root itself changes) -- the counter Task 1 added for exactly this, which
//     bumps on attach/detach/reparent/destroy/clear and on nothing else.
//   * The per-frame pass is then
//     `world[i] = parentIndex[i] == kNoParent ? local[i] : world[parentIndex[i]] * local[i]`.
//     Parents precede children, so the parent's world matrix is already final;
//     the parent lookup is an array index, not a hash lookup.
//   * DIRTY FLAGS skip untouched subtrees. Dirtiness is INHERITED: a moved
//     parent leaves every descendant's WORLD matrix stale even though their
//     LOCAL transforms did not change. Because `order` is topological, one
//     forward pass suffices -- row i ORs its parent's already-decided
//     dirtiness into its own (see Compose).
//
// A steady-state frame now calls ForEachDescendant ZERO times; so does a
// rebuild, which walks RelationshipGraph::GetChildren directly (it needs the
// parent index, which the descendant cache does not carry). The two mutex
// locks, two cache copies and two heap allocations per frame are gone
// outright, and the per-entity lookups go four to TWO (its own Transform and
// WorldTransform) -- the parent's world matrix and the parent's identity both
// come from arrays now. Pinned in ArcaneTests/src/TransformOrderTest.cpp.
//
// The ordering and dirty policy live HERE, in Arcane. Astra supplied the
// version and nothing more; "spatial" is not a concept the ECS needs.

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Container/FlatSet.hpp>
#include <Astra/Entity/Entity.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Arcane
{
    // The propagation cache, held as a Registry RESOURCE rather than as system
    // state: the editor calls the system as a temporary
    // (`TransformPropagationSystem{}(reg)` in EditorAppFrame/EditorAppScene)
    // while the runtime scheduler owns a long-lived instance, so per-instance
    // state would persist for one host and not the other. The registry is the
    // one thing both share, and it is also the right lifetime -- a swapped
    // registry (RestoreRegistry, ResetRegistry, scene load) drops the cache
    // with the world it described.
    //
    // Deliberately public: the properties this task exists for (topological
    // validity, and "rebuilt on structure, never on values") are only
    // assertable if the order and the rebuild counter can be read.
    struct TransformOrder
    {
        // parentIndex sentinel: this row has no parent WITHIN the order. Only
        // row 0 (the scene root) ever carries it.
        static constexpr std::uint32_t kNoParent = 0xFFFFFFFFu;

        // ---- structure: rebuilt only when StructureVersion()/root moves ----
        std::vector<Astra::Entity> order;        // BFS from the scene root; order[0] IS the root
        std::vector<std::uint32_t> parentIndex;  // index INTO order, always strictly < own index

        // ---- per-row value state, parallel to `order` ----
        // shadow/shadowValid are the change detector. Astra has no component
        // change tracking and Task 4 is explicitly not allowed to add one, so
        // "did this local pose move?" is answered by comparing against the pose
        // the row was last composed from: ten float compares against ~128 flops
        // of mat4_cast + mat4 product, which is the trade that makes skipping
        // worth doing at all.
        std::vector<Transform>    shadow;
        std::vector<std::uint8_t> shadowValid;   // 0 => row i must recompose
        std::vector<std::uint8_t> dirty;         // decided this pass; read by children

        // Row i's world matrix -- and THE SOURCE its children compose against,
        // which "mirror" would understate. A child multiplies by
        // `world[parentIndex[i]]`, never by the parent's WorldTransform
        // component, so:
        //
        //   Compose (below) is the SOLE writer of WorldTransform::matrix in
        //   engine source. Anything else that writes one is invisible to that
        //   entity's CHILDREN -- permanently, until the next rebuild reseeds
        //   this array from the components.
        //
        // Not a latent hazard today: WorldTransform is IsStructureLocked AND
        // IsHiddenInInspector (ComponentCatalog.cpp), so the editor can neither
        // add, remove nor edit one, and every binary-restore path lands in a
        // FRESH Registry, which drops this cache with it. But it is a CONTRACT
        // a future writer must either honour or route through here.
        // TransformOrderTest.cpp's "a clean leaf is not rewritten by the pass"
        // enshrines exactly this behaviour -- deliberately on a LEAF, so what
        // it pins is the skip and not the divergence.
        std::vector<glm::mat4>    world;

        // Scratch, kept here so a steady frame allocates nothing at all.
        std::vector<Astra::Entity>    needsWorld;
        Astra::FlatSet<Astra::Entity> visited;   // Rebuild's cycle guard

        // ---- invalidation keys ----
        Astra::Entity root{};
        // 0 == never built. This works ONLY because a fresh RelationshipGraph
        // starts at 1 (RelationshipGraph.hpp:848, and its move ctor resets the
        // moved-from counter to 1 rather than 0 at :91) -- the same reason
        // Astra's own TraversalCache::IsValid treats 0 as "no valid version".
        // Had Astra started at 0, a single-entity scene that never calls
        // SetParent would match on the first frame, skip the rebuild, and
        // propagate NOTHING out of an empty order -- silently, and no fixture
        // that calls SetParent could catch it. If that constant ever moves,
        // this sentinel has to become an explicit `built` flag.
        std::uint32_t structureVersion = 0;

        // How many times the order has been rebuilt. Instrumentation, and the
        // only way a test can state the headline property as an assertion
        // rather than as a hope.
        std::uint32_t rebuilds = 0;

        // Transient derived state; Registry::Save excludes resources entirely.
        // The no-op Serialize satisfies Astra's HasSerializeMethod so the
        // vector members never reach the trivially-copyable path (same reason
        // PhysicsInterpBuffer carries one).
        template<typename Archive> void Serialize(Archive& /*ar*/) {}
    };

    struct TransformPropagationSystem
        : Astra::SystemTraits<Astra::Reads<Transform>, Astra::Writes<WorldTransform>>
    {
        void operator()(Astra::Registry& reg)
        {
            const SceneRoot* sceneRoot = reg.GetResource<SceneRoot>();
            if (!sceneRoot) return;
            const Astra::Entity root = sceneRoot->entity;

            TransformOrder* cache = reg.GetResource<TransformOrder>();
            if (!cache)
                cache = reg.EmplaceResource<TransformOrder>();
            if (!cache) return;

            // The whole invalidation policy, in two comparisons. StructureVersion
            // covers attach/detach/reparent/destroy/clear; the root comparison
            // covers the one structural change Astra cannot see, because the
            // scene root is Arcane's idea, not the graph's.
            const std::uint32_t version = reg.StructureVersion();
            if (cache->structureVersion != version || cache->root != root)
                Rebuild(reg, root, version, *cache);

            // Compose reports the rows that want a WorldTransform rather than
            // adding one inline: AddComponent moves the entity to a different
            // archetype, invalidating every component pointer the pass is
            // holding for OTHER entities. (The same hazard the retired
            // two-walk version deferred for, and the one Astra's traversals
            // explicitly do not protect against.)
            //
            // The second pass then composes them. It is bounded at two by
            // construction -- the adds cannot fail into a third -- and it only
            // runs on a frame that actually materialised something, where its
            // rows are almost all clean skips anyway.
            if (Compose(reg, *cache))
            {
                for (Astra::Entity e : cache->needsWorld)
                    reg.AddComponent<WorldTransform>(e, WorldTransform{});
                Compose(reg, *cache);
            }
        }

    private:
        // BFS from the root over the live relationship graph. BFS is what makes
        // the result topological: a row's children are appended after it, so
        // every parent index is strictly less than its child's.
        //
        // Everything recomposes on the pass after a rebuild (shadowValid is
        // cleared), which is both correct and cheap to reason about: a
        // structural change moves whole subtrees anyway, and rebuilds are rare
        // by construction.
        static void Rebuild(Astra::Registry& reg, Astra::Entity root,
                            std::uint32_t version, TransformOrder& c)
        {
            const Astra::RelationshipGraph& graph = reg.GetRelationshipGraph();

            c.order.clear();
            c.parentIndex.clear();
            c.order.push_back(root);
            c.parentIndex.push_back(TransformOrder::kNoParent);

            // Cycle guard. SetParent rejects cycles, but
            // RelationshipGraph::Deserialize writes the parent/child maps
            // straight from file bytes, and an unbounded BFS over a cyclic map
            // never terminates. Astra's own BuildDescendantCache carries the
            // same visited set for the same reason.
            //
            // Lives on the cache, not on the stack, for the same reason
            // needsWorld does: rebuilds are rare but they are not exceptional
            // (every reparent is one), and a set that keeps its buckets between
            // them costs one vector's worth of memory to make them allocation-
            // free too.
            Astra::FlatSet<Astra::Entity>& visited = c.visited;
            visited.Clear();
            visited.Reserve(64);
            visited.Insert(root);

            for (std::size_t i = 0; i < c.order.size(); ++i)
            {
                const Astra::Entity parent = c.order[i];
                for (Astra::Entity child : graph.GetChildren(parent))
                {
                    if (!visited.Insert(child).second)
                        continue;
                    c.order.push_back(child);
                    c.parentIndex.push_back(static_cast<std::uint32_t>(i));
                }
            }

            const std::size_t n = c.order.size();
            c.shadow.assign(n, Transform{});
            c.shadowValid.assign(n, 0);
            c.dirty.assign(n, 0);
            c.world.resize(n);
            c.needsWorld.clear();

            // Seed the mirror from whatever each entity's WorldTransform holds
            // right now. A row this pass never composes -- a node with no
            // Transform -- must hand its children exactly the matrix the old
            // walk would have read off the component, including one a binary
            // load restored, not a default identity.
            for (std::size_t i = 0; i < n; ++i)
            {
                const WorldTransform* w = reg.GetComponent<WorldTransform>(c.order[i]);
                c.world[i] = w ? w->matrix : glm::mat4(1.0f);
            }

            c.root = root;
            c.structureVersion = version;
            ++c.rebuilds;
        }

        // The linear pass. Returns true when at least one row wants a
        // WorldTransform materialised, which the caller does between the two
        // passes.
        //
        // WorldTransform is DERIVED, never authored: an entity that reaches this
        // subtree with a Transform but no WorldTransform (a node Edit::CreateEntity
        // just created, a SceneRoot minted by SceneAsset::CreateEmpty, one loaded
        // from a pre-fix .arcscene, or one the Inspector's Add Component just put a
        // Transform on) must get one here, or it can never satisfy
        // RenderSubmissionSystem's view no matter what components get added to it
        // afterward.
        //
        // That check cannot live in Rebuild: gaining a Transform is a COMPONENT
        // change, so it moves no structure version and would be missed forever.
        // And it deliberately does NOT skip rows this pass considers clean.
        // An earlier draft keyed it on shadowValid -- "a row that has composed
        // must already have a WorldTransform, because nothing removes one" --
        // which bought a lookup per clean row by resting the guarantee on a
        // CONVENTION (the editor's structure-lock) that a plugin does not share.
        // An entity whose WorldTransform was removed behind our back and whose
        // local then never moved again would have gone un-healed forever, where
        // the retired walk re-added it the very next frame. The presence check
        // is now unconditional, so the guarantee holds by construction: every
        // spatial row in the subtree leaves this pass with a WorldTransform.
        static bool Compose(Astra::Registry& reg, TransformOrder& c)
        {
            c.needsWorld.clear();
            const std::size_t n = c.order.size();
            for (std::size_t i = 0; i < n; ++i)
            {
                const Astra::Entity e = c.order[i];
                const std::uint32_t p = c.parentIndex[i];
                Transform* local = reg.GetComponent<Transform>(e);
                if (!local)
                {
                    // Non-spatial node (never had a Transform, or the Inspector
                    // removed one -- Transform is deliberately not structure-
                    // locked). The old walk skipped it and left its
                    // WorldTransform, if any, at its last value; the mirror row
                    // holds that same value, so its children compose against
                    // exactly what they used to read off the component. It
                    // contributes no dirtiness, because its world did not move.
                    c.shadowValid[i] = 0;
                    c.dirty[i] = 0;
                    continue;
                }

                WorldTransform* world = reg.GetComponent<WorldTransform>(e);
                if (!world)
                {
                    // Report and skip. The row recomposes on the second pass
                    // (shadowValid is cleared, so `moved` is true there), and
                    // its descendants pick the change up from `dirty` on that
                    // same pass because they are ordered after it.
                    c.needsWorld.push_back(e);
                    c.shadowValid[i] = 0;
                    c.dirty[i] = 0;
                    continue;
                }

                // THE SUBTLETY. `inherited` is what makes a moved parent drag its
                // whole subtree with it; a scheme that only checked `moved` would
                // leave every descendant of a moved node at a stale world pose.
                // One forward pass is enough precisely because `order` is
                // topological -- row p was decided before row i is read.
                const bool inherited = (p != TransformOrder::kNoParent) && c.dirty[p] != 0;
                const bool moved     = !c.shadowValid[i] || !SamePose(c.shadow[i], *local);
                if (!inherited && !moved)
                {
                    c.dirty[i] = 0;
                    continue;   // the point of the exercise: no matrix work at all
                }

                const glm::mat4 localMat = local->ToMatrix();
                c.world[i] = (p == TransformOrder::kNoParent) ? localMat
                                                              : c.world[p] * localMat;
                world->matrix    = c.world[i];
                c.shadow[i]      = *local;
                c.shadowValid[i] = 1;
                c.dirty[i]       = 1;
            }
            return !c.needsWorld.empty();
        }

        // Exact float comparison, on purpose. This is a "did anything change"
        // question, not a "are these close" one: a tolerance would make a slow
        // drift invisible until it crossed the threshold, and any false
        // NEGATIVE here silently freezes an entity's world matrix. Exact
        // compare can only ever be conservative in the safe direction (NaN
        // compares unequal, so a NaN pose recomposes every frame).
        [[nodiscard]] static bool SamePose(const Transform& a, const Transform& b) noexcept
        {
            return a.position.x == b.position.x &&
                   a.position.y == b.position.y &&
                   a.position.z == b.position.z &&
                   a.rotation.w == b.rotation.w &&
                   a.rotation.x == b.rotation.x &&
                   a.rotation.y == b.rotation.y &&
                   a.rotation.z == b.rotation.z &&
                   a.scale.x    == b.scale.x    &&
                   a.scale.y    == b.scale.y    &&
                   a.scale.z    == b.scale.z;
        }
    };
}
