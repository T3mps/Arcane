# Rendering Phase 2c — Scene Unification & Color Pipeline Design

**Date:** 2026-05-27
**Scope:** GachaClient (Love2D / LuaJIT / OpenGL)
**Status:** Approved design, pending implementation plan(s)
**Parent spec:** `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md` (Phase 2). This sub-spec fills the Phase-2 mechanics that the master spec left underspecified.

## Context

Phase 2a (render-graph foundation: `lib/Pool`, `systems/render/{RenderTargets,Pass,Renderer}`, `love.draw` → one `legacy` pass) and Phase 2b (RenderTargets wired into the Renderer/`ctx` + per-frame `frameReset`; `legacy` split into `scene`/`present`/`hud`; the full-frame AA canvas moved onto the pool) are **DONE and validated**.

Phase 2c does the deferred, cross-cutting work: unify the **distributed** per-renderer scene→grade→upscale chains into a shared `sceneRT` consumed by **global** passes, and then flip that path to linear/HDR with an ACES tonemap.

### Why this is non-trivial (the current reality)

Scene→post→upscale is **not** centralized. Each producer owns its own chain:
- **Overworld `RenderSystem`** renders the world into its own `ppCanvas` (internal-res), grades via `PostFx.apply` → `gradeCanvas`, then `RenderScale.upscale` into the externally-set frame target (lines ~1183–1998). It owns 6 canvases.
- **Screens** draw their own background (bg color + bg shader, e.g. `SpaceBackground`/`pull_tunnel`), optionally wrap content in `PostProcessEffect` (its own capture canvas + the *same* `PostFx` grade shader, UI-flavored defaults) for grading, and optionally call `FrostedGlass.prepare` — which **re-renders the background** into its own 3 canvases to produce the blur source.
- **`WaveGame` / `CombatRenderer`** follow the same own-chain pattern (combat rendering plumbing only — the abilities system is off-limits).

So there are **two** grade paths today (world via `RenderSystem`; screen content via `PostProcessEffect`) and a **third** background render inside `FrostedGlass`. The reveal's ~50fps is `FrostedGlass` re-running the 596-line `pull_tunnel.glsl` 2–3× per frame.

## Decisions (locked with the user, 2026-05-27)

- **Per-screen render profile** decides which passes run for a given screen/state (chosen over preserving the current split or a forced global look change).
- **Profile = a composable capability table, NOT a closed enum of presets** (composition over enumeration — avoids the preset-explosion footgun). Named defaults survive only as optional spread-and-override shorthand.
- **Order static, enablement dynamic** — one canonical pass order in the Renderer; the active profile flips each pass's `enabled` per frame. Explicitly **not** a per-frame dependency compiler (the master spec's locked non-goal).
- **Dependencies handled by light normalization, not resolution** — a small `Profile.normalize` enforces sane implications; no input/output graph wiring engine.
- **Structural first (8-bit, parity-gated), then a separate linear/HDR + ACES flip** — the structural migration is screenshot-identical; the flip is the one deliberate look change.
- **Color scope: scene-only linear, UI stays sRGB** (drawn after tonemap — industry-standard seam). **Tonemap operator: ACES filmic.**
- Combat **abilities** system (kits/data/power/execution) is off-limits — rendering plumbing only.
- **Portal distortion is removed, not migrated** (user, 2026-05-27). The in-world portal pass in `RenderSystem` is already **dead code** — `systems/PortalSystem.lua` is a stub (`entities = {}`, `drawWorld()` a no-op), so the pass's `#PortalSystem.entities > 0` guard never fires. `ui/components/PortalDistortEffect.lua` (referenced only via `shader_registry`'s `portal_distort` factory) is retired. Removing both is visually invisible and sheds RenderSystem's `portalDistortCanvas`/`portalCaptureCanvas` + helpers, simplifying the overworld migration.
- **The `effects` pass hosts a composable *set* of named screen-space effects** (user, 2026-05-27) — not a single flag. `profile.effects` is a set like `{"frosted"}`; the pass runs when non-empty. `frosted` is the only live effect in 2c; `pixelate` (`ui/components/PixelateEffect.lua`) and any future distortion become first-class effects-pass ops when reintroduced. Portal distortion is **not** reintroduced.
- **Screen transitions are kept** (user, 2026-05-27), with a possible future rework. The transition-capture layer (`screen.lua` `_transitionCanvas`, per-screen copies in `character_select`/`weapon_select`, `ui_loader` `_contentCanvas`) coexists with the graph; `PostProcessEffect`'s save/restore-the-active-(transition)-canvas semantics MUST be preserved when its grade folds into the `post` pass. A transition/pipeline rework is out of 2c scope.

