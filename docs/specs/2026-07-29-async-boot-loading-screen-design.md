# Async boot + loading screen (BootSequence / BootPresenter)

**Status: design.** Today every Arcane host shows an undrawn window for the
whole of startup: `EditorApp::Run()` is `Init()` -> `MainLoop()`, the window and
device are created at the top of `Init()` (`GpuContext::Create`), but nothing is
presented until MainLoop's first frame. Everything in between -- project open,
asset-registry scan, font atlas, plugin `LoadLibrary` + `Init` -- runs behind a
black rectangle that also cannot be dragged or closed.

This arc replaces that with a real loading screen driven by a dependency-DAG
boot pipeline. Independent stages run concurrently (the filesystem-bound project
open overlaps the GPU resource chain), so the change is a wall-clock win as well
as a presentation one.

Three consumers, one mechanism: editor boot, `ArcaneRuntime` host boot, and the
in-editor project switch.

## 0. The stronger justification: a recurring bug class (added 2026-07-30)

The loading screen is the visible motivation. The *structural* one is bigger.

`EditorApp::Init` and `RuntimeApp` each hand-roll boot as a straight-line
function. Nothing forces them to agree, and divergence never fails loudly -- the
missing step degrades to "component not found" or "nothing drawn". Three bugs in
two days came from exactly this:

1. **Camera** was editor host state; the runtime had none (fixed @7b3b7f65 by
   making it a scene component).
2. **Sprite/material tables** -- `RenderSubmissionSystem` reads pre-resolved
   tables the HOST must publish per frame; the caches were editor-side only, so
   the runtime drew nothing textured.
3. **Astra TypeContext** -- `ArcaneRuntime.exe` never called
   `Astra::SetTypeContext` in its own module (Astra's context is a per-module
   static slot by design). The exe's private empty context assigned ids from its
   own counter, which ALIASED the shared ones: `TypeID<Camera>` collided with
   `Transform`'s id, so `CreateView<Camera>` returned every entity with a
   Transform and reinterpreted those bytes as a Camera. Not a miss -- a
   misread. Fixed @a6992da3.

`GpuContext` extracted the shared GPU prefix; the rest of boot stayed
duplicated. **This arc's real product is that the duplication becomes
impossible** -- see the CoreStages contract in section 2.

Note what this does NOT fix: a shared *mechanism* is not a shared *list*. An
earlier draft of this spec said "ArcaneRuntime registers a shorter list of the
same stages", which is the same divergence hazard in new syntax. The contract
below is what actually closes it.

```
main   |--runtime_create--|--gpu_core------|--fonts--|--splash--|--shell--|--plugin_load--|
worker                    |--project_open (scan)--------|--lock--|
                                                         ^
                                                         first presented frame;
                                                         window is HIDDEN until here
```

## 1. Components

Two new files in `Arcane/Arcane/src/Arcane/Host/`, plus one `Window` addition.

### `BootSequence.{hpp,cpp}` -- the scheduler

A stage DAG. Each stage declares a name, a weight, an execution mode
(`MainThread` | `Worker`), a failure policy (`Fatal` | `Optional`), and its
dependencies by name. `Run()` drives the whole boot to completion and returns a
`BootResult { bool ok; std::string failedStage; bool quitRequested; }`.

**This type has ZERO GPU, window, and ImGui dependency.** It is a pure
scheduler over callables plus a progress model. All presentation lives in
`BootPresenter`, which `BootSequence` calls through a small interface. See §7
for why that split is load-bearing.

Ordering is a stable Kahn topological sort -- the same shape as
`TopoSortPasses` in the material pass chain, so it reads like existing engine
code. Cycles are refused at `Run()` with the offending stage names.

**Why a DAG at all, stated so it is not cargo-culted later.** The graph exists
for exactly ONE overlap: `project_open` (filesystem) against `gpu_core` (device
creation). Absent that, ordered buckets would be simpler and sufficient -- which
is what UE actually uses: `ELoadingPhase` is an enum pumped by a flat `for` loop
(`ModuleDescriptor.cpp:741-764`) with no sort at all. UE *ships*
`Algo/KahnTopologicalSort.h` but applies it to cooking and blueprint
compilation, never to boot, and carries a `@todo` at `:751-754` conceding that
module load order is unverified. UE's boot is sequential and repeatedly JOINS on
background work rather than overlapping it (`WaitForAsyncTasks` at
`LaunchEngineLoop.cpp:2907`, `:3502`, `:4103`).

