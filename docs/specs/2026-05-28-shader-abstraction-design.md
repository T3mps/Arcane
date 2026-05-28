# Shader Abstraction (`Effect`) — Design

**Date:** 2026-05-28
**Status:** Brainstormed → spec'd. Plan to follow.
**Driver:** AAA rendering homogenization workstream — give the bespoke per-screen render pipelines a canonical type to converge onto.
**Cross-references:** `[[aaa-rendering-initiative]]`, `[[feedback_homogenized_rendering]]`, `[[asset_manager]]`, `[[feedback_abilities_system_ownership]]`, `[[project_inventory_party_rework]]`.

---

## Goals and non-goals

**Goal.** Give the bespoke per-screen render pipelines — `WaveGame.lua` (4-canvas bloom + 4-5 inline GLSL strings), `CombatRenderer.lua`, `reveal_item.lua` (parallel multi-pass), `glass_lattice.lua`, the inline frosted-glass-and-bgCanvas chain in `login.lua`, and the `weapon_select`/`character_select` transition draws — a canonical type to converge onto. Each is a small render pipeline today; the abstraction makes them one shape.

**The abstraction owns:**
- The Pass graph (ordered list of `{ shader, src, target, uniforms }`)
- Typed Params with defaults / ranges / labels that pass uniforms bind to via `@name`
- Canvas requirements declared as `{ scale, format, opts }`; resolved against `ctx.targets` per apply
- The `update(dt)` tick (timers, param animation) and the `apply(ctx)` call

**The abstraction does NOT own:**
- Where it sits in a screen's render order — the screen's `drawScene(ctx)` decides; `Effect`s are slotted in by callers
- Combat ABILITIES gameplay (boundary memory)
- The render-graph topology beyond a single Effect — `Profile`/`Layer`/`FrameCtx` still composes Effects with the rest of the scene
- Asset loading — delegates to existing `Assets.shader(key)`; shaders stay keyed by stem

**Migration mandate.** Every existing bespoke pipeline migrates to `Effect`. New effects added after this lands must use `Effect`. The `backgrounds` / `overlays` split in `ShaderRegistry` collapses — those were lifecycle hints for `ui_loader`, not effect-type distinctions; the new contract treats them as the same type with different placement.

**Explicit non-goal.** Unifying with `Profile` / `Layer` / `FrameCtx`. That layer composes the scene (producers + post). `Effect` is one step deeper — it's what a producer or a post step CAN BE. They stack; they don't merge.

---

## Type taxonomy and lifecycle

Four conceptual types; three are descriptors, one is a runtime object.

### `Effect` (runtime)

The only thing callers hold. Constructed once, kept for the screen's lifetime. Owns: the current param values, any internal state the `update(dt)` hook accumulates (timers, oscillators), and a per-pass last-known-good shader handle used only for the compile-failure fallback path (see Hot-reload). Effect does NOT primary-cache the shader handle — it calls `Assets.shader(passShader)` on each apply; that lookup is a constant-time cache hit handled by Assets. Stateless across the apply boundary — no canvases held between frames.

### `Pass` (descriptor, lives in `Effect.passes[i]`)

```lua
{ shader   = "<asset key>",                              -- resolved via Assets.shader at Effect.new
  src      = "scene" | "<canvasName>" | { name, ... },   -- input texture(s)
  target   = "<canvasName>" | nil,                       -- output (nil = caller's bound target)
  uniforms = { <uniformName> = value | "@param" | function(self) ... end },
  blend    = "alpha" | "add" | "multiply" | "replace",   -- default "alpha"
  color    = { r, g, b, a } | nil }                       -- default {1,1,1,1}
```

`src` / `target` reference names from the Effect's `canvases` table or the sentinel `"scene"` which resolves to the caller-supplied scene canvas. A `nil` target means the pass draws to whatever canvas the caller had bound when it called `apply` — that's how multi-pass effects "land" their composite in the screen's main target.

### `Param` (descriptor, lives in `Effect.params[name]`)

```lua
{ type    = "float" | "int" | "bool" | "vec2" | "vec3" | "vec4" | "color" | "texture" | "raw",
  default = <literal>,
  min     = <number>?,        -- numeric types only
  max     = <number>?,
  label   = "<UIEditor display>"? }
```

