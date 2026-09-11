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
//   * DIRTY FLAGS skip untouched subtrees. `moved` comes from the
//     Changed<Transform> pre-pass (Astra adoption 2026-09-11, spec s6.3): one
//     chunk-version compare per chunk and one tick compare per entity, in
//     place of the retired per-row shadow-copy comparison. Dirtiness is
//     INHERITED: a moved parent leaves every descendant's WORLD matrix stale
//     even though their LOCAL transforms did not change. Because `order` is
//     topological, one forward pass suffices -- row i ORs its parent's
//     already-decided dirtiness into its own (see Compose). When the pre-pass
//     finds nothing and nothing was rebuilt or materialised, operator() skips
//     the linear pass entirely -- the EARLY-OUT.
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

#include <Astra/Container/FlatMap.hpp>
#include <Astra/Container/FlatSet.hpp>
#include <Astra/Core/Tick.hpp>
#include <Astra/Entity/Entity.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace Arcane
{
    // The propagation cache, held as a Registry RESOURCE rather than as system
    // state: the editor's EditModeSchedule and the game module's fixedUpdate
    // scheduler both own a long-lived instance, so per-instance state would
    // persist for one host and not the other. The registry is the one thing
    // both share, and it is also the right lifetime -- a swapped registry
    // (RestoreRegistry, ResetRegistry, scene load) drops the cache with the
    // world it described.
    //
    // Deliberately public: the properties this task exists for (topological
    // validity, and "rebuilt on structure, never on values") are only
    // assertable if the order and the rebuild counter can be read.
    struct TransformOrder
    {
        static constexpr std::uint32_t kNoParent = 0xFFFFFFFFu;

        // ---- structure: rebuilt only when StructureVersion()/root moves ----
        std::vector<Astra::Entity> order;        // BFS from the scene root; order[0] IS the root
        std::vector<std::uint32_t> parentIndex;  // index INTO order, always strictly < own index
        // entity -> row in `order`, filled by Rebuild (which already walks every
        // entity). THE bridge between Astra's entity-keyed Changed<Transform>
        // yield and this cache's row-indexed arrays. An entity absent here is
        // outside the scene root's subtree and is never written by the pass.
        Astra::FlatMap<Astra::Entity, std::uint32_t> rowOf;

        // ---- per-row value state, parallel to `order` ----
        // `moved` is the change detector (Astra adoption 2026-09-11, spec s6.3):
        // set by the Changed<Transform> pre-pass for exactly the rows whose LOCAL
        // pose was written since `lastRun` -- exact per entity because Transform
        // is AstraChangeTracked (Components.hpp) -- and consumed (reset to 0) by
        // Compose. It replaced a per-row shadow copy of the last-composed pose
        // compared ten floats at a time every frame: the registry now answers
        // "did this local move?" with one chunk-version compare per chunk and one
        // tick compare per entity, and a scene in which nothing moved skips the
        // linear pass entirely (the early-out in operator()).
        std::vector<std::uint8_t> moved;         // 1 => row i's local was written since lastRun
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

        // The "since" tick of the last pass: the pre-pass yields Transforms
        // written STRICTLY after it. 0 == never (Astra's tick sentinel), so a
        // fresh cache -- and one a Rebuild just reset -- sees every stamped
        // Transform on its next pass. Lives on the RESOURCE, not the system: the
        // editor's per-frame scheduler and the game module's fixedUpdate
        // scheduler drive this same cache, and a swapped registry
        // (RestoreRegistry, ResetRegistry, scene load) restarts its ticks at 1 --
        // a since-tick stored anywhere else would be stale against it.
        Astra::Tick lastRun = 0;

        // How many times the order has been rebuilt. Instrumentation, and the
        // only way a test can state the headline property as an assertion
        // rather than as a hope.
        std::uint32_t rebuilds = 0;
        // Instrumentation for the adoption's tests: operator() calls that reached
        // this cache, and rows Compose actually recomposed. An early-out leaves
        // `composed` unchanged -- that is the assertion "a static scene does no
        // matrix work" is made of.
        std::uint32_t runs = 0;
        std::uint32_t composed = 0;

        // Transient derived state; Registry::Save excludes resources entirely.
        // The no-op Serialize satisfies Astra's HasSerializeMethod so the
        // vector members never reach the trivially-copyable path (same reason
        // PhysicsInterpBuffer carries one).
        template<typename Archive> void Serialize(Archive& /*ar*/) {}
    };

    struct TransformPropagationSystem
        : Astra::SystemTraits<Astra::Reads<Transform>, Astra::Writes<WorldTransform>>
    {
        // EXCLUSIVE, because of the end-of-pass AdvanceTick below. Astra's tick
        // contract (ArchetypeManager.hpp: CurrentTick "NEVER advanced concurrently
        // with a running system"; SystemExecutor.hpp: systems in one group SHARE
        // the group's tick) forbids a system advancing the counter while any
        // other system of its group may be running or stamping. The scheduler
        // honours this flag by giving the system its own group (SystemScheduler
        // .hpp reads T::RequiresExclusive into the metadata), so the advance
        // happens with nothing else in flight. Today both schedulers that own
        // this system hold it alone, which is why the violation was latent; the
        // flag makes the contract hold by construction, not by roster luck.
        static constexpr bool RequiresExclusive = true;

        // The ONE overload, everywhere: the game module's fixedUpdate scheduler,
        // the editor's Edit-mode scheduler (EditModeSchedule) and the tests all
        // drive this. Time base per pass: `since` is the cache's lastRun; at the
        // end the cache takes CurrentTick() and the tick is ADVANCED -- mirroring
        // the scheduler's post-segment advance -- so a write made after this pass
        // is strictly newer than lastRun and IsNewer(t, t) never hides it (the
        // "a clean leaf is not rewritten" canary in TransformOrderTest.cpp writes
        // between two bare calls). Under a scheduler that advance is one extra
        // tick per pass, taken in an exclusive group (above): ticks are cheap and
        // only ever compared, and no other system can be mid-stamp when it moves.
        void operator()(Astra::Registry& reg)
        {
            const SceneRoot* sceneRoot = reg.GetResource<SceneRoot>();
            if (!sceneRoot) return;
            const Astra::Entity root = sceneRoot->entity;

            TransformOrder* cache = reg.GetResource<TransformOrder>();
            if (!cache)
                cache = reg.EmplaceResource<TransformOrder>();
            if (!cache) return;
            TransformOrder& c = *cache;
            ++c.runs;

            // 1. Structure. StructureVersion covers attach/detach/reparent/destroy/
            // clear; the root comparison covers the one structural change Astra
            // cannot see. A Rebuild resets lastRun to 0 (inside Rebuild): a reparent
            // must recompose everything, and a rebuilt row order invalidates any
            // per-row memory.
            const std::uint32_t version = reg.StructureVersion();
            const bool rebuilt = (c.structureVersion != version || c.root != root);
            if (rebuilt)
                Rebuild(reg, root, version, c);

            // 2. Materialise missing WorldTransforms (the heal contract), 3. mark the
            // rows whose local moved. Both are cheap in steady state: the probe's
            // archetypes are empty, and the pre-pass chunk-rejects untouched chunks.
            bool work = rebuilt;
            work = Materialise(reg, c) || work;
            work = MarkMoved(reg, c) || work;

            // 4. THE EARLY-OUT. Nothing moved and nothing was rebuilt or
            // materialised => nothing inherited => no row can need a matrix.
            if (work)
                Compose(reg, c);

            // 5. Advance, on EVERY path (early-out included, so the pre-pass's
            // chunk reject stays tight instead of re-scanning chunks stamped by
            // unrelated writes until the next full pass).
            c.lastRun = reg.CurrentTick();
            reg.AdvanceTick();
        }

    private:
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
            c.rowOf.Clear();
            c.rowOf.Reserve(n);
            for (std::size_t i = 0; i < n; ++i)
                c.rowOf[c.order[i]] = static_cast<std::uint32_t>(i);
            c.moved.assign(n, 0);
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
                const WorldTransform* w = std::as_const(reg).GetComponent<WorldTransform>(c.order[i]);
                c.world[i] = w ? w->matrix : glm::mat4(1.0f);
            }

            c.root = root;
            c.structureVersion = version;
            // Everything recomposes on the pass after a rebuild: with lastRun at
            // "never", the pre-pass yields every Transform that was ever stamped.
            c.lastRun = 0;
            ++c.rebuilds;
        }

        // WorldTransform is DERIVED, never authored: a subtree row with a
        // Transform but no WorldTransform (Edit::CreateEntity, SceneAsset::
        // CreateEmpty's root, a pre-fix .arcscene, an Inspector Add Component --
        // or one REMOVED behind our back by a plugin) gets one here, or it can
        // never satisfy RenderSubmissionSystem's view. Probed through the
        // registry rather than per row so the early-out below cannot skip it:
        // gaining a Transform marks the entity (the pre-pass sees it), but LOSING
        // a WorldTransform marks nothing on Transform, and the archetypes this
        // view matches are empty in steady state, so the probe is one chunk-list
        // check per frame. Adds happen AFTER the walk (a structural change during
        // a ForEach is refused) and BEFORE any component pointer is held.
        static bool Materialise(Astra::Registry& reg, TransformOrder& c)
        {
            c.needsWorld.clear();
            auto missing = reg.CreateView<const Transform, Astra::Not<WorldTransform>>();
            missing.ForEach([&](Astra::Entity e, const Transform&)
            {
                if (c.rowOf.TryGet(e))
                    c.needsWorld.push_back(e);
            });
            for (Astra::Entity e : c.needsWorld)
            {
                if (!reg.AddComponent<WorldTransform>(e, WorldTransform{}))
                    continue;
                c.moved[*c.rowOf.TryGet(e)] = 1;   // a fresh identity matrix must be composed
            }
            return !c.needsWorld.empty();
        }

        // The pre-pass: chunk-reject first, then -- because Transform is tracked
        // -- exactly the entities whose Transform was written since lastRun.
        // Entities outside the subtree (not in rowOf) are ignored.
        static bool MarkMoved(Astra::Registry& reg, TransformOrder& c)
        {
            bool any = false;
            auto changed = reg.CreateView<const Transform, Astra::Changed<Transform>>();
            changed.Since(c.lastRun).ForEach([&](Astra::Entity e, const Transform&)
            {
                if (const std::uint32_t* row = c.rowOf.TryGet(e))
                {
                    c.moved[*row] = 1;
                    any = true;
                }
            });
            return any;
        }

        // The linear pass. THE SUBTLETY is unchanged: `inherited` is what makes a
        // moved parent drag its whole subtree; one forward pass is enough because
        // `order` is topological -- row p was decided before row i is read.
        // A clean, non-inherited row does no lookup at all. A non-spatial node
        // (never had a Transform, or the Inspector removed one) contributes no
        // dirtiness and keeps its mirror row, so its children compose against
        // exactly what they used to read off the component.
        static void Compose(Astra::Registry& reg, TransformOrder& c)
        {
            const std::size_t n = c.order.size();
            for (std::size_t i = 0; i < n; ++i)
            {
                const std::uint32_t p = c.parentIndex[i];
                const bool inherited = (p != TransformOrder::kNoParent) && c.dirty[p] != 0;
                const bool moved     = c.moved[i] != 0;
                c.moved[i] = 0;   // consumed: the next pre-pass starts clean
                if (!inherited && !moved)
                {
                    c.dirty[i] = 0;
                    continue;   // the point of the exercise: no matrix work at all
                }

                const Astra::Entity e = c.order[i];
                const Transform* local = std::as_const(reg).GetComponent<Transform>(e);
                if (!local)
                {
                    c.dirty[i] = 0;
                    continue;
                }
                // The ONE non-const fetch: this is the write. Compose is the sole
                // writer of WorldTransform::matrix in engine source (see world[]).
                WorldTransform* world = reg.GetComponent<WorldTransform>(e);
                if (!world)
                {
                    c.dirty[i] = 0;   // Materialise refused it this frame; it retries next frame
                    continue;
                }

                const glm::mat4 localMat = local->ToMatrix();
                c.world[i] = (p == TransformOrder::kNoParent) ? localMat
                                                              : c.world[p] * localMat;
                world->matrix = c.world[i];
                c.dirty[i]    = 1;
                ++c.composed;
            }
        }
    };
}
