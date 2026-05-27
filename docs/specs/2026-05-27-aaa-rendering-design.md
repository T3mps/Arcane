# AAA Rendering Ecosystem — Design Spec

**Date:** 2026-05-27
**Scope:** GachaClient (Love2D / LuaJIT / OpenGL)
**Status:** Approved design, pending implementation plan(s)
**Structure:** Master phased spec — one architecture, six phases, each detailed enough to drive its own implementation plan. Ships phase-by-phase; the game stays playable throughout.

## Problem / motivation

The client has strong rendering *pieces* but no cohesive pipeline, no instrumentation, and IO on the render thread. Two concrete incidents motivated this:
- **150 fps flat, GPU-independent** — a per-frame blocking `socket:receive` (1 ms timeout × 3 service connections, rounded up to the Windows scheduler tick) stalled every frame. Found by grepping, not tooling.
- **50 fps in the pull reveal** — the reveal screen ran a 596-line `pull_tunnel.glsl` full-screen, and `FrostedGlass` re-rendered that same background into its blur source, so the heavy shader ran 2–3× per frame.

Both are symptoms of the same root causes: no frame orchestration, no profiler, uncached heavy passes, IO not isolated.

### Already solid (integrate, do not rebuild)
Unified camera (`systems/Camera.lua`), unified asset manager (`services/Assets` + `services/assets/*`), post-process grade (`ui/components/PostFx.lua` + `data/shader/post/post_process.glsl`), full-frame AA (`ui/components/FrameAA.lua`: MSAA/FXAA), render-scale + upscaling (`ui/components/RenderScale.lua`: bilinear/Lanczos/FSR1 EASU+RCAS), data-driven quality settings (`services/Settings.lua`), particles (`ui/particles/`), immediate-mode UI + data-driven screens (`ui/core/*`, `ui_loader`).

### Gaps this spec closes
1. No frame orchestration / render-graph (draw logic spread across `main.lua` `love.draw`, `FrameAA`, `RenderSystem` present, per-screen backgrounds, `PostFx`).
2. No profiler / GPU-timing overlay.
3. Blocking IO on the render thread.
4. No render-target/canvas pooling.
5. No explicit batching strategy.
6. Heavy full-screen background shaders re-rendered (often multiple times) per frame.
7. No 2D dynamic lighting.

## Decisions (locked)

- **Spec structure:** master phased spec (full).
- **Frame orchestration:** Approach A — a **pragmatic retained render-graph** (ordered pass list + pooled render targets + per-pass profiling), adopted incrementally. Not an async dependency compiler.
- **2D lighting:** full **normal-mapped** lighting (G-buffer, point/directional lights, soft shadows) — accepts the normal-map content-pipeline commitment.
- **IO threading:** full **worker thread** (network + heavy disk IO on `love.thread`; render thread exchanges plain-data messages via channels, never blocks).
- **Performance goal:** **hitch-free, no hard fps number** — zero frame spikes, per-pass budgets enforced by the profiler, fps otherwise uncapped.
- **Reusable primitives:** any general-purpose mechanism (ring buffer, pools, SoA buffers, scoped timer, channel envelope, dirty/interval memo) is extracted into a standalone, independently-testable module — never buried in its first consumer.

## Architecture

```
lib/                       -- engine-level reusable primitives (no LÖVE-render coupling)
  RingBuffer.lua           -- fixed-capacity circular buffer (rolling windows)
  Pool.lua                 -- generic keyed acquire/release free-list
  SoA.lua                  -- struct-of-arrays storage (FFI-friendly typed columns)
  ScopedTimer.lua          -- nestable begin/end timing -> durations
  Channel.lua              -- typed cross-thread message envelope + reqId correlation
  Memo.lua                 -- recompute-on-dirty-or-interval wrapper

systems/render/
  Renderer.lua             -- the frame graph: ordered pass list, executes a frame, drives present
  Pass.lua                 -- pass descriptor { name, inputs[], outputs[], enabled, draw(ctx), budgetMs }
  RenderTargets.lua        -- pooled canvas manager (specialization of lib/Pool) + MSAA/format policy
  Profiler.lua             -- CPU + GPU scoped timing (uses RingBuffer/ScopedTimer), overlay, budgets
  LightManager.lua         -- (P6) registered lights + shadow casters (uses SoA), feeds the lights pass
  passes/
    scene.lua  lights.lua  effects.lua  post.lua  ui.lua  present.lua

services/io/
  worker.lua               -- (P5) runs on a love.thread; owns sockets; blocking IO is fine here
  IOClient.lua             -- (P5) main-thread facade over channels; Network shims over this
```

