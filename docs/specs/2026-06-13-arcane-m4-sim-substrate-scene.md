# Arcane M4 — Simulation Substrate + Scene Vertical Slice — Design

**Date:** 2026-06-13
**Status:** APPROVED — design contract for the engine's simulation substrate
(Astra Registry + enkiTS + per-phase schedulers + RunLoop) and a minimal formal
scene system, gated by an Astra 3.2 serialization-flexibility release (Phase 0).
**Upstream:**
- `2026-06-11-engine-architecture-design.md` (module layout, plugin ABI, host loop,
  IWorkScheduler seam — this spec fills the "Astra ball scene on enkiTS + RunLoop"
  substrate the bring-up order assumed but never built).
- `2026-06-12-arcane-2d-renderer-architecture.md` (Batcher2D submission path + sort keys).
- `docs/superpowers/research/2026-06-13-arcane-scene-system-research.md` (Astra 3.1 API
  audit; scene-pattern survey; **Option A** recommendation = lean on native Astra relations).
- `docs/superpowers/research/2026-06-13-serialization-architecture-aaa.md` (AAA serialization
  survey; **Option B/reflection-as-truth** recommendation; binary path stays untouched).

## Milestone framing and numbering

The engine bring-up order in the architecture spec labelled "M4 = Plugin module
(PluginHost + Game DLL + hot reload)". That order also assumed M3 would land a full
Playground scene "on Astra + enkiTS". In practice M3 shipped only the snapshot-driven
**input action system** — the Astra Registry, the enkiTS wiring, the per-phase scheduler
layer, the `RunLoop`, and any formal scene were never built. This milestone builds that
**simulation substrate** and a thin **scene vertical slice**, so:

- **This milestone is M4 = Simulation Substrate + Scene Slice.**
- The architecture spec's old "M4 = Plugin module" **renumbers to M5**; physics → M6;
  Grimoire/LevelEditor → M7. The architecture spec's bring-up table should be updated to
  match (a one-line edit; recorded here so the renumber is explicit, not silent).

The substrate is the load-bearing prerequisite for the plugin host: a `Game.dll` cannot
register components/systems into "the engine Registry" or survive hot reload via
`registry.Save(buffer)` until the engine actually owns a Registry, schedulers, and a loop.
M4 builds exactly that, end-to-end, with the smallest scene that exercises all of it.

## Why one spec with Astra 3.2 as Phase 0

