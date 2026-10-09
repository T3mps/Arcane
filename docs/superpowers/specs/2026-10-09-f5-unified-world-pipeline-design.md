# F5 unified world pipeline — design

**Date:** 2026-10-09
**Status:** APPROVED design 2026-10-09 (user). Plans are not yet written.
**Branch measured:** `feat/f5-unified-world` at `7e6f8881` (off `main`). Line citations below were re-read in this tree.
**Roadmap:** the F-series foundation that precedes D17 and D1 (`docs/superpowers/specs/2026-10-05-arcane-1.0-roadmap.md`).
**ABI:** `kGamePluginABIVersion` is 54 (`ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:1131`). Any bump is numbered against `main` at execution, not in this spec.

Binding rulings: Q1–Q7 and the adopted outside-review amendments (section 12). Material direction of 2026-10-02, with its exclusion of parts 3–4 superseded: F5 does all of it. Facts measured 2026-10-09, re-verified here before citation.

---

## 0. Status

Approved design, 2026-10-09, by the user. Sections 1–4 of the design discussion, the adopted amendments, and Q7 are closed. This document is that design. No plan has been written from it, and no product code lands with it.

Roadmap placement. D1 is the render foundation (clustered deferred, GGX, shadows, GTAO, TAA, the upscaler, and the Surface domain as a finished lighting model). D17 is large-world precision (64-bit positions, camera-relative rendering, origin rebasing). D3 is the render thread. F5 is the F-series foundation those rows attach to: one world pipeline, one material contract, one extract door, the deferred pass slots declared, and the TAA inputs reserved. D1, D17, and D3 each get built once, on this path.

What Source 2 / Deadlock does, and where this spec matches or diverges, is section 3.5. The standing rule (2026-09-04, restated in `docs/research/2026-09-16-direction-and-sequencing.md`) requires that of every design.

---

## 1. Goal and non-goals

### 1.1 Goal

World-space 2D draws through the 3D path. A sprite is a quad instance in the GPU scene, shaded by the same Surface material a mesh uses, depth-tested against the same depth buffer, in one world pass. Screen-space UI, text, and editor/debug chrome stay a later pass, the way Unreal keeps Slate/UMG and Source 2 keeps Panorama off the world colour.

Three contracts fall out of that, and F5 ships all three:

1. **One frame shape.** A game-thread extract produces an immutable packet. GPU-scene build, cull, the world pass, post, tonemap, the overlay, and screen UI read the packet and nothing from the ECS.
2. **One Surface material.** Domain, shading model, and blend mode are separate settings. Sprite and mesh are vertex-factory permutations of that one material. The output contract is the Unreal-shaped set, so D1's deferred lighting consumes it without a second material model.
3. **One sprite order.** Real view depth, then `sortingLayer`, then `orderInLayer`, then an optional per-camera sort axis, then a stable id. Masked and translucent sprites share that key.

Aphelyon is the product and is 2D. Its one sprite, viewed through its orthographic camera, looks the same after F5 as it does today (Q1). The old world path is deleted only after that parity holds (section 7).

### 1.2 Non-goals

F5 leaves these to the roadmap row named with them.

| Left to | What |
|---|---|
| D3 | A render thread, and parallel command recording. The packet is the seam D3 will move; F5 records the graph on the game thread, as today. |
| D1 | GGX / PBR lighting, the deferred G-buffer and its lighting pass, cascaded and local shadows, GTAO, TAA, the upscaler. F5's lit shading is Lambert from BaseColor. |
| D1 | Implementing TAA jitter or a velocity target. Both are reserved (G2). |
| D17 | 64-bit world positions, a moving render origin, tile-and-offset versus rebasing. F5's origin is the constant 0. |
| D11 / the tilemap epic | Tilemaps. No component, asset, or draw. A chunk row is reserved. |
| D1 | Shadow maps. A `casts-shadow` flag is reserved on sprite rows and is not read. |
| D7 | A UI material domain, and replacing ImGui. Screen UI stays the existing `gameui` / `imgui` nodes. |

Also out of this arc, and not given a roadmap row here: reverse-Z, MSAA, a second simultaneous world camera in one graph, GPU sorting of sprites, and any change to Manifold2D or Box3D.

---

## 2. Where it stands

Measured on this branch at `7e6f8881`. One function declares every host frame: `DeclareGraphFrame` (`ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp:1231`). `RenderGraph::AddNode` appends, and declaration order is execution order (`NriGraphContext.cpp:1486-1488`).

### 2.1 Frame order today

| Order | Node | Where it is declared | What it does |
|---|---|---|---|
| 1 | `batch2d` | `NriGraphContext.cpp:1277` | Creates the linear canvas and is the only node that clears it (`Batch2DNode.cpp:46-51`, `1395-1405`). No depth (`depthFormat = UNKNOWN`, `Batch2DNode.cpp:1313`). An empty batch still clears (`Batch2DNode.cpp:1407-1408`). |
| 2 | `gpuscene-sync` | `GpuSceneSyncNode.cpp:33` | Uploads rows the CPU already built. It does not walk the registry. |
| 3 | `mesh-cull` | `MeshCullNode.cpp:150` | Fills indirect counts. `numthreads(64)` (`MeshCullNode.hpp:29-30`). Dispatch only when `registryReady` and `rowCount != 0` (`MeshCullNode.hpp:39-42`). |
| 4 | `mesh` | `MeshNode.cpp:1301` | Creates `depth` (`MeshNode.cpp:1305-1311`), writes colour and depth (`MeshNode.cpp:1317-1318`), clears **depth only** (`MeshNode.cpp:942-957`). |
| 5 | readback nodes | `GpuSceneSyncNode.cpp:84` and `:124`, armed from `MeshNode.cpp:1345-1350` | Host copies, only when armed. |
| — | `depth` placeholder | `NriGraphContext.cpp:1352-1364` | A depth transient with no attachment, only when a frame asks for depth and has no mesh pass. |
| 6 | `grid` | `NriGraphContext.cpp:1392-1397` | The 3D grid. Depth-tested `LESS_OR_EQUAL`, depth write off (`GridNode.hpp:7-9`, `GridNode.cpp:363-375`). Colour goes into the canvas, so the post chain and the tonemap grade it. |
| 7 | `post0..` | `NriGraphContext.cpp:1412-1437` | Fullscreen passes (`FullscreenNodes.cpp:1055`). |
| 8 | `tonemap` | `NriGraphContext.cpp:1445` | Scene colour to the swapchain or the offscreen target (`FullscreenNodes.cpp:1506-1526`). |
| 9 | `gameui` | `NriGraphContext.cpp:1474-1475` | Game HUD on the tonemap target. The runtime never sets it (`NriGraphContext.cpp:1469-1472`). |
| 10–12 | `pick`, readback, outline | `NriGraphContext.cpp:1494-1507` | Its own id and depth targets. Clears both (`PickOutlineNodes.cpp:400`). |
| 13 | `imgui` | `NriGraphContext.cpp:1521-1522` | Host chrome on the tonemap target. |
| 14 | `capture` | `NriGraphContext.cpp:1524-1576` | Backbuffer readback. |

Attachments are load/store. The graph has no declarative clear (`NriGraphContext.cpp:1235-1252`). `Batch2DNode::Record` calls `CmdClearAttachments` on the colour plane after the raster pass is open (`Batch2DNode.cpp:1395-1405`). `MeshNode` does not clear colour, so `batch2d` sits under the mesh pass (`MeshNode.hpp:40-46`, `MeshNode.cpp:949-952`). That is why 3D draws over world sprites, and why anything declared before `batch2d` would be erased.

