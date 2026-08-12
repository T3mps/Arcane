# NRI (v180) Survey — adoption evidence, source scan, migration mapping

Research input for a prospective NVRHI -> NRI migration decision. NOT a spec;
no decision is ratified by this document.

**Method.** (1) Web sweep for production adopters and migration literature.
(2) Reference clone of NRI pinned at tag `v180` (`NRI_VERSION 180`, dated
"23 June 2026", commit `4b48531`) plus NRISamples (embedding NRIFramework and
the same NRI commit) into `.example/NRI` / `.example/NRISamples` (gitignored;
re-create with `git clone --depth 1 --branch v180 --recurse-submodules
https://github.com/NVIDIA-RTX/NRI`). (3) Three parallel source-scan passes:
core API/execution, resources/descriptors/upload, ecosystem/backends/build.
Raw citation-dense scans are committed beside this file as
`nri-v180-scan-{core,resources,ecosystem}.md`; every `file:line` citation in
them resolves against the pinned clone.

---

## 1. Adoption evidence: a null result

- **No publicly documented game engine — AAA or otherwise — uses NRI as its
  RHI.** NRI's visible production surface is NVIDIA's own tooling: the NRD
  sample/integration layer, NRISamples, and the NRIFramework sandbox.
- NRD itself has shipped in AAA titles (Watch Dogs: Legion, Cyberpunk 2077),
  but NRD is API-agnostic and studios integrate it natively; the NRI-based
  `NRDIntegration` convenience layer is the samples path. **No NRI code is
  confirmed inside any shipped AAA title.**
- NVRHI's production surface is larger by comparison: RTX Remix, Donut, and
  the RTX SDK family (RTXDI, Path Tracing SDK, OMM) are NVRHI-based.
