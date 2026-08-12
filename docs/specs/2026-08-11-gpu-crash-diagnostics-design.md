# GPU Crash Diagnostics -- Design

**Date:** 2026-08-11
**Decisions (user-ratified):** first-party, platform-agnostic GPU crash
capture folded into the existing `Arcane::Diagnostics` family -- NO vendor
SDK integration this arc (Nsight Aftermath is closed-source driver tooling;
its samples/headers are reference material only, like the UE tree). Reports
surface through the ASSET BROWSER (no dedicated diagnostics pane -- the
browser is the one file interface), opening as a DocumentHost document in
the central area like the shader graph. `.arcdiag` files carry their own
GUID (everything in Arcane has a GUID). Instrumented-repro mode (D3D12 GBV /
Vulkan GPU-AV relaunch) is a FAST-FOLLOW arc, not this one. Profiling is a
separate existing direction and is untouched.

## Why

Diagnostics today captures CPU crashes and hangs (symbolized all-thread
reports + minidumps) but a GPU device-removed or TDR produces nothing --
no attribution, no artifact. Post-mortem *instruction*-level attribution is
impossible vendor-agnostically (the faulting shader PC lives in closed
driver state; every vendor gates it behind their own tool). The realistic
and sufficient target is **pass/draw/PSO-level attribution + fault data**,
which portable APIs fully support, with a backend seam so vendor SDKs can
deepen it later.

## Architecture

Four units, one seam each:

### 1. GpuBreadcrumbs (ArcaneClient/src/Arcane/Render/)

Per-queue persistent marker trail identifying what the GPU was executing
when it died.

- **D3D12:** core `WriteBufferImmediate` (vendor-agnostic) into a dedicated
  buffer on a heap that remains CPU-readable after device removal.
  `MODE_MARKER_IN`/`OUT` give begin/end semantics.
- **Vulkan:** `VK_AMD_buffer_marker` when the device offers it; otherwise
  degrade to fence-granularity CPU-side scope tracking (a ring of submitted
  scopes correlated with fence progress). `VK_NV_device_diagnostic_
  checkpoints` is a later backend, not a dependency.
- **API shape:** `BeginScope(name)/EndScope()` markers written at the
  render path's pass seams (Canvas passes, tonemap, ImGui, resolver passes)
  via `getNativeObject()` on the NVRHI command list -- the NVRHI boundary
  rules hold (no wrapping, no own barriers). Draw-level markers use the
  same API inside a pass, gated by the runtime toggle.
- Pure ring/state logic is headless-testable; only the writes touch the GPU.

### 2. Fault collection + GPU-progress watchdog

- **Device-removed trigger:** where NVRHI device errors surface
  (Render/Device seam). Collect: DXGI `GetDeviceRemovedReason`; DRED
  auto-breadcrumbs + page-fault data (`ID3D12DeviceRemovedExtendedData`,
  named allocations -> faulting-resource attribution); on Vulkan,
  `VK_EXT_device_fault` fault address/type plus the vendor binary blob
  saved raw for offline vendor tools.
- **GPU-progress watchdog:** the CPU hang watchdog cannot see a GPU-only
  stall (CPU heartbeats fine, frame fence stops advancing). A fence-progress
  check rides the existing watchdog thread; N seconds of no progress ->
  `WriteReport("gpu-stall")` with breadcrumb state, device not removed.
- **Backend seam:** `IGpuCrashBackend` -- `Enable(deviceDesc)`,
  `CollectFault(...)`, `WriteMarker(...)` -- with exactly one
  implementation this arc (the portable one above). Aftermath/RGD become
  additional backends behind the same call sites later.

### 3. Report emission (Base/Diagnostics + hosts)

- `Diagnostics` gains a **GPU-section provider seam**: the render layer
  registers a callback that serializes breadcrumb/fault state into the
  report. Base/ keeps zero Render dependencies.
- Every report now emits a third artifact beside the `.txt` + `.dmp`:
  **`.arcdiag`** -- versioned JSON envelope in the `.arc*` family style:

```json
{
  "formatVersion": 1,
  "guid": "<minted at write time -- Arcane::Guid::Generate()>",
  "kind": "crash | hang | gpu-crash | gpu-stall",
  "timestampUtc": "...", "appName": "...", "phase": "...", "buildInfo": "...",
  "cpu": { "threadSummary": [ ... ] },
  "gpu": {
    "queues": [ { "name": "...", "lastCompleted": "...", "inFlight": ["..."] } ],
    "fault": { "type": "...", "address": "...", "resource": "..." }
  },
  "siblings": { "txt": "...", "dmp": "...", "gpuDump": "<gpu kinds only>" }
}
```