The same declarator serves every host. The runtime (`ArcaneRuntime/src/RuntimeFrame.cpp:508-749`) arms the batcher, the mesh pass when the scene is non-empty, post, and host `imgui` (which already includes the plugin `DrawUI`). It has no grid. The editor viewport (`ArcaneEditor/src/App/EditorAppFrame.cpp:1642-1758`) is a separate offscreen graph: scene batch, mesh, the 3D grid only in Edit + Perspective + show-grid (`EditorAppFrame.cpp:2097-2119`), post, `gameui` only in Play, pick/outline in Edit. Editor chrome (`EditorAppFrame.cpp:4016-4035`) sets `imgui` only; `DeclareGraphFrame` still emits an empty `batch2d` (the clear) then tonemap then `imgui`. Thumbnails and the shader-editor preview are their own offscreen graphs (checkerboard rects, an optional sprite quad, an optional post chain, an optional mesh).

### 2.2 Who submits 2D

`Batcher2D` is device-less CPU recording (`Batcher2D.hpp:190-200`). `Drain` stable-sorts by `layer(16) | order(16) | material(16) | textureSlot(16)` (`Batcher2D.cpp:363-368`, `424-431`). `worldSpace` is not in the key (`Batcher2D.cpp:373-381`). Overlapping translucency at the same `(layer, order)` is not a stable order: texture slots are first-use order inside one `Begin` (`Batcher2D.hpp:240-248`).

Screen primitives store `z = 0` and `worldSpace = false`. `QuadWorld` / `CircleWorld` store caller corners and `worldSpace = true` (`Batcher2D.cpp:248-279`, `416-422`). Screen `z` is hardcoded `0` in `data/shaders/sprite.hlsl:57-59`. World `z` is `mul(viewProj, pos)` (`sprite.hlsl:55-56`). The batch node has no depth attachment, so that clip `z` is not tested. The built-in sprite pipelines are alpha-over with depth write off (`Batch2DNode.cpp:1315-1316`).

| Producer | Space | Sort |
|---|---|---|
| Sprites (`RenderSubmissionSystem`, `RenderSystems.hpp:47-250`) | World metres. `QuadWorld` / `CircleWorld`. | `SetLayer(sortingLayer, orderInLayer)` cast `int32` to `uint16` (`RenderSystems.hpp:159-160`). Fields: `Components.hpp:150-151`. |
| 2D editor grid (`ViewportGrid.cpp:169-233`, called `EditorAppFrame.cpp:1883-1887`) | Screen. World lines projected, then `Line`. | Layer `(0, 0)`, before sprites. Edit + 2D + show-grid. |
| Physics debug (`EditorAppFrame.cpp:1893-1926`) | Screen, through `Affine2D`. | Does not call `SetLayer`. Inherits the previous layer. After sprites. |
| Camera rect (`EditorAppFrame.cpp:1950-1981`) | Screen. | No `SetLayer`. Edit only. After sprites. |
| Material / shader previews | Screen `Rect` and `QuadMaterial`. | Default layer `(0, 0)`. |
| Glyph / MSDF text | Screen API (`Batcher2D.cpp:182-191`). | No production caller under `ArcaneClient/src`. Tests call `Glyph`. |
| Transform gizmo | Not the batcher. Pixel `GizmoDrawSink`, painted as viewport foreground (`EditorAppFrame.cpp:1986-1990`). | Already after the scene image. |
| 3D reference grid | `GridNode`, not the batcher. | After `mesh`, before post. |
| Game / editor ImGui | `ImDrawData` after tonemap. | ImGui's own order. |

Runtime submission inside one `Begin`: globals, `SetViewProjection(View())`, `SubmitRender` (`RuntimeFrame.cpp:521-548`). No grid, physics overlay, or camera rect. Editor `SubmitSceneToBatcher` (`EditorAppFrame.cpp:1857-1984`): globals, view-projection, optional 2D grid, `SubmitRender`, optional physics overlay, optional camera rect.

Sprite shapes today (`Components.hpp:133-138`, `RenderSystems.hpp:167-208`): `Rect` is a textured quad; `Circle` is an SDF disc; `Capsule` is a band quad plus two discs. Circle and capsule ignore the sprite asset and draw a 1×1 m base times scale, so they stay on the collider.

Sprite cull is `MainVisibleSet` (`views[0]`). A null set culls nothing (`RenderSystems.hpp:57-63`).

There is no `Tilemap` type under Arcane `*.hpp` / `*.cpp`.

### 2.3 The mesh path

One GPU-scene row per `(entity, mesh section)`, 240 bytes, `static_assert` (`GpuSceneTypes.hpp:47-67`), mirrored by `data/shaders/gpu_scene.hlsli:15-29`. The row carries `model`, `prevModel`, CPU normal columns, world AABB (`boundsMax.w` = alpha cutoff), `baseColor`, `materialSlot`, `batch`, `flags`. The batch key is `(mesh, section, blend, twoSided)` (`GpuSceneTypes.hpp:69-75`). `MaterialBlendMode` is `Opaque = 0`, `Masked = 1`, `Transparent = 2` (`MaterialBlendMode.hpp:7-12`). F5 keeps those numeric values; section 5 names value 2 Translucent.

`PrepareSceneForRender` (`GpuSceneHost.hpp:87-110`) builds the visible set or sets, syncs the mirror, and builds the frame. `GpuSceneSync` walks the registry (`GpuSceneSync.hpp:92-111`, `203-227`). `BuildGpuSceneFrame` (`GpuSceneSync.hpp:285-391`) does not: it reads the mirror, the `VisibleSet`, and `MeshTable`.

Opaque and masked rows become indirect batches, opaque before masked, nearest first (`GpuSceneSync.hpp:349-357`). Transparent rows skip those lists and become direct draws sorted by `renderOrder` ascending, then `projectedDepth` descending, then entity id, then mesh guid, then section (`GpuSceneSync.hpp:380-388`). `projectedDepth = -viewCenter.z + depthSortBias` (`GpuSceneSync.hpp:324-328`). Ad-hoc rows are direct, submission order, opaque one-sided (`MeshNode.cpp:1217-1239`). One raster pass, six pipelines (`MeshNode.hpp:11-16`, `MeshNode.cpp:161-183`). Opaque and masked write depth. Transparent tests and does not write. Compare is `LESS` (`MeshNode.cpp:823-832`). Clear value is `1.0` (`MeshNode.cpp:77-83`).

Depth is `[0, 1]`, forward-Z (`SceneCamera.hpp:120-129`). `glm::perspectiveRH_ZO` (`SceneCamera.hpp:153-156`). Default format `D32_SFLOAT`. Canvas default `RGBA16_SFLOAT`. Both latched once per process (`NriGraphContext.hpp:286-293`). `mesh.hlsl` is one directional light, Lambert plus constant ambient, one albedo (`mesh.hlsl:184-206`). The light is `render.mesh.defaultLight.*`. There is no light component (`RuntimeFrame.cpp:676-681`).

### 2.4 Materials

`MaterialSurface` is `Fullscreen`, `Sprite`, `Mesh` (`MaterialSource.hpp:72`). There is no Surface value.

| Model | Compiles |
|---|---|
| Built-in 2D ids 0/1/2 (`Batcher2D.hpp:208-213`) | Offline `sprite`, `circle`, `msdf`. Alpha-over, no depth. |
| `.arcmat` kind `sprite` | Template `materials/sprite_material.hlsl` (`MaterialSource.cpp:333-334`), registered on the batcher. |
| `.arcmat` kind `fullscreen` | The post chain. |
| `.arcmat` kind `mesh` | **No template.** `MaterialTemplateFile` `ARC_ENSURE`s and falls back (`MaterialSource.cpp:313-334`). `GenerateGraphSnippet` refuses `Mesh` the same way (`MaterialGraph.cpp:480-482`). Drawn by the six fixed `mesh*.hlsl` pipelines plus a bindless albedo slot. |

