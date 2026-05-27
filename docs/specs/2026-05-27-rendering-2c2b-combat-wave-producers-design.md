# Rendering Phase 2c-2b — Combat & WaveGame Scene Producers (Design)

**Date:** 2026-05-27
**Parent specs:** `2026-05-27-rendering-2c-design.md` (layer compositor), `2026-05-27-rendering-2c2-screen-producers-design.md` (screen producers + the hand-coded opt-in contract)
**Status:** design, pending user review → writing-plans

---

## Goal

Migrate the two hand-coded gameplay screens — **CombatScreen** (`ui/screens/combat/`) and **WaveGame**
(`ui/screens/dailies/WaveGame.lua`) — onto the unified `composeScene` pipeline as scene producers,
reusing the 2c-2 hand-coded opt-in contract. This removes their bespoke present/frameTarget handling
and folds their scenes into the shared `ctx.sceneRT` + layered compositor, so the cross-cutting passes
(grade, tonemap, upscale, AA, frosted) become uniform and 2c-3's linear/HDR flip reaches combat/wave
for free. Parity-gated: **no look change**.

## Locked decision (effects folding)

**Producer-internal effects (user, 2026-05-27).** Each screen's signature post effects stay *inside*
its producer: WaveGame's tuned 2-pass bloom + near-miss lens vignette, and Combat's additive
holographic glow, are rendered into `ctx.sceneRT` by `drawScene`. The compositor owns only the
cross-cutting passes. This is the **same model as the WORLD producer** (`RenderSystem` does rich
internal rendering into `sceneRT`); a producer is free to render its scene however it likes as long as
it (a) outputs to `ctx.sceneRT`, (b) stops doing its own grade/upscale/present, (c) leaves AA/grade/
tonemap/frosted to the compositor. The homogenization mandate ([[feedback-homogenized-rendering]]) is
about the cross-cutting pipeline, not about forbidding rich producer internals.

---

## Current state

**CombatScreen** (`init.lua:109` `CombatScreen:draw`, extends `Screen`): bg fill (dark rect) →
`camera:attach` + `renderer:draw` (grid/units/floatnums) + `camera:detach` → `renderer:drawBloom()`
(screen-space additive glow, `bloomA`/`bloomB`) → `hud:draw` → defensive `setCanvas()/setShader()/
setColor()` reset. Draws to the active canvas. No grade, no render-scale.

**WaveGame** (`:836` `WaveGame:draw`): reads `frameTarget = getCanvas()` (already compositor-aware);
(1) scene → `_canvasScene` (fiber-bg shader + tunnel + trail + wave head, under a `displayScale`
transform); (2) tuned bloom → `_canvasGlow`/`_canvasBlur` (brightpass + H/V blur at step 1 & 2, gated
on `Settings.bloom()`); (3) composite scene + additive glow → `_canvasComp`; (4) near-miss lens
vignette (`_lensShader`) → `frameTarget`; (5) screen-space overlays: particles, flash, HUD.

Both are stack screens. Today (post-2c-2) they have no `renderProfile`/`drawScene`, so `resolveFrame`
falls them back to `PLAIN_UI` — they render self-contained in the ui layer. That works (parity) but
keeps two bespoke present chains outside the unified pipeline.

---

## Architecture

Each screen adopts the **hand-coded opt-in contract** (from 2c-2 §6): `drawScene(ctx)`,
`renderProfile`, `gradeSettings`. No compositor changes — `resolveFrame`/`composeScreensScene` already
drive any screen that has the contract.

- **`screen:drawScene(ctx)`** — renders the SCENE (world + signature internal effects + composite)
  into the current target. `composeScreensScene` has already set the target to `ctx.sceneRT` (or a
  per-screen scratch canvas during a transition) and applied the transition transform, so `drawScene`
  just draws; it must NOT call `setCanvas` to the screen or read `frameTarget`.
- **`screen.renderProfile`** — `MENU`-shaped: `scene(source="background", aa=true, effects={})` +
  `ui` + `hud`. **No `frosted`** (neither uses frosted panels). **`grade=false`** on scene + ui
  (parity: neither grades today).
