# Selection + Hover Outline (GPU edge-detect) — Arcane engine

**Date:** 2026-07-21
**Status:** Design — approved (AAA edge-detect route; picking + outlining separate; exterior outline; single selected + single hovered v1). Awaiting spec review.
**Layer:** Arcane engine (`ARCANE_API` render), consumed by Grimoire. Builds on the entity-id picking buffer (`2026-07-19-arcane-entity-id-picking-design.md`) and the offscreen output framebuffer (`2026-07-20-grimoire-game-imgui-viewport-design.md`).

---

## 1. Why

Grimoire has no visual selection feedback today — the only cue that an entity is selected is the gizmo appearing at its transform. There is no hover feedback at all. We want:

- **Select-highlight** — a persistent outline around the selected entity (amber).
- **Hover-highlight** — an outline around the entity under the cursor (cyan), updating as the mouse moves.

The two states use the same outline style, different colors.

We do this the way AAA engines do: **picking and outlining are separate systems.** Picking ("what did I click") stays an on-demand hit-proxy readback. Outlining ("draw borders around selected + hovered") is a per-frame GPU screen-space edge-detect on the same hit-proxy id buffer — the Unreal (custom-depth/stencil + post-process edge-detect) / Unity (mask + outline post-effect) model. No per-frame CPU readback, no stall. This is the render feature that survives the move to 3D (the id-buffer edge-detect works for meshes and alpha silhouettes unchanged), matching why the hit-proxy pick buffer was chosen in the first place.

Rejected alternatives: per-frame full GPU readback (a `waitForIdle` stall every frame — no engine does this); CPU shape-outline drawn directly through `Batcher2D` (a legitimate 2D-tool method, but not the AAA path and not 3D-forward).

---

## 2. Grounding (verified against the tree, 2026-07-21)

- **`Arcane::PickBuffer`** (`Arcane/Arcane/src/Arcane/Render/PickBuffer.hpp`) already owns the hit-proxy machinery and exposes exactly the seam this feature needs:
  - `Pick(registry, view, pixel) -> Astra::Entity` (on-demand click readback — System 1, unchanged).
  - `RenderIdPass(registry, view)` — renders every pickable silhouette into the `R32_UINT` id target (documented as "BOTH the render half of `Pick()` and the per-frame hover-highlight seam").
  - `IdTarget() -> nvrhi::ITexture*` — the `R32_UINT` id buffer.
  - `Resize(w,h)`, `Width()`, `Height()`.
  - `PickView` (`PickEmit.hpp`): `{ glm::vec2 offset; float worldToScreenScale; }` — the world->canvas mapping (`screen_px = world * worldToScreenScale + offset`, no Y-flip).
- **Id assignment:** `CollectPickables` (`PickEmit.cpp`) builds an ordered `std::vector<PickDrawable>` while emitting; the k-th drawable's entity gets pass id `k+1`; `0` = background. The table is rebuilt each `RenderIdPass`/`Pick`.
- **`OffscreenCanvas::OutputFramebuffer()`** (`Arcane/Render/OffscreenCanvas.hpp`, added 2026-07-20): the post-tonemap, display-referred (`BGRA8_UNORM`) viewport framebuffer that the Viewport panel samples via `ImGui::Image`. The outline composites into this (over the scene, not tonemapped) — the same slot the game-imgui overlay uses.
- **Grimoire viewport draw** (`GrimoireApp.cpp`): `m_viewport->Draw([&](Batcher2D& b){ SetRenderContext(&b); Loop().SubmitRender(); /* gizmo */ }, clear)` renders scene + gizmo into the canvas; the editor ImGui pass runs later and samples `OutputFramebuffer()`. Selection is single-entity: `Grimoire::SelectionContext m_selection` (`SelectionContext.hpp`, `Astra::Entity selected`). The viewport-local cursor pixel + viewport-active flag are already computed each frame (the hoisted `m_lastViewportMouse`/`m_lastInViewport` from the game-imgui arc).
- **Shaders** are HLSL in `Arcane/shaders/`, compiled to DXIL+SPIR-V by the Arcane prebuild; SPIR-V register shifts match `nvrhi::VulkanBindingOffsets` (t=0 s=128 b=256 u=384). `TonemapPass` is the reference fullscreen-triangle pass (samples a texture, writes a framebuffer). A `uint` texture is read with `Load` (integer coords), not `Sample`.
- **GPU test convention:** render tests are `[gpu]`-tagged (excluded by `~[gpu]`), assert `Arcane::RenderErrorCount() == 0` (validation-silent). Readback pattern in `Tests/Helpers/GpuTestHelpers.hpp`.

