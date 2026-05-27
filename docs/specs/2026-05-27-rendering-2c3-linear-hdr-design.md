# Rendering Phase 2c-3 — Linear/HDR Scene Pipeline + ACES Tonemap Design

**Date:** 2026-05-27
**Scope:** GachaClient (Love2D / LuaJIT / OpenGL)
**Status:** Approved design, pending implementation plan
**Parent spec:** `docs/superpowers/specs/2026-05-27-rendering-2c-design.md` ("Color flip" section). This sub-spec details the deliberate look-change phase the 2c structural migration enabled.

## Context

Phases 2c-1/2c-1b/2c-1c (layer compositor) and 2c-2/2c-2b (screen + combat/wave scene producers) are **DONE and validated**. Every scene producer — overworld `RenderSystem`, declarative screens, `WaveGame`, `CombatScreen` — now flows through the one canonical path: `composeScene(ctx, producer, layer)` fills `ctx.sceneRT`, runs the layer's `effects`, applies the shared `PostFx` grade, then presents into the frame.

The `tonemap` per-layer flag exists in `Layer.lua` and is **already set true** by `Profile.WORLD` and by `Profile.forScreenDef` on overlay/post-process screens — but **no code consumes it yet**. 2c-3 activates it: the flagged scene layers render in HDR (`rgba16f`) and gain an ACES filmic tonemap, while UI/HUD stay in display/sRGB space.

This is **the one deliberate look change** of the 2c initiative. It is *not* parity-gated by screenshot identity for the flagged producers; it is gated by deliberate before/after review + headless math sanity. The untouched paths (plain menus, all UI, all HUD) **must remain pixel-identical**.

### Current reality

- **No global gamma-correction** is enabled (`conf.lua` does not set `t.gammacorrect`); everything renders in display/gamma space today. The linear seam must therefore be **scoped manually to the scene path** — matching the locked "scene-only linear, UI stays sRGB" decision. The global flag is rejected (it would gamma-correct UI too).
- **`rgba16f` is already plumbed**: `RenderTargets:acquire(w, h, "rgba16f")` works and is exercised by `tests/render_harness`. HDR is a format argument, not new infrastructure.
- **The grade** (`data/shader/post/post_process.glsl`) runs entirely in display space, clamps to `1.0`, and has **no tonemap**. It is the single grade shared by every graded layer (world + screens), owned by `ui/components/PostFx.lua`.
- **`composeScene` (`main.lua`)** is the one place a scene is graded + presented, so the flip lands in one function + the grade shader + format threading.

## Decisions (locked with the user, 2026-05-27)

1. **Linear scope = tonemap-wrap (not true linear, not linear-grade).** `sceneRT` becomes `rgba16f` so additive bloom/emissive survive past 1.0. The grade shader decodes the composite to linear, does bloom in linear, applies ACES, encodes back to sRGB, **then** runs the existing artistic grade unchanged. Scene *input textures are NOT individually linearized* — inter-sprite blending stays in gamma space. The artistic grade (contrast/saturation/SMH/tint/vignette/grain) is **not re-tuned**; it keeps operating on display-space 0–1 values after the tonemap+encode. True per-input linearization is deferred to Phase 6 (2D lighting).
2. **Tonemap operator = Narkowicz ACES approximation** — the canonical one-function "ACES filmic" fit (no matrices, cheap), satisfying the parent spec's "ACES filmic" decision.
3. **UI-sRGB seam = gate tonemap by the per-layer `tonemap` flag.** The shared grade shader runs decode→ACES→encode **only when** `tonemap > 0.5`. UI/plain-menu layers keep `tonemap=false` and take the byte-for-byte current grade path. **This supersedes the old "flip `ui.grade=false`" note** — UI keeps `grade` as-is, so the Gacha-reveal widgets retain their bloom/vignette/grain. Tonemap becomes just another per-layer capability the one grade honors (homogenization mandate preserved).
4. **Producers going HDR this phase = all four scene-producer types:** overworld (`WORLD`), the 3 overlay/post-process declarative screens (`screen_1` Gacha Pull, `screen_0`, `screen_2`), **and** `WaveGame` + `CombatScreen`. Combat/wave were `grade=false`/`tonemap=false` for 2c-2b parity; they flip on this phase so their additive holographic glow gets filmic rolloff instead of clipping at 1.0.
5. **Single grade path for combat/wave.** To reach the tonemap they route through the one grade with a **neutral tonemap gradeSettings** (`{ tonemap=true, bloom_intensity=0, vignette_intensity=0, grain_intensity=0 }`) — they already do their own internal bloom in `drawScene`, so the grade adds only the ACES rolloff + encode, no second bloom. No dedicated tonemap-only shader.
6. **`rgba8` graceful fallback** when `rgba16f` is unavailable: tonemap still runs on 0–1 (bloom clips, no crash). One-time format-availability check.
7. **No persistent feature flag** (per CLAUDE.md). A/B during review via git or a one-line comment toggle.