The graph's output node is one colour (`MaterialGraph.cpp:86-88`). `VertexColor` and `SpriteTexture` are gated to the sprite surface (`MaterialGraph.cpp:107-109`, `116-118`).

### 2.5 Two cameras in one frame

Editor Edit already uses one view for both: the editor camera (`EditorAppFrame.cpp:1685-1686` and `1873`).

The runtime and Editor Play do not. Sprites use `Runtime::View()`, which `PushSceneCamera` sets from the first active orthographic camera (`RuntimeApp.cpp:846-861`, `SceneCamera.hpp:57-112`). Meshes use `ActivePerspectiveSceneCamera` when one exists (`RuntimeFrame.cpp:541-542`, `EditorAppFrame.cpp:1687-1688`). `SameView` false produces two CPU visible sets (`GpuSceneHost.hpp:93-97`). The comment on that function already says F5 unifies the passes under one view (`GpuSceneHost.hpp:13-14`).

`ReferenceProject/Content/scenes/main.arcscene` (the `runtime-scene` golden) has an active orthographic camera (`:12-18`), an active perspective camera (`:292-298`), sprites, two `MeshRenderer`s, and a `PostProcess`. `f3_cull_blend.arcscene` (the `f3-cull-blend` golden) has the same pair of active cameras (`:27-33`, `:66-72`) and twenty mesh renderers, no sprites. Aphelyon's `Content/scenes/test.arcscene` has one active orthographic camera (`orthographicSize` 5, `:40-46`) and one `SpriteRenderer` (`:89-106`, layer 0, order 0, `Rect`).

### 2.6 Extract, views, TAA reservations

Render nodes already consume snapshots on `FrameDesc`. The registry reads that still sit on the way into that snapshot are listed in section 4.2. They are the ones that move.

One `DeclareGraphFrame` call has one canvas size and at most one mesh view/projection. `SceneVisibility::views` index 0 is the main view; the comment reserves further views for shadows and previews (`VisibilitySystem.hpp:88`). Separate editor graphs (viewport, chrome, per-document preview, thumbnails) are separate frames, not extra cameras inside one graph. No `RenderTexture` component exists.

No TAA pass, no jitter on a projection, no velocity target. Reserved already:

- `Frustum.hpp:15-16`: extract from the unjittered view, so TAA jitter cannot flip a cull.
- `GpuInstance::prevModel` is uploaded (`GpuSceneTypes.hpp:50-55`, `gpu_scene.hlsli:18`). Contract: the model the row was drawn with last frame; a new row, a full rebuild, or a teleport stores `prev == model`, and a row with `prev != model` is re-dirtied next frame (`GpuSceneSync.hpp:15-19`, `:250-263`). No shader reads `prevModel` except the struct declaration.
- `NriGraphContext.hpp:387-402`: the frame is sliced by nulling optional fields, not by a stage ordinal. The comment names TAA, with shadows, GI, SSAO and SSR, as the reason "truncate at stage N" stops being a meaningful question once those passes exist.

### 2.7 What the goldens actually picture

`scripts/golden-gate.ps1:24-37` and `:344-394`. Both D3D12 and Vulkan. `runtime-scene` and `f3-cull-blend` are backend-split. `editor-ui`, `editor-ui-perspective`, `editor-input-doc`, `editor-asset-page`, and `editor-material-page` are shared. Witnesses that assert a compare: `W1`, `W3`, `W5`, `W6` (`ArcaneTests/src/WitnessScenariosTest.cpp`) and `E2`, `E3`, `E4`, `E6` (`EditorWitnessTest.cpp`). `W5` (`WitnessScenariosTest.cpp:307`) pins `f3-cull-blend`: `diffCount == 0` at `:385`, visibility counts at `:390-406`. The comment at `:315-318` records that the boot scene has no transparent, masked, or two-sided material. `:322-329` records that the fixture is twenty single-section meshes: fourteen cubes and six planes, one one-sided/two-sided pair per blend mode.

Aphelyon `Source/Game/Aphelyon.cpp:11-39` does not spawn entities and does not submit draws. `OnDrawUI` opens one ImGui window (`:33-39`), which joins whichever host ImGui context calls `DrawUI`. Tilemaps are a Jira epic, not code (`Aphelyon/docs/jira/epic-10-tilemap-overworld.md:10-16`: "the engine cannot render a map at all"). One `Cube.arcmesh` exists under Aphelyon's `Content/meshes/`. No second scene.

---

## 3. Architecture

### 3.1 The frame

End state, one graph:

```
extract (game thread)
  -> immutable RenderFramePacket
  -> GPU-scene build
  -> cull
  -> world pass          declarative clear, one depth
  -> post                existing chain, scene colour only
  -> tonemap             existing operator, scene colour only
  -> overlay pass        reads scene depth; after tonemap
  -> screen UI           game HUD, pick/outline, host ImGui
```

The world pass is one raster pass with one depth target. Its colour load-op clears the canvas to `render.clearColor`. Its depth load-op clears to `1.0`. `CmdClearAttachments` is gone for those two planes (`Batch2DNode.cpp:1405` and `MeshNode.cpp:957` are the calls that die). The graph gains a per-attachment load op and clear value; the seam documented at `NriGraphContext.cpp:1235-1252` is the one this closes. Pick's own targets (`PickOutlineNodes.cpp:400`) may use the same load op in the same plan. Their pixels stay as they are.

Draw order inside the world pass:

1. Mesh opaque and masked indirect batches, existing order (opaque before masked, nearest first), compare `LESS`, depth write on. Ad-hoc opaque rows draw with them.
2. Every sprite, as instanced draws in the unified key order (section 6). Compare `LESS_EQUAL`. Opaque and masked sprites write depth. Translucent sprites test and do not write.
3. Mesh translucent direct draws, existing sort (`GpuSceneSync.hpp:380-388`), compare `LESS`, depth write off.

Step 3 is what Q7 means by "later 3D transparents and particles occlude correctly": a masked sprite has already written depth, so a translucent mesh behind it fails the test. Particles, when D8 exists, join step 3.

The mesh row stays the 240-byte `GpuInstance`. Sprite rows are a second struct in the same GPU scene (section 6). They are coarse-culled by the same `VisibleSet` and then CPU-sorted. They do not enter `mesh-cull`'s indirect batches: an indirect batch grouped by mesh cannot express the key order. `mesh-cull` stays the opaque/masked mesh path.

Post and tonemap read scene colour and do not read or write the world depth. The depth transient's lifetime extends through the overlay. The overlay samples it as a texture. The overlay's target is the tonemap target (display-referred), so overlay primitives are display-referred when they land: each one is passed through the tonemap operator alone, and never through the post chain. A frame whose post chain is empty therefore keeps today's grid colour. A frame whose post chain is not empty keeps today's scene colour and stops grading the grid. That is the bug named in Q4.

Overlay primitive classes:

| Primitive | Test | Where it sits relative to world colour |
|---|---|---|
| 2D editor grid | Depth-tested `LESS_EQUAL`, no write, once sprites write depth (plan 3). In plans 1–2 sprites write no depth, so the 2D grid is not yet occluded by them. | After tonemap. Leaving the batcher's layer 0 is part of the explained grid diff (section 7). |
| 3D editor grid | Depth-tested `LESS_OR_EQUAL`, no write, the compare `GridNode.cpp:363-375` already uses. | After tonemap, over the scene where the depth test passes. Same occlusion against opaque and masked meshes as today. |
| Physics debug, camera rect | Always on top. | After tonemap. They are already drawn after sprites (`EditorAppFrame.cpp:1893-1981`). |
| Gizmo lines | Always on top. | The pixel positions `GizmoDrawSink` already produces (`EditorAppFrame.cpp:1986-1990`). Those pixels are already outside the post chain; plan 1 keeps them. |

