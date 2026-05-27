# Rendering Phase 2c — Scene Unification, Layer Compositor & Color Pipeline Design

**Date:** 2026-05-27
**Scope:** GachaClient (Love2D / LuaJIT / OpenGL)
**Status:** Approved design, pending implementation plan(s)
**Parent spec:** `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md` (Phase 2). This sub-spec fills the Phase-2 mechanics the master spec left underspecified.

## Context

Phase 2a (render-graph foundation: `lib/Pool`, `systems/render/{RenderTargets,Pass,Renderer}`, `love.draw` → one `legacy` pass) and Phase 2b (RenderTargets wired into the Renderer/`ctx` + per-frame `frameReset`; `legacy` split into `scene`/`present`/`hud`; AA canvas onto the pool) are **DONE and validated**.

Phase 2c unifies the **distributed** per-renderer scene→grade→upscale chains into a shared `sceneRT` driven by a **layer compositor**, then flips the scene path to linear/HDR + ACES tonemap.

### Why this is non-trivial (the current reality)

Scene→grade→upscale is **not** centralized. Each producer owns its own chain:
- **Overworld `RenderSystem`** renders the world into its own `ppCanvas` (internal-res), grades via `PostFx.apply` → `gradeCanvas`, then `RenderScale.upscale` into the externally-set frame target. Owns 6 canvases. Invoked indirectly via `state.world:update(0, nil, "draw")` (no `ctx` parameter).
- **Screens** draw their own background (bg color + bg shader, e.g. `SpaceBackground`/`pull_tunnel`), optionally wrap content in `PostProcessEffect` (own capture canvas + the *same* `PostFx` shader, UI-flavored defaults), and optionally call `FrostedGlass.prepare` — which **re-renders the background** into its own 3 canvases for the blur source.
- **`WaveGame` / `CombatRenderer`** follow the same own-chain pattern (combat rendering plumbing only — abilities off-limits).

Two grade paths today (world via `RenderSystem`; screen content via `PostProcessEffect`) + a third background render inside `FrostedGlass`. The reveal's ~50fps is `FrostedGlass` re-running the 596-line `pull_tunnel.glsl` 2–3× per frame.

Today the whole frame (world **and** UI) is composited into one AA canvas and FXAA/MSAA-resolved together, so **UI is currently anti-aliased**.

## Decisions (locked with the user, 2026-05-27)