## Architecture

### Pass graph (full set; the active profile enables a subset per frame)

```
scene    active producer draws into sceneRT      -> sceneRT   (8-bit now; rgba16f-linear after the flip)
effects  screen-space sampling of sceneRT        <- sceneRT   (frosted blur, distortion)
post     PostFx grade                            sceneRT[+effects] -> sceneRT (ping-pong)
tonemap  (flip phase) ACES + sRGB encode          sceneRT -> sceneRT
present  blit/upscale sceneRT to the frame canvas (RenderScale) + AA resolve
ui       UI.draw() widgets on the frame canvas, AFTER present (sRGB, ungraded)
hud      FPS readout + warp flash                 (from 2b)
```

`present`/`ui`/`hud` keep the 2b ordering (widgets/HUD stay sharp and ungraded on the frame canvas after the AA resolve). `scene`/`effects`/`post`/`tonemap` slot in *before* `present`. The canonical order is fixed once in the Renderer; profiles never reorder it.

### Render profile (the hybrid: declarative config + one draw hook)

**Data (the mechanism):** each screen and the overworld declares a profile — a plain table:
```lua
{ scene   = "world" | "background" | "none",
  effects = { ... },   -- a SET of named screen-space effects on sceneRT, e.g. {"frosted"}; empty = none
  post    = boolean,   -- run the PostFx grade
  tonemap = boolean }  -- (flip phase) run ACES; ignored until the flip lands
```
Composable — any combination is valid; there is no fixed preset set, and `effects` is an open set so new effects (`pixelate`, future distortions) compose in without a schema change. **Named defaults** (`WORLD`, `MENU`, `PLAIN_UI`, `OVERLAY`) exist only as pre-filled tables a screen may spread and override (e.g. `MENU` is `{ scene="background", effects={"frosted"}, post=true, tonemap=true }`, override `effects={}` to drop frosting), so common screens stay one-liners. The profile lives next to a screen's existing `background` field and is data-drivable from `ui_loader` JSON.