**Frame context (`ctx`)** passed to every pass: `{ dt, time, camera, targets, profiler, settings, viewport }`. Passes read declared `inputs`, draw to declared `outputs`, make no global canvas-state assumptions.

**`Renderer.frame(dt)`** — ordered, not dependency-compiled:
1. `profiler.beginFrame()`; build `ctx`.
2. For each enabled pass in list order: acquire `outputs` from the RT pool, bind, run `profiler.scope(name) → pass.draw(ctx)` (pass reads `inputs` via `ctx.targets:get(k)`).
3. `present` blits the final target to the screen; `profiler.endFrame()` runs spike detection.

**Existing systems fold in as passes/params:** `present` = FrameAA-resolve + RenderScale-upscale + FXAA; `post` = PostFx; `effects` = FrostedGlass/distortion (sample `sceneRT`); `scene` = world/screen content (incl. `RenderSystem`); AA-mode and render-scale are present-pass parameters from `Settings`. Camera supplies transforms via `ctx`.

**Standard pass list** (passes skippable per screen profile):
```
scene    world/screen content        -> sceneRT (+ normalRT in P6) at render-scale resolution
lights   (P6) N·L accumulation       -> lightRT
effects  screen-space (frosted/etc.) <- sceneRT
post     PostFx grade (+ light composite) -> gradedRT
ui       immediate-mode UI on top
present  resolve MSAA + upscale + FXAA -> screen
```
Menu profile: `[scene(bg) → effects(frosted) → ui → present]`. Combat profile: full set incl. `lights`. Screens declare their profile.

## Reusable primitives (`lib/`)

Each is standalone, LÖVE-render-agnostic, unit-testable headless, and FFI-friendly (flat arrays, no per-frame table churn).

- **`RingBuffer`** — `new(capacity)`, `push(v)`, `at(i)`, `last()`, `avg()/max()` over the window. Consumers: profiler frame-time + per-scope history, FPS smoothing.
- **`Pool`** — generic keyed free-list: `acquire(key, factory)`, `release(handle)`, `frameReset()`. Consumers: `RenderTargets` (key = `{w,h,format,msaa}`), particle/object pooling.
- **`SoA`** — struct-of-arrays columns (`add{...}`, column accessors, dense iteration), shaped to back with `ffi.new` typed arrays later. Consumers: `LightManager` (pos/color/radius/intensity), particles.
- **`ScopedTimer`** — nestable `begin(name)/finish()` → durations; zero-cost when disabled. Consumer: profiler.
- **`Channel`** — typed message envelope `{type, payload, reqId}` + correlation map for request/response across `love.thread` channels (plain data only). Consumer: IO worker (P5); reusable for any future IPC.
- **`Memo`** — `recomputeIf(dirty | intervalElapsed, fn)` recompute-or-reuse wrapper. Consumer: `CachedPass` (P3), any throttled/derived state.

## Phase 1 — Profiler & instrumentation

Ships first; everything else is validated against it.

- **Scoped CPU timing:** `profiler.scope(name)` (built on `lib/ScopedTimer` + `lib/RingBuffer`). The Renderer wraps each pass; `main.lua` wraps the `guarded(...)` update sub-steps. A CPU stall (e.g. the blocking receive) surfaces as a labeled `network.update` marker over budget.
- **Draw stats:** `love.graphics.getStats()` per frame — draw calls, canvas/shader switches, texture memory (flags batching problems and repeated full-screen passes).
- **GPU time (honest):** LÖVE has **no native GPU timer query**. Default reporting = CPU-submission time + draw stats. True per-pass GPU time comes from an **optional FFI backend** (`GL_ARB_timer_query` / `GL_TIME_ELAPSED`) that slots in with the planned FFI pass without changing call sites; a debug-only `glFinish`-bracket mode gives coarse GPU wall-time on demand (off by default — it stalls).
- **Budgets + spike detection:** each scope has `budgetMs`; exceeding flags it (overlay highlight + throttled log). A frame-spike detector (frame time > rolling median × threshold) dumps the per-scope breakdown so regressions self-report.
- **Overlay:** toggle (key + Settings → "Show Profiler"): frame-time graph (RingBuffer, ~120 frames), per-scope ms table sorted by cost, draw-call/canvas-switch/VRAM counters, red flash on over-budget frames.
- **Zero-cost off:** `scope()` early-returns; ring buffers preallocated; no hot-path allocation either way.

