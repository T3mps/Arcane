# NRI Core API & Execution Model Survey (pinned v180)

Repo: `D:\dev\starworks\Arcane\.example\NRI` (`NRI_VERSION 180`, `NRI_VERSION_DATE "23 June 2026"`, `Include/NRI.h:41-42`)
Samples: `D:\dev\starworks\Arcane\.example\NRISamples`

Audience: engine team evaluating migration FROM NVRHI (D3D11-style, auto-barriers, refcounted handles).
All citations are `file:line` unless a symbol name is a better anchor.

---

## 1. Device creation

### `DeviceCreationDesc` (native path) — `Include/Extensions/NRIDeviceCreation.h:54-83`

```c
NriStruct(DeviceCreationDesc) {
    Nri(GraphicsAPI) graphicsAPI;
    NriOptional Nri(Robustness) robustness;
    NriOptional const NriPtr(AdapterDesc) adapterDesc;
    NriOptional Nri(CallbackInterface) callbackInterface;
    NriOptional Nri(AllocationCallbacks) allocationCallbacks;

    // One "GRAPHICS" queue is created by default
    NriOptional const NriPtr(QueueFamilyDesc) queueFamilies;
    NriOptional uint32_t queueFamilyNum;        // put "GRAPHICS" queue at the beginning of the list

    // D3D specific
    NriOptional uint32_t d3dShaderExtRegister;
    NriOptional uint32_t d3dZeroBufferSize;

    // Vulkan specific
    Nri(VKBindingOffsets) vkBindingOffsets;
    NriOptional Nri(VKExtensions) vkExtensions; // to enable

    // Switches (disabled by default)
    bool enableNRIValidation;
    bool enableGraphicsAPIValidation;
    bool enableD3D11CommandBufferEmulation;
    bool enableD3D12RayTracingValidation;       // needs envvar NV_ALLOW_RAYTRACING_VALIDATION=1
    bool enableMemoryZeroInitialization;

    // Switches (enabled by default)
    bool disableVKRayTracing;
    bool disableD3D12EnhancedBarriers;
};
```

- `QueueFamilyDesc` (`NRIDeviceCreation.h:48-52`) is a collection-of-queues-of-one-type request: `queuePriorities[-1;1]`, `queueNum`, `QueueType`. There is no per-queue allocator — allocation is device-wide only (`AllocationCallbacks`).
- No AGS/NVAPI context field exists on the generic `DeviceCreationDesc` — vendor-lib init is entirely internal to NRI on the native path (see §"Vendor libs" below).
- `nriEnumerateAdapters(adapterDescs, adapterDescNum)` — two-call pattern, `Include/Extensions/NRIDeviceCreation.h:87`.
- `nriCreateDevice` orchestration lives in `Source/Creation/Creation.cpp:805-881`: if no `adapterDesc` given it enumerates and picks the first adapter whose `supportedGraphicsAPIs` matches; if no queue families given it defaults to a single `GRAPHICS` queue; then dispatches to `CreateDeviceD3D11/D3D12/VK/WGPU/NONE` based on `#if NRI_ENABLE_*_SUPPORT` compile switches, then `FinalizeDeviceCreation`.
- `nriReportLiveObjects()` (`NRIDeviceCreation.h:93`) — D3D-only global leak report (comment: "not needed for VK because validation is tied to the logical device").

### D3D12 native creation — `Source/D3D12/DeviceD3D12.hpp:178-260`

- **DRED is never configured anywhere in NRI** (see §7 grep sweep). The debug-layer enable (`D3D12GetDebugInterface`→`EnableDebugLayer()`) happens at `DeviceD3D12.hpp:185-189`, explicitly commented `// IMPORTANT: Must be called before the D3D12 device is created, or the D3D12 runtime removes the device` — but there is **no** `ID3D12DeviceRemovedExtendedDataSettings::SetAutoBreadcrumbsEnablement`/`SetPageFaultEnablement` call before `D3D12CreateDevice` at `DeviceD3D12.hpp:236`. If the engine calls `nriCreateDevice` with `GraphicsAPI::D3D12`, it gets NRI's own `D3D12CreateDevice` call with **zero DRED setup** — the team's DRED config point is skipped entirely on the native path.
- Debug-layer / `ID3D12InfoQueue::SetBreakOnSeverity`/`RegisterMessageCallback` setup (`DeviceD3D12.hpp:257-290`) is gated only by `desc.enableGraphicsAPIValidation`.
- Vendor extensions: `InitializeNvExt`/`InitializeAmdExt` (`DeviceD3D12.hpp:203-206`) are called automatically based on `adapterDesc.vendor`, before device creation, using AGS to *create* the device itself when AMD (`AGSDX12DeviceCreationParams` at `DeviceD3D12.hpp:219-233`) instead of calling `D3D12CreateDevice` directly.

### D3D12 wrapper — `nriCreateDeviceFromD3D12Device` / `DeviceCreationD3D12Desc`

`Include/Extensions/NRIWrapperD3D12.h:27-50`:
```c
NriStruct(QueueFamilyD3D12Desc) {
    NriOptional ID3D12CommandQueue* const* d3d12Queues; // if not provided, will be created
    uint32_t queueNum;
    Nri(QueueType) queueType;
};

NriStruct(DeviceCreationD3D12Desc) {
    ID3D12Device* d3d12Device;
    const NriPtr(QueueFamilyD3D12Desc) queueFamilies;
    uint32_t queueFamilyNum;
    NriOptional AGSContext* agsContext;
    NriOptional Nri(CallbackInterface) callbackInterface;
    NriOptional Nri(AllocationCallbacks) allocationCallbacks;
    NriOptional uint32_t d3dShaderExtRegister;
    NriOptional uint32_t d3dZeroBufferSize;
    bool enableNRIValidation;
    bool enableMemoryZeroInitialization;
    bool disableD3D12EnhancedBarriers;
    bool disableNVAPIInitialization;
};
```
- **No `enableGraphicsAPIValidation`, no `robustness` field.** `nriCreateDeviceFromD3D12Device` (`Source/Creation/Creation.cpp:943-993`) copies only `callbackInterface`, `allocationCallbacks`, `d3dShaderExtRegister`, `d3dZeroBufferSize`, `enableNRIValidation`, `enableMemoryZeroInitialization`, `disableD3D12EnhancedBarriers` into the internal `DeviceCreationDesc` — `enableGraphicsAPIValidation` is left at its default `false`, so the D3D12 debug-layer/InfoQueue block at `DeviceD3D12.hpp:185-189,257-290` is **not exercised at all** on the wrapper path. This is correct/expected: the caller already called `D3D12GetDebugInterface`/`EnableDebugLayer` (and DRED config) themselves before their own `D3D12CreateDevice`.
- `DeviceD3D12::Create` (`DeviceD3D12.hpp:178-251`): when `m_IsWrapped` (i.e. `descD3D12.d3d12Device != nullptr`), the entire "Device" block (`D3D12CreateDevice`/AGS device-creation call, lines 209-251) is skipped — NRI never touches the caller's device creation. It still does adapter lookup via `EnumAdapterByLuid` (line 197) for capability queries, and still calls `InitializeNvExt(disableNVAPIInitialization, isWrapped=true)` / `InitializeAmdExt(agsContext, isWrapped=true)` — so **NVAPI init can still happen implicitly** unless `disableNVAPIInitialization=true` is set on the wrapper desc.
- **Queues**: `QueueFamilyD3D12Desc::d3d12Queues` is `NriOptional` — if the caller passes `nullptr` for a family, NRI creates the `ID3D12CommandQueue`s itself (`QueueD3D12::Create(QueueType, priority)`, `Source/D3D12/QueueD3D12.hpp:3-16`); if provided, NRI wraps the pre-existing queue via `QueueD3D12::Create(ID3D12CommandQueue*)` (`QueueD3D12.hpp:18-28`) with only a null-check, no validation against `queueType`.
- **Verdict**: the D3D12 wrapper path is functionally complete for keeping native device creation (DRED config, custom `D3D12CreateDevice` flags/adapter selection) — NRI does not fight it. What's lost by *not* using `nriCreateDevice`: NRI's own debug-layer wiring (must be replicated manually, which the team already plans to do), and NRI's automatic AGS/NVAPI vendor-lib bootstrap (opt out via `disableNVAPIInitialization`; AGS needs a pre-existing `agsContext` passed in or it's simply not initialized).