## Architecture

### HDR buffer threading

The only new plumbing is carrying the float format from the active scene layer's `tonemap` flag to the producer that builds the sceneRT:

- `composeScene(ctx, producer, layer)` sets `ctx.sceneFormat = (layer and layer.tonemap) and "rgba16f" or nil` **before** calling `producer(ctx)`.
- `composeScreensScene(ctx)` acquires its `rt` and the per-screen scratch canvas with `ctx.sceneFormat` (so overlay screens, WaveGame, and Combat inherit the float buffer from the single place that composites their backgrounds).
- `RenderSystem` (overworld, reached via the ECS `state.world:update(0,nil,"draw")`) reads `ctx.sceneFormat` through `FrameCtx` when it acquires its scene canvas.
- The post-grade `graded` copy in `composeScene` and the full-frame AA canvas **stay rgba8**: the grade *outputs* display-encoded sRGB 0–1, so only the producer's pre-grade sceneRT needs to be float.

### Grade shader (`post_process.glsl`)

Add `extern float tonemap;` (default 0) plus sRGB decode/encode helpers and the Narkowicz ACES fit. `effect()` branches:

- **`tonemap > 0.5`** (scene HDR): `chromatic sample → srgbDecode → linear bloom (decode samples) → ACES → srgbEncode → applyColorGrading + vignette + dither + grain` (the existing artistic grade, unchanged, on the 0–1 encoded value).
- **`tonemap == 0.0`** (UI / plain menus / today): the **exact current path** — `chromatic → display-space getBloom → applyColorGrading → vignette → dither → grain`. Byte-for-byte unchanged.

The tonemap branch only *inserts* decode/linear-bloom/ACES/encode ahead of the unchanged artistic grade; the off-path is literally today's shader.

Reference Narkowicz ACES (linear in → tonemapped linear out, per channel):
```glsl
vec3 ACESFilmic(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
```
sRGB transfer (approximate gamma 2.2 is acceptable for this stylized target; the exact piecewise sRGB curve is an option if banding appears):
```glsl
vec3 srgbDecode(vec3 c) { return pow(max(c, 0.0), vec3(2.2)); }
vec3 srgbEncode(vec3 c) { return pow(max(c, 0.0), vec3(1.0 / 2.2)); }
```

### `PostFx.lua`

