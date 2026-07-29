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

| Stage | Thread | Depends on | Policy | Wt | Work |
|---|---|---|---|---|---|
| `runtime_create` | main | -- | Fatal | 5 | `TypeContext` + `Runtime` (enkiTS pool + engine component roster, `Runtime.cpp:114-121`). |
| `project_open` | **worker** | `runtime_create` | Optional | 45 | `Project::Open` -> `ScanContent` -> ABI gate -> content root -> asset resolver -> config layering (`Runtime.cpp:385-420`). |
| `gpu_core` | main | -- | Fatal | 25 | `GpuContext::Create`: window (hidden) / device / swapchain / shaders / canvas / batcher / tonemap / ImGui / input / command list. |
| `editor_fonts` | main | `gpu_core` | Fatal | 5 | Theme + `InstallEditorFonts` (Roboto + merged lucide). Must precede the first presented frame -- see below. |
| `splash_ready` | main | `editor_fonts` | Fatal | 2 | Load splash texture, `Window::Show()`, present frame 1. |
| `editor_shell` | main | `editor_fonts` | Fatal | 3 | Title, icon, toolbar logo, docking flag, layout settings, console sink. |
| `render_bridge` | main | `gpu_core`, `runtime_create` | Fatal | 3 | `SetRenderResources`, `OffscreenImGuiLayer::Create`, `SetImGui`. |
| `editor_lock` | worker | `project_open` | Optional | 1 | `EditorLock::Write`. |
| `input_config` | main | `project_open`, `gpu_core` | Optional | 2 | `HostBoot::LoadInputConfig`. |
| `plugin_load` | main | `project_open`, `render_bridge` | Fatal | 9 | `PluginHost` emplace + `AddPlugin` + `Load`. |
| `finalize` | main | all | Fatal | -- | `UpdateWindowTitle`, raise-open-project, hand off to `MainLoop`. |

`ArcaneRuntime` registers a shorter list of the same stages (no editor shell, no
editor lock). The project switch registers a four-stage list (§5).

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
  "showProgress": true,
  "minDurationSeconds": 0.0
}
```

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

Plus: `ConsoleBuffer` concurrency test (worker pushes while main reads) beside
its mutex fix; `ScanContent` progress-callback test (monotonic, terminates at
total).

Splash appearance itself is **desk-verify only** -- windowed runs SIGSEGV under
this box's virtual-display setup, so `[gpu]` and interactive runs happen at the
desk.

No byte-identity gate: this changes boot ordering deliberately, and per the
standing directive the engine evolves rather than being frozen. The invariants
that matter are that `--frames N` scripted runs still work and the existing
`~[gpu]` suite stays green.

## 9. Desk-verify checklist

1. Editor cold start on a real project: no black window at any point -- nothing,
   then the splash, then the editor.
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

## 10. Non-goals

- Jobifying GPU resource creation (fights SDL/ImGui/loader-lock affinities for
  marginal gain -- the GPU prefix is ~1s).
- Animated/scripted splash content beyond a static image plus the bar.
- A progress API for game code (the game owns the window after `plugin_load`).
- Any server-side change (§7).
