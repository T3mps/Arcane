# Agent Verification -- Offscreen Hosts and a Verification Surface -- Design

Date: 2026-08-23
Status: design, approved section-by-section; plan not yet written
Supersedes nothing. Follows F2a (`docs/plans/2026-08-22-f2a-scene-3d-vocabulary.md`).

---

## Why

Every visual verification in this engine currently terminates at a human.

F2a is the proof. Twelve tasks and a full whole-branch review shipped with the
normal-matrix fix's **shader half unverified**; it was only proven when the user
ran `ArcaneTests.exe "[gpu]"` by hand at midnight. Both of that arc's Important
findings lived in `MeshDocument::Draw()` -- the half no test reaches. Every
remaining arc (F2b, F3, F4, F5, Box3D) ends the same way unless something changes.

The reason it terminates at a human is not that the engine lacks the pieces. It
is that **an agent has nowhere to run GPU work.** Windowed d3d12 destabilises
under a remote/virtual display session, so GPU work was exiled to the physical
desk, and the desk is where the human is.

This design gives agents a place to run GPU work that is not a desk, not a
remote display, and not a shadow copy of the host.

---

## The evidence base

Two facts reshape the problem, and both were verified rather than recalled.

### 1. The driver crash is avoided, not fixed

The signature is `nvwgf2umx.dll +0x1203ce` (NVIDIA d3d12 user-mode driver),
recorded in Windows Application Event Log ID 1000. Distribution over the last
45 days on the dev box:

| Window | Events |
|---|---|
| 2026-07-09 21:51 -- 23:22 | 6 |
| 2026-07-10 11:56 | 1 |
| 2026-07-10 -> 2026-08-23 | **0** |

Seven events, one ~14-hour window, nothing since. It went quiet because the
*workaround* held -- from 2026-07-10 the standing rule was "run the GPU gauntlet
at the physical desk," and that is what happened. **The trigger was never
removed.** Machine context when it fired: `parsecd` + Parsec Virtual Display
Adapter + Virtual Desktop Monitor; `~[gpu]` was immune (no windows, no devices).

Recent faults on the same box are a different animal and are **ours**, not the
driver's: `2026-08-22` `ArcaneTests.exe` faulting in `ArcaneTests.exe +0x1808755`
(twice, 14 s apart); `2026-08-20 / 08-18 / 08-15` `ArcaneEditor` / `ArcaneRuntime`
faulting in `KERNELBASE.dll`; `2026-08-14` `ArcaneTests.exe` in `ArcaneClient.dll`.

Note also that the engine's own crash capture has **never** recorded this fault.
Every artifact under `bin/*/Arcane*/diagnostics/` dates from 2026-08-12 and is a
deliberate `--crash-gpu` capture whose stacks cite `nvrhi::d3d12::CommandList::open`
-- a renderer deleted in NRI Phase 5a. The instrument that caught the real fault
was the Windows Event Log, because the fault is inside the driver.

### 2. The engine can already render the real frame graph with no window

`NriGraphPixelTest.cpp:32-34` states it outright: `NriGraphContext::CreateOffscreen`
*"builds the real graph -- same nodes"* and reads pixels back through
`ReadCapture()`. The harness asserts `REQUIRE(v.ctx->IsOffscreen())` (`:132`) and
renders via `RenderFrameOffscreen` (`:168`). `NriGraphContext.hpp:70` records the
intent: offscreen mode is *"same `RenderFrame()`, same `Resize()`, same barriers."*

The editor already runs an offscreen graph half on the shared device for its
asset previews.

---

## Architecture

### The seam

**Offscreen mode is not "no window." It is a window that is never shown, and no
swapchain.**

`GpuContext` (`ArcaneClient/src/Arcane/Host/GpuContext.hpp`) owns the platform
stack and is explicit about what it does *not* own (`:19-24`):

> "there is **no render device, swapchain**, shader library, canvas or command
> list anywhere in this object"

and about the window's initial state (`:46-48`):

> "The window is created **HIDDEN**; the caller reveals it (`Window::Show`) once
> the render vehicle that owns this window's only swapchain exists."

The swapchain belongs to the render vehicle, not to `GpuContext`. The reveal has
exactly two call sites: `RuntimeApp.cpp:366` and `EditorApp.cpp:1392`. In the
runtime they are adjacent lines:

```
:366    m_gpu->Win().Show();
:368    m_graphContext = Arcane::NriGraphContext::Create(m_config, m_gpu->Win());
```

So offscreen mode is four changes:

1. `GpuContext::Create(cfg)` -- **unchanged**. The window comes up hidden as it
   already does.