`Batcher2D` remains the CPU producer of overlay and screen primitives (Q4). It stops being the producer of `SpriteRenderer` draws in plan 3.

Screen UI is unchanged in order and in target: `gameui`, then pick/outline, then host `imgui`, all on the tonemap target, all after the overlay. The chrome graph (editor ImGui with no scene) declares no world pass and no overlay.

### 3.2 Reserved D1 pass slots

Declared now, built by D1 except where a row says F5. This replaces the 2026-09-16 G1 order. The separate "2D-world" stage after translucency is gone: sprites are in the forward world pass.

| Slot | Who builds it | F5 |
|---|---|---|
| G-buffer | D1 | Declared, not built. |
| Lighting | D1 | Declared, not built. Lambert runs inside the forward world pass until then. |
| Forward world | F5 | Opaque and masked meshes, then the unified sprite list. |
| Translucent world | F5 for mesh translucency; D8 for particles | After sprites, so masked-sprite depth occludes them. |
| Post | already shipped | Scene colour only. |
| TAA and upscale | D1 | Reserved. The overlay note in section 3.4 sits on this slot. |
| Tonemap | already shipped | Scene colour only. |
| Overlay | F5 | Section 3.1. |
| Screen UI | already shipped | After the overlay. |

F5 implements the forward world path, the post/tonemap path that already exists, and the overlay. It does not implement the G-buffer or the lighting slot.

### 3.3 Render origin

Every position in the packet and in both GPU-scene row types is relative to a per-view render origin. F5 stores that origin as 0, so the values are world positions. The field exists so D17 does not invent a second row layout.

`prevModel` and `model` are relative to the same origin. A rebase (D17) rebases both, in the same extract, including the previous frame's stored `prevModel`. F5 does not rebase, and it does not rewrite rows into per-frame camera-relative positions: that would dirty every row whenever the camera moves. D17 chooses tile-and-offset versus rebasing. The row layout does not.

Culling extracts from the unjittered view (`Frustum.hpp:15-16`). With no jitter, that is the view itself.

### 3.4 Overlay depth and the TAA slot

The overlay reads scene depth at the resolution of the tonemap target, taken from the unjittered world view. F5 has no jitter and no upscale, so that depth is the world-pass depth.

The reserved TAA/upscale slot sits after the translucent world and before the overlay. When D1 fills it, that slot is what publishes the depth the overlay samples: unjittered, output resolution. The overlay does not sample the jittered depth and does not sample a pre-upscale depth. Jitter is applied to the world projection after the unjittered view has been handed to culling and to the overlay. `prevModel` on mesh rows, and the same field on sprite rows, is the velocity input; F5 still has no velocity target and no shader that reads the field.

### 3.5 Source 2 / Deadlock, and Unreal where the user pointed at it

The visual target is Deadlock / Source 2 the renderer (`docs/research/2026-08-12-deadlock-render-target.md`, `docs/research/2026-09-14-engine-ceiling-deadlock-and-box3d.md`). The material *shape* follows Unreal, by the user's direction of 2026-10-02. No Source 2 source is consulted. Unreal is read-only reference, as the 2026-08-12 policy already says.

| | Source 2 / Deadlock | This spec |
|---|---|---|
| World | One deferred renderer. G-buffer, then lighting, then a forward path for translucency and special passes. | The slot order in section 3.2 matches that shape. F5 fills the forward world path and leaves the G-buffer and lighting empty. |
| 2D in the world | Scene primitives through the world renderer, not a second batcher. | Sprites are GPU-scene instances in the forward world pass. |
| UI | Panorama composites apart from the world colour. | Screen UI stays after tonemap. The overlay is a third thing: editor and debug primitives, depth-tested or always on top, also after tonemap. |
| AA | No MSAA. TAA-family only. | TAA is reserved (G2) and not built. No MSAA is added. |
| Materials | Feature combos on `.vmat`, static and dynamic. | One Surface domain, permutations chosen by usage. The output pins are Unreal's, because that set is what a deferred lighting pass and a forward pass can share. That is the deliberate divergence from copying Source 2's combo language. |
| Lighting | Deferred GGX, baked GI. | Lambert from BaseColor until D1 swaps GGX inside the same template. The ceiling doc's rule stands: no second, half-PBR model beside `mesh.hlsl`. |

Unreal's Paper2D is the precedent for "a sprite is a mesh-pipeline primitive under an orthographic camera." Unreal's split of domain / shading model / blend mode, its vertex factories, and its specular default of 0.5 are the material contract in section 5. One divergence from Unreal is deliberate and is Q7: masked and translucent sprites share one sort key. A depth bias on masked sprites plus a separate translucent key lets a lower layer draw over a higher layer at the same z. This spec does not do that.

### 3.6 One world view

One graph has one world view and one depth. Selection:

- **Editor Edit.** The editor camera, 2D or Persp. Sprites and meshes already share it (`EditorAppFrame.cpp:1685-1688`).
- **Runtime and Editor Play.** The active orthographic scene camera when one exists (the camera `PushSceneCamera` already installs, `RuntimeApp.cpp:856-861`). Otherwise the first active perspective camera.

A perspective camera that is not the world view is not drawn into that graph. Edit + Persp still looks through the editor camera, which is unchanged.

Consequences, both of them plan 3, neither of them plan 1 or 2:

- Aphelyon's scene has only the orthographic camera. Its picture stays on that camera (Q1).
- `f3_cull_blend.arcscene` also has an active orthographic camera, and its witnessed picture is the perspective one. Plan 3 deactivates that orthographic camera in the fixture, so the rule above falls through to the perspective camera and the cull/blend picture stays the perspective picture. Plans 1 and 2 do not edit the scene and do not change camera selection, so the lane stays pixel-identical until that edit. The edit is an explained diff only if a pixel moves; deactivating an orthographic camera that the mesh pass does not use should move none.
- `main.arcscene` keeps its orthographic camera as the world view. Its perspective meshes join that view and the shared depth in plan 3. `runtime-scene` is re-blessed then, with that sentence as the note. Plans 1 and 2 still draw those meshes with `ActivePerspectiveSceneCamera`, so the golden does not move early.

`SceneVisibility::views` keeps the ability to hold more than one set (`VisibilitySystem.hpp:88`). F5 fills one. Further views are the multi-view seam (section 9).

### 3.7 Hosts that do not render

Extract runs only for a frame that will draw. ArcaneServer and any other host that does not record a graph do not call it and do not build a packet. A headless runtime does draw (`RenderFrameOffscreen`); it runs extract.

---

## 4. The extract boundary

### 4.1 The stage

One game-thread stage, called where `PrepareSceneForRender` is called today: after simulation has published `WorldTransform` and `WorldBounds`, before `DeclareGraphFrame`. It is the only stage that reads the registry for rendering.

It writes a `RenderFramePacket`. After it returns, the packet is immutable. It holds no `Astra::Registry`, no entity pointers, and no spans into component storage. Spans point at storage the packet owns. The packet lives until the next extract on that host (two packets, so the GPU can still be reading the previous one). Everything after extract — GPU-scene build, cull, every node, the overlay — reads the packet.

The packet carries:

| Field | Contents |
|---|---|
| World view | The view from section 3.6, its viewport, its projection, and its render origin (0). |
| Mesh rows | The staged `GpuInstance` rows, batch table, and draw args that `GpuSceneSync` / `BuildGpuSceneFrame` produce today, positions relative to the origin. |
| Sprite rows | Section 6. Empty in plans 1 and 2; the packet still carries the drained world quads those plans record through `Batcher2D`, so the pixels do not move. |
| Visibility | The `VisibleSet` for the world view. |
| Overlay primitives | 2D grid lines, 3D grid description, physics-debug lines, camera-rect lines, gizmo lines. Each primitive has a flag: depth-tested or always-on-top. |
| Pick drawables | The `PickDrawable` span `CollectPickables` builds today. |
| Post, tonemap inputs, UI | The post-chain bytes, the ImGui draw data, the pick selection. These are already snapshots; extract is where they enter the packet. |
| Light | The default directional light `RuntimeFrame.cpp:676-681` already resolves. No light component is added. |

GPU-scene build and cull become pure functions of the packet. `GpuSceneSyncNode` already refuses to invent rows the CPU did not stage; that stays.

### 4.2 Registry reads that move

These are the render-related registry reads in this tree. Each one moves into the extract translation unit. The line is the read, as re-verified.

| Site | What it reads | Notes |
|---|---|---|
| `RenderSystems.hpp:50-59` | `RenderContext2D`, `SpriteTable`, `SpriteMaterialTable`, `PhysicsInterpBuffer`; view `WorldTransform + SpriteRenderer + Not<Hidden>` | Sprite submission. The body through `:250` becomes extract's sprite-row builder in plan 3. In plans 1–2 extract still performs this read and the packet feeds `Batcher2D`. |
| `GpuSceneSync.hpp:92-111` | `MeshTable`, `MeshMaterialTable`; view `WorldTransform + WorldBounds + MeshRenderer + Not<Hidden>` | Mirror reconcile. |
| `GpuSceneSync.hpp:203-227` | `Changed<>` views of those three since `lastSyncTick`; per-row `GetComponent` | Dirty tracking. `BuildGpuSceneFrame` (`:285-391`) already avoids the registry except a `MeshTable*`. Extract copies the section ranges the builder needs into the packet, so the builder drops that pointer too. |
| `VisibilitySystem.hpp:97-129` | `SceneVisibility`; view `WorldBounds + Not<Hidden>` | `BuildVisibleSet` moves. `MainVisibleSet` becomes "the packet's set". |
| `GpuSceneHost.hpp:87-111` | `GetResource` / `EmplaceResource` of `SceneVisibility` and `GpuSceneMirror`; `GetResource<MeshTable>` | `PrepareSceneForRender` is the extract entry, or extract calls it. It stops being a second door. |
| `PickEmit.cpp:32-185` | `SpriteTable`; sprite view; `PhysicsResource`; collider view; `GetComponent<WorldTransform>`; `MeshTable`; mesh view | Pick drawables in the packet. |
| `SceneCamera.hpp:57-94` and `ActivePerspectiveSceneCamera` (`:206`) | `Camera`, `WorldTransform`, `Transform` | The functions stay in ArcaneCore. Extract becomes their only frame caller. `PushSceneCamera` (`RuntimeApp.cpp:846-861`) becomes "ask extract for the view and store it", at the same point in the frame, so gameplay that already ran Update is undisturbed. |
| `EditorAppFrame.cpp:1898-1922` | `PhysicsResource`, `RenderContext2D`, `PhysicsInterpBuffer` | Physics-debug primitives. The editor passes the flags (play mode, overlay mode, selection). |
| `EditorAppFrame.cpp:1953` | `ActiveSceneCamera` for the camera rect | The rect's corners are computed in extract. |

`Batch2DNode::Prepare` / `Record` already read drained spans and not the registry (`Batch2DNode.cpp:1254` onward, `:1390`). Nodes stay that way.

### 4.3 The include test

A Catch2 test walks the sources under `ArcaneClient/src/Arcane/Render/` and fails if a file other than the extract translation unit includes `Astra/Registry/Registry.hpp` or names `Astra::Registry`. `ArcaneClient/src/Arcane/Render/Nri/` has no allowlist entry: a node that grows a registry include fails the test. The extract TU is the one allowlisted file. The test is the gate that the list in section 4.2 stays moved.

Hosts (the editor, the runtime) may include the registry for gameplay. They do not, after this move, walk `SpriteRenderer`, `MeshRenderer`, `WorldBounds`, or `Camera` in order to build a frame. They call extract.

---

## 5. The Surface material model

This is the 2026-10-02 direction, including the parts that direction had parked. F5 implements all of it.

### 5.1 Settings

Three independent settings. The user never authors a vertex factory.

| Setting | Values | Meaning |
|---|---|---|
| Domain | `Surface`, `Post` | What the material produces. UI is a later domain (D7), not a value F5 adds. |
| Shading model | `Unlit`, `Lit` | Lit in F5 is Lambert from BaseColor (section 5.4). |
| Blend mode | `Opaque`, `Masked`, `Translucent` | `MaterialBlendMode::Transparent` (value 2) is Translucent. The enumerator may be renamed in the same ABI bump; the numeric value stays 2. Load accepts the old token `transparent` and the new token `translucent`. Save writes `translucent`. |

`MaterialSurface::{Fullscreen, Sprite, Mesh}` (`MaterialSource.hpp:72`) is replaced by `{Surface, Post}`. On-disk `.arcmat` kind `sprite` or `mesh` loads as domain Surface. Kind `fullscreen` loads as domain Post. New files write `surface` or `post`.

A sprite with no material, and a migrated sprite material that does not set a shading model, is **Unlit + Translucent**. That is today's alpha-over sprite (`Batch2DNode.cpp:1315-1316`), which is what Q1 requires for Aphelyon. Masked is the blend an author picks for cutout. A migrated mesh material is **Lit**, with the blend and two-sided flag the section already has.

Instances inherit the domain from the parent and have no domain field (`MaterialAsset.hpp` already treats `parent` as the instance link). Switching a material's domain keeps the graph. Nodes that do not fit the new domain become compile errors. Output pins that do not apply to the new domain grey out. Nothing is dropped.

### 5.2 Outputs

One output struct, forward- and deferred-agnostic. Unconnected defaults:

| Output | Default | F5 lighting reads it |
|---|---|---|
| BaseColor | `(1,1,1,1)`, times the sampled texture when one is bound | Yes. |
| Metallic | `0` | No. D1's GGX will. |
| Specular | `0.5` | No. The Unreal default, stored so D1 does not invent the pin. |
| Roughness | `0.5` | No. |
| Emissive | `0` | Added when the pin is connected. Unconnected, adding 0 is a bit-exact no-op on today's finite colours. |
| Opacity | BaseColor.a | Translucent blend. |
| OpacityMask | BaseColor.a | Masked clip against the existing cutoff (`boundsMax.w`). |
| Normal | unconnected | Unconnected leaves the vertex factory's geometric normal in place. The pin is tangent-space. A connected Normal on a vertex factory with no tangent frame is a compile error (the same "node does not fit" rule). No F5 default material connects it, which is what keeps `mesh.hlsl` pixels. |
| AO | `1` | No. |
| WorldPositionOffset | `0` | Applied in the vertex factory. Zero leaves positions unchanged. |

The default mesh lit colour is today's formula (`mesh.hlsl:184-206`):

```
albedo = sampled(UV0) * VertexColor * InstanceTint
lit    = albedo.rgb * (ambient + lightColor * saturate(dot(N, L)))
```

with `N` the geometric normal, `L` the existing default light, and alpha `albedo.a`. Opaque returns that. Masked clips on `alpha - cutoff` before it (`mesh.hlsl:216-228`). Translucent returns it with straight alpha and the existing alpha-over blend. Invalid material slot stays the flat path (`mesh.hlsl:192-203`): no sample, `VertexColor * InstanceTint` only.

