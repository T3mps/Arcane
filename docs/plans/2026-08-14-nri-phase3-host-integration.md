# NRI Phase 3: Host Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Both hosts (ArcaneRuntime, ArcaneEditor) render through the NRI
frame graph on ONE device and ONE window per process — real textured content,
editor viewport/pick/outline/documents included, crash-diagnostics parity
intact — gated by fresh stage goldens on both hosts (the spec's 2D-parity
gate, desk milestone D3).

**Architecture:** Sever every NVRHI-device dependency on the frame's
data-supply side first (retained decoded pixels in Assets, bytes-only
material/post registration, device-less Batcher2D), so the one step that
cannot be split — dropping the NVRHI device and binding NRI's swapchain to
the host window (DXGI allows one flip-model swapchain per HWND) — lands as a
single reviewed cutover with its own desk checkpoint. The editor gets the
phase's one genuinely new capability: an offscreen graph output mode (render
the frame graph into a persistent sampled texture, no present), which its
viewport, documents, and golden harness all consume.

**Tech Stack:** NRI v180 (vendored), the Phase-2 frame graph
(`Render/Nri/`), ImGuiNri, SDL3, Catch2 (`[nri]` headless inside the
`~[gpu]` gate).

## Global Constraints

- **Binding inputs, read before any task:** the Phase 2 milestone record —
  `docs/plans/2026-08-13-nri-phase2-framegraph-2d-cutover.md` tail — whose
  FOUR PLAN-INPUT CORRECTIONS override anything here that contradicts them:
  (1) CAN_ALIAS does not exist in v180; (2) root descriptors are unbuildable
  with our all-space0 shaders; (3) descriptor-set CB views take no dynamic
  offsets — per-frame CB data uses the node-owned per-frame-slot HOST_UPLOAD
  arena idiom, the upload ring carries VB/IB only; (4) pool-handover barriers
  carry the previous tenant's REAL layout (never UNDEFINED+access), texture
  handovers always emit a barrier, `CheckAllBarriersD3D12Legal` runs on every
  compile. Also the spec (`docs/specs/2026-08-12-nri-adoption-design.md`
  § Phases item 3, § Testing) and the Phase-3 carry list in the milestone
  record.
- **The goldens are the regression floor.** The DEFAULT (no `--nri-graph`)
  NVRHI path stays bit-green through every task. The baseline SET is
  replaced exactly once, at desk checkpoint D3a (Task 3), when the scene
  gains textured content — the same pattern as Phase 2's Task 2/D1. After
  D3a the new set is frozen for the rest of the phase.
- **Reference-code policy (Deadlock contract, BINDING):** legal channels
  only — Filament (Apache-2.0), NRISamples (MIT, attribute where adapted),
  Unreal read-only. Never leaked source.
- **All barriers inside graph code:** nodes NEVER record `CmdBarrier`; the
  executor derives them. **Wrapper path only** (`nriCreateDeviceFrom*Device`);
  `nriCreateDevice` for a real backend = review defect (NONE test carve-out
  unchanged).
- **Crash-diagnostics parity is a GATE, not a hope** (spec § Swapchain +
  pacing + diagnostics): the chain must be armed by whichever device the
  host actually created. A graph-only host must produce device-removed
  reports, `.gpudump` sections, GPU-stall watchdog coverage, and a working
  `--crash-gpu`, at pre-port fidelity.
- **Plugin-facing vtables are FROZEN**: `OffscreenCanvas`, `Batcher2D`, and
  anything a game module dispatches into take new virtuals at the END of the
  class only, with a `kGamePluginABIVersion` bump (12→13 in Task 2) and the
  `Runtime::RestampProjectEngineAbi` seam restamping projects. ONE bump for
  the whole phase — batch every ABI-visible change into Task 2.
- **`--nri-graph` stays the opt-in for the whole phase** (Dist-guarded, as
  today). The default-path flip to NRI is Phase 5, not here.
- New headless tests carry `[nri]` (inside the `~[gpu]` dev gate).
  NONE-backend footguns: `MapBuffer` → null, `GetFenceValue` → 0,
  `CmdBarrier` empty. GPU halves are desk-verified at the named checkpoints;
  NO GPU/windowed runs in-session (desk-only machine hazard).
- New files → premake regen (`ThirdParty\premake5\premake5.exe vs2026` from
  repo root). Build BOTH configs per task
  (`"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug|Release /m`);
  full gate `bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "~[gpu]"`
  FROM THE EXE DIR (time-seeded random order — capture the seed banner on
  failure). Engine repo `main`, commit per task, conventional commits.
  clangd/IDE diagnostics are false positives; msbuild is the compiler of
  record. ONE BUILDER AT A TIME on the checkout.
- Fixed-function shaders stay offline-compiled; material shaders stay
  stitched + in-process-compiled; the graph consumes the same bytecode.
  Register conventions unchanged.

## Reconciliations vs the spec and the carries (recorded, not silent)

1. **The device flip and the window flip are ONE landing (Task 6).** DXGI
   permits one flip-model swapchain per HWND (`NriGraphContext.hpp:89-98`),
   so "render into the host window, then remove NVRHI" does not exist as an
   intermediate on dx12. Ordering chosen: sever data supply first (Tasks
   1-2, headless-provable), then one reviewed cutover with desk checkpoint
   D3b immediately after. The Phase-2 per-node desk cadence is deliberately
   unavailable for this one step; D3b is its compensating gate.
2. **The baseline set is REPLACED at D3a** (textured content). Phase 2's
   six frozen goldens retire when Task 3's enriched captures land — they
   were the contract for an untextured scene. Comparator and tolerances
   (2, 0.1%) are unchanged.
3. **The per-window SDL event-routing carry is DROPPED as obsoleted.** The
   one-window flip removes the second SDL window from every production
   configuration; `Window::PumpEvents`' process-wide drain becomes harmless
   again. Revisit only if multi-window returns (recorded here so the Task 7
   carry from Phase 2 has a disposition).
