# Rendering Phase 2c-1b — Overworld onto the Scene-Target Compositor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate the **overworld** off `RenderSystem`'s self-owned `PostFx`+`RenderScale` present and onto a shared pooled `sceneRT` graded + upscaled by `main.lua`'s scene step — and delete the dead portal-distortion code — keeping the frame pixel-identical.

**Architecture:** `RenderSystem:draw` renders the world into a pooled `sceneRT` (from `ctx.targets`) instead of presenting it itself; it publishes `ctx.sceneRT` + its grade settings via a tiny synchronous `FrameCtx` holder (because the ECS reaches `:draw` through `state.world:update("draw")` with no `ctx`). `main.lua`'s scene step (`composeWorld`) then grades (`PostFx`) + upscales (`RenderScale`) `ctx.sceneRT` into the existing 2b AA canvas, before `UI.draw`/present/hud — preserving the 2b pass order. **Scope:** overworld only. Screens keep the unchanged 2b path (their backgrounds/`PostProcessEffect`/`FrostedGlass` migrate in 2c-2). The `Profile`/`composePlan` layer system (Plan 2c-1′) stays foundation, consumed by the full compositor wiring in Plan 2c-1c.

**Tech Stack:** LÖVE 11.x / LuaJIT. `FrameCtx` is headless-unit-tested (`ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`). The portal removal and the overworld migration are GL — **verified by boot + visual + F9 parity (the human's gate), not headless** (LÖVE GL isn't headless-testable), exactly like the 2b migrations.

**Spec:** `docs/superpowers/specs/2026-05-27-rendering-2c-design.md`.

**Study basis (in memory `project_aaa_rendering.md`):** `RenderSystem:draw` (1181–2008) — entry (1181–1212) computes render-scale + camera and acquires `ppCanvas` only when `needScene`; body (1213–~1978) renders into the active canvas (scratch `mapCanvas`/`abilityCanvas` restore to `ppCanvas` at lines 1256/1312); exit (1979–2007) grades+upscales into `frameTarget` + decays chromatic pulse. Portal block (1402–1465) is dead (PortalSystem stub).

---

## File structure

| File | Responsibility | Change |
|---|---|---|
| `GachaClient/systems/render/FrameCtx.lua` | Single-slot current-frame ctx holder (ECS→producer bridge) | Create |
| `GachaClient/systems/RenderSystem.lua` | Delete dead portal pass; `:draw` renders into pooled `sceneRT` + publishes `ctx`, drops its present block | Modify |
| `GachaClient/ui/components/PortalDistortEffect.lua` | Retired portal-distortion overlay | Delete |
| `GachaClient/ui/shader_registry.lua` | Drop the `portal_distort` overlay entry + its require | Modify |
| `GachaClient/main.lua` | `composeWorld` grades+upscales `ctx.sceneRT` into the AA canvas; wired into the scene step | Modify |
| `GachaClient/tests/render_harness/main.lua` | `== FrameCtx ==` unit section | Modify |

Headless run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`. Game: `GachaClient/run.bat`, F9 for the profiler.

---

### Task 1: `FrameCtx.lua` — the ECS→producer ctx bridge

**Files:** Create `GachaClient/systems/render/FrameCtx.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line of `GachaClient/tests/render_harness/main.lua`:
```lua
print("== FrameCtx ==")
do
    local FrameCtx = require "systems.render.FrameCtx"
    eq(FrameCtx.get(), nil, "starts nil")
    local c = { x = 1 }
    FrameCtx.set(c)
    eq(FrameCtx.get(), c, "get returns the set ctx")
    FrameCtx.set(nil)
    eq(FrameCtx.get(), nil, "cleared after set(nil)")
end
```

- [ ] **Step 2: Run → FAIL** (`module 'systems.render.FrameCtx' not found`).
Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`

- [ ] **Step 3: Create `GachaClient/systems/render/FrameCtx.lua`:**
```lua
-- systems/render/FrameCtx.lua
-- A tiny single-slot holder for the current frame's render ctx. RenderSystem:draw is reached
-- indirectly through the ECS (state.world:update(0,nil,"draw")) and has no ctx parameter; the
-- compositor sets the ctx here immediately before triggering the world draw and clears it right
-- after. Strictly synchronous, same-frame set-then-read -- not a persistent global.
local FrameCtx = {}
local current = nil
function FrameCtx.set(ctx) current = ctx end
function FrameCtx.get() return current end
return FrameCtx
```

- [ ] **Step 4: Run → PASS** (`ALL RENDER HARNESS CHECKS PASSED`, exit 0).

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/FrameCtx.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): FrameCtx single-slot ctx bridge for ECS-invoked producers"
```

