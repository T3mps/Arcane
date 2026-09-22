# F3 — visibility and the GPU scene: world bounds, frustum culling, a GPU-driven mesh pass, draw ordering

**Date:** 2026-09-18
**Status:** Design, approved in brainstorm 2026-09-18; **vetted against UE
5.8.2 source 2026-09-18 and amended against Deadlock build 25379491 on
2026-09-19** (Appendix B: two mechanisms amended to UE's shape —
the prior pose via CPU history + re-dirty, the normal matrix in the row — and
three statements sharpened). **Plan 1 closed at `3b50ff3d`
(2026-09-18, the T8 head; the close booking is the commit after it): bounds,
visibility, the GPU scene with CPU-written indices + indirect draws; ABI 36, raised to 37 by the review fix 4363b825 -- MeshTable/SpriteTable carry the cache's publish generation.**
Step 2 of the binding
order (`docs/research/2026-09-16-direction-and-sequencing.md`: F4 → **F3** → F5);
F4 closed at `e95de920` (2026-09-18). Implementation plans follow this spec
(§12): two plans.
**Charter:** the pivot's F3 line (`docs/research/2026-08-21-3d-foundations-assessment.md`
§F3: per-mesh AABBs, world bounds, frustum culling, draw sorting — "the graph
does not reorder, cull") — **widened by the user 2026-09-18** to every world
drawable (meshes AND sprites) through one bounds path, and to a **GPU-driven
mesh pass** (a persistent GPU instance scene, a compute cull, indirect draws)
with a CPU per-view coarse stage in front of it. The 2026-09-19 amendment
adopts Deadlock's hybrid transparency shape: conventional ordered
translucency is the baseline, while moment-based OIT and refraction remain
separate later passes. G2 of the direction doc
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
masked / transparent** plus an independent **two-sided** raster flag;
transparent rows bypass the cull and are drawn last using an explicit
render-order, biased projected-depth and stable-identity key. This conventional
ordered path is the required F3 baseline, not Arcane's permanent transparency
ceiling: the pass contract reserves a later Deadlock-shaped moment-based OIT
(MBOIT) path and a separate refraction pass. `prevModel` on the row is the named prior-pose
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
| No transparent mesh path: the mesh pipeline is fixed opaque + back-face cull; the `.arcmat` mesh kind carries no blend mode | `MeshNode.hpp` WINDING AND CULLING, `Material/MaterialAsset.hpp` |
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
(a `WorldBounds` whose entity lost its renderer is removed), plus — the
asset-side producer — every mesh drawable when `MeshTable::generation` moved and
every sprite drawable when `SpriteTable::generation` moved (a counter the owning
cache bumps on any publish: a primitive parameter edit, a reimport, a sprite
resize re-resolves the entry with new bounds while nothing on the entity
changes; an unchanged box is not rewritten, so `Changed<WorldBounds>` stays
exact). A static scene does no work.

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
has today, because the material travels per section. `GpuInstance`, 240 bytes,
std430, mirrored by a `static_assert(sizeof == 240)` and an HLSL struct in
`data/shaders/gpu_scene.hlsli` that `mesh.hlsl` and `mesh_cull.hlsl` share:

| Field | Type | Bytes | Written by |
|---|---|---|---|
| `model` | `float4x4` | 64 | Sync (dirty) |
| `prevModel` | `float4x4` | 64 | Sync, from the mirror's CPU history (§5.3) |
| `normal0..2` | `float4` × 3 (xyz = the normal matrix's columns; w unused) | 48 | Sync, `NormalMatrixFor(model)` — the guarded CPU function stays the one producer (UE stores `InvNonUniformScale` + `DeterminantSign` for the same reason: nobody inverts a 3×3 per vertex, `SceneData.ush:233-235`) |
| `boundsMin` | `float4` (w unused) | 16 | Sync |
| `boundsMax` | `float4` (w = `alphaCutoff` for masked rows) | 16 | Sync |
| `baseColor` | `float4` | 16 | Sync |
| `materialSlot` | `uint` | 4 | Sync |
| `batch` | `uint` | 4 | Sync (see 5.4: the batch KEY id, stable per (mesh, section, blend)) |
| `flags` | `uint` | 4 | bit0 `teleported` (reserved, never set in F3); bits1–2 blend mode (0 opaque, 1 masked, 2 transparent); bit3 `twoSided`; bit4 `live` |
| `pad` | `uint` | 4 | 0 |

**`live` (bit 4) is load-bearing, not bookkeeping** (added by plan 2 Task 4's
fix round): a freed row is not erased from the persistent buffer — erasing it
would cost a scatter upload per free — it is STAGED AS A TOMBSTONE with this
bit clear, and both the CPU coarse pass and `mesh_cull.hlsl` skip any row
whose `live` bit is 0. Without it a dispatch sized to `rowCount` would
frustum-test a dead row's stale bounds and emit a draw for geometry nothing
owns.

The rows live in **one persistent structured buffer** owned by `GpuScene`
(`Render/Nri/GpuScene.{hpp,cpp}`, a member of `NriGraphContext` beside the
mesh-buffer cache), grown by doubling; the old buffer is buried in the
`Graveyard` with the frame fence, the `NriMeshBufferCache` discipline. The
128-byte `MeshConstants` root block is retired; everything it carried now
travels in the row.

**World bounds on the row, not local (a stated divergence):** UE keeps
`LocalBoundsCenter/Extent` per instance and transforms in the cull shader
(`SceneData.ush:236,242`) because its bounds are shared per primitive. Ours
are already world-space on `WorldBounds` for the CPU stage, a moved row is
re-uploaded anyway, and a pure plane test in the shader is cheaper.

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
    // The prior-pose history (§5.3). `lastModel` is the matrix each row was
    // last uploaded with (the one CPU matrix copy per resident row);
    // `dirtyLastFrame` is the re-dirty list. A static row's GPU prevModel
    // already equals its model.
    std::vector<glm::mat4>     lastModel;        // by row
    std::vector<std::uint32_t> dirtyLastFrame;
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
   row — rare, cheap) ∪ **`dirtyLastFrame`** (the re-dirty, step 4).
3. **The upload.** For each dirty row: `prevModel = isNew ? model :
   lastModel[row]`; then `lastModel[row] = model`. The full `GpuInstance` —
   `model`, `prevModel`, the normal matrix (`NormalMatrixFor(model)`), bounds,
   material — is staged through the upload ring and copied into the
   persistent buffer (per-row `CmdCopyBuffer` in plan 1; a scatter compute
   when the copy count is measured to matter — UE's
   `FRDGAsyncScatterUploadBuffer`). **No compute pass in `Sync`.**
4. **The re-dirty.** Rows uploaded this frame with `prevModel ≠ model` go
   into `dirtyLastFrame`; next frame step 2 uploads them once more, and since
   `lastModel[row] == model` by then the row lands with `prev == model` — so
   a row moved on frame N has `prev ≠ model` on frame N (velocity) and `prev
   == model` on frame N+1 (at rest). A row moved on N and again on N+1 is
   simply dirty on N+1 with N's pose as prev. This is UE's exact shape:
   `FSceneVelocityData::StartFrame` advances the CPU history and marks the
   primitive `ChangedTransform` the frame after it moved
   (`RendererScene.cpp:3329-3336`). Static rows cost nothing.

The G2 contract, for the record: **a row's `prevModel` is the `model` it was
drawn with on the previous frame; a new row, a rebuilt scene, or a
`teleported` row has `prev == model`.** `MeshRenderer` gains no field for
`teleported` in F3; the bit is named in the layout and left unset.

The copies are recorded by `GpuSceneSyncNode` (§8), a transfer-only node
reading the ring slice the host staged.

### 5.4 Batches — CPU per frame, from the mirror

Key = `(mesh Guid, section index, blend mode, twoSided)`. The row's `batch` field holds
a stable **key id** (a `FlatMap<key, id>` in `GpuScene`, ids reused on free).
Per frame the CPU builds:

- **The batch table** — for every key with ≥ 1 resident row: `capacity` (the
  count of resident rows with that key, known from the mirror without
  visibility), `firstOutput` = the exclusive prefix sum of capacities in key-id
  order. Uploaded through the ring; `GpuCullBatch { uint firstOutput; uint capacity; uint argIndex; uint emitted; }` (`argIndex` = the batch's position in the emitted list, meaningful only when `emitted`). **That field order is the shipped one** — `Render/GpuSceneTypes.hpp`'s `static_assert(sizeof(GpuCullBatch) == 16)` and `data/shaders/mesh_cull.hlsl`'s mirror (the struct lives in the cull shader, not in the shared `gpu_scene.hlsli`, which carries only `GpuInstance` and its flag constants) both spell it `firstOutput, capacity, argIndex, emitted`; this spec's draft wrote `GpuBatch` with `emitted` and `argIndex` the other way round, and the code is the authority.
- **The emitted list** — keys with at least one row whose entity is in
  `SceneVisibility.views[0]` (the coarse pass prunes whole batches; a batch
  entirely off-screen never reaches the cull or the draw), **excluding
  transparent keys** (§7). A row whose resolved material changes blend mode
  or two-sided state is removed from its old key and inserted into the new key
  in the same reconciliation; merely rewriting its GPU flags is insufficient.
  Ordered: **opaque before masked**, and within each
  by the minimum `nearDepth` of the batch's coarse-visible entities,
  ascending (nearest first — the early-Z key). The order is a CPU sort over
  tens of batches.
- **The indirect args array** — one `nri::DrawIndexedDesc` per emitted batch,
  in emitted order: `indexNum = section.indexCount`, `baseIndex =
  section.indexOffset`, `baseVertex = 0` (a resident mesh's buffers are its
  own — §1), **`instanceNum = 0`**, `baseInstance = 0`. Every frame starts
  from these zeroed counters before dispatch; stale counts may never survive
  a frame. Uploaded through the ring into a UAV-capable transient the cull
  writes and the draw reads.

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
the draw node. The HLSL mirrors `GpuInstance`, `GpuBatch` and
`nri::DrawIndexedDesc` field-for-field; it guards `id >= rowCount` before any
structured-buffer read, uses the interlocked operation's out parameter, and
never casts an argument buffer to a byte-address layout with invented offsets.

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

then `model`, `normal0..2`, `baseColor`, `materialSlot` exactly as
`MeshConstants` carried them. A zero-`instanceNum` batch is a
no-op draw; **no count buffer**, so the `drawIndirectCount` cap is not
required and `baseInstance` is never used (no dependence on NRI's
draw-parameters emulation). Pipeline layout: root constants b0 (8 bytes),
the root sampler s0, space1 = `{ b1 frame CB, t0 instances, t1 visibleIndices }`,
space2 = the bindless material array — the register-space rule from
`MeshNode.hpp` unchanged in kind.

`MeshSceneDesc` (in `FrameDesc`) loses `instances` and gains
`const GpuSceneFrame*` — `{ emitted batches (ordered), args (ring slice),
transparent draw records (ordered, §7), frustum, rowCount }`, host-owned for the
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
| `twoSided` | boolean | `false` (independent of blend mode) |

Source 2 carries the trio as `F_ALPHA_TEST` / `F_TRANSLUCENT` on the material;
UE as Blend Mode Opaque / Masked / Translucent. `MaterialAsset` parses them
into `enum class MaterialBlendMode : uint8_t { Opaque, Masked, Transparent }`;
an unknown string **refuses** with the field named (the loader's posture).
`ResolvedMeshMaterial` (Core, backend-agnostic) carries `blend` and
`alphaCutoff` plus `twoSided` beside `baseColor` / `albedo`; `Sync` copies them
into the row's `flags` / `boundsMax.w`. `ShaderEditorDocument`'s mesh panel
gets the dropdown, two-sided checkbox and a bounded `alphaCutoff` drag (the
F2a "unauthorable invalid state" idiom). Because `ResolvedMeshMaterial` is a
Core type visible to game modules, this change raises the plugin ABI from 37
to 38 and restamps both in-tree project manifests.

**Pipeline variants** — `NriPipelineCache`'s key gains the blend mode:

| Variant | Depth | Blend | Cull | Shader |
|---|---|---|---|---|
| Opaque | test + write | off | back unless `twoSided` | precompiled opaque pixel shader |
| Masked | test + write | off | back unless `twoSided` | precompiled masked pixel shader: `clip(alpha - cutoff)` |
| Transparent | test, **no write** | `SrcAlpha / InvSrcAlpha`, add | back unless `twoSided` | precompiled transparent pixel shader, alpha through |

The shader variants are offline build outputs and pipeline-cache keys; runtime
pipeline creation does not attempt to inject preprocessor defines into already
compiled bytecode.

Alpha = `baseColor.a × albedo.a` — the first time `baseColor.a` means
anything; opaque rows ignore it as they do now. The mesh shader's flat path
(`kInvalidSlot`) keeps `albedo.a = 1`.

---

## 7. Draw order across the frame

1. **Opaque batches**, nearest-first by batch (§5.4), indirect.
2. **Masked batches**, same order, after every opaque batch: a `clip` disables
   early-Z for that draw, so masked geometry goes last among the depth
   writers. **This flips when the prepass slot is filled**: UE orders masked
   *last* with no prepass and *first* with one
   (`BasePassRendering.cpp:335-342`, `EarlyZPassMode`); whoever mints the
   prepass flips the batch order in the same change.
3. **Transparent rows**, direct: the CPU takes the `VisibleSet` entries whose
   entity has transparent rows and emits a complete draw record containing
   row id, mesh Guid, section index/range, render order, depth bias and stable
   identity. `MeshRenderer` supplies `translucencyRenderOrder` (default 0) and
   `translucencyDepthSortBias` (default 0 m). For Arcane's right-handed camera
   looking down -Z, `projectedDepth = -viewSpaceCenter.z + bias`; records sort
   by render order ascending (higher values draw later), projected depth
   descending (far-to-near), then `(entity, mesh, section)` ascending. The
   final identity term makes equal-depth frames deterministic. The renderer
   issues one `CmdDrawIndexed` per record on the transparent pipeline with the
   root block `{ row, direct=1 }`. Transparent rows never enter the cull: their
   batch is not emitted (§5.5), and the CPU coarse test is their whole culling.
4. `GridNode` stays after the mesh pass, depth-tested against the same depth,
   as F4 left it; the sprite batch, the post chain, the tonemap and the
   overlay are unchanged. Sprites-over-meshes and shared depth remain F5's.

---

## 8. Graph wiring and the declared slots

`DeclareGraphFrame` (`NriGraphContext.cpp`) gains, between `batch2d` and the
mesh draw:

```
batch2d → GpuSceneSyncNode (the row copies; transfer only) → MeshCullNode → MeshNode (indirect + direct) → GridNode → post → tonemap → …
```

New node types (`GpuSceneSyncNode`, `MeshCullNode`, the rewritten `MeshNode`),
and plan 2 added a second readback beside the sync node's debug one.
`RenderGraph.hpp` is reworded: the graph still does not reorder; **culling is
a node**, not a graph property.

**Pass-type recount at the close (plan 2 Task 6).** Counting the types
`Nri/nodes/` declares — nine classes (`Batch2DNode`, `PostChainNode`,
`TonemapNode`, `GridNode`, `ImGuiNriNode`, `MeshCullNode`, `MeshNode`,
`PickNode`, `OutlineNode`) plus the three free node-declaring functions in
`GpuSceneSyncNode.hpp` (`AddGpuSceneSyncNode` → `gpuscene-sync`,
`AddGpuSceneDebugReadbackNode` → `gpuscene-readback`,
`AddGpuSceneVisibilityReadbackNode` → `gpuscene-visibility-readback`) — the
tree now holds **twelve pass types**, not the nine this section predicted
before plan 2 split the readbacks. The domain-reorg trigger (10+,
`project_arcane_render_pass_domain_reorg`) is therefore **crossed**. It is
STATED here and NOT pulled: plan 2 forbids reorganising `Nri/nodes/`, because
a directory move in the same change as a new pass buries the pass's diff.
The reorg stays the standing maintenance item it already was.

**Declared pass slots — the G1/G2 seams, written here so F5 builds on them:**

| Slot | Position | Minted by | Written by |
|---|---|---|---|
| `prepass` (depth-only opaque) | before the mesh draw | nobody in F3 | Hi-Z (T7) or F5 if it chooses; its value scales with overdraw and shading cost, neither present |
| `velocity` (RG16F) | beside the mesh draw's colour/depth | nobody in F3 | F5 mints it with the shared depth; T5 writes it from `model`/`prevModel` |
| `sharedDepth` | the mesh pass's D32 | F4's `MeshNode` (today) | F5 makes it the frame's one depth |

The full list F5 inherits: `prepass → G-buffer → lighting → forward/masked →
transparent-ordered → transparent-mboit → mboit-combine → refraction →
2D-world → post → overlay` (G1). F3 implements the forward, masked and
ordered-transparent slots only. The MBOIT accumulation/combination targets and
the refraction source/destination contract are named seams, not F3 resources.

**Debug switch:** a compile-time `kMeshCullEnabled` (default on,
`MeshCullNode.hpp`) — for bisecting a wrong-picture report. The cvar arc
makes it runtime. **It bypasses the frustum predicate INSIDE the compute
shader** (`mesh_cull.hlsl`'s `g_Cull.cullEnabled` guard on `Contains`), so
every live row of an emitted batch is counted; it does NOT restore
CPU-authored counts. The invariant that the compute pass is the sole writer
of `instanceNum` holds in both settings — the draft's `instanceNum =
capacity` spelling would have broken it.

---

## 9. Testing and verification — the definition of done

In order of authority:

1. **The oracle test (`[gpu]`, `GpuSceneCullTest.cpp`).** A fixture scene:
   rows fully inside, fully outside, straddling each of the six planes, at
   every blend mode, several sections per mesh, two meshes. After one frame,
   read back the args' `instanceNum` per batch and the `visibleIndices`
   contents; they must equal the CPU `VisibleSet`'s exact row-id answer for the
   same rows (set-equal per batch — the atomic order is arbitrary), including
   each batch partition's bounds and excluding stale tail data. The readback
   is delayed/asynchronous and never inserts a per-frame device flush. Runs on D3D12 and
   Vulkan. **The CPU pass is the oracle by construction.**
2. **Witness.** `VerifyReport` gains `visibility { total, coarseVisible,
   gpuVisible, batches, draws, transparentRows }` at **schemaVersion 9**, and
   `gpuVisible` is **`null` until a readback retires** — it is the cull pass's
   OWN asynchronously read-back count, never a copy of `coarseVisible`, so a
   run that never armed the ring (or whose frames were all still in flight)
   reports the absence rather than a plausible number. The witness lanes
   assert the block for ReferenceProject's 3D scenes: `EditorWitnessTest.cpp`'s
   E2 on the all-opaque boot scene (`total`/`coarseVisible`/`batches`/`draws`
   exact, `transparentRows == 0`, `gpuVisible` a number equal to
   `coarseVisible`), and `WitnessScenariosTest.cpp`'s W5 on the F3 fixture
   scene (the same four exact, plus `transparentRows > 0`, `draws == batches +
   transparentRows`, and `gpuVisible` a number STRICTLY below `coarseVisible`
   — transparent keys never reach the cull).
3. **Goldens.** `editor-ui` and `editor-ui-perspective` must not move as a
   RENDER (nothing in them is off-screen); adding fixture assets does move
   their asset-census text, which is a content fact and not a render one. One
   new golden pair — `f3-cull-blend` on dx12 and vulkan, rendered by
   ArcaneRuntime with `--scene` pointed at
   `ReferenceProject/Content/scenes/f3_cull_blend.arcscene` — carrying all six
   required visual cases: conservative culling of a straddling box, masked
   over opaque with correct depth order, a masked surface clipped away
   entirely by `alphaCutoff`, far-to-near transparent blending, stable
   identity order at equal depth, `translucencyRenderOrder`/
   `translucencyDepthSortBias` overriding depth order, and a one-sided
   surface culled where its two-sided twin draws.
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
| UE: `FPrimitiveBounds` + `ComputeViewVisibility` (CPU, **linear by default** — `GFrustumCullUseOctree = false`, `SceneVisibility.cpp:343`) → `GPUScene` (persistent `FSpanAllocator` slots, `EPrimitiveDirtyState`) → `InstanceCullingManager` (`InterlockedAdd` on `instanceNum`, ids at a per-draw prefix offset — `BuildInstanceDrawCommands.usf:302,352`) → indirect draws | the same two-layer shape, one row per section | Match |
| UE: prior pose = `FSceneVelocityData` CPU history for moved primitives, advanced at `StartFrame`, primitive re-dirtied the frame after it moved (`ScenePrivate.h:1274-1352`, `RendererScene.cpp:3320-3348`) | the same (§5.3): `lastModel` + `dirtyLastFrame` in the mirror | Match (amended from a GPU row-writer after the vet) |
| UE: per-draw instance-id offset via an instance-stepped vertex stream (`VertexFactoryCommon.ush:40,54`) because many draws share one indirect call | one root constant per batch | Diverge: one draw per batch; a root constant is the simpler equivalent until draws are merged |
| UE base pass key: shader hashes + a Masked bit, **no depth term** (`FMeshDrawCommandSortKey::BasePass`, `MeshPassProcessor.h:1545-1548`); early-Z is the prepass alone | batches opaque → masked, then nearest-first by batch; no prepass | Diverge: nearest-first is an early-Z stand-in while the prepass slot is empty; the masked bit's order matches UE's no-prepass branch |
| Deadlock build 25379491: conventional `Translucent` passes coexist with `Moments` → `Translucent Color` → `MBOIT Combine`; refraction is a later separate pass. Particle systems still expose depth/creation/nearest sorting, sort bias and sort override position | ordered direct translucency in F3; named MBOIT accumulation/combine and refraction seams for the effects tier | Match the hybrid architecture without paying for its extra targets and moments before Arcane has the effects workload |
| UE translucency: multiple policies, including priority plus distance; conventional translucency remains order-dependent | explicit render order, biased projected depth and stable identity | Similar control surface, deliberately smaller policy set |
| UE / Source 2: Opaque / Masked / Translucent (`EngineTypes.h:247-249`; `F_ALPHA_TEST`, `F_TRANSLUCENT`) | the same trio | Match. UE's default `OpacityMaskClipValue` is 0.3333 (`Material.cpp:1170`); ours is 0.5, Source 2's `g_flAlphaTestReference` / Unity's default |
| UE translucency key: `Priority` then `Distance` (`BasePassRendering.cpp:322-324`) | render order, then `-viewSpaceCenter.z + depthSortBias`, then stable identity | Match the useful control shape and make ties deterministic |
| UE and Source 2 treat two-sidedness independently from blend mode | `twoSided` is an independent material field and pipeline key | Match; transparent does not imply two-sided |
| UE instance record: `PrevLocalToWorld`, `InvNonUniformScale` + `DeterminantSign`, LOCAL bounds (`SceneData.ush:229-243`) | `prevModel`, the normal matrix, WORLD bounds | Match on prev and on precomputing the normal transform; diverge on bounds space (§5.1) |
| UE Paper2D sprites: CPU everything (`FDynamicMeshBuilder` per frame, `PaperRenderSceneProxy.cpp:372`) | sprites CPU-culled through `VisibleSet`, drawn by `Batcher2D` as today | Match; sprites join the GPU scene only on a measured 2D-perf trigger |
| UE `GPUScene`: per-instance `bCastShadow`, LOD, custom data | none | YAGNI; the row has a `pad` and a `flags` word |

---

## 11. G2 and the greedy-ordering guards, restated

- **G1** — the pass-slot list is declared (§8); F3 implements forward/masked/
  ordered transparent only, with MBOIT/combine/refraction seams left unminted.
- **G2** — the prior pose is `GpuInstance::prevModel`, sourced from the
  mirror's CPU history for moved rows (§5.3, UE's shape), the velocity
  target is a declared slot (§8), the culling frustum is unjittered (§3).
  The direction doc's "`PreviousTransform` already carries the data" is
  superseded by this spec; no ECS history component is recreated.
- **G3** — untouched here (the hygiene wave's artifact stamp); the `.arcmat`
  gains three fields with defaults, so existing materials load unchanged.

---

## 12. Plans

1. **Plan 1 — bounds, visibility, and the GPU scene without the cull.**
   `Aabb`; `WorldBounds` + `BoundsSystem`; `Frustum`; `VisibleSet` /
   `SceneVisibility` + `BuildVisibleSet`; the sprite sweep and `PickEmit`
   consume it; the editor framing lift; `GpuSceneMirror`, `GpuScene` rows /
   slots / `Sync` with the `lastModel` history and the re-dirty; the batch builder;
   the rewritten `MeshNode` drawing every emitted batch **indirect with
   `instanceNum` written by the CPU** (`= capacity`, identity indices — the
   indirect path is proven before the compute pass exists); `MeshSceneDesc`
   → `GpuSceneFrame`; `CollectMeshInstances` retired; ABI bump; unit tests
   (§9.4); goldens unchanged; the witness counts (GPU figure = coarse figure
   until plan 2).
2. **Plan 2 — the cull, the modes, the order.** `MeshCullNode` and
   `mesh_cull.hlsl`; the oracle test on both backends; `blend` /
   `alphaCutoff` / `twoSided` through `.arcmat` → `MaterialAsset` →
   `ResolvedMeshMaterial` → the row; the pipeline variants; masked and
   deterministic transparent order; the stats overlay line and readback ring; the new
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
| MBOIT moments, accumulation targets and combine pass | T5/effects tier, when intersecting translucent effects justify the memory and bandwidth |
| Refraction | T5/effects tier; remains after conventional and MBOIT transparency so it samples a defined composed scene |

---

## 14. Rulings ledger (2026-09-18)

| # | Ruling | Rejected |
|---|---|---|
| R1 | Meshes **and** sprites through one engine-owned bounds path (`WorldBounds`); the editor's framing becomes a consumer | meshes only; a declared sprite seam |
| R2 | **GPU-driven** for meshes: persistent GPU scene, compute cull, indirect draws — with a CPU per-view coarse stage in front as the one seam for sprites, picking, framing and tests | inline per-sweep culling; a CPU-only `VisibleSet` stage |
| R3 | GPU scene for meshes + CPU coarse (UE's shape); sprites stay CPU; Hi-Z declared, not built | a full GPU scene including sprites; Hi-Z now |
| R4 | Opaque early-Z from **batch state-sort + CPU nearest-first by batch**; the depth prepass is a declared slot | a prepass now; a GPU depth sort |
| R5 | Build the ordered transparent path **and** masked: `blend` plus independent `twoSided` on the mesh material; transparent rows sort by render order, biased projected depth and stable identity, then draw directly after indirect batches | reserve only; transparent-implies-two-sided |
| R6 | Prior pose = `GpuInstance::prevModel`; no ECS history component. **Mechanism amended after the UE vet (user, 2026-09-18):** a CPU history in the mirror + a re-dirty the frame after a move (UE's `FSceneVelocityData`), replacing the GPU row-writer + advance dispatch | `PreviousWorldTransform` + a system; defer to F5; the GPU row-writer |
| R8 | The row carries the normal matrix from `NormalMatrixFor` (240 B), not a per-vertex 3×3 inverse (UE vet, 2026-09-18) | computing it in the vertex shader |
| R7 | Persistent slots with dirty-tracked uploads (`Changed<>` + reconciliation); a registry generation stamp forces the full rebuild | full re-upload every frame; transient per-frame rows |
| R9 | Deadlock-shaped hybrid transparency contract: ordered translucency now; MBOIT accumulation/combine and refraction are distinct later pass seams | projected-Z as Arcane's permanent transparency solution; MBOIT in F3 |

**Executor rulings, plan 1 (2026-09-18, ledgered in the plan's progress notes):**
R-A `AddMeshNode` declares `gpuscene-sync` before `mesh` itself, so the
copy -> read barriers are the graph's; R-B the scene grows at declaration time
(`GpuScene::Reserve`, before the imports), never inside `Record` -- amended in
T6: `Reserve` PARKS the retired buffers stamped with `CurrentFence()`
(`DebugSubmitCount() + 1`, the fence this frame's submit signals) and
`GpuScene::FlushGraves` buries them right after a successful `Execute`, since
`Graveyard::Bury` asserts nondecreasing fences and `Execute` itself buries at
`DebugSubmitCount()` mid-run; the old -> new live-row copy is issued at the
top of `Apply` behind an explicit SHADER_RESOURCE -> COPY_SOURCE barrier on
the old buffer and a COPY_DST -> COPY_DST barrier on the new one before the
staged rows land; R-C
`GpuScene.hpp` carries the `ARCANE_API` declaration of `GpuSceneSyncedGeneration`
so its definition exports; R-D the mesh node is declared whenever the stage has rows
(or a full rebuild is pending), not only when a batch emits -- a drawless
frame still uploads; R-E the sprite `WorldBounds` widens Z ONLY, per s2.3's
table (the plan's uniform `Widened()` moved the framed editor camera
sub-pixel and diffed both editor golden lanes); R-F a Skipped or Failed
graph frame invalidates the mirror (`GpuSceneInvalidate`) -- the rows it
staged never reached the device and no re-dirty would repeat them.

---

## 15. Costs and risks, stated plainly

- **`MeshNode` is rewritten, not extended.** The 128-byte root block, the
  per-draw `NormalMatrixFor` push and the `instances` span all go. Plan 1
  proves the indirect path with CPU-written counts before any compute exists,
  so a wrong picture bisects to one of two halves.
- **First production compute pass (the cull, plan 2) and first indirect draw (plan 1).** A D3D12-vs-Vulkan
  disagreement on the args layout, the UAV → indirect-argument barrier, or
  structured-buffer packing is the real risk; NRI abstracts all three and the
  oracle test runs on both backends. `static_assert`s pin the row size and
  the `DrawIndexedDesc` stride against the HLSL side.
- **`Sync`'s re-dirty is the subtle piece** (a row moved on N must be at
  rest on N+1; a row moved on N and N+1 must carry N's pose as prev on N+1).
  It is pure CPU logic now and has its own unit test; the G2 contract
  sentence in §5.3 is the thing T5 will read. The one CPU matrix copy per
  resident row (`lastModel`, 64 B) is the price of never reading the GPU back.
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
| `MaterialAsset` parsing and mesh inheritance | `ArcaneCore/src/Arcane/Material/MaterialAsset.hpp`, `ArcaneClient/src/Arcane/Render/MeshMaterialCache.cpp` | `blend`, `alphaCutoff`, `twoSided` |
| `VerifyReport` | `ArcaneClient/src/Arcane/Host/VerifyReport.hpp` | the `visibility` block |
| NRI indirect + dispatch | `ThirdParty/NRI/Include/NRI.h:197-204`, `NRIDescs.h:1659` | consumed as-is |

## Appendix B — references consulted

- UE 5.8.2 (`D:\dev\_reference\UnrealEngine-5.8.2-release\Engine`), **read
  2026-09-18, line-cited above**: `Source/Runtime/Renderer/Private/ScenePrivate.h:1274-1352`
  + `RendererScene.cpp:3320-3348` (`FSceneVelocityData`: CPU prior pose,
  advance, re-dirty); `GPUScene.h:341-415` (`FSpanAllocator` slots,
  `FRDGAsyncScatterUploadBuffer`), `Source/Runtime/Engine/Public/PrimitiveDirtyState.h`,
  `SpanAllocator.h:23-67`; `InstanceCulling/InstanceCullingContext.cpp:94-115,253-278`
  and `Shaders/Private/InstanceCulling/BuildInstanceDrawCommands.usf:99-109,302-352,378`
  (the cull → args → id list); `Shaders/Private/VertexFactoryCommon.ush:33-65`
  (the per-draw offset stream); `Source/Runtime/Engine/Private/ConvexVolume.cpp:217-300,714-770`
  (box push-out test, plane extraction); `SceneVisibility.cpp:325-360,531-625`
  (linear default, `IntersectBox8Plane`); `Public/MeshPassProcessor.h:1541-1561`
  and `Private/BasePassRendering.cpp:318-343` (sort keys, the masked flip);
  `Shaders/Private/SceneData.ush:229-243` (`FInstanceSceneData`);
  `Source/Runtime/Engine/Classes/Engine/EngineTypes.h:247-249` (blend modes),
  `Source/Runtime/Engine/Private/Materials/Material.cpp:1170` (cutoff default);
  `Plugins/2D/Paper2D/.../PaperRenderSceneProxy.cpp:292-372` (CPU sprites).
- Source 2 (public evidence via VRF and the Deadlock render target doc,
  `docs/research/2026-08-12-deadlock-render-target.md` T7 row): per-object
  AABBs, CPU frustum + voxel visibility, aggregate static props culled and
  drawn GPU-side, depth-pyramid occlusion in Deadlock; `F_ALPHA_TEST` /
  `F_TRANSLUCENT` material features.
- Deadlock installed build 25379491 (inspected 2026-09-19): render passes and
  target strings for conventional `Translucent`, `Moments`, `Translucent
  Color`, `MBOIT Combine`, and later refraction; MBOIT enable/quality/bias/
  overestimation controls; particle depth/creation/nearest sorting, bias,
  disable-sort and sort-position controls. These observations establish the
  hybrid contract; they do not imply every material enters MBOIT.
- Gribb & Hartmann, *Fast Extraction of Viewing Frustum Planes from the
  World-View-Projection Matrix* (2001).