Param values are set via `effect:setParam(name, value)` (validates) or computed by the `update(dt)` hook. Passes bind to params with `"@name"` strings. A function-valued uniform is the dynamic escape hatch (e.g. `time = function(self) return self.t end`) — used sparingly; preferred path is "param + update tick advances it." `type = "raw"` bypasses validation for cases the basic types don't cover (uniform arrays, mat4, texture arrays); flag for adding to the type taxonomy when a real use case lands.

### `CanvasReq` (descriptor, lives in `Effect.canvases[name]`)

```lua
{ scale  = 1.0,                          -- relative to scene resolution (0.5 = half-res)
  format = "rgba8" | "rgba16f",
  opts   = { ... } }                     -- forwarded to love.graphics.newCanvas
```

The pool resolves to a concrete canvas at apply time and reclaims it when apply returns.

### Lifecycle

```
construct:  Effect.new{params, passes, canvases, update?}
            → validates schema, requires Assets.shader for each pass, fills state buckets

per frame:  effect:update(dt)        → advances animated params and self.t
            effect:apply(ctx)        → borrows canvases, runs pass chain, returns canvases

runtime:    effect:setParam(k, v)    → validate against schema + assign

hot-reload: Assets evicts shader     → next Assets.shader lookup recompiles
                                     → Effect picks up new handle on next apply, no callback

gc:         no explicit dispose; canvases are pool-borrowed, shaders are Assets-cached
```

---

## `apply(ctx)` contract and FrameCtx integration

The contract is "draw the configured pass chain through `ctx`'s resources, ending on whatever canvas the caller had bound when `apply` was called." Effect is well-behaved: graphics state in == graphics state out.

### What `ctx` must provide

```lua
ctx.sceneCanvas   -- Canvas. Resolves the "scene" sentinel in pass src/target.
ctx.targets       -- RenderTargets pool. Effect calls :checkout(reqs) / :checkin(borrowed).
ctx.width         -- number. Scene resolution for canvas sizing.
ctx.height        -- number.
```

This is the existing `FrameCtx` — no new fields beyond what W2c-1b / W2c-2 already shipped.

### What the caller does

```lua
love.graphics.setCanvas(myFinalTarget)   -- or default framebuffer = screen
love.graphics.setColor(1, 1, 1, 1)
effect:apply(ctx)
-- Caller's bound canvas + color + blend mode all restored on return.
```

The final pass (the one with `target = nil`) draws onto whatever was bound at apply entry. That's how the composite "lands" — the caller controls placement; the Effect only knows "draw through my chain to the current target."

### Inside `apply`, per pass

```
1. Resolve src:
   - "scene"     → ctx.sceneCanvas
   - "<name>"    → borrowed[name]
   - { a, b, … } → multi-source (a → main src drawn as quad,
                                  b… bound to additional sampler uniforms as src1, src2, …)
2. Resolve target:
   - "<name>"    → borrowed[name]; setCanvas + clear
   - nil         → leave caller's bound target alone (final composite pass)
3. setShader(passShader); send all resolved uniforms (literals, @param lookups, function evals)
4. push blend mode (Pass.blend or "alpha"); push color (Pass.color or {1,1,1,1})
5. love.graphics.draw(src, 0, 0, 0, scaleX, scaleY)  -- full-screen quad to current target
6. pop blend, pop color
After loop: restore caller's canvas / shader / blend / color.
```

### Multi-source uniform convention

`src = { "scene", "blurV" }` — first entry is the canvas drawn as the full-screen quad; remaining entries bind to additional sampler uniforms by index (`src1`, `src2`, …). The shader declares e.g. `uniform Image src1;`. This keeps the common single-source path zero-ceremony; the multi-source case stays explicit.

### Bg / overlay placement vs Effect

`Effect` knows nothing about "before widgets" vs "after widgets." That's the screen's `drawScene(ctx)` decision: a bg Effect's `apply` runs before `Widget.draw(self)`, an overlay's after. The Effect type is identical in both cases.

---

## ShaderRegistry fate and ui_loader integration

The two-table split (`backgrounds` / `overlays`) collapses; each becomes a tag, not a separate registry.

### New registry shape

```lua
ShaderRegistry.effects = {
  space_background = {
    factory = function() return require("ui.components.SpaceBackground").new() end,
    tags    = { "background" },
  },
  void_background  = { factory = ..., tags = { "background" } },
  post_process     = { factory = ..., tags = { "overlay" } },
  -- An effect that works as either:
  film_grain       = { factory = ..., tags = { "background", "overlay" } },
}
```