4. **Editor goldens capture the VIEWPORT OUTPUT, not the backbuffer.** The
   editor's backbuffer is a clear + editor chrome (`EditorAppFrame.cpp:
   1925-1930`) — chrome pixels (fonts, docking layout) are not golden
   material. The capture source is the viewport output texture in both
   editor modes (NVRHI: `OffscreenCanvas` output; graph: the offscreen graph
   target), full-resolution, including the outline/gameui composite. Naming
   gains the host axis: `editor-<stage>-<backend>.png` (runtime keeps
   `main-…`).
5. **Editor click-pick latency semantics change, ACCEPTED.** `PickBuffer::
   Pick` stalls with `waitForIdle` per click (`PickBuffer.cpp:168`); the
   graph pick node reads back with ≤2-frame latency by design (Phase 2
   Task 11). Phase 17's same-frame `Select/Clear` flow becomes a deferred
   apply when the readback lands (Task 9). A one-to-two-frame click-to-
   select delay at 240 FPS is imperceptible; the stall removal is the win.
6. **GpuContext grows a graph FLAVOR, not a fork.** One class; in graph
   mode the NVRHI members (`Device()`, `Cmd()`, `Swap()`, `Cnv()`, `Tone()`,
   `FramebufferFor`, `EnsurePost`) are never constructed and their accessors
   `ARC_ASSERT` + return-null on touch. All 133 call sites live on paths
   that are mode-gated already (the NVRHI arm / editor NVRHI phases). The
   flavor split is cleaned up at Phase 5 when the NVRHI members die for
   real.
7. **Plugin-created `OffscreenCanvas` against a null device is a LOUD
   refusal, carried.** `Runtime::SetRenderResources(nullptr, nullptr)` is
   the graph-mode contract (its `Device()` accessor has zero consumers
   today); a module that calls `OffscreenCanvas::Create(nullptr, …)` gets a
   null + `ARC_ERROR` naming the phase-5 arc. ReferenceProject and Aphelyon
   use no OffscreenCanvas; the general story is Phase 5's.
8. **The spec's "no ABI bump" non-goal is superseded** (standing directive:
   ABI bumps are cheap during engine dev; Phase 2 closed at 12). Task 2
   takes 12→13 for the span texture key; both in-tree and out-of-tree
   projects restamp through the existing seam.
9. **`GpuFaultInjector` gets an NRI twin instead of a port.** The nvrhi
   injector stays for the NVRHI mode; graph mode fires the same TDR-loop
   shader (`data/shaders/gpu_fault.hlsl`) through a one-off NRI compute
   dispatch (Task 5). Same CLI, same battery items, per-mode implementation.

## File structure (locked)

```
ArcaneClient/src/Arcane/
  Assets/Assets.hpp/.cpp            Task 1: retained decoded-pixel cache (PixelsFor)
  Assets/ImageIo.hpp                Task 1: PixelData struct (device-free)
  Scene/SpriteCache.cpp             Task 1: geometry from PixelsFor dims
  Render/Batcher2D.hpp/.cpp         Task 2: device-less Create, span texture key (ABI 13),
                                            bytes-only material registration
  Render/SpriteMaterialCache.cpp    Task 2: null-device guards, blob-only Bind
  Render/PostChainCache.cpp         Task 2: null-device guards, desc publish hoist
  Render/Nri/NriTextureCache.hpp/.cpp  Task 2: promoted Guid→nri::Texture cache
                                            (from Batch2DNode's private one)
  Render/Nri/nodes/Batch2DNode.*    Task 2: consume NriTextureCache; sample real textures
  Render/Nri/NriGraphContext.hpp/.cpp  Task 6: host-window mode; Task 7: offscreen mode
  Render/Nri/NriDiagnostics.hpp/.cpp   Task 5: device-agnostic arming + GpuHeartbeat
                                            publisher + NRI fault injector
  Host/GpuContext.hpp/.cpp          Task 6: graph flavor (CreateForGraph)
  Host/HostConfig.hpp/.cpp          Task 13: editor golden honor list
  Platform/Window.*                 untouched (reconciliation 3)
ArcaneRuntime/src/
  RuntimeApp.cpp/.hpp               Task 4: frame-body extraction; Task 6: the landing
  RuntimeFrame.hpp/.cpp             Task 4: extracted frame body (new TU)
ArcaneEditor/src/App/
  EditorApp.* / EditorAppFrame.cpp  Tasks 8-10: viewport/pick/chrome onto the graph
  EditorAppProject.cpp              Task 12: switch/rebuild lifecycle in graph mode
ArcaneEditor/src/Documents+Widgets  Task 11: ShaderEditorDocument preview, GraphGridPass
ReferenceProject/Content/           Task 3: textured sprite + textured material param
ReferenceProject/Goldens/           Task 3 (D3a): REPLACED baseline set, + editor set (Task 13)
ArcaneTests/src/                    per task: RenderGraphTest, AssetsTest (new),
                                            HostConfigTest additions
```

---

### Task 1: Retained decoded pixels — Assets grows a device-free supply

The single change with three payoffs (seam map §C.10): sprite geometry stops
needing a live GPU texture, THE TEXTURE GAP gets its feed, and the graph's
per-Guid decode becomes a cache hit.

**Files:** Modify: `Assets/ImageIo.hpp` (add `PixelData`),
`Assets/Assets.hpp/.cpp`, `Scene/SpriteCache.cpp:79-96`. Test: new
`ArcaneTests/src/AssetsPixelsTest.cpp` (headless, real PNG fixtures on disk).

**Interfaces (produces):**
- `ImageIo.hpp`: `struct PixelData { std::uint32_t width = 0, height = 0;
  std::vector<unsigned char> rgba; bool Valid() const; };` — nvrhi-free by
  the header's existing charter.
- `Assets`: `virtual const PixelData* PixelsFor(const Guid& id) = 0;` —
  decode-once, retained, LRU-budgeted beside `m_textures` (same
  `AssetCache` idiom, bytes-weighted). Null on missing/undecodable, warn
  once, memoize failures like `Assets.cpp:151-156` does.
- `GetTexture` (`Assets.cpp:145-215`) becomes a CONSUMER: decode moves into
  `PixelsFor`; the NVRHI upload path reads the retained pixels instead of
  its own `stbi_load` (the `:210` free disappears; eviction of the pixel
  entry is the new reclaim point). Identical GPU result — the floor.
- `SpriteCache.cpp:84-88`: `ComputeSpriteGeom` inputs come from
  `PixelsFor(id)->width/height` when the device path is absent; when the
  NVRHI texture exists its desc still agrees (assert both when both exist).

- [ ] **Step 1:** Write failing headless tests: `PixelsFor` decodes a
  fixture PNG (known 8x4 checker) to the exact byte pattern; second call
  returns the same pointer (cache hit); missing Guid → null, memoized;
  LRU eviction under a tiny budget evicts pixels only after handle release
  rules matching the existing texture-cache tests.
- [ ] **Step 2:** Implement `PixelData` + `PixelsFor` on `AssetsImpl` via
  the existing `LoadPngRgba` (`Assets.cpp:682-701`); route `GetTexture`
  through it; keep `SetDevice`'s wholesale texture clear (`Assets.cpp:82-92`)
  but NOT the pixel cache (pixels are device-independent — say so in the
  comment).
- [ ] **Step 3:** `SpriteCache` reads dims from `PixelsFor` (null-device
  legal); gate green; both configs build.
- [ ] **Step 4:** Commit:
  `feat(assets): retained decoded pixels -- device-free texture supply (PixelsFor)`

### Task 2: The severance + the shared NRI texture cache (ABI 13)

Everything ABI-visible in the phase lands here, once.

**Files:** Modify: `Render/Batcher2D.hpp/.cpp`,
`Render/SpriteMaterialCache.cpp`, `Render/PostChainCache.cpp`,
`Plugin/PluginABI.hpp` (13 + changelog), `ReferenceProject.arcproj` restamp.
Create: `Render/Nri/NriTextureCache.hpp/.cpp` (promotion of
`Batch2DNode.cpp:733-799`'s private cache). Modify:
`Render/Nri/nodes/Batch2DNode.hpp/.cpp` (consume it; sample real textures),
`Render/Nri/nodes/FullscreenNodes.cpp` (post declared-texture params consume
it), `Render/Nri/NriGraphContext.cpp` (owns the cache; feeds
`Assets::PixelsFor` through the existing `AssetResolveFn` seam extended to
pixels). Test: RenderGraphTest additions + existing batcher CPU tests.

**Interfaces:**
- Consumes: Task 1's `Assets::PixelsFor`.
- Produces: `Batch2DDrawSpan` gains `Guid textureId;` APPENDED after
  `indexCount` (`Batcher2D.hpp:133-139`; struct crosses the vtable by value
  → `kGamePluginABIVersion` 12→13; the `nvrhi::ITexture* texture` member
  STAYS for the NVRHI recorder — both keys, one truth).
- `Batcher2D::Create(nvrhi::IDevice* device, ShaderLibrary* shaders)` where
  BOTH may be null → device-less instance: `Init()`'s five GPU creations
  (`Batcher2D.cpp:107-183`) skipped; `End()` refuses loudly (records
  nothing, `ARC_ERROR` once); `Begin/Quad*/Drain/MaterialDesc` fully live.
- `Batcher2D::BuildEntry` accepts a desc whose `vs`/`ps` handles are null
  when `vsBytes`/`psBytes` are present (relax `Batcher2D.cpp:599-603`); the
  GPU-side lazy creations (`:628,631,641`) guard on device and are simply
  never reached device-less.
- `SpriteMaterialCache.cpp:209-218` and `PostChainCache.cpp:293-302`: null
  device → skip `createShader` (blobs are the product); `PostChainCache`'s
  `e.desc` publish (`:352-355`) hoists ABOVE the chain gate (`:331`) so a
  device-less run still publishes `PostChainDesc`.
- `NriTextureCache`: `Create(NriDevice&)`,
  `nri::Texture* Resolve(const Guid&)` (upload-once via
  `HelperInterface::UploadData` from `PixelsFor` bytes, SRGB rule identical
  to `Assets.cpp:190`'s format choice), `nri::Descriptor* View(const Guid&)`,
  graveyard-buried teardown, failure memoized. The one-shot
  THE-TEXTURE-GAP warn moves here and fires only when `Resolve` misses.
- `Batch2DNode`: spans with a non-nil `textureId` bind the cache's view at
  t0 instead of the white texel (the descriptor-set-per-(texture,slot)
  machinery from Task 9 of Phase 2 already exists for materials — reuse its
  pattern); nil Guid keeps the white texel (colored quads).

- [ ] **Step 1:** Batcher severance + span key first (device-less Create,
  drain carries Guids, BuildEntry relaxation, ABI 13 + changelog +
  ReferenceProject restamp); existing batcher CPU tests green + a new
  device-less-instance test (Begin/Quad/Drain produce identical spans to a
  devicecarrying instance minus the `texture` pointers).
- [ ] **Step 2:** Cache-severance guards (4 sites + hoist), with a
  device-less `[render]` test: a fake compiled material Binds without a
  device and its `MaterialDesc`/`PostChainDesc` publish bytes.
- [ ] **Step 3:** `NriTextureCache` promotion + node consumption; `[nri]`
  frame-shape tests extend (a span with a texture Guid produces the same
  graph shape — textures are not graph resources; assert the set-update
  count, not pixels).
- [ ] **Step 4:** Both configs; full gate. Commit:
  `feat(nri): device-free data supply + shared texture cache -- ABI 13, spans carry Guids`

### Task 3: Textured content + D3a — the new baseline set (ends at desk)

**Files:** Modify: `ReferenceProject/Content/` (scene + a small committed
PNG asset, e.g. a 64x64 four-quadrant marker texture; one existing sprite
gains the texture Guid; the pulse material's `DetailTexture` param binds it).
No engine code.

- [ ] **Step 1:** Add the texture asset + bind it (one sprite + the material
  param). Deterministic under the golden clock (static texture — nothing
  time-varying added).
- [ ] **Step 2:** Gate green (census counts change: update the settled-scene
  expectations in HostBootTest if they pin counts).
- [ ] **Step 3 — DESK CHECKPOINT D3a (USER, blocks Tasks 6+ desk items, not
  Tasks 4-5):** on the NVRHI path, capture the replacement baselines —
  `--golden-capture` × `batch|post|full` × `dx12|vulkan` (6 PNGs), self-
  compare all `maxDelta 0`, eyeball the texture (marker orientation proves
  UV handedness both backends), THEN run the still-two-window vehicle's
  `--golden-compare` per stage per backend: the graph path with
  `NriTextureCache` must match the textured NVRHI baselines. Any delta is a
  texture-path finding (suspects: SRGB vs UNORM view, UV V-flip, filter
  mode). Commit goldens:
  `test(golden): phase-3 textured baselines -- both backends, replaces phase-2 set`

### Task 4: RuntimeApp frame-body extraction (mechanical, floor-preserving)

The landing (Task 6) must be reviewable; `MainLoop` is a 725-line function
(`RuntimeApp.cpp:536-1260`). Extract first, change nothing.

**Files:** Create: `ArcaneRuntime/src/RuntimeFrame.hpp/.cpp`. Modify:
`RuntimeApp.cpp` (MainLoop delegates), `RuntimeApp.hpp`.

**Interfaces (produces):** free functions in namespace `Arcane::RuntimeFrame`,
each taking a context struct of references (no new state):
`PumpAndResize(FrameIo&) -> bool quit`, `AdvanceSim(FrameIo&)`,
`BuildHud(FrameIo&)`, `RenderNvrhi(FrameIo&)`, `RenderGraph(FrameIo&) ->
FrameOutcome`, `CaptureTail(FrameIo&) -> bool stop` — boundaries exactly at
the seam map §A's shared-head / arm / shared-tail lines. `FrameIo` carries
the pointers MainLoop already owns (`GpuContext*`, `NriGraphContext*`,
`SceneRenderResolver*`, `HostConfig&`, the perf/golden locals).

- [ ] **Step 1:** Move code verbatim (comments included), one commit,
  no behavior change; `MainLoop` becomes the loop skeleton + calls.
- [ ] **Step 2:** Both configs; full gate; `git diff --stat` sanity: net
  new lines ≈ plumbing only. The floor proof is D3a's fresh self-compares
  re-run at the next desk visit (note in report; not a blocker).
- [ ] **Step 3:** Commit:
  `refactor(runtime): extract the frame body -- MainLoop becomes reviewable before the flip`

### Task 5: Diagnostics ownership — armed by whichever device exists

Seam map §H: the crash chain is armed by the NVRHI device
(`DeviceD3D12.cpp:274-291`, VK twin `:1299-1318`); `GpuHeartbeat` has one
publisher (`GpuInstrumentation.cpp:203`); `--crash-gpu` is a graph no-op.

**Files:** Create: `Render/Nri/NriDiagnostics.hpp/.cpp`. Modify:
`Render/Nri/NriGraphContext.cpp` (arm on create when NVRHI absent; publish
heartbeat), `Render/Nri/NriSwapChain.cpp` (completed-value source),
`ArcaneRuntime/src/RuntimeFrame.cpp` (graph-arm `--crash-gpu`),
`Host/HostConfig.hpp` (the no-op doc becomes history). Test: `[nri]` +
`ProgressStallRule` unit tests (the rule is already clock-parameterized,
`Diagnostics.hpp:179-211`).

**Interfaces (produces):**
- `NriDiagnostics::Arm(NriDevice&)` — installs
  `SetDeviceRemovedHook(&ObserveDeviceRemoved)` (reusing the existing
  observer via a small export — the D3D12/VK observers stay where they are;
  what moves is INSTALLATION), a graph-flavored
  `Diagnostics::SetGpuSectionProvider` (breadcrumb dump from the graph's
  CPU breadcrumbs + native markers when armed), and
  `SetActiveGpuCrashBackend` for the NRI device's backend. Idempotent;
  no-ops when the NVRHI device already armed (two-device transition runs).
- `NriDiagnostics::PublishHeartbeat(uint64 completedFenceValue)` →
  `Diagnostics::GpuHeartbeat` — called after every presented frame from
  `NriGraphContext::RenderFrame` with the pacing fence's completed value
  (the timeline fence IS `GpuFrameSlot`'s 1:1 replacement per the spec).
- `NriDiagnostics::FireFault(NriDevice&, nri::CommandQueue&)` — the
  `gpu_fault.hlsl` TDR loop as a one-off NRI compute dispatch (pipeline via
  NriPipelineCache compute path if present, else a local one-shot; barriers
  via a dedicated tiny command buffer OUTSIDE graph code — this is a
  deliberate diagnostics-only exemption from the no-hand-barriers rule,
  same as the smoke's was, comment it as such).
- `RuntimeFrame::RenderGraph` fires it at `--crash-gpu N` exactly like the
  NVRHI arm (`RuntimeApp.cpp:857-868` semantics).

- [ ] **Step 1:** Heartbeat publisher + stall-rule reachability test
  (simulated clock: publish → starve → rule fires at 8s).
- [ ] **Step 2:** Arming seam + idempotence test (NONE device arms the hook
  slot; double-arm is a no-op).
- [ ] **Step 3:** Fault injector twin (desk-proven at D3b; in-session just
  compile + shape).
- [ ] **Step 4:** Both configs; gate. Commit:
  `feat(nri): diagnostics armed by the device that exists -- heartbeat, hooks, fault twin`

### Task 6: THE LANDING — runtime host, one device, one window (ends at desk D3b)

The one step that cannot be split (reconciliation 1). `--nri-graph` now
means: NO NVRHI device is created; NRI wraps the only device; the swapchain
binds the HOST window; the vehicle's second window is deleted.

**Files:** Modify: `Host/GpuContext.hpp/.cpp` (`CreateForGraph(config)`:
window + device-less `Batcher2D` + `ImGuiLayer` on the ImGuiNri backend +
`InputDevices/InputActions`; NVRHI accessors assert-unreachable — recon 6),
`Render/Nri/NriGraphContext.hpp/.cpp` (accept a borrowed `Window&` — member
becomes `Window* m_borrowedWindow` beside the owned one, teardown contract
comment updated; device arming calls `NriDiagnostics::Arm`),
`ImGui/ImGuiLayer.hpp/.cpp` (backend delegation: constructed-for-graph
renders via `ImGuiNri`; `BeginFrame/Render` keep the SetCurrentContext pin —
this closes the Phase-2 carry), `ArcaneRuntime/src/RuntimeApp.cpp` +
`RuntimeFrame.cpp` (boot: graph mode → `CreateForGraph`; frame: one window
pumped, resize collapses to `m_graphContext->Resize` alone — `:701`'s
SetSize lockstep and `:708`'s OnResize residual both die; shader hot-reload
poll and `Runtime::SetRenderResources` go null-device; capture/screenshot
unchanged), `Render/ShaderLibrary` consumers (graph mode uses
`NriGraphContext::ShaderBytecode` — already true in the nodes; the
`GpuContext.cpp:38` NVRHI ShaderLibrary is simply not built).

**Interfaces:** consumes Tasks 1-5 (device-less supply, texture cache,
extracted frame, diagnostics arming). Produces: the one-device graph mode
that Tasks 7-13 build the editor on.

- [ ] **Step 1:** `GpuContext::CreateForGraph` + accessor gating; boot-path
  split in `StageGpuCore` (`RuntimeApp.cpp:254`); `Runtime::
  SetRenderResources(nullptr, nullptr)` on this path (Assets device-less —
  `PixelsFor` carries textures).
- [ ] **Step 2:** `NriGraphContext` borrowed-window mode + `NriDiagnostics::
  Arm` on create; delete the vehicle window-creation branch and the
  two-VkDevice / debug-layer-declined warnings (they describe a topology
  that no longer exists — the graph device is now FIRST and dx12 CAN take
  the debug layer: enable it in Debug exactly as `DeviceCreationD3D12`
  always wanted; this un-adjudicates the InfoQueue1 dead-channel ruling,
  note it in the report).
- [ ] **Step 3:** ImGuiLayer delegation (HUD becomes interactive again on
  the graph path is NOT desired — keep the non-interactive stance until
  after D3b's compares, gated exactly as today, comment why).
- [ ] **Step 4:** Frame unification in `RuntimeFrame` (one pump, one
  resize, arms select recorder); `--crash-gpu` live via Task 5.
- [ ] **Step 5:** Both configs; full gate; grep sweep: no
  `Arcane NRI Graph` window title, no `m_gpu->OnResize` on the graph path.
- [ ] **Step 6 — DESK CHECKPOINT D3b (USER):** stage compares vs the D3a
  set, both backends, Debug + Release — now on ONE device in the HOST
  window (dx12 Debug runs WITH the debug layer for the first time: read the
  log; InfoQueue1 findings are new signal, not regressions); NVRHI floor
  self-compares still `maxDelta 0`; drag-storms both backends (native GPU
  markers are back ON — the identity gate passes now; verify marker lines
  present, zero errors); `--crash-gpu 60 --nri-graph` produces a gpu-crash
  report with breadcrumbs on BOTH backends; `--pick-probe` HIT; `--perf`
  budget. Commit:
  `feat(nri): the one-device landing -- graph mode owns the host window, NVRHI absent`

### Task 7: Offscreen graph output mode

The editor's capability (hard problem 3): render the graph into a sampled
persistent texture, no present.

**Files:** Modify: `Render/Nri/NriGraphContext.hpp/.cpp`,
`Render/Nri/RenderGraphExec.cpp` (only if the no-swapchain execute needs a
seam the NONE tests don't already prove), `Render/Nri/NriGraphContext.cpp`
(`DeclareGraphFrame` gains the offscreen target import). Test:
RenderGraphTest `[nri]` — the offscreen frame shape headless.

**Interfaces (produces):**
- `NriGraphContext::CreateOffscreen(const HostConfig&, NriDevice& shared,
  uint32 w, uint32 h)` — SHARES the host's device (one device per process;
  the editor's chrome context and viewport context are two NriGraphContext
  over one NriDevice — graveyard and pipeline cache are per-context, fine),
  no swapchain, owns a persistent `BGRA8_UNORM` output texture
  (`OffscreenCanvas.cpp:22`'s non-sRGB rationale carries over verbatim —
  quote it).
- `FrameOutcome RenderFrameOffscreen(const FrameDesc&)` — Reset → declare
  (tonemap's final target = the imported output texture instead of a
  backbuffer) → Compile → Execute with `RgExecuteDesc.swapChain = nullptr`
  (no acquire, no present; the graph fence still signals — the headless
  NONE tests already exercise this executor path; extend one to pin
  "no swapchain → no PRESENT exit barrier, final state SHADER_RESOURCE").
- `nri::Texture* OffscreenOutput()` + `ImTextureID OffscreenTextureId()`
  (the ImGuiNri convention: the raw `nri::Texture*` through `intptr_t`).
- `ResizeOffscreen(w,h)` — idle → invalidate views (PoolEpoch discipline
  unchanged) → recreate the output through the graveyard.
- Pacing without a swapchain: `RenderFrameOffscreen` waits its own timeline
  fence at `kSwapchainFramesInFlight` depth (the ring/arena slot math is
  already keyed off `FrameSlot()` — same modulus, fence-driven).

- [ ] **Step 1:** Headless shape tests first (offscreen frame: N nodes, no
  present barrier, output imported with SHADER_RESOURCE exit state; resize
  bumps epoch).
- [ ] **Step 2:** Implement; both configs; gate.
- [ ] **Step 3:** Commit:
  `feat(nri): offscreen graph output -- render-to-sampled-texture, no present`

### Task 8: Editor viewport onto the graph

**Files:** Modify: `ArcaneEditor/src/App/EditorApp.cpp` (boot:
`CreateForGraph` flavor + a viewport `NriGraphContext::CreateOffscreen`),
`EditorApp.hpp` (`ViewportTargets` grows the graph trio),
`EditorAppFrame.cpp` phases 8-10 (`ApplyPendingViewportResize` →
`ResizeOffscreen`; `RenderSceneToViewport` → build a `FrameDesc` from the
editor's resolver exactly as `RuntimeFrame` does and call
`RenderFrameOffscreen`), phase 16 (`DrawViewportPanel` takes
`OffscreenTextureId()`).

**Interfaces:** consumes Tasks 6-7. The editor's graph mode is opt-in the
same way (`--nri-graph` honored by the editor from here on — add it to the
editor's honor list; the editor NVRHI mode stays the default and the floor).

- [ ] **Step 1:** Boot split + viewport render swap; Play/Edit both render
  the scene (gameui/outline still NVRHI-composited — they move in Task 9;
  UNTIL Task 9, editor graph mode composites nothing over the viewport:
  acceptable mid-phase state, named in the report).
- [ ] **Step 2:** Both configs; gate; editor NVRHI mode untouched (grep the
  phase functions for mode gates).
- [ ] **Step 3:** Commit:
  `feat(editor): viewport renders through the offscreen graph -- opt-in graph mode`

### Task 9: Editor pick, outline, and gameui composite on the graph

**Files:** Modify: `EditorAppFrame.cpp` phases 11/12/17,
`EditorApp.cpp` (graph-mode `ViewportTargets`: the pick/outline nodes
replace `PickBuffer`/`SelectionOutline` instances),
`Render/Nri/nodes/PickOutlineNodes.hpp/.cpp` (arming from real selection —
the `FrameDesc.selectedIds` "replace THIS ONE LINE" seam),
`ImGui/OffscreenImGuiLayer` consumers (gameui draws through a second
`ImDrawData` into the offscreen frame — an `ImGuiNriNode` instance over the
gameui context's draw data, composited before the outline exactly per the
phase 11/12 order).

**Interfaces:** consumes Task 7's offscreen frame + Phase 2's nodes.
Produces: `FrameDesc.pickables/selectedIds/pickOutline` fed from
`m_selection` + `PassIdOf` (`EditorAppFrame.cpp:1229-1236`); a
`FrameDesc.gameUi` (`ImDrawData*` from `m_gameImgui`) drawn by a second
ImGui node onto the offscreen target between tonemap and outline-composite
(matching phases 11 then 12's mutual exclusion by mode).

- [ ] **Step 1:** Selection/outline: `selectedIds` from the editor
  selection; outline composites onto the offscreen output; Edit-mode only.
- [ ] **Step 2:** Click-pick: phase 17 arms the pick node with the click
  pixel; the HIT applies on the frame the readback lands (deferred
  `Select/Toggle/Clear` — recon 5; keep the guard conditions at
  `EditorAppFrame.cpp:1868` verbatim).
- [ ] **Step 3:** Play-mode gameui through the graph ImGui node; Edit-mode
  input-reset semantics (`:1179`) preserved.
- [ ] **Step 4:** Both configs; gate. Commit:
  `feat(editor): pick + outline + gameui composite through the graph`

### Task 10: Editor chrome through ImGuiNri — the delegation flip completes

**Files:** Modify: `EditorAppFrame.cpp` `PresentFrame` (graph mode: the
editor's main window frame becomes a minimal graph frame — clear + one
ImGui node over the editor context's draw data → backbuffer → present,
via the host-window `NriGraphContext` from Task 6's machinery),
`ImGui/ImGuiLayer.cpp` (editor flavor same delegation as Task 6),
`EditorApp.cpp` (exit-code fold: device-lost → 1, latch growth → 2,
mirroring `RuntimeApp.cpp:1472-1481` — seam map §G.5 gap 3).

- [ ] **Step 1:** Chrome frame + present; fonts/atlas prove ImGuiNri under
  the editor's heavier ImGui load (docking, many windows).
- [ ] **Step 2:** Exit-code fold + `GpuFrameProgress`-equivalent heartbeat
  already publishing via Task 5 (verify the editor frame calls it — one
  line in the chrome frame).
- [ ] **Step 3:** Both configs; gate. Commit:
  `feat(editor): chrome renders through ImGuiNri -- delegation flip complete, exit codes folded`

### Task 11: Editor documents — ShaderEditorDocument preview + GraphGridPass

The editor-side data supply (seam map §C, category C).

**Files:** Modify: `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp`
(preview compile keeps producing blobs — the `createShader` sites
`:1231-1429` gain the same null-device guard + blob retention as Task 2's
caches; the preview RENDER `:3624-3730` targets a small
`CreateOffscreen` context), `ArcaneEditor/src/Widgets/GraphGridPass.hpp`
(same: grid renders through a tiny offscreen graph frame or is mode-gated
to a solid fill with a ledgered follow-up — implementer assesses which is
honest within the task; a blank-but-labeled grid is NOT acceptable, a
solid-color grid with the lines drawn by ImGui primitives IS an acceptable
fallback since the grid is chrome, record the choice), `EditorApp.cpp:387`
(toolbar logo via `NriTextureCache`), `EditorApp.cpp:1087-1108`
(`WriteAutoScreenshot` reads the graph offscreen output via the capture
path).

- [ ] **Step 1:** Preview compile guards + blob path (headless-testable).
- [ ] **Step 2:** Preview render + grid + logo + auto-screenshot on the
  offscreen mode.
- [ ] **Step 3:** Both configs; gate. Commit:
  `feat(editor): documents render through the graph -- previews, grid, logo severed from NVRHI`

### Task 12: SwitchProject + module rebuild in graph mode

**Files:** Modify: `EditorAppProject.cpp` (`switch_teardown` `:596-609`:
graph-mode equivalent of the `waitForIdle` at `:605` is the ordered
idle→invalidate→release→drain from `NriGraphContext.cpp:451-493`, applied to
BOTH contexts (viewport offscreen + chrome); `render_bridge` switch body
covers the graph trio), plus the module-rebuild poll (`:907-1005`) —
verify the restamp seam fires for ABI 13 and hot-reload works while the
graph renders (the plugin's DLL swap must not race a recorded frame: the
reload point sits at the frame top already, `EditorAppFrame.cpp:227` — pin
with a comment, not new machinery, unless the implementer finds a real
race, which is a BLOCKED-and-report).

- [ ] **Step 1:** Teardown/bridge stage bodies for graph mode; a scripted
  switch under `--frames` as the smoke (editor `--project A --frames 120`
  then switch via the recents path is NOT scriptable today — the honest
  in-session proof is the gate + review; the desk proof is D3's
  drive-checklist item "switch project twice, rebuild module once").
- [ ] **Step 2:** Both configs; gate. Commit:
  `feat(editor): project switch + module rebuild live on the graph path`

### Task 13: The editor golden harness + the honor list (ends at desk D3c)

Hard problem 5, built deliberately.

**Files:** Modify: `Host/HostConfig.hpp/.cpp` (no new flags; the change is
the EDITOR honoring the existing ones), `ArcaneEditor/src/main.cpp` +
`EditorApp.cpp/EditorAppFrame.cpp`: golden mode pins the editor clock
(`AdvanceSim` takes the fixed `1/60` dt under `GoldenMode()` — same pin as
`RuntimeApp.cpp:727,753-754`), runs the warm-up + census refusal (reuse
`DrainSceneCompiles` — move it from RuntimeApp's anon namespace to a shared
`Host/` helper verbatim), captures the VIEWPORT OUTPUT (recon 4) at
native resolution on the last frame, stage semantics: `batch` = viewport
scene only (no post, no outline/gameui), `post` = +post chain, `full` =
+outline/gameui composite (chrome never captured), naming
`editor-<stage>-<backend>.png` via the existing stem derivation with an
`editor-` prefix constant, `.actual/.diff` siblings + exit 3 identical to
`GoldenArtifact` (`RuntimeApp.cpp:123-186` — extract it to `Host/` and
share rather than duplicate).

- [ ] **Step 1:** Shared `GoldenArtifact` + `DrainSceneCompiles` extraction
  (verbatim moves; runtime behavior byte-identical — the floor).
- [ ] **Step 2:** Editor honor list: clock pin, warm-up, capture source per
  mode (NVRHI: `OffscreenCanvas` output via `ReadTexturePixels`; graph: the
  offscreen capture path), stage gating in the phase functions,
  HostConfigTest round-trips for the editor-relevant refusals.
- [ ] **Step 3:** Both configs; gate.
- [ ] **Step 4 — DESK CHECKPOINT D3c (USER):** editor NVRHI mode captures
  the `editor-*` baseline set (6 PNGs, both backends), self-compares
  `maxDelta 0`, commit; then editor GRAPH mode compares against them — the
  editor half of the 2D-parity gate. Commit:
  `feat(host): editor golden harness -- shared artifact machinery, editor baselines`

### Task 14: Desk milestone D3 (USER) — the Phase 3 exit

From Release AND Debug exe dirs, dx12 first, both backends per item:

- [ ] 1. Runtime graph mode (one device, host window): 6 stage compares vs
  the D3a set — all `golden PASS`, latch clean. NVRHI floor self-compares
  still `maxDelta 0`.
- [ ] 2. Editor graph mode: 6 `editor-*` stage compares vs the D3c set.
- [ ] 3. Editor desk-drive checklist: open ReferenceProject; select/deselect
  via click-pick (note the one-frame apply); outline correct; enter/leave
  Play with gameui; open the shader editor (preview renders); switch
  project twice; Build → Rebuild Game Module once (restamp fires, hot
  reload lands); no latch growth anywhere.
- [ ] 4. Drag-storms, both backends, both hosts' graph modes — native GPU
  markers PRESENT in the logs now; zero validation errors; heartbeats.
- [ ] 5. Diagnostics battery: `--crash-gpu` on runtime + editor graph modes
  → gpu-crash report with breadcrumbs/`.gpudump` at pre-port fidelity;
  pull-the-rug device-removed check if cheap (optional); confirm a
  `gpu-stall` (not plain `hang`) fires on a synthetic wedge if the fault
  shader produces one.
- [ ] 6. `--perf` on both hosts' graph modes: ≤~1ms waiting-frame.
- [ ] 7. Report; controller appends the milestone record (carries forward:
  Phase 5 deletion list grows GpuContext's NVRHI members, PickBuffer,
  SelectionOutline, OffscreenCanvas internals, ImGuiNvrhi, the injector's
  nvrhi half; plugin OffscreenCanvas-on-null-device carry; InfoQueue1
  re-adjudication result from D3b).

## Verification ladder (Phase 3 exit criteria)

1. `~[gpu]` gate green with the new suites (pixel cache, severance,
   offscreen shape, stall-rule reachability) — every task.
2. Desk D3a: textured baselines captured + committed; vehicle compares
   green (texture path proven in pixels).
3. Desk D3b: the one-device landing — runtime stage compares green in the
   host window; dx12 debug layer LIVE for the first time; markers back on;
   crash battery items pass.
4. Desk D3c: editor baselines + editor graph compares green.
5. Desk D3 (Task 14): the full 2D-parity gate — both hosts, both backends,
   desk-drive + diagnostics + storms + perf. THEN Phase 4 (the 3D slice)
   gets planned.

## Plan inputs resolved here / carried

- Texture residency exercise + fresh baselines: Tasks 2-3 (carry closed).
- Two-device residuals (`m_gpu->OnResize`, SetCurrentContext pin, second
  window, dispatcher rebinding, marker identity gate): Task 6 (closed).
- GpuFrameProgress + `--crash-gpu` on the graph path: Task 5 (closed).
- Per-window event routing: DROPPED, reconciliation 3.
- Remaining `[nvrhi]`-labeled producers: unchanged, Phase 5.
- ShaderRead widen tripwire, buffer same-state WAW rule, arena-helper
  extraction (all-three-or-none), GraphError dedup, material-id drain-order
  latent, stale-carry-after-failed-submit: unchanged, carried to Phase 4/5
  planning (none is load-bearing for this phase; the final review re-triages).
- InfoQueue1 dead-channel adjudication: RE-OPENED by Task 6 (the graph
  device becomes first-in-process); D3b decides the new truth.

## Phase 3 milestone record (appended 2026-08-18 at desk D3 GREEN)

**Phase 3 is COMPLETE.** Engine main `9651efc8..f7001ad9` (47 commits): Tasks
1-13 plus the sanctioned Task-8-pre enabler, executed via SDD — every task
implemented and reviewed, 12 in-loop fix rounds, one D3a shakedown wave, one
final whole-branch review (fable; 2 Critical + 4 Important) with its fix wave,
and three desk-driven fix rounds. Desk milestones D3a, D3b, D3c and D3 all
green.

**THE EDITOR AND THE RUNTIME BOTH RENDER THROUGH NRI.** `--nri-graph` now
means: no NVRHI device in the process, NRI wraps the only device, the swapchain
binds the HOST window. The editor is a full application on that path —
viewport, click-pick, JFA outline, game HUD, chrome, shader/material documents
with previews, project switch, module rebuild, and its own golden harness —
running two `NriGraphContext`s over one device with per-context graveyard
lanes. Gate at close: **47694 assertions / 1015 cases**, both configs.
Plugin ABI at close: **13**.

### D3 desk results (2026-08-18, RTX 3070, Win10 19045, 1280x720)

- **Stage compares:** 12/12 `maxDelta 0` across D3a/D3b/D3c against the frozen
  baselines — runtime `main-*` @c131692f (textured; replaces the Phase-2 set),
  editor `editor-*` @db648b4f. Debug runs under NRI validation on both backends
  plus VK sync validation. **NVRHI floor 6/6 `maxDelta 0`: the landing moved
  zero default-path pixels.**
- **Pick:** both `--pick-probe` runs HIT; editor click-pick driven by hand.
- **Crash parity:** `--crash-gpu` PASS on **both hosts x both backends** — exit
  1, device-removed verdict, `.arcdiag`/`.txt`/`.dmp`/`.gpudump` written.
  Artifacts land in the PROJECT's `Saved\Diagnostics`, not `<exe>\diagnostics`
  (the boot line names only where ARMING points).
- **Drag-storms:** clean on both backends, host window, debug layer live.
- **Perf:** runtime 4.12-4.19 ms @ ~240 Hz with present 4.04-4.10 = pure vsync
  wait; sim/rec/end/tone/imgui <=0.01 ms each. Editor perf **waived** — see
  amendment 4.
- **Drive checklist:** all 8 items pass — click-pick select/reselect/clear,
  outline, Play + game HUD, shader-editor preview + node grid, two project
  switches, one Build -> Rebuild Game Module, drag-resize, no latch growth.
- **Latch:** `RenderErrorCount B -> N` with `N == B` on every clean run.

### AMENDMENTS — the plan's text is WRONG where it contradicts these

1. **Native GPU markers are NOT at pre-port fidelity.** The crash-diagnostics
   Global Constraint demands `.gpudump` sections "at pre-port fidelity"
   (plan:54-58). The NRI marker layer is a declared later arc, so `.gpudump`
   has zero sections and queue timelines read `<none>`. `NativeDevice` is wired
   so the executor's cross-device gate opens when that arc lands. **Consciously
   renegotiated at Task 5, not missed.**
2. **Pick latency is 3 frames, not 1-2.** Reconciliation 5 says
   "one-to-two-frame" (plan:108-113); measured 3, and one of those is
   STRUCTURAL — the click is published in phase 16, after phase 10 has already
   declared that frame's graph, and no reordering preserves ImGui
   hover/focus/popup semantics (phase-6 raw clicks would break parity). Within
   recon 5's spirit (12.5 ms @ 240 Hz), outside its number (50 ms @ 60 Hz).
   **USER RATIFIED at the D3 drive, 2026-08-18: not perceptible, accepted.**
3. **InfoQueue1 is a DEAD CHANNEL** (the re-adjudication the plan assigned to
   D3b) — now for a concrete reason: the servicing `D3D12SDKLayers.dll` returns
   `E_NOINTERFACE` (0x80004002). Enabling the debug layer bought ENFORCEMENT,
   not OBSERVABILITY. NRI's own message callback is what carries dx12
   validation signal today — and it is what caught the 256 B CBV bug below.
4. **Editor `--perf` is WAIVED.** Plan item 6 ("`--perf` on both hosts' graph
   modes", plan:629) is **runtime-satisfied, editor-waived**. `--perf` has never
   done anything on the editor host in any commit: `EditorApp` constructs
   `FramePerf m_perf(m_config.perf)` and never calls `FrameStart`/`Add`/`Tick`.
   Not an NRI regression. Decisive: **no pre-port editor baseline exists**, so
   the number could never have proven non-regression, only stated an absolute —
   while `EditorAppFrame.cpp` is 3157 lines across 19+ declared phases onto
   which FramePerf's seven fixed buckets do not map. The item's real risk is
   already covered by D3c's editor compares. Fixed alongside: the flags table
   in `ArcaneEditor/src/main.cpp` wrongly listed `--perf` as *honoured*
   (@f50f3029).

### Engine defects only DESK verification could reach — four, all fixed

Every one of these sat behind a fully green gate and green golden compares.

1. **`FireFault` sized its CB/CBV to 16 B** where D3D12 requires a 256 B
   multiple -> `E_INVALIDARG` -> and that *recoverable* error killed the process
   because `MakeNriCallbacks` left `AbortExecution` null, which NRI silently
   fills with its own `DebugBreak()`. Two bugs in one. (@21759d73)
2. **`NRI_TIMEOUT_FENCE` is 5000 ms**, so `FenceD3D12::Wait` returned
   WAIT_TIMEOUT and `GetDeviceRemovedReason()` correctly answered `S_OK` — **the
   device was HUNG, not removed**. The probe read false, nothing disarmed, and
   teardown freed a descriptor pool the GPU was still executing against -> debug
   layer fail-fast (0x87D). (@1cd0d3f9)
3. **A VMA "unfreed dedicated allocations" abort on vulkan** that the
   intervening fix had itself introduced. (@1cd0d3f9)
4. **The `--nri-graph` editor could not be clicked AT ALL.**
   `ImGuiLayer::InitForGraph` withheld the SDL event tap — the ONLY route by
   which mouse BUTTON events reach ImGui — and that is the editor's PRIMARY
   context, so the whole chrome was dead: menus, panels, viewport pick, gizmo.
   The stance was correct when written and explicitly time-boxed ("until desk
   checkpoint D3b's compares are done"), and its comment even named the fix
   (`InitCommon(true)`); D3b closed and **the flip was missed**, while three
   other files had meanwhile hardened it into prose describing current fact.
   (@f7001ad9)

**THE LESSON, and it is this phase's most important output:** a green gate
proves nothing about either host — but this phase proved something sharper.
ArcaneTests never drives a real cursor, and **every scripted host run takes no
input at all** (`--frames`, `--screenshot`, `--golden-*`, `--perf`,
`--crash-gpu`). So no automated signal this phase could produce — not 1015
green cases, not 12 stage compares at maxDelta 0, not the NVRHI floor — was
capable of observing that the editor was unusable. It took one human click.
Desk verification is not ceremony.

### Phase 4/5 carry list (consolidated; the SDD ledger is DELETED — this is the surviving copy)

**Correctness carries**

- **`PluginHost::Reload` swaps the registry without bumping `m_sceneEpoch` or
  resetting `m_deferredPick`** — a pick armed before a hot reload can apply to
  the post-reload registry (wrong selection, never a fault). Pre-existing;
  correctly triaged and reported rather than widened at Task 12.
- **Buffer same-state WAW handovers emit no barrier** (frozen rule; texture
  handovers are exempt via the always-barrier override) — revisit at the first
  compute-heavy phase; do not rely on pooled-buffer same-state handovers.
- **ShaderRead stage-mask narrowed to VERTEX|FRAGMENT|COMPUTE** — widen
  tripwire pinned in code + tests; revisit at the first non-graphics consumer.
- **`PoolResource::carry` is updated during recording** — stale after a failed
  `EndCommandBuffer`/`QueueSubmit`; unreachable today (hosts stop on Failed),
  real if a host ever continues past a failed frame.
- **`kMaxSpriteTextures` is 64** (raised from 8); one linear scan is 65 entries.
  Real content pressure is untested.

**Named gaps / declared later arcs**

- **The native NRI marker layer** (amendment 1) — its own arc; `.gpudump`
  sections and queue timelines return when it lands.
- **The glyph-atlas texture gap**: atlases have no asset Guid, so
  `NriTextureCache` cannot key them.
- **Real aliasing needs BOTH an allocator swap AND an initializing-barrier
  story** (the Phase-2 handover fix removed the DISCARD hint).
- **Graph-mode project switch has no progress overlay and no window pump** —
  the window is frozen for the switch duration; the heartbeat still fires, so
  no false hang report. A real graph overlay is Task-13-sized: it must render
  before or after, never between.
- **Editor `full` golden exercises NO outline nodes.** C2 of the final review
  removed the live-mouse dependency (`HoverLive()`); whether editor `full`
  should script a selection the way the runtime scripts `--pick-probe` was
  left open at the site and is still the user's call.
- **Editor golden captures are viewport-only and LAYOUT-SIZED** (docked panel
  size, persisted per project GUID). Capture-vs-compare layout drift fails
  LOUD (exit 3 + dims MISMATCH). The imgui.ini-vetoes-authored-UI lesson
  applies to the golden layout itself.
- **Exit-code namespace collision**: Hub `launch.rs` maps `Some(2)`/`Some(3)`
  to pre-boot meanings inside its 2 s boot watchdog; golden exit 3 and latch
  exit 2 collide if a Hub-launched run exits fast. Guarded, not resolved.
- **Graph chrome background renders ~2x brighter/bluer** than the NVRHI arm
  (linear 0.02,0.02,0.04 through ACES vs direct 0.06,0.06,0.08) — cosmetic
  only; chrome is never captured. EXPECTED, not a finding.

**Cleanups worth one pass**

- `GraphError` helper duplicated in six TUs; the arena helper
  (`CbRegionStride`) duplicated in three — all-three-or-none.
- `Batcher2D` material-id drain order: two materials sharing
  (layer, orderInLayer) could reorder run-to-run.
- Remaining `[nvrhi]`-labelled producers to retag when NVRHI's callback dies.

**THE FOUR PHASE-2 PLAN-INPUT CORRECTIONS STILL BIND EVERY LATER PLAN** —
`CAN_ALIAS` does not exist in v180; root descriptors are unbuildable with
all-space0 shaders; descriptor-set CB views take no dynamic offsets (hence
per-frame-slot HOST_UPLOAD arenas); pool-handover barriers must carry the
previous tenant's REAL before-layout. Full text at the tail of
`docs/plans/2026-08-13-nri-phase2-framegraph-2d-cutover.md`.

**Operational**

- **Out-of-tree projects need restamp + rebuild at ABI 13.**
  `Gacha\Game\Aphelyon.arcproj` was restamped 12->13 and its Debug module
  rebuilt; a Release host session needs its own `/t:Rebuild` (single-slot
  `Binaries\`). Auto-restamp does NOT heal a stale stamp — it is a refusal.
- Desk batteries live on the Desktop (`D3a-`, `D3b-`, `D3b-Debug-Auto`, `D3c-`,
  `D3-Exit-Battery.ps1`). They are NOT in the repo.
- No GPU or windowed runs in an agent session (machine hazard: windowed d3d12
  SIGSEGVs under remote/locked sessions). The `~[gpu]` gate runs FROM the exe
  directory.

### What Phase 4 is (per the ladder, unchanged)

The foundational 3D slice: perspective camera, programmatic StaticMesh, one
forward-lit bindless pass — with these same frozen baselines as the 2D floor.
**Phase 5** then deletes NVRHI (GpuContext's NVRHI members, PickBuffer,
SelectionOutline, OffscreenCanvas internals, ImGuiNvrhi, the injector's nvrhi
half), flips the default path, rewrites and re-ratifies the boundary doc, and
re-runs the batteries.

**ULTIMATE GOAL unchanged:** unify 2D and 3D on ONE frame-graph path as the
foundation for the next game (an open-world multi-planet space game). Aphelyon
inherits via pixel-parity and never blocks. The renderer target is the Deadlock
contract (`docs/research/2026-08-12-deadlock-render-target.md`, tiers T1-T7).