- **The frame is an ordered list of uniform rendering layers**, each a composable capability table. The scene/UI distinction and the linear/sRGB seam are **not special cases** — they are different flag values on uniform layers. (Chosen as the full per-layer model over a single profile capability table + hardcoded UI handling.)
- **Per-layer feature toggles**: `scale`, `grade`, `tonemap`, `aa`, `effects` are independent per layer. Notably **`aa` is per-layer** — so "UI anti-aliased" (today) vs "UI sharp" becomes a one-flag opt-in, not an architecture decision.
- **Composition over enumeration**: layers and their flags compose freely; named defaults (`WORLD`/`MENU`/`PLAIN_UI`/`OVERLAY`) are spread-and-override shorthand, not a closed set.
- **Order static, behavior dynamic**: the compositor processes the active profile's ordered layers; it is **not** a per-frame dependency compiler (master-spec non-goal). A small `normalize` enforces per-layer dependency sanity; no input/output wiring engine.
- **Structural first (8-bit, parity-gated), then a separate linear/HDR + ACES flip.** Defaults reproduce today exactly (scene+ui `aa=true`, hud sharp) so the structural migration is screenshot-identical; the flip is the one deliberate look change.
- **Color scope: scene-only linear, UI stays sRGB.** Falls out of flags: scene layer `tonemap=true`/HDR; ui/hud `tonemap=false`. **Tonemap operator: ACES filmic.**
- **Portal distortion is removed, not migrated.** The in-world `RenderSystem` portal pass is **dead code** — `systems/PortalSystem.lua` is a stub (`entities={}`, `drawWorld()` no-op), so the `#PortalSystem.entities > 0` guard never fires. `ui/components/PortalDistortEffect.lua` (referenced only via `shader_registry`'s `portal_distort` factory) is retired. Removing both is visually invisible and sheds `RenderSystem`'s `portalDistortCanvas`/`portalCaptureCanvas` + helpers.
- **`effects` is a composable *set* of named screen-space effects** per layer — `{"frosted"}` now; `pixelate` (`ui/components/PixelateEffect.lua`) and future distortions reintroducible. Portal distortion is **not** reintroduced.
- **Screen transitions are kept** (possible future rework). The transition-capture layer (`screen.lua` `_transitionCanvas`, per-screen copies, `ui_loader` `_contentCanvas`) coexists with the compositor; `PostProcessEffect`'s active-canvas save/restore semantics MUST be preserved where its grade folds into a layer's `grade`.
- Combat **abilities** (kits/data/power/execution) off-limits — rendering plumbing only.

## Architecture

### Layer descriptor (the unit)

A **Layer** is a plain capability table — render-agnostic data, FFI-neutral, headless-testable:
```lua
{ name    = "scene"|"ui"|"hud"|<custom>,
  source  = "world"|"background"|"ui"|"hud"|"none",  -- how the layer's pixels are produced
  scale   = boolean,    -- render at internal render-scale res into sceneRT, then upscale (else native)
  grade   = boolean,    -- apply the PostFx grade to this layer
  tonemap = boolean,    -- linear→display ACES encode (flip phase; HDR layers only)
  aa      = boolean,    -- this layer joins the anti-aliased resolve group
  effects = { ... } }   -- ordered set of screen-space effects on this layer's RT, e.g. {"frosted"}
```

### Profile = ordered layers

A **profile** (per screen / the overworld) is `{ layers = { <Layer>, ... }, overlay = boolean }`. The scene/UI/HDR distinctions are just flag values:
- **scene layer:** `{ name="scene", source="world", scale=true, grade=true, tonemap=true, aa=true, effects={} }`
- **ui layer:** `{ name="ui", source="ui", scale=false, grade=false, tonemap=false, aa=true, effects={} }`  ← set `aa=false` for crisp UI
- **hud layer:** `{ name="hud", source="hud", scale=false, grade=false, tonemap=false, aa=false, effects={} }`

**Named defaults** (spread-and-override layer lists):
- `WORLD` = `{ scene(world), ui, hud }`
- `MENU`  = `{ scene(background, effects={"frosted"}), ui, hud }`
- `PLAIN_UI` = `{ ui, hud }` (no scene layer)
- `OVERLAY` = `{ ui }`, `overlay=true` (a modal — contributes nothing as a scene owner; its widgets still appear because the `ui` layer's `UI.draw()` renders the whole screen stack)

### The compositor (in `Renderer`)

The active profile's layers are processed in order:
1. For each layer, **produce** its pixels by `source`:
   - `world`/`background` (scene-type): call `producer:drawScene(ctx)` into a `sceneRT` (sized to internal render-scale res when `scale`, else native); then apply this layer's `effects` (sample `sceneRT`), `grade` (PostFx), `tonemap` (flip phase) in its RT.
   - `ui`: `UI.draw()` (+ transition) — renders the whole screen stack (so stacked modals' widgets appear).
   - `hud`: FPS readout + warp flash.
2. **Composite** per `aa`: layers with `aa=true` are drawn into the shared AA canvas (FrameAA; scene layers upscaled in when `scale`); after the contiguous AA group is composited, **one** AA resolve (`FrameAA.present`); layers with `aa=false` draw sharp, in order, after the resolve. Defaults give the today-exact order: `[scene(aa) → ui(aa)] → resolve → hud(sharp)`.

Producers publish what global stages need on `ctx` (`ctx.sceneRT`, the layer's grade settings). Because `RenderSystem:draw` is reached through the ECS (`state.world:update(...,"draw")`), the scene-producing step sets a small current-frame `ctx` accessor the producer reads synchronously (set-then-use within the same frame — the safe form of the pattern).

### Per-frame profile selection (screen stack)

- `not UI.hasScreens()` → `WORLD`.
- Else walk the stack top-down; the **topmost non-`overlay` profile** is the active profile (supplies the frame's layer list, including its scene layer). `overlay` modals above it add no scene; their widgets still render via the `ui` layer's `UI.draw()`.
- All-overlay stack → `PLAIN_UI` (no scene owner).

### Dependency normalization (`normalize`, per layer)

Headless-testable, enforces sanity so a bad layer can't render garbage:
- `effects`/`grade`/`tonemap`/`scale` require `source ~= "none"` → cleared (effects emptied; the rest false; one-time warn) on a sourceless layer.
- Effect names not in the recognized set are dropped.

### How the current paths reconcile

- `RenderSystem`'s internal `PostFx` and `PostProcessEffect`'s per-screen grade both become a layer's `grade=true` (the single PostFx applied by the compositor). `PostProcessEffect` is retired as a separate capture/grade path.
- `FrostedGlass` stops re-rendering the background; it becomes the `frosted` entry in a scene layer's `effects`, sampling `sceneRT`. The reveal's 2–3× → 1×.
- `RenderScale.upscale` moves into the compositor's scene-layer upscale step (one place); its `midCanvas` moves onto the `RenderTargets` pool.
- The in-world portal distortion pass + `PortalDistortEffect.lua` are **deleted** (dead/retired); `PixelateEffect.lua` is reintroduced later as an `effects` member.
- The **screen-transition capture layer** stays intact; the `ui` layer draws through the existing transition machinery, and the compositor preserves `PostProcessEffect`'s active-canvas save/restore where grade folds in.

## Reconciliation with the executed Plan 2c-1

Plan 2c-1 (done) built `systems/render/Profile.lua` as a **single** capability table (`{scene, effects, post, tonemap, overlay}`) + `Renderer:applyProfile`. The layer model **generalizes** this, low-cost because it has **no in-game consumer yet**:
- The single capability table becomes the **Layer descriptor** (`scene`→`source`, `post`→`grade`, add `scale`/`aa`/`name`); `resolve` (spread+override) and `normalize` (now per layer) carry over.
- `overlay` moves from the table to the **profile** level; a profile wraps an **ordered layer list**.
- `Renderer:applyProfile` (toggle four named passes) is replaced by the **layer compositor** (consumes the layer list). `selectActive` still picks the active profile (now layer-bearing).
The revised foundation plan (2c-1′) reworks `Profile.lua` → `Layer.lua` + layered `Profile`, retaining the tested `resolve`/`normalize`/`selectActive` logic at the layer/profile levels.

## Migration plan (structural, 8-bit, parity-gated, one producer at a time)

- **Stage 0 — Layer foundation (rework 2c-1):** `Layer` descriptor + layered `Profile` (`resolve`/`normalize`/`selectActive` + named defaults as layer lists) + the compositor in `Renderer`, all headless-tested. No in-game wiring yet.
- **Stage 1 — Scaffolding (parity by inertness):** `sceneRT` infra; wire the compositor into `love.draw` with every screen/state's profile set to reproduce today (defaults: scene+ui `aa=true`, hud sharp); existing per-renderer chains still run until a producer is migrated.
- **Stage 2 — Overworld:** dead-code portal cleanup (invisible), then `RenderSystem:draw` → `drawScene(ctx)` into `sceneRT`; the `WORLD` scene layer's `grade`/`scale` replace RenderSystem's internal `PostFx`/`RenderScale`; RenderSystem sheds `ppCanvas`/`gradeCanvas`/present block. Gate: overworld pixel-identical.
- **Stage 3 — Frosted screens (perf win):** screen backgrounds → `drawScene` into `sceneRT`; the scene layer's `effects={"frosted"}` samples `sceneRT`; `FrostedGlass` stops re-rendering. Gate: menus/reveal identical **and** heavy-shader/`draws` count drops (2–3× → 1×).
- **Stage 4 — `WaveGame` / `CombatRenderer`-plumbing:** same producer-split pattern (abilities untouched).

Each stage is screenshot-comparable to the prior commit and must not be worse on frame time.

## Color flip (separate phase — the one deliberate look change)

Once all producers feed `sceneRT`, the scene layer's flags flip to HDR: `sceneRT` becomes `rgba16f` linear (verify via `getCanvasFormats`); `tonemap=true` runs a real **ACES filmic** encode; `PostFx` grade math re-tuned for linear. UI/hud layers keep `tonemap=false`/sRGB. Gated by deliberate before/after screenshots + a mid-gray sRGB→linear→tonemap→sRGB round-trip sanity check.

## Decomposition into implementation plans

- **Plan 2c-1′ (revised foundation):** `Layer` + layered `Profile` (`resolve`/`normalize`/`selectActive`/named defaults) + the `Renderer` compositor — headless-tested, supersedes 2c-1's single-table model.
- **Plan 2c-1b:** `sceneRT` infra + wire the compositor into `love.draw` (Stage 1) + portal dead-code removal + overworld migration (Stage 2) — visual + F9 parity gated.
- **Plan 2c-2:** frosted screens + `WaveGame`/`CombatRenderer` producers (Stages 3–4) — kills the reveal 2–3×.
- **Plan 2c-3:** the linear/HDR `rgba16f` + ACES flip + `PostFx` linear re-tune.

## Components & ownership

| Unit | Responsibility | New / changed |
|---|---|---|
| `systems/render/Layer.lua` | Layer capability descriptor + `resolve` + `normalize` | New (generalizes 2c-1's Profile table) |
| `systems/render/Profile.lua` | Layered profile (ordered layers) + named defaults + `selectActive` | Reworked from 2c-1 |
| `systems/render/Renderer.lua` | Layer compositor: produce + composite per-layer flags, AA grouping | Modify (compositor replaces `applyProfile`) |
| `systems/RenderSystem.lua` | `:draw` → `:drawScene(ctx)` into `sceneRT`; sheds internal post/upscale + dead portal pass | Modify (plumbing) |
| `ui/components/PostFx.lua` | Shared grade; invoked by a layer's `grade` | Reused; linear re-tune in 2c-3 |
| `ui/components/RenderScale.lua` | Upscale; invoked by the scene-layer composite; `midCanvas` → pool | Modify |
| `ui/components/FrostedGlass.lua` | `frosted` effect samples `sceneRT` (no bg re-render) | Modify |
| `ui/components/PostProcessEffect.lua` | Retired as a separate grade path (folds into a layer `grade`) | Removed/absorbed |
| `ui/components/PortalDistortEffect.lua` + `shader_registry` entry + in-world portal pass | Dead/retired | Removed |
| `ui/components/PixelateEffect.lua` | Reintroduced later as an `effects` member | Deferred |
| `ui/core/screen.lua` + transition canvases | Transition capture; coexists with the compositor | Unchanged (rework deferred) |
| Screens + overworld | Declare a `renderProfile` (layer list); scene owners implement `drawScene(ctx)` | Modify per producer |

## Testing & verification

- **Headless (`tests/render_harness`):** `Layer.resolve` composition (spread+override, no base mutation); `Layer.normalize` per-layer dependency rules + effect-name filtering; `Profile` named defaults are well-formed layer lists; `selectActive` stack rule (top-non-overlay, empty→world, all-overlay→PLAIN_UI); compositor layer-ordering / AA-grouping logic driven by a fake profile + fake producers (assert produce-order and which layers join the AA group). All pure logic, no GL.
- **GL / visual:** boot + visual + F9 profiler parity per migration stage (screenshot-compare). Stage 3 asserts heavy-shader/`draws` drop (2–3× → 1×). The flip uses deliberate before/after + the mid-gray round-trip.
- **Honest caveat:** true per-pass GPU time awaits the optional FFI timer-query backend; until then CPU-time + draw-stats + visual.

## Risks

- **Producer migration regressions** → per-stage parity gating (screenshot + profiler).
- **Compositor AA-grouping edge cases** (non-contiguous `aa` flags) → define simple semantics (contiguous bottom AA group resolved once; non-aa drawn sharp on top in order); the live defaults are the clean contiguous case; nail exact semantics in 2c-1′.
- **ctx threading to the ECS-invoked `RenderSystem`** → small synchronous current-frame ctx accessor; covered by the overworld migration gate.
- **Linear flip changes the look** → isolated phase, deliberate, before/after gated; per-layer flags keep UI sRGB.
- **`rgba16f` bandwidth** → profiler-budgeted; render-scale helps; verify format availability.

## Open items (resolve during planning)

- Exact named-default layer lists — derive from auditing current screens' actual grade/frosted usage in Plan 2c-1′/2c-1b.
- Compositor home: inline layer functions in `main.lua` (like 2b) vs `systems/render/passes/*` — decide by size in 2c-1b.
- Per-layer/per-pass budget values — set empirically once the compositor exists.
- Exact AA-grouping semantics for non-contiguous `aa` (degenerate today) — pin down in 2c-1′.
- Screen-transition rework (kept-but-could-change) — deferred; revisit after producers are unified.