---

## 3. Architecture — two systems, one shared id buffer

```
Per Edit-mode frame, only when (HasSelection || cursor in viewport):

  System 1 -- PICKING (exists, unchanged)
     click -> PickBuffer::Pick(reg, view, pixel) -> Entity -> m_selection

  System 2 -- SELECTION OUTLINE (new, per-frame, fully GPU, zero readback)
     1. PickBuffer::RenderIdPass(reg, view)         -> R32_UINT IdTarget (depth-tested, front-most wins)
     2. uint selectedId = PickBuffer::PassIdOf(m_selection.selected)   // 0 if none / not pickable
     3. SelectionOutline::Render(cmd, IdTarget, OutputFramebuffer,
                                 selectedId, cursorPx, params)
           edge-detect shader:
             hoveredId = (cursorPx in range) ? Load(IdTarget, cursorPx) : 0
             for each pixel: if it borders selectedId -> amber
                             else if it borders hoveredId -> cyan
                             else discard (scene shows through)
```

The two systems share the **id buffer** (the hit-proxy render) but are distinct responsibilities: picking is on-demand readback; outlining is per-frame edge-detect. The hovered id is resolved **in the shader** by loading the id at the cursor pixel — no CPU readback, no stall, ever.

**Occlusion:** the id pass is depth-tested, so `IdTarget` holds the front-most entity per pixel; the outline is therefore of the *visible* silhouette (AAA occlusion-correct behavior).

---

## 4. The new engine piece — `Arcane::SelectionOutline` (ARCANE_API, editor-free)

A screen-space edge-detect post-pass, sibling to `TonemapPass` / the game-imgui overlay. Editor- and game-agnostic: it knows nothing but "given an id buffer, two ids, and a cursor, draw two-color outlines into a framebuffer."

```cpp
// Arcane/Arcane/src/Arcane/Render/SelectionOutline.hpp
namespace Arcane
{
    class ShaderLibrary;

    class ARCANE_API SelectionOutline
    {
    public:
        static std::unique_ptr<SelectionOutline> Create(nvrhi::IDevice*, ShaderLibrary&);
        virtual ~SelectionOutline() = default;

        struct Params
        {
            uint32_t  selectedId = 0;               // 0 = no selection
            glm::ivec2 cursorPx  = { -1, -1 };      // viewport-local; out-of-range => no hover
            glm::vec4 selectColor = { 1.0f, 0.65f, 0.10f, 1.0f };  // amber
            glm::vec4 hoverColor  = { 0.25f, 0.70f, 1.0f, 1.0f };  // cyan
            float     selectThicknessPx = 3.0f;
            float     hoverThicknessPx   = 2.0f;
        };

        // Edge-detect `idBuffer` (R32_UINT) and composite two-color EXTERIOR outlines
        // into `target` (display-referred; blends over the existing scene) on the OPEN
        // command list. `selectedId`/hovered (derived in-shader from cursorPx) drive the
        // two colors; select takes precedence over hover on shared pixels.
        virtual void Render(nvrhi::ICommandList*,
                            nvrhi::ITexture* idBuffer,
                            nvrhi::IFramebuffer* target,
                            const Params&) = 0;
    };
}
```