## Phase 2 — Render-graph + render-target pool

- **`Pass`** is plain data + one `draw` fn (no inheritance).
- **`RenderTargets`** (over `lib/Pool`): `acquire{w,h,format,msaa}` returns a reused canvas. **Transient** targets (blur scratch, grade) auto-release at frame end; **persistent** ones (cached backgrounds, light buffers) live until invalidated. Single home for MSAA/format policy: **shader-sampled targets are `msaa=0`; only the final AA target is MSAA.** Recreates on resolution / `gfxGeneration` change. Replaces every ad-hoc canvas today (`ppCanvas`, `gradeCanvas`, FrameAA canvas, FrostedGlass ×3, RenderScale `mid`).
- **Incremental migration (always playable):**
  1. `love.draw → Renderer.frame`; pass list = a single `legacy` pass running today's exact sequence — identical output, now profiled.
  2. Peel off `present` (reads a `sceneRT` the legacy pass renders into) — verify visual + profiler parity.
  3. Then `post`, then `scene`, then add `effects`/`lights`. Each step small and parity-gated.

## Phase 3 — Heavy-shader / background caching

- **Structural de-duplication (free from P2):** once `scene` renders the background once into `sceneRT`, the `effects` pass **samples `sceneRT`** instead of re-invoking the bg shader — the reveal's 2–3× collapses to 1× with no caching logic.
- **`CachedPass` (over `lib/Memo`):** re-render heavy time-driven shaders into a *persistent* RT at a throttled rate (e.g. 30 Hz) and/or reduced scale (e.g. 50%), sample on intervening frames. Imperceptible for ambient backgrounds; cuts heavy-shader cost ~½–¾.
- **Per-background policy:** `static` (render once), `ambient` (reduced-rate + reduced-res), `interactive` (every frame). `invalidate()` on param/resolution change.

## Phase 4 — Batching (profiler-driven)

Only touches hotspots the profiler flags (draw-call / canvas-switch / shader-switch counters).
- **Atlases:** pack sprites/icons into atlases (extend the existing icon-bake registry into a packed atlas); `Assets` gains atlas-region lookup so shared-texture draws auto-batch.
- **SpriteBatch / mesh** for repeated geometry — tiles and especially **particles** (per-emitter batch, not per-particle draws).
- **State sorting** within a layer by (shader, texture, blendmode).
- **UI:** batch the worst offenders (e.g. panel backgrounds as one mesh) — not a UI rewrite.

## Phase 5 — IO worker thread

