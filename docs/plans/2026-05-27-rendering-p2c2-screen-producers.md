# Rendering Phase 2c-2 — Screen Scene Producers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fold declarative UI screens into the unified layer compositor — each screen renders its background once into the shared `ctx.sceneRT` via one `composeScene` primitive, `frosted` becomes a scene-layer effect sampling sceneRT (the pull reveal's heavy bg shader runs 1× instead of 2–3×), and the overlay path is homogenized to a single `PostFx` grade (pixelate folded in, `PixelateEffect`/`PostProcessEffect` capture roles retired). Parity-gated; no look change.

**Architecture:** Generalize `composeWorld` → `composeScene(ctx, producer, layer)` (world + screens + later WaveGame/CombatRenderer share it). A per-frame `resolveFrame` selects the active profile + scene producer from the screen stack; `composeScreensScene` composites visible non-overlay screens' backgrounds into one sceneRT at their per-screen transition scale+alpha. `ui_loader` auto-derives `drawScene`/`renderProfile`/`gradeSettings`; `screen:draw` reduces to widgets-only.

**Tech Stack:** LÖVE 11.x / LuaJIT. Modules under `systems/render/` (Layer, Profile, Renderer, RenderTargets, FrameCtx, Pass, Profiler — all exist) and `ui/components/` (PostFx, FrostedGlass, FrameAA, RenderScale). Headless harness: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`.

**Spec:** `docs/superpowers/specs/2026-05-27-rendering-2c2-screen-producers-design.md`.

**Governing constraint (homogenization):** one scene path, one grade pass — no bespoke per-renderer chains, no second capture pass. Combat **abilities** are off-limits.

**Git hygiene:** the working tree is heavily dirty. Use **targeted `git add <file>`** per task — never `git add -A`/`.`.

---

## File Structure

| File | Change |
|---|---|
| `GachaClient/systems/render/Profile.lua` | **Modify** — add `Profile.forScreenDef(def)` (pure profile derivation) |
| `GachaClient/ui/core/screenstack.lua` | **Modify** — add `ScreenStack:sceneScreens()` (visible non-overlay, bottom→top) |
| `GachaClient/ui/init.lua` | **Modify** — add `UI.sceneScreens()` |
| `GachaClient/data/shader/post/post_process.glsl` | **Modify** — add `pixelate_size` uniform + uv quantization |
| `GachaClient/ui/components/PostFx.lua` | **Modify** — always-send `pixelate_size` |
| `GachaClient/ui/shader_registry.lua` | **Modify** — drop `PixelateEffect` require + `overlays.pixelate` |
| `GachaClient/ui/components/PixelateEffect.lua` | **Delete** |
| `GachaClient/tests/assets_harness/main.lua` | **Modify** — drop `PixelateEffect.lua` file entry |
| `GachaClient/ui/components/FrostedGlass.lua` | **Modify** — add `fromScene(src)`; later remove `prepare` |
| `GachaClient/systems/render/SceneEffects.lua` | **Create** — `{ frosted = … }` dispatch |
| `GachaClient/main.lua` | **Modify** — `composeScene`/`worldProducer`, `produceLayer` dispatch, `produceUI` grade variant, `resolveFrame`, `composeScreensScene`, `composeFrame` |
| `GachaClient/ui/ui_loader.lua` | **Modify** — auto-derive `drawScene`/`renderProfile`/`gradeSettings`; simplify `screen:draw` |
| `GachaClient/ui/components/PostProcessEffect.lua` | **Modify** — retire capture/grade role (settings carrier) |
| `GachaClient/tests/render_harness/main.lua` | **Modify** — add tests for the pure helpers |

**Verification legend:** _Headless_ = `lovec.exe GachaClient/tests/render_harness` → "ALL RENDER HARNESS CHECKS PASSED". _Visual_ = launch `GachaClient/run.bat`, exercise the feature, press F9 — **flag for the user; do not claim success on visual gates.**

---

### Task 1: `Profile.forScreenDef(def)` — pure profile derivation

**Files:**
- Modify: `GachaClient/systems/render/Profile.lua` (add function before `return Profile`)
- Test: `GachaClient/tests/render_harness/main.lua`

- [ ] **Step 1: Write the failing test** — insert before the final `if fails == 0` line (currently line ~323) in `tests/render_harness/main.lua`:

```lua
print("== Profile forScreenDef ==")
do
    local Profile = require "systems.render.Profile"
    -- A declarative screen WITH a post_process overlay -> MENU-shaped, graded.
    local p = Profile.forScreenDef({ background = { shader = "x" }, overlay = { shader = "post_process" } })
    eq(#p.layers, 3, "forScreenDef: 3 layers")
    eq(p.layers[1].source, "background", "scene layer is background")
    eq(p.layers[1].effects[1], "frosted", "scene has frosted effect")
    eq(p.layers[1].grade, true, "overlay present -> scene graded")
    eq(p.layers[2].source, "ui", "layer2 ui")
    eq(p.layers[2].grade, true, "overlay present -> ui graded")
    eq(p.layers[3].source, "hud", "layer3 hud")
    eq(p.layers[3].aa, false, "hud sharp")
    eq(p.overlay, false, "not an overlay profile")
    -- No overlay -> same shape, but NOT graded; frosted still present (prepare ran unconditionally today).
    local q = Profile.forScreenDef({ background = { shader = "x" } })
    eq(q.layers[1].grade, false, "no overlay -> scene not graded")
    eq(q.layers[2].grade, false, "no overlay -> ui not graded")
    eq(q.layers[1].effects[1], "frosted", "frosted present without overlay")
    -- Deep-copied: mutating the result must not bleed into another call.
    p.layers[1].effects[1] = "z"
    local r = Profile.forScreenDef({ background = { shader = "x" }, overlay = { shader = "post_process" } })
    eq(r.layers[1].effects[1], "frosted", "forScreenDef returns independent tables")
end
```

- [ ] **Step 2: Run to verify it fails**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: FAIL — `attempt to call field 'forScreenDef' (a nil value)`.

- [ ] **Step 3: Implement** — in `GachaClient/systems/render/Profile.lua`, add before `return Profile`:

```lua
-- Build a render profile for a declarative screen from its def table. Every declarative screen is
-- MENU-shaped: a background scene layer (with the frosted effect), then ui, then hud. The grade flag
-- on the scene + ui layers is on iff the screen declares an overlay (today only `post_process` screens
-- are graded). Pure: FFI-neutral, headless-testable. Each call returns independent (Layer.resolve'd) tables.
function Profile.forScreenDef(def)
    local graded = def ~= nil and def.overlay ~= nil
    return {
        layers = {
            Layer.resolve({ name = "scene", source = "background", scale = false, grade = graded, tonemap = graded, aa = true,  effects = { "frosted" } }),
            Layer.resolve({ name = "ui",    source = "ui",         scale = false, grade = graded, tonemap = false,  aa = true,  effects = {} }),
            Layer.resolve({ name = "hud",   source = "hud",        scale = false, grade = false,  tonemap = false,  aa = false, effects = {} }),
        },
        overlay = false,
    }
end
```

- [ ] **Step 4: Run to verify it passes**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: PASS — "ALL RENDER HARNESS CHECKS PASSED".

- [ ] **Step 5: Commit**

```bash
git add GachaClient/systems/render/Profile.lua GachaClient/tests/render_harness/main.lua
git commit -m "feat(render): Profile.forScreenDef — derive a screen's layered profile"
```

---

### Task 2: `ScreenStack:sceneScreens()` + `UI.sceneScreens()`

**Files:**
- Modify: `GachaClient/ui/core/screenstack.lua` (add method before `return ScreenStack`)
- Modify: `GachaClient/ui/init.lua` (add `UI.sceneScreens`)
- Test: `GachaClient/tests/render_harness/main.lua`

- [ ] **Step 1: Write the failing test** — insert before the final summary line in `tests/render_harness/main.lua`:

```lua
print("== ScreenStack sceneScreens ==")
do
    local ScreenStack = require "ui.core.screenstack"
    local s = ScreenStack.new()
    local base    = { id = "base",    visible = true }
    local top     = { id = "top",     visible = true }
    local hidden  = { id = "hidden",  visible = false }
    local exiting = { id = "exiting", visible = true }
    local modal   = { id = "modal",   visible = true }
    s.screens   = { base, hidden, top }
    s.exitingScreens = { exiting }
    s.overlays  = { modal }
    local out = s:sceneScreens()
    -- exiting (behind) first, then stack bottom->top, hidden + overlays excluded.
    eq(#out, 3, "sceneScreens count (exiting + 2 visible stack)")
    eq(out[1].id, "exiting", "exiting screens first")
    eq(out[2].id, "base", "then stack bottom")
    eq(out[3].id, "top", "then stack top")
    -- empty stack
    local e = ScreenStack.new()
    eq(#e:sceneScreens(), 0, "empty stack -> none")
end
```

- [ ] **Step 2: Run to verify it fails**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: FAIL — `attempt to call method 'sceneScreens' (a nil value)`.

- [ ] **Step 3a: Implement `ScreenStack:sceneScreens`** — in `GachaClient/ui/core/screenstack.lua`, add before `return ScreenStack`:

```lua
--- Visible non-overlay screens, bottom→top: exiting screens (drawn behind) then the stack.
--- These are the screens whose backgrounds the scene compositor renders into ctx.sceneRT.
--- Overlays (modals/tooltips) are excluded — they have no background scene layer.
-- @return table Array of screens (may be empty)
function ScreenStack:sceneScreens()
    local out = {}
    for _, s in ipairs(self.exitingScreens) do
        if s.visible ~= false then out[#out + 1] = s end
    end
    for _, s in ipairs(self.screens) do
        if s.visible ~= false then out[#out + 1] = s end
    end
    return out
end
```

- [ ] **Step 3b: Implement `UI.sceneScreens`** — in `GachaClient/ui/init.lua`, add near `UI.hasScreens` (after it):

```lua
--- Visible non-overlay screens bottom→top, for the scene compositor. Empty when no stack.
-- @return table
function UI.sceneScreens()
    if UI.stack then return UI.stack:sceneScreens() end
    return {}
end
```

- [ ] **Step 4: Run to verify it passes**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: PASS — "ALL RENDER HARNESS CHECKS PASSED".

- [ ] **Step 5: Commit**

```bash
git add GachaClient/ui/core/screenstack.lua GachaClient/ui/init.lua GachaClient/tests/render_harness/main.lua
git commit -m "feat(ui): sceneScreens — visible non-overlay screens for the scene compositor"
```

---

### Task 3: Homogenize overlays — fold pixelate into the grade, retire `PixelateEffect`

**Files:**
- Modify: `GachaClient/data/shader/post/post_process.glsl`
- Modify: `GachaClient/ui/components/PostFx.lua:45-61` (`PostFx.send`)
- Modify: `GachaClient/ui/shader_registry.lua:18,66-68`
- Delete: `GachaClient/ui/components/PixelateEffect.lua`
- Modify: `GachaClient/tests/assets_harness/main.lua:18`

This task is additive/parity-safe: `pixelate_size` defaults to `{0,0}` (off) and no screen uses the `pixelate` overlay. No headless coverage (GL/data) — verify by boot + assets harness.

- [ ] **Step 1: Add the uniform + quantization to `post_process.glsl`** — add the extern near the others (after the `time`/`screen_size` externs, ~line 5):

```glsl
extern vec2 pixelate_size;            // UV-space block size; <=0 disables (Default: vec2(0.0))
```

And change the top of `effect()` (currently `vec2 uv = texture_coords;`, ~line 245) to:

```glsl
    vec2 uv = texture_coords;
    // Optional pixelation, folded into the single grade pass (PixelateEffect retired).
    if (pixelate_size.x > 0.0) {
        uv = floor(uv / pixelate_size) * pixelate_size + pixelate_size * 0.5;
    }
```

- [ ] **Step 2: Always-send `pixelate_size` in `PostFx.send`** — in `GachaClient/ui/components/PostFx.lua`, after the bloom block (line 54, before the color-calibration comment) insert:

```lua
    -- Pixelation: folded into this grade pass (replaces the standalone PixelateEffect overlay).
    -- s.pixelate_block is a screen-pixel block size; 0/absent = off. Always sent so a prior screen's
    -- value never leaks across the shared shader instance.
    if sh:hasUniform("pixelate_size") then
        local b = s.pixelate_block or 0
        sh:send("pixelate_size", (b > 0) and { b / W, b / H } or { 0, 0 })
    end
```

- [ ] **Step 3: Drop the `pixelate` overlay class** — in `GachaClient/ui/shader_registry.lua`:
  - Delete line 18: `local PixelateEffect    = require "ui.components.PixelateEffect"`
  - Delete the `overlays.pixelate` entry (lines 64-68):

```lua
    -- Pixelation (pixelate.glsl)
    -- opts: { block_size = 8 }
    pixelate = function(opts)
        return PixelateEffect.new(opts)
    end,
```

- [ ] **Step 4: Delete the file**

```bash
git rm GachaClient/ui/components/PixelateEffect.lua
```

- [ ] **Step 5: Drop the harness reference** — in `GachaClient/tests/assets_harness/main.lua:18`, remove the `"ui/components/PixelateEffect.lua",` entry from the file list (leave the `"pixelate"` shader entry on line ~145 — `pixelate.glsl` stays for the combat/particle path).

- [ ] **Step 6: Verify** — run the assets harness and boot:

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness`
Expected: passes (no missing-file error for PixelateEffect).
Then **Visual (flag for user):** launch `run.bat`; the shader compiles (no `[ShaderRegistry]`/compile errors in console) and screens 0/1/2 look identical (pixelate off by default).

- [ ] **Step 7: Commit**

```bash
git add GachaClient/data/shader/post/post_process.glsl GachaClient/ui/components/PostFx.lua GachaClient/ui/shader_registry.lua GachaClient/tests/assets_harness/main.lua GachaClient/ui/components/PixelateEffect.lua
git commit -m "refactor(render): fold pixelate into the post_process grade; retire PixelateEffect overlay"
```

---

### Task 4: `FrostedGlass.fromScene(src)` + `SceneEffects` dispatch

**Files:**
- Modify: `GachaClient/ui/components/FrostedGlass.lua` (add `fromScene`; keep `prepare`)
- Create: `GachaClient/systems/render/SceneEffects.lua`
- Test: `GachaClient/tests/render_harness/main.lua`

Additive: nothing calls these yet. `fromScene` is GL (visual-gated at the flip); the `SceneEffects` shape is headless.

- [ ] **Step 1: Write the failing test** — insert before the final summary line in `tests/render_harness/main.lua`:

```lua
print("== SceneEffects dispatch ==")
do
    local SceneEffects = require "systems.render.SceneEffects"
    eq(type(SceneEffects.frosted), "function", "frosted effect is a function")
end
```

- [ ] **Step 2: Run to verify it fails**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: FAIL — module `systems.render.SceneEffects` not found.

- [ ] **Step 3a: Create `SceneEffects.lua`** — `GachaClient/systems/render/SceneEffects.lua`:

```lua
-- systems/render/SceneEffects.lua
-- Dispatch table for scene-layer effects. An effect produces an auxiliary resource FROM ctx.sceneRT
-- that widgets later sample (e.g. the frosted blur). It is NOT a fullscreen post — that lives in the
-- PostFx grade. composeScene runs the active scene layer's `effects` set through this table after the
-- producer fills ctx.sceneRT and before the grade, so the effect samples the SHARP scene.
local SceneEffects = {
    frosted = function(ctx)
        require("ui.components.FrostedGlass").fromScene(ctx.sceneRT)
    end,
}

return SceneEffects
```

- [ ] **Step 3b: Add `FrostedGlass.fromScene`** — in `GachaClient/ui/components/FrostedGlass.lua`, add after `prepare` (before `function FrostedGlass.canvas`):

```lua
-- Blur an ALREADY-rendered canvas (the scene RT) into the frosted-glass source. Same 2-pass blur as
-- prepare(), minus the background re-render — the 2c-2 win (heavy bg shaders run once). Leaves the
-- default render target active on return.
function FrostedGlass.fromScene(src)
    loadShader()
    if not blurShader or not src then FrostedGlass._out = nil; FrostedGlass._scene = nil; return end
    local W, H = src:getWidth(), src:getHeight()
    ensureCanvases(W, H)

    local prevCanvas = love.graphics.getCanvas()
    local prevShader = love.graphics.getShader()

    -- Horizontal blur: src -> tmpC
    love.graphics.setCanvas(tmpC)
    love.graphics.clear(0, 0, 0, 1)
    love.graphics.setShader(blurShader)
    love.graphics.setColor(1, 1, 1, 1)
    blurShader:send("direction", { STEP / W, 0 })
    love.graphics.draw(src, 0, 0)

    -- Vertical blur: tmpC -> outC
    love.graphics.setCanvas(outC)
    love.graphics.clear(0, 0, 0, 1)
    blurShader:send("direction", { 0, STEP / H })
    love.graphics.draw(tmpC, 0, 0)

    love.graphics.setShader(prevShader)
    love.graphics.setCanvas(prevCanvas)
    love.graphics.setColor(1, 1, 1, 1)

    FrostedGlass._out   = outC
    FrostedGlass._scene = src   -- the sharp scene RT, for the glass lattice's refraction
end
```

- [ ] **Step 4: Run to verify it passes**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: PASS — "ALL RENDER HARNESS CHECKS PASSED".

- [ ] **Step 5: Commit**

```bash
git add GachaClient/ui/components/FrostedGlass.lua GachaClient/systems/render/SceneEffects.lua GachaClient/tests/render_harness/main.lua
git commit -m "feat(render): FrostedGlass.fromScene + SceneEffects dispatch"
```

---

### Task 5: `composeScene(ctx, producer, layer)` — world refactor + producer plumbing

**Files:**
- Modify: `GachaClient/main.lua` — replace `composeWorld` (currently `main.lua:510-540`); update `produceUI` (544), `produceHud` (563), `produceLayer` (584). `composeFrame` is **unchanged** in this task (profile selection still via `Profile.frameProfile`).

No headless coverage (GL). **Visual+F9:** overworld parity across None/FXAA/MSAA + render-scale + resize; menus unchanged (still `PLAIN_UI` — `background` branch and `produceUI` grade stay dormant).

- [ ] **Step 1: Add the `SceneEffects` require** — near the other `systems.render` requires at the top of `main.lua`, add:

```lua
local SceneEffects = require "systems.render.SceneEffects"
```

- [ ] **Step 2: Replace `composeWorld` with `worldProducer` + `composeScene`** — replace the whole `local function composeWorld(ctx) ... end` block (`main.lua:510-540`) with:

```lua
-- The overworld scene producer: render the ECS world into ctx.sceneRT (+ sceneSettings, sceneScaled).
-- RenderSystem:draw is ECS-invoked, so the ctx crosses via FrameCtx (no ctx param on draw).
local function worldProducer(ctx)
    local FrameCtx = require "systems.render.FrameCtx"
    FrameCtx.set(ctx)
    state.world:update(0, nil, "draw")
    FrameCtx.set(nil)
end

-- The ONE canonical scene path. `producer` fills ctx.sceneRT (+ sceneSettings/sceneScaled); then the
-- layer's effects run (auxiliary resources sampling the SHARP sceneRT, e.g. frosted), then the grade,
-- then present (upscale if sceneScaled) into ctx.frame (or the screen when aa=="none").
local function composeScene(ctx, producer, layer)
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
    if ctx.sceneSettings and Settings.postFx() and ctx.sceneSettings.enabled ~= false then
        local graded = ctx.targets:acquire(src:getWidth(), src:getHeight())
        love.graphics.setCanvas(graded)
        love.graphics.clear(0, 0, 0, 1)
        PostFx.apply(src, ctx.sceneSettings, love.timer.getTime())
        src = graded
    end

    if ctx.frame then love.graphics.setCanvas(ctx.frame) else love.graphics.setCanvas() end
    love.graphics.setColor(1, 1, 1, 1)
    if ctx.sceneScaled and RenderScale.active() then
        RenderScale.upscale(src)
    else
        love.graphics.draw(src, 0, 0)
    end
end
```

- [ ] **Step 3: Make `produceUI` take a layer + grade variant** — replace `local function produceUI(ctx) ... end` (`main.lua:544-560`) with:

```lua
-- UI layer producer: warp screen-shake wrapping UI.draw, plus the screen transition. When the layer
-- is graded (MENU's ui.grade=true, option b), widgets are captured and graded through the one PostFx
-- pass; otherwise they draw straight into the current target (WORLD ui.grade=false, and 2c-3 onward).
local function produceUI(ctx, layer)
    local Settings = require "services.Settings"
    local function drawWidgets()
        local SpaceBackground = require "ui.components.SpaceBackground"
        local sx, sy = SpaceBackground.getShake()
        if not Settings.get("screenShake") then sx, sy = 0, 0 end
        if sx ~= 0 or sy ~= 0 then
            local w, h = love.graphics.getDimensions()
            love.graphics.push()
            love.graphics.translate(w * 0.5 + sx, h * 0.5 + sy)
            love.graphics.scale(1.04)
            love.graphics.translate(-w * 0.5, -h * 0.5)
            UI.draw()
            love.graphics.pop()
        else
            UI.draw()
        end
        UI.drawTransition()
    end

    if layer and layer.grade and ctx.sceneSettings and Settings.postFx() and ctx.sceneSettings.enabled ~= false then
        local PostFx = require "ui.components.PostFx"
        local target = ctx.frame
        local cap = ctx.targets:acquire(ctx.viewportW, ctx.viewportH)
        love.graphics.setCanvas(cap)
        love.graphics.clear(0, 0, 0, 0)
        drawWidgets()
        if target then love.graphics.setCanvas(target) else love.graphics.setCanvas() end
        love.graphics.setColor(1, 1, 1, 1)
        PostFx.apply(cap, ctx.sceneSettings, love.timer.getTime())
    else
        drawWidgets()
    end
end
```

- [ ] **Step 4: Give `produceHud` the layer param** — change its signature (`main.lua:563`) from `local function produceHud(ctx)` to `local function produceHud(ctx, layer)` (body unchanged).

- [ ] **Step 5: Route `produceLayer` through `composeScene`** — replace `local function produceLayer(layer, ctx) ... end` (`main.lua:584-597`) with:

```lua
-- Produce one layer by its source into the current target. world/background go through composeScene;
-- ui/hud have their own producers. The world keeps its PLAYING+live-world guard; background runs only
-- when a scene producer was selected for this frame (ctx.sceneProducer, set by resolveFrame).
local function produceLayer(layer, ctx)
    local s = layer.source
    if s == "world" then
        if state.gameState == GAME_STATE.PLAYING and state.world then
            composeScene(ctx, worldProducer, layer)
        end
    elseif s == "background" then
        if ctx.sceneProducer then composeScene(ctx, ctx.sceneProducer, layer) end
    elseif s == "ui" then
        produceUI(ctx, layer)
    elseif s == "hud" then
        produceHud(ctx, layer)
    end
end
```

- [ ] **Step 6: Sanity-grep** — confirm no stale `composeWorld` references remain:

Run: `Grep pattern="composeWorld" path="GachaClient/main.lua"`
Expected: no matches.

- [ ] **Step 7: Verify (Visual+F9 — flag for user)** — launch `run.bat`:
  - Overworld renders identically; F9 shows `compose` → `scene`/`ui`/`present`/`hud` scopes.
  - Toggle AA None/FXAA/MSAA (settings) + change render scale + resize the window — overworld stays correct.
  - Open a menu — still renders as before (screens unchanged this task).

- [ ] **Step 8: Commit**

```bash
git add GachaClient/main.lua
git commit -m "refactor(render): generalize composeWorld into composeScene(ctx, producer, layer)"
```

---

### Task 6: ui_loader auto-derive `drawScene` / `renderProfile` / `gradeSettings` (additive)

**Files:**
- Modify: `GachaClient/ui/ui_loader.lua` — add a `Profile` require; set the three members in the screen factory (after `_overlay` construction, ~line 792). **`screen:draw` is unchanged** in this task.

No headless coverage (constructing a screen needs GL/data). **Visual:** menus identical (the new members are unused until Task 7).

- [ ] **Step 1: Add the `Profile` require** — near the top requires of `ui_loader.lua` add:

```lua
local Profile = require "systems.render.Profile"
```

- [ ] **Step 2: Auto-derive the members** — in the screen factory, immediately after the overlay block (`ui_loader.lua:791-793`, the `if def.overlay and def.overlay.shader then screen._overlay = ... end`), add:

```lua
    -- 2c-2: declarative screens are scene producers. renderProfile drives the layered compositor;
    -- drawScene renders the background into ctx.sceneRT (the compositor owns the transition transform);
    -- gradeSettings is the post-process grade carried by _overlay (nil when the screen has no overlay).
    screen.renderProfile = Profile.forScreenDef(def)
    screen.gradeSettings = screen._overlay
    function screen:drawScene(ctx)
        local c = self._bgColor
        love.graphics.clear(c[1], c[2], c[3], c[4] or 1)
        love.graphics.setColor(1, 1, 1, 1)
        if self._bgBase   then self._bgBase:draw()   end
        if self._bgShader then self._bgShader:draw() end
    end
```

- [ ] **Step 3: Verify (Visual — flag for user)** — launch `run.bat`; every menu looks exactly as before (additive members are not yet consumed). No console errors at screen load.

- [ ] **Step 4: Commit**

```bash
git add GachaClient/ui/ui_loader.lua
git commit -m "feat(ui): auto-derive drawScene/renderProfile/gradeSettings on declarative screens"
```

---

### Task 7: The flip — screen scene producers live (`resolveFrame` + `composeScreensScene` + widgets-only `screen:draw`)

**Files:**
- Modify: `GachaClient/main.lua` — add `composeScreensScene` + `resolveFrame`; change `composeFrame` profile selection.
- Modify: `GachaClient/ui/ui_loader.lua` — reduce `screen:draw` (`ui_loader.lua:915-994`) to widgets-only.

This is the parity flip (bg leaves `screen:draw`, appears in the scene layer; the `frosted` win lands). One commit. No headless coverage. **Visual+F9 — flag for user; the full parity checklist is in Step 4.**

- [ ] **Step 1: Add `composeScreensScene`** — in `main.lua`, immediately after `worldProducer` (from Task 5), add:

```lua
-- Scene producer for menus: composite every visible non-overlay screen's background into ONE
-- ctx.sceneRT, bottom→top, each at its own transition scale+alpha. This mirrors screen.lua's transition
-- canvas (opaque draw → composite at scale/alpha), lifted into the compositor so a screen's bg + widgets
-- transition coherently and cross-transitions (exiting + entering) compose correctly. Single settled
-- screen takes a fast path (no scratch canvas). Screens lacking drawScene (hand-coded opt-outs) are skipped.
local function composeScreensScene(ctx)
    local vw, vh = ctx.viewportW, ctx.viewportH
    local rt = ctx.targets:acquire(vw, vh)
    love.graphics.setCanvas(rt)
    love.graphics.clear(0, 0, 0, 1)

    local scenes = UI.sceneScreens()
    for _, screen in ipairs(scenes) do
        if screen.drawScene then
            local scale, alpha = 1, 1
            if screen.getTransitionValues then scale, alpha = screen:getTransitionValues() end
            if alpha == 1 and scale == 1 and #scenes == 1 then
                screen:drawScene(ctx)                          -- fast path: straight into rt
            else
                local scr = ctx.targets:acquire(vw, vh)
                love.graphics.setCanvas(scr)
                love.graphics.clear(0, 0, 0, 0)
                screen:drawScene(ctx)                          -- opaque into scratch
                love.graphics.setCanvas(rt)
                love.graphics.push()
                love.graphics.translate(vw * 0.5, vh * 0.5)
                love.graphics.scale(scale, scale)
                love.graphics.translate(-vw * 0.5, -vh * 0.5)
                love.graphics.setColor(1, 1, 1, alpha)
                love.graphics.draw(scr, 0, 0)
                love.graphics.pop()
                love.graphics.setColor(1, 1, 1, 1)
            end
        end
    end

    local top = scenes[#scenes]
    ctx.sceneRT       = rt
    ctx.sceneScaled   = false
    ctx.sceneSettings = top and top.gradeSettings or nil
end
```

- [ ] **Step 2: Add `resolveFrame` + rewire `composeFrame`** — add `resolveFrame` immediately before `composeFrame` (`main.lua:603`):

```lua
-- Pick the active profile for this frame and bind the scene producer onto ctx. Screens present →
-- the topmost non-overlay screen's renderProfile (selectActive) + composeScreensScene; otherwise the
-- overworld → WORLD (its world layer is gated in produceLayer). Screens whose renderProfile is absent
-- (hand-coded opt-outs) fall back to PLAIN_UI.
local function resolveFrame(ctx)
    local scenes = UI.sceneScreens()
    if #scenes > 0 then
        local topDown = {}
        for i = #scenes, 1, -1 do topDown[#topDown + 1] = scenes[i].renderProfile or Profile.PLAIN_UI end
        ctx.sceneProducer = composeScreensScene
        return Profile.selectActive(topDown, Profile.WORLD)
    end
    ctx.sceneProducer = nil
    if UI.hasScreens() then return Profile.normalize(Profile.PLAIN_UI) end
    return Profile.normalize(Profile.WORLD)
end
```

Then in `composeFrame` (`main.lua:606`), replace:

```lua
    local plan = Profile.composePlan(Profile.frameProfile(UI.hasScreens()))
```

with:

```lua
    local plan = Profile.composePlan(resolveFrame(ctx))
```

- [ ] **Step 3: Reduce `screen:draw` to widgets-only** — in `GachaClient/ui/ui_loader.lua`, replace the entire `function screen:draw() ... end` block (`ui_loader.lua:915-994`) with:

```lua
    -- Patch draw: widgets only. The background is now a scene producer (screen:drawScene → ctx.sceneRT,
    -- composited + graded by the layer compositor); frosted panels sample the blurred sceneRT. The
    -- per-screen enter/exit transition still wraps the widgets here so bg (in the scene layer) and
    -- widgets (here) scale+fade by the same getTransitionValues.
    function screen:draw()
        if not self.visible then return end

        local useTransition = self:beginTransitionDraw()

        -- Widget layer. When _contentAlpha < 1 (fade_screen_content), render widgets to a canvas and
        -- draw it at that alpha so the UI fades while the background (scene layer) stays.
        local ca = self._contentAlpha or 1
        if ca >= 1 then
            Viewport.beginUI()
            Widget.draw(self)
            Viewport.endUI()
        elseif ca > 0 then
            local cw, ch = love.graphics.getDimensions()
            local Settings = require "services.Settings"
            local gfxGen = Settings.gfxGeneration()
            if not self._contentCanvas
               or self._contentCanvas:getWidth() ~= cw or self._contentCanvas:getHeight() ~= ch
               or self._contentGfxGen ~= gfxGen then
                self._contentCanvas = Settings.newCanvas(cw, ch)
                self._contentGfxGen = gfxGen
            end
            local prev = love.graphics.getCanvas()
            love.graphics.setCanvas(self._contentCanvas)
            love.graphics.clear(0, 0, 0, 0)
            Viewport.beginUI()
            Widget.draw(self)
            Viewport.endUI()
            if prev then love.graphics.setCanvas(prev) else love.graphics.setCanvas() end
            love.graphics.setColor(1, 1, 1, ca)
            love.graphics.draw(self._contentCanvas)
            love.graphics.setColor(1, 1, 1, 1)
        end

        if useTransition then
            self:endTransitionDraw()
        end
    end
```

- [ ] **Step 4: Verify (Visual+F9 — flag for user; full parity checklist)** — launch `run.bat`:
  - **Every menu** (screens 0/1/2 + party/inventory/dailies/settings/pause/login) renders identically: backgrounds, frosted panels, `glass_lattice` refraction, the grade (vignette/grain/bloom), widget text.
  - **Transitions:** push/pop enter & exit pop+fade; **screen-to-screen cross-transition** (exiting + entering both with backgrounds) cross-fades as before.
  - **Pull reveal** (`screen_3`/Stellar Warp): visually identical, but F9 `scene` scope shows the `pull_tunnel` bg cost ~1×, not 2–3×.
  - **Modals** (e.g. character/weapon select) draw on top; base screen's bg stays.
  - F9 across None/FXAA/MSAA + render-scale + resize — all stable; scopes `scene`/`ui`/`present`/`hud` present.

- [ ] **Step 5: Commit**

```bash
git add GachaClient/main.lua GachaClient/ui/ui_loader.lua
git commit -m "feat(render): screens are scene producers — bg via sceneRT, frosted samples it (reveal 2-3x->1x)"
```

---

### Task 8: Retire the capture/grade roles — `PostProcessEffect` trim + `FrostedGlass.prepare` removal

**Files:**
- Modify: `GachaClient/ui/components/PostProcessEffect.lua` — drop capture/grade; keep settings + warp-stress tick
- Modify: `GachaClient/ui/components/FrostedGlass.lua` — remove the now-unused `prepare` (+ `sceneC`)

Cleanup; no behavior change. **Visual:** menus identical. Guard with greps first.

- [ ] **Step 1: Confirm no remaining capture callers**

Run: `Grep pattern=":beginCapture|:endCapture|_overlay:draw|FrostedGlass.prepare" glob="*.lua"`
Expected: no matches (Task 3 removed PixelateEffect; Task 7 removed the ui_loader `_overlay` capture + `FrostedGlass.prepare` calls). If any remain, stop and resolve before trimming.

- [ ] **Step 2: Trim `PostProcessEffect.lua`** — replace the file body so it is a grade-settings carrier (keep `new`, `DEFAULTS`, `_baseChromatic`, and the warp-stress `update`; remove `beginCapture`/`endCapture`/`draw` and the `_canvas`/`_gfxGen`/`globalTime`/`lastFrame` capture state). Result:

```lua
-- ui/components/PostProcessEffect.lua
-- Grade-settings carrier for declarative screens (def.overlay -> _overlay). It holds the UI-flavoured
-- post-process defaults and the warp-stress chromatic tick; the actual grade is applied by the one
-- shared PostFx pass (composeScene / produceUI) which reads these fields. (Capture/grade rendering was
-- retired in rendering Phase 2c-2 — there is no second capture pass.)

local Settings = require "services.Settings"   -- kept for parity with sibling carriers; PostFx owns the toggle

local PostProcessEffect = {}
PostProcessEffect.__index = PostProcessEffect

-- Default values mirror sensible UI usage (not combat usage).
local DEFAULTS = {
    vignette_intensity  = 0.25,
    vignette_softness   = 0.5,
    chromatic_intensity = 0.0,
    chromatic_pulse     = 0.0,
    saturation          = 1.0,
    contrast            = 1.0,
    brightness          = 0.0,
    color_tint          = {1, 1, 1},
    shadow_tint         = {1, 1, 1},
    midtone_tint        = {1, 1, 1},
    highlight_tint      = {1, 1, 1},
    smh_intensity       = 0.0,
    health_desaturation = 0.0,
    grain_intensity     = 0.03,
    dither_intensity    = 0.0,
    bloom_intensity     = 0.0,
    bloom_threshold     = 0.6,
}

function PostProcessEffect.new(opts)
    opts = opts or {}
    local self = setmetatable({}, PostProcessEffect)
    for k, v in pairs(DEFAULTS) do self[k] = opts[k] ~= nil and opts[k] or v end
    self._baseChromatic = self.chromatic_intensity   -- warp strain adds on top of this
    return self
end

function PostProcessEffect:update(dt)
    -- Edges split under the warp-launch strain, then settle.
    local ok, SB = pcall(require, "ui.components.SpaceBackground")
    if ok and SB and SB.getWarpStress then
        self.chromatic_intensity = (self._baseChromatic or 0) + SB.getWarpStress() * 0.6
    end
end

return PostProcessEffect
```

(If LÖVE warns `Settings` is unused, drop the `local Settings = require ...` line — it is not referenced.)

- [ ] **Step 3: Remove `FrostedGlass.prepare`** — in `GachaClient/ui/components/FrostedGlass.lua`, delete the `function FrostedGlass.prepare(drawBg, W, H) ... end` block and the now-unused `sceneC` (from the `local sceneC, tmpC, outC` declaration and the `sceneC = love.graphics.newCanvas(W, H)` line in `ensureCanvases`). Keep `fromScene`, `canvas`, `sceneCanvas`, `ensureCanvases` (allocating `tmpC`/`outC`), `loadShader`, `STEP`. Update the file's top comment to describe `fromScene` (blurs the scene RT), not `prepare`.

- [ ] **Step 4: Verify**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: PASS (unaffected).
**Visual (flag for user):** launch `run.bat`; menus + the pull reveal look identical; the warp-launch chromatic strain on the reveal still pulses (PostProcessEffect:update still ticks via screen:update).

- [ ] **Step 5: Commit**

```bash
git add GachaClient/ui/components/PostProcessEffect.lua GachaClient/ui/components/FrostedGlass.lua
git commit -m "refactor(render): retire capture/grade roles (PostProcessEffect carrier; drop FrostedGlass.prepare)"
```

---

## Self-Review

**Spec coverage:**
- §1 composeScene primitive → Task 5. §2 producer selection (`UI.sceneScreens`, `resolveFrame`) → Tasks 2, 7. §3 `composeScreensScene` (scratch model) → Task 7. §4 `frosted` effect (`fromScene` + `SceneEffects`) → Task 4. §5 ui-layer grade variant → Task 5 (added) + Task 7 (activated). §5b overlay homogenization (pixelate fold + PixelateEffect retire) → Task 3. §6 auto-derive `drawScene`/`renderProfile`/`gradeSettings` + simplified `screen:draw` → Tasks 1, 6, 7. §7 PostProcessEffect capture-role retirement → Task 8. All spec sections map to a task.

**Type/name consistency:** `composeScene(ctx, producer, layer)`, `worldProducer(ctx)`, `composeScreensScene(ctx)`, `resolveFrame(ctx)`, `produceLayer(layer, ctx)`, `produceUI(ctx, layer)`, `produceHud(ctx, layer)` — consistent across Tasks 5/7. `Profile.forScreenDef(def)`, `UI.sceneScreens()`, `ScreenStack:sceneScreens()`, `FrostedGlass.fromScene(src)`, `SceneEffects.frosted(ctx)`, `screen:drawScene(ctx)`, `screen.renderProfile`, `screen.gradeSettings`, `s.pixelate_block`/`pixelate_size` — used identically where referenced.

**Ordering / parity:** Tasks 1–6 are additive (no behavior change — verified by "menus/overworld unchanged"). Task 7 is the single atomic flip. Task 8 is dead-code cleanup behind greps. `composeScene` runs effects on the sharp `ctx.sceneRT` before grading a copy, so `frosted` matches today. `resolveFrame` falls back to PLAIN_UI/WORLD for opt-outs and guards the world layer; `composeScreensScene` skips screens without `drawScene`.

**No placeholders:** every code step contains complete code; every run step has an exact command + expected result. Visual/F9 gates are explicitly flagged as human-verified (not claimable by an agent).

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-27-rendering-p2c2-screen-producers.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task + two-stage review (spec, then quality); visual/F9 tasks (3, 5, 6, 7, 8) pause for your in-game confirmation.
2. **Inline Execution** — execute here with checkpoints.

Which approach?