Each registered effect returns an `Effect` instance. The `tags` array is editor metadata only; runtime doesn't care.

### `ui_loader.lua` change is minimal

JSON-screen schema stays exactly the same:

```json
{
  "background": "space_background",
  "background_opts": { "theme": "default" },
  "overlay": "post_process",
  "overlay_opts": { "vignette_intensity": 0.4 }
}
```

`ui_loader` resolves `screen.background` → `ShaderRegistry.make("space_background")` → gets back an `Effect` → applies `background_opts` via `Effect:setParam(k, v)` for each key. Same for `overlay`. The `opts` table becomes "initial param values" — declarative param state from JSON, validated against the effect's schema at apply time. Existing `data/ui_screens/*.json` files stay compatible; no JSON migration needed.

### UIEditor introspection

```lua
ShaderRegistry.listByTag("background")    -- replaces backgroundNames()
ShaderRegistry.listByTag("overlay")       -- replaces overlayNames()
ShaderRegistry.paramSchema("post_process")  -- NEW — returns the Effect's params table
                                            --       editor renders sliders/color pickers from it
```

### Lua-constructed Effects skip the registry

`WaveGame`'s bloom, `reveal_item`'s parallel chain, `glass_lattice`'s effects are constructed inline by their widget modules via `Effect.new{...}` and held as widget fields. They're not addressable by name from JSON, which is correct — they're SCREEN internals, not user-facing effects. Registry stays small and editor-meaningful.

### `services/assets/shader.lua` is unchanged

Still does the GLSL compile + cache via `Assets.shader(key)`. `Effect.new` calls it once per pass at construction. Hot-reload operates at the Assets layer; Effect rebinds when notified.

---

## Hot-reload story

Shader hot-reload already exists at the Assets layer (QW-2): polls shader file mtimes at 4Hz, evicts cache on change. The Effect abstraction rides on top.

### The mechanism

At each `apply`, Effect calls `Assets.shader(pass.shader)` per pass. That's a hash lookup against the cache — constant-time, no IO. When the watcher in Assets evicts a key, the next lookup recompiles and returns a new handle. Effect picks up the new handle the same frame, no subscription, no callback.

### State preservation across reload

| State | Survives? | Why |
|---|---|---|
| Param values (`effect.params.*`) | ✅ | Live in Effect, not in shader |
| Internal timers (`effect.t`, oscillators) | ✅ | Updated by `update(dt)`, untouched by reload |
| Canvas requirements | ✅ | Declarative, in descriptor |
| Compiled shader handle | ❌ | New compile = new handle. That's the point. |
| Uniform "send" state | N/A | Re-sent every apply by name — works across new handle |

### Compile-failure fallback

When `Assets.shader(key)` returns `nil` after a broken edit, Effect falls back to a per-pass last-known-good cache and logs once. The screen keeps rendering with the previous frame's shader until the next save fixes the GLSL.

```lua
-- Inside Effect, per-pass shader resolution:
local sh = Assets.shader(pass.shader)
if sh then
  self._lastGoodShader[i] = sh    -- update cache on success
  if self._loggedFail[i] then self._loggedFail[i] = nil end   -- reset once-logged so next break re-logs
else
  if not self._loggedFail[i] or (love.timer.getTime() - self._loggedFail[i]) > 5 then
    log.warn("Effect '%s' pass %d shader '%s' compile failed; using last good", self.name, i, pass.shader)
    self._loggedFail[i] = love.timer.getTime()
  end
  sh = self._lastGoodShader[i]
end
```

The 5-second relog cadence catches deleted/renamed shader keys that would otherwise log once and silently stay broken.

### What does NOT hot-reload

Param schemas, pass topology, canvas requirements — anything declared in the Effect's Lua descriptor. Those require re-loading the Effect's Lua module (typically by re-opening the screen). Only the GLSL files themselves get live reload — that's the high-frequency edit loop during shader authoring.

### Cost

One hash lookup per pass per frame. With ~20 effects on screen each averaging 1.5 passes = 30 lookups/frame. Trivial.

### Latency

The 4Hz polling introduces up to 250ms between save and visible reload. Acceptable for shader authoring; tunable via one constant in `Assets.lua` if it ever feels laggy.

---

## Migration plan

Phased rollout. Each phase ends in a commit; each commit either ships green tests, a visual-gated screen change, or both. Order: primitive first, then cheap mechanical migrations, then the marquee bespoke pipeline, then the long tail.

