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

**Desk battery (user -- the arc is not done until these run):**
1. Hidden dev command faults the GPU deliberately (OOB dispatch) in
   ReferenceProject under the editor: report lands in `Saved/Diagnostics/`,
   Problems pane notifies, Assets browser shows it, double-click opens the
   document with a sane breadcrumb timeline naming the faulting pass.
2. Same via ArcaneRuntime (project-less fallback dir).
3. GPU-stall path: infinite-loop shader -> gpu-stall report without device
   removal (TDR may race it; either artifact acceptable, noted in report).
4. Vulkan backend run of (1) -- degraded detail is expected on non-AMD/NV
   extension sets; the report must say what was unavailable.
5. Project switch retargets the dump dir (crash in B never lands in A's
   Saved/).

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