---

### Task 2: Delete the dead in-world portal distortion from `RenderSystem`

Invisible removal — `PortalSystem` is a stub (`entities = {}`), so the pass never runs. **Verification is visual** (game looks identical), not headless.

**Files:** Modify `GachaClient/systems/RenderSystem.lua`.

- [ ] **Step 1: Read `GachaClient/systems/RenderSystem.lua`** to locate the four pieces below.

- [ ] **Step 2: Delete the portal-distortion pass block.** Remove the entire block that begins with the comment `-- Portal gravitational distortion pass` (the `local distortShader = getPortalDistortShader()` line) through the `end` that closes its `if distortShader and PortalSystem.entities and #PortalSystem.entities > 0 then ... end` (currently ~lines 1402–1465). After removal, `state.camera:attach()` (the line immediately before the removed block) must be immediately followed by the comment `-- Draw portal visual effects (within camera context)` and `PortalSystem:drawWorld()`. (The removed block's internal `camera:detach()`/`camera:attach()` were self-balancing, so the outer `attach()`…`detach()` around `PortalSystem:drawWorld()` stays balanced.)

- [ ] **Step 3: Delete the three portal helper functions** — `getPortalDistortShader` (the `-- Load portal distortion shader on first use` function, ~198–203), `getPortalDistortCanvas` (~213–219), and `getPortalCaptureCanvas` (~221–227).

- [ ] **Step 4: Delete the four module locals** (the `-- Portal distortion shader (gravitational lensing effect)` group, ~44–47):
```lua
local portalDistortShader = nil
local portalDistortShaderError = false
local portalDistortCanvas = nil
local portalCaptureCanvas = nil
```
(Leave `local PortalSystem = require "systems.PortalSystem"` at the top — `PortalSystem:drawWorld()` (no-op stub) is still called.)

- [ ] **Step 5: Confirm clean.** Grep `GachaClient/systems/RenderSystem.lua` for `portalDistort`, `portalCapture`, `getPortalDistort`, `getPortalCapture` → expect 0 matches. Grep for `distortShader` → 0 matches.

- [ ] **Step 6: Verify in-game (visual parity).** Run `GachaClient/run.bat`. The overworld (and everything) looks **identical** — no crash, no visual change (the distortion never ran). If you can't launch, say so; do not claim parity you didn't observe.

- [ ] **Step 7: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/RenderSystem.lua
git -C D:/dev/starworks/Gacha commit -m "refactor(render): delete dead in-world portal distortion (PortalSystem is a stub)"
```

---

### Task 3: Retire `PortalDistortEffect.lua` + its registry entry

`PortalDistortEffect` is referenced only via `shader_registry`'s `portal_distort` overlay factory. Confirm no screen uses it, then delete it.

**Files:** Delete `GachaClient/ui/components/PortalDistortEffect.lua`; Modify `GachaClient/ui/shader_registry.lua`.

- [ ] **Step 1: Confirm no screen declares the `portal_distort` overlay.** Grep the screen data for usage: search `GachaClient/data/ui_screens` and `GachaClient/data` for `portal_distort`. Expected: no screen JSON references it (it's vestigial). **If any screen DOES reference `portal_distort` as an overlay, STOP and report it** — removing the registry entry would break that screen (it would need its overlay removed too, which is out of this task's scope).

- [ ] **Step 2: Remove the registry entry** in `GachaClient/ui/shader_registry.lua`. Delete the require (line ~20):
```lua
local PortalDistortEffect = require "ui.components.PortalDistortEffect"
```
and the overlay factory entry (lines ~78–82):
```lua
    -- Portal gravitational distortion (portal_distort.glsl)
    -- opts: { portals={{x,y,radius},...}, distortion_strength=0.05 }
    portal_distort = function(opts)
        return PortalDistortEffect.new(opts)
    end,
```
Leave the `pixelate` and `post_process` overlay entries intact.

- [ ] **Step 3: Delete the file** `GachaClient/ui/components/PortalDistortEffect.lua`:
```bash
git -C D:/dev/starworks/Gacha rm GachaClient/ui/components/PortalDistortEffect.lua
```

- [ ] **Step 4: Confirm clean.** Grep the whole `GachaClient` tree for `PortalDistortEffect` → expect 0 matches (the `assets_harness` test file lists it among component files; if it appears there, remove that one list entry too and note it). Run the render harness to confirm nothing shared broke: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness` → `ALL RENDER HARNESS CHECKS PASSED`.

- [ ] **Step 5: Verify in-game (visual parity).** Run `GachaClient/run.bat`: identical, no crash, no missing-shader log spam.

- [ ] **Step 6: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/ui/shader_registry.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "refactor(render): retire PortalDistortEffect + its shader_registry entry"
```

---

### Task 4: Migrate the overworld onto `ctx.sceneRT` (atomic, parity-gated)

`RenderSystem` stops presenting (drops `PostFx`+`RenderScale`); `main.lua`'s `composeWorld` does the grade+upscale into the AA canvas. These MUST land together — `RenderSystem` alone would render the world to an off-screen `sceneRT` nothing presents (black overworld). Output-identical; **visual + F9 parity is the human's gate.**

**Files:** Modify `GachaClient/systems/RenderSystem.lua`; Modify `GachaClient/main.lua`.

- [ ] **Step 1: RenderSystem entry → acquire a pooled `sceneRT` (named `ppCanvas` so the body is untouched).** In `RenderSystem:draw`, replace the entry from `local frameTarget = love.graphics.getCanvas()` through the `if needScene then ... end` block (currently ~1189–1212):
```lua
    local frameTarget = love.graphics.getCanvas()

    local renderW, renderH = Settings.internalSize(screenW, screenH)
    local scaled = (renderW ~= screenW) or (renderH ~= screenH)
    -- ... camera setup ...
    local usePost   = Settings.postFx() and postProcessSettings.enabled ~= false
    local needScene = usePost or scaled
    local ppCanvas  = nil
    if needScene then
        ppCanvas = getPostProcessCanvas(renderW, renderH)
        love.graphics.setCanvas(ppCanvas)
        love.graphics.clear(0, 0, 0, 1)
    end
```
with (keep the camera-setup lines between exactly as they are; only the `frameTarget` line and the `usePost`/`needScene`/`ppCanvas` block change):
```lua
    local FrameCtx = require "systems.render.FrameCtx"
    local ctx = FrameCtx.get()

    local renderW, renderH = Settings.internalSize(screenW, screenH)
    local scaled = (renderW ~= screenW) or (renderH ~= screenH)
    -- ... camera setup (unchanged) ...
    -- Render the world into the pooled scene target (always; the compositor grades + upscales it).
    -- Named ppCanvas so the rest of the body (which targets ppCanvas) is unchanged.
    local ppCanvas = ctx.targets:acquire(renderW, renderH)
    love.graphics.setCanvas(ppCanvas)
    love.graphics.clear(0, 0, 0, 1)
```
(The camera-setup lines — `state.camera.renderScale = renderW / screenW` through `local renderZoom = state.camera:renderZoom()` — stay verbatim between `scaled` and the `ppCanvas` acquire.)

- [ ] **Step 2: Confirm `frameTarget`/`usePost`/`needScene` are now unused in the body.** Grep `GachaClient/systems/RenderSystem.lua` for `frameTarget`, `needScene` → expect the only remaining matches to be in the exit block you replace in Step 3 (none in the body 1213–1978). Grep `usePost` → only in the exit block. (If any appears in the body, STOP and report — the migration assumed they don't.)

- [ ] **Step 3: RenderSystem exit → publish to ctx instead of presenting.** Replace the present block (currently ~1979–2007), which is:
```lua
    -- Present: grade the captured scene and/or upscale it to the window.
    --   scaled + post : grade at internal res, then upscale (bilinear/FSR) to the window.
    --   scaled only   : upscale the raw scene to the window.
    --   native + post : grade straight to the screen at 1:1 (unchanged default path).
    if ppCanvas then
        love.graphics.setCanvas(frameTarget)
        if scaled then
            if usePost then
                local g = getGradeCanvas(renderW, renderH)
                love.graphics.setCanvas(g)
                love.graphics.clear(0, 0, 0, 1)
                PostFx.apply(ppCanvas, postProcessSettings, love.timer.getTime())  -- grade → g
                love.graphics.setCanvas(frameTarget)
                RenderScale.upscale(g)
            else
                RenderScale.upscale(ppCanvas)
            end
        elseif usePost then
            PostFx.apply(ppCanvas, postProcessSettings, love.timer.getTime())
        end

        -- Decay chromatic pulse over time
        if usePost and postProcessSettings.chromatic_pulse > 0 then
            postProcessSettings.chromatic_pulse = postProcessSettings.chromatic_pulse * 0.85
            if postProcessSettings.chromatic_pulse < 0.01 then
                postProcessSettings.chromatic_pulse = 0
            end
        end
    end
end
```
with:
```lua
    -- Publish the scene target + its grade settings + scale flag; the compositor (main.lua
    -- composeWorld) grades (PostFx) and upscales (RenderScale) it into the frame's AA canvas.
    ctx.sceneRT       = ppCanvas
    ctx.sceneSettings = postProcessSettings
    ctx.sceneScaled   = scaled

    -- Decay chromatic pulse over time (was inside the old present block).
    if Settings.postFx() and postProcessSettings.enabled ~= false and postProcessSettings.chromatic_pulse > 0 then
        postProcessSettings.chromatic_pulse = postProcessSettings.chromatic_pulse * 0.85
        if postProcessSettings.chromatic_pulse < 0.01 then
            postProcessSettings.chromatic_pulse = 0
        end
    end
end
```
(`getPostProcessCanvas`/`getGradeCanvas` and the `postProcessCanvas`/`gradeCanvas` locals are now unused; leave them — removing them is optional cleanup and not needed for parity.)

- [ ] **Step 4: `main.lua` — add `composeWorld`.** Immediately BEFORE `local function drawScene(ctx)` in `GachaClient/main.lua`, add:
```lua
-- Produce the overworld scene into ctx.sceneRT (RenderSystem, via the ECS), then grade + upscale
-- it into the current AA canvas (ctx.frame; or the screen when aa=="none") -- the post/upscale that
-- RenderSystem used to do internally. Only called for the visible overworld.
local function composeWorld(ctx)
    local FrameCtx = require "systems.render.FrameCtx"
    FrameCtx.set(ctx)
    state.world:update(0, nil, "draw")   -- RenderSystem:draw fills ctx.sceneRT (+ sceneSettings, sceneScaled)
    FrameCtx.set(nil)

    local src = ctx.sceneRT
    if not src then return end

    local Settings   = require "services.Settings"
    local PostFx     = require "ui.components.PostFx"
    local RenderScale = require "ui.components.RenderScale"

    -- Grade at internal res (matches RenderSystem's old grade→gradeCanvas step).
    if ctx.sceneSettings and Settings.postFx() and ctx.sceneSettings.enabled ~= false then
        local graded = ctx.targets:acquire(src:getWidth(), src:getHeight())
        love.graphics.setCanvas(graded)
        love.graphics.clear(0, 0, 0, 1)
        PostFx.apply(src, ctx.sceneSettings, love.timer.getTime())
        src = graded
    end

    -- Present into the AA canvas (ctx.frame) or the screen.
    if ctx.frame then love.graphics.setCanvas(ctx.frame) else love.graphics.setCanvas() end
    love.graphics.setColor(1, 1, 1, 1)
    if ctx.sceneScaled and RenderScale.active() then
        RenderScale.upscale(src)         -- internal → window
    else
        love.graphics.draw(src, 0, 0)    -- 1:1
    end
end
```

- [ ] **Step 5: `main.lua` — call `composeWorld` from the scene step.** In `drawScene`, replace the world block (currently):
```lua
    -- Only draw the overworld when it is actually visible (no screen covering it).
    if state.gameState == GAME_STATE.PLAYING and state.world and not UI.hasScreens() then
        state.world:update(0, nil, "draw")
    end
```
with:
```lua
    -- Only draw the overworld when it is actually visible (no screen covering it). The world now
    -- renders into ctx.sceneRT and composeWorld grades + upscales it into the AA canvas.
    if state.gameState == GAME_STATE.PLAYING and state.world and not UI.hasScreens() then
        composeWorld(ctx)
    end
```
Leave the rest of `drawScene` (FrameAA begin, screen-shake/`UI.draw`, `UI.drawTransition`), `drawPresent`, `drawHud`, and `love.draw` unchanged. (Screens still hit the unchanged path; `UI.draw` renders the stack with its own bg/post/frosted as before.)

- [ ] **Step 6: Verify in-game (visual + F9 parity), all AA modes.** Run `GachaClient/run.bat`. Confirm the **overworld renders identically** — geometry, the void/map/entities, the post grade (vignette/chromatic/etc.), and render-scale upscaling all match the prior build. Toggle Settings → Video → Anti-aliasing **None / FXAA / MSAA**; toggle render-scale below 100%; toggle Post-Processing off/on; **resize the window** — each identical, no corruption, no crash on the first overworld frame (first real `sceneRT` through the pool). Confirm a screen/menu (which still uses the 2b path) is unaffected. Press **F9**: scene/present/hud scopes present; `vram` stable frame-over-frame (sceneRT + graded canvas reused via the pool, not leaked). If you can't launch, say so — do not claim parity you didn't observe.

- [ ] **Step 7: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/RenderSystem.lua GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): overworld renders into pooled sceneRT; main composites grade+upscale"
```

---

## Self-review (completed)

- **Spec coverage (2c-1b scope):** portal dead-code removal → T2 (RenderSystem) + T3 (PortalDistortEffect/registry); `sceneRT` infra + producer/ctx bridge → T1 (`FrameCtx`) + T4 (RenderSystem acquires from `ctx.targets`, publishes `ctx.sceneRT`); the grade+upscale moved out of RenderSystem into the compositor step → T4 (`composeWorld`); overworld-only staging with screens on the unchanged 2b path → T4 (gated by the existing `not UI.hasScreens()` branch). Deferred per spec: screen producers + `frosted` (2c-2); the full `Profile.composePlan`-driven layered compositor + per-layer AA grouping (2c-1c); linear/HDR+ACES (2c-3).
- **Placeholders:** none — T1 is exact + headless; T2/T3 quote exact removal boundaries (and add STOP-conditions where a grep must confirm no consumer); T4 gives the exact entry/exit replacements and the full `composeWorld` + scene-step edit. The unread RenderSystem body (1213–1978) is genuinely untouched: the migration keeps the variable named `ppCanvas`, so the body's `setCanvas(ppCanvas)` restores (1256/1312) and all world drawing are unchanged — verified by the Step-2 grep gate that `frameTarget`/`needScene`/`usePost` don't appear in the body.
- **Parity discipline:** T2/T3 invisible (dead/unused code). T4 reproduces RenderSystem's exact present semantics (grade at internal res → upscale; or 1:1; chromatic decay preserved) in `composeWorld`, just sourced from `ctx.sceneRT` and targeting the AA canvas instead of `frameTarget`. The always-`sceneRT` round-trip (vs the old post-off+native straight-to-screen) is one extra blit, visually identical, profiler-gated. GL tasks are human-verified (no false headless claims).
- **Type/name consistency:** `FrameCtx.set/get`; `ctx.targets:acquire(w,h)` (from 2b RenderTargets); `ctx.sceneRT`/`ctx.sceneSettings`/`ctx.sceneScaled` published by RenderSystem and read by `composeWorld`; `ctx.frame` (the 2b AA canvas) is the composite target; `Settings.postFx()`/`RenderScale.active()`/`RenderScale.upscale`/`PostFx.apply` match their existing signatures.

## Next: Plan 2c-1c, then 2c-2 / 2c-3

- **2c-1c:** replace `main.lua`'s 2b `scene`/`present`/`hud` passes with the full layered compositor driven by `Profile.selectActive` + `Profile.composePlan` (consuming Plan 2c-1′), now that the overworld producer feeds `sceneRT` — including per-layer `aa` grouping and the `ui`/`hud` layers as first-class. Parity-gated.
- **2c-2:** migrate screen producers (backgrounds + `WaveGame`/`CombatRenderer`-plumbing) onto `sceneRT`; `frosted` becomes a scene-layer `effects` member sampling `sceneRT` (kills the reveal 2–3×); retire `PostProcessEffect`/`FrostedGlass` re-render.
- **2c-3:** the linear/HDR `rgba16f` + ACES tonemap flip + `PostFx` linear re-tune.
