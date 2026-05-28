# Rendering Homogenization — Design Spec

**Date:** 2026-05-27
**Scope:** GachaClient (Love2D / LuaJIT)
**Status:** Approved design, pending implementation plans (one per sub-workstream)
**Parent initiative:** [AAA rendering ecosystem](2026-05-27-aaa-rendering-design.md). Homogenization is the first "remaining unaddressed workstream" called out in the Phase 4 audit (`project_aaa_rendering.md`).

## Problem / motivation

Phase 2c-2 / 2c-2b / 2c-3 established a single canonical rendering contract: a screen exposes `renderProfile` + `gradeSettings` + `drawScene(ctx)`, and the compositor (`composeScene` / `composeScreensScene` / `resolveFrame` / `composeFrame` in `main.lua`) owns scene capture, scene-layer effects (`frosted`), grade, tonemap, AA resolve, and present. Declarative screens auto-derive this contract via `Profile.forScreenDef` in `ui_loader.lua`. Hand-coded screens (CombatScreen, WaveGame) opted in via `Profile.forProducer` during 2c-2b.

The Phase 4 audit identified three consumers still off-contract or partially off-contract:

1. **WaveGame** is on the contract but compiles five shaders from inline `[[ GLSL ]]` strings via raw `love.graphics.newShader`, bypassing `services.Assets`. The producer-internal bloom pipeline is intentional (locked by 2c-2b: "producer-internal effects stay inside the producer") and stays.
2. **`login.lua`** is fully off-contract. No `renderProfile` / `drawScene` / `gradeSettings`. Owns its own `bgCanvas` (raw `love.graphics.newCanvas`), captures `SpaceBackground:draw` into it, applies frosted via `HudEffects` (a parallel implementation of the canonical `FrostedGlass.fromScene` scene-layer effect), draws a per-pixel gradient line (~364 `1×1` `rectangle` calls), and iterates `self.children` directly bypassing `Widget.draw`.
3. **`weapon_select.lua` and `character_select.lua`** are widget components, not screens, but each carries ~50 lines of copy-pasted transition state machine (`self.transition` FSM, `self._transitionCanvas` via raw `newCanvas`, own `getTransitionValues`/`beginTransitionDraw`/`endTransitionDraw`). Their canvas is `screenW × screenH` regardless of `RenderScale` setting, producing the audit's "undersized at non-1× scale" bug.

Two other audit items (`reveal_item` parallel multi-pass pipeline, `WaveGame` bespoke bloom) were re-examined during brainstorming and removed from scope:

- `reveal_item` is a widget on the already-declarative `screen_3`. Its internal multi-step animation (burst → silhouette → flash → stars) is ordinary widget drawing — no parallel canvas pipeline. It already samples `FrostedGlass.sceneCanvas()` (the canonical seam).
- `WaveGame` bespoke 4-canvas bloom is locked-in producer-internal infrastructure per 2c-2b. Out of scope.

## Decisions (locked)

- **Three independent sub-workstreams, one design spec, three implementation plans.** Each sub-workstream ships and visual-gates independently. Order: W1 → W2 → W3 (cheapest first, biggest blast radius last). Rejected alternatives: single combined plan (bigger blast radius per commit, gates collide), infrastructure-first (nothing shared to extract beyond what the contract already provides).
- **Subselects (weapon_select / character_select) are promoted to full `Screen`s with `Profile.OVERLAY`.** Rejected: keep as widgets with fixed render-scale canvas (leaves ~50 lines of duplicated state machine × 2). Rejected: strip the transition entirely (loses UX polish). Promotion eliminates the duplicated machinery and lets the existing screen-stack transition system handle it.
- **W3 login gradient line replaced with a 4-vertex linear gradient mesh** approximating the existing `sin(πt) * 0.4` gold alpha falloff via two endpoint vertices. Rejected: keep the per-pixel line (not a contract issue but trivially fixable on the same touch), flat single line (visibly different look).
- **W2 inter-screen communication: callbacks on the screen instance**, not a new event bus. A bus is a worthwhile future direction (see `project_ui_event_bus.md`) but is its own architecture decision and own brainstorm. Doing it here would balloon scope. Callbacks today become `Bus.emit(...)` one-liners later if the bus lands.
- **W3 login chrome (panel border + accent stripe with edge glow) stays manual in `:draw`**. It is login-specific and not worth pushing into `Panel` style properties.
- **Producer-internal effects stay inside the producer.** WaveGame's 4-canvas bloom + near-miss lens vignette are not refactored. The compositor owns only cross-cutting passes (grade / upscale / AA / frosted).
- **Combat ABILITIES gameplay is off-limits.** This workstream touches rendering plumbing only.

