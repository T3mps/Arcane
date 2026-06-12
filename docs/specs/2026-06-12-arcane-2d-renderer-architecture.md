# Arcane 2D Renderer Architecture — North Star

**Date:** 2026-06-12
**Status:** APPROVED — design contract for the renderer stack built on NVRHI, from M2a
through the lighting/GI milestones. Source: user architectural directive, 2026-06-12.
**Upstream:** `2026-06-11-engine-architecture-design.md` (module layout; this spec fills
the "render-graph internals beyond the seam" deferral), `2026-06-10-engine-thirdparty-
stack-design.md` (NVRHI/DXC/SDL3 stack).

## The prime rule: the NVRHI boundary

Decide the boundary with NVRHI and never cross back over it. NVRHI already provides:
device, command lists, PSOs, resource creation, binding layouts/sets, **automatic
resource-state tracking and barrier insertion**, descriptor tables including bindless,
and the DX12/Vulkan backends. Everything Arcane builds is **policy and orchestration on
top of NVRHI's mechanism**.

Hard prohibitions (review-enforced):

1. **Never wrap `nvrhi::ICommandList`** in our own recording abstraction — that loses
   the state tracker. Systems take `ICommandList*` and record into it.
2. **Never write our own barrier/state tracker.** `keepInitialState` + automatic
   barriers everywhere; explicit state calls only at documented seams.
3. **No descriptor-set abstraction below the bindless allocator.** NVRHI's binding
   sets/descriptor tables are the mechanism; we build only the bindless handle
   allocator + free-list on top.

Engineering weight lands in: **render graph, shader/PSO system, sprite batcher + sort
keys, the lighting/GI stack, and the post chain.** Budget those generously; the rest
hangs off them.

## The stack, bottom-up

### Core / framework layer (just above NVRHI)