`PostFx.send` uploads a new `tonemap` uniform from `s.tonemap` (guarded by `:hasUniform`, default 0 when absent so a prior screen's value never leaks). `composeScene` injects `layer.tonemap` into the settings table it hands the grade (so the per-layer flag and the artistic params arrive together). No change to the master `Settings.postFx()` gate or the existing uniform list.

Because tonemap lives **inside** the grade pass, it rides on the existing `Settings.postFx()` gate: with post-processing disabled the grade is skipped entirely, so there is no tonemap and the float sceneRT's values clip on draw to the rgba8 frame — i.e. post-fx-off behavior is unchanged from today (rawer, clipped look). Tonemap is a property of the graded scene, not an always-on stage.

### Producer wiring

- **Overworld + overlay screens:** already carry `tonemap=true` and real `gradeSettings` (world PostFx settings / `_overlay`). They begin tonemapping with zero new tables — only the format thread + shader branch reach them.
- **`WaveGame` / `CombatScreen`:** `Profile.forProducer` flips to `tonemap=true`; their constructors set a shared **neutral tonemap gradeSettings** (`{ tonemap=true, bloom_intensity=0, vignette_intensity=0, grain_intensity=0 }`) so the grade pass runs (reaching the tonemap) without adding a second bloom or any vignette/grain over their own internal post.

### UI / HUD seam

No `ui.grade` change. `ui`/`hud` layers keep `tonemap=false` → unchanged shader path. The seam: the scene tonemaps→encodes to sRGB into the rgba8 AA canvas; UI then composites on top in sRGB into the same canvas — one consistent display space. Gacha-reveal widgets keep their bloom/vignette/grain.

## Components & ownership

| Unit | Change |
|---|---|
| `data/shader/post/post_process.glsl` | Add `tonemap` uniform + sRGB decode/encode + Narkowicz ACES; branch `effect()` (tonemap path inserts decode/linear-bloom/ACES/encode before the unchanged grade) |
| `ui/components/PostFx.lua` | Send `tonemap` uniform from `s.tonemap` (guarded, default 0) |
| `main.lua` `composeScene` | Set `ctx.sceneFormat` from `layer.tonemap`; inject `layer.tonemap` into the grade settings |
| `main.lua` `composeScreensScene` | Acquire `rt` + scratch with `ctx.sceneFormat` |
| `systems/RenderSystem.lua` | Honor `ctx.sceneFormat` when acquiring its scene canvas (via `FrameCtx`) |
| `systems/render/Profile.lua` `forProducer` | `tonemap=true` on the scene layer |
| `ui/screens/combat/init.lua`, `ui/screens/dailies/WaveGame.lua` | Set the shared neutral tonemap `gradeSettings` |
| `tests/render_harness/main.lua` | Headless math-property tests (Lua mirror of the GLSL math) |
| `systems/render/RenderTargets.lua` | (No change — `rgba16f` already supported) |

## Testing & verification

- **Headless (`tests/render_harness`):** a small pure-Lua mirror of the GLSL math (next to a comment naming the GLSL as source of truth) asserting: `srgbDecode`∘`srgbEncode` ≈ identity; ACES monotonic, `ACES(0)=0`, `ACES(large)→~1`, output ≤ 1; and that the tonemap-off path is identity-preserving (guards the untouched UI path's math). All pure logic, no GL. Expect "ALL RENDER HARNESS CHECKS PASSED".
- **Format check:** a one-time `love.graphics.getCanvasFormats().rgba16f` probe; on absence, sceneRT falls back to rgba8 (tonemap still runs, bloom clips).
- **GL / visual (the real gate — deliberate look change):** before/after screenshots of overworld + Gacha reveal + Combat + WaveGame, reviewed by the user. Plain menus, all UI widgets, and the HUD must be **identical** (regression check on the tonemap=false paths). F9 profiler: confirm no new pass and acceptable scene-layer cost with the float buffer.
- **Honest caveat:** the Lua mirror can drift from the GLSL; it documents intent and guards properties, but the visual gate is authoritative for the look.

## Risks

- **`rgba16f` unsupported on a GPU** → format-availability fallback to rgba8 (graceful; bloom clips, no crash).
- **Float-canvas linear filtering** (bloom sampling / render-scale upscale) → supported on modern GL; verify in the overworld visual gate.
- **Combat/wave double-bloom** → neutral gradeSettings (`bloom_intensity=0`); tonemap adds only ACES rolloff + encode over their own internal bloom.
- **`rgba16f` bandwidth** → profiler-budgeted; render-scale already shrinks the world RT; combat/wave are full-res but single scene buffer.
- **Look-change surface = 4 producer types** → per-producer before/after review + untouched-path (menu/UI/HUD) regression identity check.
- **Approximate gamma 2.2 vs exact sRGB curve** → start with pow(2.2); switch to the piecewise sRGB curve only if banding/mismatch surfaces in review.

## Decomposition into implementation plan

A single plan (`docs/superpowers/plans/2026-05-27-rendering-p2c3-linear-hdr.md`), TDD where pure-logic (the math mirror) and visual-gated for the GL flip. Suggested task order:

1. **Grade shader tonemap branch** + ACES/sRGB helpers + `PostFx` `tonemap` send; headless math mirror tests. (No behavior change yet — `tonemap` defaults 0, nothing sends it true.)
2. **Format threading**: `composeScene` `ctx.sceneFormat` + grade `tonemap` inject; `composeScreensScene` format-aware acquire; `RenderSystem` honors `ctx.sceneFormat`; `rgba16f` availability fallback. (Activates tonemap for the already-flagged overworld + overlay screens.) Visual gate: overworld + Gacha reveal before/after; menus/UI/HUD identical.
3. **Combat/wave HDR**: `forProducer` `tonemap=true` + neutral gradeSettings on both screens. Visual gate: Combat + WaveGame before/after.

Each task ends with its own commit (targeted `git add` — the working tree is heavily dirty). The game stays playable throughout; combat **abilities** gameplay stays off-limits (rendering plumbing only).
