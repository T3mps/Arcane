# Selection + Hover Outline v2 (Jump-Flood distance field) — Arcane engine

**Date:** 2026-07-21
**Status:** Design — approved (JFA distance-field outline; Approach 1 = 2x-supersampled id for sub-pixel coverage; single tagged JFA for the two colors; thin crisp ~3px default with thickness/softness as tunable Params). Awaiting spec review.
**Layer:** Arcane engine (`ARCANE_API` render), consumed by Grimoire. Supersedes the v1 single-pass edge-detect (`2026-07-21-arcane-selection-outline-design.md`). Builds on the entity-id picking buffer (`2026-07-19-arcane-entity-id-picking-design.md`) and the offscreen output framebuffer (`2026-07-20-grimoire-game-imgui-viewport-design.md`).

---

## 1. Why

The shipped v1 outline (`SelectionOutline` + `selection_outline.hlsl`) is a single-pass brute-force dilation: for each pixel it scans a square (Chebyshev) neighborhood of the `R32_UINT` id buffer and returns a **binary** amber/cyan/`discard`. Desk screenshots show the two failure modes this produces:

- **No anti-aliasing** — every pixel is fully colored or fully absent, so the ring stair-steps.
- **Square kernel on curved edges** — the Chebyshev metric makes the ring thickness uneven (~sqrt2 variation) and breaks into a dashed, lumpy look around a circle.
- **O(radius^2)** per pixel — the perf tail the v1 final review flagged.

This is exactly the brute-force approach Ben Golus's ["The Quest for Very Wide Outlines"](https://bgolus.medium.com/the-quest-for-very-wide-outlines-ba82ed442cd9) measures as both slow (~10 ms at r=20, 1080p) and low-quality, and we shipped it *without* even the anti-aliasing that makes it tolerable.

**The AAA fix is a Jump-Flood-Algorithm (JFA) distance field**, the technique in Golus's article and the official [UE 5.3 wide-outline tutorial](https://forums.unrealengine.com/t/community-tutorial-unreal-5-3-the-jump-flood-algorithm-or-how-to-make-wide-outlines/1350234). JFA builds an (unsigned) distance field in ~log2(radius) passes; the outline is then a `smoothstep` on distance — perfectly round, anti-aliased, and **any thickness for near-free** (Golus: 100 px outline ~0.7 ms at 1080p). It also leaves a reusable distance field, so wide / glow / pulse become Param tweaks later with no rework.

Note (honesty, drove the design): JFA alone does **not** make a *thin* outline smoother than a well-written single-pass edge-detect — its wins are round shape, cheap-at-any-width, and the reusable field. The buttery thin-edge smoothness comes from feeding JFA a **sub-pixel-accurate silhouette**. A hard 1-bit id mask carries no sub-pixel information, so this design supersamples the silhouette (Section 5) — that is where the "looks like Unreal" quality actually comes from.

Rejected: Unreal's *editor selection* outline (stencil/custom-depth mask + post-process edge-detect + MSAA'd mask coverage alpha) — a legitimate thinner-machinery route, but not the reusable-distance-field foundation we chose for future wide/glow work.

---

## 2. Grounding (verified against the tree, 2026-07-21)

- **`Arcane::SelectionOutline` (v1)** (`Arcane/Arcane/src/Arcane/Render/SelectionOutline.hpp`) is **stateless**: `Create(nvrhi::IDevice*, ShaderLibrary&)`, `Render(cmd, idBuffer, target, const Params&)`, `Params{ selectedId, cursorPx, selectColor, hoverColor, selectThicknessPx, hoverThicknessPx }`. It owns one non-volatile constant buffer + a fullscreen pipeline + a binding-set cache keyed on the id texture; no render targets of its own. It is the **engine's first nvrhi constant buffer** (all other passes use push constants) and mirrors `TonemapPass`'s pipeline/binding/draw idiom with ImGui-style `SrcAlpha/InvSrcAlpha` blend.
- **`Arcane::PickBuffer`** (`Arcane/Arcane/src/Arcane/Render/PickBuffer.hpp`) owns the `R32_UINT` id target (viewport-sized): `Create(device, shaders, w, h)`, `RenderIdPass(registry, view)` (clears to 0, draws every pickable silhouette back-to-front so **front-most wins** -> occlusion-correct), `IdTarget()`, `PassIdOf(entity)->uint32_t` (k+1, no readback), `Resize(w,h)`, `Width()/Height()`, `Pick(registry, view, pixel)`. `PickView` (`PickEmit.hpp`) is `{ glm::vec2 offset; float worldToScreenScale; }`.
- **`OffscreenCanvas::OutputFramebuffer()`** — the post-tonemap, display-referred `BGRA8_UNORM` viewport framebuffer the outline composites into (over the scene, not tonemapped), same slot the game-imgui overlay uses. Grimoire's viewport draw and the v1 Edit-only outline block live in `GrimoireApp.cpp`; `m_lastViewportMouse`/`m_lastInViewport` give the viewport-local cursor.
- **Shaders are data** — HLSL in `Arcane/shaders/`, compiled to DXIL+SPIR-V by the Arcane prebuild; SPIR-V register shifts match `nvrhi::VulkanBindingOffsets` (t=0 s=128 b=256 u=384); a new `.hlsl` is registered in `compile-shaders.bat` (explicit list, not globbed); entry points `vs_main`/`ps_main`; `Texture2D<uint>` read with `.Load`. `TonemapPass` is the reference fullscreen-triangle pass; ping-pong render targets are standard.
- **GPU test convention** — render tests are `[gpu]`-tagged (excluded by `~[gpu]`), assert `Arcane::RenderErrorCount()==0`; upload/readback via `Tests/Helpers/GpuTestHelpers.hpp`. `[gpu]` tests cannot run headless on this box (Parsec GPU-driver crash hazard) — build-verified here, run at desk/CI.

