# NRI Phase 2: Frame Graph Core + 2D Cutover Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A declarative frame graph over the Phase 1 NRI substrate — barriers
derived, transients pooled through the CAN_ALIAS path, per-frame data through
upload rings — with every 2D pass (sprite batch, post chain, tonemap,
pick/outline, ImGui) ported as graph nodes, proven by stage-golden comparison
per node and the phase milestone: ReferenceProject full-2D goldens pass on
both backends through the graph.

**Architecture:** The graph is the organizing structure the whole port lands
in (spec § Frame graph). Phase 2 builds it beside the untouched NVRHI path:
a dev-only `--nri-graph` mode in ArcaneRuntime boots the REAL engine (project,
plugin, scene resolve) and renders through the graph, while NVRHI remains the
default path until Phase 3 flips the hosts. Nodes land in dependency order,
each verified against stage goldens captured from the NVRHI path first (the
Phase 0 "per-pass dumps" deviation comes due here). The graph API is sized
for Phase 2's nodes but shaped by the Deadlock chassis criteria (below) so
T1–T7 build on it without rework.

**Tech Stack:** NRI v180 (vendored, Phase 1), the Phase 1 substrate
(`NriDevice`, `NriSwapChain`, `Graveyard`, `ARC_NRI_CHECK`), offline-compiled
DXIL+SPIR-V shader bins via `ShaderLibrary`, in-process `ShaderCompiler` for
material stitching, Catch2 (`[nri]` headless / `[gpu]` desk), premake5.

## Global Constraints

- **Binding inputs, read before any task:** spec § Frame graph + § Phases
  item 2 + § Testing (`docs/specs/2026-08-12-nri-adoption-design.md`); the
  capability contract §7 ImGui ruling — PORT OURS
  (`docs/plans/2026-08-12-nri-capability-contract.md`); the Phase 1 milestone
  record (`docs/plans/2026-08-13-nri-phase1-substrate.md` tail).
- **Reference-code policy (Deadlock contract, BINDING — copied from
  `docs/research/2026-08-12-deadlock-render-target.md` § Reference-code
  policy):** Source 2 has no published or licensed source, ever. Leaked Valve
  code is **off-limits — never sought, never consulted, never referenced**.
  Legal channels only: ValveResourceFormat (MIT, attribution), Valve's
  published material, Unreal source READ-ONLY via the Epic-linked checkout
  (never copy into a non-UE engine), and the liftable list — notably
  **Filament (Apache-2.0), including its frame graph `filament/src/fg/`**,
  which is THIS plan's sanctioned design reference for graph architecture,
  and NRISamples (MIT, attribute where adapted).
- **Deadlock chassis criteria (graph-API acceptance list — T1–T7 must build
  on this graph without rework, but do NOT build speculative features):**
  raster nodes with N color attachments + optional depth (deferred GBuffer
  later); compute nodes first-class (T1 cluster grid; JFA today); nodes
  producing buffers consumed by later nodes (light grid later; pick readback
  today); transient pooling through CAN_ALIAS; imported PERSISTENT resources
  that survive frames (TAA history at T5; editor canvas at Phase 3);
  readback/copy nodes; single queue now, API not hard-wired against a second.
  Explicitly NOT now: async compute, multi-queue, pass culling, true memory
  aliasing (pool reuse only — the CAN_ALIAS flag is set so real aliasing is
  a Phase-4+ allocator swap, not an API break).
- **The goldens are the regression floor:** the NVRHI path must stay
  bit-green (`golden PASS ... maxDelta 0`) through every task. Nothing in
  this phase changes NVRHI behavior except where a task explicitly says so
  (Task 2's scene enrichment REPLACES the baseline goldens at a desk capture
  before any graph node lands).
- **Wrapper path only** (`nriCreateDeviceFrom*Device`); `nriCreateDevice`
  for a real backend anywhere = review defect. NONE-backend carve-out
  unchanged (tests only, commented).
- **All barriers inside graph code:** nodes NEVER record `CmdBarrier`; the
  executor derives and batches them. The two TEMP-barrier files from Phase 1
  (`NriSmoke.cpp`) are deleted by Task 13. The formal boundary-rule
  ratification lands at the port's merge (Phase 5), not here.
- **Volatile CBs / `writeBuffer`-in-open-list do not cross into graph code:**
  per-frame data goes through the Task 5 upload ring + root-descriptor
  offsets. The NRI Streamer stays out (spec non-goal).
- **Platform validation layers are the dev-loop gate** (NRI validation
  cannot catch barrier bugs): `--nri-graph` implies D3D12 debug layer + VK
  validation + sync validation in Debug, same wiring as the smoke; every
  validation error must land in `RenderErrorCount`; a graph desk run exits
  nonzero if the latch grew. Task 1 unblocks the dx12 half (InfoQueue1).
- New headless tests carry `[nri]` (inside the `~[gpu]` dev gate); GPU
  checks carry `[gpu]` or live in desk milestones. NONE-backend footguns:
  `MapBuffer` → null (upload-ring GPU tests are desk-only; its MATH is
  headless), `GetFenceValue` → always 0.
- New files → premake regen (`ThirdParty\premake5\premake5.exe vs2026`).
  Build both configs per task; full gate `ArcaneTests.exe "~[gpu]"` FROM THE
  EXE DIR. Engine repo `main`, commit per task, conventional commits.
- Fixed-function shaders stay offline-compiled (`data/shaders/
  compile-shaders.bat` → `ShaderLibrary`); material shaders stay stitched +
  in-process-compiled. The graph consumes the SAME bytecode; only PSO/
  binding creation moves. Register conventions unchanged
  (`ShaderConventions.hpp:17-36`, `Material/GlobalParams.hpp:16-25`).