## Architecture — the contract (recap)

A screen opts into the canonical pipeline by exposing four things:

```lua
screen.renderProfile = Profile.forProducer({...})   -- or Profile.forScreenDef(def), or Profile.OVERLAY
screen.gradeSettings = ...                           -- complete grade table, or nil to skip the grade
screen.coversBelow   = true                          -- iff a full-screen producer pushed over another screen
function screen:drawScene(ctx) ... end               -- fills ctx.sceneRT (compositor binds the canvas first)
```

The compositor (`main.lua`) then drives:
- `resolveFrame` picks the active profile and binds `ctx.sceneProducer` (the world producer for overworld, `composeScreensScene` for stacked screens).
- `composeScreensScene` composites every visible non-overlay screen's `drawScene` into one `ctx.sceneRT` at per-screen transition scale/alpha.
- `composeScene(ctx, producer, layer)` runs the producer, then the layer's effects (`SceneEffects.frosted` → `FrostedGlass.fromScene(ctx.sceneRT)`), then the grade (`PostFx.apply` with optional tonemap), then upscale (`RenderScale.upscale`) into the AA target or screen.
- `composeFrame` groups maximal-`aa` layer runs and resolves each group once via `FrameAA`.

Scene-layer effects are an OPEN SET (`SceneEffects` dispatch table). `frosted` is the only entry today; this spec adds no new entries.

`Profile.OVERLAY = { layers = { Layer.resolve(UI_LAYER) }, overlay = true }` — overlay screens skip the scene layer entirely; `Profile.selectActive` walks past them, so an overlay over a producer still composes the producer's scene underneath.

## Sub-workstream W1 — WaveGame inline shaders → `Assets.shader`

### Scope
Five shaders defined as inline GLSL strings in `ui/screens/dailies/WaveGame.lua` lines 65–209 — `_BLUR_GLSL`, `_BRIGHT_GLSL`, `_INNER_GLOW_GLSL`, `_WALL_GLSL`, `_LENS_GLSL` — compiled via raw `love.graphics.newShader(STRING)` in `_initGfx`.

### Change
Extract each to its own file under `data/shader/wave/`:

```
data/shader/wave/blur.glsl
data/shader/wave/bright.glsl
data/shader/wave/inner_glow.glsl
data/shader/wave/wall.glsl
data/shader/wave/lens.glsl
```

`_initGfx` loads each via `Assets.shader("wave/blur")` etc. The existing pattern in this file already loads `Assets.shader("fiber_bg")` from `data/shader/fiber_bg.glsl`. The five `local _X_GLSL = [[ ... ]]` blocks and their five `_X = _X or love.graphics.newShader(_X_GLSL)` lines delete. `_INNER_GLOW_FMT` and the rest of the file stay unchanged.

### Behavior
Bit-identical output. Same GLSL source, same uniform sends, same draw order. The only behavior change is one-time disk read on first WaveGame entry per session (already the pattern for `fiber_bg`).

### Verification
- `assets_harness` loads each new shader and asserts compile success.
- In-game: F4 → WaveGame, play one run — visually identical (same shader source, same draws). One-shot screenshot compare against pre-migration is sufficient.

### Risk
- `services/assets/shader.lua` keys shaders by stem; if subdirectory paths (`wave/blur`) don't resolve, fall back to flat keys (`wave_blur`, `wave_bright`, etc.). Confirmed during W1 task 1 (single grep + harness probe).

### Cost
Small. ~145 lines of Lua → ~145 lines of GLSL across 5 files, plus 5 one-line load calls. One commit.

## Sub-workstream W2 — `weapon_select` + `character_select` → full Screens