Take that as a caution: **every new Worker stage must carry its own
disjoint-ownership proof** (the one in §2 for `project_open` was verified, not
assumed). Sequential-and-join is the safer default; parallelism here is a
deliberate, narrow exception, not the house style.

### `ServiceThread.{hpp,cpp}` -- the one shape for blocking workers (added 2026-07-30)

**Why this rides in the boot arc.** This arc's thesis is "two things that should
be the same must not be able to drift apart." `BootSequence` is about to
hand-roll worker thread #2 (`ShaderCompiler` is #1, the deferred build service
will be #3). Letting each hand-roll its own start/stop/join/drain semantics is
the same failure as letting each host hand-roll its boot -- just in a different
dimension.

**Deliberately NOT a unification of the two threading models.** Arcane has two
genuinely different needs and they must stay different:

| Need | Mechanism | Rule |
|---|---|---|
| Fork-join compute (ECS iteration, physics) | `JobSystem` (enkiTS) | Workers must NEVER block. A blocked worker starves the pool -- `JobSystem.hpp` calls it "the only thread source for the simulation". |
| Long-lived BLOCKING service (compile, build, file I/O) | `ServiceThread` | Owns one dedicated thread. Never submit these to `JobSystem`. |

UE has exactly this split too (`FTaskGraphInterface` for fork-join,
`LaunchEngineLoop.cpp:2419-2429`, alongside dedicated `FRunnable` threads for
render, audio, async loading, and the splash). Collapsing ours onto one
mechanism would be a regression, not a cleanup.

**Scope: thread lifetime and queue plumbing ONLY.**

```cpp
namespace Arcane
{
    class ARCANE_API ServiceThread
    {
    public:
        explicit ServiceThread(std::string debugName);
        ~ServiceThread();                        // stop + join, always
        ServiceThread(const ServiceThread&)            = delete;
        ServiceThread& operator=(const ServiceThread&) = delete;

        void SubmitRaw(std::function<void()> work);
        [[nodiscard]] bool StopRequested() const noexcept;   // cooperative cancel
    };
}
```

Debounce, coalescing-by-key, superseded-drop, and last-good stay in
`ShaderCompiler` -- they are ITS policy, not everyone's. A build service likely
wants coalescing but not a 200ms keystroke debounce; `BootSequence`'s worker
wants neither. Extracting them from a sample size of one is how you get a wrong
abstraction; keep the base thin enough to be certainly right.

**ShaderCompiler is retrofitted onto it in this arc, and that is the point.**
Adopting the abstraction in its one proven consumer is the honest test of
whether the design is correct -- if `ShaderCompiler` cannot adopt it without
distorting, `ServiceThread` is wrong and we find out now rather than after a
third hand-rolled copy. The constraint is that ShaderCompiler's OBSERVABLE
behaviour must not change: debounce, coalesce keys, superseded-drop, last-good
survives a failed compile, `CompileNow`, and main-thread-only
`Submit`/`Poll`/`Drain` all stay exactly as they are. Its existing
`[shadercompile]` suite is the regression gate.

**Risk, stated plainly:** this couples a working, load-bearing compile service
to a boot arc. If the retrofit fights the existing debounce/superseded machinery
at implementation time, back it out and ship `ServiceThread` with
`BootSequence` as its only consumer -- the abstraction still earns its keep, and
`ShaderCompiler` retrofits when the build service provides a third data point.
Do not force it.

### `BootPresenter.{hpp,cpp}` -- the frame

Draws and presents exactly one loading frame, and owns nothing but the splash
texture. Per frame: `Swap().BeginFrame()` -> clear backbuffer -> ImGui frame
(centered logo, determinate bar, stage label) -> `Swap().Present()`.

It deliberately reuses the existing backbuffer + `ImGuiLayer` path rather than a
bespoke render chain (homogenized-rendering mandate), so it inherits the editor
theme for free and adds no new pipeline state.

Two modes:

- **Fullscreen** (boot): the whole backbuffer is the splash.
- **Overlay** (project switch): a modal panel over the last editor frame.

### `BootSplashWindow.{hpp,cpp}` -- the pre-device splash (added 2026-07-30)