### M0 — Build the `Effect` primitive (1 session, headless TDD)

**Files created:**
- `GachaClient/systems/render/Effect.lua` — the primitive: constructor, `update`, `apply`, `setParam`, schema validation, `@param` binding resolver, multi-source uniform handling, blend stack, compile-failure fallback
- `tests/render_harness/main.lua` — new test block, factory-injected fake ctx (mirrors the `SpriteBatch` / `MeshBatch` pattern). Covers:
  - Schema validation rejects bad params
  - `@name` binding resolves
  - Literal / function uniform values pass through
  - Canvas borrow/return calls match `ctx.targets:checkout` / `:checkin`
  - Multi-source binds correctly
  - Blend stack saves/restores
  - Compile-failure fallback uses last-known-good shader

Nothing else moves. Primitive exists, is tested, no consumers yet.

**Validation:** render harness green.

### M1 — Proof-of-life: migrate `SpaceBackground` (0.5 session, visual gate)

`SpaceBackground` is the simplest meaningful migration: single-pass animated background, single shader (`space_bg.glsl`), no canvas requirements, no shared-shader entanglement with `PostFx`. The wrapper module today is ~50 lines of `newShader` + `setShader` + `send` boilerplate around a few uniforms. Reshape into an `Effect.new{...}` declaration. If `Effect` can't host this without behavior change, the design is wrong.

**`PostProcessEffect` deliberately deferred to M4** because it shares `post_process.glsl` with `PostFx` (which owns the centralized uniform-upload path for both world and overlay consumers). Migrating it requires either bypassing `PostFx` (which breaks the singleton-shader / one-upload-path invariant) or a careful "Effect delegates `:send` to PostFx" handling. Either choice is bigger than a proof-of-life. (R1)

**Files:**
- `GachaClient/ui/components/SpaceBackground.lua` — internals become `Effect.new{params, passes={{shader="space_bg", uniforms={...}}}, canvases={}}`. Module's external API (`new`, `update`, `draw`) preserved; internals delegate to the Effect.
- `GachaClient/ui/shader_registry.lua` — registry entry for `space_background` switches to the flat `effects[name] = {factory, tags={"background"}}` shape. Other entries stay legacy during M1; `ShaderRegistry.make` discriminates by entry shape.

**Validation:** visual gate — the space background animates identically at login + at any screen that uses it (per-theme).

### M2 — Remaining single-pass background effects (1 session, batch migration)

Mechanical reshape of `VoidBackground`, `FiberBackground`, `WarpEnergy` into `Effect.new{...}` declarations. All single-pass; per-file migration is "lift inline GLSL/uniform/state code into the Effect descriptor, delete the wrapper-class boilerplate." Each commit per file.

**Validation:** per-file visual gate (open the screen that uses it, eyeball).

### M3 — `PullTunnel` + the WaveGame bloom marquee (1–2 sessions, the proving ground)

`PullTunnel` is multi-canvas. `WaveGame` is the 4-canvas bloom chain — the explicit reason this abstraction exists.

**Files:**
- `GachaClient/ui/components/PullTunnel.lua` — Effect with N passes
- `GachaClient/ui/screens/dailies/WaveGame.lua` — replace the inline 4-shader / 4-canvas pipeline with `local Bloom = Effect.new{...}` held as a widget field. The W1 GLSL extraction work landed earlier this session is the prereq; this is where it pays off.

**Before code lands:** sketch the WaveGame Effect descriptor on paper from the existing code and confirm every implicit dependency has a representation in `Pass` / `CanvasReq`. If anything doesn't fit (conditional passes, feedback-loop accumulation), the design grows that field before code lands. If during M3 it still doesn't fit, back out and revise — don't ship a half-fit primitive. (R2)

**Validation:** visual gate covering pull reveals (PullTunnel) + WaveGame dailies (bloom intensity unchanged, frame time unchanged).

### M4 — Long-tail bespoke pipelines + `PostProcessEffect` (1–2 sessions)

`reveal_item`, `glass_lattice`, `CombatRenderer` shader use, `weapon_select` / `character_select` transitions, and the deferred `PostProcessEffect`. Per-file commits.

