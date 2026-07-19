# Entity-ID (Hit-Proxy) Viewport Picking — Arcane engine

**Date:** 2026-07-19
**Status:** Design — approved (approach + on-demand + replace-CPU-pick decided); awaiting spec review.
**Layer:** Arcane engine (`ARCANE_API` render + pick), consumed by Grimoire. Prereq for SPEC #2 (gizmos).

---

## 1. Why

Grimoire's viewport pick (`Arcane::PickEntitiesAt`, a CPU test of `SpriteRenderer` OBBs) finds
nothing for the Sandbox physics bodies — they carry no `SpriteRenderer`; they are drawn by the
physics-debug overlay. So today you must select via the Hierarchy. Gizmos (SPEC #2) need
click-to-select in the viewport.

We adopt the **hit-proxy** approach used by Unreal (`HHitProxy`) and Unity's scene view: render every
pickable entity's silhouette into an offscreen **ID buffer** (each entity "colored" with a unique id),
then read back the pixel under the cursor. Chosen deliberately because it is **dimension-agnostic** —
the pass, ID encoding, and readback are identical in 3D; only the geometry each entity emits changes
(quads/colliders now, mesh triangles later). The user is moving to 3D, so we build the pattern that
survives that transition rather than a 2D-only collider test.

This **replaces** the CPU `PickEntitiesAt` for the viewport (it covered only sprites; the ID buffer
covers sprites *and* colliders and is pixel-exact).

---

## 2. Grounding (verified against the tree)

- Readback pattern (production-ready, from `Arcane/Tests/src/Helpers/GpuTestHelpers.hpp`):
  `createTexture(isRenderTarget)` → `createStagingTexture(desc, CpuAccessMode::Read)` → command list
  `clear…` + `copyTexture(staging, slice, target, slice)` → `executeCommandList` + `waitForIdle()` →
  `mapStagingTexture(staging, slice, Read, &rowPitch)` → read → `unmapStagingTexture`. `copyTexture`
  accepts a `TextureSlice` sub-region, so a **1×1 copy** under the cursor is supported.
- `Arcane::OffscreenCanvas` (`Arcane/Render/OffscreenCanvas.hpp`): `Create(device, shaders, w, h)`,
  `Draw(FunctionRef<void(Batcher2D&)> fn, clear)` runs canvas→batcher→tonemap into a display texture,
  `TextureId()` for `ImGui::Image`, `Resize(w,h)`. Game-agnostic (caller supplies the draw lambda).
  The ID pass is a **sibling** to this — its own `R32_UINT` target, not the tonemapped display texture.
- Current pick: `Arcane::PickEntitiesAt(registry, worldPoint)` in `Arcane/Scene/EntityPick.{hpp,cpp}`
  (+ `EntityPickTest.cpp`); called in `GrimoireApp.cpp`. This is what we replace.
- Shaders are HLSL data in `Arcane/shaders/`, compiled (DXIL+SPIR-V) by the Arcane prebuild. A new
  `entity_id.hlsl` follows the same artifact conventions as `msdf.hlsl`/`imgui.hlsl`.

---

## 3. Architecture

A new engine component, **`Arcane::PickBuffer`** (`ARCANE_API`), owns the hit-proxy machinery. It is a
sibling of `OffscreenCanvas` (created and resized alongside the viewport), and exposes one on-demand
operation: given a viewport pixel, render the ID pass and return the entity under it.

```
Grimoire viewport click (viewport-local pixel)
      │
      ▼
PickBuffer::Pick(registry, view, pixel) ──► Astra::Entity  (0/null == background; physics via registry's PhysicsResource)
      │  1. build ID->Entity table while emitting
      │  2. render EntityId pass into R32_UINT target (cleared to 0), front-most wins
      │  3. copyTexture 1x1 @pixel -> staging -> map -> uint id
      │  4. table[id] -> entity
```

### 3a. The EntityId pass (the hit-proxy pass)
- Target: an `R32_UINT` render target, viewport-sized, owned by `PickBuffer`, rebuilt on `Resize`.
  Cleared to `0` (`clearTextureUInt`) each pick — `0` is reserved for "no entity" (background).
- A dedicated pipeline + shader `entity_id.hlsl`: a minimal VS (position via the same world→canvas
  view transform the scene uses) + PS that writes a per-draw **uint id** to the `R32_UINT` target.
  Depth-tested so the front-most entity wins a contested pixel (2D: draw order / sortingLayer maps to
  depth; 3D: real depth — same mechanism, which is the point).
- The pass is **on-demand**: rendered only inside `Pick()`, so there is zero per-frame cost. (Per-frame
  rendering for hover-highlight is a clean later add — §7.)

### 3b. The emitter seam (what generalizes to 3D)
`Pick()` iterates pickable entities and, for each, emits its silhouette geometry tagged with its pass id.
Two 2D emitters today:
- **Sprite emitter:** entities with `WorldTransform` + `SpriteRenderer` → the sprite quad (the same OBB
  `PickEntitiesAt` used, now rasterized).
- **Collider emitter:** entities with a live physics body (`PhysicsBodyRef` in `PhysicsResource`) →
  each `Collider2D` fixture's shape (circle/box/capsule/polygon), reusing the physics-debug shape
  tessellation so the ID silhouette matches what the overlay draws.

The seam is a list of emitters; **3D adds a mesh emitter behind the same interface** — the pass, target,
encoding, and readback are untouched. This is the whole reason for choosing hit-proxy over collider-only.

### 3c. ID ↔ Entity mapping (Unreal's hit-proxy table)
Rather than bit-pack `Astra::Entity` into the pixel, `Pick()` builds a `std::vector<Astra::Entity>` as it
emits: the k-th drawn entity gets pass id `k+1`; `0` = background. Readback uint → `table[id-1]`.
Sidesteps entity generation-bit packing and matches how Unreal maps a hit-proxy id back to an
`HHitProxy`. The table is per-`Pick()` (rebuilt each call); on-demand picking means no persistence needed.

### 3d. Readback (on-demand, one stall per click)
After the pass renders, `copyTexture` a **1×1 `TextureSlice` at the pixel** into a 1×1 `R32_UINT` staging
texture, `waitForIdle()`, `mapStagingTexture`, read the uint, unmap. A single synchronous stall per
*click* is acceptable (it is not a per-frame path). Reuses the exact NVRHI sequence in `GpuTestHelpers`.

---

## 4. Public API (`ARCANE_API`)

```cpp
// Arcane/Render/PickBuffer.hpp  (engine; Grimoire consumes it, mirrors OffscreenCanvas ownership)
namespace Arcane
{
    class PickBuffer
    {
    public:
        static std::unique_ptr<PickBuffer> Create(nvrhi::IDevice*, ShaderLibrary&,
                                                   uint32_t width, uint32_t height);
        virtual ~PickBuffer() = default;

        // Render the EntityId pass for the current scene and return the entity whose
        // silhouette covers `pixel` (viewport-local, y-down, matching the canvas). Returns
        // a null/invalid Astra::Entity when the pixel is background. `view` is the same
        // world->canvas transform the scene render uses (camera offset + pixels-per-meter).
        virtual Astra::Entity Pick(Astra::Registry& registry,
                                   const PickView& view,
                                   glm::vec2 pixel) = 0;

        virtual void Resize(uint32_t width, uint32_t height) = 0;
    };
}
```

`PickView` carries the world→canvas mapping (camera offset + scale) — sourced from the same
`RenderContext2D`/camera Grimoire already feeds the scene render, so the ID silhouettes register
pixel-for-pixel with what is on screen. Physics access is via the registry's `PhysicsResource` (the
collider emitter reads it), so the signature stays registry-centric.

