# The 3D foundations assessment

**Date:** 2026-08-21
**Status:** assessment — input to a spec, not itself a plan
**Trigger:** NRI Phase 4 (the 3D slice) reached Task 9 and could not proceed
**Decision it records:** one 3D `Transform`, foundations before visuals, no cobbling

---

## Why this document exists

NRI Phase 4 set out to put a lit, textured, depth-tested mesh through the frame
graph. Tasks 1–8 do that, and they hold up. Task 9 — *"both hosts populate the
mesh scene from the scene"* — turned out not to be implementable, and the reason
is structural rather than an oversight in the plan's wording.

**The scene has no 3D in it.**

The complete component set in `ArcaneClient/src/Arcane/Scene/Components.hpp` is
`Transform`, `WorldTransform`, `PreviousTransform`, `SpriteRenderer`,
`PostProcess`, `Camera`, `Identity`, `Hidden`. There is no mesh component, and
`Transform` is two-dimensional:

```cpp
struct Transform {                       // Components.hpp:21
    glm::vec2 position;
    float     rotation;                  // one angle
    glm::vec2 scale;
    glm::mat3 ToMatrix() const;          // mat3, not mat4
};
struct WorldTransform { glm::mat3 matrix; };            // :40
struct PreviousTransform { glm::vec2 position; float rotation; };  // :52
```

`MeshNode` therefore receives geometry through a side channel —
`MeshSceneDesc::instances`, a borrowed `std::span<const MeshInstance>` whose
elements each carry a `glm::mat4 model` computed by the **caller**. Nothing in
the ECS produces those. A host asked to "populate it from the scene" has nothing
to read.

The same root cause explains a gap found earlier in the phase and filed then as
a Task 5 limitation: the perspective camera has projection but **no pose**. It
always looks down `−Z` from a 2D-position-derived eye. That is not an omission in
Task 5 — it is a consequence of nothing in the scene having a 3D pose to give it.

It also puts the engine in the position its own source argues against.
`Components.hpp:109-111` justifies making the camera scene data:

> *"That made the editor viewport and a standalone runtime get their view from
> different places, so a scene could look correct in the editor and render
> nothing in the game. A camera IS scene data."*

Meshes currently have exactly that split.

---

## What is already right — do not rebuild it

**The transform hierarchy exists and is dimension-agnostic in structure.**
This is the single most important finding here, because it is the expensive half
of a transform system and it is done.

`Scene/TransformSystems.hpp` walks `reg.GetRelations(root).ForEachDescendant` in
**BFS pre-order, parent before child**, handles the root/no-parent case
separately, adds a missing `WorldTransform` on demand, skips non-spatial nodes,
and composes:

```cpp
const Astra::Entity   parent      = reg.GetParent(e);
const WorldTransform* parentWorld = reg.GetComponent<WorldTransform>(parent);
const glm::mat3       parentMat   = parentWorld ? parentWorld->matrix : glm::mat3(1.0f);
world->matrix = parentMat * local->ToMatrix();
```

Astra supplies `GetParent` / `GetRelations` / `ForEachDescendant`. Ordering,
parenting, and the root case are all solved and none of them are 2D-specific.
**Going 3D is a type change through existing machinery, not a new subsystem.**

Also already correct and not in question:

- **Camera is scene data** (`Camera` component, `SceneCamera` resolves it).
- **Units are MKS/meters**, consistently, with the reasoning recorded in-source.
- **The render layer**: NRI graph, pipeline cache (graphics + compute), device
  capability gating, `mipCount`, the depth transient, `MeshNode`, `mesh.hlsl`,
  `MeshBuilder`. Phase 4 Tasks 1–8 are all render-layer with no scene
  dependency and survive this pivot intact.

---

## Measured blast radius

Derived by running `grep -rlw "Transform\|WorldTransform\|PreviousTransform"`
per project, not recalled.

| Project | Files |
|---|---|
| ArcaneClient | 22 |
| ArcaneEditor | 9 |
| ArcaneRuntime | 1 |
| ArcaneTests | 25 |
| ReferenceProject | 0 |
| **Total** | **57** |

### ArcaneClient, by subsystem

| Subsystem | Files |
|---|---|
| **Plugin ABI** | `Plugin/PluginABI.hpp` |
| **Serialization** | `Serialization/SceneAsset.hpp`, `Serialization/SceneSerializer.hpp` |
| Scene | `Components.hpp`, `TransformSystems.hpp`, `RenderSystems.hpp`, `SceneCamera.hpp`, `SceneModule.hpp`, `PhysicsComponents.hpp`, `PhysicsSystem.hpp` |
| Editing | `Edit/Gizmo.{cpp,hpp}`, `Edit/EntityOps.{cpp,hpp}`, `Edit/ComponentEditCommand.hpp` |
| Render | `Render/Batcher2D.hpp`, `Render/PickEmit.{cpp,hpp}`, `Render/PhysicsDebugDraw.cpp` |
| Boot | `Base/Runtime.cpp`, `Host/ProjectBoot.{cpp,hpp}` |