- **`.gpudump` (gpu kinds, ALWAYS):** every `gpu-crash`/`gpu-stall` report
  also writes `<stem>.gpudump` -- the raw-capture analog of `.dmp`. A
  versioned tagged-section container (magic `AGPU`, u32 version, section
  table of `{tag[16], offset, size}`): raw marker-buffer bytes, raw DRED
  output, VK device-fault info, vendor blob -- whatever the backend
  captured, one section each, written even on partial collection (the
  section table doubles as the inventory). Uniform across D3D12/Vulkan/
  degraded paths; the `.arcdiag` stays the PARSED summary. (This replaces
  the earlier Vulkan-only `.vkfault.bin`.)

- **Write location:** hosts set `Config::dumpDir`. Editor + runtime point
  it at `<project>/Saved/Diagnostics/` when a project opens (retargeted on
  project switch -- same call-site family as `RetargetLayoutIni`);
  project-less hosts keep the exe-relative `diagnostics/` fallback
  unchanged. `Saved/` never ships, so cooking stays clean.

### 4. Surfacing: registry mount + CrashReportDocument (editor)

- **AssetRegistry:** automatic read-only mount over `Saved/Diagnostics/`.
  `.arcdiag` is a native asset kind (**Diagnostic**) whose GUID is read
  from the file, exactly like other native formats. A report written
  mid-session registers incrementally (same pattern as material/sprite
  creation) and publishes ONE Problems-pane diagnostic as the live
  notifier ("crash report written -- open from the Assets browser").
- **CrashReportDocument** (ArcaneEditor/src/Documents/): registered with
  DocumentHost for `.arcdiag`; opens in the central document area.
  Read-only (no dirty state): header (kind/time/phase/build), per-queue
  breadcrumb timeline with last-completed vs in-flight highlighted, fault
  block (type, address, named resource), CPU thread summary, shell-open
  buttons for the sibling `.txt`/`.dmp`. One viewer renders the whole
  report family, CPU and GPU kinds alike.

## Config policy (locked, mirrors UE 5.5+'s tiering)

| Layer | Debug | Release | Dist |
|---|---|---|---|
| Pass-level breadcrumbs | on | on | on |
| Draw-level markers | runtime toggle, default off | same | compiled out |
| DRED (D3D12) | full | full | lightweight |
| VK device_fault / buffer_marker | if present | if present | if present |

Vulkan extensions are optional everywhere -- absence degrades (fence
granularity, no fault detail), never fails device creation. The report
records which layers were active.

## Testing

**Headless (`[editor]`/`[diag]`, CI-safe):** `.arcdiag` envelope
round-trip incl. GUID minting; breadcrumb ring logic (pure); the GPU-section
provider seam (fake provider -> report contains section); registry scan
classifies `.arcdiag` under the mount; DocumentHost routes `.arcdiag` ->
CrashReportDocument (existing document-routing test pattern).

**The trigger (Task 11, landed).** Every fault item below is caused by ONE
command, `Arcane::GpuFaultInjector` (`ArcaneClient/src/Arcane/Render/`,
`#if !defined(ARCANE_DIST)`), reached two ways:

- **Editor:** Build -> Diagnostics -> **Crash GPU (diagnostics test)**.
- **Either host:** `--crash-gpu N` -- fault on the first frame after N
  complete (N buys warm-up). ArcaneRuntime has no menu, so item 2 would be
  unrunnable without it; the editor honours it too, both because a flag one
  host silently ignores is a trap and because a scripted battery beats a
  clicked one.

It dispatches `data/shaders/gpu_fault.hlsl`, whose mechanism is NOT the
"OOB dispatch" this spec assumed at plan time. That recipe is folklore on
D3D12/Vulkan: both REQUIRE a bounds check against the view for
structured-buffer access, so an out-of-bounds UAV store is discarded and
faults nothing on a conformant device. What the shader actually relies on
is a **long, serially dependent loop whose bound is a constant-buffer
value** -- un-foldable, un-unrollable, and un-elidable (its body stores to
a UAV) -- which is *expected* to run the GPU past the OS TDR window and get
the adapter reset. It is plain portable NVRHI (no native handles), so the
SAME command serves the Vulkan item unchanged. The out-of-bounds store is
kept as the loop's un-elidable side effect and as an opportunistic upgrade
to a real page fault wherever the bounds check is absent. Nothing in it
trips CPU-side validation: an OOB store is a GPU-side condition the D3D12
debug layer never sees, and Arcane never enables GBV.

**"Expected", not guaranteed -- and unproven until the desk run.** The TDR
watchdog is the OS's, which makes the mechanism vendor-*neutral*, but not
vendor-*independent*: preemption-capable GPUs (compute preemption on
Pascal-class and later, and the equivalents elsewhere) may honour the
preempt request and yield instead of hanging, in which case the OS resets
nothing and no device is ever lost. Whether this adapter preempts or hangs
is a property of the desk machine and cannot be settled from here. If a run
produces no fault at all, that is the expected first suspect -- turn up
`kIterations` / `kThreadGroups` in `GpuFaultInjector.cpp` (see checklist
item 0) rather than assuming the capture path is broken.

**Desk battery (user -- the arc is not done until these run):**

