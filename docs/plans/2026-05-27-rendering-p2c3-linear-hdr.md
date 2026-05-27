# Rendering Phase 2c-3 — Linear/HDR Scene + ACES Tonemap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Activate the dormant per-layer `tonemap` flag so flagged scene producers render in HDR (`rgba16f`) with a Narkowicz ACES filmic tonemap, while UI/HUD stay sRGB.

**Architecture:** Tonemap-wrap. The shared grade shader (`post_process.glsl`) gains a `tonemap` uniform: when set, it decodes the composite to linear, does bloom in linear, applies ACES, encodes back to sRGB, then runs the unchanged artistic grade. The flag is threaded from the active scene `Layer.tonemap`: `composeScene` sets `ctx.sceneFormat = "rgba16f"` (so the producer's sceneRT is float and additive bloom survives past 1.0) and passes the flag to `PostFx.apply` as an explicit argument (NOT via the settings table — so the same `_overlay` settings can grade a screen's scene tonemapped AND its UI widgets un-tonemapped). UI/HUD layers keep `tonemap=false` and take the byte-for-byte current path.

**Tech Stack:** LÖVE 11.x / LuaJIT / OpenGL (GLSL). Headless tests via `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness` (expect `ALL RENDER HARNESS CHECKS PASSED`, exit 0). Spec: `docs/superpowers/specs/2026-05-27-rendering-2c3-linear-hdr-design.md` (committed `cee0da0`).

---

## Context the implementer needs

- **No global gamma-correction** is enabled — everything renders in display/gamma space. The linear seam is scoped *manually* to the graded scene path; do NOT touch `conf.lua`/`t.gammacorrect`.
- **`composeScene` is a `local function` in `GachaClient/main.lua`** (≈ line 562), not unit-testable in isolation (needs `love.graphics`). So Tasks 2 is **visual-gated**; only pure math (Task 1) and the `Profile.forProducer` shape (Task 3) get headless tests.
- **The grade runs based on `ctx.sceneSettings` presence, not `layer.grade`.** `composeScene`'s grade gate is `if ctx.sceneSettings and Settings.postFx() and ctx.sceneSettings.enabled ~= false`. So combat/wave reach the grade (and thus the tonemap) by being given a non-nil `gradeSettings` — `layer.grade` is irrelevant for the *scene* layer.
- **`PostFx.apply` / `PostFx.send` get an optional 4th `tonemap` arg.** When absent/nil → `0.0` (off) → today's behavior. This keeps Task 1 a true no-op.
- **Working tree is heavily dirty** (~40 modified files). Use **targeted `git add` of only the files each task names** — never `git add -A`/`.`.
- **Combat abilities gameplay is off-limits** — this is rendering plumbing only.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `GachaClient/data/shader/post/post_process.glsl` | Add `tonemap` uniform + sRGB decode/encode + Narkowicz ACES; branch `effect()` | 1 |
| `GachaClient/ui/components/PostFx.lua` | `send`/`apply` gain optional `tonemap` arg → upload `tonemap` uniform (guarded, default 0) | 1 |
| `GachaClient/tests/render_harness/main.lua` | Headless math-property tests (Lua mirror of the GLSL ACES/sRGB); Task 3 updates `forProducer` test | 1, 3 |
| `GachaClient/main.lua` | `composeScene`: set `ctx.sceneFormat` from `layer.tonemap` (HDR-availability gated) + pass `tonemap` to grade; `composeScreensScene`: format-aware acquire; `produceUI`: explicit `false` tonemap | 2 |
| `GachaClient/systems/RenderSystem.lua` | Honor `ctx.sceneFormat` when acquiring the scene canvas | 2 |
| `GachaClient/systems/render/Profile.lua` | `forProducer` gains a `tonemap` opt (default false) on the scene layer | 3 |
| `GachaClient/ui/screens/combat/init.lua` | `renderProfile = Profile.forProducer({tonemap=true})`; `gradeSettings = neutral` | 3 |
| `GachaClient/ui/screens/dailies/WaveGame.lua` | `renderProfile = Profile.forProducer({tonemap=true})`; `gradeSettings = neutral` | 3 |

---

## Task 1: Grade-shader tonemap branch + PostFx wiring + headless math tests

**No behavior change** — `tonemap` defaults to `0.0` and nothing passes it true yet. The off-path (`tonemap == 0.0`) must be byte-for-byte identical to today's shader.