### ArcaneEditor

`App/EditorApp.{cpp,hpp}`, `App/EditorAppFrame.cpp`, `App/EditorAppScene.cpp`,
`Panels/EditorPanels.cpp`, `Scene/ComponentCatalog.{cpp,hpp}`,
`Viewport/EditorCamera.{cpp,hpp}`

### Two consequences the file list makes unavoidable

1. **This is a plugin ABI break.** `Transform` crosses `Plugin/PluginABI.hpp`.
   The ABI goes 15 → 16, and every consuming project's `.arcproj` needs
   restamping — the same chore already visible in Gacha's history
   (`chore(game): restamp Aphelyon.arcproj to engine ABI 15`). Standing
   direction is that ABI bumps are cheap during engine development, so this is
   a tracked consequence rather than an obstacle.
2. **This is a scene file-format break.** `SceneSerializer` and `SceneAsset`
   both carry `Transform`. Every existing `.arcscene` changes shape. See Open
   Questions.

---

## The foundations, in dependency order

### F1 — the transform spine

The type change, and it must land first and alone.

| Type | From | To |
|---|---|---|
| `Transform` | vec2 pos, float rot, vec2 scale | **vec3 pos, quaternion rot, vec3 scale** |
| `Transform::ToMatrix` | `glm::mat3` | `glm::mat4` |
| `WorldTransform` | `glm::mat3` | `glm::mat4` |
| `PreviousTransform` | vec2 + float | **vec3 + quaternion** |

`PreviousTransform`'s interpolation changes character, not just type. Its current
contract (`Components.hpp:46-51`) is explicit that it is *"decomposed (position +
angle) so rotation interpolates on the shortest arc, NOT by lerping matrix
components."* The 3D equivalent of that intent is **SLERP** on the quaternion —
`glm::slerp`, not a component-wise lerp, and not a matrix lerp. Preserving the
stated intent is what makes this a port rather than a rewrite.

Rotation representation should be **quaternion in storage**, with Euler angles
presented at the inspector via reflection attributes. That is what UE
(`FTransform` holding `FQuat`), Unity, and Godot all do, and it avoids gimbal
lock in the stored data while keeping authoring legible.

**Physics stays 2D and operates on the XY plane of the 3D transform.** This is
the correct degenerate case, not a compromise: `PhysicsSystem` writes back
position/rotation, and doing so into `.xy` and the Z-axis rotation of a 3D
transform is exactly what a 2D simulation in a 3D world means. It also keeps a
57-file type change from colliding with a physics migration.

### F2 — 3D vocabulary in the scene

A `MeshRenderer` component, and mesh + material as **assets** rather than
side-channel data. This is where the asset cook arc
(`docs/research/2026-08-21-asset-cook-pipeline-design.md`) stops being optional:
without it, 3D content cannot be authored, saved, or reopened.

Note the ordering dependency with the render layer: Phase 4's `BindlessTable`
hands out descriptor slots, but it does not know what a *material* is. F2 is
where that gets defined, and a material asset design should precede or accompany
any further bindless work.

### F3 — what a 3D renderer needs that a 2D one did not

**There is no frustum culling and no bounds anywhere** in `Render/` or `Scene/`.
`RenderGraph.hpp:33` states it outright: the graph *"does not reorder, cull, or"*
otherwise reorganize. For a handful of sprite batches that is a reasonable
non-feature. For 3D scenes it is not optional:

- per-mesh local AABB, computed at build/cook time
- world-space bounds derived through the transform
- frustum culling against the camera
- draw sorting — opaque front-to-back for early-Z, transparent back-to-front

### F4 — editor authoring for 3D