## Reconciliations vs the spec's node sketch (recorded, not silent)

1. **"text" is not a separate node.** MSDF text is the third built-in
   pipeline inside `Batcher2D` (`kMaterialText`, `Batcher2D.cpp:35,187`) and
   `TextSystem` is constructed nowhere in production (tests only). The
   sprite-batch node carries all three built-in pipelines; text gets no
   dedicated node until a feature needs one.
2. **outline/pick are Phase 2 NODES with a scripted driver, Phase 3 WIRING.**
   The editor host integration (viewport panel, click-pick, PlayMode
   composite) is Phase 3 per the spec; Phase 2 builds `PickNode` +
   `OutlineNodes` as graph nodes and proves them via `--nri-graph` with a
   scripted selection on ReferenceProject content, stage-golden-compared.
3. **Stage goldens replace "per-pass dumps".** Phase 0 deferred per-pass
   capture to Phase 2. Rather than dumping linear RGBA16F intermediates,
   each stage golden is a FULL frame with later stages disabled (always
   display-referred backbuffer pixels, reusing the existing capture path,
   comparator, and tolerances). Stages: `batch` (post+HUD off), `post`
   (HUD off), `full`. Same flags drive both the NVRHI path (baseline
   capture) and the graph path (comparison).

## File structure (locked)

```
ArcaneClient/src/Arcane/Render/Nri/
  NriUploadRing.hpp/.cpp    per-frame-slot upload arenas + root-descriptor offsets (Task 5)
  RenderGraph.hpp/.cpp      resources, builder, node declaration, compile (Tasks 3-4)
  RenderGraphExec.cpp       executor: record over NriDevice, barriers, present (Task 6)
  NriPipelineCache.hpp/.cpp PSO cache keyed by (shader key, attachment formats) (Task 7)
  NriGraphContext.hpp/.cpp  the --nri-graph dev vehicle: device+swapchain+graph+capture (Task 7)
  nodes/
    Batch2DNode.hpp/.cpp        sprite/circle/msdf built-ins + registered materials (Tasks 8-9)
    FullscreenNodes.hpp/.cpp    post-chain passes + tonemap (Task 10)
    PickOutlineNodes.hpp/.cpp   entity-id, readback, JFA seed/iterate/composite (Task 11)
    ImGuiNriNode.hpp/.cpp       graph node wrapping the ported backend (Task 12)
ArcaneClient/src/Arcane/ImGui/
  ImGuiNri.hpp/.cpp         the ported backend (replaces ImGuiNvrhi at Phase 3 flip) (Task 12)
ArcaneClient/src/Arcane/Render/
  Device.hpp / DeviceD3D12.cpp   InfoQueue1 fix + NoteError tagging seam (Task 1)
ArcaneTests/src/
  RenderGraphTest.cpp       [nri] graph compile/barrier/lifetime/ring tests (Tasks 3-6)
ReferenceProject/Content/   scene enrichment: material sprite + post chain (Task 2)
```

Existing NVRHI TUs are READ but not modified in Phase 2 except:
`HostConfig.{hpp,cpp}` (stage flags, `--nri-graph`), `RuntimeApp.cpp` (the
`--nri-graph` branch + stage-flag consumption in the NVRHI path),
`DeviceD3D12.cpp`/`NvrhiMessageCallback.hpp`/`Device.hpp` (Task 1 only).

---

### Task 1: dx12 validation channel — InfoQueue1 root-cause + the tagging seam

The dev-loop gate for this whole phase is platform validation reaching the
latch. Desk fact: on Win10 19045 the smoke logs `ID3D12InfoQueue1
unavailable (pre-Agility D3D12 runtime)` while `Using ID3D12Device15` proves
Agility loaded — the WARN's diagnosis is wrong and dx12 debug-layer messages
never reach `RenderErrorCount`.

**Files:** Modify: `ArcaneClient/src/Arcane/Render/DeviceD3D12.cpp` (the
debug-layer enable + InfoQueue1 registration region, ~:227-281),
`ArcaneClient/src/Arcane/Render/NvrhiMessageCallback.hpp`,
`ArcaneClient/src/Arcane/Render/Device.hpp`.

**Interfaces (produces):** `NvrhiMessageCallback::NoteError(const char* tag,
const char* text) noexcept` — logs `ARC_ERROR("[{}] {}", tag, text)` and
bumps the same atomic the 0/0 gate reads, WITHOUT the device-removed
substring hook (kills the InfoQueue1 re-entrancy hazard AND the "[nvrhi]"
mislabeling in one seam). All Phase 2 graph code reports errors through
`ARC_NRI_CHECK` (unchanged) or `NoteError("nri-graph", ...)`.

- [ ] **Step 1:** Read the current enable/QI sequence in `DeviceD3D12.cpp`.
  Establish root cause with logging, not guesses. Known candidates, in
  probability order: (a) `D3D12GetDebugInterface`/EnableDebugLayer is not
  actually called on the smoke path (grep who consumes
  `RenderDeviceDesc::enableD3D12DebugLayer` in the creation half — if the
  flag never reaches `EnableDebugLayer`, the QI legitimately fails);
  (b) the QI runs before the debug layer is enabled; (c) SDKLayers
  genuinely lacks the interface on Win10 (falsifiable: the vendored
  `d3d12SDKLayers.dll` 1.619 exports it). Fix what you find; the WARN text
  must state the TRUE remaining failure cases only.