The default unlit sprite colour is `sampled(UV0) * VertexColor * InstanceTint`, alpha-over, which is `sprite.hlsl:68-70` once the factory feeds the product below.

### 5.3 Vertex factories

Two factories. A material compiles once per factory it is actually used with. Usage chooses. The user does not.

| Input | Sprite factory | Mesh factory |
|---|---|---|
| VertexColor | White for a generated quad (no per-vertex paint exists on `SpriteRenderer` today). | White. |
| InstanceTint | `SpriteRenderer.tint`. | The row's `baseColor` (the multiply at `mesh.hlsl:203`). |
| UV0 | The atlas rect from the sprite asset. | The mesh UV. |

The product `VertexColor * InstanceTint` is the "tint × vertex colour" the sprite factory supplies. For every sprite authored today the vertex colour is white, so the product is the tint, and the pixel matches the batcher. A later gradient can set `VertexColor` per vertex without a new material setting.

The sprite factory also carries the shape (`Rect`, `Circle`, `Capsule`). `Rect` is the quad. `Circle` is the same SDF disc `CircleWorld` rasterises today (`RenderSystems.hpp:185-188`). `Capsule` is the band quad plus two discs, three instances, sub-index 0, 1, 2 in today's submission order (`RenderSystems.hpp:202-207`). They share the sort key, so they stay contiguous.

### 5.4 Interim Lit, and D1

Lit means Lambert from BaseColor, as above. It does not read Metallic, Specular, Roughness, Normal (beyond the geometric default), or AO. D1 replaces the body of the Lit template with GGX and re-blesses every lit golden. Unlit goldens stay. The template, the pins, the vertex factories, and the `.arcmat` settings do not change in that swap.

### 5.5 The six mesh shaders

`MeshNode`'s six pipelines (`MeshNode.hpp:11-16`) become the default Surface material compiled for the mesh vertex factory: Lit × {Opaque, Masked, Translucent} × {one-sided, two-sided}. Plan 2 deletes the fixed `mesh.hlsl` pixel paths as the source of those permutations and must reproduce them under the golden comparator on both backends (section 7). The PSO set stays six until a material's usage asks for another permutation. The live set F5 expects to compile is those six, plus Unlit Translucent for the sprite factory (the default sprite), plus Unlit Masked and Lit Translucent for the sprite factory when a material actually uses them. A permutation nobody uses is not compiled.

### 5.6 Editor

The material editor gains domain, shading model, and blend mode as three controls. An instance does not show domain. A domain switch keeps every node, marks the ones that do not fit, and greys the pins that do not apply. The mesh-material page, which today says the material is not compiled (`golden-gate.ps1:369-371`), compiles the Surface template from plan 2 on. Its golden is re-blessed only with that explained diff, in plan 2, if the compiled preview is part of the picture; the page's job in plan 2 is to show the same pixels the six shaders produced.

---

## 6. Sprites in the world

A sprite row is a GPU-scene struct of its own. It is not a widening of the 240-byte `GpuInstance` (`GpuSceneTypes.hpp:67`, `gpu_scene.hlsli:1-4`).

Fields:

- `model` and `prevModel`, both relative to the view's render origin (section 3.3). `prev == model` on a new row, a rebuild, and a teleport, the mesh contract (`GpuSceneSync.hpp:15-19`).
- Atlas UV rect, sprite-factory material slot, `InstanceTint`, shape, sub-index.
- The sort key inputs: view depth, `sortingLayer`, `orderInLayer`, sort-axis value, stable id.
- Flags. Bit 0 of the reserved set is `casts-shadow`, default false, read by nothing in F5.

View depth is `-viewSpaceZ` of the entity origin (the world-matrix translation column), the same quantity as a mesh row's `projectedDepth` (`GpuSceneSync.hpp:324-328`). The anchor is the entity origin so two sprites at the same Z tie exactly, including under an orthographic camera, and a bounds-centre offset cannot reorder them.

### 6.1 The key

One key for masked and translucent sprites (Q7). Drawn in this order, first = behind, last = on top:

| Component | Direction | Why |
|---|---|---|
| View depth | Farther first (larger `-viewSpaceZ` first) | Painter's order. Equal Z, the usual orthographic case, ties. |
| `sortingLayer` | Ascending `int32` | Higher layer draws later. Matches `Batcher2D`'s ascending key (`Batcher2D.cpp:424-431`) for every non-negative layer that fits in 16 bits, which is every authored value in ReferenceProject and Aphelyon (both 0). |
| `orderInLayer` | Ascending `int32` | Same. |
| Sort-axis value | Ascending | `dot(origin, camera.sortAxis)`. A zero axis (the default) contributes 0 and does not reorder. |
| Stable id | Ascending entity id, then sub-index | No flicker. Capsule parts stay in submission order. |

`sortingLayer` and `orderInLayer` stay `int32` (`Components.hpp:150-151`). The `uint16` cast at `RenderSystems.hpp:159-160` is not carried over. Values outside `0..65535` no longer wrap. Nothing in the authored scenes depends on the wrap.

The camera gains `glm::vec3 sortAxis`, default `(0,0,0)`, a reflected field. The ABI bump is numbered at execution. Y-sort on the +Y-up plane is the axis `(0, -1, 0)`: a smaller world Y sorts later and draws on top, which is the isometric convention for this handedness.

There is no depth bias and no depth-bit budget. Layer order at the same z is the key, for both blend modes. A higher-layer translucent sprite draws after a lower-layer masked sprite and passes `LESS_EQUAL` against the depth the masked sprite wrote, so it lands on top. The inverse draws the translucent one first, then the masked one overwrites it. One rule, both blends.

### 6.2 Drawing

CPU sort of the visible sprite rows. The plan-3 witness records the cost; the expectation going in is on the order of 1–2 ms for 50k keys, and the witness is what measures it (section 7). A later GPU sort has to reproduce this key. It is reserved, not built.

Instanced draws in key order. A run is a maximal contiguous span that shares vertex factory, material, shape, and pipeline state. The list is not regrouped by material first: that would break the key. 50k overlapping sprites of one material are one run. Aphelyon's art is translucent, so this sorted path is the path that matters (section 10).

Sprite pipelines use `CompareOp::LESS_EQUAL` against the depth meshes wrote in step 1 of the world pass. Mesh pipelines stay `LESS`. Opaque and masked sprites write depth. Translucent sprites do not. Default sprites are translucent, so they do not start occluding each other by depth; the key does that, which is today's painter's order.

Circle and capsule stay. They are sprite-factory shapes (section 5.3), Unlit, Translucent, in the same sorted list. Dropping them would fail parity for any scene that uses them to match a collider.

### 6.3 What Batcher2D keeps

After plan 3, `Batcher2D` records overlay primitives and screen-space work: the 2D grid lines, physics debug, the camera rect, preview checkerboards, preview quads, and the glyph API. It does not record `SpriteRenderer`. `QuadWorld` / `CircleWorld` for world sprites are gone. The screen `Quad` / `Rect` / `Line` / `Glyph` API stays.

### 6.4 Reserved row flags

`casts-shadow` on the sprite row, default false. A chunked tilemap row kind is reserved beside the sprite row and the mesh row: no fields are allocated and no shader is compiled. World-space UI, when it exists, is a quad through this same path (a later UI domain, or a Surface quad). It is not built. Screen UI is not that quad.

---

## 7. Gates and verification

`arc` can drive a gate when D27 exists. F5 does not wait for it and does not call it. The gates are `scripts/golden-gate.ps1` and ArcaneTests, on D3D12 and on Vulkan.