**Files:**
- Modify: `GachaClient/tests/render_harness/main.lua` (add a test block before the final `if fails == 0` line at 396)
- Modify: `GachaClient/data/shader/post/post_process.glsl`
- Modify: `GachaClient/ui/components/PostFx.lua:45-83`

- [ ] **Step 1: Write the failing headless math test**

Insert this block into `GachaClient/tests/render_harness/main.lua` immediately **before** line 396 (`if fails == 0 then ...`). It references functions on a `tm` table that don't exist yet → fail.

```lua
print("== Tonemap math (Lua mirror of post_process.glsl) ==")
do
    -- Pure-Lua MIRROR of the GLSL math in data/shader/post/post_process.glsl.
    -- SOURCE OF TRUTH is the GLSL; this mirror documents intent + guards the curve
    -- properties we rely on. Keep the constants in sync with the shader by hand.
    local tm = {}
    function tm.srgbDecode(c) return c < 0 and 0 or c ^ 2.2 end
    function tm.srgbEncode(c) return c < 0 and 0 or c ^ (1.0 / 2.2) end
    function tm.aces(x)
        if x < 0 then x = 0 end
        local a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
        local v = (x * (a * x + b)) / (x * (c * x + d) + e)
        if v < 0 then v = 0 elseif v > 1 then v = 1 end
        return v
    end

    -- sRGB decode/encode are inverses (round-trip ~= identity in [0,1]).
    approx(tm.srgbEncode(tm.srgbDecode(0.5)), 0.5, "srgb round-trip 0.5")
    approx(tm.srgbEncode(tm.srgbDecode(0.18)), 0.18, "srgb round-trip 0.18")
    eq(tm.srgbDecode(0.0), 0.0, "decode 0 -> 0")
    -- ACES properties: 0->0, monotonic, saturates below 1 for large input.
    eq(tm.aces(0.0), 0.0, "aces(0) = 0")
    if not (tm.aces(0.5) < tm.aces(1.0)) then fail("aces monotonic 0.5<1.0") end
    if not (tm.aces(1.0) < tm.aces(4.0)) then fail("aces monotonic 1.0<4.0") end
    if not (tm.aces(8.0) <= 1.0) then fail("aces saturates <= 1") end
    if not (tm.aces(8.0) > 0.9) then fail("aces(8) rolls toward white (>0.9)") end
    -- ACES darkens the linear midpoint (the deliberate filmic look): aces(0.18) < 0.18.
    if not (tm.aces(0.18) < 0.18) then fail("aces compresses linear mid 0.18") end
end
```

- [ ] **Step 2: Run the harness to verify the new block fails**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: a non-zero failure count — `attempt to call ... (a nil value)` or a FAIL line under `== Tonemap math ==` (functions not yet returning correct values). It must NOT print `ALL RENDER HARNESS CHECKS PASSED`.

> Note: the `tm` functions ARE defined in the block above, so to see a genuine red first, temporarily comment out the three `function tm....` lines, run (expect `attempt to call a nil value (field 'srgbDecode')`), then restore them and re-run to confirm green. This proves the assertions are load-bearing.

- [ ] **Step 3: Confirm the math block passes**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: `ALL RENDER HARNESS CHECKS PASSED`, exit 0.

- [ ] **Step 4: Add the shader uniform + helpers + branch**

In `GachaClient/data/shader/post/post_process.glsl`:

(a) After line 38 (`extern vec2 pixelate_size;`), add:

```glsl
extern float tonemap;                 // 0 = display-space grade (UI/today); >0.5 = HDR scene: decode->linear bloom->ACES->encode

// sRGB transfer (approx gamma 2.2; switch to the piecewise sRGB curve only if banding appears).
vec3 srgbDecode(vec3 c) { return pow(max(c, 0.0), vec3(2.2)); }
vec3 srgbEncode(vec3 c) { return pow(max(c, 0.0), vec3(1.0 / 2.2)); }

// Narkowicz ACES filmic approximation (linear in -> tonemapped [0,1] out).
vec3 ACESFilmic(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
```

(b) Replace the body of `effect()` (lines 246-286) so the tonemap branch wraps the existing grade. The `tonemap == 0.0` path is the current code verbatim:

