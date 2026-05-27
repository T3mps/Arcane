# Rendering Phase 2c-2 — Screen Scene Producers (Design)

**Date:** 2026-05-27
**Parent spec:** `docs/superpowers/specs/2026-05-27-rendering-2c-design.md` (layer-compositor model)
**Status:** design, pending user review → writing-plans

---

## Goal

Fold declarative UI screens into the unified layer compositor built in 2c-1c. Each screen renders
its background **once** into the shared `ctx.sceneRT` (instead of 2–3× today), the layered
compositor drives it via the `background` scene source, `frosted` becomes an effects-pass operation
sampling `sceneRT`, and `PostProcessEffect`'s separate capture/grade is retired. Parity-gated:
no look change. Linear/HDR + ACES is the later 2c-3.

**Measurable win:** the pull reveal (`screen_3`, heavy `pull_tunnel` background shader) drops from
the background being rendered 2–3× per frame (once in `FrostedGlass.prepare`, once in the
`PostProcessEffect` capture) to **once**.

## Homogenization mandate (the governing constraint)

There must be **one** scene path. The world, declarative screens, and (in the 2c-2b follow-on)
WaveGame + CombatRenderer all produce into `ctx.sceneRT` through the **same** `composeScene(ctx, producer)`
primitive and are driven by the **same** compositor. No bespoke per-renderer `scene → grade → upscale`
chains survive. The hand-coded-screen opt-in is the *same* `drawScene`/`renderProfile` contract the
auto-derive emits — not a parallel approach.

---

## Current state (what exists, what we change)

**Compositor (`main.lua`, post-2c-1c):**
- `composeWorld(ctx)` (main.lua:510) — `FrameCtx.set(ctx)` → `state.world:update(0,nil,"draw")` fills
  `ctx.sceneRT`/`sceneSettings`/`sceneScaled` → grade via `PostFx.apply` → present (upscale if
  `sceneScaled` else 1:1) into `ctx.frame`.
- `produceUI(ctx)` (main.lua:544) — warp screen-shake wrap → `UI.draw()` → `UI.drawTransition()`.
- `produceHud(ctx)` (main.lua:563) — FPS readout + warp flash, sharp.
- `produceLayer(layer, ctx)` (main.lua:584) — dispatch by `layer.source`: `world`→composeWorld
  (guarded by `state.gameState==PLAYING and state.world`), `ui`→produceUI, `hud`→produceHud;
  `background`/`none` are **no-ops today**.
- `composeFrame(ctx)` (main.lua:603) — `Profile.composePlan(Profile.frameProfile(UI.hasScreens()))`;
  aa-groups render layers into a pooled `FrameAA` canvas + one `present`; sharp groups draw straight.

**Profile (`systems/render/Profile.lua`):** `MENU = { scene(source="background", aa, frosted), ui, hud }`
already exists. `selectActive(stackTopDown, worldProfile)` (Profile.lua:55) returns the topmost
non-overlay profile, pure. `frameProfile(hasScreens)` (Profile.lua:89) is the 2c-1c shortcut
(`hasScreens → PLAIN_UI`) we replace.

**Declarative screen draw (`ui_loader.lua` screen:draw, ~915–994):** `beginTransitionDraw` →
`drawBg` (bgColor + `_bgBase` + `_bgShader`) → `FrostedGlass.prepare(drawBg, W, H)` (RE-renders bg) →
`_overlay:beginCapture()` → `drawBg` + `drawWidgets` → `endCapture` → `_overlay:draw()` (the grade) →
`endTransitionDraw`. The `_overlay` is a `PostProcessEffect`.

**Frosted (`ui/components/FrostedGlass.lua`):** `prepare(drawBg, W, H)` renders bg into `sceneC`, blurs
to `outC`. `canvas()` → blurred (`_out`); `sceneCanvas()` → sharp (`_scene`) for glass_lattice.