0. **Before anything else, check TDR.** Confirm the Windows default
   (`HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers`, `TdrLevel=3`,
   `TdrDelay=2`; absent values mean the default is in force). Two knobs
   matter and both bite:
   - **TDR disabled (`TdrLevel=0`)** -- the loop is bounded so the machine
     *does* recover on its own, but the wait is **minutes to tens of
     minutes**, not seconds. The loop is store-issue-bound (one UAV store
     per iteration), not latency-bound, so the honest figure is far worse
     than an instruction-latency estimate suggests. Do NOT reboot partway
     through: the report is written at device loss, and a reboot mid-capture
     destroys exactly the artifact the run exists to produce. Re-enable TDR
     first instead.
   - **No fault at all** -- see the preemption note above. Raise
     `kIterations` / `kThreadGroups` in `GpuFaultInjector.cpp` and rebuild
     before concluding anything about the capture path.
1. Dev command faults the GPU deliberately in ReferenceProject under the
   editor: report lands in `Saved/Diagnostics/`, Problems pane notifies,
   Assets browser shows it, double-click opens the document with a sane
   breadcrumb timeline naming the faulting pass (`pass:gpu-fault`).
   *Trigger: the menu item.*
2. Same via ArcaneRuntime (project-less fallback dir).
   *Trigger, from the runtime's exe dir:*
   `ArcaneRuntime --plugin ReferenceProject/Binaries/ReferenceGame.dll --crash-gpu 30`
   *-- and deliberately NO `--project`.*
   **`--plugin`, not a bare run.** ArcaneRuntime refuses to boot with
   neither `--project` nor `--plugin` ("nothing to host", `RuntimeApp.cpp`)
   -- deliberate since the Sandbox retirement -- and the refusal returns
   before `MainLoop`, so a bare run never reaches the frame the fault fires
   on. `--plugin` alone still leaves `CurrentProject()` null (`ProjectBoot`:
   "no `--project`: nothing to open, not a failure"), which is precisely the
   project-less state this item exists to test: `RetargetDumpDir` gets an
   empty path and the report must land in the **fallback** dir, not under
   any project's `Saved/`.
3. GPU-stall path: the same long-running shader -> gpu-stall report without
   device removal. At stock settings this does NOT happen and cannot: TDR
   fires at 2 s and the gpu-stall rule at 8 s, so the device is always gone
   first. To reach the rule at all, **raise `TdrDelay` above the gpu-stall
   threshold** (or lower the threshold) -- otherwise item 3 is only ever
   observed as item 1 and the rule stays unproven. Either artifact is
   acceptable per run; what is NOT acceptable is closing the arc without
   having seen a gpu-stall report once.
   *Trigger: the same command, with TdrDelay raised.*
4. Vulkan backend run of (1) -- degraded detail is expected on non-AMD/NV
   extension sets; the report must say what was unavailable.
   *Trigger: the same command, host launched `--backend vulkan`. No
   separate trigger exists or is needed -- the dispatch is portable NVRHI.*
5. Project switch retargets the dump dir (crash in B never lands in A's
   `Saved/`).
   *Trigger: the same menu item, after switching projects. No separate
   trigger needed.*

**Added by the implementation's fix rounds (same standing: user, at the
desk). These are regression checks on seams this arc CHANGED, and none of
them needs the fault command:**

6. Vulkan drag-resize / swapchain out-of-date storm: resize the window
   continuously on `--backend vulkan`. No freeze, and **no false
   `gpu-stall` report** -- an unstarted frame-slot query used to spin its
   whole window and report a stall for a device that had never been asked
   to do anything (`GpuFrameSlot`, and the reason it is a type rather than
   a loose bool).
7. `--perf` before/after pacing comparison. The swapchain's frame-slot wait
   was converted from a blocking `waitEventQuery` to a heartbeat-publishing
   poll so the GPU rules are reachable at all; confirm the per-phase ms
   have not regressed on an ordinary GPU-bound scene. Expected cost is at
   most ~1 ms on frames that genuinely waited.
8. Prove `gpu-stall` actually fires -- the standing form of item 3's caveat.
   Extended `TdrDelay` or an overlong workload; the rule is unproven until a
   `gpu-stall` `.arcdiag` has been read once.

## Non-goals

- Instrumented-repro mode (GBV/GPU-AV relaunch button) -- fast-follow arc.
- Vendor backends (Aftermath/RGD) -- seam only.
- Shader instruction/line-level attribution -- driver-gated, see Why.
- Any profiling surface -- separate existing direction.
- Report upload/telemetry.

## Plan inputs

- Re-download the UE tree to `D:\dev\starworks\Arcane\.example\`
  (gitignored) before implementation: the RHI breadcrumb fine print
  (`WriteBufferImmediate` heap choice, marker encoding, DRED tier
  enablement APIs) should be checked against UE's implementation, not
  recalled. NVIDIA's nsight-aftermath-samples repo is the second reference
  (integration shape for the backend seam).
- The exact DRED lightweight-enablement API level and the post-removal
  readable-heap flags are plan-time verification items, not spec claims.