---

## 3. Architecture — multi-pass JFA, one shared id buffer

```
Per Edit-mode frame, only when (HasSelection || cursor in viewport):

  System 1 -- PICKING (exists): click -> PickBuffer::Pick -> Entity      [unchanged]

  System 2 -- SELECTION OUTLINE v2 (JFA distance field, fully GPU, zero readback)
     0. PickBuffer::RenderIdPass(reg, view)   -> R32_UINT id target AT 2x SUPERSAMPLE
                                                 (depth-tested, front-most; Section 5)
     1. SEED     : read 2x id -> per-pixel coverage of selectedId & hoveredId
                   -> RGBA16 seed { sub-pixel edge pos, tag(select/hover), coverage }
                   (hoveredId = id at cursor texel, in-shader; no CPU readback)
     2. JFA x N  : ping-pong RGBA16, jump = 2^(N-1) .. 1  (N = ceil(log2(maxThick))+1)
                   3x3 nearest-seed -> unsigned distance field + carried tag
     3. COMPOSITE: dist = |pixel - nearestSeed|;
                   alpha = 1 - smoothstep(thick - aa, thick, dist)  (exterior-only)
                   color by tag (amber select / cyan hover), select-over-hover
                   -> blend display-referred over OutputFramebuffer
```

Picking and outlining still share only the **id buffer**; picking stays on-demand readback, outlining is per-frame GPU. Occlusion is inherited from the depth-tested id pass (front-most silhouette). All passes are **fullscreen pixel shaders** (the `TonemapPass` idiom), not compute — matches the established engine pattern and keeps the pipeline/binding code uniform.

---

## 4. `Arcane::SelectionOutline` v2 — stateful, ARCANE_API, editor-free

The class keeps its role and agnostic contract ("id buffer + ids/colors/cursor in, framebuffer out") but becomes **stateful**: it owns the ping-pong JFA render targets and the seed target, so it gains a `Resize` and rebuilds them with the viewport.

```cpp
class ARCANE_API SelectionOutline
{
public:
    static std::unique_ptr<SelectionOutline> Create(nvrhi::IDevice*, ShaderLibrary&,
                                                     uint32_t width, uint32_t height);
    virtual ~SelectionOutline() = default;

    struct Params
    {
        uint32_t   selectedId = 0;                              // 0 = no selection
        glm::ivec2 cursorPx   = { -1, -1 };                     // viewport-local; <0 => no hover
        glm::vec4  selectColor = { 1.0f, 0.65f, 0.10f, 1.0f };  // amber (display-referred)
        glm::vec4  hoverColor  = { 0.25f, 0.70f, 1.0f, 1.0f };  // cyan  (display-referred)
        float      selectThicknessPx = 3.0f;                    // outline half-width (ramp)
        float      hoverThicknessPx  = 2.0f;
        float      edgeSoftnessPx    = 1.0f;                    // AA ramp width
    };

    // Seed -> JFA -> composite. `idBuffer` is the SUPERSAMPLED (>=1x) R32_UINT id
    // target; the supersample factor is derived from idBuffer size / target size.
    // hoveredId is sampled in-shader from Params::cursorPx (no readback). Owns its
    // ping-pong targets at `target` resolution; call Resize on viewport change.
    virtual void Render(nvrhi::ICommandList*,
                        nvrhi::ITexture* idBuffer,
                        nvrhi::IFramebuffer* target,
                        const Params&) = 0;

    virtual void Resize(uint32_t width, uint32_t height) = 0;
};
```

