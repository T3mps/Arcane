# NRI adoption — wholesale RHI swap + unified 2D/3D frame graph (design)

Binding research input: `docs/research/2026-08-12-nri-survey.md` (+ the three
raw scans beside it). Every verdict cited here is sourced there; this spec
does not re-derive them.

## Why

The next game — a massive open-world multi-planet space game — needs a
renderer built on transient aliasing, explicit barriers, and tier-2 bindless
from day one. NVRHI abstracts exactly the things that renderer must own.
NRI v180 hands us the two primitives that matter (VMA/D3D12MA transient
aliasing with `CAN_ALIAS` always set; two-tier bindless including SM6.6
heaps) while staying out of our crash-diagnostics stack's way — provided
device creation stays ours (wrapper path). Aphelyon inherits the unified
path through pixel parity and never blocks on it.

**Ruled 2026-08-12 (user, binding): this is a COMPLETE migration.** One
branch, wholesale swap, no dual-RHI period, no NVRHI-alive checkpoints.
NVRHI is deleted from `ThirdParty/` in this arc's final phase. The
verification consequence is accepted: the branch does not render until the
substrate is up, so correctness rides on golden images captured from the
last NVRHI build BEFORE the swap and compared pass-by-pass on the NRI build
after. Slices are verification milestones, not shippable checkpoints.

**Definition of done: "2D/3D working."** 2D means every existing pass at
golden-image parity. 3D means a FOUNDATIONAL slice proving the unified
graph — perspective camera, depth-tested forward-lit meshes, coexisting
with the full 2D stack in one frame graph — not a production 3D feature
set (see Non-goals).

## Formal ratifications (this spec, on merge)

1. **NVRHI boundary rules are RETIRED and replaced.** The old rules
   (never wrap `ICommandList`, no own barriers, bindless-first) existed
   because NVRHI owned state tracking. The replacements: the engine OWNS
   the command-recording seam; ALL barriers are frame-graph-derived (a
   hand-written `CmdBarrier` outside the graph is a review defect);
   bindless-first stands unchanged.
2. **Wrapper-path mandate (permanent, not migration scaffolding):** NRI
   devices are created ONLY via `nri::nriCreateDeviceFromD3D12Device` /
   `...FromVKDevice` around the device WE create. `nriCreateDevice` would
   silently forfeit DRED configuration and own the VK feature chain —
   grep-gate it in CI. Our creation keeps: DRED-before-create, the
   `VK_EXT/KHR_device_fault` + buffer-marker sweep, and now the explicit
   capability contract NRI expects (§ Device layer).
3. **NVRHI deletion is part of this arc**, not a follow-up: `ThirdParty/
   nvrhi` removed, `feedback`-era boundary docs updated, zero-legacy grep
   sweep (path-exclude + `-riw`, per the standing method) proving no
   `nvrhi` token survives outside `docs/`.

## Architecture

### Vendoring

NRI v180 as a premake static lib in `ThirdParty/NRI/` (engine repo), no
CMake: Shared + D3D12 + VK + Validation + NONE + Creation, ~140 files.
Deps vendored beside it: Vulkan-Headers, VMA, D3D12MA, Agility SDK
(NuGet redistributable), AGS. NVAPI: include only if/when Reflex is
wanted — proprietary redistribution terms, and the engine repo is public.
Upscalers: NIS only (zero-footprint); DLSS/XeSS/FSR excluded (licensing +
shipped DLLs). WGPU backend excluded. Pin = v180 (flat integer
versioning; `nriVersion` runtime check at device create).

### Device layer

`DeviceD3D12`/`DeviceVulkan` keep their creation halves (adapter pick,
DRED tiers, VK instance/device with the fault-extension sweep) and drop
their NVRHI halves. New in creation: the **capability contract** — the
explicit feature set the wrapper path requires NRI to find enabled
(VK: sync2, dynamic rendering, timeline semaphores, descriptor indexing,
buffer device address; D3D12: enhanced barriers availability check).
NRI does not validate this at create time; we enumerate it from NRI
source once, assert it at device create, and keep the list in the device
TU beside the sweep it extends. `CallbackInterface` routes into the
existing log + `RenderErrorCount` latch; every `nri::Result` return is
checked (discipline enforced by a wrapper macro, same spirit as the 0/0
gate).