```glsl
vec4 effect(vec4 color, Image texture, vec2 texture_coords, vec2 screen_coords) {
    vec2 uv = texture_coords;
    // Optional pixelation, folded into the single grade pass (PixelateEffect retired).
    if (pixelate_size.x > 0.0) {
        uv = floor(uv / pixelate_size) * pixelate_size + pixelate_size * 0.5;
    }

    // Sample with chromatic aberration
    vec3 finalColor = getChromaticAberration(texture, uv);

    // Add bloom (glow from bright areas)
    vec3 bloom = getBloom(texture, uv);

    if (tonemap > 0.5) {
        // HDR scene path: composite is float (rgba16f) and may exceed 1.0. Decode to linear,
        // add bloom in linear, ACES filmic rolloff, encode back to display sRGB. The artistic
        // grade below then runs unchanged on the [0,1] display value.
        vec3 lin = srgbDecode(finalColor) + srgbDecode(bloom);
        finalColor = srgbEncode(ACESFilmic(lin));
    } else {
        // Display-space path (UI / today): bloom added directly, no tonemap.
        finalColor += bloom;
    }

    // Apply color grading
    finalColor = applyColorGrading(finalColor);

    // Apply vignette
    float vignette = getVignette(uv);
    finalColor *= vignette;

    // Apply dithering (reduces banding, adds retro feel)
    if (dither_intensity > 0.0) {
        float dither = getBayerDither(screen_coords);
        finalColor += dither;
    }

    // Apply film grain (animated noise) - soft blend that never creates black spots
    if (grain_intensity > 0.0) {
        float grain = getFilmGrain(uv, screen_coords);
        float grainMult = 1.0 + grain;
        grainMult = max(grainMult, 0.85);
        finalColor *= grainMult;
    }

    // Get alpha from original sample
    float alpha = Texel(texture, uv).a;

    return vec4(clamp(finalColor, 0.0, 1.0), alpha);
}
```

- [ ] **Step 5: Wire the `tonemap` uniform in PostFx**