`selectThicknessPx`/`hoverThicknessPx` become the distance-ramp half-width (float now, for smooth sub-pixel thickness); `edgeSoftnessPx` is the `smoothstep` width. The JFA pass count is sized by a compile-time `kMaxThicknessPx` (e.g. 32 -> 6 passes) so the same pipeline supports future wide outlines without a Param-driven pass count.

### 4a. The shaders
Three new HLSL files in `Arcane/shaders/` (all fullscreen-triangle VS, register in `compile-shaders.bat`):
- **`outline_seed.hlsl`** — reads the 2x `R32_UINT` id; for the output (1x) pixel computes coverage of `selectedId` and `hoveredId` from the 2x2 subsamples; estimates the sub-pixel edge position from that coverage (Golus's coverage->offset trick, adapted to 4-sample coverage); writes `RGBA16` seed `{ pos.xy (normalized), tag, coverage }`, or a sentinel where neither id is present. `hoveredId` is computed in-shader as the id at `cursorPx` (one `.Load`) — no CPU readback.
- **`outline_jfa.hlsl`** — one ping-pong step: samples the previous seed target at the 8 neighbors offset by the current `jump`, keeps the seed whose stored position is nearest this pixel, carries the tag. Run N times with halving `jump`.
- **`outline_composite.hlsl`** — `dist = length(pixel - nearestSeed.pos)`; **exterior test**: read this pixel's ORIGINAL coverage from the retained pre-JFA seed target (`seed0`, Section 4b) — a covered (interior) pixel has `dist ~= 0` and must be skipped so the ramp does not fill the object solid, so draw only where the pixel was NOT covered; `alpha = 1 - smoothstep(thick - edgeSoftness, thick, dist)`; color by tag with select-over-hover precedence; output `color.rgb, alpha` blended `SrcAlpha/InvSrcAlpha` (display-referred, no sRGB conversion) into the target; `discard` (or alpha 0) elsewhere.

### 4b. Seed + ping-pong targets
Format `RGBA16_SNORM` (or `_FLOAT`): `.xy` = nearest silhouette edge position normalized to the viewport, `.z` = tag (select / hover / none), `.w` = coverage / spare. `RG16` suffices if the tag is packed, but RGBA16 keeps the encoding legible and carries coverage for the composite. Positions normalized so the 16-bit range covers the largest supported viewport.

Three owned targets at composite (1x) resolution: **`seed0`** (the seed pass output — original coverage + sub-pixel seed, RETAINED for the composite's exterior test) plus **two ping-pong** targets the JFA flood alternates between (seeded by copying/reading `seed0`). The composite reads `seed0` (original coverage) and the final ping-pong target (nearest seed -> distance). All three rebuild on `Resize`.

---

## 5. Coverage source — Approach 1: 2x-supersampled id

Sub-pixel smoothness is impossible from a hard 1x mask, so the silhouette must be captured with coverage at render time. **The id pass renders at 2x supersample** (`R32_UINT` at `2*w x 2*h`); the seed pass derives per-id coverage (0..1) per 1x pixel from the 2x2 subsamples, giving a 4-level coverage that JFA's sub-pixel estimation turns into smooth edges (comparable to MSAA 4x).

- **Occlusion preserved for free** — the id pass is still depth-tested, front-most-wins, now at 2x.
- **Localized** — the only upstream change is that the id target is supersampled. Preferred mechanism: `PickBuffer` gains a supersample factor (default 1x for picking-only; Grimoire creates it 2x because it outlines), and `Pick()` reads the center subsample (`pixel*ss + center`). Alternative (keeps `Pick()` untouched): a separate 2x id target for outlining. Resolved in the plan's read-first (Section 9.3).
- **Cost** — 4x id-pass fragments, paid only on Edit outline frames (already gated). The id pass draws a handful of silhouettes, so this is cheap in absolute terms.

Rejected — Approach 2 (Grimoire renders a dedicated AA coverage silhouette of just the highlighted entities via the batcher's SDF AA): continuous (smoother) coverage, but **loses free occlusion** and couples Grimoire geometry into the pass. Not worth it for a selection outline where occlusion matters.

---

## 6. Two colors + precedence — one tagged JFA

A single JFA run handles both colors: each seed carries a **tag** (select vs hover). The composite colors a pixel by its nearest seed's tag. **Select-over-hover precedence** is resolved either by seeding the selected silhouette with priority (a selected seed beats a hover seed at equal distance) or in the composite (if a pixel is within `thick` of both a select and a hover seed, select wins). Hovering the selected entity yields amber only, because that entity's silhouette is tagged select. `hoveredId` is derived in-shader from the cursor (Section 4a) — the no-CPU-readback property is preserved.

---

## 7. Grimoire integration

Minimal change from v1:
- Create the `PickBuffer` with supersample 2x (so `IdTarget()` is 2x); `SelectionOutline::Create(device, shaders, w, h)` now takes a size.
- On viewport resize, call both `m_pick->Resize(w,h)` and `m_outline->Resize(w,h)` (and the outline reads the 2x id buffer, so its internal targets stay at 1x target res).
- The per-frame Edit-only block is unchanged in shape (guard `!m_play.IsPlaying() && (HasSelection || m_lastInViewport)`, `RenderIdPass`, then `m_outline->Render(cmd, m_pick->IdTarget(), OutputFramebuffer(), params)`, one-off command-list submit). Still mutually exclusive with the Play-only game-imgui overlay pass.

---

## 8. Testing

- **`[gpu]` (device, desk/CI; excluded by `~[gpu]`):** upload a known 2x `R32_UINT` id buffer with a filled region (id=S) and a second (id=H); run the full seed->JFA->composite with `selectedId=S` and `cursorPx` inside region H; read back the target and assert: an **anti-aliased** amber ring around S (intermediate-alpha pixels exist at the ring edge — the AA the whole redesign is for), a cyan ring around H, interior/background untouched, select wins where rings meet, and `RenderErrorCount()==0`. A second case: a curved (circle) silhouette to assert the ring thickness is uniform (round, not square) — the v1 lumpiness regression guard.
- **Headless CPU:** any pure helpers extracted — JFA pass-count from thickness (`ceil(log2(maxThick))+1`), the coverage->sub-pixel-offset math, seed position (de)normalization — unit-tested `[outline]` without a device.
- **Desk (interactive, GPU hazard headless):** smooth round amber on the selected entity (no stair-steps, uniform thickness on the circle); cyan follows the cursor; hover-selected = amber only; occlusion (front-most only); absent in Play; no NVRHI/VK-validation noise (esp. the seed/JFA targets on both backends); shader hot-reload (F5) leaves it correct.

---

## 9. Non-goals (v1 of this redesign)

- **Multi-select** — single selected + single hovered still (tag is 2-valued). The JFA field extends to a set trivially later.
- **Animated / pulse / glow** — the distance field makes these a composite tweak, but the default stays a thin crisp ring; no animation this pass.
- **Interior fill / tint** — exterior outline only.
- **Play-mode outlining** — Edit-only (matches gizmo/pick).
- **Configurable supersample > 2x** — 2x is the chosen quality/cost point; higher is a later knob.

---

## 10. Impl-time verification points (resolve while planning; shape unaffected)

1. **JFA correctness** — 3x3 neighbor gather with halving jumps; whether to add the "JFA+1" (an extra jump=1 pass) or "1+JFA" refinement to kill the known JFA error pixels at our thin widths; sentinel encoding for "no seed".
2. **Sub-pixel estimation from 4-level coverage** — Golus assumes a continuous AA mask; validate that 2x2 (4-sample) coverage yields visibly smooth edges at 2-3 px, or bump to 4x (16-sample) if 4-level banding shows. Desk-verify decides.
3. **PickBuffer 2x mechanism** — supersample factor on `Create`/`Resize` with `Pick()` reading the center subsample, vs a separate 2x outline-only id target; confirm memory (2x R32_UINT ~= 4x the id target) and the `Pick()` coordinate scaling.
4. **Run JFA at 1x (seeded from 2x coverage) vs at 2x** — keep JFA targets at composite (1x) resolution seeded from the 2x id, so ping-pong targets stay viewport-sized; confirm the seed pass 2x->1x coverage reduction is correct on both backends.
5. **Seed format / range** — `RGBA16_SNORM` position normalized to viewport; confirm precision at the largest supported viewport and the tag/coverage packing.
6. **Composite exterior test** — how "outside the silhouette" is determined (own coverage 0 / not the tagged id) so the ring is exterior-only and interiors show the scene through; blend vs `discard`.
7. **Pass/pipeline count** — 1 seed + N JFA + 1 composite fullscreen pipelines + their binding layouts, all mirroring `TonemapPass`; each per-pass constant buffer byte-identical to its HLSL `cbuffer` (as v1 established, with `static_assert`).
8. **Owned-target lifetime + Resize** — ping-pong + seed targets torn down/rebuilt on `Resize`, mirroring `PickBuffer::Resize` / `OffscreenCanvas`; binding-set caches invalidated.
