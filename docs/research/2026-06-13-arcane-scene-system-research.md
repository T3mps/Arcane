# Arcane Scene System Research

**Date:** 2026-06-13
**Status:** RESEARCH -- do not implement until a design phase is run.
**Scope:** Astra v3.1 actual API audit + scene-system pattern survey + recommendation
for M4 simulation substrate (thin vertical slice).

---

## 1. Astra v3.1 -- Actual API Audit

### Version

`ThirdParty/Astra/include/Astra/Core/Version.hpp` reports **3.1.0**. This matches
the spec's "Astra v3.1 hardening" gate. The library is header-only, C++20-compatible.

### 1.1 Relations System

**Storage.** Astra separates hierarchy storage from archetype storage deliberately
(noted in `Astra/CLAUDE.md`: "Separate from component storage to prevent archetype
fragmentation"). The `RelationshipGraph` class
(`include/Astra/Registry/RelationshipGraph.hpp`) owns three `FlatMap` tables:

```
FlatMap<Entity, Entity>              m_parents;   // child -> parent
FlatMap<Entity, ChildrenContainer>   m_children;  // parent -> children (SmallVector<Entity,4>)
FlatMap<Entity, LinksContainer>      m_links;     // entity -> bidirectional links (SmallVector<Entity,8>)
```

This is NOT a Flecs-style "relationship pair as a component type" model. It is a
separate side-graph with two distinct relation kinds:

- **Parent-child hierarchy** -- single parent, multiple children, tree-only (cycle
  detection enforced). `SetParent(child, parent)` / `RemoveParent(child)` /
  `GetChildren(parent)` / `HasParent(child)`.
- **Bidirectional links** -- unordered peer connections. `AddLink(a, b)` / `RemoveLink`.

**Key API on Registry (the host facade):**
```cpp
registry.SetParent(child, parent);          // wires parent-child + emits ParentChanged
registry.RemoveParent(child);
registry.AddChild(parent, child);           // alias for SetParent(child, parent)
registry.GetParent(child) -> Entity
registry.GetChildren(parent) -> vector<Entity>
registry.ForEachChild(parent, func)
registry.GetChildCount(parent) -> size_t
registry.HasChildren(parent) / HasParent(child) / IsParentOf / IsChildOf
registry.AddLink(a, b) / RemoveLink / AreLinked
registry.GetRelations<QueryArgs...>(entity) -> Relations<QueryArgs...>
```

**Traversal via `Relations<>` object** (`include/Astra/Registry/Relations.hpp`):
```cpp
auto rel = registry.GetRelations<Transform>(entity);
rel.GetParent()                     // -> Entity (filtered by QueryArgs)
rel.GetChildren()                   // -> ChildrenContainer
rel.ForEachChild(func)              // func(Entity) or func(Entity, Transform&)
rel.ForEachDescendant(func)         // BFS, func(Entity, depth)
rel.ForEachAncestor(func)           // walk up, func(Entity, depth)
rel.ForEachLink(func)
rel.ParallelForEachDescendant(func) // batches of 64, needs injected IWorkScheduler
```

**Traversal caching.** `RelationshipGraph` maintains per-root `TraversalCache`
(BFS pre-order vector of `{entity, depth}` pairs) for both descendants and ancestors,
keyed by a `uint32_t structureVersion` counter. Cache is rebuilt lazily under a
`shared_mutex` (readers concurrent, writer exclusive). The cache is SIMD-aligned.
Cache invalidation fires on any `SetParent`/`RemoveParent` call.

**Hierarchy capability verdict.** Astra's hierarchy support is directly scene-graph
capable: single-parent tree, BFS traversal with depth, optional component filtering
during traversal, deferred structural changes via `CommandBuffer::SetParent`. It
does NOT support multi-parent or arbitrary relation types (no Flecs-style
"(ChildOf, Parent)" typed pairs). The links API is for peer relationships only
(e.g., LOD connections, shared-physics joints).

**CommandBuffer wires.** `CommandBuffer` (`include/Astra/Commands/CommandBuffer.hpp`)
has full structural-change support for deferred execution from job threads:
```cpp
cmd.SetParent(child, parent)
cmd.RemoveParent(child)
cmd.AddChild(parent, child)
cmd.RemoveAllChildren(parent)
cmd.AddLink(a, b) / RemoveLink
```
`ParallelCommandBuffer` provides per-thread `CommandBuffer` instances that merge
and flush back to the registry on the main thread after parallel passes complete.

### 1.2 Systems Scheduler and enkiTS Hook

**System concept** (`include/Astra/System/System.hpp`):
```cpp
template<typename T>
concept System = requires(T system, Registry& registry) {
    { system(registry) } -> std::same_as<void>;
};
```
Any callable `void(Registry&)` is a system. Lambdas work via `LambdaSystemWrapper`
which auto-detects `const T&` vs `T&` parameters as read/write hints.

**Dependency declaration** via nested type aliases:
```cpp
struct MySystem : SystemTraits<Reads<Position, Velocity>, Writes<Transform>> {
    void operator()(Registry& reg) { ... }
};
```
`ReadsComponents` and `WritesComponents` are `std::tuple` type lists. If absent,
the scheduler conservatively treats the system as touching everything (forces
sequential).

**SystemScheduler** (`include/Astra/System/SystemScheduler.hpp`) builds a parallel
execution plan via bitmask conflict analysis:
- Systems are grouped greedily into parallel "groups" based on read/write mask conflicts.
- Write-write, write-read, and read-write conflicts prevent co-grouping.
- Systems without trait hints are never co-grouped (conservative).
- Plan is cached and rebuilt only when systems are added/removed.
- `Execute(registry)` runs sequentially by default (SequentialExecutor).
- `Execute(registry, executor)` accepts a custom `ISystemExecutor`.

**ISystemExecutor** (`include/Astra/System/SystemExecutor.hpp`):
```cpp
struct ParallelExecutor : public ISystemExecutor {
    explicit ParallelExecutor(shared_ptr<IWorkScheduler> scheduler);
    void Execute(const SystemExecutionContext& context) override;
    // For each parallel group: m_scheduler->ParallelFor(group.size(), 1, ...)
};
```

**IWorkScheduler seam** (`include/Astra/Core/WorkScheduler.hpp`):
```cpp
class IWorkScheduler {
public:
    virtual void ParallelFor(size_t count, size_t minBatch,
                             const std::function<void(size_t, size_t)>& fn) = 0;
    virtual size_t WorkerCount() const noexcept = 0;
};
```
Astra creates NO threads. The host injects a shared `IWorkScheduler` instance via
`Registry::Config::workScheduler`. The same shared pointer must be passed to every
module in DLL setups.

**EnkiTS adapter pattern.** The engine spec (`2026-06-11-engine-architecture-design.md`)
mentions an `Arcane::Jobs` module wrapping enkiTS. Concretely:
- Arcane creates one `enkiTS::TaskScheduler` instance owned by the `Jobs` module.
- An `EnkiTSWorkScheduler : Astra::IWorkScheduler` adapter wraps it, implementing
  `ParallelFor` by splitting `[0, count)` into `minBatch`-sized `enkiTS::TaskSet`
  sub-ranges and waiting on a parent task set.
- This adapter is injected into `Registry::Config::workScheduler` and into
  `ParallelExecutor` at engine startup.
- `WorkerCount()` returns `scheduler->GetNumTaskThreads()`.
- The adapter must NOT yield or migrate OS threads mid-invocation (Astra thread_local
  contract from WorkScheduler.hpp comments).

**Important scheduler limitation for scene systems.** The `SystemScheduler` has NO
concept of phases or stages. There is one flat ordered list of systems per scheduler
instance. The engine will need to maintain multiple schedulers (FixedUpdate vs
VariableUpdate vs Render) or a phase-ordered list, and run them sequentially between
phases. This is a layer the engine must add on top of Astra -- Astra does not provide
pipeline/phase grouping itself.

### 1.3 Registry and Query API

**Archetype storage.** Classic archetype ECS: entities sharing identical component
sets are stored in a single `Archetype` backed by 16 KB chunks in SoA format.
Trivially copyable types get `memcpy` fast paths; non-trivial types use type-erased
descriptors with move/copy/dtor function pointers.

**View / query API:**
```cpp
auto view = registry.CreateView<Position, Velocity, Not<Frozen>, Optional<Tag>>();
view.ForEach([](Entity e, Position& p, Velocity& v) { ... });
view.ParallelForEach([](Entity e, Position& p, Velocity& v) { ... }); // uses workScheduler
```
Query modifiers: `Optional<T>`, `Not<T>`, `Any<T...>`, `OneOf<T...>`.

**Resources** (singletons): `registry.SetResource<T>(...)` / `GetResource<T>()` --
global per-registry, not associated with any entity. Useful for time, physics world,
render context, etc.

**Signals.** `Registry` emits `EntityCreated`, `EntityDestroyed`, `ComponentAdded`,
`ComponentRemoved`, `ParentChanged`, `LinkAdded`, `LinkRemoved`, `ResourceAdded`,
`ResourceUpdated`, `ResourceRemoved` via a `SignalManager`. Signals are opt-in
(disabled by default for performance).

**Structural change safety.** Adding/removing components mid-iteration is forbidden
(asserted). The correct pattern is `CommandBuffer` for deferred mutations from
parallel systems, flushed after the parallel phase completes.

**Reflection / editor support.** `InspectEntity(entity)` returns
`vector<ComponentInfo>{descriptor, meta, data}` for all components. Meta is
populated by `ASTRA_REFLECT` macros (`include/Astra/Reflection/`). Useful for
Grimoire inspector panels.

**Performance notes (from CLAUDE.md benchmarks):**
- ForEach: ~1.05 ns/entity; range-based for: ~3-4 ns/entity.
- Archetype transitions (add/remove component) are expensive -- minimize at runtime.
- Default entity size: 32-bit (24-bit ID + 8-bit version = 16M entities, 256 versions).
  Configurable to 64-bit via `ASTRA_ENTITY_BITS`.

### 1.4 Serialization

**Full registry serialization** is built in. `Registry::Save(path)` /
`Registry::Save() -> vector<byte>` serializes entity manager, all archetypes
(components), AND the relationship graph in one call. `Registry::Load(path,
componentRegistry)` reconstructs. The format uses:
- A `BinaryHeader` with entity/archetype counts.
- Optional LZ4 compression (default: on, threshold 1 KB).
- CRC32 checksum (computed over data region, finalized into header).
- `BinaryWriter` / `BinaryReader` with versioned component support:
  `WriteVersionedComponent<T>` emits `TypeID<T>::Hash()` (XXHash64 of type name)
  + version number + data. On load, unknown hashes are skipped for forward compat.

**Versioned migration.** `SerializationTraits<T>` can be specialized to provide a
`Version` constant and custom `Serialize`/`Deserialize` with migration logic. This
is the basis for hot-reload state survival across component-layout changes (the
`GamePlugin_SaveState` / `GamePlugin_LoadState` contract in the engine spec).

**RelationshipGraph serialization.** The graph serializes parent-child pairs AND
children maps redundantly (for faster reconstruction), plus link pairs. Deserialized
via `RelationshipGraph::Deserialize(reader)` returning `Result<RelationshipGraph,
SerializationError>`. Entity IDs are written as raw `StorageType` values.

**Implication for scene save/load.** A scene IS a registry snapshot (or a sub-range
of one). `Registry::Save()` / `Load()` is the natural scene serialization path for
hot reload and play-in-editor. For persistent level files (Grimoire), a JSON or
text-format layer on top of the binary format (or using the reflection metadata to
emit JSON via `Astra::JsonSchema`) would allow human-editable authoring.

**Spec assumption delta.** The engine spec refers to "Astra versioned serialization"
and `Astra::BinaryWriter` / `BinaryReader` as if they are just stream types. In
reality, `Registry::Save()` is a complete, integrated serialization pipeline --
callers should use it, not build their own. The spec's hot-reload sequence
(`GamePlugin_SaveState(writer)` / `GamePlugin_LoadState(reader)`) needs to write
the whole registry (or the relevant sub-registries) via `Registry::Save()` into
the writer's buffer, rather than custom per-component loops.

**Spec assumption delta 2.** The spec implies `Astra::BinaryWriter` is passed
directly as a stream to plugin code. The actual API is `operator()(T)` (chained)
for POD types and custom `Serialize(BinaryWriter&)` methods. Plugins should call
`registry.Save(buffer)` rather than manually streaming components.

### 1.5 TypeContext and DLL ABI

`TypeContext` (`include/Astra/Core/TypeContext.hpp`) assigns component IDs by stable
XXHash64 of the type name, shared across modules. Contract:
- `SetTypeContext(ctx)` in `GamePlugin_Init` before any `TypeID<T>::Value()` call.
- `componentRegistry->ReRegisterComponent<T>()` per component type after each reload.
- Never call `TypeID<T>::Value()` from static initializers in plugin modules.
These are correctly specified in the engine spec and confirmed by the CLAUDE.md.

---

## 2. Scene-System Patterns Survey

### Unity (GameObject + Transform hierarchy + Scenes/Prefabs)

The Unity model is built on a mandatory `Transform` component that owns the
parent-child wiring. Every `GameObject` has a `Transform`; hierarchy IS the
transform tree. Scenes are serialized object graphs (YAML); prefabs are reusable
sub-graphs. The scene is loaded as a monolithic file, instanced from prefabs.

Key insight worth borrowing: **the scene asset is authoritative for initial state;
the runtime world is derived from it**. Prefab overrides (instance patches) give
composition without full duplication.

Key problem: Transform hierarchy lives in a separate data structure from other
components, causing coherence issues -- deep trees with many dirty nodes incur O(N)
propagation. Unity's ECS (DOTS) moved to a pure archetype model with a separate
`LocalTransform` + `LocalToWorld` + parent-tracking component, propagating world
matrices via a system.

**What to borrow:** the clean prefab/scene split; the idea of a "scene root" entity
whose subtree IS the scene. What to avoid: mandatory `Transform` on every entity,
monolithic scene YAML files with per-field GUIDs everywhere.

### Godot (Node tree, scenes as reusable trees)

Godot's scene system is more radical: the scene IS a reusable, composable tree.
Any node can be a sub-scene (a `.tscn` file). The root node of a scene is an entity
in its parent scene's tree. Instantiating a scene = copying its node tree.

Key insight: **scenes are first-class values, not containers**. A level is a node
that instances other scenes. This compositional approach maps neatly to an ECS
hierarchy: "prefab" = a serialized entity subtree; "scene instantiation" = spawn
root + children from a template.

Key problem: Godot's node-signal system creates tight coupling between tree position
and behavior; ECS explicitly separates those concerns.

**What to borrow:** compositional scene trees, scene-as-reusable-value, lightweight
instantiation. What to avoid: node signals as the primary communication pattern
(use ECS queries instead).

### Unreal (Actor/Component, Levels, World Partition)

Unreal uses a two-level hierarchy: `World` contains `Level`s; a `Level` contains
`Actor`s; an `Actor` contains `UActorComponent`s with one `USceneComponent` root
defining the spatial tree. Levels stream in/out independently; World Partition
tiles them spatially.

Key insight: **levels as streaming units** -- this is the right mental model for
a 2D isometric game with map chunks. A "scene" is not always a monolithic thing;
in Aphelyon's context, map regions (currently `Client/data/maps/`) map to streaming
level units.

Key problem: Actor/Component is an object model, not data-oriented. Each Actor is
an allocation; component arrays are not cache-coherent. Unreal 5's Mass Entity
added ECS-style fragments alongside this.

**What to borrow:** the level-as-streaming-unit concept; the separation of "world"
(the Registry) from "level" (a serialized entity set that can load/unload). What
to avoid: per-actor allocations, OOP hierarchy for game logic.

### Bespoke ECS-native (Flecs/EnTT + transform propagation)

The consensus ECS pattern for scenes:
1. **Scene = serialized set of entities + their components + their relations.** No
   special scene object; a "scene" is just a slice of the world registry.
2. **Hierarchy via parent-child relationship.** Flecs uses typed relation pairs
   `(ChildOf, parent)`; EnTT uses a separate hierarchy component or registry groups.
   Astra's `RelationshipGraph` is the direct equivalent.
3. **Transform propagation via system.** A `TransformPropagationSystem` walks the
   hierarchy (BFS order) and computes world-space matrices from `LocalTransform` +
   parent `WorldTransform`. This is a read-modify-write pass over two components;
   it runs after any system that mutates local transforms.
4. **Prefab / scene template = entity + component archetype prototype.** Spawning
   = clone entity tree from a prototype entity (or from a serialized binary blob).
5. **No transform on non-spatial entities.** Pure ECS: only entities that need
   spatial context carry `LocalTransform`/`WorldTransform`.

Key tradeoff vs Unity/Godot: no editor-integrated scene format out of the box;
tooling must be built. Grimoire is exactly this tooling.

**The most important bespoke insight:** transform propagation correctness requires
BFS traversal ORDER. If a child's world transform is computed before its parent's,
it reads a stale parent `WorldTransform`. `Relations::ForEachDescendant` gives BFS
order already -- this maps perfectly to a single-pass propagation system.

---

## 3. Recommendation: Arcane Formal Scene System

### Summary verdict on Astra capabilities

Astra v3.1 is directly scene-graph capable. The `RelationshipGraph` provides a
proper single-parent tree with BFS traversal, filtering, and deferred structural
changes via `CommandBuffer`. The serialization system can snapshot and restore a
full world. The scheduler can be parallelized via an enkiTS adapter. No fundamental
missing piece; the scene system is a design and thin-code problem, not an
infrastructure problem.

### Core components needed

The minimal formal scene system for the thin vertical slice requires:

```
struct LocalTransform { glm::vec2 position; float rotation; glm::vec2 scale; };
struct WorldTransform { glm::mat3 matrix; };  // computed, never authored
struct SceneTag {};     // marks scene root entity (optional; can use resource)
struct SpriteRenderer { AssetHandle texture; Color tint; int sortingLayer; int orderInLayer; };
```

And one system:
```cpp
struct TransformPropagationSystem : SystemTraits<Reads<LocalTransform>, Writes<WorldTransform>> {
    void operator()(Registry& reg) {
        // BFS order guaranteed by ForEachDescendant
        reg.GetRelations(sceneRoot).ForEachDescendant([&](Entity e, size_t depth) {
            auto* local = reg.GetComponent<LocalTransform>(e);
            auto* world = reg.GetComponent<WorldTransform>(e);
            Entity parent = reg.GetParent(e);
            auto* parentWorld = reg.GetComponent<WorldTransform>(parent);
            world->matrix = parentWorld->matrix * local->ToMatrix();
        });
    }
};
```

### Option A: Lean entirely on Astra relations (RECOMMENDED for thin slice)

Use `RelationshipGraph` directly for the spatial hierarchy. One `SceneRoot` entity
(stored as a registry resource) owns all scene entities via the parent-child graph.
`TransformPropagationSystem` uses `Relations::ForEachDescendant` in BFS order.
Serialization = `registry.Save()`.

**Pros:**
- Zero extra infrastructure. Hierarchy is already implemented and tested.
- BFS traversal order is pre-baked in `TraversalCache` -- propagation is a single
  sequential pass with no re-sorting needed.
- `CommandBuffer::SetParent` works correctly from parallel jobs.
- `ParallelForEachDescendant` is available if the propagation pass ever needs to
  parallelize subtrees (requires injected scheduler; falls back to sequential if not
  provided).
- Scene save/load = `registry.Save()` / `Registry::Load()` -- one line each.

**Cons:**
- No stage/phase concept in Astra's scheduler. Engine must maintain separate
  schedulers or an ordered phase list. **This is the primary missing piece.**
- `TraversalCache` invalidates on every `SetParent`/`RemoveParent` -- scenes with
  heavy runtime reparenting will pay rebuild cost on every traversal. For 2D sprites
  this is rarely a problem.
- No multi-parent or typed relations. Non-issue for transform parenting; note it if
  LOD or animation blending needs multi-target links (use the Links API for that).

**Implementation sketch for the thin slice:**
1. Add `LocalTransform`, `WorldTransform`, `SceneTag`, `SpriteRenderer` components.
2. Create a `SceneRoot` entity, store its ID as a registry resource.
3. Parent all scene entities under it via `registry.SetParent`.
4. Add `TransformPropagationSystem` to the FixedUpdate scheduler.
5. Add `RenderSubmissionSystem` (reads `WorldTransform` + `SpriteRenderer`, submits
   quads to Batcher2D) to the per-frame scheduler.
6. Scene serialization: `registry.Save("scene.bin")`.

### Option B: Thin scene-graph layer over Astra relations

Add a `SceneGraph` class that wraps the registry, provides a `Spawn(prefab)` API,
and manages a "scene root" per loaded scene. Scenes can be loaded/unloaded as units
by tracking which entities belong to each scene (via a `SceneID` tag component or a
resource map).

**Pros:**
- Explicit scene boundary makes multi-scene (additive loading, streaming) cleaner.
- Prefab instantiation encapsulation (clone an entity subtree from a prototype).
- Natural place to add a scene JSON format for Grimoire authoring.

**Cons:**
- More code up front, before the thin slice needs it.
- Risk of duplicating logic already in the registry (lifecycle, signals).
- Deferrable: Option A can evolve into Option B by adding the `SceneGraph` wrapper
  later without breaking the relations-based hierarchy.

**Verdict:** defer Option B until Grimoire's LevelEditor demands it (M6+).

### Option C: Pure component-based hierarchy (no Astra relations)

Store parent entity ID as a component: `struct Parent { Entity id; }`. Propagate by
querying all `Parent`-tagged entities, sorting by depth (pre-computed `Depth` component),
then running propagation. This is how some EnTT users work around EnTT's lack of a
built-in hierarchy.

**Pros:** No dependency on `RelationshipGraph`; trivially serialized as a component.

**Cons:**
- Re-implements what Astra already provides -- and worse: depth sorting requires a
  sort pass every frame or a dirty flag, whereas `RelationshipGraph::TraversalCache`
  is already BFS-ordered and lazily rebuilt.
- Structural integrity (cycle detection) must be reimplemented.
- `CommandBuffer` relation commands already handle deferred reparenting -- this option
  abandons them.

**Verdict:** do not use. Astra's relations are strictly superior for this use case.

### Phase / Pipeline system (the real gap)

Astra's `SystemScheduler` is a flat ordered list with no phase concept. The engine
spec's host loop has three distinct phases (FixedUpdate, VariableUpdate, Render).
The recommended approach is:

```cpp
// In Arcane::RunLoop / EngineContext
SystemScheduler fixedScheduler;   // 60 Hz systems
SystemScheduler updateScheduler;  // per-frame systems  
SystemScheduler renderScheduler;  // render submission

// Shared executor (enkiTS-backed)
ParallelExecutor executor(workScheduler);

// Per-frame:
fixedScheduler.Execute(registry, &executor);   // 0..N fixed steps
updateScheduler.Execute(registry, &executor);  // once per frame
renderScheduler.Execute(registry, &executor);  // render submission
```

Game plugins register systems into the appropriate scheduler in `GamePlugin_Init`.
The engine owns the execution cadence. This is a thin wrapper the engine must add;
Astra does not provide it.

### Risks and unknowns

1. **TraversalCache BFS order with parallel scene writes.** If parallel jobs
   reparent entities (via `ParallelCommandBuffer::SetParent`), the cache is
   invalidated mid-frame. The correct protocol is: accumulate reparenting commands
   during the parallel phase, flush `ParallelCommandBuffer::Execute()` before
   running `TransformPropagationSystem`. Document this as a frame protocol rule.

2. **Scene entity ID stability across hot reload.** `Registry::Save()` / `Load()`
   restores entity IDs from `EntityManager::Deserialize`. Entity ID values are
   stable across save/load WITHIN a session. Across hot reloads, entity IDs in
   handles (e.g., in `Parent` components or UI binding targets) survive because the
   saved snapshot carries the IDs. Risk: if a reload adds NEW entities in
   `GamePlugin_Init` before `LoadState`, those new entities may collide with
   restored IDs. Safe protocol: init only registers/configs in `Init` before
   `LoadState`; world population happens in `LoadState`.

3. **32-bit entity limit.** Default: 16M entity IDs. For a 2D game with map
   streaming, this is more than sufficient. Do not change to 64-bit without a
   measured need -- it doubles entity storage size.

4. **Scheduler execution plan rebuild cost.** `SystemScheduler::BuildExecutionPlan`
   is O(N^2) in system count but runs only when systems are added/removed.
   For game-code systems (< 100), this is negligible. Do not add/remove systems
   at runtime within a frame.

5. **`RelationshipGraph` shared_mutex under parallel traversal.** Cache reads use
   a shared_mutex with the read path holding a shared lock. Under heavy parallel
   traversal (many workers calling `ForEachDescendant` on different roots
   concurrently), this is fine -- shared reads are concurrent. Writes (cache rebuild)
   acquire an exclusive lock; this is rare (only on structure changes).

---

## File References

| File | Purpose |
|---|---|
| `ThirdParty/Astra/include/Astra/Registry/Registry.hpp` | Central API: entities, components, resources, relations, signals, serialization |
| `ThirdParty/Astra/include/Astra/Registry/RelationshipGraph.hpp` | Parent-child + links storage, traversal cache, serialization |
| `ThirdParty/Astra/include/Astra/Registry/Relations.hpp` | Template view over the graph for filtered traversal + parallel descent |
| `ThirdParty/Astra/include/Astra/System/SystemScheduler.hpp` | System registration, parallel plan building, execution |
| `ThirdParty/Astra/include/Astra/System/SystemExecutor.hpp` | ISystemExecutor, SequentialExecutor, ParallelExecutor |
| `ThirdParty/Astra/include/Astra/System/System.hpp` | System concept, SystemTraits, Reads<>/Writes<>, LambdaSystemWrapper |
| `ThirdParty/Astra/include/Astra/Core/WorkScheduler.hpp` | IWorkScheduler seam for enkiTS adapter |
| `ThirdParty/Astra/include/Astra/Commands/CommandBuffer.hpp` | Deferred commands including SetParent/AddChild; ParallelCommandBuffer |
| `ThirdParty/Astra/include/Astra/Serialization/BinaryWriter.hpp` | Binary serialization: POD fast-path, versioned components, LZ4 |
| `ThirdParty/Astra/include/Astra/Core/Version.hpp` | Confirms v3.1.0 |
| `ThirdParty/Astra/include/Astra/Core/TypeContext.hpp` | Cross-DLL type identity; SetTypeContext / ReRegisterComponent |
| `docs/superpowers/specs/2026-06-11-engine-architecture-design.md` | Engine spec (host loop, plugin ABI, hot reload) |
| `docs/superpowers/specs/2026-06-12-arcane-2d-renderer-architecture.md` | Renderer north star |