- **`screen.gradeSettings`** — `nil` (no PostFx grade today; their look is producer-internal).
  `composeScene` therefore skips the grade and just presents `sceneRT`.
- **`sceneScaled = false`** — present 1:1 (parity). Render-scale adoption (internal-res rendering) is a
  deliberate **future** follow-on, not 2c-2b.
- **`screen:draw`** — reduced to the **screen-space ui layer**: HUD + overlays only (the parts that
  must stay sharp on top, after the scene). For Combat: `hud:draw`. For WaveGame: particles, flash,
  HUD (step 5). The bespoke `frameTarget`/present/`setCanvas()` handling is removed (the compositor
  owns present).

### Combat migration

- `drawScene(ctx)` = bg fill + `camera:attach` + `renderer:draw` + `camera:detach` +
  `renderer:drawBloom()`. (Bloom stays producer-internal — additive glow over the grid/units, into
  `sceneRT`.)
- `draw()` = `hud:draw(sw, sh)` only. Keep a defensive `setShader()/setColor()` reset; drop the
  `setCanvas()` reset (the compositor owns the canvas now).
- `renderProfile` / `gradeSettings=nil` as above.

### WaveGame migration

- `drawScene(ctx)` = steps 1–4 (scene canvas + tuned bloom + composite + lens vignette), rendered into
  `ctx.sceneRT` instead of `frameTarget`. The internal `_canvasScene`/`_canvasGlow`/`_canvasBlur`/
  `_canvasComp` chain stays (producer-internal); the final lens-vignette draw targets the current
  canvas (`ctx.sceneRT`) rather than a fetched `frameTarget`.
- `draw()` = step 5 (particles, flash, HUD) only.
- `renderProfile` / `gradeSettings=nil` / `sceneScaled=false` as above.

---

## Scope

**In 2c-2b:** the two producer migrations (Combat first — simpler; WaveGame second). Reuses the
existing `composeScene`/`composeScreensScene`/`resolveFrame` — **no changes to the compositor or
`main.lua`** beyond what 2c-2 built.

**Off-limits:** Combat **abilities** gameplay (kits/data/power/execution) — rendering plumbing only
(`CombatRenderer`/`HUD`/`camera` are fair game per [[feedback-abilities-system-ownership]]).

**Deferred (future, not 2c-2b):** render-scale adoption (`sceneScaled=true` + internal-res) for
combat/wave; any PostFx grade on these scenes; 2c-3's linear/HDR + ACES flip (which will then reach
them via the now-unified path).

---

## Verification (parity gates)

GL/visual + F9 by the user; no headless coverage (these are GL screen producers).

1. **Combat** (launch a test encounter): grid, units, hover hologram, float numbers, the additive
   holographic bloom, and HUD all render identically; enter/exit transition (scale+fade) intact.
2. **WaveGame** (dailies minigame): fiber background, tunnel/trail/wave, the tuned 2-pass bloom, the
   near-miss lens vignette, particles/flash, and HUD all render identically; `displayScale` framing
   unchanged.
3. Both across None/FXAA/MSAA + window resize. F9 shows `compose` → `scene`/`ui`/`hud` scopes with the
   combat/wave scene under the `scene` scope.
4. The overworld + declarative menus (2c-2) are unaffected (no compositor changes).

## Risks / notes

- **Transition transform**: `composeScreensScene` wraps `drawScene` in the screen's
  `getTransitionValues` scale+alpha. Combat/Wave use the base `Screen` transition; the HUD/overlays in
  `:draw` keep whatever transition behavior they have today (none observed → parity).
- **WaveGame canvas sizing**: its internal canvases are sized to the window (`_initGfx(sw, sh)`); with
  `sceneScaled=false` the sceneRT is also window-sized, so the final composite maps 1:1. No change.
- **Combat defensive reset**: dropping `setCanvas()` from `CombatScreen:draw` is safe — the compositor
  sets/owns the canvas per layer; leaving `setShader()/setColor()` resets is harmless hygiene.
- **Dirty working tree**: targeted `git add` per task; never `git add -A`.