Astra 3.2 is a gating prerequisite, not an independent subsystem: its only near-term
consumer is this milestone, and the serialization seam it adds exists specifically to
enable the **reflection -> JSON** path that this milestone first exercises (the engine's
JSON backend is the seam's first real consumer). Keeping them in one milestone keeps the
"why" coherent. Astra retains its own repo/branch/release workflow (Phase 0 captures it),
but the narrative is one milestone: *make serialization format-flexible, then stand up the
simulation that will use it.*

## Goals

1. **Astra 3.2:** a dependency-free, format-agnostic field-visitor seam on
   `ComponentRegistry`, driven by reflection, so an end user can serialize components to
   ANY format by walking reflection — **without** adding a JSON (or any external)
   dependency to Astra and **without** touching the built-in binary path. Plus a
   field-rename / version-migration attribute. Re-vendor into the Gacha repo at 3.2.
2. **Jobs:** wire the already-vendored enkiTS into `Arcane.dll` via an
   `Astra::IWorkScheduler` adapter, so Astra parallel passes run on enkiTS threads.
3. **Simulation substrate:** an engine-owned `Astra::Registry`, three per-phase
   `Astra::SystemScheduler`s (FixedUpdate / Update / Render) backed by a shared enkiTS
   `ParallelExecutor`, and an exported `Arcane::RunLoop` (fixed-timestep accumulator +
   render alpha) so future hosts (Loom/Grimoire) stay thin.
4. **Minimal formal scene (Option A — native Astra relations):** `LocalTransform` /
   `WorldTransform` / `SpriteRenderer` components + a `SceneRoot` resource;
   `TransformPropagationSystem` (BFS world-matrix propagation) and `RenderSubmissionSystem`
   (-> Batcher2D); binary scene save/load via `Registry::Save`/`Load`.
5. **Reflection -> JSON bridge (the seam's first consumer, in Arcane):** a
   `ReflectionJsonWriter` / `ReflectionJsonReader` (IFieldVisitor impls over nlohmann/json)
   that round-trips the scene's components, proving the north-star path is real.
6. **Vertical slice:** Playground renders a moving parent/child sprite scene through
   `RunLoop` + Batcher2D; a `[gpu]` test asserts `RenderErrorCount() == 0`; headless tests
   cover the enkiTS scheduler and transform propagation without a GPU.

**Design rule (non-negotiable):** every engine component type is `ASTRA_REFLECT`-annotated
from day one. This keeps the editor / JSON / server path open at ~zero cost and is what
makes the Phase 0 seam useful to the engine immediately.

## Non-goals (deferred; captured in Roadmap)

- Plugin host, `Game.dll`, `Loom.exe`, hot reload (M5).
- Render graph, bindless, batcher v2, materials, lighting (renderer-spec later milestones).
- UI runtime, Grimoire editor, LevelEditor (later milestones).
- A general schema-driven scene-JSON for arbitrary component rosters (Grimoire-era);
  M4's JSON path is scoped to the slice's known component set.
- Full `Assets` integration for sprite textures (a thin `TextureTable` resolution seam
  stands in; `textureId == 0` renders an untextured tinted quad so the slice needs no asset).
- Promoting the reflection->JSON bridge into `Arcane.Core` for server reuse (north-star;
  it stays in `Arcane.dll` for M4 to contain the blast radius).

---

## Phase 0 — Astra 3.2 (serialization flexibility)

Worked in `D:\dev\starworks\Astra` on branch `dev`. Astra is header-only, C++20, GoogleTest
(`AstraTest`, premake5 `vs2022`). **Hard constraints:** the binary path
(`BinaryWriter`/`BinaryReader`/`SerializationTraits`/`Registry::Save`) is UNCHANGED; NO
external/JSON dependency is added to Astra; NO Unreal-style polymorphic `FArchive` (it would
virtualize Astra's hot, non-virtual binary `operator()` per field — a perf regression).
Encoding: UTF-8 without BOM, ASCII in comments (the Astra-hardening lesson).

### 0.1 The diagnosed tension (from the serialization research)

Astra already has TWO serialization-relevant mechanisms that never meet:

- `ComponentDescriptor` carries four function pointers — `serialize`, `deserialize`,
  `serializeVersioned`, `deserializeVersioned` — all hard-typed to `BinaryWriter&` /
  `BinaryReader&` (`Component/Component.hpp`). `Registry::Save` drives serialization
  exclusively through these. This is the binary lock-in.
- The reflection system (`ASTRA_REFLECT_*` -> `TypeMeta` with `FieldInfo`s carrying name,
  offset, type traits, type-erased `getter`/`setter`, `std::any` accessors, and a
  `Serializable` attribute) fully describes a type's fields — but `Registry::Save` never
  consults it. `ComponentDescriptor::meta` is already linked at registration but unused for
  serialization.

The fix is the AAA consensus (flecs/Bevy/Unity): **reflection-as-truth.** Add ONE generic,
format-agnostic visitor slot populated from `TypeMeta`, alongside (never replacing) the
binary pointers. The format backend lives in the consumer.

### 0.2 New: `IFieldVisitor` seam (`include/Astra/Reflection/FieldVisitor.hpp`)

```cpp
#pragma once
#include "FieldInfo.hpp"

namespace Astra
{
    // Format-agnostic field visitor. An end user (e.g. the Arcane engine)
    // implements this to drive ANY serialization format by walking reflection.
    // Astra ships NO format backend beyond the built-in binary path -- a JSON,
    // protobuf, or editor backend lives entirely in the consumer.
    class IFieldVisitor
    {
    public:
        virtual ~IFieldVisitor() = default;

        // Invoked once per serializable reflected field of a component instance.
        // `instance` is the component base pointer; read or write the field via
        // field.GetAny(instance) / field.SetAny(instance, value), the typed
        // field.Get<T>/Set<T>, or field.GetPtr<T>(instance). For a nested
        // reflected struct, look its type up by field.typeHash in MetaRegistry
        // and recurse (the consumer owns recursion policy and POD-math fallbacks).
        virtual void Visit(const FieldInfo& field, void* instance) = 0;

        // Direction hint so a single visitor type can serve read and write, and
        // so consumers can branch (e.g. allocate vs read) without RTTI.
        ASTRA_NODISCARD virtual bool IsWriting() const noexcept = 0;
    };
}
```

### 0.3 `ComponentDescriptor` gains ONE slot (`Component/Component.hpp`)

Forward-declare `class IFieldVisitor;` beside the existing `BinaryWriter`/`BinaryReader`
forward declarations, then add (binary pointers untouched):

```cpp
using VisitFieldsFn = void(void* instance, IFieldVisitor& visitor);
...
VisitFieldsFn* visitFields = nullptr;  // null when the type is not reflected
```

### 0.4 `ComponentRegistry` populates the slot from reflection (`Component/ComponentRegistry.hpp`)

`#include "../Reflection/FieldVisitor.hpp"`. In `RegisterComponentImpl<T>`, after the
existing `desc.meta = MetaRegistry::Instance().Get<T>();` line:

```cpp
desc.visitFields = desc.meta ? &VisitFields<T> : nullptr;
```

and add the static helper (mirrors the existing `Serialize<T>` family):

```cpp
template<typename T>
static void VisitFields(void* instance, IFieldVisitor& visitor)
{
    const TypeMeta* meta = MetaRegistry::Instance().Get<T>();
    if (!meta) return;
    for (const FieldInfo& field : meta->fields)
    {
        if (!field.IsSerializable()) continue;   // honors the Serializable(false) attribute
        visitor.Visit(field, instance);
    }
}
```

This is the entire registry change. `Registry::Save`, the binary writer/reader, and every
existing component's `Serialize` method are untouched. Consumers reach the slot through the
already-public `ComponentRegistry::GetComponentDescriptor(id)` / `GetAllComponentIDs()` and
call `desc->visitFields(componentPtr, visitor)`.

### 0.5 New: `AliasName` attribute (`include/Astra/Reflection/Attribute.hpp`)

For field renames / version migration on name-keyed formats (binary uses
`SerializationTraits<T>::Version`/`Migrate` and is unaffected):

```cpp
// Records a former serialized name for a field so name-keyed format loaders can
// find a value written under the old name after a rename. Multiple AliasName
// attributes may be attached (a field renamed more than once).
struct AliasName : AttributeBase<AliasName>
{
    std::string_view name;
    constexpr explicit AliasName(std::string_view formerName) noexcept
        : name(formerName) {}
};
```

Loaders query via the existing `FieldInfo::ForEachAttribute<AliasName>(func)`: try
`field.name` first, then each alias. No new `FieldInfo` API is required.

### 0.6 AstraTest coverage (`tests/Reflection/FieldVisitorTest.cpp`, GoogleTest)

Format-agnostic — the test "format" is an in-memory `std::map<std::string, std::any>`, so
no JSON enters Astra:

- **Enumeration:** a reflected struct (a few arithmetic fields, one `Serializable(false)`
  field); a `RecordingVisitor` (`IsWriting()==true`) appends `{field.name, field.GetAny}`.
  Assert it visits exactly the serializable fields, in declaration order, skipping the
  opted-out field.
- **Round-trip (both directions, format-agnostic):** a `MapWriteVisitor` fills a
  `std::map<string, std::any>` from instance A; a `MapReadVisitor` (`IsWriting()==false`)
  writes from the map into a default-constructed instance B via `field.SetAny`. Assert A == B
  field-by-field. Proves the seam drives a format both ways with Astra knowing nothing about
  the format.
- **Null slot for unreflected types:** register a reflected and an unreflected component;
  assert `visitFields != nullptr` for the former and `== nullptr` for the latter.
- **AliasName:** a field annotated `ASTRA_REFLECT_ATTR(AliasName, "oldName")`; assert
  `ForEachAttribute<AliasName>` yields `"oldName"`.
- **Binary regression:** the existing `tests/Serialization/*` and `tests/Registry/RegistryTest`
  suites stay green (binary path proven unchanged).

(If the test premake does not auto-glob `tests/**`, register the new `.cpp`.)

### 0.7 Phase 0 workflow (release + re-vendor)

1. Develop + test on branch `dev` in `D:\dev\starworks\Astra`; run `AstraTest` green; push `dev`.
2. In `D:\dev\github\Astra` (branch `main`, the other clone of `github.com/T3mps/Astra.git`):
   merge `dev` -> `main`, then cut a **`3.2`** branch for release.
3. Bump `include/Astra/Core/Version.hpp` to `MINOR 2` (so `ASTRA_VERSION_MINOR == 2`).
4. **Re-vendor** into `D:\dev\starworks\Gacha\ThirdParty\Astra` — a plain vendored copy
   (no submodule): copy the updated headers and the bumped `Version.hpp`. Verify
   `ThirdParty/Astra/include/Astra/Core/Version.hpp` reports 3.2 and that
   `ThirdParty/Astra/include/Astra/Reflection/FieldVisitor.hpp` exists.

Phase 0 is DONE when ThirdParty/Astra is at 3.2 with the seam present and `AstraTest` green.

---

## Phase 1 — Jobs: enkiTS wired into Arcane via the IWorkScheduler seam

enkiTS is vendored (`ThirdParty/enkiTS`, premake project already in the Arcane workspace's
`Dependencies` group and linked by `ArcaneTests`) but NOT yet included or linked by
`Arcane.dll`. Astra creates no threads; the host injects one shared `IWorkScheduler`.

### 1.1 Build wiring (`Arcane/premake5.lua`)

- Add to the `Arcane` (DLL) project `includedirs`: `%{IncludeDir.Astra}`, `%{IncludeDir.enkiTS}`.
- Add `"enkiTS"` to the `Arcane` project `links`.
- Add `%{IncludeDir.Astra}` (and `%{IncludeDir.enkiTS}` where needed) to `Playground`
  `includedirs`. (`ArcaneTests` already has both and links enkiTS.)

### 1.2 `Arcane/Arcane/src/Arcane/Jobs/JobSystem.hpp` / `.cpp`

```cpp
namespace Arcane
{
    // Owns the engine's single enki::TaskScheduler (one per process; the same
    // shared scheduler must be handed to every module in future DLL setups).
    class ARCANE_API JobSystem
    {
    public:
        explicit JobSystem(uint32_t threads = 0);  // 0 => hardware default
        ~JobSystem();
        enki::TaskScheduler& Scheduler() noexcept;
        uint32_t WorkerCount() const noexcept;      // GetNumTaskThreads()
    private:
        // pimpl or direct member; enki::TaskScheduler is move-only/non-copyable
    };

    // Adapts enkiTS to Astra's IWorkScheduler. Injected into Registry::Config
    // and into Astra::ParallelExecutor.
    class EnkiWorkScheduler : public Astra::IWorkScheduler
    {
    public:
        explicit EnkiWorkScheduler(JobSystem& jobs);
        void ParallelFor(size_t count, size_t minBatch,
                         const std::function<void(size_t, size_t)>& fn) override;
        size_t WorkerCount() const noexcept override;
    };
}
```

`ParallelFor` implementation: build an `enki::TaskSet` over `[0,count)` with
`SetSize(count)` and `m_MinRange = minBatch`; the set's callback receives an
`enki::TaskSetPartition{start,end}` and calls `fn(range.start, range.end)`;
`AddTaskSetToPipe(&set)` then `WaitforTask(&set)`. `WorkerCount()` returns
`GetNumTaskThreads()`. **Contract (from `WorkScheduler.hpp`):** the adapter must not yield
or migrate OS threads mid-invocation (Astra's `thread_local` contract). `EngineContext`
will later carry `Astra::IWorkScheduler*` (already in the plugin ABI); for M4 the
adapter is owned by the substrate.

### 1.3 Test (`Tests/src/JobSchedulerTest.cpp`, headless, Catch2)

`Astra::Registry::CreateView<...>().ParallelForEach(...)` over N entities using a Registry
configured with the `EnkiWorkScheduler`; assert every element was visited exactly once
(atomic counter / per-index flag array) and `WorkerCount() >= 1`. No GPU.

---

## Phase 2 — Simulation substrate: Registry, per-phase schedulers, RunLoop

Astra's `SystemScheduler` is a flat ordered list with NO phase concept (research §1.2).
The engine adds the phase layer.

### 2.1 `Arcane/Arcane/src/Arcane/Sim/SystemSchedulers.hpp`

```cpp
namespace Arcane
{
    // The engine's phase layer over Astra's flat schedulers. One scheduler per
    // phase, all sharing one enkiTS-backed executor. Game/scene code registers
    // systems into the appropriate phase; the engine owns the cadence.
    struct SystemSchedulers
    {
        Astra::SystemScheduler fixedUpdate;  // 60 Hz sim
        Astra::SystemScheduler update;       // once per rendered frame (variable dt)
        Astra::SystemScheduler render;       // render submission (-> Batcher2D)
        Astra::ParallelExecutor executor;    // shared, enkiTS-backed

        explicit SystemSchedulers(std::shared_ptr<Astra::IWorkScheduler> sched)
            : executor(std::move(sched)) {}
    };
}
```

### 2.2 `Arcane/Arcane/src/Arcane/Sim/RunLoop.hpp` / `.cpp`

Mirrors the proven Lua Application cadence (fixed 60 UPS, alpha on draw). Exported so hosts
stay thin. M4 honestly splits simulation cadence (engine-owned) from GPU frame plumbing
(host-owned — swapchain/canvas/tonemap/present), because the render graph does not exist yet
(renderer-spec later milestone). RunLoop owns the accumulator + the Fixed/Update phases; the
host brackets the Render phase between `Batcher2D::Begin/End`.

```cpp
namespace Arcane
{
    class ARCANE_API RunLoop
    {
    public:
        struct Config { double fixedHz = 60.0; int maxStepsPerFrame = 5; };

        RunLoop(Astra::Registry& registry, SystemSchedulers& schedulers, Config cfg = {});

        // Advance one real frame: runs >=0 FixedUpdate steps (each at fixed dt,
        // clamped to maxStepsPerFrame to avoid the spiral of death) then the
        // Update scheduler once. Returns the render alpha in [0,1) for
        // interpolation. Does NOT run the Render scheduler.
        double Advance(double realDt);

        // Runs the Render scheduler. The host calls this AFTER Batcher2D::Begin
        // and after setting the RenderContext2D resource, and BEFORE End().
        void SubmitRender();

        double Alpha() const noexcept;
    private:
        Astra::Registry*  m_registry;
        SystemSchedulers* m_schedulers;
        Config m_cfg;
        double m_accumulator = 0.0;
        double m_alpha = 0.0;
    };
}
```

**Frame protocol rule (documented in the header):** any reparenting accumulated via Astra
`CommandBuffer::SetParent` during a parallel pass MUST be flushed before
`TransformPropagationSystem` runs, so the `RelationshipGraph` traversal cache is rebuilt
before BFS propagation reads it. For the M4 slice there is no runtime reparenting, but the
rule is stated so later systems honor it.

### 2.3 Engine Registry assembly (`Arcane/Arcane/src/Arcane/Sim/Simulation.hpp` / `.cpp`)

A thin owner that builds the Registry with the injected scheduler and exposes it:

```cpp
namespace Arcane
{
    class ARCANE_API Simulation
    {
    public:
        explicit Simulation(std::shared_ptr<Astra::IWorkScheduler> sched);
        Astra::Registry& Registry() noexcept;
        SystemSchedulers& Schedulers() noexcept;
    private:
        std::shared_ptr<Astra::IWorkScheduler> m_sched;
        Astra::Registry m_registry;          // Config{ .workScheduler = m_sched }
        SystemSchedulers m_schedulers;        // shares m_sched
    };
}
```

### 2.4 Test (`Tests/src/RunLoopTest.cpp`, headless, Catch2)

- A FixedUpdate system increments a counter resource. Drive `RunLoop::Advance` with a fixed
  real dt for K seconds; assert the counter == round(K * fixedHz) within tolerance and that
  `maxStepsPerFrame` clamps a large dt spike (no runaway).
- Assert `Alpha()` stays in `[0,1)`.

---

## Phase 3 — Minimal formal scene (Option A: native Astra relations)

All components `ASTRA_REFLECT`-annotated (design rule).

### 3.1 Components (`Arcane/Arcane/src/Arcane/Scene/Components.hpp`)

```cpp
namespace Arcane
{
    struct LocalTransform
    {
        glm::vec2 position{0.0f, 0.0f};
        float     rotation = 0.0f;          // radians
        glm::vec2 scale{1.0f, 1.0f};
        glm::mat3 ToMatrix() const;         // TRS in 2D homogeneous form
    };

    struct WorldTransform
    {
        glm::mat3 matrix{1.0f};             // computed by TransformPropagationSystem; never authored
    };

    struct SpriteRenderer
    {
        uint32_t  textureId = 0;            // 0 => untextured tinted quad; resolved via TextureTable
        glm::vec2 size{32.0f, 32.0f};       // base pixel size before world scale
        glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
        int32_t   sortingLayer = 0;
        int32_t   orderInLayer = 0;
    };
}
```

Reflection blocks live in `Components.cpp` (or an adjacent `.inl`). `WorldTransform::matrix`
is reflected but marked `ASTRA_REFLECT_ATTR(Serializable, false)` — it is derived state,
recomputed on load, so it is skipped by the name-keyed JSON path (the binary path's
trivially-copyable fast path still round-trips it harmlessly). `glm::vec2`/`vec3`/`vec4` are
NOT reflected member-by-member (their anonymous-union members are fragile to reflect);
instead the Arcane JSON backend (Phase 4) special-cases these math types as JSON arrays.
`glm::mat3` never needs JSON (derived / non-serialized).

### 3.2 Resources (`Arcane/Arcane/src/Arcane/Scene/SceneResources.hpp`)

```cpp
namespace Arcane
{
    struct SceneRoot { Astra::Entity entity; };           // registry resource; subtree IS the scene

    struct RenderContext2D                                 // set by the host each frame
    {
        Batcher2D* batcher = nullptr;
        glm::vec2  cameraOffset{0.0f, 0.0f};               // world->screen; world unit == canvas px for the slice
    };

    struct TextureTable                                    // thin id -> texture resolution seam
    {
        // textureId 0 reserved for "untextured". Full Assets integration deferred.
        std::unordered_map<uint32_t, nvrhi::ITexture*> textures;
        nvrhi::ITexture* Resolve(uint32_t id) const;       // nullptr when id==0 or unknown
    };
}
```

### 3.3 Systems

`Arcane/Arcane/src/Arcane/Scene/TransformSystems.hpp/.cpp`:

```cpp
struct TransformPropagationSystem
    : Astra::SystemTraits<Astra::Reads<LocalTransform>, Astra::Writes<WorldTransform>>
{
    void operator()(Astra::Registry& reg);
};
```

Implementation: read `SceneRoot` resource; compute the root's `WorldTransform` first
(`root.world = root.local.ToMatrix()`, no parent); then
`reg.GetRelations(root).ForEachDescendant([&](Astra::Entity e, size_t /*depth*/){ ... })`,
which yields BFS pre-order (parent visited before child, so the parent's `WorldTransform` is
already computed): `world(e) = world(parent(e)) * local(e).ToMatrix()`. Missing-component
guards skip non-spatial entities. Correctness hinges on the BFS order Astra's
`TraversalCache` guarantees (research §2 "the most important bespoke insight").

`Arcane/Arcane/src/Arcane/Scene/RenderSystems.hpp/.cpp`:

```cpp
struct RenderSubmissionSystem
    : Astra::SystemTraits<Astra::Reads<WorldTransform, SpriteRenderer>>
{
    void operator()(Astra::Registry& reg);
};
```

Implementation: fetch `RenderContext2D` + `TextureTable` resources (no-op if batcher null);
`reg.CreateView<WorldTransform, SpriteRenderer>().ForEach(...)`; per sprite derive screen
position from the world matrix translation + `cameraOffset`, set
`batcher->SetLayer(sortingLayer, orderInLayer)`, then submit:
`tex ? batcher->Quad(pos, size*worldScale, tex, {0,0}, {1,1}, tint)` else
`batcher->Rect(pos, size*worldScale, tint)`. Sort keys (layer/order) come straight from the
component, matching the renderer spec's sort-key contract. **This system is read-only and
runs in the Render phase; the host owns `Begin/End`, tonemap, and present.** It is M4's
sole render-phase system and Batcher2D submission is single-threaded — `Batcher2D` is not
thread-safe, and parallel command recording is a later renderer-spec milestone.

### 3.4 Registration + binary save/load (`Arcane/Arcane/src/Arcane/Scene/SceneModule.hpp/.cpp`)

```cpp
namespace Arcane
{
    // Registers scene component types into the registry's ComponentRegistry
    // (and, transitively, links their reflection meta -> the Phase 0 visitFields slot).
    void RegisterSceneComponents(Astra::Registry& reg);

    // Registers TransformPropagationSystem into schedulers.fixedUpdate and
    // RenderSubmissionSystem into schedulers.render.
    void RegisterSceneSystems(SystemSchedulers& schedulers);

    namespace Scene
    {
        Astra::Result<void, Astra::SerializationError>
            SaveBinary(const Astra::Registry& reg, const std::filesystem::path& path);   // -> Registry::Save
        Astra::Result<std::unique_ptr<Astra::Registry>, Astra::SerializationError>
            LoadBinary(const std::filesystem::path& path,
                       std::shared_ptr<Astra::ComponentRegistry> components);             // -> Registry::Load
    }
}
```

Binary save/load is one line each over `Registry::Save`/`Load` — the natural snapshot path
(and the future hot-reload path). This is the scene's **primary runtime persistence**.

### 3.5 Test (`Tests/src/TransformPropagationTest.cpp`, headless, Catch2)

- Build root + child + grandchild via `reg.SetParent`; set distinct local transforms; store
  `SceneRoot`; run `TransformPropagationSystem`. Assert each `WorldTransform` translation
  equals the composed parent chain (BFS-order correctness; a deliberately reversed insertion
  order proves order-independence of the result).
- `SaveBinary` then `LoadBinary`; assert transforms + parent relations survived.

---

## Phase 4 — Reflection -> JSON bridge (Arcane; the Phase 0 seam's first consumer)

Lives in `Arcane.dll` (`Arcane/Arcane/src/Arcane/Serialization/`); nlohmann/json is already
in the Arcane include path. This is what proves the format-agnostic seam serves the
north-star (human-editable / cross-language data) without Astra owning a format. The scene's
runtime persistence remains binary (Phase 3); JSON is the inspectable/editable peer.

### 4.1 `ReflectionJson.hpp/.cpp`

```cpp
namespace Arcane
{
    class ReflectionJsonWriter : public Astra::IFieldVisitor
    {
    public:
        explicit ReflectionJsonWriter(nlohmann::json& out);
        void Visit(const Astra::FieldInfo& field, void* instance) override; // out[name] = value
        bool IsWriting() const noexcept override { return true; }
    };

    class ReflectionJsonReader : public Astra::IFieldVisitor
    {
    public:
        explicit ReflectionJsonReader(const nlohmann::json& in);
        void Visit(const Astra::FieldInfo& field, void* instance) override; // value <- in[name | alias]
        bool IsWriting() const noexcept override { return false; }
    };
}
```

Value dispatch (writer/reader symmetric), by `field.typeHash` / trait flags:
- arithmetic (`bool`, integer widths, `float`, `double`), `std::string` via typed
  `field.Get<T>` / `field.Set<T>`;
- enums (`field.isEnum`): write the value's name via the type's `EnumInfo::ToString`, read
  via `EnumFromString` (looked up through `MetaRegistry` by `field.typeHash`);
- **glm math fallback** (special-cased by `typeHash`): `glm::vec2/3/4` as JSON arrays;
- **nested reflected struct:** look up `MetaRegistry::Instance().Get(field.typeHash)`; recurse
  over the sub-instance at `static_cast<std::byte*>(instance) + field.offset` into a nested
  JSON object;
- reader uses `field.ForEachAttribute<AliasName>` to fall back to old names; a missing key
  leaves the default-constructed value (forward/backward compatible by construction).

### 4.2 `SceneSerializer` JSON path (`SceneSerializer.hpp/.cpp`)

Scoped to the slice's component roster (LocalTransform + SpriteRenderer) + parent links —
**not** a general schema-driven scene format (Grimoire-era). Save: iterate scene entities
(`reg.GetRelations(root).ForEachDescendant` + the root), and per component use
`desc->visitFields(componentPtr, writer)` (component pointer + descriptor obtained via
`reg.InspectEntity(e)`), emitting `{ entities: [ { components: {...}, parent: <index> } ] }`.
Load (typed roster for M4): parse JSON, create entities, `AddComponent<LocalTransform>` /
`AddComponent<SpriteRenderer>`, drive `ReflectionJsonReader` to fill each, re-wire parents.
(Generic add-by-descriptor load is deferred — it needs a registry factory API.)

### 4.3 Test (`Tests/src/ReflectionJsonTest.cpp`, headless, Catch2)

- **Component round-trip:** a `SpriteRenderer` with non-default fields -> `ReflectionJsonWriter`
  -> nlohmann json -> `ReflectionJsonReader` into a fresh instance -> assert equality. Repeat
  for `LocalTransform` (covers the glm-array fallback).
- **AliasName:** write JSON under an old field name; load with a component whose field carries
  `AliasName("oldName")`; assert the value is recovered.
- **Scene round-trip:** build the slice scene, `SceneSerializer::SaveJson` then `LoadJson`;
  assert transforms + parents survived. Confirms the seam works end-to-end through a real
  external format with zero format knowledge in Astra.

---

## Phase 5 — Playground vertical slice + GPU verification

### 5.1 Slice (`Arcane/Playground/src/` — add a scene path, gated by `--scene`, default on)

Construct: `JobSystem` -> `EnkiWorkScheduler` -> `Simulation` (Registry + SystemSchedulers)
-> `RegisterSceneComponents` + `RegisterSceneSystems` -> build a tiny scene: a root entity, a
parent sprite, and a child sprite parented to it; a FixedUpdate lambda system orbits the
parent's `LocalTransform` (so propagation visibly moves the child). Keep the existing
M3 device/swapchain/canvas/batcher/tonemap/present plumbing. Per frame:

```
events -> input
alpha = runLoop.Advance(realDt)          // FixedUpdate (orbit + transform propagation) + Update
swapchain.BeginFrame(); commandList.open(); clear canvas
batcher.Begin(commandList, canvas.Framebuffer(), w, h)
reg.SetResource(RenderContext2D{ batcher.get(), cameraOffset })
runLoop.SubmitRender()                   // RenderSubmissionSystem -> batcher draws
batcher.End(); tonemap.Run(...); imgui (stats); commandList.close(); execute; Present()
```

`--frames N` exits 0 after N frames (existing convention) for scripted CI verification.
Textures stay `textureId == 0` (untextured tinted quads) so the slice needs no asset file.

### 5.2 `[gpu]` test (`Tests/src/SceneSliceTest.cpp`, Catch2, tagged `[gpu]`)

Headlessly drive the substrate for a few fixed steps, then render the scene through a
Batcher2D into an offscreen HDR canvas (the `GpuTestHelpers` pattern + the existing
`BatcherTest` offscreen setup): `Begin` -> set `RenderContext2D` -> `RenderSubmissionSystem`
-> `End` -> execute -> `waitForIdle`. Assert `Batcher2D::Stats().quads >= 2` (parent+child
submitted) and, per the foundation rule, `Arcane::RenderErrorCount() == 0` (NVRHI/VK
validation noise is a test failure). Excludable on GPU-less machines via `~[gpu]`; CI's
`windows-1` runs it.

---

## Testing strategy (summary)

| Test | Project | Tag | Asserts |
|---|---|---|---|
| `FieldVisitorTest` | AstraTest | — | seam enumerates serializable fields; map round-trip both ways; null slot for unreflected; AliasName |
| binary regression | AstraTest | — | existing `Serialization/*` + `RegistryTest` stay green |
| `JobSchedulerTest` | ArcaneTests | — | enkiTS ParallelForEach visits each element once; WorkerCount>=1 |
| `RunLoopTest` | ArcaneTests | — | fixed-step count vs wall time; maxStepsPerFrame clamp; alpha in [0,1) |
| `TransformPropagationTest` | ArcaneTests | — | BFS world-matrix correctness; binary save/load round-trip |
| `ReflectionJsonTest` | ArcaneTests | — | component + scene JSON round-trip; AliasName fallback |
| `SceneSliceTest` | ArcaneTests | `[gpu]` | quads submitted; `RenderErrorCount()==0` |
| Playground `--frames N` | Playground | — | scripted slice renders N frames, exits 0, both backends |

Build verification each phase: `msbuild Arcane.slnx /p:Configuration=Debug /m` and `Release`
(both backends where relevant); run `ArcaneTests.exe` from its output dir (the `Arcane.dll`
+ `shaders/` are copied there post-build).

## Roadmap notes (capture, do NOT build in M4)

- **M5 = Plugin host + Game.dll + Loom.exe + hot reload** (the architecture spec's old "M4").
  **Spec deviation to record there:** hot-reload state round-trips via `registry.Save(buffer)`
  / `Registry::Load` (a single integrated, CRC'd, versioned snapshot), NOT the per-component
  `Astra::BinaryWriter` loops the architecture spec's hot-reload sequence implied. The plugin
  ABI's `GamePlugin_SaveState(BinaryWriter&)` / `LoadState(BinaryReader&)` should wrap
  `registry.Save`/`Load` into/from that stream (research §1.4 "Spec assumption delta").
  M4's `Simulation` (engine-owned Registry) and `RunLoop` are the substrate M5 plugs into.
- **Engine-owned rendering:** M4 leaves GPU frame plumbing host-side; the render graph
  (renderer-spec M3.5/M4-adjacent) is what later lets `RunLoop` own render submission end to end.
- **UI runtime, Grimoire editor, LevelEditor:** later milestones (deferred).
- **North-star:** servers eventually meshed with Arcane -> one coherent ecosystem editing ALL
  client+server data in the engine editor via the reflection->JSON path. Keep Core / data /
  serialization boundaries clean to allow it; the Phase 0 seam is the first brick. The
  reflection->JSON bridge stays in `Arcane.dll` for M4 and is a candidate to promote into
  `Arcane.Core` when server-side data editing lands.
- **Astra scene capability:** `SceneGraph` wrapper (Option B), multi-scene/streaming, prefab
  instantiation, and `ParallelForEachDescendant` propagation are deferred until Grimoire's
  LevelEditor demands them (research §3 Option B).
- **The Lua client is a prototype/oracle only** — reimplement AAA / maintainable / modular;
  do not port its structure into the C++ engine.

## Constraints (carried forward)

- **/MD** (dynamic CRT) across the whole Arcane workspace; **no `/fp:fast`** (determinism);
  UTF-8 without BOM; ASCII in comments. C++23 in Arcane; Astra stays C++20-compatible.
- New engine components are `ASTRA_REFLECT`-annotated from day one (design rule).
- `GenerateProjects.bat` hangs on a `pause`; run `ThirdParty\premake5\premake5.exe vs2026`
  directly (Arcane workspace) / `scripts\generate_vs2022.bat` for Astra.
- Build with full msbuild path / `premake5.exe` directly; never `db-reset` / `clean --deep` /
  `docker compose down -v`; never run `tauri dev`. Commit trailer:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## Out of scope (deferred to their own designs)

Plugin host & hot reload (M5); physics port (M6); Grimoire/LevelEditor (M7); render graph,
bindless, batcher v2, materials, 2D lighting/GI, post stack (renderer-spec later milestones);
UI runtime; full Assets-backed sprite textures; generic schema-driven scene JSON for arbitrary
component rosters; server workspace merge / Core promotion of the JSON bridge; macOS/mobile.