**The gap this closes.** `BootPresenter` cannot present until `gpu_core`
finishes, because it renders through the real swapchain and ImGui. The original
draft called that harmless "because the window is hidden" -- but the actual
experience is *click, then ~1s of nothing at all*, which reads as a failed
launch. That is a regression against the very problem this arc exists to fix.

UE does not have this gap, and the reason is architectural: its splash is a
separate OS window on its own thread (`WindowsPlatformSplash.cpp:722` creates
the thread; the thread entry at `:407` registers its own `WNDCLASS` and
`WndProc`), shown at `LaunchEngineLoop.cpp:2913-2917` **deliberately before**
`FSlateApplication::Create()` at `:2920` -- the comment at `:2891` states it
must precede any window. It needs no graphics device because it blits a bitmap
with GDI.

We copy that shape:

- A plain OS window on its own thread, created at the very top of `main()`,
  before `BootSequence` and long before `gpu_core`. No device, no swapchain,
  no ImGui -- a static image (and, in the editor, one line of status text).
- Torn down by `splash_ready`, which is also the frame that calls
  `Window::Show()` on the real window. Ordering matters: show the real window
  *first*, then close the pre-device splash, so there is never a frame with
  neither on screen. UE gets this right in the game host by hiding its splash
  during the first `Tick` rather than at the end of `Init`
  (`GameEngine.cpp:1975`, on a `static bool bFirstTime`).
- **Windows-only to start.** Elsewhere it compiles to a no-op and boot behaves
  as the original draft described. This is a UX nicety, not a correctness
  requirement, and it must never be able to fail boot.
- It is NOT a `BootSequence` stage. It has to exist before the sequence does,
  and it must not participate in the DAG, the progress model, or the failure
  policies.

### `BootProgress` -- the cross-thread surface

The entire shared state between worker and main:

- `std::atomic<float>` overall fraction
- `std::atomic<int>` active stage index
- a mutex-guarded stage-detail string (e.g. `"Scanning content... 412 / 1180"`)

Worker writes, main reads once per frame. Nothing else crosses the boundary.

### `Window::Show()`

`WindowDesc::hidden` already exists (`Window.hpp:22`, used by tests) but there is
no way to un-hide, so hosts must currently create the window visible. Add a
one-line `Show()` over `SDL_ShowWindow`. Hosts now create hidden and show at the
first presented frame, which removes the black flash entirely -- the window
never exists in an undrawn state.

## 2. Stage table (editor)

`EditorApp::Init()`'s straight-line body decomposes into these registrations.
Weights are approximate shares of a typical boot and feed the determinate bar.

**CORE** stages come from `HostBoot::CoreStages()` and are shared verbatim.
**Editor** stages are appended by `EditorApp` only.

| Stage | Set | Thread | Depends on | Policy | Wt | Work |
|---|---|---|---|---|---|---|
| `runtime_create` | core | main | -- | Fatal | 5 | `TypeContext` + `Runtime` (enkiTS pool + engine component roster, `Runtime.cpp:114-121`). |
| `type_context_install` | core | main | `runtime_create` | Fatal | 1 | `Astra::SetTypeContext` in **this module's** slot + `HostBoot::VerifySharedTypeContext`. Bug-class instance 3 lived here. |
| `project_open` | core | **worker** | `runtime_create` | Optional | 45 | `Project::Open` -> `ScanContent` -> ABI gate -> content root -> asset resolver -> config layering (`Runtime.cpp:385-420`). |
| `gpu_core` | core | main | -- | Fatal | 25 | `GpuContext::Create`: window (hidden) / device / swapchain / shaders / canvas / batcher / tonemap / ImGui / input / command list. |
| `render_bridge` | core | main | `gpu_core`, `runtime_create` | Fatal | 3 | `SetRenderResources`, `OffscreenImGuiLayer::Create`, `SetImGui`. |
| `input_config` | core | main | `project_open`, `gpu_core` | Optional | 2 | `HostBoot::LoadInputConfig`. |
| `sprite_tables` | core | main | `project_open`, `render_bridge` | Optional | 2 | Publish `SpriteTable` / `SpriteMaterialTable`. Bug-class instance 2 lived here. |
| `plugin_load` | core | main | `project_open`, `render_bridge` | Fatal | 9 | `PluginHost` emplace + `AddPlugin` + `Load`. |
| `editor_fonts` | editor | main | `gpu_core` | Fatal | 5 | Theme + `InstallEditorFonts`. Must precede the first presented frame -- see below. |
| `splash_ready` | editor | main | `editor_fonts` | Fatal | 2 | Load splash texture, `Window::Show()`, present frame 1. |
| `editor_shell` | editor | main | `editor_fonts` | Fatal | 3 | Title, icon, toolbar logo, docking flag, layout settings, console sink. |
| `editor_lock` | editor | worker | `project_open` | Optional | 1 | `EditorLock::Write`. |
| `finalize` | core | main | all | Fatal | -- | `UpdateWindowTitle`, hand off to `MainLoop`. |