- **Device & frame manager** — swapchain ownership, frames-in-flight ring, present,
  pacing, resize. (M1/M2a: lives inside the swapchain implementations; a DeviceManager
  shell emerges with multi-window/Grimoire. Donut's is reference, not gospel.)
- **Render graph (frame graph)** — the backbone. Pass declaration with read/write
  dependencies, transient-resource allocation + aliasing, pass culling, async-compute
  scheduling. The graph decides WHICH transient targets exist and WHEN compute overlaps
  graphics; NVRHI inserts the barriers. Sits on the state tracker, never fights it.
- **Transient & persistent resource pools** — buffer/texture suballocation, per-frame
  dynamic-data ring, staging/upload management, fence-tied GC (never free in-flight).
  (NVRHI's upload manager covers the M2a needs; pools formalize when the graph lands.)
- **Bindless descriptor system** — global descriptor table, handle allocator +
  free-list, shaders reference textures/buffers by index. Prerequisite for instanced
  batching v2, materials, and GPU-driven rendering.
- **Shader system** — DXC, one HLSL source -> DXIL + SPIR-V; permutation/variant
  matrix; reflection; PSO cache keyed on state+permutation; **async PSO compilation**
  (no hitches); hot reload in dev. One of the largest subsystems — do not under-budget.
- **Sync abstraction** — timeline-semaphore/fence wrappers for cross-queue ordering;
  async-transfer queue for streaming uploads off graphics.
- **Threaded command recording** — parallel pass recording into multiple NVRHI command
  lists; enkiTS integration; render-thread / sim-thread split.

### 2D rendering core

- **Sprite batcher** — the heart. v1 (M2a): CPU quad expansion, batch breaks on
  state/texture. v2: instancing with per-instance structured buffers (transform, UV
  rect, color, material index) + bindless to keep batches huge across textures. The
  submission API records draws; `End()` compiles — internals swap without API break.
- **Sort-key system** — 64-bit key per draw (sorting layer, order-in-layer, blend
  mode, material/PSO, texture), stable-sorted before submission. Drives correct
  transparency ordering AND minimal state changes. Contract: draws sharing
  (layer, order) may be reordered for batching — overlapping content takes distinct
  orders.
- **Sorting-layer / z-order model** — named layers + order-in-layer over actual depth;
  deterministic, float-precision-independent authoring.
- **Atlas & sprite-sheet management** — offline packing + runtime atlas allocator
  (text/decals) with rotation/padding/bleed. Bindless relaxes atlas pressure; keep
  atlases for batch coherence. (The client's tested skyline packer is the port seed.)
- **Camera & viewport system** — ortho cameras, multiple viewports, pixel-perfect
  snapping, render-to-texture cameras, shake, letterboxing.
- **Mesh/quad geometry generation** — quad, nine-slice, tiled, arbitrary 2D mesh;
  index/vertex pooling.

### Materials & 2D shading

- **Material system** — templates -> instances, parameter blocks (CBs or bindless
  material buffers), blend/PSO selection, texture slots; small definition format.
- **2D lighting** — point/spot/directional/freeform lights; normal-mapped sprites
  (normal + specular + emission + height/AO); per-light blend modes; tiled-or-additive
  light accumulation. This separates flat 2D from AAA 2D.
- **2D shadows** — caster geometry, soft shadows, falloff.
- **2D GI — radiance cascades.** The marquee modern 2D feature; design the lighting
  buffers around it EARLY even though it ships late.

### HDR, post, output

- **HDR pipeline** — linear RGBA16F scene targets (landed M2a), exposure/auto-exposure,
  tonemap; ping-pong target pool for post chains (render-graph transients).
- **Post-process stack** — composable effect graph as render-graph passes: bloom
  (energy-conserving down/upsample), tonemap operators, 3D-LUT grading, vignette,
  chromatic aberration, grain, distortion, stylized (CRT/scanlines). The client's 33
  GLSL effects are the porting backlog.
- **Anti-aliasing** — MSAA where geometry-heavy; FXAA/SMAA; optionally TAA (2D motion
  vectors from sprite transforms); FSR/DLSS if dynamic resolution.
- **Color management** — linear throughout, sRGB at the boundary (landed M2a:
  gamma-2.2 oracle parity; piecewise sRGB when banding appears), HDR10/scRGB output
  paths + wide gamut later. Get this wrong early and every asset is subtly off.

### Content systems

- **Text** — MSDF/SDF glyph atlas + font manager (NOT hand-rolled bitmap fonts);
  shaping/layout (HarfBuzz when non-Latin becomes a business reality — stack-spec
  deferral stands); rich-text markup; outline/shadow/gradient effects.
- **Particles** — GPU-driven (compute emit/simulate/sort + indirect draw), CPU fallback
  for gameplay-coupled emitters, lit particles in the 2D lighting.
- **Tilemap renderer** — chunked + culled, animated tiles, auto-tiling, streaming.
- **Skeletal/mesh 2D animation** — bone hierarchies, skinning, FFD; own vertex path.
- **Decals, trails/ribbons, vector/line rendering** — plus an always-available
  immediate-mode debug draw (lines, shapes, text).

### Streaming, culling, GPU-driven

- **Asset/texture pipeline** — async load, mips, BCn compression, streaming residency.
- **Culling & spatial structures** — ortho frustum culling, quadtree/grid, visibility
  feeding the batcher.
- **GPU-driven rendering** — indirect draw, GPU culling/compaction, draw merging.
  Bindless is the prerequisite.

## Milestone mapping (engine bring-up order, refined)

| Milestone | Renderer deliverables from this spec |
|---|---|
| **M2a** (planned) | FIF ring, shader system v1 (hot reload, PSO cache), linear-HDR canvas, ACES output, batcher v1 + **sort keys**, primitives |
| **M2b** | SDF/MSDF text + skyline atlas port, asset loaders v1, `imgui_impl_nvrhi` + upstream `imgui_impl_sdl3` |
| **M3** | Playground full scene (stack-spec gate), runtime backend swap, Tracy, cameras/viewports, debug draw |
| **M3.5/M4-adjacent** | **Render graph** + transient pools + ping-pong post seed; bindless allocator; batcher v2 (instanced + structured buffers); shader permutations + async PSO; threaded recording (enkiTS) |
| **Lighting milestone** | Materials, 2D lights/shadows, light buffers shaped for radiance cascades |
| **Later** | Radiance cascades GI, GPU particles, tilemaps, skeletal 2D, AA stack, HDR10/scRGB output, GPU-driven path, streaming |

Exact sequencing past M3 is set when those milestones are planned; the DEPENDENCIES are
binding (bindless before batcher v2 and GPU-driven; render graph before the post stack;
lighting buffers shaped for cascades before lighting ships).