- [ ] **Step 2:** Add `NoteError` to `NvrhiMessageCallback` (atomic bump +
  tagged log, no substring hook; document why beside the existing
  `message()`), and reroute the InfoQueue1 callback through
  `NoteError("d3d12", ...)` for ERROR/CORRUPTION severities. Add a
  `D3D12_INFO_QUEUE_FILTER` storage filter keeping INFO/MESSAGE out of the
  callback (closes the Task 6 deferred minor).
- [ ] **Step 3:** `[nri]` test: `NoteError` bumps `RenderErrorCount` by
  exactly 1 and does not fire the device-removed hook (register a hook fn
  in-test, assert not called; reset latch after — follow the existing
  NriSubstrateTest reset idiom).
- [ ] **Step 4:** Build both configs; full gate. Desk proof is deferred to
  desk checkpoint D1 (Task 2): the smoke run must show the InfoQueue1 WARN
  GONE (or a corrected, truthful message) with the debug layer on.
- [ ] **Step 5:** Commit:
  `fix(render): dx12 debug-layer messages reach the latch -- InfoQueue1 root-cause + tagged NoteError seam`

### Task 2: Golden scene enrichment + stage flags + desk baseline capture (ends at desk checkpoint D1)

The current golden scene (3 plain SpriteRenderers, 0 materials, 0 post) does
not exercise the material-CB path or the post chain — the cutover would be
"proven" against content that never touches half the seams. Enrich the scene
BEFORE any graph node exists, recapture baselines on the NVRHI path at the
desk, commit them. These goldens are then FROZEN for the phase.

**Files:** Modify: `ReferenceProject/Content/scenes/main.arcscene` (+ new
`.arcmat` assets under `ReferenceProject/Content/`),
`ArcaneClient/src/Arcane/Host/HostConfig.{hpp,cpp}`,
`ArcaneRuntime/src/RuntimeApp.cpp`. Test: `ArcaneTests/src/HostConfigTest.cpp`.

**Interfaces (produces):** `HostConfig::goldenStage` — enum `GoldenStage
{ Full, Batch, Post }` parsed from `--golden-stage batch|post|full`
(default Full). Semantics in the runtime frame: `Batch` = skip post chain +
skip ImGui; `Post` = skip ImGui; `Full` = everything. Golden file naming:
`<name>-<stage>-<backend>.png` for batch/post, unchanged `main-<backend>.png`
for full (backward compatible).

- [ ] **Step 1:** Scene enrichment: add (a) one sprite using a registered
  sprite material with at least one numeric param animated by `Time` and one
  declared texture param (exercises `Batcher2D`'s per-material volatile CB +
  binding-set cache, `Batcher2D.cpp:565-641`), and (b) a two-pass post chain
  (`.arcmat` chain via `PostChainCache`) with visible effect (e.g. vignette +
  color tint — param values chosen so the effect is unmistakable in a diff).
  Deterministic under the pinned golden clock (fixed dt — verify the
  animated param uses sim time, which golden mode pins).
- [ ] **Step 2:** `--golden-stage` flag: parse (Dist-guarded beside the other
  golden flags, refuse unknown values at parse time), HostConfigTest
  round-trip case (all three values + default + rejection).
- [ ] **Step 3:** Wire stage semantics into `RuntimeApp::MainLoop`'s frame
  (`RuntimeApp.cpp:605-646`): `Batch` bypasses the post-chain call and the
  ImGui pass; `Post` bypasses ImGui only. Guard with `GoldenMode()` so
  normal runs are untouched.
- [ ] **Step 4:** Build both configs; full gate; NVRHI path unchanged for
  non-golden runs (the flag defaults Full and gates on GoldenMode).
- [ ] **Step 5:** Commit:
  `feat(golden): stage-golden flags + enriched reference scene -- material CB + post chain coverage`
- [ ] **Step 6 — DESK CHECKPOINT D1 (USER, blocks Task 8+, not Tasks 3-7):**
  1. Task 1 proof: Debug `--nri-smoke --backend dx12` — InfoQueue1 WARN gone
     (or truthful), then a deliberate-error probe if cheap.
  2. Capture new baselines (Release exe dir, both backends, per stage):
     `ArcaneRuntime --project <repo>\ReferenceProject --frames 120 --no-vsync
     --golden-capture <repo>\ReferenceProject\Goldens --golden-stage batch`
     (then `post`, then `full`) × `--backend dx12|vulkan` → 6 PNGs.
  3. Self-compare each (swap capture→compare): all `maxDelta 0`.
  4. Commit goldens:
     `test(golden): phase-2 stage baselines -- enriched scene, both backends`

### Task 3: RenderGraph declaration API (headless, TDD)

**Files:** Create: `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.hpp/.cpp`.
Test: `ArcaneTests/src/RenderGraphTest.cpp` (new, `[nri]`).

**Interfaces (produces — the phase's core contract, consumed by every later
task):**