`sprite_tables` and `type_context_install` are listed as core stages because
those are the two places bug-class instances 2 and 3 actually lived; confirm
against the shipped fixes (@a6992da3 and the sprite-lift work) and fold in
whatever those settled on rather than re-deriving them.

### The CoreStages contract -- what actually kills the bug class

```cpp
// Arcane/Arcane/src/Arcane/Host/HostBoot.hpp
// The canonical boot sequence. BOTH hosts take this list whole. A host may
// APPEND its own stages; it may not omit, reorder, or rewrite one. Divergence
// therefore has to be written deliberately -- it can no longer be forgotten,
// which is exactly how the camera / sprite-table / TypeContext bugs happened.
[[nodiscard]] std::vector<BootStage> CoreStages(BootContext& ctx);
```

Usage is deliberately asymmetric-looking but identical in substance:

```cpp
// EditorApp
auto stages = Arcane::HostBoot::CoreStages(ctx);
stages.push_back(EditorFontsStage(ctx));
stages.push_back(SplashReadyStage(ctx));
stages.push_back(EditorShellStage(ctx));
stages.push_back(EditorLockStage(ctx));

// RuntimeApp -- appends nothing. That is the point.
auto stages = Arcane::HostBoot::CoreStages(ctx);
```

Enforced by test (§8): build both hosts' stage lists and assert every id in
`CoreStages()` appears in each. A future engine-wide install step added to
`CoreStages` is then automatically in the runtime host too, and a host that
tries to drop one fails the gate.

The project switch registers a four-stage list (§5) and is deliberately NOT
`CoreStages` -- it runs against an already-booted process.

### `ArcaneRuntime` gains the ABI gate it never had

Separate from the stage work, and found during the same survey:
`RuntimeApp.cpp:113-115` treats a failed `OpenProject` as a warning and falls
through to the `data/` + `--plugin` fallback, so an ABI-stale project **silently
boots the wrong content** where the editor at least shows a modal. The runtime
must refuse with a clear message instead. Same bug class -- the runtime host
quietly doing less than the editor.

(Related, for the follow-on arc rather than this one: the Hub maps editor exit
code 2 to "the editor refused the project (engine/abi gate)" at
`launch.rs:273-274`, but `main.cpp:89`'s only `return 2` is the unrelated
`--frames`-with-no-project case. That error path is currently unreachable.)

### Thread assignment rationale

`project_open` is the only Worker stage of consequence, and it qualifies because
it is pure CPU + filesystem: it touches `Runtime` state and the `Assets` facade
(`SetContentRoot` / `SetAssetResolver`) and nothing else.

The overlap is safe by **disjoint ownership, verified not assumed**: while
`project_open` runs, the concurrently-eligible main stages (`gpu_core`,
`editor_fonts`, `splash_ready`, `editor_shell`) touch only `GpuContext`, ImGui,
and free functions. `InstallEditorFonts` builds an ImGui font atlas from files;
`LoadDisplayTexture` is a free function taking the nvrhi device directly. Neither
goes through the `Assets` facade that `project_open` mutates.

### Why `editor_fonts` precedes `splash_ready`

The splash label needs a font, and the only font available before
`InstallEditorFonts` is ImGui's stock built-in. Installing the real fonts
afterwards would rebuild the font atlas and re-upload its texture *while
presented frames are already in flight* -- a needless hazard against the
ImGui-NVRHI backend's texture lifetime, and a visible font pop on the splash.

Ordering fonts before the first present costs nothing (they are read from disk
either way) and makes the splash render in Roboto from frame one. The atlas is
then built exactly once per boot. Do not reorder these two.

Everything with genuine thread affinity is `MainThread` and chunked instead: the
SDL event pump and window, ImGui's single context, plugin `LoadLibrary` /
`Init` under the loader lock, and every NVRHI call.