In `GachaClient/ui/components/PostFx.lua`, change `send` and `apply` to accept an optional `tonemap` argument and upload it (guarded, default 0 so a prior screen's value never leaks).

Replace the `send` signature/body start at line 45:

```lua
function PostFx.send(sh, s, time, tonemap)
```

Add, inside `send`, right after the `pixelate_size` block (after line 61, before the color-calibration `brightness` line):

```lua
    -- Tonemap is a per-LAYER property (not a settings field): the caller passes it explicitly so
    -- the same settings table can grade a scene tonemapped AND its UI widgets un-tonemapped.
    if sh:hasUniform("tonemap") then sh:send("tonemap", tonemap and 1.0 or 0.0) end
```

Change `apply` (line 72) to thread the arg:

```lua
function PostFx.apply(canvas, s, time, tonemap)
    local sh = PostFx.shader()
    love.graphics.setColor(1, 1, 1, 1)
    if not sh or not Settings.postFx() then
        love.graphics.draw(canvas, 0, 0)
        return
    end
    love.graphics.setShader(sh)
    PostFx.send(sh, s, time, tonemap)
    love.graphics.draw(canvas, 0, 0)
    love.graphics.setShader()
end
```

- [ ] **Step 6: Re-run the harness (still green — no caller passes tonemap yet)**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: `ALL RENDER HARNESS CHECKS PASSED`, exit 0. (The shader/PostFx edits are not exercised headlessly; this confirms nothing else broke.)

- [ ] **Step 7: Commit**

```bash
git add GachaClient/data/shader/post/post_process.glsl GachaClient/ui/components/PostFx.lua GachaClient/tests/render_harness/main.lua
git commit -m "feat(render): add tonemap branch to grade shader (2c-3 task 1)

Narkowicz ACES + sRGB decode/encode in post_process.glsl, gated by a new
tonemap uniform (default 0 = today's display-space path, byte-identical).
PostFx.send/apply gain an optional tonemap arg. No caller passes it true
yet, so no behavior change. Headless math-property tests added.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: Format threading — activate HDR+tonemap for overworld + the 3 overlay screens

**Visual-gated** (the deliberate look change). Activates tonemap for producers whose `tonemap` flag is already true: `Profile.WORLD` (overworld) and `Profile.forScreenDef` overlay screens (`screen_1` Gacha Pull, `screen_0`, `screen_2`).

**Files:**
- Modify: `GachaClient/main.lua` (`composeScene` ≈ 562-594; `composeScreensScene` ≈ 522-557; `produceUI` ≈ 619-628)
- Modify: `GachaClient/systems/RenderSystem.lua:698`

- [ ] **Step 1: Add a one-time `rgba16f` availability probe to main.lua**

Near the other compositor locals in `GachaClient/main.lua` (just above `composeScene` at line 559), add a lazily-computed support flag:

```lua
-- HDR scene targets need rgba16f; probe once. If unsupported, scene layers fall back to rgba8
-- (tonemap still runs on [0,1] — bloom clips, no crash).
local _hdrSupported = nil
local function hdrSupported()
    if _hdrSupported == nil then
        local fmts = love.graphics.getCanvasFormats()
        _hdrSupported = fmts and fmts.rgba16f == true
    end
    return _hdrSupported
end
```

- [ ] **Step 2: `composeScene` — set `ctx.sceneFormat` and pass `tonemap` to the grade**

In `GachaClient/main.lua` `composeScene` (line 562), set the format **before** calling the producer, and thread the flag into the grade. Replace lines 562-585:

```lua
local function composeScene(ctx, producer, layer)
    -- HDR scene buffer: float only when this layer tonemaps AND the GPU supports rgba16f.
    ctx.sceneFormat = (layer and layer.tonemap and hdrSupported()) and "rgba16f" or nil
    producer(ctx)
    local src = ctx.sceneRT
    if not src then return end

    if layer and layer.effects then
        for _, name in ipairs(layer.effects) do
            local fx = SceneEffects[name]
            if fx then fx(ctx) end
        end
    end

    local Settings    = require "services.Settings"
    local PostFx      = require "ui.components.PostFx"
    local RenderScale = require "ui.components.RenderScale"

    -- Grade at the source resolution (a COPY; ctx.sceneRT stays sharp for any effects/consumers).
    -- The graded copy stays rgba8: the grade OUTPUTS display-encoded sRGB, so only the pre-grade
    -- sceneRT needs to be float. tonemap is the per-layer flag.
    if ctx.sceneSettings and Settings.postFx() and ctx.sceneSettings.enabled ~= false then
        local graded = ctx.targets:acquire(src:getWidth(), src:getHeight())
        love.graphics.setCanvas(graded)
        love.graphics.clear(0, 0, 0, 1)
        PostFx.apply(src, ctx.sceneSettings, love.timer.getTime(), layer and layer.tonemap)
        src = graded
    end
```

(Leave lines 587-594 — the present/upscale block — unchanged.)

- [ ] **Step 3: `composeScreensScene` — acquire the sceneRT (and scratch) at `ctx.sceneFormat`**

In `GachaClient/main.lua` `composeScreensScene`, line 524, change:

```lua
    local rt = ctx.targets:acquire(vw, vh, ctx.sceneFormat)
```

and line 536 (the cross-transition scratch canvas):

```lua
                local scr = ctx.targets:acquire(vw, vh, ctx.sceneFormat)
```

- [ ] **Step 4: `RenderSystem` — acquire the scene canvas at `ctx.sceneFormat`**

In `GachaClient/systems/RenderSystem.lua`, line 698, change:

```lua
    local ppCanvas = ctx.targets:acquire(renderW, renderH, ctx.sceneFormat)
```

(The rest of `RenderSystem:draw` is unchanged — it already publishes `ctx.sceneRT = ppCanvas` at line 1368 and `ctx.sceneSettings`/`ctx.sceneScaled`.)

- [ ] **Step 5: `produceUI` — pass explicit `false` so UI never tonemaps**

In `GachaClient/main.lua` `produceUI`, line 628, change the graded-UI call so UI is explicitly never tonemapped (display-space sRGB):

```lua
        PostFx.apply(cap, ctx.sceneSettings, love.timer.getTime(), false)   -- UI is sRGB: never tonemap
```

- [ ] **Step 6: Headless smoke — harness still green**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: `ALL RENDER HARNESS CHECKS PASSED`, exit 0. (Confirms no syntax break; the GL behavior is visual-gated.)

- [ ] **Step 7: VISUAL GATE (user-confirmed — cannot verify headlessly)**

Launch `GachaClient/run.bat`. Capture before/after and confirm with the user:
- **Overworld** (PLAYING): now ACES-tonemapped — expect filmic highlight rolloff + slightly compressed mids. Acceptable/intended.
- **Gacha reveal** (`screen_1`): the `pull_tunnel` bloom now rolls off instead of clipping; frosted panels now sample an HDR sceneRT — verify the blur doesn't blow out (values >1 may brighten it).
- **Regression check (must be IDENTICAL):** plain menus (no `overlay` block → `tonemap=false`), all UI widgets, the FPS HUD, and the warp flash.
- **F9 profiler:** no new pass; `scene` scope cost acceptable with the float buffer.
- **GPU-fallback sanity:** behavior is graceful if `rgba16f` is unsupported (no crash; tonemap on clipped 0–1).

> Implementer: do NOT claim success on the visual items. Report them as "needs user confirmation" and stop for review.

- [ ] **Step 8: Commit (after user confirms the visual gate)**

```bash
git add GachaClient/main.lua GachaClient/systems/RenderSystem.lua
git commit -m "feat(render): thread HDR format + activate tonemap for world/overlay (2c-3 task 2)

composeScene sets ctx.sceneFormat=rgba16f when the active scene layer
tonemaps (rgba16f-availability gated) and passes the flag to PostFx;
composeScreensScene + RenderSystem acquire the sceneRT at that format;
produceUI explicitly never tonemaps (UI stays sRGB). Activates ACES for
the overworld and the 3 post_process overlay screens.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: Combat + WaveGame go HDR (forProducer `tonemap` opt + neutral grade)

**Visual-gated** + one headless component (the `forProducer` shape). Flips `WaveGame` and `CombatScreen` to HDR so their additive holographic/bloom glow gets filmic rolloff instead of clipping at 1.0. They reach the tonemap by (a) `forProducer({tonemap=true})` on the scene layer and (b) a non-nil **neutral** `gradeSettings` (zeroed bloom/vignette/grain) so the grade pass runs without adding a second bloom or any vignette/grain over their own internal post.

**Files:**
- Modify: `GachaClient/systems/render/Profile.lua:107-120` (`forProducer`)
- Modify: `GachaClient/tests/render_harness/main.lua` (the `== Profile forProducer ==` block, lines 368-394)
- Modify: `GachaClient/ui/screens/combat/init.lua:44-46`
- Modify: `GachaClient/ui/screens/dailies/WaveGame.lua:293-294`

- [ ] **Step 1: Extend the `forProducer` headless test (failing)**

In `GachaClient/tests/render_harness/main.lua`, inside the `== Profile forProducer ==` block, add assertions. After line 378 (`eq(p.layers[1].aa, true, "scene aa")`) add:

```lua
    eq(p.layers[1].tonemap, false, "tonemap off by default")
```

And after line 388 (`eq(q.layers[1].scale, true, "scale opt honored")`) add:

```lua
    local t = Profile.forProducer({ tonemap = true })
    eq(t.layers[1].tonemap, true, "tonemap opt honored on scene layer")
    eq(t.layers[2].tonemap, false, "ui layer never tonemaps")
    eq(t.layers[3].tonemap, false, "hud layer never tonemaps")
```

- [ ] **Step 2: Run the harness to verify it fails**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: FAIL under `== Profile forProducer ==` — `tonemap opt honored on scene layer expected true got nil` (the opt isn't read yet).

- [ ] **Step 3: Add the `tonemap` opt to `forProducer`**

In `GachaClient/systems/render/Profile.lua`, replace `forProducer` (lines 107-120):

```lua
function Profile.forProducer(opts)
    opts = opts or {}
    local effects = opts.effects or {}
    local grade = opts.grade and true or false
    local scale = opts.scale and true or false
    local tonemap = opts.tonemap and true or false
    return {
        layers = {
            Layer.resolve({ name = "scene", source = "background", scale = scale, grade = grade, tonemap = tonemap, aa = true,  effects = effects }),
            Layer.resolve({ name = "ui",    source = "ui",         scale = false, grade = grade, tonemap = false,   aa = true,  effects = {} }),
            Layer.resolve({ name = "hud",   source = "hud",        scale = false, grade = false, tonemap = false,   aa = false, effects = {} }),
        },
        overlay = false,
    }
end
```

- [ ] **Step 4: Run the harness to verify it passes**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: `ALL RENDER HARNESS CHECKS PASSED`, exit 0.

- [ ] **Step 5: Flip CombatScreen to HDR + neutral grade**

In `GachaClient/ui/screens/combat/init.lua`, replace lines 44-46:

```lua
    -- 2c-3: HDR scene. tonemap=true -> rgba16f sceneRT + ACES on the additive holographic glow.
    -- Neutral gradeSettings (zeroed) routes through the one grade so tonemap runs, WITHOUT adding a
    -- second bloom or any vignette/grain over the renderer's own internal post.
    screen.renderProfile = Profile.forProducer({ tonemap = true })
    screen.gradeSettings = { bloom_intensity = 0, vignette_intensity = 0, grain_intensity = 0 }
    screen.coversBelow   = true
```

- [ ] **Step 6: Flip WaveGame to HDR + neutral grade**

In `GachaClient/ui/screens/dailies/WaveGame.lua`, replace lines 293-294:

```lua
    screen.renderProfile = Profile.forProducer({ tonemap = true })
    -- 2c-3: neutral gradeSettings routes through the one grade so the tonemap runs (WaveGame keeps
    -- its own internal bloom/lens in drawScene; the grade adds only the ACES rolloff + encode).
    screen.gradeSettings = { bloom_intensity = 0, vignette_intensity = 0, grain_intensity = 0 }
```

(Leave line 295 `screen.coversBelow = true` unchanged.)

- [ ] **Step 7: VISUAL GATE (user-confirmed — cannot verify headlessly)**

Launch `GachaClient/run.bat`:
- **Combat** (F4 test encounter): the additive holographic glow now rolls off filmically instead of clipping to white. Confirm it reads as intended (brighter glows compress rather than blow out).
- **WaveGame** (Dailies → wave game): its internal 2-pass bloom + near-miss lens now ACES-tonemapped; confirm no double-bloom and the look is acceptable.
- **Regression:** combat/wave HUD (drawn in the `ui`/`hud` layers, `tonemap=false`) unchanged.

> Implementer: report visual items as "needs user confirmation"; do not claim success.

- [ ] **Step 8: Commit (after user confirms the visual gate)**

```bash
git add GachaClient/systems/render/Profile.lua GachaClient/tests/render_harness/main.lua GachaClient/ui/screens/combat/init.lua GachaClient/ui/screens/dailies/WaveGame.lua
git commit -m "feat(render): combat + wave go HDR/ACES (2c-3 task 3)

forProducer gains a tonemap opt; CombatScreen and WaveGame set it true +
a neutral (zeroed) gradeSettings so their additive glow routes through the
one grade and gets filmic ACES rolloff instead of clipping at 1.0.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Self-review (against the spec)

**Spec coverage:**
- Decision 1 (tonemap-wrap, no input linearization, grade not re-tuned) → Task 1 shader branch decodes the composite (not per-input) and runs the existing grade unchanged on the encoded result. ✓
- Decision 2 (Narkowicz ACES) → Task 1 `ACESFilmic` + mirror test. ✓
- Decision 3 (gate tonemap by per-layer flag; no `ui.grade=false`) → Task 1 `tonemap` uniform; Task 2 passes `layer.tonemap` to the scene grade and explicit `false` to `produceUI`; `ui.grade` untouched. ✓
- Decision 4 (4 producer types) → Tasks 2 (world + 3 overlay screens, already flagged) + 3 (combat/wave). ✓
- Decision 5 (single grade path + neutral gradeSettings) → Task 3 neutral table; no separate tonemap shader. ✓
- Decision 6 (rgba8 fallback) → Task 2 `hdrSupported()` probe; sceneFormat nil-falls-back, tonemap still passed. ✓
- Decision 7 (no persistent flag) → none added; A/B via git. ✓
- `Settings.postFx()` gate note (tonemap off when post-fx disabled) → falls out of the unchanged grade gate in `composeScene`/`PostFx.apply`. ✓
- Testing: headless math (Task 1) + `forProducer` shape (Task 3) + per-producer visual gates (Tasks 2, 3) + untouched-path regression checks. ✓

**Placeholder scan:** none — every code step shows full content and exact commands.

**Type/name consistency:** `tonemap` arg threaded consistently `PostFx.apply(…, tonemap)` → `PostFx.send(…, tonemap)` → uniform `tonemap`; `ctx.sceneFormat` written by `composeScene`, read by `composeScreensScene` + `RenderSystem`; `forProducer({tonemap=true})` matches the new opt and the harness assertions. Neutral gradeSettings table identical in both screens. ✓

**Risk left to the visual gate (not a plan gap):** the exact look of ACES on each producer, frosted-on-HDR brightness, and float-canvas filtering — all explicitly listed in the Task 2/3 visual gates per the spec's "deliberate look change" gating.
