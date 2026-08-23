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

**Chrome paid for it at a much larger scale, and the outcome is the strongest
external evidence available for this choice.** Chrome's original headless mode
was, in Google's own words, *"a separate, alternate browser implementation"*
that *"didn't share any of the Chrome browser code in `//chrome`."* Maintaining
two implementations produced divergent codebases and behavioural
inconsistencies, and in **Chrome 112 the modes were unified** so headless shares
the browser's code; the old implementation survives only as a legacy
`chrome-headless-shell` binary.

That is precisely the rejected alternative here -- a separate `arcverify.exe`
harness that links the engine and drives scenes itself. Google shipped that
shape for years, paid for the divergence, and converged on "the real browser
with nothing displaying it," which is the same shape as "the real host with the
window never shown."
(https://developer.chrome.com/docs/chromium/new-headless)

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

#### Locators are descriptions, not captured ids

Playwright deprecates `ElementHandle` -- a direct reference to a node captured
at a point in time -- because it **goes stale** when the page changes. A locator
instead *"does not hold a reference to a DOM node. It holds a description of how
to find one,"* and is **re-resolved on every action**.

The same hazard exists here and is sharper: entity ids churn across scene
reload, and any step that reloads or re-parents invalidates a captured id
silently. **Scene targets are therefore addressed by a stable description --
name or hierarchy path -- resolved fresh at each step**, never by an id or index
carried across steps. A raw entity id remains available in probe *output* as an
observation; it is not an input an agent is expected to hold.

Resolution is **strict**, matching Playwright's strict mode: a description that
matches more than one entity is an error, not a silent first-match. Zero matches
and two matches are distinct, separately-reported failures -- a scene query that
quietly picks one of three candidates is exactly the confident-wrong-answer
failure this whole design exists to remove.

### Layer 3 -- interaction

`--script <path>`: a JSON step list executed at frame boundaries and injected via
`SDL_PushEvent`. Steps: `key`, `click`, `move`, `screenshot`, `probe`.

This tier exists because no combination of flags expresses *"then"* -- and the
checks that matter most need it. Proving F2a's per-instance normal matrix means
setting a non-uniform scale, probing two faces, and asserting their luma differs.

#### Actionability checks, and the one we already own

A naive `wait-frames` step is a sleep, and sleeps are how flaky suites are born.
Playwright replaces sleeps with **actionability checks** evaluated on a retry
loop before every action, and two of its five map directly onto primitives this
engine already has:

- **Stable** -- Playwright's definition is *"the element has maintained the same
  bounding box for at least two consecutive animation frames."* A **measured**
  settle test, not a duration. The analogue here is a target whose screen-space
  bounds are unchanged across two consecutive rendered frames.
- **Receives events** -- *"the element is the hit target of the pointer event at
  the action point,"* i.e. checking that an overlay will not capture the click
  instead.

> **That second check is `FrameDesc::pickPixel`.** The pick probe is not only a
> locator; it is the actionability check. Before a scripted click at (x,y), probe
> (x,y) and confirm the entity there is the intended target; a mismatch is a
> failed action, not a click sent into whatever happened to be on top.
> Playwright had to inject script into the page to obtain this. It already exists
> in our render graph, which is why the script tier is cheaper here than the
> analogy suggests.

`wait-frames` is therefore **deliberately removed** from the step list above:
`click` waits for *stable* + *hit-target*, and a step that needs to wait for a
condition names the condition. A `force` escape hatch skips the non-essential
checks, matching Playwright's, so a deliberate test of an obscured target is
still expressible.

Checks are **per-action**, not universal: in Playwright, `click` requires all
five, `fill` requires visible + enabled + editable, and `focus`/`press`/
`dispatchEvent` require none. The step list here should follow the same shape --
a key press needs no hit-target test, and demanding one would make keyboard
steps fail for no reason.

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

### Trace artifacts

A `--report` JSON is enough to know *that* a check failed. It is not enough to
know *why* without re-running -- and re-running is exactly what an agent is bad
at, because the failure may not reproduce.

Playwright solves this with a **trace**: a single `trace.zip` bundling
screenshots, DOM snapshots, the action log, console output, errors, and test
source, recorded `on-first-retry` by default and replayed in a viewer that shows
**the state before and after each action**.

That before/after pairing is the load-bearing part. A screenshot of the end
state shows a wrong result; a before/after pair per step shows *which step made
it wrong*.

**Requirement:** `--trace <path>` bundles, per script step: the step record, a
capture before and after, the probe results evaluated at that point, and any log
output emitted during it. It is a **bundle of existing artifacts, not new
instrumentation** -- captures, probes and the log already exist; the trace is
their correlation by step index.

Two adaptations, because our failures are not Playwright's:

- **The engine log belongs in the trace.** Playwright captures console output
  from the page; the equivalent here is the `[nri-graph]` / census / resolver
  log, which is where this engine actually explains itself. The census line
  alone (`1 resolved mesh(es), 1 bound mesh material(s)`) resolves a whole class
  of "why is nothing on screen."
- **Crash and hang reports belong in the trace too.** Diagnostics already
  auto-capture to `<exe dir>/diagnostics` on crash and hang. If a scripted run
  dies, the trace should carry the report rather than leaving an agent to
  discover a directory it was never told about -- the failure mode this design
  found in its own evidence base, where six weeks of driver faults sat in a log
  nobody was reading.

Recording is opt-in and off by default: it costs a capture per step, which is
the wrong default for a fast probe-only run.

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

### 3. Content animation, and a stabilisation detector

Fixing the clock makes *time* deterministic; it does not make *content*
deterministic. Playwright treats these as two separate problems and this spec
should too.

**Disable content animation by construction.** Playwright's `toHaveScreenshot`
defaults to `animations: 'disabled'`, which fast-forwards finite animations to
completion and cancels infinite ones. The analogue is a capture-time setting
that puts animated scene content into a defined state rather than whatever phase
the clock happened to land on -- the same move as the fixed timestep, applied to
content instead of time.

**Detect stability rather than assume it.** Playwright's own screenshot helper
*"took a bunch of screenshots until two consecutive screenshots matched, and
saved the last screenshot."* That is a cheap, general safety net for the
non-determinism a fixed timestep cannot reach: async resource loads, shader
compilation, cache warm-up -- all of which this engine does, and all of which
complete on wall-clock schedules the sim clock does not control.

**Requirement:** captures may be taken in *settle* mode -- repeat until two
consecutive rendered frames compare equal, up to a bounded retry count, then
fail explicitly if they never converge. Non-convergence is a reported fact, not
a silent last-frame-wins. This is the same instrument as the "stable" 
actionability check, applied to the whole frame instead of one target's bounds.

### 4. Golden-image gate

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

Two distinct properties are being asserted and the plan must not conflate them:

| Property | Comparison | Achievable |
|---|---|---|
| Offscreen matches windowed | perceptual cascade, cross-format | yes |
| Offscreen matches itself run-to-run | **bitwise** | yes -- same format, same path |

#### The comparison is a cascade with two independent knobs, not one threshold

A single scalar threshold is the wrong instrument, and Playwright's comparator
(`packages/utils/image_tools/compare.ts`) shows the better structure -- a
four-stage cascade, cheapest test first:

1. **Exact RGB equality** -- fast path, not a difference.
2. **Perceptual colour difference**: `colorDeltaE94(...) <= maxColorDeltaE94`,
   default **1.0**. The constant is **derived, not tuned**, and the call site
   says why: *"All dE* formulae are originally designed to have the difference of
   1.0 stand for a 'just noticeable difference' (JND)."*
3. **Local variance / flood-fill test** over a 3x3 window: *"if this pixel is a
   part of a flood fill of a 3x3 square of either of the images, then it cannot
   be anti-aliasing pixel so it must be a pixel difference."*
4. **SSIM** over a 31x31 window (`SSIM_WINDOW_RADIUS = 15`) averaged across
   R/G/B; `ssimRGB >= 0.99` classifies the pixel as antialiasing and does not
   count it.

And **two independent knobs**, which this spec previously collapsed into one:

| Knob | Question | Playwright default |
|---|---|---|
| per-pixel tolerance | is *this pixel* different? | `threshold` 0.2 (YIQ, pixelmatch) / `maxColorDeltaE94` 1.0 |
| aggregate tolerance | how *many* differing pixels are acceptable? | `maxDiffPixels` / `maxDiffPixelRatio`, **default 0** |

**Adopt the two-knob structure and prefer a derived per-pixel constant over a
tuned one.** This is a better answer to "default values are not measurements"
than the measured-threshold wording it replaces: a constant with a physical
meaning beats a number fitted to today's hardware. The `format=9` vs `format=11`
delta is exactly the small per-channel difference a perceptual test absorbs,
while a missing mesh or a wrong normal matrix produces structural differences
that survive all four stages.

Two details worth carrying over verbatim:

- **Size mismatch is a separate, named error.** Playwright pads both images to
  the larger size and reports `sizesMismatchError` *alongside* the pixel count
  rather than aborting. A dimension mismatch should be its own reported fact,
  never a crash and never silently rescaled.
- **SSIM antialiasing detection has known traps.** Playwright keeps a
  `julia-ssim-trap` fixture under `tests/image_tools/fixtures/should-fail/`
  precisely because SSIM can false-pass. The aggregate knob is what covers this;
  do not treat stage 4 as sound on its own.

Accuracy note for whoever implements this: Playwright's **default comparator is
`pixelmatch`**, not the SSIM/CIE94 one -- `(options.comparator ?? 'pixelmatch')`
in `packages/utils/comparators.ts`. The cascade above is opt-in there. We are
adopting the opt-in one deliberately, because cross-format comparison is exactly
the case a plain per-channel threshold handles worst.

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
- **The comparator needs its own fixtures, including should-fail ones.**
  Playwright keeps `tests/image_tools/fixtures/should-fail/` with a
  `julia-ssim-trap` case specifically because SSIM false-passes. A comparator
  tested only on pairs it correctly passes is untested in the direction that
  matters: an image comparison that never says no is indistinguishable from no
  comparison at all.
- **Locator strictness**: a description matching zero and one matching two are
  distinct, separately-asserted failures.
- **Actionability**: a target under an overlay must fail the hit-target check
  rather than dispatching the click, and `force` must bypass it.
- **Settle mode**: a scene that never converges must fail explicitly at the
  retry bound rather than silently returning the last frame.
- **Trace round-trip**: a deliberately failing script produces a trace whose
  before/after pair localises the failing step without a re-run. This is the
  only test of the trace that means anything -- the artifact exists to answer
  "which step", so the test must ask "which step".

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
- Determinism (fixed timestep + pinned layout + content-animation state +
  settle-mode capture) is a **prerequisite**, not a follow-up, and was not
  costed before this design.
- **This arc is large and the Playwright research grew it further.** The
  determinism contract, the comparator cascade, actionability checks, and the
  trace bundle are each real work. Sequencing them so the read-only half
  (`--offscreen` + probes + report + parity gate) lands and proves itself before
  the interactive half (script tier + actionability + trace) is the obvious
  wave split, and the plan should either take it or say why not.
- **The comparator is the one component with a genuine build-or-port decision.**
  The cascade is well-specified above and small, but it is real image-processing
  code -- CIE94, local variance, SSIM -- and it is the piece most likely to be
  under-estimated because the spec makes it sound like four `if` statements.
- Do a **spec -> plan coverage diff before task 1**, listing every requirement in
  this document and the task that owns it. F2a's plan was a lossy compression of
  its spec and nothing checked the compression, so twelve reviewers each
  correctly checking their task against the plan were structurally blind.
- Plan-supplied code is unrun code. F2a's prose was reliable; its code was not,
  four separate times.
- Rebuild before hunting. A stale binary cost most of a debugging session in F2a
  and produced six correct-but-irrelevant disproofs.

---

## Prior art

Playwright and headless Chrome were researched directly for this design rather
than cited from memory; the findings changed four sections of it. What was taken,
and from where:

| Borrowed | Source |
|---|---|
| Unified headless architecture, and why a separate implementation diverges | [Chrome: old vs new headless](https://developer.chrome.com/docs/chromium/new-headless) |
| The four-stage comparison cascade, dE94 = 1.0 as a derived JND constant, SSIM window radii | [`packages/utils/image_tools/compare.ts`](https://github.com/microsoft/playwright/blob/main/packages/utils/image_tools/compare.ts) |
| Two-knob tolerance, `maxDiffPixels` default 0, size mismatch as a named error, default comparator is `pixelmatch` | [`packages/utils/comparators.ts`](https://github.com/microsoft/playwright/blob/main/packages/utils/comparators.ts) |
| Actionability checks; "stable = same bounding box for two consecutive animation frames"; hit-target test; per-action check sets; `force` | [actionability.md](https://raw.githubusercontent.com/microsoft/playwright/refs/heads/main/docs/src/actionability.md) · [docs](https://playwright.dev/docs/actionability) |
| Locators as lazy descriptions; `ElementHandle` staleness; strict mode | [Locators](https://playwright.dev/docs/locators) |
| `threshold` 0.2 YIQ default, `animations: 'disabled'` default, screenshot stabilisation ("until two consecutive screenshots matched") | [LocatorAssertions](https://playwright.dev/docs/api/class-locatorassertions) · [Visual comparisons](https://playwright.dev/docs/test-snapshots) |
| Trace bundle contents and before/after-per-action replay | [Trace viewer](https://playwright.dev/docs/trace-viewer-intro) |

Two accuracy notes for anyone re-checking this: Playwright's default comparator
is `pixelmatch`, not the SSIM/CIE94 cascade adopted here (that one is opt-in);
and GitHub issue #24312, which reads like a team post-mortem on visual
comparison, is a third-party proposal that was **closed as not-planned** -- it is
evidence about the problem, not about Playwright's chosen solution.

The deeper lesson is the first row. Chrome ran a separate headless
implementation for years, absorbed the divergence, and unified. This spec's
central choice -- the real host with the window never shown, rather than a
purpose-built verification harness -- is the destination they arrived at the
expensive way.