**`PostProcessEffect` migration approach.** Since `PostFx` owns the singleton `post_process.glsl` shader + its centralized `send(settings)` uniform-upload path used by both world (`RenderSystem`) and overlay (`PostProcessEffect`) consumers, the Effect for `PostProcessEffect` does NOT bind the shader directly in its `apply`. Instead:
- Its pass declares `shader = "post_process"` only for hot-reload / dependency tracking; `apply` knows this shader is "PostFx-owned" via a sentinel field (e.g. `pass.delegate = "PostFx"`).
- For the actual draw step, `apply` builds a settings table from the resolved params and calls `PostFx.send(settings)` + `PostFx.draw(src, target)` (or equivalent) instead of `setShader + send + draw`.
- The "delegate" pattern is documented as a one-off — needed because `PostFx`'s singleton-shader-with-shared-upload design predates this abstraction and is itself a homogenization win we don't want to undo. If more shared-shader effects appear in the future, generalize then.

**Before touching `CombatRenderer`:** scope-check what's there — purely visual (post-process, screen tint, transitions) = plumbing, OK to touch. Ability-specific (per-element VFX, hit flashes, status overlays) = defer to the abilities owner. (R10)

**Validation:** per-screen visual gate. PostProcessEffect specifically validated by visual comparison at login (vignette intensity) + any screen with a strong color grade.

### M5 — Cleanup + audit close (0.5 session)

- `ShaderRegistry.backgrounds` / `.overlays` tables removed; the `effects` flat map + `tags` is the only shape left
- `backgroundNames()` / `overlayNames()` replaced with `listByTag(tag)`
- `paramSchema(name)` ships for UIEditor consumption
- `ui_loader.lua` switches to `make(name) + setParam` per option key
- Audit doc updated: `docs/audits/shader-homogenization.md` records before/after, the 16 `newShader` / `setShader` call sites reduced to N
- Memory: `project_aaa_rendering.md` gets a phase entry; `feedback_homogenized_rendering.md` references this as the canonical shape

**Validation:** assets harness syntax-checks new files; engine smoke; UIEditor dropdowns render new param schemas.

### What stays unchanged across the entire migration

- JSON `ui_screens/*.json` schema — `"background": "x", "background_opts": {...}` still works post-M5
- `Assets.shader(key)` API + the 4Hz hot-reload watcher
- `Profile` / `Layer` / `FrameCtx` / `RenderTargets` / `PostFx` external surfaces
- Combat ABILITIES gameplay (boundary)
- F2/F3 inventory/party menus (deferred per memory)

### Total budget

~5–6 sessions for full homogenization (M0: 1, M1: 0.5, M2: 1, M3: 1–2, M4: 1–2, M5: 0.5). Each phase is independently shippable; can pause after any of them and the codebase is in a consistent state.

---

## Risks and mitigations

Ordered by likelihood × blast radius.

### R1 — `PostFx` is the canonical grade hook; reshaping it must not break the canonical path

The render pipeline routes through `PostFx` for the final grade. `PostFx` owns the singleton `post_process.glsl` shader plus the one centralized `send(settings)` uniform-upload path used by both world (`RenderSystem`) and overlay (`PostProcessEffect`) consumers — that's itself a homogenization win we don't want to undo.

**Mitigation:** `PostProcessEffect` migration is deferred to M4 specifically so we don't make the proof-of-life carry this constraint. When M4 lands it, the Effect uses a "delegate to PostFx" pattern (see M4 above) — the Effect declares `shader = "post_process"` for hot-reload and dependency tracking, but its `apply` calls `PostFx.send` / `PostFx.draw` rather than binding the shader directly. PostFx's external API surface to `composeScene` / `composeScreensScene` stays unchanged. Verify by grepping callers across M4 and confirming zero call-site change.

### R2 — WaveGame bloom is the marquee migration; if it doesn't fit, the abstraction is wrong

The bloom chain has implicit ordering (extract → blur H → blur V → composite), source-target wiring, and a half-res optimization. The `Effect.canvases + src/target + scale` mechanism is designed for it but unvalidated until M3.

**Mitigation:** descriptor-sketching gate before M3 starts (see M3 above). If it still doesn't fit during implementation, back out and revise rather than shipping a half-fit primitive.

### R3 — `Effect` overlaps `Profile` / `Layer` / `FrameCtx` conceptually

Both express "draw this stuff with this shader to this target." A future reader could confuse them.