```cpp
// RenderGraph.hpp (namespace Arcane) — declaration side. No nri calls in
// this TU beyond desc types; compile/execute live in Tasks 4/6.
struct RgTexture { std::uint32_t index = kInvalid; };   // value handles
struct RgBuffer  { std::uint32_t index = kInvalid; };

struct RgTextureDesc {           // subset of nri::TextureDesc the graph owns
    nri::Format format = nri::Format::UNKNOWN;
    std::uint32_t width = 0, height = 0;
    bool depthStencil = false;   // chooses attachment vs shader usage bits
};

enum class RgUsage : std::uint8_t {                     // per-declaration
    ColorWrite, DepthWrite, ShaderRead, ShaderWriteCs,  // (UAV, compute)
    CopySrc, CopyDst, Present, ReadbackHost
};

class ARCANE_API RenderGraphBuilder {                   // handed to setup fns
public:
    RgTexture CreateTexture(const char* name, const RgTextureDesc&); // transient
    RgTexture ImportTexture(const char* name, nri::Texture*,
                            nri::AccessLayoutStage entry,
                            nri::AccessLayoutStage exit,
                            bool persistent);           // swapchain, history, canvas
    RgBuffer  CreateBuffer(const char* name, std::uint64_t size,
                           nri::BufferUsageBits);       // transient
    RgBuffer  ImportBuffer(const char* name, nri::Buffer*, std::uint64_t size);
    void Read (RgTexture, RgUsage);  void Read (RgBuffer, RgUsage);
    void Write(RgTexture, RgUsage);  void Write(RgBuffer, RgUsage);
};

struct RenderGraphNodeContext {                         // handed to exec fns
    nri::CommandBuffer& cmd;
    const nri::CoreInterface& core;
    nri::Texture*   Resolve(RgTexture) const;
    nri::Descriptor* ColorView(RgTexture) const;        // cached attachment view
    nri::Buffer*    Resolve(RgBuffer)  const;
    NriUploadRing&  ring;                               // Task 5
    NriPipelineCache& pipelines;                        // Task 7
};

class ARCANE_API RenderGraph {
public:
    using Setup = std::function<void(RenderGraphBuilder&)>;
    using Exec  = std::function<void(RenderGraphNodeContext&)>;
    enum class NodeKind : std::uint8_t { Raster, Compute, Copy };
    void AddNode(const char* name, NodeKind, Setup, Exec);
    // Raster nodes additionally declare attachments (formats live in PSOs):
    void SetColorAttachments(std::span<const RgTexture>);  // for current node
    void SetDepthAttachment(RgTexture);
    // Compile/Execute in Tasks 4/6. Reset() clears for the next frame's build.
    void Reset();
};
```

Design reference (sanctioned, Apache-2.0): Filament `filament/src/fg/` —
read `FrameGraph.h`/`PassNode`/`ResourceNode` for the declaration/virtualize
split; our version is deliberately smaller (no subresources, no culling).
One-line attribution comment at RenderGraph.hpp's head.

- [ ] **Step 1:** Failing `[nri]` tests in `RenderGraphTest.cpp` (pure
  declaration invariants, no device): handles are unique + name-retrievable;
  transient vs imported classification; a Read of a never-written transient
  is a compile-refusal precondition recorded at declaration (assert-later
  model); Raster nodes require ≥1 attachment; node order preserved as
  declared (execution order = declaration order — no reordering in Phase 2,
  document it).
- [ ] **Step 2:** Regen, build, tests fail; implement the declaration side
  (storage: flat vectors of NodeDesc/ResourceDesc; `Reset()` = clear +
  generation bump so stale handles assert in debug). Run `[nri]`, full gate.
- [ ] **Step 3:** Commit: `feat(nri): RenderGraph declaration API -- nodes, transients, imports`

### Task 4: Graph compile — derived barriers + transient lifetimes (headless, TDD)

**Files:** Modify: `RenderGraph.hpp/.cpp`. Test: `RenderGraphTest.cpp`.

**Interfaces (produces):**

```cpp
struct RgBarrier {                      // pure data, unit-testable
    std::uint32_t resourceIndex; bool isTexture;
    nri::AccessLayoutStage before, after;
};
struct RgCompiledNode {
    std::uint32_t nodeIndex;
    std::vector<RgBarrier> preBarriers;  // batched: one CmdBarrier group per node
};
struct RgCompiled {
    std::vector<RgCompiledNode> nodes;
    std::vector<RgBarrier> exitBarriers;         // imported exits (e.g. Present)
    struct Lifetime { std::uint32_t first, last; };
    std::vector<Lifetime> transientLifetimes;    // index-parallel to transients
};
RgCompiled RenderGraph::Compile();      // pure: no nri device calls
```

Barrier derivation rules (each is a test): usage → (access, layout, stage)
mapping table (ColorWrite→COLOR_ATTACHMENT, ShaderRead→SHADER_RESOURCE+ALL
relevant stages, ShaderWriteCs→SHADER_RESOURCE_STORAGE+COMPUTE, CopySrc/Dst,
Present, ReadbackHost→COPY_DEST on a host-readable buffer); consecutive same-
state declarations produce NO barrier; write→read and read→write edges
produce exactly one; imported entry state seeds the chain and exit state
appends to `exitBarriers`; transient first-use needs no "from UNDEFINED"
special case beyond `before = {UNKNOWN, UNDEFINED}`.

- [ ] **Step 1:** Failing tests: (a) linear chain canvas: A writes color,
  B reads shader, C writes color again → 2 barriers, correct
  before/after; (b) no-op edge (two reads back-to-back) → 0 barriers;
  (c) import entry/exit (swapchain: entry UNDEFINED→ColorWrite in node 1,
  exit →PRESENT in exitBarriers); (d) transient lifetimes: first/last node
  indices correct for straight-line and skip-a-node usage; (e) two
  transients with disjoint lifetimes + identical desc get the SAME pool
  slot id, overlapping lifetimes get different ids (pool assignment is part
  of compile output: add `std::vector<std::uint32_t> transientPoolSlot`);
  (f) compile-refusal: reading a never-written transient returns an error
  (Compile returns std::optional or asserts — pick std::optional<RgCompiled>
  + error string, tests assert the message names the resource).