### Scope
- `ui/screens/inventory/weapon_select.lua` (Widget today, ~830 lines)
- `ui/screens/party/components/character_select.lua` (Widget today, ~850 lines)
- Callsites: `ui/screens/inventory/init.lua`, `ui/screens/party/init.lua` (where each component is currently added as a child and `:open()`-ed)

### Change

Both files become Screens following the standard hand-coded pattern:

```lua
local Screen  = require "ui"
local Profile = require "systems.render.Profile"

function WeaponSelectScreen.new(opts)
    local screen = Screen.new({ escapeCloses = true, blocksInput = true,
                                transition = "fade_grow" })
    setmetatable(screen, { __index = setmetatable(WeaponSelectScreen, { __index = Screen }) })
    screen.renderProfile = Profile.OVERLAY
    screen.gradeSettings = nil
    -- existing initialization (inventory data, selection state, button bounds, etc.)
    return screen
end

function WeaponSelectScreen:draw()
    if not self.visible then return end
    local useT = self:beginTransitionDraw()
    -- existing draw body, but using Widget.draw(self) for children, NOT _transitionCanvas
    if useT then self:endTransitionDraw() end
end
```

Deleted from both files:
- `self.transition = { state, progress, duration, onComplete }` FSM field
- `self._transitionCanvas` / `self._prevCanvas` fields
- `:open()` / `:close()` methods (parent uses `UI.push` / `UI.pop` instead)
- `:getTransitionValues`, `:beginTransitionDraw`, `:endTransitionDraw` overrides — base `Screen` versions take over
- The `_transitionCanvas` / `_prevCanvas` blocks inside `:draw` (the base class handles this through the standard `beginTransitionDraw`/`endTransitionDraw`).
- The transition advancement block in `:update(dt)` (base class handles).

Kept:
- All existing data (selection state, scroll, button bounds, item lists, layout cache).
- All existing input handlers (now invoked by `ScreenStack` directly).
- All existing callbacks (`onClose`, `onEquip`, `onUnequip`, `onSelect`, `onRemove`, `onEquipWeapon`) — stay as fields on the screen instance, parent passes them in opts.

### Callsite changes

In `ui/screens/inventory/init.lua`, where weapon_select is added as a child and `:open(character, inventory, equipment, weaponRefinement)`-called, replace with:

```lua
local WeaponSelectScreen = require "ui.screens.inventory.weapon_select"
UI.push(WeaponSelectScreen.new({
    character          = character,
    inventory          = self.inventory,
    equipment          = self.equipment,
    weaponRefinement   = self.weaponRefinement,
    readOnly           = self.readOnly,
    onEquip            = function(weapon) self:_onWeaponEquipped(character, weapon) end,
    onUnequip          = function() self:_onWeaponUnequipped(character) end,
    onClose            = function() ... end,
}))
```

The same pattern for `character_select` in `ui/screens/party/init.lua`. Parent screen stays on the stack underneath; its `renderProfile` (scene producer or declarative) still composes through. The overlay subselect draws on top in the ui layer.

### Parent-state extraction

The plan task will enumerate every `self.parent.X` or `self.parent:Y(...)` read currently in each component. Each becomes either:
- A constructor option (immutable snapshot), or
- A callback the screen invokes to fetch live data, or
- Surfaced through callbacks fired back to the parent.

This is mechanical but exhaustive — missing one leaves a stale read. Done as part of plan-writing.

### Verification
- F2 → Party → click empty slot → `character_select` opens with a fade+grow; pick a character → closes; party slot fills.
- F3 → Inventory → click character → equip weapon button → `weapon_select` opens; pick a weapon → closes; equipment updates.
- ESC closes either subselect cleanly; parent screen retains focus and selection.
- Resize window while subselect is open: layout reflows without artifacts.
- Set render-scale to 0.75 (or similar) in Settings: subselect renders at the correct on-screen size (the existing bug fix gate).
- Open/close transitions visually match today's (same `fade_grow` curve via shared `Transition` module).
- Existing equip / unequip / select / remove / refine flows: every callback that fires today still fires.