**Hook (the imperative part — one method):** when the active profile's `scene ~= "none"`, the `scene` pass calls `producer:drawScene(ctx)` to fill `sceneRT`. That is the only method a producer implements. The overworld's `RenderSystem:draw` becomes `RenderSystem:drawScene` (renders the world into `ctx.targets`'s `sceneRT`, no longer owning `PostFx`/`RenderScale`); a frosted screen's `drawScene` paints its background once. UI widgets keep drawing through the existing `UI.draw()` in the `ui` pass.

**Per-frame profile selection (handles the screen stack):**
- `not UI.hasScreens()` → `WORLD` profile (producer = `RenderSystem`).
- Otherwise the Renderer walks the screen stack top-down and uses the **topmost screen whose profile is not `OVERLAY`** as the active profile; its `drawScene` fills `sceneRT`. `OVERLAY` modals above it contribute only widgets to the `ui` pass — so a modal over a menu keeps the menu's scene, exactly as today.
- The active profile's flags drive `Renderer:setEnabled` for `scene`/`effects`/`post`/`tonemap` that frame, so a `PLAIN_UI` screen pays for none of the scene pipeline.

**Dependency normalization (`Profile.normalize(p)`):** enforces sane implications so a bad combo cannot render garbage, headless-testable, ~a dozen lines:
- A non-empty `effects` set, `post`, or `tonemap` require `scene ~= "none"` → cleared (the effects set is emptied; `post`/`tonemap` set false; one-time warn) if set without a scene.
- Each name in the `effects` set requires a scene producer that wrote `sceneRT`; unrecognized effect names are dropped.
`Pass.inputs/outputs` (already on the descriptor) stay as documentation + an optional debug assert — not a runtime wiring engine.

### How the current paths reconcile

- `RenderSystem`'s internal `PostFx` grade and `PostProcessEffect`'s per-screen grade both fold into the **single** `post` pass, selected by profile. `PostProcessEffect` is retired as a separate capture/grade path (its UI-flavored defaults become a `MENU`-style profile's `post` config).
- `FrostedGlass` stops re-rendering the background; its `effects`-pass form samples `sceneRT` for the blur source. The reveal's 2–3× collapses to 1×.
- `RenderScale.upscale` moves into the `present` pass (one place), and its `midCanvas` moves onto the `RenderTargets` pool.
- The in-world portal distortion pass and `PortalDistortEffect.lua` are **deleted** (dead/retired — see Decisions), removing `RenderSystem`'s `portalDistortCanvas`/`portalCaptureCanvas`/`getPortalDistort*` and the `shader_registry` `portal_distort` entry. `PixelateEffect.lua` is reimplemented as an `effects`-set member when reintroduced (not in 2c-1).
- The **screen-transition capture layer** is left intact and coexists with the graph: the `ui` pass draws through the existing transition machinery, and the `post` pass preserves `PostProcessEffect`'s active-canvas save/restore so a mid-transition screen still grades into its transition canvas rather than the screen.

## Migration plan (structural, 8-bit, parity-gated, one producer at a time)

- **Stage 1 — Scaffolding (parity by inertness):** add the profile field (defaults reproduce today), `sceneRT` acquire infra, and the global `scene`/`effects`/`post`/`tonemap`(passthrough) pass slots — but configured so nothing routes through them yet; existing per-renderer chains run untouched. Identical because no producer is rerouted.
- **Stage 2 — Overworld:** first a **dead-code cleanup** (delete the in-world portal pass + `getPortalDistort*`/`portal*Canvas` + `PortalDistortEffect.lua` + its `shader_registry` entry — invisible, PortalSystem is a stub), then `RenderSystem:draw` → `drawScene(ctx)` into `sceneRT`; `WORLD` profile enables the global `post` (replacing internal `PostFx`) and `present` upscale (replacing internal `RenderScale`); RenderSystem sheds `ppCanvas`/`gradeCanvas`/internal post+upscale. Gate: overworld pixel-identical.
- **Stage 3 — Frosted screens (perf win):** screen backgrounds → `drawScene` into `sceneRT`; the `effects` pass samples `sceneRT` for the frosted blur; `FrostedGlass` stops re-rendering the background. Gate: menus/reveal identical **and** the profiler's heavy-shader / `draws` count drops (2–3× → 1×).
- **Stage 4 — `WaveGame` / `CombatRenderer`-plumbing:** same producer-split pattern (abilities untouched).

Each stage is screenshot-comparable to the prior commit and must not be worse on frame time before proceeding.

## Color flip (separate phase — the one deliberate look change)

Once all producers feed `sceneRT`:
- `sceneRT` becomes `rgba16f` **linear** (verify availability via `getCanvasFormats`).
- `tonemap` becomes a real **ACES filmic** pass (linear HDR → display, then sRGB-encode) as the final pre-`present` step.
- `PostFx` grade math is re-tuned for linear input (saturation/contrast/brightness/bloom).
- **UI stays sRGB**, drawn after tonemap — unchanged.
Gated by deliberate before/after screenshots + a mid-gray sRGB→linear→tonemap→sRGB round-trip sanity check (not parity). Tonemap operator is swappable (one shader function) if the look needs tuning.

## Decomposition into implementation plans

Each is shippable on its own and mirrors how 2a/2b each got their own plan:
- **Plan 2c-1:** profile system (`Profile` capability table + `normalize` + named defaults) + `sceneRT` infra + global pass scaffolding + the overworld migration (Stages 1–2).
- **Plan 2c-2:** frosted screens + remaining producers (`WaveGame`, `CombatRenderer`-plumbing) onto `sceneRT` (Stages 3–4) — kills the reveal 2–3×.
- **Plan 2c-3:** the linear/HDR `rgba16f` + ACES tonemap flip + `PostFx` linear re-tune.

## Components & ownership

| Unit | Responsibility | New / changed |
|---|---|---|
| `systems/render/Profile.lua` | Capability table type, named defaults, `normalize(p)` | New (render-coupled config; headless-testable) |
| `systems/render/Renderer.lua` | Per-frame profile selection + pass enablement; canonical order | Modify (add profile selection/apply) |
| `systems/render/passes/*` (or pass fns in `main.lua` initially) | `scene`/`effects`/`post`/`tonemap` | New pass bodies |
| `systems/RenderSystem.lua` | `:draw` → `:drawScene(ctx)` into `sceneRT`; sheds internal post/upscale | Modify (plumbing) |
| `ui/components/PostFx.lua` | Shared grade; invoked by the `post` pass | Reused; linear re-tune in 2c-3 |
| `ui/components/RenderScale.lua` | Upscale; invoked by `present`; `midCanvas` → pool | Modify |
| `ui/components/FrostedGlass.lua` | `effects`-pass form samples `sceneRT` (no bg re-render) | Modify |
| `ui/components/PostProcessEffect.lua` | Retired as a separate grade path (folds into `post`) | Removed/absorbed |
| `RenderSystem.lua` in-world portal pass | Dead (PortalSystem stub); distortion retired | Removed (canvases + helpers + draw block) |
| `ui/components/PortalDistortEffect.lua` + `shader_registry` entry | Retired portal distortion component | Removed |
| `ui/components/PixelateEffect.lua` | Reimplemented as an `effects`-set member | Deferred (not 2c-1) |
| `ui/core/screen.lua` + transition canvases | Screen-transition capture; coexists with graph | Unchanged (rework is a future option) |
| Screens + overworld | Declare a `renderProfile`; scene owners implement `drawScene(ctx)` | Modify per producer |

Combat **abilities** (kits/data/power/execution) are off-limits; only `CombatRenderer` plumbing is touched.

## Testing & verification

- **Headless (`tests/render_harness`):** `Profile.normalize` dependency rules; capability composition (spread + override a named default); Renderer pass enable/disable driven by a profile; per-frame profile selection from a fake screen stack (the top-non-`OVERLAY` rule). All pure logic, no GL.
- **GL / visual:** boot + visual + F9 profiler parity per migration stage (screenshot-compare to the prior commit). Stage 3 additionally asserts the heavy-shader / `draws` count drops (2–3× → 1×). The color flip uses deliberate before/after + the mid-gray round-trip sanity check.
- **Honest caveat:** true per-pass GPU time awaits the optional FFI timer-query backend; until then verification is CPU-time + draw-stats + visual (per the master spec).

## Risks

- **Producer migration regressions** → mitigated by per-stage parity gating (screenshot + profiler).
- **Screen-stack edge cases** (modals, transitions) → the top-non-`OVERLAY` selection rule + `PostProcessEffect`'s transition-canvas save/restore behavior must be preserved when folding into `post`; covered by the per-screen migration gate.
- **Linear flip changes the look** → isolated to its own phase, deliberate, before/after gated; UI seam keeps widgets stable.
- **`rgba16f` bandwidth** → budgeted by the profiler; render-scale helps; verify format availability first.

## Open items (resolve during planning)

- Exact named-default tables (`WORLD`/`MENU`/`PLAIN_UI`/`OVERLAY`) — derive from auditing current screens' actual post/frosted usage in Plan 2c-1.
- Whether `scene`/`effects`/`post`/`tonemap` pass bodies live inline in `main.lua` (like 2b) or move to `systems/render/passes/*` — decide in Plan 2c-1 by size.
- Per-pass budget values — set empirically once the passes exist.
- Screen-transition rework (kept-but-could-change): whether transitions eventually become a graph pass vs. staying the current capture layer — deferred; out of 2c scope, revisit after producers are unified.