Goldens are re-blessed only with an explained diff, written in the bless note. An unexplained pixel fails the plan.

### 7.1 Plans 1 and 2 — pixel identity of world content

On both backends, world sprites and world meshes match the existing goldens. The one expected picture change is the editor grid leaving the post chain: grid pixels are identical when the post chain is empty (the overlay tonemaps them on their own, section 3.1), and they do not move when a post chain is added. `editor-ui` and `editor-ui-perspective` carry the boot scene's post process (`main.arcscene:27-31`), so those lanes' grid pixels are an explained re-bless. The 2D grid also leaves the batcher's layer 0, so a grid line that crosses a sprite is part of that same note.

Physics-debug lines and the camera rect leave the canvas with the grid. If a blessed frame contains them, they are in the same note. The gizmo is already outside the post chain; its pixels stay.

A test pins the bug directly: the same grid pixel with a post chain and without one is equal. World pixels in that test still show the post chain.

Plan 2's extra identity: the default Surface material reproduces the six mesh shaders. `f3-cull-blend` and `W5` stay at `diffCount == 0` on both backends. Lit goldens are not re-blessed in F5. D1 re-blesses them when Lambert becomes GGX.

### 7.2 Plan 3 — parity, then delete

The old world path is deleted only after every one of these has passed.

**Parity.** Every existing 2D scene renders equivalently on the new path and the old path, including ReferenceProject's sprites viewed through the orthographic camera and Aphelyon's one sprite. "Equivalently" means sprite pixels that no mesh occludes match the old batcher, and sprite pixels a mesh occludes match the depth rule in section 6 (a closer opaque or masked mesh hides the sprite; a farther one does not). The two-camera composite in today's `runtime-scene` is not the parity picture: plan 3 re-blesses that golden with the section 3.6 note.

**Stress scene.** Many sorting layers, Y-sorted overlaps, and mixed masked and translucent sprites at the same z. The picture is checked against a CPU oracle of the key in section 6.1, not against the old batcher (the old key cannot express Y-sort or "masked and translucent at the same z"). Each sprite carries a flat colour so the order is visible.

**Perf witness.** 50k overlapping translucent sprites, old path versus new path, both backends. Records frame time and CPU sort time. Gates plan 3: the old path is not deleted if the new path is slower than the old path by more than the sort itself. The sort is the expected added cost, on the order of 1–2 ms for 50k keys; the witness records the actual number.

**Witnesses**, each a Catch2 case unless noted:

| Witness | Asserts |
|---|---|
| Y-sort / layer / order matrix | The key order for a table of layers, orders, equal z, and a non-zero sort axis. |
| Sprite occluded by a mesh | A mesh in front hides the sprite; a sprite in front (closer view depth, `LESS_EQUAL`) draws over the mesh. |
| Lit opt-in sprite | An Unlit sprite and a Lit sprite of the same BaseColor differ by the Lambert term, and the Unlit one matches the default sprite. |
| Extract include test | Section 4.3. |
| Overlay depth-tested vs always-on-top | One line behind a mesh is hidden when depth-tested and visible when always-on-top. |
| Grid unaffected by post | Section 7.1. |
| Stress parity | The scene above. |
| Perf, 50k | The witness above. |
| Goldens | Plans 1–2 identity; plan 3 explained re-blesses only. Both backends. |

---

## 8. Phased delivery

Three plans, in this order (Q6). Each plan leaves the tree green on both backends before the next starts. ABI bumps, if a reflected field or a game-visible header changed, are taken in the plan that changes them and numbered against `main` at that time. Known causes: `MaterialSurface` becoming `{Surface, Post}` in a public header, and `Camera::sortAxis`. This branch is ABI 54.

### 8.1 Plan 1 — structure

- Declarative clear on the world colour and the world depth. The mid-pass `CmdClearAttachments` for those planes goes away.
- The world pass is the mesh pass under that clear. Sprites still reach the GPU through `Batcher2D`. Camera selection is unchanged, so `runtime-scene` and `f3-cull-blend` world pixels stay.
- Extract and `RenderFramePacket`. The reads in section 4.2 move. The include test lands. The packet's sprite payload is still the batch the batcher drains.
- The overlay pass after tonemap: 2D grid, 3D grid (GridNode folds in), physics debug, camera rect, gizmo lines. Screen UI stays after it.
- Non-rendering hosts do not call extract.
- Gate: section 7.1.

### 8.2 Plan 2 — materials

- Domain, shading model, blend mode. `.arcmat` migration. Instances inherit domain. Domain switch keeps the graph, errors the nodes that do not fit, greys the pins that do not.
- The Surface template, both vertex factories, the output struct in section 5.2.
- The six fixed mesh shaders become the default Surface material and reproduce today's pixels under the golden comparator, both backends.
- The default sprite factory permutation reproduces `sprite.hlsl` for Unlit Translucent, so a sprite preview does not churn.
- Editor controls (section 5.6).
- Meshes look identical. Sprites are still drawn by the batcher. Lit is Lambert. No golden is re-blessed for a lighting change.
- Gate: section 7.1, plus the six-shader identity.

### 8.3 Plan 3 — sprites, parity, retirement

- Sprite rows, the key, the CPU sort, the instanced draws, circle and capsule, `LESS_EQUAL`, masked depth writes.
- One world view (section 3.6), including the `f3` fixture edit.
- The parity gate, the stress scene, and the 50k witness, all green, both backends.
- Then `Batcher2D` retires from world content (section 6.3). Not before.
- `runtime-scene` re-bless, explained. `f3-cull-blend` stays the perspective picture.
- Aphelyon's sprite matches the old path.

---

## 9. Reserved seams

Named so a later arc extends them instead of replacing the pipeline.

| Seam | What F5 leaves in place | Who fills it |
|---|---|---|
| Render origin | Per-view origin on the packet and both row types. Value 0. `prevModel` and `model` share it. A rebase moves both. | D17. Tile-and-offset versus rebasing is D17's choice. |
| TAA jitter and velocity | Unjittered cull (`Frustum.hpp:15-16`). `prevModel` on mesh and sprite rows, same origin as `model`. No jitter, no velocity target. | D1, at the slot in section 3.4. The overlay samples the unjittered output-resolution depth that slot publishes. |
| D1 pass slots | The order in section 3.2. G-buffer and lighting declared empty. | D1. |
| Multi-view | `SceneVisibility::views` can hold more than one set. F5 fills the world view only. | Shadows, extra cameras, render-to-texture. |
| Render thread | The packet is immutable and contains no registry. Nodes already read snapshots. | D3 moves everything after extract off the game thread. |
| `casts-shadow` | A flag on the sprite row, default false, not read. | D1 shadows. |
| Tilemap chunk row | A row kind reserved next to mesh and sprite. No struct body, no shader. | The tilemap epic / D11. |
| World-space UI | A quad through this path, later. | D7's UI domain, or a Surface quad. Screen UI is not this. |
| GPU sprite sort | The CPU key in section 6.1 is the contract. | When the 50k witness says the CPU sort is the wrong place. |

---

## 10. Risks and mitigations

