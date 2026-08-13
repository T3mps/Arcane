# NRI Phase 1: Substrate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** NRI vendored and building; the fence-tagged graveyard live and
headless-tested; our device creation upgraded to the wrapper capability
contract and wrapped by NRI; NRISwapChain + timeline-fence pacing proven by
a clear+triangle smoke on BOTH backends with platform validation layers
clean — while the existing NVRHI 2D path keeps rendering bit-identically
(the Phase 0 goldens are this phase's regression floor).

**Architecture:** All new code lands in `ArcaneClient/src/Arcane/Render/Nri/`
beside the untouched NVRHI path — not a dual RENDER path (nothing renders
through NRI except the dev-only smoke) but the substrate Phase 2's frame
graph will consume. The smoke (`--nri-smoke`, non-Dist) creates its OWN
native device via the refactored creation halves, wraps it with
`nriCreateDeviceFrom*Device`, and draws one triangle with hand-written
barriers that are explicitly TEMPORARY (Phase 2's graph absorbs them; the
boundary-rule ratification lands at the port's merge, not here).

**Tech Stack:** NRI v180 (vendored from `.example/NRI`, tag v180 commit
4b48531), VMA + D3D12MA (new vendors), DirectX-Headers + Vulkan-Headers
(already vendored), Agility SDK (new, NuGet redistributable), premake5,
Catch2, our ShaderCompiler (in-process dxc, already dual-targets DXIL and
SPIR-V — `ShaderCompiler.cpp:451/:463`).

## Global Constraints

- **Binding inputs, read before any task:** the spec's Phase 1 bullet +
  Device layer section (`docs/specs/2026-08-12-nri-adoption-design.md`) and
  the capability contract (`docs/plans/2026-08-12-nri-capability-contract.md`)
  — the contract's **§5 numbered 15-edit list is normative**; every device
  edit cites its row. §6 lists its open checks (Agility log line).
- **The Phase 0 goldens are the regression floor:** nothing in this phase
  may change a pixel of the NVRHI path. The milestone desk run re-executes
  the golden self-compares (both backends) and they must stay bit-green.
- Wrapper path ONLY (`nriCreateDeviceFrom*Device`); a call to
  `nriCreateDevice` anywhere is a review defect (spec ratification 2).
- D3D11 and WGPU backends are never vendored (spec; `feedback_no_legacy_graphics_apis`).
- New headless tests carry `[nri]` (in the `~[gpu]` dev gate); GPU/desk
  checks carry `[gpu]` or live in the milestone checklist. NONE-backend
  footguns respected: `MapBuffer` → null, `GetFenceValue` → always 0.
- New files → premake regen (`ThirdParty\premake5\premake5.exe vs2026`).
- Engine repo `main`, commit per task, conventional-commit style; NRI is
  MIT — adapting `.example/NRISamples` code is licensed and encouraged
  (keep a one-line attribution comment at the smoke's head).
- Build both configs each task; full gate `ArcaneTests.exe "~[gpu]"` FROM
  THE EXE DIR.

## File structure (locked)

```
ThirdParty/VMA/include/vk_mem_alloc.h            (Task 1)
ThirdParty/D3D12MA/{include,src}/...             (Task 1)
ThirdParty/NRI/{Include,Source,premake5.lua,README.md}  (Task 2)
ThirdParty/AgilitySDK/build/native/bin/x64/...   (Task 3)
ArcaneClient/src/Arcane/Render/Nri/
  NriCommon.hpp/.cpp     Result macro, CallbackInterface plumbing, version log   (Task 4)
  Graveyard.hpp/.cpp     fence-tagged deferred destruction (pure core + NRI face) (Task 5)
  NriDevice.hpp/.cpp     creation-half consumption + wrap + post-wrap asserts     (Task 7)
  NriSwapChain.hpp/.cpp  NRISwapChain + timeline-fence frame pacing               (Task 8)
  NriSmoke.hpp/.cpp      the --nri-smoke triangle host path                       (Task 9)
ArcaneTests/src/NriSubstrateTest.cpp             ([nri] headless suite)
```

---

### Task 1: Vendor VMA + D3D12MA

NRI's backends `#include "vk_mem_alloc.h"` / `"D3D12MemAlloc.h"`; the clone
carries neither (CMake FetchContent). Vendor them at the versions NRI pins.

- [ ] **Step 1:** Read `.example/NRI/CMakeLists.txt` (and any
  `External/CMakeLists.txt`) and record the pinned VMA and D3D12MA
  versions/URLs. Download those exact tags (zip from the official GitHub
  repos: GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator and
  /D3D12MemoryAllocator).
- [ ] **Step 2:** Vendor per our conventions (CLAUDE.md dependency table):
  VMA is header-only → `ThirdParty/VMA/include/vk_mem_alloc.h` (+ LICENSE).
  D3D12MA ships `D3D12MemAlloc.h` + `D3D12MemAlloc.cpp` →
  `ThirdParty/D3D12MA/{include,src}` (+ LICENSE). Check how NRI includes
  and instantiates each (grep `VMA_IMPLEMENTATION` and `D3D12MemAlloc.cpp`
  usage in `.example/NRI/Source`) — if NRI's own TUs provide the
  implementation, ship headers only and note it in each README; if not,
  D3D12MA's .cpp joins Task 2's premake files list.
- [ ] **Step 3:** Add `IncludeDir["VMA"]` / `IncludeDir["D3D12MA"]` to the
  workspace `premake5.lua` IncludeDir table (pattern: the existing
  `IncludeDir["nvrhi"]` line at premake5.lua:79).
- [ ] **Step 4:** Commit: `chore(vendor): VMA + D3D12MA at NRI v180's pinned versions`

### Task 2: Vendor NRI as a premake static lib

- [ ] **Step 1:** Copy from `.example/NRI` into `ThirdParty/NRI/`:
  `Include/` (all, including `Extensions/`), `Source/{Shared,D3D12,VK,NONE,Validation,Creation}`,
  `Source/NRIConfig.h`, `LICENSE`. Do NOT copy `Source/D3D11`,
  `Source/WGPU`, tests, samples, CMake files. Write `ThirdParty/NRI/README.md`:
  one paragraph — vendored from tag v180 (commit 4b48531), what was
  excluded, update procedure (re-copy from a fresh clone, never hand-edit
  vendored files).
- [ ] **Step 2:** Write `ThirdParty/NRI/premake5.lua` mirroring
  `ThirdParty/nvrhi/premake5.lua`'s shape (READ IT FIRST — static lib,
  staticruntime off, per-config defines incl. the NDEBUG note at workspace
  premake5.lua:271). Project `"NRI"`, files = the six copied dirs' cpp/h/hpp,
  includedirs = its own `Include`, `Source`, plus `%{IncludeDir.VulkanHeaders}`,
  `%{IncludeDir.DirectXHeaders}` (add these IncludeDir entries if the
  existing names differ — grep the IncludeDir table first), VMA, D3D12MA.
  Defines: enable D3D12+VK+NONE+Validation, disable D3D11/WGPU/AGS/NVTX —
  find the exact macro names in `Source/NRIConfig.h` and the CMake options
  (e.g. the pattern is `NRI_ENABLE_*`); set `NRI_ENABLE_AGILITY_SDK_SUPPORT`
  per Task 3 (start OFF here; Task 3 turns it on). NRI is `/W4 /WX`-clean
  upstream — keep warnings high; document any suppression you're forced to
  add as a comment naming the warning and why.
- [ ] **Step 3:** Workspace `premake5.lua`: `IncludeDir["NRI"] =
  "%{wks.location}/ThirdParty/NRI/Include"`, `include "ThirdParty/NRI"`
  beside the nvrhi include (premake5.lua:95), and add `"NRI"` to
  ArcaneClient's links list (premake5.lua:218) + `%{IncludeDir.NRI}` to its
  includedirs. Regen, build Debug AND Release. Expected: NRI compiles into
  the workspace; nothing consumes it yet.
- [ ] **Step 4:** Full gate `~[gpu]` — must stay green (pure addition).
- [ ] **Step 5:** Commit: `feat(vendor): NRI v180 as a premake static lib -- D3D12+VK+NONE+Validation+Creation`

### Task 3: Agility SDK

Without it the D3D12 backend is permanently degraded (contract §2.3: no
enhanced barriers, `m_Version <= 8`). Contract §6 item 3 carries the one
empirical check.

- [ ] **Step 1:** Fetch the `Microsoft.Direct3D.D3D12` NuGet package
  (redistributable; a NuGet .nupkg is a zip — download from nuget.org,
  extract `build/native/`). Vendor into `ThirdParty/AgilitySDK/` (headers
  if newer than DirectX-Headers' — compare and note; the runtime DLLs
  `D3D12Core.dll`/`d3d12SDKLayers.dll` under `bin/x64`). Pin the version in
  a README; pick the newest release the contract doesn't warn against.
- [ ] **Step 2:** Exe-side exports — Agility requires the EXE (not a DLL)
  to export the SDK version and path. Add to BOTH host mains
  (`ArcaneRuntime/src/main.cpp`, `ArcaneEditor/src/main.cpp`) and
  `ArcaneTests`' main TU:

```cpp
// Agility SDK handshake (NRI Phase 1, contract §2.3): the D3D12 loader
// reads these EXPORTED symbols from the EXE to redirect device creation
// into the vendored D3D12Core.dll under .\D3D12\. Version must match the
// vendored package; the empirical proof is NRI logging "Using
// ID3D12Device10+" (contract §6 item 3) at the desk milestone.
extern "C" __declspec(dllexport) extern const unsigned D3D12SDKVersion = /* vendored version */;
extern "C" __declspec(dllexport) extern const char*    D3D12SDKPath    = ".\\D3D12\\";
```

- [ ] **Step 3:** Post-build copy of the two DLLs into `<exedir>/D3D12/`
  for the three exes (postbuildcommands in premake, mirroring the existing
  data-copy postbuild patterns — grep `postbuildcommands` in premake5.lua).
  Flip `NRI_ENABLE_AGILITY_SDK_SUPPORT` ON in `ThirdParty/NRI/premake5.lua`
  (with whatever companion define carries the SDK version — read how
  `SharedD3D12.h:9`'s static_assert consumes it, per contract §2.3).
- [ ] **Step 4:** Regen, build both configs, full gate green (D3D12 device
  creation via NVRHI must be unaffected — the export changes the loader's
  D3D12Core source, which is exactly the point; if anything regresses the
  desk milestone's golden check catches it, and this is the task to revert).
- [ ] **Step 5:** Commit: `feat(render): Agility SDK vendored + exe exports -- enhanced-barriers floor for NRI D3D12`

### Task 4: NriCommon — Result discipline + callbacks + version log

**Interfaces (consumed by every later task):**

```cpp
// ArcaneClient/src/Arcane/Render/Nri/NriCommon.hpp
#include <NRI.h>            // vendored NRI include root
namespace Arcane
{
    // Every nri call that returns nri::Result goes through this. Logs the
    // failing expression + result name at ERROR and bumps RenderErrorCount
    // (the 0/0 gate latch), returns false on failure. Never throws.
    bool NriCheckImpl(nri::Result result, const char* expr, const char* file, int line);
    #define ARC_NRI_CHECK(expr) ::Arcane::NriCheckImpl((expr), #expr, __FILE__, __LINE__)

    // Install into DeviceCreationDesc/wrapper descs: routes NRI's
    // MessageCallback into ARC_INFO/WARN/ERROR by severity, ERRORs bump
    // RenderErrorCount. Mirrors the NVRHI message-callback latch
    // (DeviceD3D12.cpp's NvrhiMessageCallback) -- same gate, new producer.
    nri::CallbackInterface MakeNriCallbacks() noexcept;

    // One INFO line: nriVersion + which backend + validation on/off.
    void LogNriIdentity(nri::Device& device);
}
```

- [ ] **Step 1:** Write the failing `[nri]` tests in a NEW
  `ArcaneTests/src/NriSubstrateTest.cpp`: (a) `ARC_NRI_CHECK(nri::Result::SUCCESS)`
  returns true and does not bump `RenderErrorCount`; (b) a failure result
  returns false and bumps it by exactly 1 (read how existing tests
  reset/read `RenderErrorCount` — grep it in ArcaneTests first, follow that
  idiom); (c) NONE-backend device lifecycle: `nriCreateDevice`-family is
  forbidden for REAL backends, but the NONE device is created through the
  standard creation path — read `.example/NRI` for how a NONE device is
  created (graphicsAPI NONE in DeviceCreationDesc via `nriCreateDevice` is
  ACCEPTABLE HERE and only here: NONE has no native device to wrap; add
  the comment saying exactly that) — create it with `MakeNriCallbacks()`,
  `LogNriIdentity`, destroy it, assert no RenderErrorCount growth.
- [ ] **Step 2:** Regen (new files), build, tests fail to compile. Implement
  NriCommon.{hpp,cpp}. Run `[nri]` then the full gate.
- [ ] **Step 3:** Commit: `feat(nri): NriCommon -- result discipline, callback latch, identity log`

### Task 5: The graveyard

Contract precondition: NRI has no refcounts/deferred destroy; destroying a
resource the GPU still reads is a device fault. Pure core, NRI-facing edge.

**Interfaces:**

```cpp
// ArcaneClient/src/Arcane/Render/Nri/Graveyard.hpp
namespace Arcane
{
    // Fence-tagged deferred destruction. Pure core: Bury records a destroy
    // thunk against the fence value that must COMPLETE before it may run;
    // Reap(completed) runs every thunk with fenceValue <= completed, in
    // burial order; Drain() runs everything (teardown/device-loss path --
    // caller has already made the GPU idle). Not thread-safe by design:
    // one graveyard per queue timeline, owned by the recording thread.
    class ARCANE_API Graveyard
    {
    public:
        using Destroyer = std::function<void()>;
        void Bury(std::uint64_t fenceValue, Destroyer destroy);
        void Reap(std::uint64_t completedValue);
        void Drain();
        [[nodiscard]] std::size_t Pending() const noexcept;
        ~Graveyard();   // asserts empty in debug; Drains (with a WARN) in release
    };
}
```

- [ ] **Step 1:** Failing `[nri]` tests (pure, no device): burial order
  preserved within a fence value; `Reap(5)` runs \<=5 and not 6;
  interleaved Bury/Reap; `Drain` runs all; `Pending` counts; destructor
  path (release-mode Drain behavior can be exercised by calling Drain
  explicitly; the debug assert is documented, not tested).
- [ ] **Step 2:** Implement (vector of {fence,thunk}; Reap does a stable
  partition from the front since burials arrive in nondecreasing fence
  order — assert that invariant in debug and document it). Plus ONE
  NONE-device integration case: create a buffer via CoreInterface, Bury its
  destroy (CoreInterface function-table call), Reap at a value that
  releases it, destroy device — no leak report from NRI's leak checking if
  the NONE backend has any, no RenderErrorCount growth. (GetFenceValue is
  0 on NONE — feed Reap values manually.)
- [ ] **Step 3:** Full gate; commit:
  `feat(nri): fence-tagged graveyard -- the deferred-destruction precondition`

### Task 6: Device-creation contract edits (both backends, NVRHI still consuming)

Apply the capability contract's §5 edits to OUR creation halves in
`DeviceVulkan.cpp` / `DeviceD3D12.cpp`. **The contract rows are the spec —
implement each cited item exactly; do not re-derive.** Highlights the
implementer must not miss: the four hard VK items
(`VK_KHR_get_surface_capabilities2`, `VK_KHR_push_descriptor`,
`bufferDeviceAddress`, `hostQueryReset`), `timelineSemaphore`, the core-1.0
`VkPhysicalDeviceFeatures2` chain (we currently enable NONE —
`DeviceVulkan.cpp:794-799`), sync2/dynamic-rendering confirmations, and the
preserve-actions (crash-diagnostics sweep untouched — contract §4 says
zero conflicts, keep it that way).

- [ ] **Step 1:** VK edits per §5 items 1–9, each with a one-line comment
  citing its contract item number. Feature enablement follows the NVIDIA
  sample's reference behavior quoted in the contract (query
  `vkGetPhysicalDeviceFeatures2`, enable what's supported, assert the
  contract's hard-required bits are present — a missing hard bit is a
  loud create-time error naming the feature).
- [ ] **Step 2:** D3D12 edits per §5 items 10–13 (+ the InfoQueue1 callback
  from item 14's asserts family if the contract places it here — follow
  the contract's wording; the debug-layer message callback must reach our
  log + RenderErrorCount because NRI won't carry it in wrapper mode).
- [ ] **Step 3:** Build both configs; full gate. The NVRHI path must be
  pixel-identical — that proof is the milestone's golden re-run (desk);
  note it in the report.
- [ ] **Step 4:** Commit: `feat(render): device creation meets the NRI wrapper capability contract (SS5)`

### Task 7: Creation-half refactor + the wrap

- [ ] **Step 1:** Factor each backend's native-device creation into a
  reusable piece (a struct of the created handles + the chosen
  queues/families + enabled-feature record) consumed by (a) the existing
  NVRHI device path — byte-identical behavior — and (b) the new
  `NriDevice`. Keep the factoring minimal: extract, don't redesign.
- [ ] **Step 2:** `NriDevice.{hpp,cpp}`: given the creation piece, fill
  `nri::DeviceCreationVKDesc` / `nri::DeviceCreationD3D12Desc` per contract
  §1.1/§2.1 (enabled-extension name lists — NRI trusts the desc verbatim in
  wrapper mode; queue declaration; `minorVersion` from the actual created
  API version, NOT a constant — contract concern 3; `vkBindingOffsets`
  matching our `VulkanBindingOffsets`), install `MakeNriCallbacks()`, wrap
  via `nriCreateDeviceFromVKDevice`/`...D3D12Device`, then the post-wrap
  asserts (contract item 14): graphics queue reachable via `GetQueue`,
  `LogNriIdentity`, hard-fail loudly on any miss.
- [ ] **Step 3:** Headless coverage is limited (wrapping needs a real
  device): add the `[gpu]`-tagged test `nri wrap smoke` that creates the
  native device + wrap + destroys, both backends — runs at the desk, not
  in the dev gate. Build both configs; full `~[gpu]` gate.
- [ ] **Step 4:** Commit: `feat(nri): NriDevice -- creation halves reused, wrapper path, post-wrap asserts`

### Task 8: NriSwapChain + timeline-fence pacing

- [ ] **Step 1:** `NriSwapChain.{hpp,cpp}`: NRISwapChain over our SDL
  window (read `.example/NRISamples`' swapchain setup for the desc shape),
  BGRA8/sRGB-correct format chosen to match the NVRHI swapchain's, resize =
  destroy+recreate (spec), `VK_SUBOPTIMAL` handled ours (survey: NRI
  swallows it — check what acquire/present actually return, and recreate
  on the window-resize event instead). Frame pacing: `kSwapchainFramesInFlight`
  slots on a timeline `nri::Fence` — signal at submit, `Wait(value)` with
  the SAME heartbeat-publishing poll semantics as `GpuFrameSlot`
  (`Diagnostics::GpuHeartbeat` keeps beating while waiting; reuse the
  SDL_DelayNS poll pattern — read GpuInstrumentation.cpp's pacing comment
  block first).
- [ ] **Step 2:** Desk-only proof (Task 9's smoke drives it). Build + gate.
- [ ] **Step 3:** Commit: `feat(nri): NriSwapChain + timeline-fence frame pacing`

### Task 9: The triangle smoke

- [ ] **Step 1:** `--nri-smoke` flag (non-Dist, beside `--crash-gpu` in
  HostConfig.cpp, same `#if !defined(ARCANE_DIST)` guard; HostConfigTest
  case for the flag round-trip). In RuntimeApp::Run, BEFORE normal boot:
  if set, run `NriSmoke::Run(config)` and return its exit code — the
  normal NVRHI boot never starts.
- [ ] **Step 2:** `NriSmoke.{hpp,cpp}`: window → creation half → wrap →
  swapchain → graveyard → per-frame: acquire, TEMPORARY hand barriers
  (comment block: "Phase 2's frame graph replaces every barrier in this
  file; hand-written CmdBarrier outside the graph becomes a review defect
  at the port's merge — spec ratification 1"), clear, draw one triangle
  (vertex colors, no textures), present; honors `--frames N` and
  `--screenshot <png>` (readback via NRI staging→`WritePngRgba` — adapt
  `.example/NRISamples`' Triangle + Readback samples, MIT, attribute).
  Shaders: minimal VS/PS compiled through our ShaderCompiler dual-target
  (DXIL for D3D12, SPIR-V for VK) at smoke startup — no new shader
  infrastructure; if the ShaderCompiler seam resists a one-off compile,
  fall back to prebaked blob headers generated once offline and say so in
  the report.
- [ ] **Step 3:** Validation layers: `--nri-smoke` implies D3D12 debug
  layer / VK validation + sync validation ON in Debug config (wired so
  every validation error lands in RenderErrorCount via the InfoQueue1
  callback and our VK debug messenger — both already exist after Task 6);
  the smoke's exit code is nonzero if RenderErrorCount grew. Document the
  two desk commands in the file header.
- [ ] **Step 4:** Build both configs; full gate (headless untouched).
  Commit: `feat(nri): --nri-smoke -- clear+triangle on the wrapped device, validation-gated`

### Task 10: Desk milestone (USER)

From the Release AND Debug exe dirs (Debug = validation layers):

- [ ] 1. `ArcaneRuntime --nri-smoke --frames 120 --screenshot nri-tri-dx12.png`
  (Debug build, both `--backend dx12` and `--backend vulkan`): triangle
  renders, exit 0, no validation errors, and the Agility proof line from
  contract §6 item 3 appears for dx12.
- [ ] 2. Same on Release for pacing sanity (no validation layers).
- [ ] 3. Eyeball both screenshots; they should differ only trivially
  across backends (report numbers by comparing with the Phase 0 comparator
  if desired — informational).
- [ ] 4. **Golden regression floor:** run the Phase 0 golden self-compare
  (both backends, the exact Task 6 commands from the Phase 0 plan's
  calibration section) — MUST still be `golden PASS ... maxDelta 0`.
- [ ] 5. Resize the smoke window on vulkan (drag-storm style) — no crash,
  recreate works.
- [ ] 6. Report results; the controller appends a milestone record to this
  plan and commits screenshots ONLY if useful (not goldens — the smoke is
  scaffolding, deleted in Phase 2 when the graph renders real content).

## Verification ladder (Phase 1 exit criteria)

1. `~[gpu]` gate green with the `[nri]` suite in it (graveyard pure tests,
   NriCommon tests, NONE-device lifecycle).
2. Desk: triangle on both backends, validation-clean in Debug, Agility
   line proven, resize survives.
3. **Phase 0 goldens still bit-green on the NVRHI path.**
4. THEN the Phase 2 (frame graph + 2D cutover) plan gets written.

## Plan inputs resolved here / deferred

- Graveyard placement (spec plan-input): per-device object owned by
  `NriDevice`, one per queue timeline — locked by Task 5/7's interfaces.
- Streamer: still excluded (spec non-goal; ImGui brief reinforces).
- The `[gpu] nri wrap smoke` test + smoke path are scaffolding with a
  planned deletion point (Phase 2 end / Phase 5) — each carries a comment
  saying so.