2. **Skip the reveal** (two guarded lines).
3. Build the vehicle with **`CreateOffscreen`** (`NriGraphContext.hpp:614`)
   instead of `Create(cfg, window)` (`:566`). No swapchain is ever constructed.
4. Route `--screenshot` through `ReadCapture()` instead of the backbuffer.

### Why a real (hidden) window is kept

`GpuContext.hpp:11-14` and `:63` make the window's removal expensive and unsafe:

> "MEMBER DECLARATION ORDER IS THE TEARDOWN CONTRACT -- window first (destructs
> LAST: imgui/input hold SDL window refs) ... Do NOT reorder."

`Window m_window` is a by-value member (`:66`). Removing it means changing
`Win()`'s signature (`:51`), churning every consumer -- `RuntimeFrame.cpp:91`
pumps events off `io.gpu->Win()`, `EditorApp.cpp:379/1036` set the title -- and
breaking a contract explicitly marked do-not-touch.

Keeping a hidden window instead buys three things:

- `ImGuiLayer`, `InputDevices`, `InputActions`, `Batcher2D` and the event pump
  are **untouched**.
- **Input injection goes through `SDL_PushEvent`**, upstream of the real backend,
  so an agent drives the identical input path a human does.
- No swapchain means no present, so the crash path is **structurally absent**
  rather than merely avoided.

### `CreateOffscreen` is mandatory, not an optimisation

`RuntimeApp.cpp:353-365` explains why Show precedes Create today:

> "a surface created against a window the compositor has never mapped is exactly
> the kind of backend-specific corner a desk-only machine cannot pre-clear."

*Hidden window + ordinary swapchain* is therefore a known-bad shape. The design
creates no swapchain at all, which sidesteps it -- but the requirement is stated
here so a later simplification does not walk into that corner.

### What is deliberately not built

Not a second renderer, not a test-only scene loader, not a mock host. If an
agent can only reach a scene through a special mode, it proves nothing about the
editor a human uses. This is the rule F2a paid for.

---

## The agent-facing surface

### Naming