*(Exact type of `view` and whether physics is reached via the registry resource or passed explicitly is
finalized at plan time against `RenderContext2D` — see §8.)*

---

## 5. Grimoire integration

- Own a `PickBuffer` beside the `OffscreenCanvas` (`Create` at the same size, `Resize` together).
- On a viewport left-click (already remapped to viewport-local pixels, gated on viewport-active), call
  `m_pick->Pick(registry, view, pixel)` and set the editor selection to the result — an invalid entity
  **clears** the selection (click on empty space deselects, matching editor convention).
- Delete the viewport call to `PickEntitiesAt`; retire `Arcane/Scene/EntityPick.{hpp,cpp}` and
  `EntityPickTest.cpp` (no other consumer — confirm with a grep at plan time; if one exists, keep the
  file but drop Grimoire's use).
- Alt-click cycling (front-most → next) that the CPU pick offered is **dropped for v1** (the ID buffer
  returns the single front-most entity; cycling through occluded entities is a later enhancement, §7).

---

## 6. Testing

- `[gpu]`-tagged (needs a device, like the other render tests; excluded by `~[gpu]` dev-loop):
  - Two non-overlapping bodies at known screen pixels → `Pick` returns each correctly; a pixel between
    them → invalid (background).
  - Overlapping bodies → `Pick` returns the front-most (higher draw order / depth).
  - A sprite entity and a collider entity → both pickable through the one path.
  - `RenderErrorCount() == 0` after each (validation-silent, per the GPU-test rule).
- Headless unit test: the ID↔Entity table encode/lookup (id `k+1`, `0`=background, out-of-range → invalid).

---

## 7. Non-goals / deferred (same seam, later)

- **Hover-highlight** (per-frame ID render) — v1 is on-demand click only.
- **3D mesh emitter** — added behind the emitter seam when 3D lands; no pass/readback change.
- **Occlusion cycling** (alt-click through stacked entities) — v1 returns front-most only.
- **Marquee / multi-select** — later.
- **Gizmo-handle hit-testing** — SPEC #2, geometric (analytic ray-vs-handle), not this buffer.

---

## 8. Impl-time verification points (shape unaffected; resolve while planning)

1. **`entity_id.hlsl` id plumbing** — how the per-draw uint id reaches the PS (per-instance vertex
   attribute vs per-draw constant), and whether the pass reuses `Batcher2D` geometry or a dedicated
   id-geometry submission. Confirm `R32_UINT` render-target + `clearTextureUInt` are supported on both
   backends (D3D12 + Vulkan) with validation silent.
2. **Collider silhouette reuse** — the exact physics-debug shape tessellation entrypoint to reuse for
   circle/box/capsule/polygon, so ID silhouettes match the overlay. (Polygon fixtures remain a known
   engine gap — a polygon-collider entity may pick by its AABB until authored-poly support lands.)
3. **`PickView`/camera source** — the precise `RenderContext2D`/camera value Grimoire passes so the ID
   transform matches the scene render exactly (offset + pixels-per-meter, y-down).
4. **`EntityPick` retirement** — grep for any non-Grimoire consumer before deleting.
