# C++23 Client Port — Scoping Study (analyzed with editor-engine-ification)

**Date:** 2026-06-10
**Status:** SCOPING — decision-support, no work authorized. Companion to the
`project_overworld_engine_arc` and `project_cpp_port_consideration` memory notes.
**Question:** Should the LÖVE 11.5 / LuaJIT client (`Client/`, ~217 .lua files,
~52.5K LOC under `src/`) be ported to C++23 — weighed *together* with the planned
"editor-engine-ification" (the C++ ImGui tools at `Tools/Editor/` becoming a full
game engine before/with the LevelEditor)?
**Method:** Direct evidence-gathering over the repo (PowerShell/ripgrep line counts,
per-subsystem LÖVE-coupling greps, full reads of the 5 test harnesses, the editor's
DX11/Win32 entry point + DataStore + UDP preview, and `Server/Common`). LOC are
`wc -l` raw line counts (comments + blanks included); coupling levels are
file-grep-derived (`love.*` references / total files in the subsystem). Numbers are
load-bearing where cited and were measured, not estimated, unless flagged "≈".

**The honest framing (user's own words):** *"a really bad choice now, I know that."*
This study confirms that instinct and quantifies *why*, then defines the trigger
conditions under which the answer changes. The deliverable is a tradeoff map, not
a recommendation to port.

---

## Executive Summary

**Primary recommendation: STAY on LÖVE now (Option C), with a deliberate "C++ core
later" on-ramp (Option B) gated on three concrete triggers.** A full port (Option A)
is the wrong move at this stage and stays wrong until the game is content-complete.

Three facts dominate the decision:

1. **The port surface is 80% UI, and UI is the part with no oracle and the highest
   LÖVE entanglement.** Of ~52.5K LOC in `Client/src`, the `ui/` subsystem alone is
   ~32.7K LOC across 104 files, of which **84/104 touch `love.*` and 82/104 touch
   `love.graphics` directly**. There are **2,669 `love.graphics` call sites across
   108 files**. This is the bulk of the work, it is the most entangled with LÖVE's
   immediate-mode batteries (Text, Canvas, blend modes, transforms), and **it is
   slated to migrate to declarative JSON authored in the editor anyway**
   (`project_ui_json_only`). Porting hand-coded Lua screens to hand-coded C++ screens
   would be re-implementing code that is on the chopping block.

2. **The easy-to-port parts are exactly the parts that don't need porting yet, and
   the hard parts have no test oracle.** The pure-logic subsystems — physics (2,479
   LOC, 1 LÖVE file = the debug-draw), combat (1,978 LOC, **0** LÖVE files), the world
   logic kernel (Iso/Map/MapLoader/Pathfinder/PathFollower/MovementController) — are
   genuinely portable and have **hash-identical determinism oracles** (the physics
   harness asserts `run() == run()` and cross-broadphase replay equality; the world
   harness asserts iso round-trips and pathfinding invariants). But these subsystems
   are also the newest, most-tested, and least-likely-to-change code in the client —
   porting them buys the least. The UI/render/animation/post-fx layer that *would*
   benefit from static types has **no oracle at all** and would be re-verified entirely
   "by feel."

3. **The C++ "engine core" you'd port *onto* does not exist yet.** The editor is
   ~25.3K LOC of **DX11 + Win32 + ImGui immediate-mode tooling** (`main.cpp` calls
   `D3D11CreateDeviceAndSwapChain` directly; no renderer abstraction, no audio, no
   game-loop, no scene/entity runtime, no GPU material/shader system, no text layout
   beyond ImGui's font atlas). `Server/Common` gives you a real `Protocol`/`TcpSocket`/
   `Types` layer to reuse (~8.2K LOC, genuinely shareable), but there is **no SDL3 /
   sokol / GLFW / bgfx anywhere in `ThirdParty/`** — the only windowing/GPU substrate
   vendored is LÖVE's own bundled `SDL2.dll`. A C++ client doesn't port onto an
   engine; it *builds* the engine first. That engine-build is the real cost, and it's
   the same cost the editor-engine-ification plan incurs regardless of the client.

**The editor-engine coupling is the actual decision axis, and it cuts toward "not
now."** The engine ambition does structurally favor C++ (one toolchain, editor
runtime == game runtime, shared scene/asset/material code). But "editor becomes an
engine" is achievable *without* porting the client: the engine the editor needs first
is an **authoring + level/scene runtime**, and the client can keep consuming the
JSON/scene data that engine produces (exactly the relationship that already exists for
`ui_screens`, `maps`, `protocol`, `input_actions`). Porting the client to share the
engine binary is the *maximal* coupling, not the *necessary* one. Doing it now would
freeze client feature work for months (Combat Sphere + LevelEditor are the roadmap)
to chase a benefit (code sharing) that the data-driven boundary already largely
delivers.

---

## Inventory Summary

### Subsystem map (LOC = raw `wc -l`; coupling = `love.*` files / total files)

| Subsystem | Files | LOC | LÖVE-coupling | `love.graphics` files | Oracle? |
|---|---:|---:|---|---:|---|
| `ui/` (screens, widgets, components, effects, core, hud, icons, particles) | 104 | 32,667 | **Entangled** (84/104) | 82/104 | **No** |
| `services/` (Assets, Input, gacha/quest/progression/weapon services, io, jobs) | 36 | 5,354 | Mixed (21/36) | 8/36 | Partial (assets harness) |
| `physics/` | 15 | 2,479 | **Pure-logic** (1/15 = PhysicsDebug) | 1/15 | **Yes** (determinism hashes) |
| `tests/` (5 harnesses) | 10 | 2,978 | thin (harness scaffolding) | n/a | (is the oracle) |
| `systems/` (render pipeline, camera, animation, particles) | 20 | 2,474 | Mixed (11/20) | 10/20 | Partial (render harness: profiler/ringbuffer/blobbatch math only) |
| `combat/` (turn-based: state, units, turn queue, damage, AI, status, elements, grid) | 8 | 1,978 | **Pure-logic** (0/8) | 0/8 | No (logic untested; Grid covered via world harness) |
| `world/` (Iso, Map, MapLoader, WorldRenderer, Movement, Pathfinder, PathFollower) | 9 | 1,039 | Thin (4/9; 3 are render/debug) | 3/9 | **Yes** (world harness) |
| `engine/` (Application, Bus, InputDispatch, bootstrap) | 4 | 617 | Thin (3/4) | 0/4 | No |
| `game/` (Game.lua — overworld glue) | 1 | 483 | Entangled (1/1) | 1/1 | No |
| `lib/` (RingBuffer, ScopedTimer) | 3 | 137 | Thin (1/3) | 0/3 | **Yes** (render harness) |
| `state/` | 1 | 34 | Pure | 0/1 | No |
| Root: `network_tcp.lua` + `network.lua` + `main.lua` | 3 | 2,279 | Mixed (network_tcp uses `love.thread`/`love.timer`; main uses graphics) | 1/3 | Partial (stream_buffer framing in threading harness) |

**Totals:** ~52.5K LOC, `love.graphics` referenced at **2,669 sites across 108
files**, `love.timer` in 31 files, `love.filesystem` in 25, `love.mouse` in 14,
`love.thread` in 6, `love.audio` in only 3.

### LÖVE-runtime surface a C++ replacement must provide

The grep distribution shows the replacement burden is **overwhelmingly graphics**,
with a thin long tail:

- **Graphics (the wall):** 2,669 `love.graphics` sites. This is immediate-mode 2D —
  `setColor`/`rectangle`/`draw`/`print`/`push`/`pop`/`setBlendMode`/`setShader`/
  `setCanvas` plus `newCanvas`/`newImage`/`newFont`/`newShader`/`newMesh`. Replacing
  it means a full 2D renderer: GPU device + swapchain (the editor has DX11), a
  sprite/mesh batcher, **render targets/canvases**, a **GLSL→HLSL shader story** (33
  `.glsl` files under `Client/data/shader/`), blend-mode state, transform stack, and —
  the quietly large one — **text layout + font atlasing with kerning/wrapping**
  (ImGui's atlas is not a game text system). The existing render pipeline
  (`systems/render/`, ~1.7K LOC: Pipeline, Renderer, Pass, Layer, SpriteBatch,
  MeshBatch, RenderTargets) is the Lua-side abstraction *over* `love.graphics`; it is
  a design to re-host, not throw away, but every leaf still bottoms out in `love.*`.
- **Threading:** `love.thread` + Channels in 6 files (network worker, job system,
  IO). Replaceable with `std::thread` + a lock-free queue; `Server/Common` already
  has `StripedMutex`/`Logger`/threading idioms to borrow. Small.
- **Filesystem:** `love.filesystem` in 25 files (mounting, `getDirectoryItems`,
  `getInfo`, `read`). Replaceable with `std::filesystem` (the editor's `DataStore`
  already does exactly this). Small, mechanical.
- **Input:** `love.keyboard`/`love.mouse`/`love.event` in ~15 files, but already
  funneled through `services/Input.lua` + `engine/InputDispatch.lua` + the
  data-driven `input_actions.json` (the Input system rebuild). The abstraction exists;
  only the backend changes.
- **Audio:** `love.audio` in **3 files**. Trivially small surface today (assets
  lacking), but a real audio backend (OpenAL/miniaudio/XAudio2) is net-new — there is
  none in C++ today.
- **Timer/window/system:** `love.timer` (31 files — but almost all `getTime`/`getFPS`,
  trivially replaced), `love.window`/`love.system` (3 each).
- **FFI already in use (the bridge that matters for Option B):** `ffi.cdef`/`ffi.new`
  at 17 sites across `physics/PhysicsWorld.lua` (full SoA body storage in
  `double[?]`/`uint8_t[?]` buffers — the FFI-mandate flagship), `lib/RingBuffer.lua`,
  `services/assets/cache.lua`, `world/Map.lua`. **The physics SoA layout is already
  C-struct-shaped**, which is precisely what makes Option B's boundary cheap there.

---

## Three Options, Honestly Costed

Estimates are in **person-weeks of solo-dev-with-Claude**, with ranges. "By feel"
means re-verified by running the game and looking, with no automated oracle. The
unit assumes sustained focus; calendar time is longer.

### Option A — Full C++23 Port (everything in use today, rebuilt)

**Runtime stack that replaces LÖVE:** The honest pick is **SDL3 (window + input +
audio + GPU abstraction) + a hand-rolled 2D renderer**, *not* "reuse the editor's
DX11." The editor's DX11 path is Win32-only and welded to ImGui's backend; the client
needs cross-vendor GPU, a real 2D batcher, canvases, and a shader pipeline that DX11-
direct doesn't give you for free. SDL3's GPU API (or sokol_gfx as a lighter
alternative) gives one shader target you cross-compile the 33 GLSL files to.
**Counter-pull:** if the engine-ification commits hard to DX11 (editor already is),
you'd unify on DX11 + a custom 2D layer and eat the Windows-only constraint — defensible
for a Windows-first game, and it lets editor + game literally share the renderer.
Either way **the renderer + text + audio substrate is net-new C++ and is the dominant
cost**, not the gameplay logic.

**Migration order (oracle-first):**
1. `Common`-reuse spike: protocol framing + `TcpSocket` + `Types` shared from
   `Server/Common` (already C++; ~0 new design). *Verified by:* existing Common tests
   + a round-trip against a running server. **~1–2 wk.**
2. Pure-logic core: physics + world kernel + combat logic. *Verified by oracle:* port
   the determinism + invariant assertions from the Lua harnesses to C++ (Catch2 is
   already vendored) and require hash-parity. **~3–5 wk** (physics ~easiest; combat
   logic has no current oracle so you write one as you go).
3. Engine substrate: SDL3/DX11 device, 2D batcher, canvas/RT, GLSL→target shader
   cross-compile, font atlas + text layout, audio backend, input backend, asset
   loader, job/IO threads. **~8–14 wk** — *the long pole, all net-new, mostly
   "by feel" + smoke tests.*
4. Render pipeline re-host (`systems/render` design onto the new backend) +
   world/combat *rendering* (not logic). **~3–5 wk, by feel.**
5. UI: 104 files / 32.7K LOC. Even assuming JSON-migration lands first and you only
   port the *runtime* (`ui_loader` + widget renderers + the 21 element types) plus the
   handful of bespoke shader-heavy screens, this is **~10–18 wk, entirely by feel**,
   and it's throwaway risk against the JSON-only direction.
6. Glue: Application/Bus/ServiceLocator, networking integration, end-to-end.
   **~2–4 wk.**

**Total: ~27–48 person-weeks (≈6–11 months solo).** Dominated by (3) the net-new
engine substrate and (5) the un-oracled UI. **Re-verified by oracle:** physics, world
kernel, protocol framing (~15% of the work). **Re-verified by feel:** everything
graphical, animated, audio, and UI (~70%+). **Feature freeze for the duration** —
this is the killer at pre-launch with Combat Sphere + LevelEditor unbuilt.

### Option B — C++ core + LuaJIT scripting (the "later" option)

**Shape:** Keep LÖVE as the shell/renderer **initially**; move the pure-logic hot/
heavy subsystems into a C++ core compiled as a LuaJIT-loadable library, called over
the **FFI boundary**. Candidates, in dependency order of cheapness:

- **Physics first.** It already stores bodies as SoA `ffi.new` buffers; a C++
  `PhysicsWorld` exposing a C ABI (`step`, `addBody`, `raycast`, `shapeCast`,
  accessors over the same SoA layout) is a near-mechanical FFI swap. **The Lua
  determinism harness becomes the cross-language oracle for free** — port nothing in
  the test, just point it at the C core and require the same hashes. This is the
  single highest-confidence port in the whole study.
- **World kernel + pathfinding** next (pure math, world harness as oracle).
- **Render *backend*** (batcher/canvas) is the expensive, low-confidence one — defer
  or never; LÖVE's renderer is its best battery.

**Boundary mechanics:** FFI structs for the SoA arrays (zero-copy — Lua reads/writes
the same `double[?]` the C core owns), a thin C ABI for control calls, and a build
that produces a `.dll` LuaJIT `ffi.load`s. **This composes directly with the existing
engine-wide FFI-optimization plan** (`project_ffi_optimization`) — that plan already
mandates FFI-friendly, low-GC, typed-buffer data layouts, which *is* the C-core
boundary. Option B is the FFI plan with the implementation language flipped from
LuaJIT to C++ for the subsystems where types/perf matter most.

**LÖVE's role:** stays as shell + renderer + audio + window. You get C++ types,
debugger, and one build for the *logic*, while keeping LÖVE's batteries and the
hot-reload loop for the *presentation*. The editor-engine plan can grow the same C++
core as its simulation runtime, so editor + game share the *engine math/sim*, not the
whole binary.

**Total to first useful slice (physics core behind FFI): ~2–4 person-weeks.**
Full pure-logic core (physics + world + combat sim): **~6–10 wk.** Dominated by the
build/ABI plumbing and FFI struct discipline, not the algorithms (which exist). **No
feature freeze** — it's incremental, subsystem-by-subsystem, each gated by an existing
oracle. **Risk:** a second toolchain in the loop (C++ build + Lua), debugging across
the FFI seam, and platform/ABI care for the `.dll`.

### Option C — Stay on LÖVE (status quo + hardening)

**Real growth risks, honestly:**
- **No static types at scale** — the genuine pain. 52.5K LOC of Lua with no compiler
  to catch a renamed field or a wrong-arity call; refactoring confidence comes only
  from the harnesses, which cover ~4 subsystems out of ~11. *Mitigable* with
  **LuaLS + type annotations** (EmmyLua) — buys most of the rename-safety without a
  port — and by **expanding the harness gate** to combat logic and services (currently
  unoracled). Not fully mitigable: you never get a true compiler.
- **GC pauses** — real for a 60 UPS action+combat game as entity/particle counts grow.
  *Mitigable* by the **FFI pass** (typed buffers, low churn) — the physics engine
  already proves the harness can assert "zero steady-state allocation after warmup."
  This is the FFI plan's whole point and it's working.
- **LuaJIT frozen at 5.1** — no modern syntax (`bit.*` not `|`, `math.floor` not `//`,
  no `string.pack`), and upstream LuaJIT is effectively maintenance-mode. *Not
  mitigable* — but also not currently biting; the codebase already lives within these
  constraints (`feedback_lua_jit_compat`).
- **Single-threaded Lua VM** — heavy sim can't trivially parallelize. *Partially
  mitigable* via `love.thread` workers (already used for IO/jobs) and, if needed,
  Option B's C core for the parallel-friendly bits.

**Cost: ~0 to start; ~2–4 wk for the hardening pass** (LuaLS annotations on the
public surface + harness expansion to combat/services). Keeps every battery, the
hot-reload loop, and the `lovec` harness gate intact.

---

## The Editor-Engine Coupling Analysis

This is the axis the orchestrator flagged as potentially "the tail wagging the dog."
Per option:

- **Option A** makes the editor and the game **share one C++ engine** — editor runtime
  *is* game runtime, the maximal-coupling end state and the cleanest "real engine"
  story. It is also the only option that justifies its cost *through* the engine
  ambition rather than through client needs. The trap: it forces the engine to be
  built *now*, on the client's critical path, before the LevelEditor that's supposed to
  motivate it even has requirements.
- **Option B** makes the editor and the game **share a C++ simulation/engine *core***
  (physics, world, combat math, eventually scene), while the editor keeps ImGui/DX11
  for authoring and the game keeps LÖVE for presentation. The shared thing is the part
  that genuinely benefits from being one implementation (sim correctness, determinism,
  perf). This is the **structurally honest middle**: the engine grows where engine-ness
  pays, the presentation layers stay specialized to their jobs.
- **Option C** means the "engine" is, today, a **Lua runtime that the C++ editor
  authors data for** (the existing `ui_screens`/`maps`/`protocol`/`input_actions`
  relationship). Is that "engine-ification"? *Partly.* The editor becoming a full
  authoring suite (LevelEditor, scene/prefab authoring, behavior graphs already exist)
  is real engine work that needs **none** of the client porting. What Option C's
  data-driven boundary *cannot* give you is editor-runtime == game-runtime fidelity
  (WYSIWYG that's the actual game code), which is the one thing a serious engine
  eventually wants.

**Which the engine ambition favors, and whether it should drive:** The ambition favors
**B now, A's end-state eventually** — *not* A now. The decisive observation: the
editor-becomes-engine plan's *first* deliverable is an **authoring + level/scene
runtime**, and that can be built as a C++ engine core (Option B's core, extended with
scene/asset/material) that **both** the editor consumes directly **and** the client
consumes via data export — exactly today's pattern, one level richer. The client port
is a *separate, later* decision about whether the game binary should *be* the engine or
*consume* it. **Letting the engine ambition force a client port now is the tail wagging
the dog**: you'd take on 6–11 months of client feature-freeze to pre-pay a coupling the
data boundary already approximates, before the LevelEditor that defines the engine's
real shape exists.

---

## Decision-Relevant Facts (not vibes)

1. **80% of the port is UI, and UI is on the migration chopping block.** 32.7K of
   52.5K LOC is `ui/`, 84/104 files LÖVE-entangled, **no oracle**, and slated for
   JSON-migration. Porting it to C++ re-implements throwaway code "by feel." This
   single fact sinks Option A's timing.
2. **The portable parts have oracles; the valuable-to-port parts don't.** Physics
   (determinism hashes) and world kernel (invariants) port with cross-language
   verification — but they're the newest, most-tested, least-volatile code, so porting
   buys the least. UI/render/animation/post-fx/networking-integration have **zero**
   automated oracle and would be re-verified entirely by running the game.
3. **There is no C++ engine to port onto — it must be built first.** Editor = 25.3K
   LOC of DX11+Win32+ImGui authoring tooling with **no renderer abstraction, no audio,
   no game loop, no scene/material/text-layout system**. `ThirdParty/` has **no SDL3/
   sokol/GLFW/bgfx** — only LÖVE's bundled SDL2. The net-new engine substrate (renderer,
   text, audio, shader cross-compile) is the dominant cost of any port and is the same
   cost the engine plan pays regardless.
4. **`Server/Common` is real, immediate reuse (~8.2K LOC).** `Protocol`, `TcpSocket`,
   `Types`, `Crypto`, `RateLimiter`, `SessionCache`, the row/persistence descriptors —
   a ported client reuses the wire protocol and types directly. This is the one place
   the C++ side is genuinely ahead, and it's exploitable in *all three* options
   (a C++ network core could back even the LÖVE client via FFI under Option B).
5. **The FFI bridge already exists and the physics SoA is C-struct-shaped.** 17
   `ffi.cdef`/`ffi.new` sites; `PhysicsWorld` stores all body state in `double[?]`/
   `uint8_t[?]` SoA buffers today. Option B's cheapest, highest-confidence move —
   swapping that one subsystem to a C++ core behind FFI with the existing determinism
   harness as oracle — is **~2–4 weeks** and composes with the already-planned FFI pass.
6. **What's lost vs gained is asymmetric at this stage.** *Lost on port:* LÖVE's
   renderer/audio/text batteries, the hot-reload preview loop, the `lovec` harness gate,
   and months of feature velocity at pre-launch with assets still lacking. *Gained:*
   static types, one toolchain, a real debugger, editor/game code sharing. At
   content-incomplete pre-launch with Combat Sphere + LevelEditor next, the losses are
   acute and immediate; the gains compound slowly and mostly matter post-content-lock.

---

## Recommendation

**Primary pick: Option C now (stay on LÖVE + a short hardening pass), with Option B as
the pre-planned on-ramp.**

Concretely, in priority order:
1. **Do the LÖVE-hardening pass (~2–4 wk):** LuaLS type annotations on the public
   surface of each subsystem + expand the `lovec` harness gate to **combat logic** and
   the **services** layer (the two biggest unoracled bodies). This buys most of the
   "static types for refactoring confidence" benefit at a fraction of a port's cost.
2. **Execute the planned FFI optimization pass in Lua** (`project_ffi_optimization`).
   It is the cheap, reversible mitigation for the one growth risk (GC) that a port
   would address, and it *is* the prep work for Option B's boundary.
3. **Treat the editor-engine-ification as an authoring + scene/sim engine that the
   client *consumes via data*, not *links against*** — keep widening the data boundary
   (the `project_ui_json_only` direction is exactly this). Build the LevelEditor against
   that engine. This delivers "real engine" progress with zero client porting.

**Concrete triggers that flip the recommendation to Option B (C++ core under LuaJIT):**
- **T1 — A measured, FFI-resistant perf wall** in a pure-logic subsystem (physics/
  combat sim) that profiling shows the Lua FFI pass cannot close (e.g. Combat Sphere
  scales bodies/AI past what a single LuaJIT VM sustains at 60 UPS). *First move:* port
  **physics only** behind FFI, oracle-gated. Low risk, ~2–4 wk, reversible.
- **T2 — The engine core needs to be one implementation for editor + game sim**
  (e.g. the LevelEditor must run the *actual* physics/combat sim for authoring fidelity,
  not a re-impl). Then the shared C++ sim core pays for itself across both binaries —
  build it as Option B's core, extended.
- **T3 — A cross-language determinism/anti-cheat or server-authoritative-sim
  requirement** that wants the same sim in C++ on server and client. `Server/Common`
  + a C++ sim core is the natural home; the client consumes it via FFI.

**Trigger that would justify Option A (full port) — and only this:** the game reaches
**content-complete / pre-launch-stable**, UI has **already** migrated to JSON, and a
deliberate, scheduled re-platform window exists with no competing feature work. Until
all three hold, a full port trades 6–11 months of pre-launch velocity for benefits that
mostly accrue *after* the window where velocity matters most. **Not now — and the
burden of proof is on a trigger firing, not on inertia.**

---

## Appendix — Test Oracle Detail (port-verification leverage)

The 5 `lovec` harnesses (`ThirdParty/love2d/lovec.exe Client/src/tests/<name>`,
exit 0 on pass) are the cross-language oracle inventory:

| Harness | LOC | Asserts | Cross-lang oracle strength |
|---|---:|---|---|
| `physics_harness` | 977 | Shapes/geometry kernel exact values; broadphase pair-set equivalence (hash vs SAP vs AABBTree); **`run()==run()` determinism**; **scripted-replay hash identical across broadphases**; bounded-population leak audit; raycast/shapeCast/CCD/LOS/joints/islands/sleep | **Strongest** — determinism hashes are language-neutral; a C++ port must reproduce the same hash. Gold-standard oracle. |
| `world_harness` | 495 | Iso projection round-trips (every cell center → its cell); Map flag accessors; MapLoader validation (version/length/entity rejection); WorldRenderer pure math (depth sort, tileset slicing); Pathfinder (4/8-dir, no corner-cut, unreachable, zero-GC); PathFollower; loads real `devtest.json` | **Strong** — exact-value + invariant assertions port directly. |
| `render_harness` | 1,111 | RingBuffer; ScopedTimer (injected clock); Profiler core (budgets/spikes/per-scope draw stats via injected `getStats`) | **Partial** — covers profiler/timing *math*, **not** actual rendering. No pixel oracle. |
| `assets_harness` | 263 | Syntax-check ~60 files; cache unit (refcount/evict/dropType); manifest resolution + ambiguity vs mock fs; real-fs shader-stem uniqueness; skyline packer; BlobBatch falloff profile | **Partial** — cache/manifest/packer logic ports; asset *loading* against a real GPU/fs does not. |
| `threading_harness` | 132 | `love.graphics` absence under worker conf; worker-safe modules require cleanly; **stream_buffer TCP framing** (`LENGTH:TYPE\|TOKEN\|PAYLOAD`, partial/glued/bogus frames) | **Targeted** — the protocol-framing assertions are a direct oracle for a C++ network port (and `Server/Common` already implements the same framing). |

**Oracle gaps a port would carry as "by feel" risk:** all UI rendering/layout/feel,
animations, post-fx/shaders (visual correctness), combat *logic* (no harness; only
`Grid` is covered transitively), networking *integration* (only framing is oracled),
audio, and input feel.