- **Migration literature: none.** One hobbyist blog post ("Considering
  Transferring to NVIDIA Render Interface", motivated by OpenXR/multiview)
  existed and has been taken down. Nobody has published an NVRHI->NRI
  migration report. We would be first on the record.
- NVIDIA's own positioning (NRI README): NRI is the **lower-level**
  alternative — explicit non-goals are "high-level (D3D11-like) abstraction"
  and "automatic barriers", i.e. precisely the two things NVRHI provides.
  A migration is a level change, not a swap.

## 2. Compatibility with the GPU crash diagnostics arc (2026-08-11/12)

The best news in the survey. Verified against source:

- **NRI contains no DRED, device-fault, breadcrumb, or buffer-marker
  facility anywhere** (exhaustive grep; the only related code is 4 bare
  `GetDeviceRemovedReason()` polls). Our first-party crash stack has nothing
  to reconcile with or route around inside NRI.
- **The wrapper creation paths preserve our stack completely.**
  `nriCreateDeviceFromD3D12Device` never calls `D3D12CreateDevice` — we keep
  our DRED-before-create configuration (F-2) verbatim and hand NRI the
  finished device. `nriCreateDeviceFromVKDevice` **never calls
  `vkCreateDevice`** and enables zero features/extensions itself — our
  `VK_EXT_device_fault` sweep (F-5) survives untouched.
- **Corollary: the native `nriCreateDevice` path is forbidden for us** — on
  D3D12 it calls `D3D12CreateDevice` with zero DRED config (silently forfeits
  breadcrumbs/page-fault data), and on VK it owns the feature chain. Any
  adoption spec must mandate the wrapper path.
- Two diagnostics *improvements* under NRI: `Result::DEVICE_LOST` is returned
  directly from `QueueSubmit`/`AcquireNextTexture`/`QueuePresent`/
  `WaitForPresent` (typed enum, negative codes never trigger AbortExecution)
  — replacing our substring-match on NVRHI's "Device Removed!" log text
  (F-3 rewrite, more robust); and `nri::Fence` is a true timeline fence
  (`ID3D12Fence` / VK timeline semaphore), onto which `GpuFrameSlot`'s
  event-query pacing maps 1:1 (the exact pattern is documented as canonical
  usage in `NRISwapChain.h:139-207`).
- Caveats: wrapped VK devices get **no capability verification** — a missing
  feature surfaces as scattered `UNSUPPORTED` results at call sites, not at
  creation. `VK_SUBOPTIMAL_KHR` is swallowed as `SUCCESS` (no proactive
  recreate signal). Hard-coded CPU wait timeouts: fence 5s, present 1s
  (`NRI_TIMEOUT_*`, overridable only at NRI build time).

## 3. The four structural costs, confirmed from source

1. **No refcounting, no deferred destruction — anywhere.** `Destroy*` /
   `FreeMemory` are immediate pass-throughs even under the validation layer
   (host-side "still bound" checks only; zero GPU-in-flight awareness —
   grep for fence-aware destroy checks returned nothing). The one deferred
   pattern in the whole library is private to the Streamer's ring regrowth.
   **An engine-side, fence-tagged resource graveyard is a precondition** for
   porting anything that leans on NVRHI's refcounted teardown. Failure mode
   changes from leak to GPU fault.
2. **Barriers are fully manual** — `CmdBarrier(BarrierDesc{globals, buffers,
   textures})`, explicit before/after `{AccessBits, Layout, StageBits}` per
   transition (~4-6 lines each once the structs are set up; one call batches
   any number). Cross-queue transfer exists (`srcQueue`/`dstQueue`) but has
   **zero worked examples in NRISamples** — the samples rely on VK
   CONCURRENT sharing mode instead. NRI mostly sidesteps queue-ownership
   barriers by defaulting to concurrent sharing when async queues are
   acquired before resource creation.
3. **Volatile CBs are replaced by Streamer + root-descriptor dynamic
   offsets**: one static CB view over the Streamer's constant ring +
   `CmdSetRootDescriptor{index, descriptor, byteOffset}` per draw. This is
   the structural redesign of both cached-binding-set sites (split static
   texture/sampler sets from the per-draw offset). Note the Streamer is
   optional — most samples hand-roll per-queued-frame CBs with `Map`/`Unmap`
   (free on D3D12/VK: persistent mapping, Map/Unmap are no-ops).
4. **No framebuffer/render-pass objects** — dynamic rendering only;
   attachment formats bake into pipelines; viewport/scissor always dynamic.
   Every `IFramebuffer` in an ARCANE_API signature is a redesign site.

## 4. What maps cleanly / what we gain

- **Transient aliasing is a first-class primitive**: VMA (VK) / D3D12MA
  (D3D12) are baked into the backends; `vma.enable` always sets
  `CAN_ALIAS`; `Bind*Memory` explicitly permits overlapped/aliased bindings.
  This is the frame-graph transient allocator's foundation, present today.
- **Bindless, two tiers**: `tiers.bindless=1` variable-sized/partially-bound
  arrays; `=2` SM6.6 directly-indexed heaps (`ResourceDescriptorHeap[]`,
  `MUTABLE` descriptor type, `RESOURCE_HEAP_DIRECTLY_INDEXED`). Canonical
  sample exists (`DescriptorHeapIndexing`).
- **VK binding offsets**: `DeviceCreationDesc::vkBindingOffsets` is a direct
  analog of NVRHI's `VulkanBindingOffsets{t,s,b,u}`, with a per-layout
  opt-out NVRHI lacks (`IGNORE_GLOBAL_SPIRV_OFFSETS`). Carve-out: the shift
  is NOT applied to `MUTABLE`/`INPUT_ATTACHMENT` ranges — two numbering
  models when mixing bindless heaps with typed ranges. `NRI.hlsl` provides
  the cross-backend `NRI_RESOURCE`/`NRI_ROOT_CONSTANTS` declaration macros.
- **Readback**: the `Readback.cpp` sample is our `PickBuffer` almost
  verbatim (row-aligned HOST_READBACK buffer, `CmdReadbackTextureToBuffer`,
  map a queued-frame later off the frame fence).
- **Push constants** -> `CmdSetRootConstants` near-1:1; root/static samplers
  exist; `FitPipelineLayoutSettingsIntoDeviceLimits` solves the D3D12
  256-byte root-signature budget mechanically.
- **NONE backend**: real dummy backend usable for headless CI of engine-side
  logic (max-capability DeviceDesc, no-op submits). Footguns: `MapBuffer`
  returns null; `GetFenceValue` always 0.
- **C ABI**: genuine and well-defended — single-source C/C++ headers,
  function-table `nriGetInterface` with exact sizeof + full null-slot
  checks; pure-C samples prove it. (Future Rust/tooling bindings.)
- **Extensions**: Reflex (`NRILowLatency`), upscalers (NIS zero-footprint;
  FSR runtime DLLs; XeSS/DLSS static import libs + shipped DLLs + vendor SDK
  fetch), ImGui renderer (consumer still brings real Dear ImGui 1.92+),
  feature-complete ray tracing (incl. OMM, dual VK/D3D12-style AS memory
  APIs), mesh shaders, pipeline caches with `FAIL_ON_CACHE_MISS` (Xbox GDK
  named in the comment — API is console-aware; no console backend exists).
- **HDR-capable swapchain formats** (BT.709 G10/G22, BT2020 PQ) +
  `GetDisplayDesc` luminance/chromaticity — our tonemap direction benefits.

## 5. Risk register (honesty findings)

- **NRI's validation layer is an API-contract checker, NOT a barrier/hazard
  tracker** — 539 call-time checks (nulls, ranges, feature gates,
  command-buffer state machine, barrier *argument* plausibility); zero
  GPU-timeline state tracking, zero thread-safety instrumentation, zero
  leak detection. **Correction to prior discussion:** "NRI validation will
  catch missing barriers" is wrong. Barrier correctness rides on the
  platform layers (D3D12 debug layer / VK validation incl. sync validation)
  + our own gates (golden images, RenderErrorCount).
- Hidden management exceptions to the README's claim: VK-only spinlock on
  `QueueSubmit`/`QueueWaitIdle`/`AcquireNextTexture`; internal locks on
  descriptor-pool/command-allocator/PSO-cache paths. `Cmd*` recording
  verified genuinely lock-free on both backends.
- **Streamer constant ring has no overflow protection** — undersizing
  `constantBufferSize` silently corrupts in-flight frames (wraps on size
  only, no fence check). A live NVIDIA `// TODO ... right? :)` questions
  whether `CmdCopyStreamedData` needs a barrier. Both need empirical
  validation before a frame graph leans on the Streamer.
- No swapchain resize API (destroy+recreate only); `VK_SUBOPTIMAL_KHR`
  swallowed; no secondary command buffers/bundles anywhere.
- D3D12 draw-parameter emulation (below SM6.8) silently reserves a root
  constant at space999 and changes indirect-draw layouts when enabled.
- Known workaround in-tree for an NVIDIA-driver crash under Vulkan
  Validation Layers (`CommandBufferVK.hpp:927`, path disabled via `#if 0`).
- DLSS/XeSS backends are global-locked, documented not-thread-safe.
- WGPU is a real 5th backend and clearly the least mature (TODO density
  ~1/170 LOC vs D3D12's ~1/510) — excluded from any plan; no Metal backend
  (macOS = VK over MoltenVK; one known MoltenVK gap: depth_bias_control).
- Trunk-based dev, flat integer versioning (v180 = a dated build counter,
  not semver); `nriVersion` queryable at runtime; interface-size checks are
  the ABI shim. Thin bus factor unchanged from prior assessment.

## 6. Vendoring feasibility (our conventions: premake, ThirdParty/, no CMake)

**Verdict: feasible without CMake.** CMake orchestrates *dependency
fetching* (all URL-zip FetchContent — no submodules, no packman), not
anything premake can't express. Realistic Windows slice (Shared + D3D12 +
VK + Validation + NONE + Creation) is ~140 files; D3D12-only slice ~96
files/24k LOC. Warnings-as-errors clean (`/W4 /WX`) — a good sign for our
0/0 gate. One-time manual vendoring items:

- **Deps to vendor**: Vulkan-Headers (Apache/MIT), VMA + D3D12MA (MIT),
  AGS (MIT), NVTX (Apache, optional), Agility SDK (NuGet redistributable),
  and — only if the Upscaler extension is wanted — the proprietary vendor
  SDKs (NGX/DLSS, XeSS; FSR SDK is MIT with prebuilt signed DLLs).
- **License friction is exactly two items**: NVAPI and NGX/DLSS carry
  NVIDIA proprietary SDK terms (NVAPI source is not freely redistributable
  — matters if the engine repo is public). Everything else MIT/Apache.
- NIS is the free-lunch upscaler (fully self-contained, zero DLLs).
- ShaderMake (NIS/ImGui shader precompile) can be bypassed by pre-baking
  the generated blob headers once, offline.
- WGPU backend excluded (prebuilt-binary dependency, no need).

## 7. NVRHI -> NRI mapping table (for the eventual spec)

| NVRHI (today) | NRI (v180) |
|---|---|
| `IFramebuffer` in every pass signature | none — `CmdBeginRendering(AttachmentDesc[]{view,loadOp,storeOp,...})`; formats baked in pipeline `OutputMergerDesc` |
| automatic state tracking / barriers | `CmdBarrier(BarrierDesc)` per transition, engine/frame-graph owned |
| refcounted handles + `runGarbageCollection()` | none — engine-side fence-tagged graveyard (precondition) |
| volatile CBs (`setIsVolatile`, `maxVersions`) | Streamer constant ring + `CmdSetRootDescriptor` byte offset (or hand-rolled per-queued-frame CB + persistent Map) |
| `writeBuffer` inside open list | Streamer `StreamBufferData` + `CmdCopyStreamedData`, or Map/memcpy with N-buffering (engine's burden) |
| event query frame pacing (`GpuFrameSlot`) | timeline `nri::Fence` signal-at-submit + `Wait(value)` — 1:1 |
| "Device Removed!" message-callback substring (F-3) | typed `Result::DEVICE_LOST` from submit/acquire/present |
| `VulkanBindingOffsets` global | `vkBindingOffsets` + per-layout `IGNORE_GLOBAL_SPIRV_OFFSETS` (MUTABLE/INPUT_ATTACHMENT exempt) |
| push constants | `CmdSetRootConstants` (near 1:1) |
| binding sets cached on stable handles | `DescriptorSet` stable across frames (pool-scoped); D3D12 side is NRI-synthesized; split static sets from per-draw offsets |
| ImGui-NVRHI backend | `NRIImgui` (bring Dear ImGui >= 1.92) |
| swapchain impls per backend + native-handle dance | `NRISwapChain` (destroy+recreate on resize) |
| device creation (ours, incl. DRED + VK fault sweep) | **kept** — wrapper path only (`CreateDeviceFrom*Device`) |
| `getNativeObject` at crash-stack seams | same model (`GetXxxNativeObject`), crash stack stays native |

## 8. Open questions for the adoption spec (if ratified)

1. Graveyard design: fence-value-tagged deferred destruction — engine-owned;
   where it lives relative to the frame graph.
2. Validation-gate strategy given §5: platform layers in the dev loop (incl.
   VK sync validation), golden-image harness as Task 1, RenderErrorCount
   equivalent over `CallbackInterface` + `Result` checking discipline.
3. Streamer adoption vs hand-rolled per-frame rings (given the overflow
   footgun and the barrier TODO) — measure, then decide.
4. Wrapper-path capability contract: our device creation must enable
   everything NRI expects (sync2, dynamic rendering, etc. on VK) — enumerate
   the required feature set explicitly (NRI won't tell us at create time).
5. Upscaler extension: in or out of the initial vendoring (licensing +
   shipping DLLs); NIS-only as a start?
6. Queue strategy: concurrent sharing (NRI default) vs `queueExclusive` +
   explicit transfers (no sample exists; we'd be first).
7. Boundary-rule reversal ratification (never-wrap -> engine-owned seam) and
   the per-game render path split (2D stack stays NVRHI vs full cutover).

## 9. Bottom line

The source is better than its adoption story: small (~42k LOC for the four
real backends + shared), warnings-clean, honestly commented, well-defended
C ABI, and it hands us the two primitives the space-game renderer actually
needs (aliasing for transients, tier-2 bindless) while staying completely
out of our crash-diagnostics stack's way — provided we mandate the wrapper
creation path. The costs are exactly the four structural ones predicted,
now confirmed and quantified, plus one correction: NRI's validation layer
will not catch barrier bugs — that burden lands on platform validation
layers and our own golden-image/soak discipline, which reinforces the
"verification instrument first, port second" sequencing. Nothing found
changes the standing recommendation (desk battery -> push -> spec arc ->
vertical slice); several things found would have burned us mid-port had we
started blind — which is what this survey was for.