| Risk | Why it is real | Mitigation |
|---|---|---|
| Aphelyon's art is translucent, so the sorted path is the main path, not a rare one. | The one Aphelyon sprite is a default sprite, and default is Unlit Translucent. A sort bug is a game bug. | Q7's single key. The stress scene mixes masked and translucent at the same z. Parity runs on Aphelyon's scene before the old path is deleted. |
| CPU sort cost at 50k. | Overlapping translucent sprites do not instance across a material change, and the sort is on the game thread. | The 50k witness gates plan 3. The old path stays if the new path loses more than the sort. GPU sort is reserved on the same key. |
| Vertex-factory permutation count. | Domain × shading × blend × factory × sidedness is a large product. | Compile on use (section 5.5). The live set is the six mesh permutations plus the sprite permutations something actually references. |
| `.arcmat` migration of user content. | ReferenceProject and Aphelyon both have `sprite` and `mesh` kinds on disk. A bad load is a blank material. | Load maps `sprite`/`mesh` → Surface and `fullscreen` → Post. Save writes the new tokens. Instances inherit domain and are not rewritten into a domain of their own. A domain switch keeps the graph. |
| Golden churn. | The grid leaves the post chain in plan 1. The boot scene's two cameras become one view in plan 3. | Plans 1–2 re-bless only the grid class of diff, with a note, and pin it with a test. Plan 3 re-blesses `runtime-scene` with the section 3.6 note. Any other diff fails the plan. Lit goldens are not touched until D1. |
| Two cameras in `main.arcscene` and `f3`. | One depth cannot be both projections. | Section 3.6. Plans 1–2 do not change selection, so neither golden moves then. |
| 2D grid coverage. | The 2D grid was batcher layer 0, under sprites. The overlay is after tonemap, so a line can cross a sprite it used to sit under. | Named in the plan-1 bless note. The pinned test is the post-chain half, which is the bug. Plan 3's masked sprites occlude a depth-tested grid; translucent sprites do not write depth, so the grid shows through them. |
| Extract misses a read. | A registry read left behind compiles, and the include test is the only thing that catches the next one. | Section 4.2 is the move list. Section 4.3 fails CI on a new include outside the extract TU. |
| Circle and capsule parity. | They are not quads. A quad-only row would move every collider-matched sprite. | They are sprite-factory shapes in the same sorted list (section 5.3). |
| Default Lit on meshes, default Unlit on sprites. | Using one default for both would move either the mesh goldens or Aphelyon. | Section 5.1. The template is shared; the setting is not. |

---

## 11. Out of scope

Recorded so a plan does not pick them up.

- The render thread and parallel command recording (D3).
- GGX, the G-buffer, clustered lighting, shadows, GTAO, TAA, the upscaler (D1).
- 64-bit positions and a moving origin (D17).
- Tilemaps, beyond the reserved row kind.
- A UI domain and the ImGui replacement (D7).
- Reverse-Z, MSAA, a velocity target, GPU sprite sorting.
- A second world camera drawn into the same graph.
- Changing `mesh.hlsl`'s Lambert result in F5. D1 changes it, inside the template, and re-blesses.
- Rewriting Aphelyon content. The one sprite and the one camera are a parity input, not a content task.
- `arc` (D27). Helpful when it exists, not a dependency.

---

## 12. Rulings ledger

Each line is the user's pick on 2026-10-09. The amendments were adopted from the outside review the same day. Q7 replaces the depth-bias sentence that was attached to Q2; the precedence Q2 named is unchanged.

| # | Pick |
|---|---|
| Q1 | Sprites are Unlit by default. Lighting is an opt-in per material. Aphelyon sees no visual change. This answers G4: no separate 2D lighting path, and T1's light grid stays with D1. |
| Parity | Existing 2D content renders correctly on the new path before the old path is retired. |
| G1 | The 2026-09-16 separate "2D-world" stage is superseded by unification. |
| Q2 | Order is real z, then `(sortingLayer, orderInLayer)`, then an optional per-camera sort axis (Y-sort). |
| Q3 | The 2026-10-02 Surface model, in full, including the parts that note had excluded. One template, sprite and mesh vertex factories, Unreal-shaped outputs, Unlit plus interim Lambert Lit, the six mesh shaders become the default Surface material, `.arcmat` migrates. |
| Q4 | One debug/editor-primitive pass after tonemap, reading scene depth. Each primitive is depth-tested or always-on-top. `Batcher2D` stays the CPU producer. The 3D grid folds in. Screen UI composites after it. A test pins that a post chain does not affect the grid. |
| Q5 | One game-thread extract owns every render-related registry read and writes an immutable packet. Everything after reads the packet. A test enforces that render-side code does not include the registry. No render thread in F5. |
| Q6 | Plan 1 structure, plan 2 materials, plan 3 sprites plus parity plus `Batcher2D` retiring from world content. |
| Q7 | One sort key for masked and translucent sprites: view depth, then `sortingLayer`, then `orderInLayer`, then sort-axis value, then stable id. Instanced draws in that order. `LESS_EQUAL` against meshes. Masked sprites write depth. No depth-bit budget. CPU sort now. GPU sort later. |
| Amendment | The packet and the rows carry positions against a per-view render origin, 0 until D17. `prevModel` and `model` share the origin. A rebase rebases both. |
| Amendment | The overlay's depth test against TAA jitter and upscale is noted on the reserved TAA slot. Overlays need unjittered, output-resolution depth. |
| Amendment | Dedicated servers and other non-rendering hosts skip extract. |
| Amendment | `VertexColor`, `InstanceTint`, and `UV0` come from the vertex factory. The sprite factory supplies tint × vertex colour and the atlas UV. The mesh factory supplies white and the mesh UV. |
| Amendment | D1 re-blesses the lit goldens when Lambert becomes GGX. |
| Amendment | Plan 2 reproduces the six fixed mesh shaders under the golden comparator. |
| Amendment | Specular stays as a pin with Unreal's default, 0.5. |
| Amendment | No tilemaps exist. A chunked row is reserved. |
| Amendment | A `casts-shadow` flag is reserved on sprite rows. |
| Amendment | Plan 3 gates on the 50k perf witness, the stress parity scene, and per-backend goldens for D3D12 and Vulkan. |
| Amendment | `arc` is helpful and is not a dependency. |

---

## 13. Amendments applied to the binding docs

Applied in the same change as this file. Each edit points back here.

**`docs/research/2026-09-16-direction-and-sequencing.md`.**

- G1's guard no longer says F5 implements a separate 2D-world stage after forward/transparent. It now says the declared order is G-buffer → lighting → forward world → translucent world → post → TAA (reserved) → tonemap → overlay → screen UI, that F5 implements the forward world path and the overlay, and that world sprites live in the forward world pass.
- The F5 bullet in the order list points at this spec: declarative clear, one world pass, one depth, 2D drawn through the 3D path. G1 (as amended) and G2 live here.
- G4's open question is closed: answered 2026-10-09 by F5 Q1, unlit by default, lit opt-in on the same Surface material, no separate 2D lighting path, T1's light grid stays with D1. The step-6 sentence and the "what would change this order" bullet that still treated that decision as open say the same.

**`docs/research/2026-08-21-3d-foundations-assessment.md`, F5 section.**

- A supersession line at the F5 heading: the "both sort into a shared depth" bridge is not the design; this spec is. The clear still moves off `CmdClearAttachments`. The mid-pass clear cite is corrected from `Batch2DNode.cpp:1321-1325` to `Batch2DNode.cpp:1395-1405`. The clear-seam cite is corrected from `NriGraphContext.cpp:1123-1163` to `NriGraphContext.cpp:1235-1252`.
- The open question "does `SpriteRenderer` stay 2D-positioned?" is answered: it is a world quad in the shared depth buffer, drawn through the 3D path. The sequencing line for F5 points here.

**Material direction of 2026-10-02** (`D:/dev/starworks/Arcane/.superpowers/sdd/2026-10-09-f5-spec/material-direction-2026-10-02.md`).

- The exclusion of parts 3–4 is superseded. F5 implements parts 1, 2, 3, 4, and 5. The file points at this spec. That file lives outside this repository; the sentence is edited there, and this section is the copy that lands in git.