### Resource lifetime: the graveyard (precondition)

NRI has no refcounts and no deferred destruction; destroying a resource
the GPU still reads is a device fault. Engine-owned, built and tested
BEFORE any NRI resource exists: every `Destroy*` routes through a
fence-tagged graveyard — (object, queue-fence value) pairs reaped when
the frame fence passes. Per-queued-frame buckets, flush-on-device-loss
(reap everything after `WaitIdle` in teardown), and a leak report at
shutdown (count by type — the NONE backend makes this CI-testable
headlessly). The device-lost latch work (@419fc571) already gives the
orderly-teardown window the graveyard needs on the loss path.

### Frame graph

The organizing structure the whole port lands in — 2D passes become the
first nodes, the 3D slice proves the unification:

- **Declared passes**: each node declares reads/writes (resource +
  usage/layout). Barriers are DERIVED from declarations at compile time
  of the graph, batched per edge. No pass records a barrier itself.
- **Transients**: graph-owned textures/buffers allocated through the
  aliasing path (`CAN_ALIAS`); lifetime = declared first/last use.
  Imported resources (swapchain image, persistent targets, readback
  buffers) declared as externals with explicit entry/exit states.
- **Formats live in PSOs** (no framebuffer objects): pipeline creation
  moves to graph-node setup where attachment formats are known;
  `CmdBeginRendering(AttachmentDesc[])` per node.
- **Per-frame data**: hand-rolled per-queued-frame upload rings +
  `CmdSetRootDescriptor` byte offsets replace volatile CBs and
  `writeBuffer`-in-open-list (~20 sites: Batcher2D uploads, material
  globals, ImGui). The NRI Streamer is NOT adopted until its overflow
  footgun and the `CmdCopyStreamedData` barrier TODO are empirically
  cleared (survey §5) — measure later, keep the seam swappable.
- **Queues**: concurrent sharing (NRI default). `queueExclusive` +
  ownership transfers have no sample anywhere; not first on this too.

The existing 2D pass inventory maps to nodes: sprite batch, text, post
chain (fullscreen material passes), tonemap, selection outline (JFA),
pick buffer (readback node — NRI's `Readback` sample is this nearly
verbatim), ImGui, editor offscreen canvas.

### Swapchain + pacing + diagnostics carry-over

- `NRISwapChain`; resize = destroy+recreate (no resize API); we own
  `VK_SUBOPTIMAL` handling since NRI swallows it. The Vulkan drag-resize
  storm (battery item 6) is the standing regression check.
- `GpuFrameSlot` semantics move to timeline `nri::Fence`
  (signal-at-submit, `Wait(value)`) — 1:1, and the heartbeat-publishing
  poll transfers unchanged.
- Crash diagnostics parity is a GATE, not a hope: markers keep writing
  through native command buffers (`GetXxxNativeObject` at the same
  seams), breadcrumbs/freeze/`.arcdiag`/`.gpudump` are RHI-agnostic,
  device-removed observation upgrades from the message-substring hook to
  typed `Result::DEVICE_LOST` at submit/acquire/present feeding the same
  `ObserveDeviceRemoved` → latch chain.

### The 3D slice (Phase 4 — the "3D working" in the definition of done)

Scoped to prove unification, deliberately small:

- `Camera` component gains a projection mode: perspective (vertical FOV,
  near/far) beside the existing orthographic half-height. No new
  component type — one camera vocabulary, editor inspector extends.
- Mesh primitive: programmatic meshes (cube/sphere/plane/quad) behind a
  minimal `StaticMesh` render type. Model IMPORT (glTF) is a named
  follow-on arc, not this slice.
- One forward-lit opaque pass node: depth-tested, single directional
  light + ambient, bindless material slot (albedo texture + factors) —
  enough to exercise depth transients, PSO-format baking, and bindless
  in one node.