The mode flag is **`--offscreen`**, not `--headless`. The codebase already uses
"headless" to mean *device-less* (`SceneRenderResolver.hpp:211`: "this one CAN
report bound in a headless census"); this mode is *windowless but device-ful*.
`--offscreen` also matches the existing `CreateOffscreen` / `IsOffscreen()`
vocabulary.

### Layer 1 -- boot

`--offscreen`, composing with the existing `--project`, `--scene`, `--frames N`,
`--backend dx12|vulkan`, `--screenshot` (`HostConfig.cpp:9-37`).

`--backend` matters on every visual check: `mesh.hlsl` carries a `#if SPIRV`
split, so a single-backend run proves half a shader change.

### Layer 2 -- observation

Repeatable `--probe <kind>` or `--probe <kind>@<args>` plus `--report <path>`
writing one JSON file. The `@<args>` suffix is present exactly when the probe
kind takes coordinates: `--probe luma@640,360`, but a bare `--probe census`.
Supplying args to an argless kind, or omitting them from a positional kind, is
refused at parse time -- same treatment `--pick-probe` already gives a malformed
coordinate pair (`HostConfig.cpp:109-114`).

| Probe | Yields | Mechanism |
|---|---|---|
| `luma@x,y` | pixel intensity | `ReadCapture()`, as `[gpu][pixel]` already does |
| `rgba@x,y` | raw pixel | `ReadCapture()` |
| `pick@x,y` | entity id under the pixel | **`FrameDesc::pickPixel`** |
| `census` | 6-field resolution census | `SceneRenderResolver::Materials()` |

**`pick` must be driven by `FrameDesc::pickPixel`, not by the runtime's
`--pick-probe` flag.** `ArcaneEditor/src/main.cpp:92-98` records that
`--pick-probe` is *"PARSED AND INERT ON THIS HOST"* and that *"`NriGraphContext`
refuses to latch the flag on an offscreen (viewport) context."* The editor's own
pick already works offscreen on every click, through `FrameDesc::pickPixel`
(`EditorAppFrame.cpp:1643`). Driving that path is both the only thing that works
offscreen and the one that matches a human click.

`MaterialCensus` (`SceneRenderResolver.hpp:184-217`) is a 6-field POD --
`spriteReferenced`, `spriteBound`, `postReferenced`, `postBound`,
`meshReferenced`, `meshBound` -- so JSON serialisation is mechanical. It is only
logged as a string today (`SceneRenderResolver.cpp:423`).

The report carries `backend`, `mode`, `framesRendered` and `exitReason` alongside
probe results, so a `0` is always interpretable. This matters concretely:
`spriteBound` is compiler/batcher-gated (`SceneRenderResolver.hpp:211-214`) and
can read 0 for reasons unrelated to correctness.

Precedent for machine-readable emit-and-exit already exists: `--print-engine-info`
(`ArcaneRuntime/src/main.cpp:36`, `ArcaneEditor/src/main.cpp:122`).

### Layer 3 -- interaction

`--script <path>`: a JSON step list executed at frame boundaries and injected via
`SDL_PushEvent`. Steps: `key`, `click`, `move`, `wait-frames`, `screenshot`,
`probe`.

This tier exists because no combination of flags expresses *"then"* -- and the
checks that matter most need it. Proving F2a's per-instance normal matrix means
setting a non-uniform scale, probing two faces, and asserting their luma differs.

### Full-frame capture

`ArcaneEditor/src/main.cpp:87-88` records that `--screenshot` on the editor
*"captures the VIEWPORT panel's texture, not the editor window."* So the
Inspector, the asset browser, and every `Draw()` -- exactly where F2a's two
Important findings lived -- are invisible to a screenshot today.

In offscreen mode the composited frame including ImGui chrome becomes the
capture target, so this is specced as a **named deliverable**, not a side effect.
Two facts make it tractable: `ImGuiNri::Init` takes a device and pipeline cache,
**not a window** (`ImGuiNri.hpp:159`), and the editor sets only
`ImGuiConfigFlags_DockingEnable` (`EditorApp.cpp:404`) with **no
`ViewportsEnable`** -- so ImGui will never try to spawn OS windows for floating
panels.

### Three rules

1. **The engine emits facts; the agent asserts.** No assertion DSL in C++.
2. **Exit code means "did the host run,"** not "did the check pass." (The legacy
   `--pick-probe` exit-code convention stays as it is; it is a desk tool.)
3. **No flag is ever silently inert.** Honoured or refused, per host. Today
   `--pick-probe` and `--perf` are both "PARSED AND INERT" on the editor
   (`ArcaneEditor/src/main.cpp:92-100`) and exit 0 having done nothing -- which
   an agent reads as success. For an agent, silent inertness is the worst
   failure mode there is.

---

## The determinism contract

A verification surface without this produces confident, irreproducible answers --
worse than no surface at all. Neither half exists today.

### 1. Fixed timestep

`RuntimeApp.cpp:331` and `RuntimeFrame.cpp:154,176` all take
`std::chrono::steady_clock::now()`, and no fixed-step accumulator was found in
the host loop (the `acc*` names in `FramePerf.hpp` are perf counters, not sim).
So `--frames 5` advances simulation by *however long those five frames took* --
different on a loaded CI box than on an idle desk. Pixel assertions on anything
animated are flaky by construction.

**Requirement:** under `--offscreen`, the sim advances by a fixed delta
(`--fixed-dt`, default 1/60 s). Frame N is frame N regardless of machine load.

`--fixed-dt` outside `--offscreen` is **refused at parse time** (stderr + exit 2,
the idiom `HostConfig.cpp:64-68` already uses for `--screenshot` without
`--frames`), rather than accepted and ignored. Rule 3 admits no exceptions:
a flag that silently does nothing is the worst failure mode an agent can meet.

### 2. Pinned layout

`EditorApp.cpp:971` sets `io.IniFilename = m_layoutIniPath.c_str()`, described at
`:487` as "the PER-PROJECT LAYOUT FILE," and `:959` writes it back on exit. So
editor screenshots differ across machines **and every run mutates the layout**,
making run N+1 differ from run N. `EditorApp.cpp:492` already names this "the
imgui.ini veto class, which is the worst kind of bug."

**Requirement:** under `--offscreen`, `io.IniFilename = nullptr`, and nothing is
ever written back. Layout comes from a **single committed seed file tracked in
this repository** -- one canonical verification layout, loaded read-only via
`ImGui::LoadIniSettingsFromDisk`, shared by every agent run on every machine.
It is deliberately *not* the per-project layout file, which is user state.
Pinning precedent exists at `EditorApp.cpp:455`.

### 3. Golden-image gate

Offscreen must render the same *scene* as windowed. If they diverge, everything
built on this surface is confidently wrong. This is the easiest requirement in
the arc to forget and the most expensive to discover late.

**This comparison is tolerance-based, not bitwise, and the reason is structural:
the two paths do not share a surface format.** Last night's logs show the
windowed vehicle reporting `format=11` and the offscreen half `format=9`, at
identical 1280x720, on **both** D3D12 and Vulkan:

```
[nri-graph] ready: 1280x720 format=11 textures=3 ring=4096KiB/slot
[nri-graph] starting the graph render half OFFSCREEN: 1280x720 format=9 ...
```

So the gate is a per-pixel tolerance comparison with a stated threshold, in the
shape the `[gpu][pixel]` luma assertions already use, plus a dimension check --
and per the standing rule that default values are not measurements, the
threshold must be **derived from a real measured delta between the two paths**,
not picked and then declared to pass. Any pixel exceeding it fails the gate.

Two distinct properties are being asserted and the plan must not conflate them:

| Property | Comparison | Achievable |
|---|---|---|
| Offscreen matches windowed | tolerance, cross-format | yes, with a measured threshold |
| Offscreen matches itself run-to-run | **bitwise** | yes -- same format, same path |

Determinism pays off beyond agents: it makes the human desk pass reproducible too.

---

## Phasing

**Phase 1 -- the offscreen surface.** The real work. Building it first removes
the *need* for a crash fix, so it does not block on one.

**Phase 2 -- the crash, timeboxed.** Two deliverables:

1. A standing Event-Log detector -- one command that queries Application Error
   events for our executables and reports the faulting module, so a recurrence
   announces itself instead of being rediscovered by hand a month later.
2. One deliberate reproduction attempt: Parsec active, windowed d3d12 `[gpu]`,
   **three consecutive full-suite runs** at the desk. Three because the original
   cluster produced six faults inside 91 minutes -- a trigger that live would
   very likely show inside three suite runs, and one that does not is dormant by
   any useful definition.

Outcome is either *"reproduced, trigger characterised"* or *"not reproduced in
three runs, recorded dormant, detector in place."* A negative result is a valid
and expected outcome here, not a failure of the phase. Per the standing rule,
treat hypotheses as theories to disprove; read captured reports rather than
theorising.

Phase 2 must not block phase 1, and phase 1 must not wait on phase 2.

---

## Testing

- **Golden-image parity** -- offscreen vs windowed, both backends. The gate for
  the whole design.
- **Round-trip through the real wiring** -- probe parsing, report serialisation,
  and script parsing pinned in `ArcaneTests`, following the precedent set by
  `2003bb14` (which pinned `MeshDocument::Save -> MeshTable` through the editor's
  own wiring rather than a stand-in).
- **Both hosts, both backends.** A green `ArcaneTests` gate proves nothing about
  either host -- it compiles neither `EditorApp` nor `RuntimeApp`.
- **A determinism test**: the same command twice produces byte-identical
  captures.
- **`[mesh]` must be run as `"[mesh]~[gpu]"`** in the dev loop -- four cases
  carry `[gpu][pixel][mesh][nri]` tags and are real GPU tests.

Error handling: the report is always written when requested, including on
failure, carrying `exitReason`. The distinct "no readback landed" case already
has precedent and a clear message at `RuntimeApp.cpp:544`.

---

## Non-goals

- **Fixing the NVIDIA driver.** Not our code.
- **A lightweight remote-display environment** (replacing Parsec). Scoped
  separately, for human interactive use and the presentation-path desk pass. It
  is explicitly *not* agent infrastructure: a lossy H.264 stream corrupts exactly
  the luma values these assertions read, making it worse evidence than a direct
  readback, and confidently so.
- **Presentation-path verification** -- vsync, window resize, tearing, actual
  present. These need a real window at a real desk, by design. The division is:
  agents verify content and behaviour offscreen; the human verifies presentation
  at the desk. That is a far smaller desk pass than today's, where the human is
  the only instrument for everything.
- **F4's editor camera work** (separate editor camera, 2D/3D viewport toggle,
  grids, `.arcmesh` on the normal asset-create flow). Already scoped; the user
  directed explicitly that it not be bolted on early.
- **An in-editor AI assistant for game developers.** A different product. Build
  this surface regardless of whether that ships -- this is how you would validate
  it.

---

## Plan inputs

- The seam is four changes in two hosts; `RuntimeApp.cpp:366/368` are adjacent.
- The editor is the harder host and the more valuable one -- editor-side `Draw()`
  is where F2a's bugs actually were.
- Determinism (fixed timestep + pinned layout) is a **prerequisite**, not a
  follow-up, and was not costed before this design.
- Do a **spec -> plan coverage diff before task 1**, listing every requirement in
  this document and the task that owns it. F2a's plan was a lossy compression of
  its spec and nothing checked the compression, so twelve reviewers each
  correctly checking their task against the plan were structurally blind.
- Plan-supplied code is unrun code. F2a's prose was reliable; its code was not,
  four separate times.
- Rebuild before hunting. A stale binary cost most of a debugging session in F2a
  and produced six correct-but-irrelevant disproofs.