`runtime_create` stays on main (it installs `Astra::SetTypeContext` into
`Arcane.dll`'s per-module slot and spins the enkiTS pool) and is cheap enough
that running it first to unblock the worker costs nothing.

### Scheduling policy

1. **Ready worker-eligible stages dispatch first.** This is what produces the
   overlap: main clears the cheap `runtime_create`, which frees `project_open`
   to the worker, and main then spends the next second in `gpu_core` while the
   scan runs.
2. **Among ready main stages, registration order wins** (stable Kahn).
   `splash_ready` is registered immediately after `gpu_core` so pixels appear at
   the earliest possible moment.
3. **One main stage per frame, then present.** Stages reporting sub-progress are
   chunked across frames so the animation never stalls.

One stage cannot honor rule 3: `gpu_core` is a single monolithic call and no
frames are presented during it. This is harmless *because* the window is hidden
until `splash_ready` -- the user sees nothing, then the splash, which is the
Unity/UE behavior. Do not "fix" this by showing the window earlier.

A single worker thread runs at most one Worker stage at a time. Two Worker
stages are never co-scheduled, so worker-vs-worker interleaving does not exist.

## 3. Progress model

Overall fraction = (sum of completed stage weights + active stages' partials) /
total weight. Because main and worker advance concurrently the bar moves from
both, which is honest and monotonic.

The label shows whichever active stage reports sub-detail, else the
highest-weight active stage.

`AssetRegistry::ScanContent` gains a **defaulted** progress-callback parameter
so every existing call site is untouched. Reporting `"412 / 1180"` needs the
denominator up front, which means a cheap `directory_iterator` count pass before
the real scan. That count pass is the one added cost in this design; it is
stat-only with no file reads.

## 4. Failure, abort, and quit

Two failure policies exist because today's boot already treats stages
differently, and this arc must preserve that exactly rather than tightening it
by accident.

**Fatal** (`gpu_core`, `render_bridge`, `plugin_load`, `runtime_create`):
log, abort the sequence, join the worker, return `ok = false`. `Run()` exits 1.
Dependents never run. Matches `EditorApp.cpp:93-97`, `211-215`, `253-257`.

**Optional** (`project_open`, `editor_lock`, `input_config`): the failure is
recorded and **dependents still run**, in a project-less state. This preserves
current behavior precisely -- `OpenProject` failing today is not fatal; it warns,
sets `m_projectOpenError` for first-frame surfacing, and continues with the
`data/` + `--plugin` fallback (`EditorApp.cpp:179-187`). `plugin_load` already
handles an empty `gameModule` (plugins-only or plugin-less host).

Two paths that do not exist today and come free with the loop:

- **Quit during boot.** The event pump runs every frame, so closing the window
  mid-load sets `quitRequested`; the sequence aborts, joins the worker, and the
  host exits 0 without entering `MainLoop`.
- **Worker exceptions** are caught at the stage boundary and converted to a
  stage failure. Nothing crosses the thread boundary unhandled.

**Scripted runs:** `--frames N` counts `MainLoop` frames, and splash frames are
presented before `MainLoop` starts, so the headless GPU-verify budget is
untouched. This gets an explicit test (§8) -- silently consuming frames would
corrupt every scripted run and every CI lane that depends on them.

### `ConsoleBuffer` thread safety (required, not optional)

`ConsoleBuffer.hpp:11-15` documents exactly this hazard: *"Not thread-safe on
its own ... If a worker ever logs, wrap Push in the sink's lock."* This arc
introduces the first logging worker, so `ConsoleBuffer` gains a real mutex
guarding both `Push` and the UI read. Not deferrable: `project_open` logs during
the scan.

## 5. Project switch

`SwitchProject` reuses the same machinery in Overlay mode. Its existing
early-return guards -- rival editor, invalid project, ABI mismatch, unsaved
documents (`EditorAppProject.cpp:309-370`) -- stay exactly where they are,
**before** the sequence is built, so a refused switch still costs nothing and
never shows an overlay.

The sequence is four stages: teardown (main) -> `project_open` (worker) ->
`render_bridge` (main) -> `plugin_load` (main).

## 6. Splash customization

The editor's splash is engine-branded and not configurable -- it is our tool.

The **runtime host** reads an optional `splash` block from the `.arcproj`
manifest:

```json
"splash": {
  "enabled": true,
  "image": "game://Branding/splash.png",
  "backgroundColor": [0.05, 0.05, 0.06],
  "showProgress": false,
  "minDurationSeconds": 0.0
}
```

- `showProgress` **defaults to `false` for the runtime host** (changed
  2026-07-30). The editor always shows progress; a player does not care that we
  are scanning asset 412 of 1180. UE reached the same conclusion and enforces it
  structurally: `FFeedbackContext::ProgressReported` is a no-op base
  (`FeedbackContext.h:103`) and only the editor overrides it
  (`FeedbackContextEditor.cpp:664-669`), with the splash backend commenting at
  `WindowsPlatformSplash.cpp:783-790` that startup progress is *"not interesting
  to an end-user"*. Opt in per project if you want it.
- Block absent -> Arcane engine branding.
- `"enabled": false` -> no splash at all; the window stays hidden until the
  game's first real frame, so a game wanting a fully custom loading experience
  turns ours off and draws its own from frame one.
- `minDurationSeconds` exists because a fast boot can flash the splash for ~80ms,
  which reads as a glitch. Defaults to `0.0`, so it never slows down anyone who
  does not ask for it.

## 7. Layering: why `Host`, and the Core-promotion door

The servers (`Auth` / `Account` / `Combat`) do **not** need this, and mostly
cannot use it:

- Their startup is milliseconds of fail-fast work -- two small JSON reads, env
  and secret checks, a connection pool -- then a blocking `Start()`
  (`Server/Account/src/main.cpp`). There is no second-scale work for a worker to
  overlap.
- A loading screen solves "a human is staring at an undrawn window." A headless
  service has no window and no observer; its startup feedback is log lines and an
  exit code, which Docker and the Jenkins lane already consume.
- Structurally, the servers link `ArcaneCore` (`Arcane/Core/src`, static CRT --
  `Server/premake5.lua:70-87`), not `Arcane.dll`. `Arcane/Host` lives in the
  engine DLL.

`Host` is therefore the correct home: all three consumers live in the DLL, and
putting the scheduler in Core now would be speculative generality for a consumer
that does not exist.

**The door stays open by construction.** `BootSequence` has zero GPU, window,
and ImGui dependency (§1) -- all of that is isolated in `BootPresenter`. If a
server-side need ever appears, promoting `BootSequence` to Core is a file move
plus a premake line, not a redesign. Keep it that way: no NVRHI, SDL, or ImGui
include may enter `BootSequence.hpp`.

## 8. Testing

`BootSequence` is host-agnostic and GPU-free, so the interesting logic is
headless-testable with fake stages. New tag `[boot]`:

- Topological order respects declared dependencies.
- **Real overlap is proven, not assumed**: a worker stage blocks on an atomic
  until a main stage sets it; the sequence completes only if they genuinely ran
  concurrently.
- Dependency cycles are refused with the offending names.
- An Optional stage's failure lets dependents run; a Fatal one aborts and joins.
- Quit-during-boot aborts cleanly and joins the worker.
- Splash frames do not consume the `--frames N` budget.

**The bug-class gate (§0) -- the most valuable test in this arc:**

- Build `EditorApp`'s stage list and `RuntimeApp`'s stage list, and assert that
  **every id in `CoreStages()` appears in both**. This is what makes a future
  engine-wide install step impossible to forget in the runtime host, and it is
  the automated expression of the contract in §2.
- Assert `CoreStages()` ids are unique and that appended host stages cannot
  shadow a core id (a duplicate id would silently replace a core stage).
- Assert the runtime host's list is exactly `CoreStages()` -- it appends
  nothing today, and if that ever changes it should be a deliberate edit to
  this test.

These are cheap (no GPU, no window, no plugin) precisely because stage lists are
data. Note the honest limitation: this proves both hosts *run* the same stages,
not that a stage's body is correct in both modules. Instance 3 was a
per-module static slot, which is why `type_context_install` also carries the
`VerifySharedTypeContext` runtime check -- the list test and the in-stage check
cover different halves.

**`ServiceThread`** (new tag `[threading]`, all headless):

- Destructor stops and joins even with work still queued -- no leak, no detach.
- `StopRequested()` flips before the thread is joined, so cooperative cancel
  actually has a window to observe it.
- Submitted work runs on the service thread, never on the caller's.
- Destroying immediately after construction (zero work submitted) is clean.

**The ShaderCompiler retrofit's gate is its EXISTING suite, unchanged.** The
whole point is that observable behaviour does not move: run `[shadercompile]`
before and after and require the same result. Specifically preserved -- debounce
coalescing keystrokes, superseded-result drop at `Drain`, last-good staying
bound through a failed compile, `CompileNow`'s synchronous path, and
`Submit`/`Poll`/`Drain` remaining main-thread-only. If any of those need a test
change to pass, the retrofit is distorting the consumer and should be backed out
per §1.

Plus: `ConsoleBuffer` concurrency test (worker pushes while main reads) beside
its mutex fix; `ScanContent` progress-callback test (monotonic, terminates at
total); and `ArcaneRuntime` refusing an ABI-stale project instead of falling
through to the `data/` fallback (§2).

Splash appearance itself is **desk-verify only** -- windowed runs SIGSEGV under
this box's virtual-display setup, so `[gpu]` and interactive runs happen at the
desk.

No byte-identity gate: this changes boot ordering deliberately, and per the
standing directive the engine evolves rather than being frozen. The invariants
that matter are that `--frames N` scripted runs still work and the existing
`~[gpu]` suite stays green.

## 9. Desk-verify checklist

1. Editor cold start on a real project: **something is on screen within ~100ms
   of the click** (the pre-device splash), then the real window takes over, then
   the editor. At no point is there a black window, and at no point is there a
   second of nothing.
1b. Watch the pre-device -> real-window handoff frame by frame: the real window
   must appear BEFORE the pre-device splash closes. A flicker of empty desktop
   between them is the bug this ordering exists to prevent.
1c. Kill the pre-device splash path (or run on a non-Windows build) and confirm
   boot still completes normally -- it must never be able to fail boot.
2. Bar advances from both stages (it should move during the scan while the GPU
   chain is still building) and reaches 100% without going backwards.
2b. Splash text renders in Roboto from the first frame -- no stock-ImGui font
   visible, and no font "pop" partway through the splash.
3. Scan detail line shows real counts on a large project.
4. Window can be dragged and closed mid-boot; closing exits cleanly with no
   hang (proves the worker joins).
5. Project with a bad/missing `.arcproj`: boot completes project-less with the
   existing error surfaced at first frame.
6. Project switch shows the overlay; a refused switch (unsaved docs) shows none.
7. `ArcaneRuntime` splash: default branding; `"enabled": false` shows no splash.
8. `ArcaneEditor --project <p> --frames 10` still exits with the same frame
   count as before.
9. `ArcaneRuntime --project <p>` on an ABI-stale project REFUSES with a clear
   message instead of silently booting the `data/` fallback.
10. `ArcaneRuntime --frames N --screenshot out.png` still captures the scene --
   the runtime host's stage list is now `CoreStages()` and must lose nothing.

## 10. Non-goals

- Jobifying GPU resource creation (fights SDL/ImGui/loader-lock affinities for
  marginal gain -- the GPU prefix is ~1s).
- Animated/scripted splash content beyond a static image plus the bar.
- A progress API for game code (the game owns the window after `plugin_load`).
- Any server-side change (§7).
- **The ABI detect-and-rebuild flow.** Scoped OUT deliberately (decision
  2026-07-30) and deferred to its own arc, because two pieces it needs do not
  exist yet: a `BootSequence` stage that can PAUSE FOR USER CONSENT (stages
  today run to completion or fail -- you do not silently rebuild someone's
  source), and a build service. That service must NOT use the JobSystem: it is
  one enkiTS pool that `JobSystem.hpp` calls "the only thread source for the
  simulation", sized to hardware threads for short fork-join compute, and
  parking a worker on a multi-minute `msbuild` wait would starve it.
  `ShaderCompiler` faced the same choice and used a dedicated `std::thread`
  (`ShaderCompiler.cpp:415,604`) behind a main-thread-only
  `Submit`/`Poll`/`Drain` API -- that is the shape to copy. The follow-on arc
  also covers stale detection (DLL-exported ABI + source mtime rather than a
  hand-authored manifest int), manifest auto-stamping, Problems-panel
  integration for build diagnostics, and the Hub's pre-launch "Rebuild &
  Launch". Note `RewriteManifestField` (`Project.cpp:33`) can only write
  TOP-LEVEL STRING keys today, so re-stamping nested `engine.abi` needs new
  code.