### Risk
- **Parent-state coupling missed during extraction.** Subselect opens with stale or missing data. Mitigation: exhaustive enumeration during plan-writing; visual gate would catch it.
- **Input plumbing change.** As widgets, subselects receive routed input from the parent screen. As Screens, they get input from `ScreenStack` directly with `blocksInput = true`. Focus/tab/ESC routing needs verification. Mitigation: visual gate covers it; existing `escapeCloses` semantic is in place.
- **Modal stacking interactions.** A subselect open over inventory; user opens Settings (overlay) via F-key. Need to verify the input + draw order remains sane. Settings is `Profile.OVERLAY` too — two overlays stacked. Existing `ScreenStack` handles N-deep, but worth a manual check.

### Cost
Medium. ~30% net code reduction across the two component files (transition machinery delete) plus callsite-and-callback wiring at two parent screens. One commit per subselect (W2 = 2 commits), maybe a third for shared boilerplate if anything emerges.

## Sub-workstream W3 — `login.lua` → producer contract

### Scope
`ui/screens/login.lua`. The constructor, `:draw` method, and the module-local `bgCanvas` variable are the touch points; `:update`, network state machine, button handlers, and `tryAutoLogin` / `doAuth` flows are untouched.

### Change

Constructor adopts the contract:

```lua
local Profile = require "systems.render.Profile"

function LoginScreen.new(onLoginSuccess)
    local screen = Screen.new({ escapeCloses = false, onEnter = ... })
    setmetatable(screen, { __index = setmetatable(LoginScreen, { __index = Screen }) })
    screen.renderProfile = Profile.forProducer({ effects = { "frosted" } })
    screen.gradeSettings = nil
    screen.spaceBackground = SpaceBackground.new("default")
    -- existing initialization (server presets, saved username, state machine, etc.)
    screen:buildUI()
    screen:rebuildServerSection()
    return screen
end
```

New method:

```lua
function LoginScreen:drawScene(ctx)
    if not self.visible then return end
    love.graphics.clear(0, 0, 0, 1)
    self.spaceBackground:draw()
end
```

`composeScreensScene` picks Login up as a sceneScreen automatically. `frosted` scene effect runs `FrostedGlass.fromScene(ctx.sceneRT)`, blurring the rendered scene into `FrostedGlass.sceneCanvas()` for any panel set to `frosted = true` to sample.

### Panel becomes frosted via the canonical widget

Today: `style = { background = {0,0,0,0}, border = {0,0,0,0}, cornerRadius = 0, borderWidth = 0 }` + manual `HudEffects.applyFrostedGlass(bgCanvas)` over the panel rect.

After: `frosted = true, cornerRadius = 0` on the Panel widget. The widget samples `FrostedGlass.sceneCanvas()` (set by the canonical scene effect) and draws the blurred sample over its rect. Same visible result; one widget property instead of manual region management.

### Per-pixel gradient line → mesh

The `for i = 0, lineWidth do love.graphics.rectangle("fill", lineX + i, lineY, 1, 1) end` loop becomes a 4-vertex linear gradient mesh built once in `:buildUI()`. Vertex colors approximate the `sin(πt) * 0.4` gold falloff: endpoints `(gold, 0)`, midpoint conceptually `(gold, 0.4)`. A 4-vertex strip (left-edge, left-mid, right-mid, right-edge) gives a 3-segment approximation; or a 5-vertex `fan` centered with two edge vertices. Either way: one `draw` call instead of ~364.

### `:draw` becomes widgets-only

What changes inside `:draw`:

| Block | Decision |
|---|---|
| `ensureCanvas(...)` + `bgCanvas` capture + screen blit | Delete. Compositor owns the scene render. The module-local `bgCanvas` variable + `ensureCanvas` method delete. |
| `HudEffects.clearRegions / addRegion / applyFrostedGlass` | Delete. Replaced by `Panel.frosted = true` widget property. |
| Panel border + top accent stripe + edge glow loop | Keep (UI chrome). |
| Per-pixel gradient line | Replace with mesh draw. |
| `for _, child in ipairs(self.children) do child:draw() end` | Replace with `Widget.draw(self)` (standard recursion). |
| Status indicator (bottom-right) | Keep inside the transition wrap. |
| `beginTransitionDraw` / `endTransitionDraw` | Keep. |

Module-local `bgCanvas` and `LoginScreen:ensureCanvas` delete entirely (their only consumer was the deleted capture path).