- The gizmo is 2D (`Edit/Gizmo.{cpp,hpp}`, already in F1's blast radius) and
  needs 3D translate/rotate/scale.
- `Viewport/EditorCamera.{cpp,hpp}` is where orbit/fly controls belong. Note the
  editor already has its own camera distinct from the scene `Camera` component —
  this is the seam that makes camera *pose* tractable ahead of a full 3D
  camera-component story.
- 3D picking **may already work**: picking is a GPU id-buffer read
  (`Render/PickEmit`), not a 2D math path. **Verify rather than assume** — this
  could be free or could be a whole item.

### F5 — the compositing architecture

Two coupled changes:

1. **Hoist the clear to a declarative attachment load-op.** Today
   `Batch2DNode::Record` issues `CmdClearAttachments` mid-pass
   (`Batch2DNode.cpp:1321-1325`), with the intent stated as *"the only node that
   clears the canvas at all, so there is exactly one background colour in the
   process."* `NriGraphContext.cpp:1123-1163` already records the declarative
   clear-op as deferred work. This is how both D3D12 and Vulkan want it
   expressed, and it is faster on tiled hardware.
2. **One world pass with a shared depth buffer** that 3D geometry and 2D world
   content both sort into.

Until (1) exists, node ordering is forced: anything declared before `batch2d` is
erased by its clear, and anything after composites on top. That is why 3D
currently draws over `Batcher2D` content. ImGui UI is unaffected — `gameui` and
`hostHud` are declared after the tonemap, which is where UI belongs.

---

## Physics — decided

**Vendor Box3D**, but **not until F1 has landed.**

[Box3D](https://github.com/erincatto/box3d) — Erin Catto, released 2026-06-30.
MIT, C17 with a clean C API, forked from the Box2D codebase. Cross-platform
determinism, SIMD contact solving, multithreading, triangle-mesh and heightfield
collision, large worlds. In production in Facepunch's s&box and the Esoterica
engine.

Chosen over Jolt for three reasons specific to this engine:

1. **Heritage from Valve's Rubikon** — Source 2's physics engine — aligning with
   a Deadlock-class renderer target.
2. **It is a Box2D fork.** Box2D v3.1.1 is already vendored here as *the parity
   citation source* for Manifold2D. Box3D slots into precisely that role for a
   future Manifold3D: same author, same methodology, continuous architecture.
   Jolt would mean learning an unrelated design and later porting away from it.
3. **C17 with a C API** matches the existing vendoring shape — no nested build
   system, consistent with the stated preference for vendoring over package
   managers.

**Accepted risk:** Box3D is **v0.1 with no API stability guarantees**, roughly
seven weeks old at time of writing; breaking changes are expected, and the
near-term roadmap is character movement, ghost collisions, and the joint solver.
Jolt is the mature alternative (Horizon Forbidden West, Godot 4) and would be
the correct choice under a near-term ship date. Vendoring plus a self-controlled
update cadence is what makes the v0.1 risk acceptable here.

---

## Open questions — needed before F1 can be specced

1. **Scene file migration.** Widening `Transform` changes the serialized shape.
   Version-bump with an upgrade path, or a clean break? Pre-launch, a clean
   break is likely right and is cheap now and expensive later — but it is a
   decision, and `ReferenceProject` plus any Gacha scenes are the corpus.
2. **Does `SpriteRenderer` stay 2D-positioned, or become a billboard/quad in 3D
   space?** This determines whether 2D content shares the world depth buffer
   with meshes, which is what resolves F5 properly rather than by ordering. It
   also determines whether `Batcher2D` is a *world* renderer or a *screen-space*
   one — the ambiguity that made the 3D-over-2D question hard to answer at all.
3. **Does the orthographic camera path survive unchanged?** It is `glm::vec2`-
   shaped throughout and is the path every existing 2D scene uses. Preserving it
   byte-identically through F1 is achievable but should be an explicit
   requirement, as it was in Phase 4 Task 5.

---

## Recommended sequencing

1. **F1 alone** — the transform spine, gate green at every step, nothing else
   moving. ABI 15 → 16. Scene format decision applied.
2. **F2** — `MeshRenderer` + mesh/material assets, with the cook pipeline.
3. **F3** — bounds, frustum culling, draw sorting.
4. **F5** — declarative clear-op and the unified world pass.
5. **F4** alongside F2/F3 as authoring catches up.
6. **Box3D** after F1, as its own arc.
7. **Phase 4's Task 9** then becomes trivially correct, because the scene will
   have meshes to submit.

---

## What Phase 4 leaves behind

Tasks 1–8 stand. Two items pass to whoever picks this up:

- **Two desk-check items** that no automated signal in this repo can reach: the
  `frontCounterClockwise = true` + `CullMode::BACK` pair (derivation verified
  algebraically but never executed), and the fact that **the depth-attachment
  path has never run anywhere in this tree** — three `RenderGraphExec.cpp` sites
  go live with the mesh pass.
- **Task 10's desk checklist needs rewording.** Its camera-orbit item is
  unperformable (projection without pose), and its *"the game UI in Play still
  composites"* item names the ImGui half that works rather than the `Batcher2D`
  half that does not — as worded it would produce a false pass.