- Proof scene in ReferenceProject: 3D meshes + sprite layer + post +
  text + ImGui in ONE graph, golden-captured on both backends.

Shadows, GI, skinning, model import, clustered/deferred anything: named
follow-on arcs on this foundation (Non-goals).

## Phases (one branch; each phase ends at a named verification milestone)

0. **Golden harness on the LAST NVRHI build (Task 1, before any port
   code).** Offscreen capture of named scenes per pass and composited,
   both backends, `--golden-capture/--golden-compare` on ReferenceProject
   (HostBootTest idiom); comparator with per-channel tolerance +
   SSIM-style fallback, calibrated at the desk (GPU-gated, `[gpu]` tag).
   Goldens committed (small PNG set). THEN the swap branch opens.
1. **Substrate**: vendored NRI builds; graveyard (headless-tested on
   NONE); device layer wrapped; swapchain + frame pacing; clear +
   triangle on both backends. Milestone: triangle at parity, platform
   validation layers clean (D3D12 debug layer, VK sync validation — the
   dev-loop gate for the whole port, since NRI validation cannot catch
   barrier bugs).
2. **Frame graph core + 2D cutover**: nodes ported in dependency order
   (sprite batch → text → post/tonemap → outline/pick → ImGui), golden
   comparison per node as it lands. Milestone: ReferenceProject full-2D
   goldens pass on both backends.
3. **Host integration**: editor offscreen viewport canvas, pick
   readback, module-rebuild/project-switch device lifecycle (graveyard
   across SwitchProject), runtime `--frames`/`--screenshot`. Milestone:
   editor desk-drive (spec Testing) + full goldens on both hosts = the
   2D-parity gate.
4. **3D slice** (§ above). Milestone: proof scene goldens, both
   backends.
5. **Deletion + ratification**: NVRHI + old swapchain/device halves
   removed; boundary docs rewritten; zero-legacy sweep; gate/CI green;
   crash-diagnostics battery re-run (fault injector on BOTH NRI
   backends — the injector dispatch is already portable; markers/DRED/
   device-fault must land in reports at pre-port fidelity; drag-resize
   storm; `--perf` pacing eyeball). Milestone: the desk battery, on NRI.

## Testing

- **Headless (CI-safe)**: graph compile (barrier derivation, transient
  lifetime/aliasing correctness as pure logic), graveyard reap ordering
  incl. device-loss flush (NONE backend; respect its footguns —
  `MapBuffer` null, `GetFenceValue` 0), capability-contract enumeration
  against a mocked DeviceDesc, golden comparator unit tests.
- **Desk (GPU, user)**: golden captures/compares each phase milestone;
  platform validation layers clean through phases 1–4; phase-5 battery.
- **Standing regression checks inherited**: Vulkan drag-resize storm; the
  `--perf` ≤~1ms waiting-frame budget; device-loss clean exit + frozen
  breadcrumb timeline (@419fc571 / @0aff2e3e behaviors re-verified on
  NRI observation sites).

## Non-goals

- Production 3D features: shadows, GI, skinning/animation, model (glTF)
  import, LOD/streaming, deferred/clustered — follow-on arcs.
- Streamer adoption (until measured), ray tracing, mesh shaders, Reflex,
  upscalers beyond NIS, HDR output (formats noted as future direction).
- WGPU/Metal/console backends.
- Any Aphelyon/game-module change: zero game-side references exist; the
  plugin ABI is untouched (no `kGamePluginABIVersion` bump).

## Plan inputs (resolve during planning, not desk-blocking)

- Enumerate the wrapper capability contract from NRI source (exact VK
  features/extensions + D3D12 requirements it assumes enabled).
- Dear ImGui version vs `NRIImgui`'s ≥1.92 requirement — bump ours or
  port our backend to raw NRI (decide in plan; porting ours is small and
  keeps the game-debug-ImGui-in-viewport path unchanged).
- Golden tolerance calibration data (D3D12-vs-VK rasterization deltas on
  the existing 2D stack) — capture in Phase 0.
- Graveyard placement relative to the graph (module-level singleton vs
  per-device object) — decide with the device-layer task.
