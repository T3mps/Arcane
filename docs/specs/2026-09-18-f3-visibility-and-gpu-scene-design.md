# F3 — visibility and the GPU scene: world bounds, frustum culling, a GPU-driven mesh pass, draw ordering

**Date:** 2026-09-18
**Status:** Design, approved in brainstorm 2026-09-18. Step 2 of the binding
order (`docs/research/2026-09-16-direction-and-sequencing.md`: F4 → **F3** → F5);
F4 closed at `e95de920` (2026-09-18). Implementation plans follow this spec
(§12): two plans.
**Charter:** the pivot's F3 line (`docs/research/2026-08-21-3d-foundations-assessment.md`
§F3: per-mesh AABBs, world bounds, frustum culling, draw sorting — "the graph
does not reorder, cull") — **widened by the user 2026-09-18** to every world
drawable (meshes AND sprites) through one bounds path, and to a **GPU-driven
mesh pass** (a persistent GPU instance scene, a compute cull, indirect draws)
with a CPU per-view coarse stage in front of it. G2 of the direction doc
(TAA's frame contract) is co-owned here: this spec names the prior-pose source
the replication spec found stale.
**Prior law this builds on:** F1 (`Transform`/`WorldTransform`, the
`Changed<Transform>` dirty pre-pass in `TransformSystems.hpp`); F2a
(`MeshRenderer`, `ComputeMeshBounds`, `MeshBounds`); F2b/F2c (`.arcmesh`
artifact AABB, `NriMeshBufferCache` residency, the bindless material slot,
per-section `MeshInstance`); F4 (`ViewTransform`, `SelectionFramingBounds` /
`SceneFramingBounds`, world-space sprite vertices, the id-buffer pick pass);
the Astra adoption (`Changed<T>` + `Since(tick)`, the registry-swap rule).
**Standing rules obeyed:** every design states what Source 2 / Deadlock does
and why we match or diverge (§10); the greedy-ordering guards G1–G3 are
written into the seams (§8).

---

## 0. In one paragraph

Every world drawable gets an engine-written **`WorldBounds`** (a world-space
AABB, produced after `TransformSystems` from the mesh artifact's AABB or the
sprite's world quad). A **`Frustum`** is extracted from `ViewTransform`, and a
CPU **`VisibleSet`** per view — the coarse stage — is what the sprite sweep,
the pick pass and the editor consume. Meshes go further: every `(entity,
section)` is a **row in a persistent GPU instance buffer** (the GPU scene:
model, previous model, bounds, material), kept current by dirty-tracked uploads
with stable slots; a **compute cull** tests every resident row against the
frustum and appends survivors into per-batch lists, writing `instanceNum` into
**indirect draw arguments**; the mesh pass draws one indirect call per batch,
batches ordered opaque → masked, nearest-first. Materials gain **opaque /
masked / transparent**; transparent rows bypass the cull and are drawn last,
CPU-sorted back-to-front. `prevModel` on the row is the named prior-pose
source for TAA's motion vectors (G2); the velocity target and the depth
prepass are declared pass slots nobody mints yet.

---

## 1. What the survey found (2026-09-18, Arcane @ `e95de920`)

| Fact | Where |
|---|---|
| Bounds exist only per asset: `MeshBounds{min,max}`, computed by `ComputeMeshBounds` / stored in the `.arcmesh` artifact, published on `MeshEntry::bounds` | `ArcaneCore/src/Arcane/Mesh/MeshBuilder.hpp:110`, `Scene/SceneResources.hpp:190` |
| World AABBs ARE computed today — in the editor, for framing only: mesh = eight local corners through the world matrix; sprite = `SpriteWorldQuad` | `ArcaneEditor/src/Viewport/EditorCamera.hpp:98-211` (`FramingBounds`, `SelectionFramingBounds`, `SceneFramingBounds`) |
| No culling anywhere. `CollectMeshInstances` emits every non-Hidden `(WorldTransform, MeshRenderer)` in registry order; the graph "does not reorder, cull" | `Render/MeshSubmissionSystem.hpp`, `Render/Nri/RenderGraph.hpp:33` |
| `Batcher2D` sorts sprites by `sortingLayer` only; no sprite is ever skipped for being off-screen | `Render/RenderSystems.hpp:154`, `Batcher2D.hpp` |
| No draw sorting: `MeshNode::Record` walks `scene.instances` in submission order, one 128-byte root-constant block (`MeshConstants`: model + tint + normal matrix + packed slot) per draw | `Render/Nri/nodes/MeshNode.cpp:787,950`, `MeshNode.hpp` (THE MATERIAL SLOT block) |
| No transparent mesh path: the mesh pipeline is fixed opaque + back-face cull; the `.arcmat` mesh kind carries no blend mode | `MeshNode.hpp` WINDING AND CULLING, `Material/MaterialSource.hpp:71` |
| `ViewTransform` has view/projection/`IsOrthographic()`, no frustum | `ArcaneCore/src/Arcane/Scene/ViewTransform.hpp:50` |
| `PreviousTransform` is gone (Astra adoption Plan 2, ABI 27); `PhysicsInterpBuffer` is physics-only. G2's "already carries the data" is stale | `2026-09-17-replication-v1-design.md` (the stale-line note), `Scene/SceneResources.hpp:85` |
| Astra has change detection: `Changed<T>` views with `Since(tick)`; `TransformSystems` already runs a `Changed<Transform>` pre-pass | `Scene/TransformSystems.hpp:28-104,341` |
| NRI exposes `CmdDrawIndexedIndirect` (optional count buffer, cap `drawIndirectCount`) and `CmdDispatchIndirect`; `DrawIndexedDesc{indexNum, instanceNum, baseIndex, baseVertex, baseInstance}` is the arg layout on both backends | `ThirdParty/NRI/Include/NRI.h:197-204`, `NRIDescs.h:1659,2128` |
| Compute is dispatched only by the diagnostics self-test; F3 lands the engine's first production compute pass and first indirect draw | `Render/Nri/NriDiagnostics.cpp:757` |
| Mesh geometry is resident per Guid as one vertex/index buffer pair | `Render/Nri/NriMeshBufferCache.hpp:3,81` |
| Sections: one `MeshInstance` per `MeshData::sections` entry; material chain = override → slot → white | `MeshSubmissionSystem.hpp` PER SECTION |

---

## 2. Bounds

### 2.1 `Aabb`

`ArcaneCore/src/Arcane/Math/Aabb.hpp`, header-only, pure, no engine
dependencies:

```cpp
struct Aabb
{
    glm::vec3 min{0.0f}, max{0.0f};
    [[nodiscard]] static Aabb Empty() noexcept;            // the fold sentinel (+inf/-inf)
    [[nodiscard]] bool  IsEmpty() const noexcept;
    [[nodiscard]] glm::vec3 Center() const noexcept;
    [[nodiscard]] glm::vec3 Extent() const noexcept;       // half-size
    [[nodiscard]] Aabb  Union(const Aabb&) const noexcept;
    [[nodiscard]] Aabb  Transformed(const glm::mat4&) const noexcept;  // eight corners, conservative
    [[nodiscard]] Aabb  Widened(float epsilon) const noexcept;
};
```

`MeshBounds` (`MeshBuilder.hpp:110`) becomes `using MeshBounds = Aabb;`. The
default-constructed zero box stays the "framing an empty mesh" answer that
comment describes; `Empty()` is the fold sentinel and is never stored on a
component. No rename wave through the mesh code.

### 2.2 `WorldBounds`

A component in `Scene/Components.hpp` (ArcaneCore — a plugin-ABI bump, cheap):

```cpp
struct WorldBounds { Aabb box; };
```

**Engine-written, never authored:** not serialized (the scene loader neither
reads nor writes it; a stale box in a file is impossible), reflected with an
attribute the Inspector hides, and rebuilt from scratch on scene open. It is a
plain cache of `(drawable's local box) × WorldTransform`.

### 2.3 `BoundsSystem`

Registered by `Runtime::InstallEngineSystems` in the **`fixedUpdate`
scheduler with `Astra::After<TransformPropagationSystem>`** (it reads
`WorldTransform`; engine-owned, Core, behind the same `HasSystem` guard as its
two siblings). Dirty-driven, the `TransformSystems` idiom: the rows visited are the union of `Changed<WorldTransform>`,
`Changed<MeshRenderer>`, `Changed<SpriteRenderer>` since its last run, plus
every entity that has no `WorldBounds` yet, plus the removal reconciliation
(a `WorldBounds` whose entity lost its renderer is removed). A static scene
does no work.

Producers, in priority order when both components are present (the pick pass's
mesh-beats-sprite rule):

| Component | Local box | Notes |
|---|---|---|
| `MeshRenderer` whose Guid resolves in `MeshTable` | `MeshEntry::bounds` (the artifact's stored AABB; `ComputeMeshBounds` for primitives) | Per entity, not per section — one box for all sections |
| `MeshRenderer` that does not resolve | **no `WorldBounds`** | Not drawable today either; the mesh pass skips it |
| `SpriteRenderer` | the AABB of `SpriteWorldQuad`'s four corners (the identical quad the batcher draws; 1×1 m unresolved), Z widened by `kSpriteDepthEpsilon` (1 mm) | A zero-thickness box is a degenerate frustum input; the epsilon makes it a box |
| neither | no `WorldBounds` | Bare transform nodes do not cull, frame, or pick |

The editor's `SelectionFramingBounds` / `SceneFramingBounds` become consumers:
each is `Union` over the entities' `WorldBounds` (selection: the given set,
falling back to the position as a zero-extent point for a bare node, as
today; scene: every non-Hidden `WorldBounds`). The eight-corner and
`SpriteWorldQuad` code they carry today is deleted. Framing, culling and
picking now read one box.

**Seams named, not built:** a per-entity bounds override (skinned meshes,
particle systems — the animation arc adds `BoundsOverride` when it has a
consumer); bounds of the physics debug draw and gizmo (foreground chrome, never
culled).

---

## 3. Frustum

`ArcaneCore/src/Arcane/Scene/Frustum.hpp` (beside `ViewTransform.hpp`):

```cpp
struct Plane { glm::vec3 n; float d; };            // n·p + d >= 0 is inside
struct Frustum
{
    std::array<Plane, 6> planes;                    // L R B T N F, normalised
    [[nodiscard]] static Frustum FromViewProjection(const glm::mat4& vp) noexcept;  // Gribb–Hartmann
    [[nodiscard]] static Frustum From(const ViewTransform&) noexcept;
    [[nodiscard]] bool Contains(const Aabb&) const noexcept;  // p-vertex test; conservative
};
```

The orthographic case needs no branch: the same row extraction on
`orthoRH_ZO` yields six planes. `Contains` is **conservative by contract**: a
box that intersects the frustum is never rejected (the p-vertex test has
false positives at corners, never false negatives) — the property the
rapidcheck test pins (§9). The same six planes, as six `float4`, are the
compute cull's constant buffer (§5.3); the CPU and GPU tests read one source.

**The jitter rule (G2):** the frustum is always extracted from the
**unjittered** `ViewTransform`. When F5 adds TAA's sub-pixel jitter to the
projection, the culling frustum must not follow it — a jitter that flips a
culling decision frame to frame is a flicker. F5 states this again on its side.

---

## 4. The CPU coarse stage — `VisibleSet`

```cpp
struct VisibleEntry { Astra::Entity entity; Aabb box; float nearDepth; };  // nearDepth: view-space distance of the box's nearest point, >= 0
struct VisibleSet
{
    ViewTransform            view;
    Frustum                  frustum;
    std::vector<VisibleEntry> entries;
    std::vector<std::uint64_t> members;          // bitset by entity index; O(1) membership for the sweeps
    [[nodiscard]] bool Contains(Astra::Entity) const noexcept;
};
struct SceneVisibility { std::vector<VisibleSet> views; };   // index 0 = the main view; a registry resource
```

`BuildVisibleSet(Astra::Registry&, const ViewTransform&, VisibleSet& out)` is
a **free header-only function** in the `CollectMeshInstances` idiom
(`Render/VisibilitySystem.hpp`, ArcaneClient): it clears `out`, extracts the
frustum, walks every `(WorldBounds, !Hidden)` linearly and appends the
survivors. Device-free, testable in ArcaneTests under `~[gpu]`.

It is a **registry resource** (`SceneVisibility`) rather than a host-local
because `RenderSubmissionSystem` is an `Astra::System` and must read it. The
stage boundary already exists: `TransformPropagationSystem` and `BoundsSystem`
run in the `fixedUpdate` scheduler, `RenderSubmissionSystem` in the separate
`render` scheduler (`Sim/SystemSchedulers.hpp:19-24`), and the host runs them
as stages. The host builds `views[0]` **between the two** — after the last
fixed/update tick of the frame, before the render scheduler — from the
viewport's `ViewTransform` (`ClientRuntime::View()`).

**The interpolation slack, stated:** `WorldBounds` is the fixed-step pose;
the sprite sweep renders a pose interpolated toward it from the previous
step (`PhysicsInterpBuffer`), so a fast sprite at the screen edge can be a
fraction of one step outside its box. `BuildVisibleSet` therefore tests
against the frustum with its planes pushed outward by `kVisibilitySlack`
(0.25 m, a named constant beside the sprite epsilon): conservative, one
constant, and it absorbs the case without per-entity displacement tracking.
The GPU cull (§5.5) uses the same widened planes so the two stages agree.

Consumers in F3:

| Consumer | Change |
|---|---|
| `RenderSubmissionSystem` (sprites) | `if (!vis.Contains(e)) return;` at the top of the sweep. Layer sorting unchanged |
| `PickEmit` | the sprite and mesh walks emit members only; collider shapes (physics debug pickables) are unchanged — they carry no `WorldBounds` |
| The GPU draw-list builder (§5.4) | batch emission and ordering read `entries` |
| `SceneFramingBounds` | does NOT read the set (Frame All frames what exists, not what is on screen) |

**No spatial structure.** Linear over `WorldBounds`; the trigger is stated:
when a measured scene shows `BuildVisibleSet` above **0.5 ms** on the dev
machine, a BVH/grid arc opens. Multi-view: `views` is already a vector; F3
fills index 0 only (Play and Edit alike — the viewport's `ViewTransform`).
Shadow views (T1/T5) and document previews append.

---

## 5. The GPU scene

### 5.1 The row

One row per **(entity, mesh section)** — the same granularity `MeshInstance`
has today, because the material travels per section. `GpuInstance`, 192 bytes,
std430, mirrored by a `static_assert(sizeof == 192)` and an HLSL struct in
`data/shaders/gpu_scene.hlsli` that `mesh.hlsl` and `mesh_cull.hlsl` share:

| Field | Type | Bytes | Written by |
|---|---|---|---|
| `model` | `float4x4` | 64 | Sync (dirty) |
| `prevModel` | `float4x4` | 64 | the GPU row-writer (from the row's old `model`) |
| `boundsMin` | `float4` (w unused) | 16 | Sync |
| `boundsMax` | `float4` (w = `alphaCutoff` for masked rows) | 16 | Sync |
| `baseColor` | `float4` | 16 | Sync |
| `materialSlot` | `uint` | 4 | Sync |
| `batch` | `uint` | 4 | Sync (see 5.4: the batch KEY id, stable per (mesh, section, blend)) |
| `flags` | `uint` | 4 | bit0 `teleported` (reserved, never set in F3); bits1–2 blend mode (0 opaque, 1 masked, 2 transparent) |
| `pad` | `uint` | 4 | 0 |

The rows live in **one persistent structured buffer** owned by `GpuScene`
(`Render/Nri/GpuScene.{hpp,cpp}`, a member of `NriGraphContext` beside the
mesh-buffer cache), grown by doubling; the old buffer is buried in the
`Graveyard` with the frame fence, the `NriMeshBufferCache` discipline. The
normal matrix is **computed in the vertex shader** from `model`
(`NormalMatrixFor` stays in C++ for tests and tools); the 128-byte
`MeshConstants` root block is retired.

### 5.2 Slot identity

`GpuSceneMirror`, a registry resource (scene side):

```cpp
struct GpuSceneMirror
{
    struct Rows { std::uint32_t first; std::uint32_t count; };   // one row per section
    Astra::FlatMap<Astra::Entity, Rows> slots;
    std::vector<std::uint32_t> freeList;                           // single rows; a multi-section entity takes `count` consecutive rows
    std::uint32_t rowCount = 0;
    std::uint64_t generation = 0;                                  // fresh per registry
    std::uint64_t lastSyncTick = 0;
};
```

The device-side `GpuScene` stamps the `generation` it last synced. A registry
swap (`RestoreRegistry`, scene open, Play → Edit) yields a mirror with a new
generation (the resource is constructed fresh with the registry) → the next
sync sees the mismatch and does a **full rebuild**: every row re-uploaded,
`prev = model` everywhere. That is the registry-swap rule applied here: the
swap never touches the host's mode or the device; it invalidates a stamp.
The mirror is NOT serialized (transient resource; `RestoreRegistry` strips
it, per the Astra resource-serialization lesson).

### 5.3 `GpuScene::Sync` — once per frame, before the graph is declared

`Sync(reg, meshTable, matTable, meshBufferCache, uploadRing)`, host-called
after the scene schedule (so `WorldBounds` is current):

1. **Reconcile.** Walk the `(WorldTransform, MeshRenderer, WorldBounds, !Hidden)`
   view against the mirror: entities absent from the mirror allocate rows
   (`count` = the resolved mesh's section count); mirror entries whose entity
   is gone, Hidden, or no longer resolves free their rows. A section-count
   change (mesh reassigned) frees and reallocates.
2. **Dirty rows** = rows of entities in `Changed<WorldTransform>` ∪
   `Changed<MeshRenderer>` since `lastSyncTick` ∪ rows allocated in step 1 ∪
   rows whose material resolution changed (the resolver's per-frame refresh
   marks the `MeshMaterialTable` generation; a changed generation dirties every
   row — rare, cheap). The dirty row's full `GpuInstance` (minus `prevModel`)
   is staged through the upload ring together with a `{row, isNew}` list.
3. **The row-writer dispatch** (`gpu_scene_write.hlsl`, one thread per staged
   row) copies the staged fields into the persistent buffer and sets
   `prevModel = isNew ? staged.model : existing.model`. **No CPU copy of any
   matrix is kept.**
4. **The advance dispatch** runs over the *previous* frame's dirty list
   (kept by row index, CPU side, one `uint` each) and sets `prevModel = model`
   for rows that were NOT re-dirtied this frame — so a row moved on frame N
   has `prev ≠ model` on frame N (velocity) and `prev == model` on frame N+1
   (at rest). Rows dirty two frames running are written by step 3 alone.
   Static rows cost nothing in either dispatch.

The G2 contract, for the record: **a row's `prevModel` is the `model` it was
drawn with on the previous frame; a new row, a rebuilt scene, or a
`teleported` row has `prev == model`.** `MeshRenderer` gains no field for
`teleported` in F3; the bit is named in the layout and left unset.

Both dispatches are recorded by `GpuSceneSyncNode` (§8), reading the ring
slice the host staged.

### 5.4 Batches — CPU per frame, from the mirror

Key = `(mesh Guid, section index, blend mode)`. The row's `batch` field holds
a stable **key id** (a `FlatMap<key, id>` in `GpuScene`, ids reused on free).
Per frame the CPU builds:

- **The batch table** — for every key with ≥ 1 resident row: `capacity` (the
  count of resident rows with that key, known from the mirror without
  visibility), `firstOutput` = the exclusive prefix sum of capacities in key-id
  order. Uploaded through the ring; `GpuBatch { uint firstOutput; uint capacity; uint emitted; uint argIndex; }` (`argIndex` = the batch's position in the emitted list, meaningful only when `emitted`).
- **The emitted list** — keys with at least one row whose entity is in
  `SceneVisibility.views[0]` (the coarse pass prunes whole batches; a batch
  entirely off-screen never reaches the cull or the draw), **excluding
  transparent keys** (§7). Ordered: **opaque before masked**, and within each
  by the minimum `nearDepth` of the batch's coarse-visible entities,
  ascending (nearest first — the early-Z key). The order is a CPU sort over
  tens of batches.
- **The indirect args array** — one `nri::DrawIndexedDesc` per emitted batch,
  in emitted order: `indexNum = section.indexCount`, `baseIndex =
  section.indexOffset`, `baseVertex = 0` (a resident mesh's buffers are its
  own — §1), **`instanceNum = 0`**, `baseInstance = 0`. Uploaded through the
  ring into a transient the cull writes and the draw reads.

### 5.5 `MeshCullNode` — the compute cull

`mesh_cull.hlsl`, one thread per resident row (dispatch = ⌈rowCount / 64⌉):

```
row = instances[id];  b = batches[row.batch];
if (!b.emitted) return;                                  // pruned by the coarse pass, or transparent
if (!FrustumContains(row.boundsMin.xyz, row.boundsMax.xyz, planes)) return;   // the p-vertex test, same six planes
n = InterlockedAdd(args[b.argIndex].instanceNum, 1);
visibleIndices[b.firstOutput + n] = id;
```

Inputs: the instance buffer (SRV), the batch table, the frustum CB (six
`float4` = `Frustum::planes`, already widened by `kVisibilitySlack`, §4). Outputs: `visibleIndices` (`uint` × rowCount,
partitioned by batch capacity) and the args buffer (UAV). The graph declares
the args buffer's transition to the indirect-argument state as an edge into
the draw node.

The GPU test is **the fine stage**: in F3 it repeats the frustum test the
coarse pass already did per entity (harmless — the coarse pass prunes
batches, the cull prunes rows), and it is the slot Hi-Z occlusion fills (F5's
shared depth → a depth pyramid → one more predicate here) without touching
the CPU side.

### 5.6 `MeshNode` — the rewritten draw

Per emitted batch, in order: bind the section's mesh's resident vertex/index
buffers, bind the batch's pipeline variant (§6), push **one 8-byte root
constant** `{ uint firstOutput; uint flags; }` (`flags` bit0 = `direct`, §7),
`CmdDrawIndexedIndirect(args, argIndex * sizeof(DrawIndexedDesc), 1,
sizeof(DrawIndexedDesc), nullptr, 0)`. The vertex shader:

```
row  = direct ? firstOutput : visibleIndices[firstOutput + SV_InstanceID];
inst = instances[row];
```

then `model`, the normal matrix from `model`, `baseColor`, `materialSlot`
exactly as `MeshConstants` carried them. A zero-`instanceNum` batch is a
no-op draw; **no count buffer**, so the `drawIndirectCount` cap is not
required and `baseInstance` is never used (no dependence on NRI's
draw-parameters emulation). Pipeline layout: root constants b0 (8 bytes),
the root sampler s0, space1 = `{ b1 frame CB, t0 instances, t1 visibleIndices }`,
space2 = the bindless material array — the register-space rule from
`MeshNode.hpp` unchanged in kind.

`MeshSceneDesc` (in `FrameDesc`) loses `instances` and gains
`const GpuSceneFrame*` — `{ emitted batches (ordered), args (ring slice),
transparent rows (ordered, §7), frustum, rowCount }`, host-owned for the
duration of `RenderFrame` exactly as the span was. `CollectMeshInstances` is
retired; its tests move to the batch builder and `Sync`'s reconciliation,
both split from the device writes so ArcaneTests covers them under `~[gpu]`.

---

## 6. Materials — opaque, masked, transparent

The `.arcmat` **mesh kind** gains:

| Field | Values | Default |
|---|---|---|
| `blend` | `"opaque"`, `"masked"`, `"transparent"` | `"opaque"` |
| `alphaCutoff` | float in [0, 1] | 0.5 (meaningful for masked only; stored regardless so a mode switch keeps it) |

Source 2 carries the trio as `F_ALPHA_TEST` / `F_TRANSLUCENT` on the material;
UE as Blend Mode Opaque / Masked / Translucent. `MaterialSource` parses them
into `enum class MaterialBlendMode : uint8_t { Opaque, Masked, Transparent }`;
an unknown string **refuses** with the field named (the loader's posture).
`ResolvedMeshMaterial` (Core, backend-agnostic) carries `blend` and
`alphaCutoff` beside `baseColor` / `albedo`; `Sync` copies them into the row's
`flags` / `boundsMax.w`. `MaterialDocument`'s mesh panel gets the dropdown and
a bounded `alphaCutoff` drag (the F2a "unauthorable invalid state" idiom).

**Pipeline variants** — `NriPipelineCache`'s key gains the blend mode:

| Variant | Depth | Blend | Cull | Shader |
|---|---|---|---|---|
| Opaque | test + write | off | back | as today |
| Masked | test + write | off | back | `MESH_MASKED` define: `clip(alpha - cutoff)` |
| Transparent | test, **no write** | `SrcAlpha / InvSrcAlpha`, add | **none** (a thin shell wants both faces) | as opaque, alpha through |

Alpha = `baseColor.a × albedo.a` — the first time `baseColor.a` means
anything; opaque rows ignore it as they do now. The mesh shader's flat path
(`kInvalidSlot`) keeps `albedo.a = 1`.

---

## 7. Draw order across the frame

1. **Opaque batches**, nearest-first by batch (§5.4), indirect.
2. **Masked batches**, same order, after every opaque batch: a `clip` disables
   early-Z for that draw, so masked geometry goes last among the depth
   writers.
3. **Transparent rows**, direct: the CPU takes the `VisibleSet` entries whose
   entity has transparent rows (the mirror knows the rows; the material knows
   the mode), sorts them **back-to-front by the box centre's view-space
   depth**, and issues one `CmdDrawIndexed` per row on the transparent
   pipeline with the root block `{ row, direct=1 }`. They never enter the
   cull: their `batch` is not emitted (§5.5), and the CPU coarse test is their
   whole culling.
4. `GridNode` stays after the mesh pass, depth-tested against the same depth,
   as F4 left it; the sprite batch, the post chain, the tonemap and the
   overlay are unchanged. Sprites-over-meshes and shared depth remain F5's.

---

## 8. Graph wiring and the declared slots

`DeclareGraphFrame` (`NriGraphContext.cpp`) gains, between `batch2d` and the
mesh draw:

```
batch2d → GpuSceneSyncNode (row-writer + advance dispatches) → MeshCullNode → MeshNode (indirect + direct) → GridNode → post → tonemap → …
```

Three node types (`GpuSceneSyncNode`, `MeshCullNode`, the rewritten
`MeshNode`), bringing the `Nri/nodes/` tree to **nine pass types**; the
domain-reorg trigger (10+, `project_arcane_render_pass_domain_reorg`) is
re-counted and stated at the close, not pulled. `RenderGraph.hpp:33` is
reworded: the graph still does not reorder; **culling is a node**, not a
graph property.

**Declared pass slots — the G1/G2 seams, written here so F5 builds on them:**

| Slot | Position | Minted by | Written by |
|---|---|---|---|
| `prepass` (depth-only opaque) | before the mesh draw | nobody in F3 | Hi-Z (T7) or F5 if it chooses; its value scales with overdraw and shading cost, neither present |
| `velocity` (RG16F) | beside the mesh draw's colour/depth | nobody in F3 | F5 mints it with the shared depth; T5 writes it from `model`/`prevModel` |
| `sharedDepth` | the mesh pass's D32 | F4's `MeshNode` (today) | F5 makes it the frame's one depth |

The full list F5 inherits: `prepass → G-buffer → lighting → forward/masked →
transparent → 2D-world → post → overlay` (G1). F3 implements the forward,
masked and transparent slots only.

**Debug switch:** a compile-time `kMeshCullEnabled` (default on) that makes
the cull pass every emitted row (`instanceNum = capacity`, identity indices)
— for bisecting a wrong-picture report. The cvar arc makes it runtime.

---

## 9. Testing and verification — the definition of done

In order of authority:

1. **The oracle test (`[gpu]`, `GpuSceneCullTest.cpp`).** A fixture scene:
   rows fully inside, fully outside, straddling each of the six planes, at
   every blend mode, several sections per mesh, two meshes. After one frame,
   read back the args' `instanceNum` per batch and the `visibleIndices`
   contents; they must equal the CPU `VisibleSet`'s answer for the same rows
   (set-equal per batch — the atomic order is arbitrary). Runs on D3D12 and
   Vulkan. **The CPU pass is the oracle by construction.**
2. **Witness.** `VerifyReport` gains `visibility { coarseVisible, total,
   gpuVisible, batches, draws }`; the servitor lanes assert them for
   ReferenceProject's 3D scene (exact for the first four, `draws ==
   batches + transparentRows`).
3. **Goldens.** `editor-ui-perspective` must not move (nothing in it is
   off-screen). One new golden pair: a straddling mesh still draws in full
   (conservative culling), and a transparent mesh in front of an opaque one
   blends in the right order.
4. **Unit (`~[gpu]`).** `Aabb` (union, transform conservativeness under
   random affine — rapidcheck); `Frustum` (ortho and perspective extraction
   against hand-built planes; the conservative property: a random box
   intersecting the frustum is never rejected — rapidcheck); `BoundsSystem`
   producers (mesh, sprite epsilon, unresolved mesh → no bounds, removal);
   `BuildVisibleSet` membership and `nearDepth`; batch emission and ordering
   (opaque → masked, depth ascending, transparent never emitted); the mirror
   (allocate, free, multi-section, generation reset → full rebuild); the
   dirty list (moved once → dirty on N, advanced on N+1); the transparent
   sort.
5. **Desk pass.** Orbit a scene with meshes leaving and entering the view and
   watch the stats line; toggle a material through the three modes; the
   Inspector shows no `WorldBounds`; Play → Edit round trip leaves every
   mesh drawn (the generation reset); Aphelyon's module rebuilds against the
   new ABI.

---

## 10. Comparison (the standing rule)

| Source 2 / Deadlock, UE | Arcane F3 | Why |
|---|---|---|
| Source 2: world AABB on every scene object; CPU per-view frustum + precomputed voxel visibility; per-view render lists | `WorldBounds` on every drawable; CPU `VisibleSet` per view; no voxel vis | Match on the CPU layer. Voxel vis is a bake-time structure (T7) |
| CS2/Deadlock: GPU-side cull + draw for *aggregate* static props only; ordinary objects CPU-culled | GPU scene + compute cull for **every** mesh | Diverge: no aggregate concept exists and a solo tree wants one mesh path; aggregates become a batching optimisation on top when T7 asks |
| Deadlock: depth-pyramid GPU occlusion | not in F3; the predicate slot in `MeshCullNode` is where it lands | Needs the prior frame's depth = F5's shared-depth contract; building the handshake before F5 builds it twice |
| UE: `FPrimitiveBounds` + `ComputeViewVisibility` (CPU) → `GPUScene` (persistent, dirty-tracked, `PreviousLocalToWorld`) → `InstanceCullingManager` → indirect draws | the same two-layer shape, one row per section | Match |
| UE base pass: sort by PSO then mesh; depth prepass supplies early-Z | batches sorted opaque → masked, nearest-first; no prepass | Diverge for now: prepass value scales with overdraw and shading cost; declared slot |
| UE translucency: CPU-sorted back-to-front, separate pass | the same | Match |
| UE / Source 2: Opaque / Masked / Translucent (`F_ALPHA_TEST`, `F_TRANSLUCENT`) | the same trio | Match |
| UE Paper2D sprites: CPU everything | sprites CPU-culled through `VisibleSet`, drawn by `Batcher2D` as today | Match; sprites join the GPU scene only on a measured 2D-perf trigger |
| UE `GPUScene`: per-instance `bCastShadow`, LOD, custom data | none | YAGNI; the row has a `pad` and a `flags` word |

---

## 11. G2 and the greedy-ordering guards, restated

- **G1** — the pass-slot list is declared (§8); F3 implements forward/masked/
  transparent only.
- **G2** — the prior pose is `GpuInstance::prevModel` (§5.3), the velocity
  target is a declared slot (§8), the culling frustum is unjittered (§3).
  The direction doc's "`PreviousTransform` already carries the data" is
  superseded by this spec; no ECS history component is recreated.
- **G3** — untouched here (the hygiene wave's artifact stamp); the `.arcmat`
  gains two fields with defaults, so existing materials load unchanged.

---

## 12. Plans

1. **Plan 1 — bounds, visibility, and the GPU scene without the cull.**
   `Aabb`; `WorldBounds` + `BoundsSystem`; `Frustum`; `VisibleSet` /
   `SceneVisibility` + `BuildVisibleSet`; the sprite sweep and `PickEmit`
   consume it; the editor framing lift; `GpuSceneMirror`, `GpuScene` rows /
   slots / `Sync` / the row-writer and advance dispatches; the batch builder;
   the rewritten `MeshNode` drawing every emitted batch **indirect with
   `instanceNum` written by the CPU** (`= capacity`, identity indices — the
   indirect path is proven before the compute pass exists); `MeshSceneDesc`
   → `GpuSceneFrame`; `CollectMeshInstances` retired; ABI bump; unit tests
   (§9.4); goldens unchanged; the witness counts (GPU figure = coarse figure
   until plan 2).
2. **Plan 2 — the cull, the modes, the order.** `MeshCullNode` and
   `mesh_cull.hlsl`; the oracle test on both backends; `blend` /
   `alphaCutoff` through `.arcmat` → `MaterialSource` →
   `ResolvedMeshMaterial` → the row; the three pipeline variants; masked and
   transparent order; the stats overlay line and readback ring; the new
   golden pair; the declared slots and the `RenderGraph.hpp` rewording;
   `kMeshCullEnabled`; the desk pass; the pass-type recount.

---

## 13. Non-goals and seams

| Out | Owner / trigger |
|---|---|
| Hi-Z / occlusion culling | F5's shared depth, then T7; the predicate slot in `MeshCullNode` |
| Sprites in the GPU scene (instanced quads, GPU-culled) | a 2D-perf arc, on a measured trigger; `Batcher2D` keeps its shape |
| A spatial index for `BuildVisibleSet` | `> 0.5 ms` measured |
| GPU sort of the visible list | never needed while batches are ordered on the CPU; a prepass beats it at scale |
| The depth prepass | declared slot; Hi-Z or F5 |
| Multi-view visible sets beyond index 0 | shadow views (T1/T5), previews |
| `BoundsOverride` (skinned, particles) | the animation arc |
| LOD, per-instance shadow flags, custom instance data | T1+ (the row has `flags`/`pad`) |
| `teleported` on `MeshRenderer` | the animation / gameplay arc that first teleports something |
| Runtime cull toggles | the cvar arc (`kMeshCullEnabled` is compile-time) |
| Aggregate / merged static batches | T7 |
| Sprite depth, 2D-over-3D, the clear-op, jitter | F5 |

---

## 14. Rulings ledger (2026-09-18)

| # | Ruling | Rejected |
|---|---|---|
| R1 | Meshes **and** sprites through one engine-owned bounds path (`WorldBounds`); the editor's framing becomes a consumer | meshes only; a declared sprite seam |
| R2 | **GPU-driven** for meshes: persistent GPU scene, compute cull, indirect draws — with a CPU per-view coarse stage in front as the one seam for sprites, picking, framing and tests | inline per-sweep culling; a CPU-only `VisibleSet` stage |
| R3 | GPU scene for meshes + CPU coarse (UE's shape); sprites stay CPU; Hi-Z declared, not built | a full GPU scene including sprites; Hi-Z now |
| R4 | Opaque early-Z from **batch state-sort + CPU nearest-first by batch**; the depth prepass is a declared slot | a prepass now; a GPU depth sort |
| R5 | Build the transparent path **and** masked: `blend` on the mesh material, three pipeline variants; transparent rows CPU-sorted back-to-front, direct draws after the indirect batches | reserve only |
| R6 | Prior pose = `GpuInstance::prevModel`, maintained on the GPU by the row-writer and the advance dispatch; no ECS history component | `PreviousWorldTransform` + a system; defer to F5 |
| R7 | Persistent slots with dirty-tracked uploads (`Changed<>` + reconciliation); a registry generation stamp forces the full rebuild | full re-upload every frame; transient per-frame rows |

---

## 15. Costs and risks, stated plainly

- **`MeshNode` is rewritten, not extended.** The 128-byte root block, the
  per-draw `NormalMatrixFor` push and the `instances` span all go. Plan 1
  proves the indirect path with CPU-written counts before any compute exists,
  so a wrong picture bisects to one of two halves.
- **First production compute pass and first indirect draw.** A D3D12-vs-Vulkan
  disagreement on the args layout, the UAV → indirect-argument barrier, or
  structured-buffer packing is the real risk; NRI abstracts all three and the
  oracle test runs on both backends. `static_assert`s pin the row size and
  the `DrawIndexedDesc` stride against the HLSL side.
- **`Sync`'s prev-advance is the subtle piece** (a row moved on N must be at
  rest on N+1). It has its own unit test and the G2 contract sentence in §5.3
  is the thing T5 will read.
- **The registry-swap generation** must fire on every swap path (scene open,
  `RestoreRegistry`, Play → Edit, the module hot-reload's registry rebuild).
  The desk pass's Play → Edit round trip and a unit test cover the first
  three; the hot-reload path is listed for the plan to enumerate (the
  borrow→cache lesson: enumerate every producer).
- **An ABI bump** (`WorldBounds`, `MaterialBlendMode` on
  `ResolvedMeshMaterial`) — cheap; Aphelyon rebuilds.
- **Nine pass types** in `Nri/nodes/`; the reorg trigger is one away and is
  stated, not pulled.

---

## Appendix A — code this spec builds on (survey 2026-09-18)

| Piece | Where | State |
|---|---|---|
| `MeshBounds`, `ComputeMeshBounds` | `ArcaneCore/src/Arcane/Mesh/MeshBuilder.hpp:110-116` | stays; `MeshBounds` → alias of `Aabb` |
| `MeshEntry::bounds`, `MeshTable`, `MeshMaterialTable`, `ResolvedMeshMaterial` | `ArcaneCore/src/Arcane/Scene/SceneResources.hpp` | gains `blend` / `alphaCutoff` on the resolved material |
| `FramingBounds`, `SelectionFramingBounds`, `SceneFramingBounds` | `ArcaneEditor/src/Viewport/EditorCamera.hpp:98-211` | become `WorldBounds` consumers |
| `ViewTransform` | `ArcaneCore/src/Arcane/Scene/ViewTransform.hpp:50` | unchanged; `Frustum::From` reads it |
| `TransformSystems` and its `Changed<Transform>` pre-pass | `ArcaneCore/src/Arcane/Scene/TransformSystems.hpp` | the dirty idiom `BoundsSystem` and `Sync` copy |
| `RenderSubmissionSystem` | `ArcaneClient/src/Arcane/Render/RenderSystems.hpp:47` | one membership check added |
| `PickEmit` | `ArcaneClient/src/Arcane/Render/PickEmit.cpp:41,162` | members only |
| `CollectMeshInstances`, `MeshInstance`, `MeshSceneDesc` | `Render/MeshSubmissionSystem.hpp`, `Render/Nri/nodes/MeshNode.hpp` | retired / replaced by `GpuSceneFrame` |
| `MeshNode` | `Render/Nri/nodes/MeshNode.{hpp,cpp}` | rewritten (§5.6) |
| `NriMeshBufferCache::Resident` | `Render/Nri/NriMeshBufferCache.hpp:81` | the per-batch vertex/index binding source |
| `NriUploadRing`, `Graveyard`, `NriPipelineCache` | `Render/Nri/` | the staging, burial and variant-key mechanisms reused |
| `DeclareGraphFrame`, `FrameDesc`, THE CLEAR SEAM block | `Render/Nri/NriGraphContext.{hpp,cpp}:1239-1400` | the three new nodes and the declared slots |
| `RenderGraph.hpp:33` "does not reorder, cull" | `Render/Nri/RenderGraph.hpp` | reworded |
| `MaterialSurface`, `MaterialSource` parsing | `ArcaneCore/src/Arcane/Material/MaterialSource.hpp:71` | `blend`, `alphaCutoff` |
| `VerifyReport` | `ArcaneClient/src/Arcane/Host/VerifyReport.hpp` | the `visibility` block |
| NRI indirect + dispatch | `ThirdParty/NRI/Include/NRI.h:197-204`, `NRIDescs.h:1659` | consumed as-is |

## Appendix B — references consulted

- UE 5.8.2 (`D:\dev\_reference\UnrealEngine-5.8.2-release`): `GPUScene.cpp/h`
  (`FGPUScene::Update`, `PreviousLocalToWorld`, dirty primitive tracking),
  `InstanceCullingManager.cpp`, `SceneVisibility.cpp` (`ComputeViewVisibility`,
  the frustum p-vertex test in `FConvexVolume::IntersectBox`),
  `MeshDrawCommands.cpp` (`FMeshDrawCommandSortKey`: base pass = PSO then
  mesh; translucency = depth), `Paper2D` (CPU sprites).
- Source 2 (public evidence via VRF and the Deadlock render target doc,
  `docs/research/2026-08-12-deadlock-render-target.md` T7 row): per-object
  AABBs, CPU frustum + voxel visibility, aggregate static props culled and
  drawn GPU-side, depth-pyramid occlusion in Deadlock; `F_ALPHA_TEST` /
  `F_TRANSLUCENT` material features.
- Gribb & Hartmann, *Fast Extraction of Viewing Frustum Planes from the
  World-View-Projection Matrix* (2001).