- **`services/io/worker.lua`** on a `love.thread` owns the auth/account/combat sockets and uses **blocking** select/receive (correct there — can't stall rendering). Parses the wire format (`LENGTH:TYPE|TOKEN|PAYLOAD`) into **plain tables** `{type, payload, token, reqId}` and pushes to an inbound `Channel`. Main pushes outbound requests to an outbound `Channel`.
- **Main-thread `Network`** becomes a thin shim over `IOClient`: `Network.update` does non-blocking `channel:pop` drains and dispatches the **same** callbacks/events as today (callbacks stay main-thread, keyed by `reqId`; the worker never runs game code). Only plain data crosses (string protocol already satisfies this).
- **Lifecycle:** start at boot; sentinel-message graceful shutdown on quit; worker errors pushed to an error channel + logged on main.
- **Disk:** the asset manager's async seam routes file-read + `ImageData` decode through the worker (decode is thread-safe; GL upload stays main-thread).
- **Supersedes** the recent non-blocking `RECV_TIMEOUT=0` fix — the main thread stops touching sockets entirely.
- Highest-risk phase → sequenced late, validated by the profiler (`network.update` marker → ~0).

## Phase 6 — Normal-mapped 2D lighting

- **G-buffer:** `scene` renders to **MRT** (`setCanvas{albedoRT, normalRT}`; shader writes `love_Canvases[0]=albedo`, `[1]=normal`). Sprites without a normal map get a flat `(0,0,1)`.
- **Asset pipeline:** `Assets` pairs normal maps by convention (`foo.png` + `foo_n.png`) or manifest override. **Content commitment:** lit sprites need authored normals.
- **`LightManager` + `lights` pass:** registered point/directional lights (pos, color, radius, intensity, falloff) in `lib/SoA`; per-light `N·L` against `normalRT` accumulates additively into a (optionally reduced-res) `lightRT`. View-culled; light-count budgeted by the profiler.
- **Soft shadows:** per-light **1D shadow maps** (occluders → polar depth buffer, PCF-style softening), with a simpler sprite-alpha occlusion fallback for cheap mode.
- **Composite:** `albedo × (ambient + lightRT)`; existing bloom in `post` blooms bright lit areas.
- **Settings (data-driven):** lighting on/off, light-buffer resolution, shadow quality.

## Sequencing & dependencies

```
P1 Profiler        (no deps)
P2 Render-graph+RT (needs P1 for parity verification)
P3 BG/shader cache (needs P2)
P4 Batching        (needs P1)            -- parallelizable
P5 IO worker       (needs P1)            -- independent of render; resequenceable
P6 Lighting        (needs P2 MRT + P1)   -- biggest; normal-map authoring can start in parallel
```
Critical path: **P1 → P2 → {P3, P6}**. P4/P5 only need the profiler.

## Verification

- **Profiler-gated:** per-phase per-pass `budgetMs`; gate = "no scope over budget, no frame spikes" across four reference scenes — menu, overworld, combat, pull-reveal.
- **Parity gating (P2 migration):** every peel-off step is visually identical (screenshot compare) and not worse on frame time before proceeding.
- **Headless harness** (same pattern as the asset-manager harness): unit-test the pure primitives + logic — `RingBuffer`, `Pool` acquire/release, `SoA`, profiler budget math, pass-list ordering/enable, **IO message serialization round-trip**, normal-map pairing. GL passes get a boot smoke + visual check (GL not unit-testable headlessly).
- **Threading tests:** request/response round-trip through channels; disconnect/reconnect stress; assert zero remaining main-thread socket calls.
- **Honest caveat:** true GPU per-pass time depends on the optional FFI timer-query backend (later); until then verification is CPU-time + draw-stats + visual.

## FFI throughline

Profiler ring buffers, RT-pool free lists, pass accumulators, and light data (`SoA`: positions/colors/radii) are flat preallocated arrays with no per-frame table churn — shaped to move into `ffi.new` typed buffers during the planned engine-wide FFI pass. The GPU timer-query backend *is* an FFI component. Channel messages are already plain data.

## Ownership boundaries

`systems/render/` + `lib/` own the new modules. Camera/Assets/PostFx/FrameAA/RenderScale/FrostedGlass integrate as passes/services. Combat `RenderSystem`/`CombatRenderer` become scene-pass producers — **rendering plumbing only; the abilities system (kits/data/power/execution) is owned by another engineer and is untouched** (rendering edits to combat renderers were previously authorized).

## Top risks

- **Graph-migration regressions** → mitigated by parity gating per peel-off step.
- **Threading races / serialization** → plain-data channels, callbacks on main thread, late sequencing, round-trip + stress tests.
- **Lighting content dependency** → normal maps must be authored; flat-normal default keeps unlit sprites working.
- **GPU-timing accuracy** limited until the FFI backend lands.

## Open questions (resolve during planning)

- Exact `lib/` location/namespace (proposed `lib/`; no existing convention — `core/` is an alternative).
- Per-pass budget values (set empirically once the profiler exists).
- Shadow approach final pick (1D shadow maps vs alpha-occlusion) — prototype both in P6, choose by profiler data.