**PostProcessEffect (`ui/components/PostProcessEffect.lua`):** owns a capture canvas; `beginCapture`
saves the active canvas (so a screen's transition canvas is restored), `draw()` = `PostFx.apply`.

**Screen stack (`ui/core/screenstack.lua`):** `draw()` renders `exitingScreens` then `screens`
bottom→top then `overlays`. Each `screen:draw()` does its own transition (`screen.lua`
`beginTransitionDraw`/`endTransitionDraw`, scale+alpha from `getTransitionValues`). **Two screens
(exiting + entering) can be visible with their own backgrounds during a cross-transition.**

---

## Architecture

### 1. The canonical scene primitive: `composeScene(ctx, producer)`

Generalize `composeWorld` into `composeScene(ctx, producer, layer)` (main.lua). Effects live **here**,
in the one canonical primitive (not in each producer), so the world and screen paths share them:

```
composeScene(ctx, producer, layer):
    producer(ctx)                       -- fills ctx.sceneRT (+ sceneSettings, sceneScaled)
    if not ctx.sceneRT then return end
    for effect in (layer.effects or {}): SCENE_EFFECTS[effect](ctx)   -- §4; samples the SHARP sceneRT, pre-grade
    src = ctx.sceneRT                                                 -- grading writes a COPY; ctx.sceneRT stays sharp
    if ctx.sceneSettings and Settings.postFx() and sceneSettings.enabled ~= false:
        graded = ctx.targets:acquire(src:w, src:h); PostFx.apply(src, sceneSettings) → graded; src = graded
    present src into ctx.frame (RenderScale.upscale if ctx.sceneScaled else 1:1)
```

- **World producer** = the existing `FrameCtx.set(ctx)` → ECS draw → `FrameCtx.set(nil)` block.
  `composeWorld(ctx)` becomes `composeScene(ctx, worldProducer, layer)` (WORLD's scene layer has
  `effects={}`, so the effect loop is a no-op). No behavior change.
- **Screen producer** = `composeScreensScene(ctx)` (below). `produceLayer`'s `background` branch calls
  `composeScene(ctx, ctx.sceneProducer, layer)` (MENU's scene layer has `effects={"frosted"}`).

### 2. Frame setup: profile + scene producer selection (`main.lua`)

Replace `Profile.frameProfile(UI.hasScreens())` in `composeFrame` with a per-frame resolve that reads
the screen stack **once**:

```
resolveFrame():
    if not UI.hasScreens(): return Profile.WORLD, worldProducer (gated as today)
    stack = UI.sceneScreens()                      -- visible non-overlay screens, TOP-FIRST
    profile = Profile.selectActive(map(stack, .renderProfile), Profile.WORLD)
    producer = composeScreensScene                  -- iterates the visible screens itself
    return profile, producer
```

- `UI.sceneScreens()` (new, `ui/init.lua`) returns visible non-overlay screens **top-first** for
  profile selection; `composeScreensScene` re-reads them **bottom→top** for painter order. (Both are
  thin reads over `UI.stack` — `exitingScreens` + `screens`, excluding `overlays`.)
- `selectActive` stays pure (profiles only). The active profile is the topmost non-overlay screen's
  `renderProfile`; if it has no `background` scene layer (a screen with no bg), the profile is
  effectively `PLAIN_UI` and the `background` branch is absent (no scene producer runs).
- `composeFrame` sets `ctx.sceneProducer = producer` before running the plan.

### 3. Screens as a multi-producer scene layer (`composeScreensScene`)

The `background` producer composites every visible non-overlay screen's background into the **one**
`ctx.sceneRT`, in z-order, each applying its own transition — this is what preserves cross-transitions
and stacked backgrounds while staying a single shared sceneRT:

```
composeScreensScene(ctx):
    rt = ctx.targets:acquire(viewportW, viewportH)   -- menus: sceneScaled=false, internal=window
    setCanvas(rt); clear(0,0,0,1)
    for screen in visible non-overlay screens, bottom→top:
        scale, alpha = screen:getTransitionValues()
        push; translate/scale about center; setColor(1,1,1,alpha)
        screen:drawScene(ctx)                        -- bgColor + bgBase + bgShader, NO widgets
        pop; setColor(1,1,1,1)
    ctx.sceneRT      = rt
    ctx.sceneScaled  = false
    ctx.sceneSettings = topScreen.gradeSettings       -- from def.overlay (or nil → no grade)
```

(The `frosted` effect is **not** run here — `composeScene` runs `layer.effects` after the producer
returns, §1/§4. The producer's only job is to fill `ctx.sceneRT`/`sceneSettings`/`sceneScaled`.)

- Single full-screen screen (common) → one iteration. Cross-transition (exiting + entering) → both
  composite with their own scale+alpha = the cross-fade/pop, matching today.
- `ctx.sceneSettings` is the **top** screen's grade settings (the combined sceneRT is graded once).
  Minor accepted deviation vs today's per-screen grade — same family as the option-(b) grade-boundary
  deviation already signed off.

### 4. `frosted` as a scene-layer effect

Add `FrostedGlass.fromScene(srcCanvas)` — blurs an **already-rendered** canvas (the 2-pass blur from
`prepare`, minus the bg re-render) and sets `_out`/`_scene`. `canvas()`/`sceneCanvas()` are unchanged,
so `glass_lattice` and frosted `panel` widgets need **no edits**.

`composeScene` runs the active scene layer's `effects` set through a dispatch table, **after** the
producer fills `ctx.sceneRT` and **before** the grade (§1):

```
SCENE_EFFECTS = { frosted = function(ctx) FrostedGlass.fromScene(ctx.sceneRT) end }
-- in composeScene: for effect in layer.effects: SCENE_EFFECTS[effect](ctx)
```

Order holds: the scene layer (produce sceneRT → run effects → grade → present) precedes the ui layer,
so `FrostedGlass.canvas()` is ready when frosted widgets draw. `fromScene` samples the **sharp**
`ctx.sceneRT` (the grade writes a copy, leaving `ctx.sceneRT` untouched), matching today (`prepare`
blurred the raw bg). Future effects (`pixelate`) add one dispatch entry — no other change.

### 5. Widgets stay in the ui layer; per-layer grade (option b)

`produceUI` draws widgets only. With `MENU` keeping `ui.grade=true` (parity), the ui-layer producer
gains a grade variant driven by `layer.grade`:

```
produceUI(ctx, layer):
    if layer.grade and ctx.sceneSettings and Settings.postFx():
        cap = ctx.targets:acquire(...)
        setCanvas(cap); clear(0,0,0,0); <screen-shake-wrapped> UI.draw(); UI.drawTransition()
        PostFx.apply(cap, ctx.sceneSettings) → ctx.frame
    else:                                  -- WORLD ui.grade=false, and 2c-3 onward
        <screen-shake-wrapped> UI.draw(); UI.drawTransition()   -- straight into ctx.frame (today's path)
```

Widgets keep their **own** per-screen transition inside `screen:draw` (the simplified declarative
draw still calls `beginTransitionDraw`/`endTransitionDraw` around `drawWidgets`). So a transitioning
screen scales+fades its bg (in `composeScreensScene`) and its widgets (in the ui layer) by the same
`getTransitionValues` → one coherent transition, no `present`-level special case needed.

### 6. Simplified declarative `screen:draw` + `drawScene` (auto-derived in `ui_loader`)

`ui_loader` auto-derives two members on every declarative screen:

- **`screen:drawScene(ctx)`** — `clear(bgColor)` → `_bgBase:draw()` → `_bgShader:draw()`. Background
  only. Called by `composeScreensScene` into `ctx.sceneRT`. (No transition transform here — the
  producer applies it around the call so it nests with other screens.)
- **`screen.renderProfile`** — derived from `def`: has `background`/`background_base` →
  `MENU`-shaped (`scene(source="background", aa, effects={"frosted"})` + `ui(grade=true)` + `hud`);
  no bg → `PLAIN_UI`. The `frosted` effect is included for every bg screen (today every declarative
  screen calls `FrostedGlass.prepare` unconditionally — parity).
- **`screen.gradeSettings`** — the `def.overlay` grade settings (or `PostProcessEffect` defaults),
  surfaced for `ctx.sceneSettings`. Replaces the `_overlay` object.

`screen:draw` is reduced to: `beginTransitionDraw` → `drawWidgets` → `endTransitionDraw`. The
`FrostedGlass.prepare` call and the whole `_overlay` capture/grade block are **removed**.

**Hand-coded screens opt in by implementing the same contract** (`drawScene(ctx)` +
`renderProfile` + `gradeSettings`). A hand-coded screen that does *not* implement `drawScene` resolves
to `PLAIN_UI` and renders self-contained in the ui layer (its current behavior) — a temporary,
clearly-bounded state, not a parallel pipeline. Migrating the remaining hand-coded screens to the same
contract is tracked as cleanup so the mandate (§Homogenization) fully lands.

### 7. PostProcessEffect retirement

The declarative path no longer constructs `_overlay`. Its grade is folded into the scene layer
(`ctx.sceneSettings`) + the ui-layer grade (§5). Its transition-canvas save/restore is **obsolete** —
the scene/ui split + per-screen transitions replace it. After removing the declarative consumer, grep
for other `PostProcessEffect` / `:beginCapture` users; if none remain, delete
`ui/components/PostProcessEffect.lua`. If a hand-coded screen still uses it, leave the file and list
that screen as a migration follow-up (do not fork a second grade path).

---

## Scope

**In 2c-2:** `composeScene` generalization; frame-setup producer selection (`UI.sceneScreens`,
`composeScreensScene`); `FrostedGlass.fromScene`; scene-layer `frosted` effect dispatch; ui-layer
`grade` variant; `ui_loader` auto-derive (`drawScene`/`renderProfile`/`gradeSettings`) + simplified
`screen:draw`; `PostProcessEffect` removal from the declarative path.

**Deferred to 2c-2b (same `composeScene`, not bespoke):** WaveGame and CombatRenderer become scene
producers. Combat **abilities** (kits/data/power/execution) are **off-limits** — only `CombatRenderer`
plumbing migrates.

**Deferred to 2c-3:** linear/HDR (`rgba16f`) + ACES tonemap; flip `ui.grade=false` (the UI-sRGB seam).

**Out of scope:** screen-transition *rework* (new transition types/direction). Existing scale+fade
enter/exit transitions are preserved exactly.

---

## Verification (parity gates)

GL/visual + F9 verified by the user (the compositor is GL); pure helpers headless via
`ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`.

1. **Overworld** unchanged across None/FXAA/MSAA + render-scale + resize (composeWorld → composeScene
   is a no-op refactor).
2. **Every menu** visually identical: backgrounds, frosted panels, `glass_lattice`, grade
   (vignette/grain/bloom), widget text.
3. **Transitions**: enter/exit pop+fade on push/pop, and **screen-to-screen cross-transitions**
   (exiting + entering both with backgrounds) animate as today.
4. **Pull reveal** (`screen_3`): visually identical, but the `pull_tunnel` background runs **once** —
   confirmable on the F9 `scene` scope (the reveal no longer triples bg cost).
5. **Modals/overlays** (OVERLAY profile) still draw on top; base screen's bg is the scene.
6. F9 still shows `scene` / `ui` / `hud` / `present` scopes.

**Headless-coverable:** the producer-selection logic (`selectActive` over derived profiles — overworld
→ WORLD, bg-screen → MENU-shaped, no-bg-screen → PLAIN_UI) if extracted pure; `renderProfile`
derivation from a `def` table; the `frosted` effect-dispatch table shape. The GL producers
(`composeScene`, `composeScreensScene`, `produceUI` grade, `FrostedGlass.fromScene`) are visual-gated.

---

## Risks / notes

- **Combined-grade vs per-screen-grade**: the combined sceneRT is graded once with the top screen's
  settings; during a cross-transition with two different grade settings the blend differs slightly
  from today. Accepted (same family as the signed-off option-(b) deviation); transitions are brief.
- **ui-layer grade cost**: a per-frame widget capture+grade for menus (a touch more than today's single
  combined grade) — dominated by the frosted 2–3×→1× win, and removed in 2c-3 when `ui.grade=false`.
- **Dirty working tree**: many uncommitted files (incl. UI components) — use targeted `git add` per
  task; never `git add -A`.
- **`UI.drawTransition()`** (global transition overlay) must remain wired inside the ui-layer producer.