- [ ] **Step 2:** Implement; run `[nri]`, full gate.
- [ ] **Step 3:** Commit: `feat(nri): graph compile -- derived batched barriers, transient lifetimes + pool slots`

### Task 5: NriUploadRing (headless math, TDD)

Replaces every volatile-CB/`writeBuffer`-in-open-list idiom on the graph
path. One ring per queued frame slot (`kSwapchainFramesInFlight`), each a
persistent UPLOAD-heap `nri::Buffer` mapped once; `Allocate` bumps an offset.

**Files:** Create: `ArcaneClient/src/Arcane/Render/Nri/NriUploadRing.hpp/.cpp`.
Test: `RenderGraphTest.cpp`.

**Interfaces (produces):**

```cpp
class ARCANE_API NriUploadRing {
public:
    struct Alloc { nri::Buffer* buffer; std::uint64_t offset; void* cpu; };
    // slotBytes per frame slot; alignment defaults to
    // deviceDesc.memoryAlignment.constantBufferOffset for CB allocs.
    bool Init(NriDevice&, std::uint64_t slotBytes);
    void BeginFrame(std::uint32_t frameSlot);   // resets that slot's offset
    Alloc Allocate(std::uint64_t size, std::uint64_t align);
    // returns {nullptr,0,nullptr} on overflow — caller ARC_NRI_CHECK-style
    // reports via NoteError("nri-graph", ...) and drops the draw; never UB.
    std::uint64_t HighWater(std::uint32_t slot) const;  // logged at shutdown
};
```

- [ ] **Step 1:** Failing tests for the OFFSET MATH ONLY (no device: factor
  the bump-allocator into a pure `RingLayout` struct the class wraps —
  `RingLayout::Allocate(size, align)` → offset or overflow): alignment
  rounding, exact-fit, overflow returns failure not wrap, BeginFrame resets,
  per-slot independence, high-water tracking.