### Verification
- Visual: login screen looks identical — space background, frosted panel, accent stripe, gold gradient line, status indicator, both dropdown closed and dropdown open states.
- Functional: connect flow, manual login, manual register, auto-login (cached session) all complete to the click-to-play landing.
- Resize while on login: bg + frosted track correctly.
- Render-scale ≠ 1: bg scales correctly through `composeScene`'s upscale path.
- F9 profiler now shows a `scene` scope on login (currently shows none — Login falls to `PLAIN_UI` with no scene producer).

### Risk
- **`Panel.frosted = true` look mismatch.** The widget may not produce the exact look of the manual `HudEffects.addRegion + applyFrostedGlass(bgCanvas)` path (region shape, blur strength, alpha). Mitigation: visual gate catches it; fallback is a one-line direct `love.graphics.draw(FrostedGlass.sceneCanvas(), ...)` over the panel rect inside `:draw`'s chrome block — still using the canonical sceneCanvas, just custom geometry. (W3 task verifies which path matches; plan picks one.) The Phase 2c-2 lesson — sparse `gradeSettings` leaks prior screen's grade — does not apply here (Login's `gradeSettings = nil` skips the grade path entirely).
- **Auto-login timing.** `LoginScreen.new` is called from `main.lua` at boot before any draw; the `spaceBackground` field is created in the constructor, so it's ready by the first `drawScene` call. No regression expected; covered by the auto-login gate.
- **`coversBelow` semantics.** Default `false`. Login is the only screen up at boot — nothing underneath. If a future flow stacks Login over another screen, revisit then.

### Cost
Medium. Roughly -80 lines net (delete `bgCanvas` + manual frosted path + per-pixel gradient loop, replace with `drawScene` method + Panel property + mesh draw). One commit; visual-gated.

## Sequencing

```
W1 (WaveGame shaders → Assets)      ← cheapest, no contract change, low risk
W2 (subselects → Screens)            ← medium, contract change, render-scale bug fix included
W3 (login → producer)                ← medium, biggest single migration, visual-gated
```

Each ships as its own plan + its own commit set. W1 is independent of W2/W3. W2 and W3 are independent of each other.

When each workstream ships, append a one-line entry to `project_aaa_rendering.md` under the audit-remaining-workstreams block, matching the existing `Phase 4b — DONE` / `Asset bypasses — DONE` entries. When all three ship, strike Homogenization from the remaining-workstreams list.

## Verification — overall

- **No headless render harness coverage for visual changes.** GL output isn't deterministically reproducible across drivers; these workstreams are visually gated in-game.
- **`assets_harness` covers W1 shader loads.**
- **F9 profiler** is the structural gate: W3 should add a `scene` scope to the login frame (today Login falls to `PLAIN_UI` and skips the scene layer); W2 should show no scope changes (subselects are `Profile.OVERLAY`, ui-layer only).
- **Resize and render-scale = 0.75** are explicit checkpoints for W2 and W3 (W2's bug fix gate; W3's compositor upscale path).

## Ownership boundaries

- This workstream touches: `ui/screens/dailies/WaveGame.lua`, `ui/screens/login.lua`, `ui/screens/inventory/weapon_select.lua`, `ui/screens/party/components/character_select.lua`, `ui/screens/inventory/init.lua`, `ui/screens/party/init.lua`, `data/shader/wave/*.glsl` (new).
- **Off-limits:** combat ABILITIES gameplay (kits / data / power / execution). Rendering plumbing only is in scope for the rendering initiative; this workstream touches none of combat anyway.

## Future direction

This work supports two longer-term directions tracked in user memory (`project_ui_event_bus.md`, `project_ui_json_only.md`):

- **UI event bus.** W2's subselect callbacks are 1-line-rewritable to `Bus.emit(...)` once a bus design lands. No structural change to W2's screen shapes will be needed.
- **JSON-only UI.** W3 leaves Login shaped exactly like a declarative screen — `renderProfile = Profile.forProducer(...)` matches `Profile.forScreenDef`'s output shape, `drawScene` clears bg + draws SpaceBackground, chrome moves toward `Panel.frosted` widget property. A future "Login as JSON" pass would extract the connection-state FSM into a behavior graph + service.

Every consumer migrated here ends in a state strictly closer to JSON-migratable than it started.