### 4a. The shader (`selection_outline.hlsl`)
- Fullscreen triangle VS (mirror `TonemapPass`).
- PS: `Load`s the `R32_UINT` id buffer (integer coords; the target's pixel = `SV_Position.xy`). Reads `hoveredId = (cursor in range) ? idBuffer.Load(cursorPx) : 0`.
- **Exterior outline via N-tap ring:** a pixel whose own id is NOT the target id, but which has a neighbor within the thickness radius whose id IS the target id, is an outline pixel. Test `selectedId` first (amber); else `hoveredId` (cyan). A pixel not on either boundary is discarded (or writes 0 alpha) so the tonemapped scene shows through.
- Precedence: select over hover — hovering the selected entity yields amber only (its border qualifies for select first).
- Constants (b-register): `selectedId`, `cursorPx`, two colors, two thicknesses, target size / inverse size. **`hoveredId` is derived IN-SHADER** as `idBuffer.Load(cursorPx)` — it must NOT be computed CPU-side, because reading the id buffer on the CPU is a readback, which is exactly the per-frame stall this design avoids. Blend state: source-over the display-referred target (outline pixels overwrite/blend; background discarded).
- **Color space:** the two colors are written into the display-referred (gamma-encoded `BGRA8`) `OutputFramebuffer` directly, so they are display-space values (what you see), NOT linear-HDR scene colors — the same convention as the game-imgui overlay and the `TonemapPass` output. No linear->sRGB conversion in the outline shader.

### 4b. Why a separate pass rather than folding into the id pass
The id pass writes uints (no color); the outline needs display-referred color composited over the *tonemapped* scene. So it is a distinct fullscreen pass reading `IdTarget` and writing `OutputFramebuffer` — exactly the post-process shape `TonemapPass` and the game-imgui overlay already establish.

---

## 5. Shared id-buffer seam — one `PickBuffer` addition

`RenderIdPass` + `IdTarget()` already exist. The only new picking-side surface:

```cpp
// PickBuffer, addition:
// The pass id assigned to `e` in the most recent RenderIdPass/Pick (k+1 order),
// or 0 if `e` was not pickable / not in the last pass. Lets the outline pass know
// the selected entity's id without a readback.
virtual uint32_t PassIdOf(Astra::Entity e) const = 0;
```

Backed by the `std::vector<Astra::Entity>` table `CollectPickables` already builds (reverse lookup, or a small `Entity -> id` map populated alongside it). Hover needs nothing new — it is the in-shader cursor sample.

---

## 6. Grimoire integration

Own a `std::unique_ptr<Arcane::SelectionOutline> m_outline`, created in `Init` beside `m_pick`/`m_viewport` (no resize state of its own — it reads whatever `IdTarget`/`OutputFramebuffer` it is handed).

Per frame, **Edit mode only** (`!m_play.IsPlaying()`), immediately after `m_viewport->Draw(...)` and before the editor ImGui `BeginFrame` (the same insertion point as the Play-only game-imgui pass, so the two never both run):

```
if (!m_play.IsPlaying() && (m_selection.HasSelection() || m_lastInViewport))
{
    const Arcane::PickView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };
    m_pick->RenderIdPass(m_runtime->Registry(), view);          // refresh IdTarget

    Arcane::SelectionOutline::Params p;
    p.selectedId = m_selection.HasSelection()
                 ? m_pick->PassIdOf(m_selection.selected) : 0;
    p.cursorPx   = m_lastInViewport ? glm::ivec2(m_lastViewportMouse) : glm::ivec2(-1);

    m_gpu->Cmd()->open();
    m_outline->Render(m_gpu->Cmd(), m_pick->IdTarget(),
                      m_viewport->OutputFramebuffer(), p);
    m_gpu->Cmd()->close();
    m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
}
```

- Zero cost when there is no selection and the cursor is outside the viewport (the whole block is skipped).
- Hovering a gizmo handle yields `hoveredId = 0` (the gizmo is not in the id pass) — no spurious hover outline over the gizmo itself.
- The gizmo (drawn pre-tonemap in the canvas) and the outline (post-tonemap overlay) are spatially near-disjoint (center handles vs edge outline); the outline draws on top where they meet, which is acceptable.
- Command-list submission mirrors the game-imgui overlay pass (open -> Render -> close -> executeCommandList; NVRHI auto-transitions the output texture).

---

## 7. Testing

- **`[gpu]` (device, desk/CI; excluded by `~[gpu]`):**
  - Feed a synthetic `R32_UINT` id buffer with a known filled region (id = S) and a second region (id = H); run `SelectionOutline::Render` with `selectedId = S` and a `cursorPx` inside region H; read back the target and assert: amber pixels ring region S, cyan pixels ring region H, interior/background pixels are untouched (scene color preserved), and where the two rings would overlap amber wins. `RenderErrorCount() == 0`.
  - Cursor out of range / `selectedId = 0` -> pass writes nothing (target unchanged).
- **Headless CPU:** `PickBuffer::PassIdOf` — returns `k+1` for the k-th collected entity, `0` for an entity not in the pass and for `Entity::Invalid()`.
- **Desk (interactive, GPU hazard headless):** amber outline on the selected entity (persists without hovering); cyan outline follows the cursor and updates on mouse-move; hovering the selected entity shows amber only; occlusion-correct (front-most only); absent in Play; no NVRHI/VK-validation noise; two-context coexistence with the editor ImGui unaffected.

---

## 8. Non-goals (v1)

- **Multi-select outlining** — single selected + single hovered now. The id-buffer edge-detect extends to a *set* of ids trivially later (pass a small id list / a per-id mask), but selection is single-entity today (gizmo + undo stack are single-select).
- **Alpha-cutout sprite silhouettes** — the id pass rasterizes sprite *quads*; an alpha-tested id pass is a later upgrade to `RenderIdPass`, transparent to the outline shader.
- **Jump-flood / soft/animated outlines** — a simple N-tap edge-detect for v1; JFA (clean thick outlines) and pulse animation are later polish on the same pass.
- **Interior outline / full tint** — exterior outline only (chosen).
- **Hover/select outlining in Play mode** — Edit-only (matches gizmo/pick).

---

## 9. Impl-time verification points (shape unaffected; resolve while planning)

1. **Fullscreen pipeline idiom** — mirror `TonemapPass`'s NVRHI pipeline/binding setup for a fullscreen-triangle pass that reads one texture and writes `OutputFramebuffer`; confirm the blend state that lets outline pixels composite over the scene while background pixels leave it untouched (discard vs 0-alpha blend).
2. **`R32_UINT` in a PS** — confirm `Texture2D<uint>` `Load` works on both backends (D3D12 + Vulkan) with validation silent; the id buffer is not a `Sample`-able format.
3. **Id-pass depth/front-most** — confirm `RenderIdPass` is depth-tested so `IdTarget` is occlusion-correct (front-most id per pixel); if not, the outline follows draw order (acceptable, note it).
4. **`PassIdOf` backing** — the exact table `CollectPickables` builds and the cheapest reverse lookup (small `Entity->id` map vs linear scan of the ordered vector); confirm it reflects the *last* `RenderIdPass` (the one this frame).
5. **Per-frame `RenderIdPass` cost + clear** — confirm driving the id pass every Edit frame (was on-demand) is acceptable and that `clearTextureUInt` to 0 each frame is correct on both backends.
6. **Cursor pixel space** — confirm `m_lastViewportMouse` is viewport-local, y-down, matching `IdTarget` texel space (same space `Pick` already uses); guard out-of-range.
7. **Thickness -> taps** — how `selectThicknessPx`/`hoverThicknessPx` map to the neighbor sample pattern (ring radius / tap count) at 1:1 target resolution.
8. **Compositing order vs the game-imgui pass** — both target `OutputFramebuffer`; confirm they are mutually exclusive by mode (outline Edit-only, game-imgui Play-only) so ordering never matters.