- [ ] **Step 2:** Implement `RingLayout` + the NRI-facing wrapper (buffer
  creation UPLOAD memory, persistent map — NONE returns null from MapBuffer,
  so the wrapper's device tests are `[gpu]`-tagged desk items; document).
  Run `[nri]`, full gate.
- [ ] **Step 3:** Commit: `feat(nri): NriUploadRing -- per-slot upload arenas, overflow-safe`

### Task 6: Graph executor over NriDevice (+ present ownership)

**Files:** Create: `ArcaneClient/src/Arcane/Render/Nri/RenderGraphExec.cpp`
(execution methods of RenderGraph). Modify: `RenderGraph.hpp`. Test:
`RenderGraphTest.cpp` (NONE-backend integration).

**Interfaces (produces):**

```cpp
struct RgExecuteDesc {
    NriDevice& device;                 // queue, CoreInterface, graveyard
    NriSwapChain* swapChain;           // optional: null = headless/offscreen
    NriUploadRing& ring;
    NriPipelineCache& pipelines;       // Task 7 (forward-declared; NONE tests pass a stub)
    std::uint32_t frameSlot;
};
// Records ALL nodes into one command buffer: for each compiled node, emit
// its batched preBarriers (ONE CmdBarrier call), CmdBeginRendering for
// Raster kinds (attachments from declarations), run exec fn, CmdEndRendering;
// then exitBarriers; submit; if swapChain, Present. Acquire happens INSIDE
// Execute when a node imported the swapchain texture (the graph owns
// acquire/present sequencing — NriSwapChain's "no Resize between Acquire
// and Present" caller contract is retired for graph users: Execute never
// resizes, and the frame driver resizes strictly between Execute calls).
bool RenderGraph::Execute(const RgExecuteDesc&, const RgCompiled&);
```

Transient realization: pool textures/buffers created lazily per pool slot
(desc + CAN_ALIAS flag set), owned by the graph, destroyed through the
device's `Graveyard` keyed to the frame's fence value on `Reset()`/resize.

Diagnostics carry-over (spec § Swapchain + pacing — a GATE, not a hope):
the executor brackets every node with the existing breadcrumb ring
(`GpuPassScope`-equivalent: `pass:<node name>` CPU breadcrumbs now; native
GPU markers through `GetCommandBufferNativeObject` at the same seam —
follow `GpuInstrumentation.hpp:100-128`'s shape), and every
submit/acquire/present result is checked for typed
`nri::Result::DEVICE_LOST` → the existing `ObserveDeviceRemoved` → latch
chain (upgrading Phase 1's message-substring-only observation on the graph
path). Device-loss during the drag-storm must produce the same `.arcdiag`
artifacts the NVRHI path does.

- [ ] **Step 1:** Failing `[nri]` NONE-backend integration tests: build a
  3-node graph (write→read→copy) on a NONE device, Compile, Execute with a
  null swapchain and a stub pipeline cache; assert: executes without latch
  growth; transient pool creates exactly the pool-slot count from compile
  (expose `DebugTransientCount()`); a second Execute on the same compiled
  graph creates ZERO new resources (pool reuse); Reset() buries transients
  in the graveyard (Pending() grows; Reap releases; NONE GetFenceValue is 0
  — feed values manually).
- [ ] **Step 2:** Implement. Barriers: translate RgBarrier lists to
  `nri::BarrierGroupDesc` verbatim — the executor is the ONLY CmdBarrier
  call site in the codebase's graph path (grep-assert in review).
- [ ] **Step 3:** Run `[nri]`, full gate, both configs.
- [ ] **Step 4:** Commit: `feat(nri): graph executor -- batched barriers, transient pool + graveyard, present ownership`

### Task 7: NriPipelineCache + the `--nri-graph` vehicle (clear-only frame)

**Files:** Create: `ArcaneClient/src/Arcane/Render/Nri/NriPipelineCache.hpp/.cpp`,
`ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp/.cpp`. Modify:
`HostConfig.{hpp,cpp}` (flag `--nri-graph`, non-Dist, refuse combining with
`--nri-smoke`), `RuntimeApp.cpp` (branch in `MainLoop`, NOT a pre-boot
early-return: boot proceeds normally — project, plugin, scene resolve — and
the RENDER half swaps), `ArcaneTests/src/HostConfigTest.cpp`.

**Interfaces (produces):**

```cpp
class ARCANE_API NriPipelineCache {    // formats live in PSOs (spec)
public:
    struct GraphicsKey {
        std::uint64_t shaderPairId;    // ShaderLibrary ids or material hash
        std::uint32_t layoutId;        // pipeline-layout id (registered below)
        std::array<nri::Format, 4> colorFormats; nri::Format depthFormat;
        std::uint8_t colorCount; /* blend/topology packed as needed */ };
    std::uint32_t RegisterLayout(const nri::PipelineLayoutDesc&); // dedup'd
    nri::Pipeline* GetGraphics(const GraphicsKey&, /*create-info callback*/
                               const std::function<void(nri::GraphicsPipelineDesc&)>& fill);
    nri::PipelineLayout* Layout(std::uint32_t id);
    void Clear(Graveyard&, std::uint64_t fence);   // project switch / shutdown
};
// NriGraphContext: owns NativeDeviceOwner + NriDevice + NriSwapChain +
// ring + pipeline cache + RenderGraph; per frame: BuildGraph(callback) →
// Compile → Execute; handles resize between frames (window event, never
// mid-execute); capture: a ReadbackHost node on the final backbuffer state
// writes RGBA-swizzled PNG via WritePngRgba (adapt NriSmoke's swizzle —
// this satisfies the Format()-aware golden rule: capture normalizes to
// RGBA bytes regardless of swapchain channel order).
```

- [ ] **Step 1:** HostConfigTest round-trips for `--nri-graph` (+ mutual
  exclusion with `--nri-smoke`; golden flags ARE allowed with `--nri-graph`
  — that's the whole point — remove/scope the Task 9 Phase-1 parse refusal
  so it applies to `--nri-smoke` only).
- [ ] **Step 2:** Implement cache + context; wire the RuntimeApp branch:
  frame = clear-the-backbuffer graph (one Raster node importing the
  swapchain texture, no draws) + capture/golden plumbing reusing
  `--frames/--screenshot/--golden-*/--golden-stage` semantics on the graph
  path (stage flags select which nodes Tasks 8-12 add).
- [ ] **Step 3:** Validation wiring identical to the smoke (Debug: debug
  layer + VK sync validation; exit nonzero on latch growth). Build both
  configs; full gate (headless untouched; the branch is flag-gated).
- [ ] **Step 4:** Commit: `feat(nri): --nri-graph vehicle -- pipeline cache, clear frame, graph-path golden plumbing`

### Task 8: Batch2DNode — built-in pipelines (sprite/circle/msdf)

Port `Batcher2D`'s draw half as a graph node. The CPU batching side
(`Quad/Circle/Line/...` accumulation, `Batcher2D.cpp` up to `End()`) is
REUSED, not rewritten: factor the vertex/index CPU arrays + draw-span list
behind a read interface (`Batcher2D::Drained() → spans of {pipelineId,
textureId, vertexRange}`) consumed by both the NVRHI `End()` (unchanged
behavior) and the new node. Extract, don't redesign — the NVRHI path stays
bit-identical (floor).

**Files:** Create: `nodes/Batch2DNode.hpp/.cpp`. Modify: `Batcher2D.hpp/.cpp`
(the read-interface factoring ONLY), `NriGraphContext.cpp` (add node when
stage ≥ batch). Test: existing gate (batcher CPU tests unchanged) + desk.

**Interfaces (consumes):** RenderGraph/Ring/PipelineCache from Tasks 3-7;
ShaderLibrary bytecode (`sprite_vs/ps`, `circle_vs/ps`, `msdf_vs/ps` — same
bins, `ShaderLibrary.cpp:41-56`); push-constant struct
`PushConstants{invHalfViewport}` (`Batcher2D.cpp:29-33`) becomes NRI root
constants; VB/IB become per-frame ring allocations (vertex data path:
`Batcher2D.cpp:341-368` semantics, ring instead of writeBuffer).
**Produces:** `AddBatch2DNode(RenderGraph&, NriGraphContext&, RgTexture
canvas)` — draws into the RGBA16F canvas transient (matches `Canvas.cpp:9`).

- [ ] **Step 1:** Factor the batcher read interface (NVRHI `End()` consumes
  it; run full gate + confirm zero behavior change — the golden floor run
  at D2 is the pixel proof).
- [ ] **Step 2:** Implement the node: canvas transient (RGBA16F,
  swapchain-sized), built-in PSOs via cache (formats from declaration),
  white-texel texture + sampler descriptor set (mirror `Batcher2D.cpp:116-180`),
  ring VB/IB, root constants. Graph wiring under `--golden-stage batch`:
  canvas → tonemap is NOT live yet, so Task 8's stage output tonemaps via a
  MINIMAL direct blit? NO — keep honest: Task 8 lands with Task 10's
  tonemap node stubbed as a straight `tonemap_vs/ps` node (the real shader,
  no post chain), because the stage golden is display-referred backbuffer.
  Declare this dependency explicitly: Task 8's desk compare needs tonemap,
  so implement the TonemapNode here (it is 40 lines: fullscreen triangle,
  canvas ShaderRead → backbuffer ColorWrite, same `tonemap_vs/ps` bins from
  `TonemapPass.cpp:39-73`) and Task 10 then only adds the post CHAIN.
- [ ] **Step 3:** Build both configs; full gate. Desk-optional checkpoint:
  `--nri-graph --golden-stage batch --golden-compare ...` vs the D1 batch
  baseline, both backends (user MAY run now; mandatory at D2).
- [ ] **Step 4:** Commit: `feat(nri): Batch2DNode built-ins + TonemapNode -- batch stage renders through the graph`

### Task 9: Batch2DNode — registered sprite materials

**Files:** Modify: `nodes/Batch2DNode.hpp/.cpp`, `NriGraphContext.cpp`.

**Interfaces (consumes):** material registration data from
`SceneRenderResolver`/`SpriteMaterialCache` (Guid → stitched shader blobs +
`MaterialInstance` params, `SceneRenderResolver.cpp:155-287`); slot map
`GlobalParams.hpp:16-25` (sprite path: push consts b0, material CB b1,
globals CB b2, textures t1+). Material CBs + globals CB go through the ring
with root-descriptor offsets (`CmdSetRootDescriptor`); declared textures →
per-material descriptor sets cached like `Batcher2D.cpp:597-641`.

- [ ] **Step 1:** Extend the node: material pipelines from the cache keyed
  by material hash + canvas format; `MaterialInstance::PackCB` into ring
  allocs (pack-once-per-batch dedup mirrors `Batcher2D.cpp:378-392`).
- [ ] **Step 2:** Pipeline-layout shape: root constants (b0) + two root
  descriptors (material b1, globals b2) + texture set — register once in
  the cache; assert the stitched SPIR-V's register shifts match
  `vkBindingOffsets` (they do by construction — same compile args — note
  where it's asserted in Phase 1's derivation).
- [ ] **Step 3:** Build; gate. Desk-optional: batch stage compare now covers
  the material sprite (this is why D1 enriched the scene).
- [ ] **Step 4:** Commit: `feat(nri): Batch2DNode registered materials -- ring CBs via root descriptors`

### Task 10: Post-chain nodes

**Files:** Create: `nodes/FullscreenNodes.hpp/.cpp` (chain; TonemapNode
moved here from Task 8 if cleaner — mechanical). Modify: `NriGraphContext.cpp`.

**Interfaces (consumes):** `PostChainCache` compiled chain entries (same
stitched blobs the NVRHI `FullscreenMaterialChain` uses); slot map
`kMaterialCbSlot=0`/`kGlobalCbSlot=1`. **Produces:** `AddPostChainNodes(...)`
— one graph node per chain pass, ping-ponging TRANSIENT RGBA16F targets
(this is the transient-pool exercise: N passes alternate two pool slots),
final output feeds TonemapNode input.

- [ ] **Step 1:** Implement chain nodes (fullscreen triangle each, material
  CB + globals via ring root descriptors, source texture of pass k = target
  of pass k-1 — declared reads/writes derive the barriers, zero hand code).
- [ ] **Step 2:** Wire under `--golden-stage post|full`. Build; gate.
  Desk-optional: post stage compare vs D1 baseline.
- [ ] **Step 3:** Commit: `feat(nri): post-chain graph nodes -- transient ping-pong, derived barriers`

### Task 11: Pick + outline nodes (scripted driver)

Editor WIRING is Phase 3; the NODES land here, driven by `--nri-graph
--pick-probe x,y` and a scripted selection (first entity in the scene) so
they render real content and are desk-comparable.

**Files:** Create: `nodes/PickOutlineNodes.hpp/.cpp`. Modify:
`NriGraphContext.cpp`, `HostConfig.{hpp,cpp}` (+`--pick-probe`, non-Dist,
prints the picked id and exits 0/1 on hit/miss — a scriptable desk check).

**Interfaces (consumes):** `entity_id_vs/ps`, `outline_seed/jfa/composite`
bins (same offline bins; pipeline shapes mirror `PickBuffer.cpp:308-367` and
`SelectionOutline.cpp`); ReadbackHost node pattern (NRISamples Readback,
MIT, attribute). **Produces:** `AddPickNode(...)` (R32_UINT transient +
1-pixel readback buffer import), `AddOutlineNodes(...)` (seed →
`ceil(log2(maxDim))` JFA iterations ping-ponging two RGBA16_SNORM transients
→ composite onto the post-tonemap target — matches editor compositing order,
`EditorAppFrame.cpp:1215-1256`).

- [ ] **Step 1:** Implement pick node + probe flag (readback drains at
  frame end through the graph's readback path — no waitForIdle stall like
  `PickBuffer.cpp:177-219`; latency 1 frame is fine for the probe).
- [ ] **Step 2:** Implement outline nodes (JFA step count computed from
  extent; per-step push constants = step size).
- [ ] **Step 3:** Build; gate (all desk-proof; graph compile tests already
  cover the multi-transient shapes headlessly). Desk-optional: outline
  visual eyeball + `--pick-probe` hit on a known pixel, both backends.
- [ ] **Step 4:** Commit: `feat(nri): pick + JFA outline nodes -- readback path, scripted probe`

### Task 12: ImGui backend port (PORT OURS — contract §7)

**Files:** Create: `ArcaneClient/src/Arcane/ImGui/ImGuiNri.hpp/.cpp`,
`nodes/ImGuiNriNode.hpp/.cpp`. Modify: `NriGraphContext.cpp` (full stage).
ImGuiLayer/OffscreenImGuiLayer glue is UNTOUCHED (they delegate; the
delegation target changes at Phase 3's flip, not now).

**Interfaces (consumes):** `imgui_vs/ps` bins; ImGui draw data
(`ImDrawData`); ring for VB/IB + font upload staging; contract §7's finding
that ONLY `ImGuiNvrhi.{hpp,cpp}` needs GPU changes — `ImGuiNri` mirrors its
365-line surface: font atlas texture, one pipeline (backbuffer format from
cache key), scissor per draw cmd, `ImTextureID` = descriptor convention
matching §7.3 (keep the existing convention so the game-debug-ImGui path
survives Phase 3 unchanged).

- [ ] **Step 1:** Implement `ImGuiNri` (Init/NewFrameTexUpdates/
  RenderDrawData against a NodeContext) + the node (reads ImDrawData
  captured pre-execute; draws onto the backbuffer import after tonemap).
- [ ] **Step 2:** Wire under `full` stage in the `--nri-graph` frame with
  the SAME HUD content the runtime NVRHI path draws (the D1 full baseline
  includes the HUD — parity requires it).
- [ ] **Step 3:** Build; gate. Commit:
  `feat(nri): ImGuiNri backend + graph node -- ported per contract SS7`

### Task 13: Delete the smoke (scaffolding retirement)

**Files:** Delete: `NriSmoke.hpp/.cpp`. Modify: `HostConfig.{hpp,cpp}`
(remove `--nri-smoke` + its tests), `RuntimeApp.cpp` (remove the pre-boot
branch), `ArcaneTests/src/HostConfigTest.cpp`.

- [ ] **Step 1:** Delete; migrate the two desk commands in its header to
  `--nri-graph` equivalents documented in `NriGraphContext.hpp`'s header.
  The parked idle-hang and the two TEMP-barrier files die here. The
  `[gpu] nri wrap smoke` TEST stays (deletion point Phase 5, per its
  comment). Grep: no `nri-smoke` token outside docs/.
- [ ] **Step 2:** Regen, build both configs, full gate.
- [ ] **Step 3:** Commit: `chore(nri): retire --nri-smoke -- the graph vehicle replaces the triangle scaffolding`

### Task 14: Desk milestone D2 (USER) — the Phase 2 exit

From Release AND Debug exe dirs (Debug = validation layers; every run must
show zero latch growth):

- [ ] 1. Stage compares, both backends, Debug:
  `ArcaneRuntime --nri-graph --project ..\..\..\ReferenceProject --frames 120
  --no-vsync --golden-compare <repo>\ReferenceProject\Goldens
  --golden-stage batch` → then `post` → then `full`, × `--backend dx12|vulkan`
  — all `golden PASS`. Tolerances are Phase 0's (2, 0.1%); on this GPU
  expect `maxDelta 0` (record actuals in the milestone note; a small
  nonzero delta on the graph path is a FINDING to explain, not shrug at —
  suspects: tonemap constant packing, sRGB-vs-G22 swapchain typing,
  RGBA/BGRA swizzle in capture).
- [ ] 2. **NVRHI floor**: plain (no `--nri-graph`) self-compares still
  `maxDelta 0` both backends (Task 8's batcher factoring is the only
  NVRHI-touching change; this is its pixel proof).
- [ ] 3. `--pick-probe` on a known-entity pixel: hit, exit 0; outline
  eyeball on both backends.
- [ ] 4. Vulkan drag-storm on `--nri-graph` (no `--frames`): resize storm,
  no crash, no validation errors, latch clean on exit.
- [ ] 5. `--perf` pacing eyeball on the graph path (≤~1ms waiting-frame
  budget, spec Testing).
- [ ] 6. Report; controller appends the milestone record + commits any
  updated stage goldens ONLY if a legitimate calibration change occurred
  (default: none — the D1 set is frozen).

## Verification ladder (Phase 2 exit criteria)

1. `~[gpu]` gate green with the graph suite in it (declaration, compile/
   barrier/lifetime/pool, ring math, NONE-backend execute).
2. Desk D1: enriched-scene stage baselines captured + committed, bit-green
   self-compares; InfoQueue1 fixed (dx12 validation channel live).
3. Desk D2: all three stage goldens PASS through the graph on both
   backends; NVRHI floor still bit-green; drag-storm + pick probe + perf
   eyeball clean.
4. THEN Phase 3 (host integration) gets planned: GpuContext/OffscreenCanvas
   internals move onto NriGraphContext, editor viewport + pick wiring, the
   ImGuiLayer delegation flip, SwitchProject/module-rebuild lifecycle.

## Plan inputs resolved here / carried

- Spec's "text" node: folded into Batch2DNode (reconciliation 1).
- Latch tagging + InfoQueue1 + D3D12 info-queue filter: Task 1 (closes three
  deferred minors and the desk follow-up).
- Format()-aware goldens: capture-side RGBA normalization (Task 7) — the
  comparator stays byte-wise on RGBA PNGs.
- Present-sequencing ownership: graph executor (Task 6) — NriSwapChain's
  caller contract retired for graph users; the parked smoke idle-hang dies
  with Task 13.
- Carried to Phase 3: host flips (GpuContext/OffscreenCanvas/ImGuiLayer
  delegation), editor pick/outline wiring, `--golden-stage` on the editor
  host, remaining "[nvrhi]"-labeled producers (NVRHI's own callback keeps
  its tag until Phase 5 deletes it).
- Carried to Phase 5: NVRHI deletion, boundary-doc rewrite + ratification,
  zero-legacy sweep, battery re-run, `NativeDeviceOwner` export narrowing,
  `kSwapchainFramesInFlight` relocation.