**Mitigation:** the §1 non-goal (Effect is one step deeper than Profile/Layer) lands in the docstring at the top of `Effect.lua`. Plus a one-pager `docs/architecture/effect-vs-profile.md` explaining the boundary: Profile/Layer composes the scene's structure; Effect describes a self-contained shader pipeline that fits INSIDE a producer or post step. They don't replace each other; they nest.

### R4 — Param schema rigidity for unusual uniforms

A shader using a uniform float array (`uniform float gradientStops[16]`), a `mat4`, or a texture-array doesn't fit the basic types in §2.

**Mitigation:** `type = "raw"` escape bypasses validation; value is passed straight to `shader:send`. Document as "use only for cases the schema doesn't cover; flag for adding to schema." Avoids ballooning the type taxonomy on day one.

### R5 — Compile-failure cascade when a `pass.shader` key is renamed or deleted

`Assets.shader(key)` returns `nil` forever; `_lastGoodShader` is stale forever; the effect renders permanently broken with no surfaced error after the first log line.

**Mitigation:** the once-logged flag uses a 5-second relog cadence (see Hot-reload section). Re-logs every 5s while broken — noticeable in dev, not log spam.

### R6 — `ShaderRegistry` tolerates two shapes during M1–M4

During the migration window, registry entries are either `{factory, tags}` (new) or bare `function(opts)` (legacy). `make(name, opts)` has to handle both.

**Mitigation:** explicit type-discrimination in `make`: `if type(entry) == "function" then ... else ... end`. M5 closes the dual-shape window. Document the dual-shape support as deprecated-on-arrival in M0.

### R7 — Instancing question deferred to M1

`ShaderRegistry.make("space_background")` — does that return a singleton, or a fresh `Effect`? Two screens using the same registered effect either share state (singleton) or don't.

**Mitigation:** M1's `factory` signature is `function() return Effect.new{...} end` — fresh instance per call. If a future use case wants singleton-shared state, add `Effect:clone()` and `factory = function() return SHARED:clone() end`. Defer; first real shared-state requirement reveals which is right.

### R8 — UIEditor introspection lands later

`paramSchema(name)` ships in M5 but UIEditor (the C++ ImGui tool) won't consume it until its own workstream.

**Mitigation:** call out as forward dependency in M5's commit message. UIEditor adopts at its own pace; nothing in M0–M5 blocks on it. Introspection just sits there until UIEditor wires it up.

### R9 — Per-frame `Assets.shader(key)` lookup × N passes × M effects

Trivial today, but a screen with 30+ active effects could add up.

**Mitigation:** Effect can cache the shader handle and only re-lookup when an `Assets.shaderVersion(key)` changes. Add only if profiling shows it matters. YAGNI for now.

### R10 — Boundary friction with combat ABILITIES

`CombatRenderer` shader use is in the M4 scope. The combat ABILITIES boundary memory says don't modify abilities gameplay; rendering plumbing is OK. The line can be fuzzy.

**Mitigation:** scope-check before edits — purely visual = plumbing; ability-specific behavior = defer to the abilities owner. Memory check before touching.

---

## Cross-references

- Master spec: `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md`
- Homogenization audit + W1/W2/W3: `docs/superpowers/specs/2026-05-27-rendering-homogenization-design.md`
- Asset manager (shader loader sits under it): `docs/superpowers/specs/2026-05-26-asset-manager-design.md`
- Memory: `[[aaa-rendering-initiative]]`, `[[feedback_homogenized_rendering]]`, `[[asset_manager]]`, `[[feedback_abilities_system_ownership]]`, `[[project_inventory_party_rework]]`, `[[ffi_optimization]]`

---

## Self-review

- [x] One polymorphic Effect type; multi-pass and single-pass collapsed into one shape
- [x] Canvas ownership is pool-borrow per apply; integrates with existing RenderTargets pool
- [x] Typed param schema with `type = "raw"` escape for the unusual cases
- [x] `apply(ctx)` contract is state-in == state-out; caller controls placement
- [x] ShaderRegistry's bg/overlay split collapses to tag-based metadata; JSON schema unchanged
- [x] Hot-reload rides on existing Assets QW-2 watcher; param state survives
- [x] Migration is 6 phases, each independently shippable; visual gates on every screen-touching phase
- [x] Marquee migration (WaveGame bloom) has a "back out and revise if it doesn't fit" gate
- [x] 10 risks enumerated with mitigations
- [x] Non-goals explicit; combat ABILITIES boundary respected; F2/F3 deferral respected
- [x] No new dependencies, no protocol changes, no JSON schema changes