### VK wrapper — `nriCreateDeviceFromVKDevice` / `DeviceCreationVKDesc`

`Include/Extensions/NRIWrapperVK.h:27-43` (full struct already quoted in transcript; key fields: `vkInstance`, `vkDevice`, `vkPhysicalDevice`, `QueueFamilyVKDesc* queueFamilies` (`familyIndex` + `queueNum` + `QueueType`, **no pre-created `VkQueue` handles accepted** — see below), `minorVersion`).
- `DeviceVK::Create` (`Source/VK/DeviceVK.hpp:432-...`): when `isWrapper = descVK.vkDevice != nullptr`:
  - Instance creation (`CreateInstance`, `ProcessInstanceExtensions`) is skipped (`DeviceVK.hpp:463-469`) — the caller's `VkInstance` is used as-is, its extensions are whatever the caller enabled.
  - Physical-device/queue-family enumeration is skipped (`DeviceVK.hpp:480-524`) — caller-supplied `vkPhysicalDevice` and `QueueFamilyVKDesc.familyIndex` are trusted directly (`DeviceVK.hpp:529-533`).
  - `ProcessDeviceExtensions` (which computes NRI's desired extension list, including ray tracing, `VK_KHR_synchronization2`, etc.) is **skipped** on the wrapper path (`DeviceVK.hpp:577-578`: `if (!isWrapper) ProcessDeviceExtensions(...)`), and **`vkCreateDevice` itself is never called** when wrapped — `DeviceVK.hpp:707-709`: `if (isWrapper) m_Device = (VkDevice)descVK.vkDevice; else { ...build VkPhysicalDeviceFeatures2 pNext chain incl. Vulkan11/12/13/14Features, robustness2, accel-structure, ray query, etc... vkCreateDevice(...) }` (full feature-request chain at `DeviceVK.hpp:583-748`).
  - **Consequence**: on the VK wrapper path NRI enables **zero** device features/extensions itself — it accepts exactly what the caller's `vkCreateDevice` produced. Anything NRI would normally turn on (dynamic rendering, sync2, maintenance5-10, accel structure, ray query, robustness2, etc.) must already be enabled by the caller, or the corresponding `DeviceDesc.features.*`/`tiers.*` will read as unsupported at runtime — NRI does not probe/require them for wrapped devices beyond best-effort dispatch-table resolution.
  - `Robustness` request (`desc.robustness`) is **only applied on the non-wrapped path** (`DeviceVK.hpp:707-720`); wrapped devices get whatever robustness the caller's `vkCreateDevice` set up.
  - Queues: even though the caller "owns" queue creation, NRI still calls `vkGetDeviceQueue2` itself per `QueueFamilyVKDesc` entry (`DeviceVK.hpp:758-777`) using the caller-given `familyIndex` — there is no way to hand NRI a pre-obtained `VkQueue` handle directly (contrast with the D3D12 wrapper's optional `d3d12Queues` array).
- **Verdict for the team's stated need** ("VK_EXT_device_fault sweep before device creation"): because NRI **never calls `vkCreateDevice`** in wrapper mode and doesn't touch the caller's `VkInstance`/`VkPhysicalDevice` selection, a caller-driven `VK_EXT_device_fault` extension request and `VkPhysicalDeviceFaultFeaturesEXT` chain survive completely untouched — NRI simply doesn't know or care that the extension is enabled (it also never queries it — see §7, zero hits for `device_fault` anywhere in NRI).

### D3D11 wrapper — `Include/Extensions/NRIWrapperD3D11.h:20-34` — same shape as D3D12's (`d3d11Device`, `agsContext`, callback/allocation, `enableD3D11CommandBufferEmulation`, `disableNVAPIInitialization`); `nriCreateDeviceFromD3D11Device` (`Creation.cpp:883-940`) similarly derives the adapter via `IDXGIDevice::GetAdapter`.

---

## 2. Queues and submission

- `QueueType` enum (`Include/NRIDescs.h:1785-1789`): `GRAPHICS, COMPUTE, COPY` — 3 types, matches D3D12 command-list types / VK queue flags.
- Queue acquisition is **not** creation — `CoreInterface::GetQueue(device, queueType, queueIndex, &queue)` (`Include/NRI.h:77`) returns one of the queues pre-created at device-creation time (native) or supplied/created at wrap time (wrapper). Returns `UNSUPPORTED` if no queues of that type exist, `INVALID_ARGUMENT` if `queueIndex` is out of bounds.
- **Multi-queue and VK sharing mode coupling** (doc comment, `NRI.h:72-76`): *"Getting `COMPUTE` and/or `COPY` queues switches VK sharing mode to `VK_SHARING_MODE_CONCURRENT` for resources created without `queueExclusive` flag... adds a requirement to 'get' all async queues BEFORE creation of resources participating into multi-queue activities. Explicit use of `queueExclusive` removes any restrictions."* This is how NRI avoids most manual queue-ownership-transfer barriers by default.
- `QueueSubmitDesc` (`Include/NRIDescs.h:1725-1733`):
```c
NriStruct(QueueSubmitDesc) {
    const NriPtr(FenceSubmitDesc) waitFences;
    uint32_t waitFenceNum;
    const NriPtr(CommandBuffer) const* commandBuffers;
    uint32_t commandBufferNum;
    const NriPtr(FenceSubmitDesc) signalFences;
    uint32_t signalFenceNum;
    NriOptional const NriPtr(SwapChain) swapChain; // required if NRILowLatency is enabled in the swap chain
};
NriStruct(FenceSubmitDesc) {
    NriPtr(Fence) fence;
    uint64_t value;
    Nri(StageBits) stages;
};
```
  Arbitrary N wait-fences + N command buffers + N signal-fences in one call — this is a direct VK `vkQueueSubmit2`-shaped API; D3D12's backend fans it out into `ID3D12CommandQueue::Wait`/`ExecuteCommandLists`/`Signal` calls (see below).
- **D3D12** `QueueD3D12::Submit` (`Source/D3D12/QueueD3D12.hpp:56-82`): loops `waitFences` calling `ID3D12CommandQueue::Wait` per fence, then one `ExecuteCommandLists`, then loops `signalFences` calling `ID3D12CommandQueue::Signal`, then checks `GetDeviceRemovedReason()` and maps to `Result::DEVICE_LOST` on failure. **No internal lock** — D3D12 queues are documented free-threaded by Microsoft.
- **VK** `QueueVK::Submit` (`Source/VK/QueueVK.hpp:77-121`): builds a single `VkSubmitInfo2` (VK 1.3 `vkQueueSubmit2`) covering all wait/signal semaphores + command buffers in one call, wrapped in `ExclusiveScope lock(m_Lock)` (line 78) — **NRI itself internally spinlocks queue submission on VK** (see §3 "hidden management" finding). Also attaches `VkLatencySubmissionPresentIdNV` when `NRILowLatency`+`presentId` is active (lines 110-114).
- `QueueWaitIdle`/`DeviceWaitIdle` — CPU-blocking full drain; D3D12's `QueueWaitIdle` (`QueueD3D12.hpp:84-95`) is implemented by creating a throwaway fence, signaling it, and CPU-waiting — there's no native "queue wait idle" HRESULT call in D3D12 so NRI synthesizes it.
- Cross-queue ownership transfer: see §5 (`TextureBarrierDesc::srcQueue`/`dstQueue`). No queue-ownership-transfer example exists in NRISamples — `AsyncCompute.cpp` (the multi-queue sample) relies on the default `VK_SHARING_MODE_CONCURRENT` behavior above rather than explicit transfers.

---

## 3. Command model

- Lifecycle (`Include/NRI.h`): `CreateCommandAllocator(queue, &commandAllocator)` (line 80) → `CreateCommandBuffer(commandAllocator, &commandBuffer)` (line 81) → `BeginCommandBuffer(commandBuffer, descriptorPool)` (line 151, "one time submit") → `Cmd*` calls → `EndCommandBuffer(commandBuffer)` (line 230, comment: *"D3D11 performs state tracking and resets it there"*) → submit → `ResetCommandAllocator(commandAllocator)` (line 250) to recycle for the next frame. `DestroyCommandAllocator`/`DestroyCommandBuffer` (lines 94-95).
- D3D12 backend: `CommandAllocatorD3D12::Create` calls `ID3D12Device::CreateCommandAllocator` (`Source/D3D12/CommandAllocatorD3D12.hpp:6-7`); `CreateCommandBuffer` creates a `ID3D12GraphicsCommandList` off it (`CommandAllocatorD3D12.hpp:12`).
- **Secondary command buffers / D3D12 bundles are not exposed anywhere** — no "Secondary"/"Bundle" symbol exists in `NRIDescs.h` or the command-buffer descs (grep came back empty). `CommandBufferD3D12Desc`/`CommandBufferVKDesc` (wrapper structs) wrap only primary/top-level command lists.
- **Header-declared thread-safety contract** (`Include/NRI.h:28-36`): *"Threadsafe: yes - free-threaded access / Threadsafe: no - external synchronization required... `Create*` — thread safe; `Destroy*` — not thread safe (because of VK); `Cmd*` — not thread safe."*
- **Verified from source — no hidden locking on `Cmd*` recording**: `Source/VK/CommandBufferVK.hpp` and `Source/D3D12/CommandBufferD3D12.hpp` contain **zero** `Lock`/`mutex`/`ExclusiveScope` usages (grep confirmed). Recording is genuinely lock-free/external-sync-only as documented — safe for one-thread-per-command-buffer multithreaded recording, matching `NRISamples/Source/MultiThreading.cpp` (per-thread `CommandAllocator`+`CommandBuffer` array in `QueuedFrame`, `MultiThreading.cpp:39-46`).
- **But NRI does silently lock in a couple of other spots** — contradicting a literal reading of "no hidden management of any kind" from the README/header goals (`NRI.h:25`):
  - `QueueVK::Submit`/`WaitIdle` lock `m_Lock` (`Source/VK/QueueVK.hpp:78,124`) — serializes concurrent `QueueSubmit`/`QueueWaitIdle` calls from multiple threads against the *same* `Queue` object on VK only (not D3D12, since `ID3D12CommandQueue` methods are documented free-threaded by Microsoft; `vkQueueSubmit`/`vkQueueWaitIdle` require external synchronization per the VK spec, so NRI is quietly providing that synchronization for you on this one queue-level operation).
  - `SwapChainVK::AcquireNextTexture` also takes `ExclusiveScope lock(m_Queue->GetLock())` (`Source/VK/SwapChainVK.hpp:441`) — shares the presentation queue's lock with `QueueSubmit`.
  - `Lock`/`ExclusiveScope` also appear in `DeviceVK.hpp`, `DescriptorPoolVK.hpp`, `CommandAllocatorVK.hpp` (VK), and `DescriptorPoolD3D12.hpp`, `CommandAllocatorD3D12.hpp`, `PipelineD3D12.hpp`, `DeviceD3D12.hpp` (D3D12) — i.e. pooled-allocation paths (descriptor pool alloc, command-allocator reset bookkeeping, PSO cache) get internal synchronization on both backends, even though "Create*" is documented thread-safe generically; these are the "additional restrictions can apply" the header warns about.
  - `Source/Shared/Lock.h:23-53` — the primitive is a tiny spin-lock (`std::atomic_uint32_t` + `_mm_pause()`), not a `std::mutex`.

---

## 4. Fences and queries

- **`nri::Fence` is a genuine GPU **timeline** fence**, not a binary/event fence:
  - D3D12: 1:1 wrap of `ID3D12Fence` (`Source/D3D12/FenceD3D12.hpp`). `Wait(value)` (lines 39-55) either busy-waits on `GetCompletedValue()` or (if an event handle exists) calls `SetEventOnCompletion` + `WaitForSingleObjectEx(..., NRI_TIMEOUT_FENCE)`.
  - VK: `VkSemaphore` created with `VK_SEMAPHORE_TYPE_TIMELINE` (`Source/VK/FenceVK.hpp:10-23`), except when `initialValue == SWAPCHAIN_SEMAPHORE` (`(uint64_t)-1`, `Include/Extensions/NRISwapChain.h:14`) in which case it's created as `VK_SEMAPHORE_TYPE_BINARY` for acquire/release swapchain semaphores.
  - `NRI_TIMEOUT_FENCE = 5000u` (5 sec, `Source/Shared/SharedExternal.h:53`), `NRI_TIMEOUT_PRESENT = 1000u` (1 sec, line 49) — hard-coded CPU wait timeouts baked into the backend (overridable only by predefining the macro before building NRI from source).
- CPU wait: `CoreInterface::Wait(fence, value)` (`NRI.h:246`, "on host"). CPU/GPU signal: `FenceSubmitDesc{fence, value, stages}` inside `QueueSubmitDesc.waitFences`/`.signalFences` — GPU-side wait/signal is expressed purely through submission, there's no separate `QueueSignal`/`QueueWait` entry point in `CoreInterface` (D3D12 backend synthesizes `ID3D12CommandQueue::Signal`/`Wait` calls internally from the submit desc, see §2).
- `GetFenceValue(fence)` (`NRI.h:247`) reads the current completed value (`ID3D12Fence::GetCompletedValue()` / `vkGetSemaphoreCounterValue`).
- **NVRHI event-query → NRI mapping**: NVRHI's `EventQuery`/`waitEventQuery` (a CPU-observable "has this GPU work finished" signal used for swapchain frame-slot pacing) maps 1:1 onto an `nri::Fence` signaled at the tail of a frame's `QueueSubmit` + a CPU `Wait(fence, expectedValue)` at the top of the next candidate frame. This exact pattern is spelled out as canonical usage in `Include/Extensions/NRISwapChain.h:139-207` (docblock): a single `frameFence` is signaled every frame with an ever-increasing value; the next frame's pacing does `NRI.Wait(frameFence, frameIndex >= QUEUED_FRAME_NUM ? 1 + frameIndex - QUEUED_FRAME_NUM : 0)` before recording (`NRISwapChain.h:166-167`). There is no separate "event query" object — the timeline fence *is* the frame-pacing primitive, and swap chain acquire/release use their own per-image `Fence`s created with the special `SWAPCHAIN_SEMAPHORE` initial value (binary semaphores under the hood on VK).
- Query pools: `QueryType` enum (`NRIDescs.h:1602-1610`): `TIMESTAMP` (needs `features.timestamp`), `TIMESTAMP_COPY_QUEUE` (needs `features.timestampCopyQueue`, COPY-queue only), `OCCLUSION` (needs `features.occlusion`), `PIPELINE_STATISTICS` (see `PipelineStatisticsDesc`, 14 fields incl. mesh/task invocation counts gated by `features.meshShaderPipelineStats`), `ACCELERATION_STRUCTURE_SIZE`, `ACCELERATION_STRUCTURE_COMPACTED_SIZE`, `MICROMAP_COMPACTED_SIZE`.
  `QueryPoolDesc{ queryType, capacity }` (`NRIDescs.h:1612-1615`).
  Recording: `CmdResetQueries`/`CmdBeginQuery`/`CmdEndQuery`/`CmdCopyQueries` (GPU-side copy into a `Buffer` for readback, `NRI.h:220-223`); host-side: `ResetQueries` (`NRI.h:238`) and `GetQuerySize` (`NRI.h:239`, byte size per query result — needed to size the readback buffer/offset math).
  `GetCalibratedTimestamps(queue, &timestampGPU, &timestampCPU)` (`NRI.h:240`) — D3D12 via `ID3D12CommandQueue::GetClockCalibration` (`QueueD3D12.hpp:51-54`), VK via `VK_KHR_calibrated_timestamps` (`QueueVK.hpp:47-75`), CPU domain matched to `QueryPerformanceCounter` on Windows for both backends.

---

## 5. Barriers

Full shapes (`Include/NRIDescs.h:583-619`):
```c
NriStruct(AccessStage)       { Nri(AccessBits) access; Nri(StageBits) stages; };
NriStruct(AccessLayoutStage) { Nri(AccessBits) access; Nri(Layout) layout; Nri(StageBits) stages; };

NriStruct(GlobalBarrierDesc) { Nri(AccessStage) before; Nri(AccessStage) after; };

NriStruct(BufferBarrierDesc) {
    NriPtr(Buffer) buffer;
    Nri(AccessStage) before;
    Nri(AccessStage) after;
};

NriStruct(TextureBarrierDesc) {
    NriPtr(Texture) texture;
    Nri(AccessLayoutStage) before;
    Nri(AccessLayoutStage) after;
    Nri(Dim_t) mipOffset;
    Nri(Dim_t) mipNum;      // can be "REMAINING"
    Nri(Dim_t) layerOffset;
    Nri(Dim_t) layerNum;    // can be "REMAINING"
    Nri(PlaneBits) planes;
    NriOptional NriPtr(Queue) srcQueue;
    NriOptional NriPtr(Queue) dstQueue;
};

// NOTE: the task brief's guessed name "BarrierGroupDesc" does not exist — the actual
// batching struct is named "BarrierDesc":
NriStruct(BarrierDesc) {
    const NriPtr(GlobalBarrierDesc) globals; uint32_t globalNum;
    const NriPtr(BufferBarrierDesc) buffers; uint32_t bufferNum;
    const NriPtr(TextureBarrierDesc) textures; uint32_t textureNum;
};
```
Submitted in one call: `CmdBarrier(commandBuffer, barrierDesc)` (`NRI.h:166`) — batches any number of global/buffer/texture barriers per call (VK: one `vkCmdPipelineBarrier2`; D3D12: one `ID3D12GraphicsCommandList7::Barrier` when enhanced barriers are available).

- `StageBits` (`NRIDescs.h:413-481`) — 24 explicit bits (`INDEX_INPUT`...`CLEAR_STORAGE`, plus `INDIRECT` modifier) + umbrella combos (`GRAPHICS_SHADERS`, `RAY_TRACING_SHADERS`, `ALL_SHADERS`, `GRAPHICS`). `ALL = 0` (lazy default), `NONE = 0x7FFFFFFF`.
- `AccessBits` (`NRIDescs.h:485-539`) — 23 bits (`INDEX_BUFFER`...`CLEAR_STORAGE`) + umbrella combos (`COLOR_ATTACHMENT`, `DEPTH_STENCIL_ATTACHMENT`, `ACCELERATION_STRUCTURE`, `MICROMAP`). `NONE = 0` maps to D3D12 `COMMON`/VK `GENERAL` if AgilitySDK/enhanced barriers unavailable.
- `Layout` (`NRIDescs.h:544-570`) — 15 values (`UNDEFINED, GENERAL, PRESENT, COLOR_ATTACHMENT, DEPTH_STENCIL_ATTACHMENT, DEPTH_READONLY_STENCIL_ATTACHMENT, DEPTH_ATTACHMENT_STENCIL_READONLY, DEPTH_STENCIL_READONLY, SHADING_RATE_ATTACHMENT, INPUT_ATTACHMENT, SHADER_RESOURCE, SHADER_RESOURCE_STORAGE, COPY_SOURCE, COPY_DESTINATION, RESOLVE_SOURCE, RESOLVE_DESTINATION`); **ignored entirely if `features.enhancedBarriers` is unsupported** (legacy D3D12 barrier fallback).
- **Real RT(render-target)→SRV-style transition, quoted verbatim from `NRISamples/Source/SceneViewer.cpp`** (swap-chain color target: UNDEFINED→COLOR_ATTACHMENT at frame start, then COLOR_ATTACHMENT→PRESENT at frame end — this is NRI's idiomatic terse form, reusing one `TextureBarrierDesc`/`BarrierDesc` pair):
```cpp
// Frame start (SceneViewer.cpp:716-726)
nri::TextureBarrierDesc textureBarriers = {};
textureBarriers.texture = swapChainTexture.texture;
textureBarriers.after = {nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT};
textureBarriers.layerNum = 1;
textureBarriers.mipNum = 1;

nri::BarrierDesc barrierDesc = {};
barrierDesc.textureNum = 1;
barrierDesc.textures = &textureBarriers;

NRI.CmdBarrier(commandBuffer, barrierDesc);

// ... draw ...

// Frame end (SceneViewer.cpp:852-855)
textureBarriers.before = textureBarriers.after;
textureBarriers.after = {nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE};
NRI.CmdBarrier(commandBuffer, barrierDesc);
```
  In practice a single-texture transition is ~4-6 lines once the `BarrierDesc` wrapper is set up once and reused — more explicit than NVRHI's auto-barriers (you must state before/after access+layout+stage yourself) but not dramatically more verbose than a raw D3D12/VK barrier struct, since NRI batches `globals`/`buffers`/`textures` into one `CmdBarrier` call instead of one API call per resource.
- **Cross-queue ownership transfer** (`TextureBarrierDesc::srcQueue`/`dstQueue`, `NRIDescs.h:604-607`, comment: "Queue ownership transfer is potentially needed only for `SharingMode::EXCLUSIVE` textures") — no worked example exists anywhere in NRISamples (`AsyncCompute.cpp` has zero `srcQueue`/`dstQueue` hits); the samples instead rely on the default `VK_SHARING_MODE_CONCURRENT` behavior triggered by calling `GetQueue` for COMPUTE/COPY queues (§2). Teams doing explicit `queueExclusive` resources will have no in-repo reference for the transfer barrier pattern.
- D3D12 barrier conversion has a couple of open TODOs worth flagging: `Source/D3D12/CommandBufferD3D12.hpp:1079` (`// TODO: verify that it works` on `D3D12_TEXTURE_BARRIER_FLAG_DISCARD` for `UNDEFINED` before-layout) and `:1168,1220` (`// TODO: "bufferForAccelerationStructuresSizes" is completely hidden from a user, transition needs to be done under the hood`).

---

## 6. Swapchain (`Include/Extensions/NRISwapChain.h`)

- `SwapChainInterface` (lines 125-137): `CreateSwapChain`, `DestroySwapChain`, `GetSwapChainTextures` (returns `Texture* const*` + count), `GetDisplayDesc` (HDR/luminance/chromaticity info, returns `FAILURE` if window is off all monitors), `AcquireNextTexture(swapChain, acquireSemaphoreFence, &textureIndex)`, `WaitForPresent(swapChain)` ("call once right before input sampling"), `QueuePresent(swapChain, releaseSemaphoreFence)`.
- **No resize API at all** — the interface has no `Resize`/`SetSize` entry point. Confirmed resize pattern (`NRISamples/Source/Resize.cpp:246,259`): `NRI.DestroySwapChain(m_SwapChain)` then `NRI.CreateSwapChain(*m_Device, swapChainDesc, m_SwapChain)` — full destroy/recreate, not in-place.
- `SwapChainDesc` (`NRISwapChain.h:84-98`): `window` (tagged union: `WindowsWindow{hwnd}` / `X11Window` / `WaylandWindow` / `MetalWindow`), `queue` (must be `GRAPHICS` or `COMPUTE`, latter needs `features.presentFromCompute`), `width`/`height`, `textureNum` (desired; real count via `GetSwapChainTextures`), `format` (`SwapChainFormat`, desired; real format via `GetTextureDesc`), `flags` (`SwapChainBits`), `queuedFrameNum` (aka max-frame-latency, "mostly for D3D11"), `scaling`/`gravityX`/`gravityY` (silently ignored without `features.resizableSwapChain`; **VK explicitly raises `OUT_OF_DATE` on resize if scaling unsupported**).
- `SwapChainFormat` (lines 27-32): `BT709_G10_16BIT` (linear HDR-capable), `BT709_G22_8BIT`, `BT709_G22_10BIT` (SDR/LDR), `BT2020_G2084_10BIT` (PQ HDR) — combines color-space + transfer-function + bit depth into 4 named presets rather than exposing raw DXGI/VK format+colorspace enums.
- `SwapChainBits` (lines 47-53): `VSYNC`, `WAITABLE` (unlocks `WaitForPresent`, needs `features.waitableSwapChain`), `ALLOW_TEARING`, `ALLOW_LOW_LATENCY` (needs `features.lowLatency`, gates `NRILowLatency` extension usage).
- Frame-in-flight/latency: `queuedFrameNum` at swapchain-creation time (D3D11-flavored `IDXGIDevice1::SetMaximumFrameLatency`, `Source/D3D12/SwapChainD3D12.hpp:141-144`) + the `WAITABLE` flag which surfaces D3D12's frame-latency waitable object (`m_FrameLatencyWaitableObject`, `WaitForSingleObjectEx(..., NRI_TIMEOUT_PRESENT)` at `SwapChainD3D12.hpp:196-208`, `UNSUPPORTED` if the object doesn't exist) — this is NRI's closest analog to NVRHI's swapchain frame-pacing, complementary to (not a replacement for) the fence-based pacing in §4.
- Present-result codes: `SwapChainD3D12::Present` (`SwapChainD3D12.hpp:210-230`) maps `IDXGISwapChain::Present` HRESULT via `GetResultFromHRESULT` → `DXGI_ERROR_DEVICE_REMOVED/RESET/DRIVER_INTERNAL_ERROR/DEVICE_HUNG` → `Result::DEVICE_LOST`. VK path maps `VK_ERROR_DEVICE_LOST`→`DEVICE_LOST`, `VK_ERROR_SURFACE_LOST_KHR`/`VK_ERROR_OUT_OF_DATE_KHR`→`OUT_OF_DATE` (`Source/VK/ConversionVK.h:519-549`, see §7). `AcquireNextTexture`/`WaitForPresent`/`QueuePresent` all funnel through the same HRESULT/VkResult→`Result` mapping, so `DEVICE_LOST`/`OUT_OF_DATE` can surface from any of Acquire/Present/WaitForPresent/QueueSubmit, matching the `Result` enum doc comment exactly (§7).

---

## 7. Result/error surface

`Result` enum, verbatim (`Include/NRIDescs.h:106-120`):
```c
NriEnum(Result, int8_t,
    // All bad, but optionally require an action ("callbackInterface.AbortExecution" is not triggered)
    DEVICE_LOST             = -3,   // may be returned by "QueueSubmit*", "*WaitIdle", "AcquireNextTexture", "QueuePresent", "WaitForPresent"
    OUT_OF_DATE             = -2,   // VK: swap chain is out of date...; D3D12: shader cache is stale
    INVALID_SDK             = -1,   // D3D12: some interfaces are missing (D3D12Core.dll load failure, SDK mismatch, dev mode off)

    SUCCESS                 = 0,

    // All bad, most likely a crash or a validation error will happen next ("callbackInterface.AbortExecution" is triggered)
    FAILURE                 = 1,
    INVALID_ARGUMENT        = 2,
    OUT_OF_MEMORY           = 3,
    UNSUPPORTED             = 4
);
```
- **The AbortExecution trigger is literally `(int8_t)result > 0`** — implemented at `Source/Shared/SharedExternal.hpp:914-916` inside `DeviceBase::ReportMessage`, confirming the doc comment exactly: negative codes (`DEVICE_LOST`/`OUT_OF_DATE`/`INVALID_SDK`) never call `AbortExecution`; positive codes (`FAILURE`/`INVALID_ARGUMENT`/`OUT_OF_MEMORY`/`UNSUPPORTED`) always do (if the callback is set).
- HRESULT→Result mapping (`Source/Shared/SharedExternal.hpp:121-...`): `DXGI_ERROR_DEVICE_REMOVED/RESET/DRIVER_INTERNAL_ERROR/DEVICE_HUNG`→`DEVICE_LOST`; `E_NOINTERFACE`/`D3D12_ERROR_INVALID_REDIST`/`DXGI_ERROR_SDK_COMPONENT_MISSING`→`INVALID_SDK`; `E_INVALIDARG`/`E_POINTER`/`E_HANDLE`→`INVALID_ARGUMENT`; `DXGI_ERROR_UNSUPPORTED`→`UNSUPPORTED`; OOM variants→`OUT_OF_MEMORY`.
- VkResult→Result mapping, verbatim (`Source/VK/ConversionVK.h:519-549`):
```cpp
constexpr Result GetResultFromVkResult(VkResult vkResult) {
    if (vkResult >= 0) return Result::SUCCESS;
    switch (vkResult) {
        case VK_ERROR_DEVICE_LOST: return Result::DEVICE_LOST;
        case VK_ERROR_SURFACE_LOST_KHR:
        case VK_ERROR_OUT_OF_DATE_KHR: return Result::OUT_OF_DATE;
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
        case VK_ERROR_FORMAT_NOT_SUPPORTED: case VK_ERROR_INCOMPATIBLE_DRIVER:
        case VK_ERROR_FEATURE_NOT_PRESENT: case VK_ERROR_EXTENSION_NOT_PRESENT:
        case VK_ERROR_LAYER_NOT_PRESENT: return Result::UNSUPPORTED;
        case VK_ERROR_OUT_OF_HOST_MEMORY: case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        case VK_ERROR_OUT_OF_POOL_MEMORY: case VK_ERROR_FRAGMENTATION:
        case VK_ERROR_FRAGMENTED_POOL: return Result::OUT_OF_MEMORY;
        default: return Result::FAILURE;
    }
}
```
  Note `VK_SUBOPTIMAL_KHR` is `>= 0` so it maps to `SUCCESS` (not surfaced as `OUT_OF_DATE`) — a caller wanting to react to suboptimal swapchains must check the raw VkResult itself; NRI doesn't expose it (it's swallowed as success at the `Result` layer).
- **Message callback plumbing**: `CallbackInterface{ MessageCallback(Message type, file, line, message, userArg); NriOptional AbortExecution(userArg); NriOptional userArg; }` (`NRIDeviceCreation.h:26-30`). Default implementation if the app doesn't supply one: `Creation.cpp:56-74` — `MessageCallback` does `fprintf(stderr, ...)` + `OutputDebugStringA` on Windows; default `AbortExecution` calls `DebugBreak()`/`raise(SIGTRAP)`. `Message` enum (`NRIDeviceCreation.h:11-15`): `INFO, WARNING, ERROR` (comment: *`"wingdi.h" must not be included after`* — `ERROR` collides with the Windows macro, `NRIMacro.h:5` `#undef ERROR`).
- **Debug names**: `CoreInterface::SetDebugName(object, name)` (`NRI.h:269`) — one call works for any `NriForwardStruct`-declared object type (skipped for buffers/textures in D3D if not yet bound to memory).
- **Host + device annotations**: host-level `nriBeginAnnotation`/`nriEndAnnotation`/`nriAnnotation`/`nriSetThreadName` (free functions, `NRI.h:59-62`, comment: *"Host annotations currently use NVTX (NVIDIA Nsight Systems)"*); command-buffer-level `CmdBeginAnnotation`/`CmdEndAnnotation`/`CmdAnnotation` (`NRI.h:226-228`); queue-level `QueueBeginAnnotation`/`QueueEndAnnotation`/`QueueAnnotation` (`NRI.h:233-235`, comment: "D3D11: NOP"). D3D12 queue annotations route through PIX (`m_Device.HasPix()` ? custom PIX loader : static `PIXBeginEvent`/`PIXSetMarker`, `Source/D3D12/QueueD3D12.hpp:30-49`) — PIX marker support requires `WinPixEventRuntime.dll` to be discoverable next to the exe (per `NRI.h:57` comment).
- **No DRED / device-fault / breadcrumb / buffer-marker facility exists anywhere in NRI** — exhaustive case-insensitive grep for `DRED|breadcrumb|DeviceRemoved|device_fault|buffer.?marker` across the whole `Source/` tree returns **only** 4 hits, all bare `ID3D12Device::GetDeviceRemovedReason()` calls used purely to detect device-lost state after the fact (`Source/D3D12/DeviceD3D12.hpp:392`, `SwapChainD3D12.hpp:190,199`, `QueueD3D12.hpp:78`) — i.e. NRI polls "is the device gone" but has **no** DRED breadcrumb/page-fault configuration, no `VK_EXT_device_fault`/`VK_EXT_device_address_binding_report` usage, no `ID3D12DeviceRemovedExtendedDataSettings`, no custom buffer-marker/breadcrumb ring, and no PIX GPU-crash-dump integration. **This confirms the team's own first-party GPU crash diagnostics stack (native handles + `diag://` .arcdiag assets) has nothing to route around or reconcile with inside NRI — NRI is fully silent on this front and won't fight a native-handle-based DRED/device-fault instrumentation layer built alongside/around it**, provided the device is created through the wrapper path (native `nriCreateDevice` path bypasses the team's own DRED setup entirely, per §1).

---

## 8. The NONE backend (`Source/NONE/ImplNONE.cpp`)

- Enabled via `GraphicsAPI::NONE` bit (`NRIDescs.h:99`, comment: *"Supports everything, does nothing, returns dummy non-NULL objects and ~0-filled descs, available if `NRI_ENABLE_NONE_SUPPORT = ON` in CMake"*).
- `DeviceNONE` constructor (`ImplNONE.cpp:12-...`) fabricates a maximally-permissive `DeviceDesc` — huge limits (`viewport.maxNum=16`, `dimensions.attachmentMaxDim=16384`, `descriptorSet.samplerMaxNum=1000000`, `shaderStage.compute.dispatchMaxDim=(uint32_t)-1`, etc.) and 4 queues per `QueueType` (line 19) — so capability-gated engine code takes its "everything supported" branch under NONE.
- Every `Create*` function returns a `DummyObject<T>()` — a non-null sentinel pointer (`(T*)(size_t)1`, `ImplNONE.cpp:7-10`), **not real backing memory**: `CreateBuffer`, `CreateTexture`, `CreateFence`, `CreateQueue`, `CreateCommandAllocator`, `CreateCommandBuffer`, `CreatePipelineLayout`, `CreatePipeline`(x2), `CreatePipelineCache`, `CreateQueryPool`, `CreateSampler`, `CreateBufferView`/`CreateTextureView`, `CreateDescriptorPool`, `AllocateMemory` all follow this pattern (`ImplNONE.cpp:251-430`).
- `QueueSubmit` (`ImplNONE.cpp:602-604`), `DeviceWaitIdle`/`QueueWaitIdle` (606-612) unconditionally return `Result::SUCCESS` with **no actual work performed** — safe for headless CI.
- `Fence::Wait` is a **no-op** (line 614-615) and `GetFenceValue` **always returns 0** (617-619) — a caller doing `Wait(fence, N)` then `assert(GetFenceValue(fence) >= N)` would fail under NONE; any code relying on `Wait()`'s return semantics (rather than manually polling `GetFenceValue`) is safe.
- `MapBuffer` returns `nullptr` unconditionally (624-626) — code paths that unconditionally memcpy into the mapped pointer without a null-check will crash under NONE; this is the one sharp edge for headless unit tests that exercise upload paths.
- `GetBufferDeviceAddress` returns `0` (631-633) — matches D3D11's documented behavior (`NRI.h:260`: "D3D11: returns 0"), so code already tolerating 0-address on D3D11 is NONE-safe too.
- **Verdict**: yes, usable for headless unit tests of engine code that only needs "a `Device` and correctly-shaped dummy handles exist and API calls don't crash/block" — CPU-side flow/logic tests, resource-lifetime tests, submission-sequencing tests. Not usable for anything that reads back real GPU-produced data (`MapBuffer` returns null, no compute actually runs) or that depends on `GetFenceValue` reflecting real progress.

---

## 9. Zero-allocation claim & C ABI

### Scratch/stack allocation, not "zero allocation" in the literal sense

- `NRI_ALLOCATE_SCRATCH(device, T, elementNum)` macro (`Source/Shared/SharedExternal.h:316-325`) and the `Scratch<T>` RAII wrapper (`SharedExternal.h:335-364`): for `elementNum * sizeof(T) + alignof(T) <= NRI_MAX_STACK_ALLOC_SIZE` (**32 KB**, `SharedExternal.h:65`), it uses `alloca()` — genuinely zero heap allocation, stack-only, freed automatically on scope exit. Above that threshold it falls back to the app-supplied `AllocationCallbacks.Allocate`/`.Free` (`Scratch::~Scratch`, line 345-348 calls `m_Allocator.Free` only `if (m_IsHeap)`).
- This scratch mechanism is used pervasively for per-call transient arrays — e.g. `QueueVK::Submit` scratch-allocates its `VkSemaphoreSubmitInfo`/`VkCommandBufferSubmitInfo` arrays this way (`QueueVK.hpp:80,88,94`) rather than using `std::vector`.
- Longer-lived internal containers (`Vector<T> = std::vector<T, StdAllocator<T>>`, `UnorderedMap`, `Map`, `String`; `SharedExternal.h:481-489`) all route through `StdAllocator<T>`, a `std::allocator`-compatible adaptor over the app's `AllocationCallbacks` (`AlignedMalloc`/`AlignedRealloc`/`AlignedFree` on Windows via `_aligned_malloc` family, `Creation.cpp:78-88`; a hand-rolled aligned-alloc emulation on non-Windows, `Creation.cpp:92-120`) — so **all** NRI-internal heap traffic (not just resource creation) is redirectable through `DeviceCreationDesc::allocationCallbacks`, including the case where `disable3rdPartyAllocationCallbacks` is *not* set, in which case D3D12MA/VMA-style sub-allocators also get pointed at the same callbacks (`m_AllocationCallbackPtr`, `DeviceVK.hpp:438`).
- **Practical reading of "zero-allocation"**: NRI avoids allocating for the common per-call/per-frame temporaries (scratch arrays under 32 KB use the stack), and everything else funnels through a single pluggable allocator seam rather than calling `malloc`/`new` directly scattered through the backends — it is not a claim that NRI *never* allocates.

### C ABI mechanics (`Include/NRIMacro.h`, `Include/NRI.h:44-53`)

- `NRIDescs.h`/`NRI.h` are **written once** and compiled twice via macro indirection selected by `NRI_CPP` (auto-detected, or forced via `NRI_FORCE_C`): `NriStruct(name)`, `NriEnum(name,type,...)`, `NriRef(name)`, `NriPtr(name)` etc. (`NRIMacro.h:284-360`) expand to:
  - **C++ mode** (`NRI_CPP` defined, `NRIMacro.h:284-321`): `namespace nri { ... }`, `enum class name : type`, struct/union names unprefixed, `NriRef(name)` → `name&` (C++ reference).
  - **C mode** (`NRIMacro.h:322-360`): no namespace, every type/enum-value gets an `Nri`/`NRI_` prefix (`Nri##name`/`NRI_##name` via `NRI_NAME_C`/`NRI_CONST_NAME_C`, `NRIMacro.h:10-12`), `NriRef(name)` → `name*` (plain pointer) — i.e. the exact same header produces a fully flat, prefixed C API (`NriBufferBarrierDesc`, `NRI_MESSAGE_ERROR`, etc.) for non-C++ consumers.
- **Interfaces are POD function-pointer tables**, not vtables/COM: e.g. `CoreInterface` (`NRI.h:65-278`) is a plain struct of `ReturnType (NRI_CALL *FuncName)(...)` members. Acquired via `nriGetInterface(device, interfaceName, interfaceSize, interfacePtr)` (`NRI.h:53`) with the `NRI_INTERFACE(name)` helper macro expanding to `(#name, sizeof(name))` — i.e. `nriGetInterface(device, NRI_INTERFACE(CoreInterface), &coreInterface)`. This is a size+name-checked table fetch (protects against header/binary skew) rather than a COM `QueryInterface`/vtable-based dispatch, and works identically from C or C++ callers since the interface struct itself is a plain-data layout in both language modes.
- Extension interfaces (`SwapChainInterface`, `WrapperD3D12Interface`, `WrapperVKInterface`, `HelperInterface`, `LowLatencyInterface`, `StreamerInterface`, `ImguiInterface`, `MeshShaderInterface`, `RayTracingInterface`, `UpscalerInterface`) are fetched the same way, each independently, so an app only pays for (and only needs to null-check) the interfaces it actually queries.
- All object handles (`Device*`, `Queue*`, `Buffer*`, ...) are **opaque, non-refcounted pointers** — no `AddRef`/`Release`, matching NVRHI's refcounted-handle model being explicitly abandoned; lifetime is caller-managed via matching `Create*`/`Destroy*` pairs.

---

## Surprising / concerning findings (cross-cutting)

1. **No DRED anywhere, on either creation path's NRI-owned code.** `nriCreateDevice`'s native D3D12 path calls `D3D12CreateDevice` (`DeviceD3D12.hpp:236`) with no `ID3D12DeviceRemovedExtendedDataSettings` configuration beforehand — a team relying on `nriCreateDevice` directly for D3D12 would silently lose DRED breadcrumbs/page-fault reporting unless they patch NRI's source. The **wrapper path** (`nriCreateDeviceFromD3D12Device`) is the only way to keep DRED, since NRI never calls `D3D12CreateDevice` when `d3d12Device` is supplied (§1).
2. **VK wrapper path never calls `vkCreateDevice`** (`DeviceVK.hpp:707-709`) — confirmed zero device-feature/extension negotiation by NRI when wrapped; a caller-driven `VK_EXT_device_fault` sweep is completely untouched by NRI, but conversely NRI also does **not** verify/require any of its normal feature set (sync2, dynamic rendering, accel structure, etc.) on wrapped devices — capability-mismatch bugs would only surface as `UNSUPPORTED` results at individual `Create*`/`Cmd*` call sites, not at device-creation time.
3. **VK-only internal spinlock on `QueueSubmit`/`QueueWaitIdle`/`AcquireNextTexture`** (`Source/VK/QueueVK.hpp:78,124`, `Source/VK/SwapChainVK.hpp:441`) is real hidden management that the D3D12 backend doesn't need or do — worth knowing if the team profiles submission-thread contention differently across backends, and it means "not thread-safe" in the header's blanket sense isn't 100% true for `QueueSubmit` on VK specifically (NRI already serializes it for you there).
4. **`VK_SUBOPTIMAL_KHR` is swallowed as `Result::SUCCESS`** (`ConversionVK.h:519-521`, any `vkResult >= 0` short-circuits to `SUCCESS`) — an engine wanting to proactively recreate the swapchain on "suboptimal" (common on Wayland/some compositors) gets no signal from the `Result` enum; would need to special-case at the raw VkResult level, which NRI doesn't expose.
5. **`GetFenceValue` under the NONE backend always returns 0**, never reflecting a "signaled" value — fine for `Wait()`-based sync (also a no-op under NONE) but a trap for any test code that manually polls/asserts on fence values against NONE.
6. **`MapBuffer` returns `nullptr` under NONE** with no engine-level guard implied by NRI itself — any upload path lacking a null check will crash under headless/CI runs specifically (not under any real backend), a footgun the team should grep for before adopting NONE for CI.
7. **No secondary command buffers / D3D12 bundles anywhere in the API** — if the current NVRHI-based renderer uses bundles for any static draw replay, that pattern has no NRI equivalent and must be redesigned around full primary command buffers.
8. **No queue-ownership-transfer example anywhere in NRISamples** despite the feature existing in `TextureBarrierDesc` — the team will be first-in-house to validate the `srcQueue`/`dstQueue` mechanics if they use `queueExclusive` resources across queues.
9. A handful of backend TODOs worth a second look before depending on the affected paths: D3D12 barrier `DISCARD` flag correctness for `UNDEFINED`-layout textures is explicitly marked `// TODO: verify that it works` (`CommandBufferD3D12.hpp:1079`); acceleration-structure buffer transitions are described as "completely hidden from a user" and done "under the hood" with a `// TODO` on the offset/size precision (`CommandBufferD3D12.hpp:1168,1220,1385`); the D3D12 backend also carries a `// TODO: this code is currently needed to disable known false-positive errors reported by the debug layer` (`DeviceD3D12.hpp:268`), meaning some D3D12 debug-layer warnings NRI-generated code triggers are being suppressed at the source rather than fixed.
10. **WGPU is a 5th backend** (`Source/WGPU/`) not mentioned in the task brief — heavily TODO-laden (busy-wait async device requests, emulated timeline fences via submission-index polling, no DRED-equivalent concept either) — irrelevant to a D3D12/VK migration decision but worth knowing it exists if anyone on the team assumed NRI was D3D12/VK/D3D11-only.
