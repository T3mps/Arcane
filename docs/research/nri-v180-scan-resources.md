# NRI Source Survey — Resources, Memory, Descriptors, Pipelines, Shader Binding, Upload Paths

Scope: NVIDIA NRI pinned at **v180** (`NRI_VERSION 180`, `NRI_VERSION_DATE "23 June 2026"`,
`Include/NRI.h:41-42`), cloned read-only at `D:\dev\starworks\Arcane\.example\NRI`
(git: `4b48531 Fixed a typo`, tag `v180`). Samples at
`D:\dev\starworks\Arcane\.example\NRISamples` (same `NRI_VERSION 180` vendored copy).

Audience: engine team evaluating migration from **NVRHI** (refcounted lifetime, volatile
constant buffers with automatic versioning, `writeBuffer` inside an open command list, cached
binding sets keyed on stable handles, VK register-shift `VulkanBindingOffsets{t,s,b,u}`).

---

## 1. Memory model

### Two parallel resource/memory flows (NRI intentionally exposes both)

**VK-style flow** (`Include/NRI.h:106-123`, `CoreInterface`):
1. `CreateBuffer` / `CreateTexture` — no backing memory yet.
2. `GetBufferMemoryDesc` / `GetTextureMemoryDesc(resource, MemoryLocation) -> MemoryDesc` — query size/alignment/type for a given `MemoryLocation`.
3. Group returned `MemoryDesc`s by `MemoryType` (opaque, encodes GAPI-specific heap info — `Include/NRIDescs.h:713`), optionally sort by alignment.
4. `AllocateMemory(AllocateMemoryDesc) -> Memory` (called even when `mustBeDedicated=true`).
5. `BindBufferMemory` / `BindTextureMemory` (batched, `BindBufferMemoryDesc{buffer,memory,offset}` — `NRIDescs.h:756-766`).
6. Optional helper: `HelperInterface::CalculateAllocationNumber` / `AllocateAndBindMemory` (`Include/Extensions/NRIHelper.h:72-73`) does steps 2-5 automatically for a `ResourceGroupDesc` (grouping/bin-packing logic lives in `Source/Shared/HelperInterface.hpp:399-641`, `HelperDeviceMemoryAllocator`).

**D3D12-style flow** (`NRI.h:125-133`):
- `CreateCommittedBuffer` / `CreateCommittedTexture(memoryLocation, priority, desc)` — one-shot, dedicated allocation.
- `CreatePlacedBuffer` / `CreatePlacedTexture(memory, offset, desc)` — place into an existing `Memory`. The `NriDeviceHeap` / `NriDeviceUploadHeap` / `NriHostUploadHeap` / `NriHostReadbackHeap` macros (`NRI.h:88-91`, expand to `0,0`/`0,1`/`0,2`/`0,3`) let you pass a placed-resource call with **implicit VMA-backed memory** — `memory=NULL, offset=<heap-id>` and NRI creates a VMA-backed device allocation under the hood (used pervasively in samples, e.g. `NRISamples/Source/DescriptorHeapIndexing.cpp:179,190,209,238`).

### `MemoryDesc` / `AllocateMemoryDesc` (verbatim)

```c
// Memory requirements for a resource (buffer or texture)
NriStruct(MemoryDesc) {
    uint64_t size;
    uint32_t alignment;
    Nri(MemoryType) type;
    bool mustBeDedicated; // must be put into a dedicated "Memory" object, containing only 1 object with offset = 0
};

// A group of non-dedicated "MemoryDesc"s of the SAME "MemoryType" can be merged into a single memory allocation
NriStruct(AllocateMemoryDesc) {
    uint64_t size;
    Nri(MemoryType) type;
    float priority; // [-1; 1]: low < 0, normal = 0, high > 0
    struct {
        bool enable;
        NriOptional uint32_t alignment; // by default worst-case alignment applied
    } vma;
    bool allowMultisampleTextures;
};
```
(`Include/NRIDescs.h:724-753`)

### ResourceAllocator extension = AMD VMA (VK) / D3D12MA (D3D12), vendored, not a separate NRI extension header

- No `NRIResourceAllocator.h` — VMA/D3D12MA integration is baked directly into the VK/D3D12 backends via the `vma.enable` flag on `AllocateMemoryDesc`, and unconditionally for `CreatePlaced*` with the `NriXxxHeap` macros.
- VK: `Source/VK/MemoryAllocatorVK.h` — single-file wrapper that `#include "vk_mem_alloc.h"` with `VMA_IMPLEMENTATION` (dynamic Vulkan function loading, `VMA_DYNAMIC_VULKAN_FUNCTIONS 1`).
- D3D12: `Source/D3D12/MemoryAllocatorD3D12.h` — `#include "D3D12MemAlloc.cpp"` (AMD D3D12 Memory Allocator).
- Actual VMA call site: `Source/VK/MemoryVK.hpp:63-70` (`MemoryVK::Create(AllocateMemoryDesc)`):
  ```cpp
  VmaAllocationCreateInfo allocationCreateInfo = {};
  allocationCreateInfo.flags = VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT | VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT;
  allocationCreateInfo.flags |= IsHostVisibleMemory(memoryTypeInfo.location) ? VMA_ALLOCATION_CREATE_MAPPED_BIT : 0;
  allocationCreateInfo.memoryTypeBits = 1u << memoryTypeInfo.index;
  allocationCreateInfo.priority = m_Priority;
  VkResult vkResult = vmaAllocateMemory(m_Device.GetVma(), &memoryRequirements, &allocationCreateInfo, &m_VmaAllocation, nullptr);
  ```
  Note `VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT` is **always set** when `vma.enable=true` — every VMA-managed `Memory` allocation supports aliasing by default.

### Dedicated allocations & aliasing — both exposed, this is the frame-graph target seam

- **Aliasing**: `BindBufferMemoryDesc`/`BindTextureMemoryDesc` docs state explicitly: *"Binding resources to a memory (**resources can overlap, i.e. alias**)"* (`Include/NRIDescs.h:755`). Any number of `Bind*Memory` calls against the same `Memory` object at overlapping offsets is legal — this is the raw primitive a transient-resource/frame-graph allocator would build on. `DeviceDesc::memory.bufferTextureGranularity` (`NRIDescs.h:1849`) reports the required placement granularity between adjacent linear/non-linear (buffer/texture) resources to avoid aliasing hazards when packing manually.
- **Dedicated allocation** is forced (`MemoryDesc::mustBeDedicated=true`) when:
  - **D3D12**: `Source/D3D12/DeviceD3D12.hpp:1272-1298` — only when `tiers.memory == 0` (Resource Heap Tier 1) **and** the resource is a render-target/depth-stencil texture (`D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|ALLOW_DEPTH_STENCIL`). On Tier 2+ hardware (virtually all modern discrete GPUs) nothing forces dedication.
  - **VK**: `Source/VK/DeviceVK.hpp:1471-1476` — driven by `VkMemoryDedicatedRequirements::requiresDedicatedAllocation` queried per-resource from the driver (i.e., whatever the ICD reports, typically MSAA/large RT attachments on some HW).
  - The app must still call `AllocateMemory` even for `mustBeDedicated` resources (comment at `NRI.h:113-115`); `MemoryVK::Create` short-circuits to `Result::SUCCESS` without allocating anything when dedicated, and the real VK allocation happens later in `MemoryVK::CreateDedicated(buffer/texture)` (`Source/VK/MemoryVK.hpp:102-144`), which uses `VkMemoryDedicatedAllocateInfo`.
- **`GetXxxMemoryDesc2` / `CreateCommittedX` / `CreatePlacedX`** are the "D3D12-style" convenience path (`NRI.h:125-133`) — closer to what an NVRHI-derived engine already does (`CreateCommittedResource`-shaped).

### Budget queries

- `HelperInterface::QueryVideoMemoryInfo(device, MemoryLocation) -> VideoMemoryInfo{budgetSize, usageSize}` (`Include/Extensions/NRIHelper.h:11-14, 79`) — lives in the **Helper** extension, not Core. Backed by `VK_EXT_memory_budget` on Vulkan (listed as a supported extension in `README.md:173`) and the DXGI/D3D12 budget query on D3D12.

---

## 2. Resource lifetime

**No refcounting anywhere in the API.** `Include/NRI.h` header comment is explicit about the ownership model (`NRI.h:33-36`):
```
Implicit:
 - Create*         - thread safe
 - Destroy*        - not thread safe (because of VK)
 - Cmd*            - not thread safe
```
There is no `AddRef`/`Release` on any `NriForwardStruct` entity (`Fence, Queue, Memory, Buffer, Device, Texture, Pipeline, SwapChain, QueryPool, Descriptor, CommandBuffer, DescriptorSet, DescriptorPool, PipelineLayout, PipelineCache, CommandAllocator` — `NRI.h:42-57`). `Destroy*` calls are pass-through — `Source/Validation/DeviceVal.hpp:683-711` shows the (optional) validation layer's `DestroyBuffer`/`DestroyTexture`/`DestroyDescriptor`/`DestroyPipeline*` etc. simply forward to the backend `Destroy*` immediately; there is no ref count, no queued/deferred free, no "is this still referenced by a live descriptor/command buffer" check.

### The rule: **you must guarantee the GPU is not using it** (identical burden to raw D3D12/VK)

- No explicit lifetime-contract prose exists in the headers beyond `DeviceWaitIdle`/`QueueWaitIdle` being offered as the tool to make this safe (`NRI.h:244-245`). This is a **strictly lower-level contract than NVRHI**, which defers destruction internally via its refcounted handles.
- The **NRI validation layer catches misuse only at the host-bookkeeping level, not GPU-timeline level**:
  - `MemoryVal::HasBoundResources()` / `ReportBoundResources()` (`Source/Validation/MemoryVal.hpp:5-33`) — `FreeMemory` on a `Memory` still carrying bound buffers/textures/AS/micromaps is rejected with `NRI_REPORT_ERROR(..., "some resources are still bound to the memory")` (`Source/Validation/DeviceVal.hpp:1055-1069`). This is a **host-side "did you unbind first" check**, not a "did the GPU finish" check.
  - `BufferVal::Map`/`Unmap` (`Source/Validation/BufferVal.hpp:8-27`) rejects double-map, unbound-buffer map, and out-of-bounds map — again host state only.
  - Grep across `Source/Validation/*` for "in flight" / "still in use" / "GPU is" / "before destroy" turned up **zero matches** — confirmed there is no GPU-fence-aware destroy-time check anywhere in NRI, including the validation backend. Catching a premature destroy-while-in-flight is entirely delegated to the underlying GAPI validation layer (`enableGraphicsAPIValidation` in `DeviceCreationDesc`, `Include/Extensions/NRIDeviceCreation.h:75`) — D3D12 Debug Layer / VK Validation Layers, not NRI's own layer.

### Deferred-destruction facility: exists, but only inside `Streamer` (not general-purpose)

`StreamerImpl` is the **one** place in NRI with a queued-frame garbage list:
```cpp
struct GarbageInFlight {
    Buffer* buffer;
    uint32_t frameNum;
};
```
(`Source/Shared/StreamerInterface.h:22-25`). `StreamerImpl::Grow()` (`Source/Shared/StreamerInterface.hpp:13-31`) pushes the old dynamic buffer onto `m_GarbageInFlight` before replacing it with a bigger one; `StreamerImpl::EndFrame()` (`.hpp:210-231`) increments each entry's `frameNum` and only calls `m_iCore.DestroyBuffer(...)` once `frameNum >= queuedFrameNum`. **This pattern is entirely internal to the Streamer extension** — there is no general "deferred destroy queue" object exposed for arbitrary `Buffer`/`Texture`/`Descriptor` in Core. An engine wanting NVRHI-style automatic deferred destruction has to build it itself (typically: a per-`queuedFrameNum` garbage ring keyed on the same fence value pattern NRI itself uses for `QueueSubmitDesc::signalFences`/`Wait`).

**Migration implication**: this is the single biggest structural gap vs. NVRHI. An NVRHI→NRI port needs an explicit resource-graveyard layer (fence-value-tagged, drained on `Wait`) wrapped around `Destroy*`/`FreeMemory` before any code that currently relies on NVRHI's automatic refcounted teardown can be ported as-is.

---

## 3. Descriptor model end-to-end

### Entities

`Descriptor` (a view or sampler — `NRI.h:51`), `DescriptorPool` (aka descriptor heap — `NRI.h:54`), `DescriptorSet` (aka `VkDescriptorSet`; **D3D12 has no native equivalent**, NRI's D3D12 backend emulates it — `NRI.h:53`, `README.md:216`), `PipelineLayout` (aka root signature — `NRI.h:55`).

### `DescriptorRangeDesc` / `DescriptorSetDesc` / `PipelineLayoutDesc` (verbatim, `Include/NRIDescs.h:991-1060`)

```c
// "DescriptorRange" consists of "Descriptor" entities
NriStruct(DescriptorRangeDesc) {
    uint32_t baseRegisterIndex;         // "VKBindingOffsets" not applied to "MUTABLE" and "INPUT_ATTACHMENT" to avoid confusion
    uint32_t descriptorNum;             // treated as max size if "VARIABLE_SIZED_ARRAY" flag is set
    Nri(DescriptorType) descriptorType;
    Nri(StageBits) shaderStages;
    Nri(DescriptorRangeBits) flags;
};

// "DescriptorSet" consists of "DescriptorRange" entities
NriStruct(DescriptorSetDesc) {
    uint32_t registerSpace;             // must be unique, avoid big gaps
    const NriPtr(DescriptorRangeDesc) ranges;
    uint32_t rangeNum;
    Nri(DescriptorSetBits) flags;
};

// "PipelineLayout" consists of "DescriptorSet" descriptions and root parameters
NriStruct(PipelineLayoutDesc) {
    uint32_t rootRegisterSpace;         // must be unique, avoid big gaps
    const NriPtr(RootConstantDesc) rootConstants;
    uint32_t rootConstantNum;
    const NriPtr(RootDescriptorDesc) rootDescriptors;
    uint32_t rootDescriptorNum;
    const NriPtr(RootSamplerDesc) rootSamplers;
    uint32_t rootSamplerNum;
    const NriPtr(DescriptorSetDesc) descriptorSets;
    uint32_t descriptorSetNum;
    Nri(StageBits) shaderStages;
    Nri(PipelineLayoutBits) flags;
};
```
`registerSpace` on `DescriptorSetDesc` = HLSL `space<N>` / VK descriptor-set index; `setIndex` (used in `SetDescriptorSetDesc`/`AllocateDescriptorSets`) is the **local index in the pipeline layout's `descriptorSets` array**, not the register space value — the doc block right above `PipelineLayoutDesc` (`NRIDescs.h:1031-1047`) spells out that all indices (`rootConstantIndex`, `rootDescriptorIndex`, `setIndex`, `rangeIndex`, `descriptorIndex`) are **local to the currently bound pipeline layout**, decoupled from the actual HLSL register/space numbers baked into `baseRegisterIndex`/`registerSpace`.

### Allocation / update / copy mechanics

- `AllocateDescriptorSets(pool, pipelineLayout, setIndex, out sets[], instanceNum, variableDescriptorNum)` (`NRI.h:144`) — allocates `instanceNum` sets matching `pipelineLayout`'s set at `setIndex`; `variableDescriptorNum` only matters if that set has a `VARIABLE_SIZED_ARRAY` range.
- `UpdateDescriptorRanges(UpdateDescriptorRangeDesc[])` — writes descriptors into a range at `baseDescriptor` offset; **all descriptors in one call must share the same `DescriptorType`** (`NRIDescs.h:1101` comment).
- `CopyDescriptorRanges(CopyDescriptorRangeDesc[])` — set-to-set copy, `descriptorNum` can be `ALL` (source-driven).
- `ResetDescriptorPool` wipes the whole pool and every set allocated from it; sets themselves **don't need individual destruction** — `DescriptorSet` is documented as `<=48 bytes` so apps are encouraged to pre-allocate many and reuse rather than reset per-frame (`NRI.h:135-138`). This is the closest analog to NVRHI's "cached binding sets keyed on stable handles" — the set pointer itself is stable across frames as long as the pool isn't reset; only its *contents* (via `UpdateDescriptorRanges`) or its *binding* need to change.
- `ALLOW_UPDATE_AFTER_SET` (on both `DescriptorPoolBits` and `DescriptorSetBits`, gating `DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET`) lets a set be updated after `CmdSetDescriptorSet` but before `QueueSubmit` — comment: *"also works as DATA_VOLATILE"* (`NRIDescs.h:961`). WGPU is explicitly called out as **not supporting true update-after-bind** (bind groups are immutable there; "update + rebind" works but previously-recorded command buffers can't be patched).

### Register spaces / sets — the actual mapping story

- HLSL `register(t/u/b/s, spaceN)` ↔ `DescriptorRangeDesc::baseRegisterIndex` (the `t`/`u`/`b`/`s` number) + `DescriptorSetDesc::registerSpace` (the `spaceN`). `DescriptorType` implicitly tells you which HLSL register letter is expected (table at `NRIDescs.h:964-989`, e.g. `TEXTURE → t`, `STORAGE_TEXTURE → u`, `CONSTANT_BUFFER → b`, `SAMPLER → s`).
- Root constants/descriptors/samplers live in an implicit **root set** at `rootRegisterSpace` (`PipelineLayoutDesc::rootRegisterSpace`), separate from the numbered `descriptorSets`.

### VK register-shift machinery — direct analog of NVRHI's `VulkanBindingOffsets{t,s,b,u}`

`DeviceCreationDesc::vkBindingOffsets` (type `VKBindingOffsets{sRegister,tRegister,bRegister,uRegister}`, `Include/Extensions/NRIDeviceCreation.h:33-38, 70`) is a **device-global** default; comment recommends *"Use largest offset for the resource type planned to be used as an unbounded array"* — i.e. same rationale NVRHI documents for its shift constants (avoid overlap with bindless ranges). Applied per-`PipelineLayout` in `Source/VK/PipelineLayoutVK.hpp:17-35`:
```cpp
bool ignoreGlobalSPIRVOffsets = (pipelineLayoutDesc.flags & PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS) != 0;
VKBindingOffsets vkBindingOffsets = {};
if (!ignoreGlobalSPIRVOffsets)
    vkBindingOffsets = m_Device.GetBindingOffsets();

std::array<uint32_t, (size_t)DescriptorType::MAX_NUM> bindingOffsets = {};
bindingOffsets[(size_t)DescriptorType::SAMPLER]                   = vkBindingOffsets.sRegister;
bindingOffsets[(size_t)DescriptorType::TEXTURE]                   = vkBindingOffsets.tRegister;
bindingOffsets[(size_t)DescriptorType::STORAGE_TEXTURE]           = vkBindingOffsets.uRegister;
bindingOffsets[(size_t)DescriptorType::BUFFER]                    = vkBindingOffsets.tRegister;
bindingOffsets[(size_t)DescriptorType::STORAGE_BUFFER]            = vkBindingOffsets.uRegister;
bindingOffsets[(size_t)DescriptorType::CONSTANT_BUFFER]           = vkBindingOffsets.bRegister;
bindingOffsets[(size_t)DescriptorType::STRUCTURED_BUFFER]         = vkBindingOffsets.tRegister;
bindingOffsets[(size_t)DescriptorType::STORAGE_STRUCTURED_BUFFER] = vkBindingOffsets.uRegister;
bindingOffsets[(size_t)DescriptorType::ACCELERATION_STRUCTURE]    = vkBindingOffsets.tRegister;
...
ranges[j].baseRegisterIndex += bindingOffsets[(uint32_t)descriptorSetDesc.ranges[j].descriptorType];   // line 67
```
i.e. the HLSL `t<N>` register becomes VK binding `N + tRegister`, exactly the shift NVRHI applies. **Per-pipeline-layout opt-out** exists via `PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS` (`NRIDescs.h:932`), which NVRHI's single global `VulkanBindingOffsets` does not offer. One documented carve-out: shift is **not applied to `MUTABLE` and `INPUT_ATTACHMENT`** descriptor types (`NRIDescs.h:993` comment) — worth flagging since a material pipeline mixing bindless `MUTABLE` ranges with regular `t`/`u`/`b` ranges needs two different shift mental models.

### Root constants / root descriptors (Push constants / push descriptors)

```c
NriStruct(RootConstantDesc) {   uint32_t registerIndex; uint32_t size; Nri(StageBits) shaderStages; };          // aka push constants block
NriStruct(RootDescriptorDesc) { uint32_t registerIndex; Nri(DescriptorType) descriptorType; Nri(StageBits) shaderStages; }; // aka push descriptor
NriStruct(RootSamplerDesc)    { uint32_t registerIndex; Nri(SamplerDesc) desc; Nri(StageBits) shaderStages; };   // aka static/immutable sampler
```
(`NRIDescs.h:1009-1026`)
- Bound via `CmdSetRootConstants(SetRootConstantsDesc{rootConstantIndex, data, size, offset})` and `CmdSetRootDescriptor(SetRootDescriptorDesc{rootDescriptorIndex, descriptor, offset})` (`NRI.h:159-160`, descs at `NRIDescs.h:1125-1138`). `SetRootDescriptorDesc::offset` is the mechanism used for **per-draw dynamic-offset constant buffers** (see §6 — this is the actual volatile-CB bind point).
- VK backend: root descriptors map onto `VK_KHR_push_descriptor` (comment "aka push descriptor"); root constants map onto `VkPushConstantRange` (`Source/VK/PipelineLayoutVK.hpp:71-87`, accumulated byte offsets per shader stage).
- D3D12 backend: straight root-signature mapping — `SetGraphicsRoot32BitConstants` / root CBV/SRV/UAV (`Source/D3D12/CommandBufferD3D12.hpp:759-782` shows root-constant calls for the built-in draw-parameter/draw-index emulation).
- Root descriptors/constants are **cheap but capacity-limited** — `DeviceDesc::pipelineLayout{descriptorSetMaxNum, rootConstantMaxSize, rootDescriptorMaxNum}` (`NRIDescs.h:1869-1873`) and the D3D12 256-byte root-signature budget is modeled explicitly in `HelperInterface::FitPipelineLayoutSettingsIntoDeviceLimits` (`Include/Extensions/NRIHelper.h:129-198`, `descriptorTableCost=4`, `rootDescriptorCost=8` bytes accounting).

### BINDLESS — two distinct mechanisms, both exposed

**(A) Variable-sized array in a normal descriptor set** (unbound-array style, `tiers.bindless >= 1`):
`DescriptorRangeBits::VARIABLE_SIZED_ARRAY` (size given at `AllocateDescriptorSets(..., variableDescriptorNum)`), combined with `PARTIALLY_BOUND` (not all slots need valid descriptors) — used in `NRISamples/Source/BindlessSceneViewer.cpp:237`:
```cpp
textureDescriptorRange[0] = {0, 128, nri::DescriptorType::TEXTURE, nri::StageBits::FRAGMENT_SHADER,
    nri::DescriptorRangeBits::VARIABLE_SIZED_ARRAY | nri::DescriptorRangeBits::PARTIALLY_BOUND};
```

**(B) Directly-indexed descriptor heaps** (`tiers.bindless >= 2`, SM 6.6+ `ResourceDescriptorHeap[]`/`SamplerDescriptorHeap[]`) — `NRISamples/Source/DescriptorHeapIndexing.cpp` is the canonical sample:
```cpp
const nri::DeviceDesc& deviceDesc = NRI.GetDeviceDesc(*m_Device);
if (!deviceDesc.tiers.bindless) { printf("Bindless is not supported!\n"); exit(0); }
...
nri::DescriptorRangeDesc heaps[2] = {
    { 0 /*VK binding for -fvk-bind-resource-heap*/, RESOURCE_NUM, nri::DescriptorType::MUTABLE,
      nri::StageBits::COMPUTE_SHADER, nri::DescriptorRangeBits::ARRAY | nri::DescriptorRangeBits::PARTIALLY_BOUND },
    { 1 /*VK binding for -fvk-bind-sampler-heap*/, 2, nri::DescriptorType::SAMPLER,
      nri::StageBits::COMPUTE_SHADER, nri::DescriptorRangeBits::ARRAY | nri::DescriptorRangeBits::PARTIALLY_BOUND },
};
nri::DescriptorSetDesc descriptorSetDesc = {0, heaps, helper::GetCountOf(heaps)};
nri::PipelineLayoutDesc pipelineLayoutDesc = {};
pipelineLayoutDesc.descriptorSetNum = 1;
pipelineLayoutDesc.descriptorSets = &descriptorSetDesc;
pipelineLayoutDesc.shaderStages = nri::StageBits::COMPUTE_SHADER;
pipelineLayoutDesc.flags = nri::PipelineLayoutBits::RESOURCE_HEAP_DIRECTLY_INDEXED | nri::PipelineLayoutBits::SAMPLER_HEAP_DIRECTLY_INDEXED;
```
(`DescriptorHeapIndexing.cpp:259-286`). `DescriptorType::MUTABLE` is the "any non-sampler resource, mutates on write" catch-all descriptor type used for the resource heap (requires `features.mutableDescriptorType`, backed by `VK_EXT_mutable_descriptor_type` — `NRIDescs.h:971-975`, `README.md:164`). The set is allocated first from the pool so `GetDescriptorSetOffsets` returns `{0,0}` (`DescriptorHeapIndexing.cpp:308-316` — this offset is what a shader needs added to a heap index if the set is **not** the first allocated). Shader side uses native SM6.6 syntax directly, no NRI macro needed:
```hlsl
RWTexture2D<float4> dest = ResourceDescriptorHeap[0];
ConstantBuffer<Constants> constants = ResourceDescriptorHeap[1];
Texture2D<float4> src0 = ResourceDescriptorHeap[2];
SamplerState sampler0 = SamplerDescriptorHeap[0];
```
(`NRISamples/Shaders/DescriptorHeapIndexing.cs.hlsl:17-23`, gated `#if (NRI_SHADER_MODEL >= 66)`).

`tiers.bindless` (`NRIDescs.h:2065-2067`): `1` = unbound arrays with dynamic indexing (mechanism A), `2` = D3D12 dynamic resources / directly-indexed heaps (mechanism B).

---

## 4. Pipelines

### Full state coverage — `GraphicsPipelineDesc` (verbatim, `Include/NRIDescs.h:1519-1531`)

```c
NriStruct(GraphicsPipelineDesc) {
    const NriPtr(PipelineLayout) pipelineLayout;
    NriOptional const NriPtr(VertexInputDesc) vertexInput;
    Nri(InputAssemblyDesc) inputAssembly;
    Nri(RasterizationDesc) rasterization;
    NriOptional const NriPtr(MultisampleDesc) multisample;
    Nri(OutputMergerDesc) outputMerger;
    const NriPtr(ShaderDesc) shaders;
    uint32_t shaderNum;
    Nri(GraphicsPipelineBits) flags;
    Nri(Robustness) robustness;
    NriOptional const NriPtr(PipelineCache) cache;
};
```
Sub-blocks: `InputAssemblyDesc{topology, tessControlPointNum, primitiveRestart}`; `RasterizationDesc{depthBias, fillMode, cullMode, frontCounterClockwise, depthClamp, lineSmoothing, conservativeRaster, shadingRate}`; `MultisampleDesc{sampleMask, sampleNum, alphaToCoverage, sampleLocations}`; `OutputMergerDesc{colors[], colorNum, depth: DepthAttachmentDesc, stencil: StencilAttachmentDesc, depthStencilFormat, logicOp, viewMask, multiview}` (`NRIDescs.h:1181-1481`). Blend state is per-color-attachment (`ColorAttachmentDesc{format, colorBlend, alphaBlend, colorWriteMask, blendEnabled}` — note **format is baked into the pipeline per attachment**, since there's no framebuffer object to source it from). `ComputePipelineDesc` (`NRIDescs.h:1533-1539`) is the same shape minus the graphics-only blocks.

### No framebuffer/render-pass objects — confirmed

The entity list in `NRI.h:42-57` has no `Framebuffer` or `RenderPass` forward-declared type. Rendering uses **dynamic rendering** exclusively at the API surface: `CmdBeginRendering(RenderingDesc{colors: AttachmentDesc[], colorNum, depth, stencil, shadingRate, viewMask})` where `AttachmentDesc{descriptor: Descriptor*, clearValue, loadOp, storeOp, resolveOp, resolveDst}` takes **views directly** (`NRIDescs.h:1572-1592`, `NRI.h:185`). Comment at `NRIDescs.h:1581-1584` clarifies the VK fallback path: *if `VK_KHR_dynamic_rendering` is unsupported, `VkRenderPass` is used internally by the backend*, but this is entirely hidden — the app-facing API shape never changes; input-attachment indices are inferred automatically from `Layout::INPUT_ATTACHMENT` transitions recorded earlier in the same command buffer.

### Dynamic state — viewport/scissor always dynamic, confirmed

`Viewport`/`Rect` (scissor) never appear inside `GraphicsPipelineDesc`. `CoreInterface::CmdSetViewports`/`CmdSetScissors` are listed under "Initial state (**mandatory**)" (`NRI.h:172-174`) — i.e. every command buffer must call these regardless of pipeline, matching D3D12/VK's baseline dynamic-state-always model. Additional *optional* dynamic state calls exist for stencil-ref, depth-bounds, blend-constants, sample-locations, shading-rate, depth-bias (`NRI.h:176-182`), each gated by a `features.*` flag.

### Pipeline caches

`PipelineCache` entity + `PipelineCacheDesc{data, size}` (blob-seeded), `CreatePipelineCache`, `GetPipelineCacheData` (2-call size-then-fill pattern, `NRI.h:266`), and both `GraphicsPipelineDesc`/`ComputePipelineDesc` take an optional `cache` pointer that both **serves** cached PSOs and **populates** on miss. `GraphicsPipelineBits::FAIL_ON_CACHE_MISS` / `ComputePipelineBits::FAIL_ON_CACHE_MISS` (`NRIDescs.h:1496-1504`) force `CreateGraphicsPipeline`/`CreateComputePipeline` to return `FAILURE` instead of compiling, for platforms that prohibit runtime PSO compilation (Xbox GDK is called out by name, `NRIDescs.h:2114`) — requires `features.pipelineCacheControl`. `features.pipelineCache` reports whether the cache mechanism is a real persistent cache vs. a NOP fallback (`NRIDescs.h:2113`).

### Shader bytecode expectations — no built-in reflection

`ShaderDesc{stage, bytecode, size, entryPointName}` (`NRIDescs.h:1512-1517`) — a raw blob + entry point string, nothing else. NRI does **not** parse/reflect shader bytecode; all binding info (which register/space/type each resource occupies) must be supplied by the app via `PipelineLayoutDesc`/`DescriptorRangeDesc`, matching what the shader compiler emitted. Accepted bytecode formats are capability flags: `features.shaderBytecodeDXBC/DXIL/SPIRV/WGSL` (`NRIDescs.h:2093-2096`) — DXBC for legacy D3D11/D3D12, DXIL for modern D3D12 (SM6+), SPIRV for VK (also usable on WGPU, "expects Vulkan 1.2 environment"), WGSL for native WebGPU. Compilation itself is entirely external to NRI (DXC/FXC/whatever the engine's shader pipeline already produces) — `Include/NRI.hlsl` only supplies binding-declaration macros (§5), not a compiler.

---

## 5. Shaders dir (`Include/NRI.hlsl`)

Single root-level helper header, no `Shaders/` directory in the main NRI repo (that only exists for the Imgui/NIS extensions' own shaders — `NRI/Shaders/Imgui.*.hlsl`, `NIS.*.hlsl`). `NRI.hlsl` establishes:

- **Resource declaration macro**, GAPI-branching (SPIRV/DXIL/DXBC/C), so one HLSL source compiles to all backends:
  ```c
  NRI_RESOURCE(Texture2D<float4>, gTexture, t, 0, 0);               // register(t0, space0)
  NRI_RESOURCE(Texture2D<float3>, gInputs[], t, 0, 0);               // DXIL/SPIRV unbounded array
  NRI_RESOURCE(RWTexture2D<float4>, gStorageTexture, u, 0, 1);       // register(u0, space1)
  NRI_RESOURCE(cbuffer, Constants, b, 0, 3) { uint32_t gConst1; ... };
  ```
  SPIRV expansion additionally emits `[[vk::binding(bindingIndex, setIndex)]]` (`NRI.hlsl:189-194`).
- **`NRI_ROOT_CONSTANTS(structName, name, bindingIndex, setIndex)`** — maps to `[[vk::push_constant]]` on SPIRV, `ConstantBuffer<T> : register(bN, spaceM)` on DXIL, a numbered `cbuffer` on DXBC (`NRI.hlsl:196-198, 233-234, 320-323`).
- **`NRI_FORMAT("unknown")`** — required annotation for typeless storage reads/writes (`[[vk::image_format(...)]]` on SPIRV, no-op on DXIL/DXBC), gated by `shaderFeatures.storageReadWithoutFormat`/`storageWriteWithoutFormat`.
- **`NRI_INPUT_ATTACHMENT` / `NRI_INPUT_ATTACHMENT_LOAD`** — subpass-input emulation: real `SubpassInput`+`SubpassLoad()` on SPIRV, a plain `Texture2D` load by pixel coordinate on DXIL/DXBC (no native input-attachment concept in D3D).
- **Draw-parameter emulation macros** — `NRI_VERTEX_ID`/`NRI_INSTANCE_ID`/`NRI_BASE_VERTEX`/`NRI_BASE_INSTANCE`/`NRI_DRAW_ID`, GAPI/SM-branched: native on SPIRV and DXIL SM6.8+, **emulated via a hidden root-constant block at `space999`** on DXIL below SM6.8 (`NRI_BASE_ATTRIBUTES_EMULATION_SPACE = 999`, `NRI.hlsl:228, 260-298`) — activated per-pipeline-layout via `PipelineLayoutBits::ENABLE_DRAW_PARAMETERS_EMULATION`/`ENABLE_DRAW_INDEX_EMULATION`. This is a real "gotcha" migration item: D3D12 draws below SM6.8 don't get `SV_StartVertexLocation`/`DrawIndex` natively, so NRI silently reserves an extra root constant + changes the indirect-draw command layout (`DrawBaseDesc`/`DrawIndexedBaseDesc` vs `DrawDesc`/`DrawIndexedDesc`, `NRIDescs.h:1671-1691`) whenever emulation is on.
- **`NRI_FILL_DRAW_DESC` / `NRI_FILL_DRAW_INDEXED_DESC` / `NRI_FILL_DRAW_BASE_DESC` / `NRI_FILL_DRAW_INDEXED_BASE_DESC`** — macros for a compute shader to author indirect-draw argument buffers matching NRI's exact command signature layout (`NRI.hlsl:150-181`) — needed because NRI's draw-indirect buffer format is fixed (`DrawDesc{vertexNum,instanceNum,baseVertex,baseInstance}` etc.) and isn't reflectable.
- **`NRI_UV_TO_CLIP` / `NRI_CLIP_TO_UV`** — D3D-style (Y-down) viewport UV↔clip helpers.

---

## 6. Upload paths

### 6a. Streamer extension — the NVRHI-volatile-CB replacement (`Include/Extensions/NRIStreamer.h`, impl `Source/Shared/StreamerInterface.hpp/.h`)

**`StreamerDesc` (verbatim, `NRIStreamer.h:23-32`):**
```c
NriStruct(StreamerDesc) {
    // Statically allocated ring-buffer for dynamic constants
    NriOptional Nri(MemoryLocation) constantBufferMemoryLocation; // UPLOAD or DEVICE_UPLOAD
    NriOptional uint64_t constantBufferSize;            // should be large enough to avoid overwriting data for enqueued frames

    // Dynamically (re)allocated ring-buffer for copying and rendering
    Nri(MemoryLocation) dynamicBufferMemoryLocation;    // UPLOAD or DEVICE_UPLOAD
    Nri(BufferDesc) dynamicBufferDesc;                  // "size" is ignored
    uint32_t queuedFrameNum;                            // number of frames "in-flight" (usually 1-3), adds 1 under the hood for the current "not-yet-committed" frame
};
```

**Two independent ring buffers, different growth policies:**
- **Constant ring** (`m_ConstantBuffer`) — fixed size from `constantBufferSize`, created once in `StreamerImpl::Create` (`Source/Shared/StreamerInterface.hpp:33-48`), **never resized**. `StreamConstantData(data, dataSize)` (`.hpp:50-77`) bump-allocates from `m_ConstantBufferOffset`, wrapping to `0` if the write would overflow `constantBufferSize` (**no frame-safety check on wraparound** — sizing it too small silently corrupts in-flight frames' constants; see §9). Copy is a direct `MapBuffer`/`memcpy`/`UnmapBuffer` (no fence involved — relies on the ring being large enough for `queuedFrameNum` frames' worth of live data, per the struct comment).
- **Dynamic ring** (`m_DynamicBuffer`) — starts empty, **grows on demand**: `StreamerImpl::Grow()` (`.hpp:13-31`) reallocates via `CreateCommittedBuffer` whenever `m_DynamicBufferOffset` exceeds the current per-frame slice, sized `Align(newSize, CHUNK_SIZE=65536)`; the buffer is `queuedFrameNum` slices wide (`bufferDesc.size = m_DynamicBufferSizePerFrame * m_Desc.queuedFrameNum`). The *old* buffer, if any, is pushed onto `m_GarbageInFlight` and only actually `DestroyBuffer`'d once `queuedFrameNum` `EndFrame()` calls have passed (§2's deferred-destruction facility) — this is NRI's only built-in "safe to free after N frames" mechanism.
- `StreamBufferData(StreamBufferDataDesc{dataChunks[], dataChunkNum, placementAlignment, dstBuffer?, dstOffset?})` → `BufferOffset{buffer, offset}` for direct use this frame; if `dstBuffer` is set, the request is queued into `m_BufferRequestsWithDst` for a later `CmdCopyStreamedData`. Same shape for `StreamTextureData` (row/slice-pitch aware, aligns to `deviceDesc.memoryAlignment.uploadBufferTextureRow/Slice`).
- `CmdCopyStreamedData(commandBuffer, streamer)` — **must be called once per frame inside a command buffer** to flush queued `dstBuffer`/`dstTexture` copy requests (`CmdCopyBuffer`/`CmdUploadBufferToTexture` per request, `Source/Shared/StreamerInterface.hpp:190-208`); there's a literal `// TODO:` comment (`.hpp:195`) musing whether the dynamic buffer's persistent `COPY_SOURCE` state removes the need for a barrier here — worth confirming empirically before relying on it.
- `EndStreamerFrame(streamer)` — **must be called once at the very end of each frame** (`NRIStreamer.h:77-78`); advances `m_FrameIndex`, resets `m_DynamicBufferOffset`, drains one tick of `m_GarbageInFlight`, and — notably — **drops any unprocessed buffer/texture requests that weren't flushed via `CmdCopyStreamedData` that frame** (`.hpp:224-226`, "Ignore unprocessed requests, they become invalid on the next frame").
- Thread safety: on by default (`NRI_STREAMER_THREAD_SAFE` CMake option, exclusive-lock guarded per call, `README.md:92`) — can be turned off for single-threaded record loops to shave the lock overhead.

**Canonical per-frame-constants usage** (`NRISamples/Source/InputAttachment.cpp` — the only sample that actually drives constants through the Streamer rather than a hand-rolled per-queued-frame buffer):
```cpp
// Setup (once): a fixed-size CONSTANT_BUFFER view over the streamer's static ring
bufferViewDesc.buffer = NRI.GetStreamerConstantBuffer(*m_Streamer);
bufferViewDesc.size   = helper::Align((uint32_t)sizeof(ConstantBufferLayout), deviceDesc.memoryAlignment.constantBufferOffset);
NRI.CreateBufferView(bufferViewDesc, m_Buffer_Constant);                         // InputAttachment.cpp:307-312

// Per frame (host): write new constants, get back a byte offset into the ring
m_ConstantBufferOffset = NRI.StreamConstantData(*m_Streamer, &constants, sizeof(constants));  // :370

// Per frame (record): bind via a ROOT DESCRIPTOR with a per-draw byte offset override
nri::SetRootDescriptorDesc rootDescriptorDesc = {0, m_Buffer_Constant, m_ConstantBufferOffset};
NRI.CmdSetRootDescriptor(*commandBuffer, rootDescriptorDesc);                    // :434-435
```
This is the direct structural replacement for NVRHI's automatic volatile-CB versioning: **one static-sized descriptor view + a per-draw dynamic byte offset via `CmdSetRootDescriptor`**, not a new descriptor/binding per frame. `SetRootDescriptorDesc::offset` comment: *"a non-`CONSTANT_BUFFER` descriptor requires `features.nonConstantBufferRootDescriptorOffset`"* (`NRIDescs.h:1133-1138`) — constant buffers get dynamic-offset binding for free, everything else needs a feature check (this maps onto D3D11's "unsupported" carve-outs listed at `NRIDescs.h:2134-2135`).

Contrast: most other samples (`Triangle.cpp`, `BindlessSceneViewer.cpp`) instead hand-roll a **per-queued-frame constant buffer + per-frame descriptor set** (`QueuedFrame::constantBufferView`/`constantBufferDescriptorSet`, `BindlessSceneViewer.cpp:83-99, 453-456`, direct `MapBuffer`/`UnmapBuffer` at `:542-549`) — i.e., Streamer is offered but **not mandatory**; an engine can equally replicate NVRHI-style "N buffers, one per queued frame" manually with plain `Map`/`Unmap`.

### 6b. Helper extension — `UploadData` (init-time only, explicitly NOT for streaming)

`Include/Extensions/NRIHelper.h:75` comment: *"Populate resources with data (**not for streaming!**)"*. `TextureUploadDesc{subresources?, texture, after: AccessLayoutStage, planes}` / `BufferUploadDesc{data?, buffer, after: AccessStage}` (`NRIHelper.h:16-34`) batch multiple resources through `HelperInterface::UploadData(queue, textureDescs[], textureNum, bufferDescs[], bufferNum)`.

Implementation (`Source/Shared/HelperInterface.hpp`) is **fully synchronous and blocking**:
- Sizes a single scratch upload buffer up to `MAX_UPLOAD_BUFFER_SIZE = 64 * 1024 * 1024` (64 MB, `.hpp:7`), created via `CreateCommittedBuffer(HOST_UPLOAD, ...)` (`.hpp:153`).
- Records a command buffer, `CmdCopyBuffer`/`CmdUploadBufferToTexture` per resource (batches barriers in groups of `BARRIERS_PER_PASS = 256`, `.hpp:6`), submits, then **`m_iCore.Wait(*m_Fence, m_FenceValue)` — a CPU stall** (`EndCommandBuffersAndSubmit`, `.hpp:268-292`) before looping to the next batch if data didn't fit in one 64 MB pass. Every `UploadData` call tears down its own fence/command-buffer/command-allocator/upload-buffer afterward (`.hpp:93-96`) — **not** reused across calls.
- **Migration implication**: this is a one-shot "load a texture/mesh at startup or during a loading screen" utility, not a per-frame `writeBuffer`-inside-an-open-command-list replacement — that role is Streamer's (§6a), or raw `Map`/`Unmap` (§6c).

`HelperInterface::AllocateAndBindMemory`/`CalculateAllocationNumber` (`.hpp:399-641`) is the memory-side counterpart — bin-packs a `ResourceGroupDesc` of buffers+textures into as few `Memory` allocations as possible (sorted by type then alignment, `preferredMemorySize` default 256 MB per chunk, `.hpp:511-512`), separating out anything with `mustBeDedicated=true` into its own allocation.

### 6c. Raw path — `MapBuffer`/`UnmapBuffer`

`CoreInterface::MapBuffer(buffer, offset, size)` / `UnmapBuffer(buffer)` (`NRI.h:252-257`), with an explicit per-backend persistence table in the header comment:
```
// D3D11: no persistent mapping
// D3D12: persistent mapping, "Map/Unmap" do nothing
// VK: persistent mapping, but "Unmap" can do a flush if underlying memory is not "HOST_COHERENT" (unlikely)
```
i.e. on D3D12/VK, `Map`/`Unmap` are effectively free/no-op wrappers around an already-persistently-mapped pointer (VK: mapped up-front in `MemoryVK::Create`, `Source/VK/MemoryVK.hpp:92-96` for the raw path, or `VMA_ALLOCATION_CREATE_MAPPED_BIT` for the VMA path, `MemoryVK.hpp:65`) — safe to call every frame without a real perf cost on those two backends, but **D3D11 genuinely maps/unmaps each call** (matches D3D11's discard/no-overwrite map semantics, not persistent). `BufferVal::Map`/`Unmap` (`Source/Validation/BufferVal.hpp:8-27`) additionally forbids nested maps ("D3D11 doesn't support nested calls") and requires the buffer be bound to memory first.

UPLOAD-heap access rule: any `HOST_UPLOAD`/`DEVICE_UPLOAD` buffer is mappable at any time from the host; NRI does not itself insert any GPU-side synchronization around `Map`/`Unmap` — the app is responsible for not writing into a region the GPU is currently reading (exactly the discipline the Streamer ring buffers and the per-queued-frame CB pattern exist to provide).

---

## 7. Formats / capabilities (`DeviceDesc`, `Include/NRIDescs.h:1807-2189`)

`DeviceDesc` is a single, large, queryable struct (`CoreInterface::GetDeviceDesc(device)`) — no separate "check feature X" calls, everything is a field. Grouped into: `adapterDesc` (name/uid/vram/vendor/architecture), `viewport`, `dimensions` (texture size maxes), `precision` (bit depths), `memory` (`deviceUploadHeapSize` = ReBAR size, `bufferMaxSize`, `allocationMaxSize/Num`, `bufferTextureGranularity`), `memoryAlignment` (upload row/slice, CB offset, scratch/SBT/AS/micromap offsets), `pipelineLayout`, `descriptorSet` (+ `updateAfterSet` sub-limits), `shaderStage` (per-stage descriptor maxes + vertex/tess/geometry/fragment/compute/task/mesh/rayTracing sub-limits), `accelerationStructure`, `wave` (`laneMinNum/MaxNum`, `waveOpsStages`, `quadOpsStages`, `derivativeOpsStages`), `other` (indirect-draw max, sampler LOD/aniso max, clip/cull distance max, `viewMaxNum` for multiview), `tiers` (`conservativeRaster`, `sampleLocations`, `rayTracing` 1-3 = DXR1.0/1.1/1.2, `shadingRate` 1-2, `resourceBinding` 0-2, **`bindless` 1-2**, `memory` = D3D12 Resource Heap Tier), `features` (~40 bools: swap chain caps, multiview modes, texture-compression families, `shaderBytecodeDXBC/DXIL/SPIRV/WGSL`, query types, shading-rate extras, pipeline-cache control, `enhancedBarriers`, `meshShader`, `lowLatency`, `mutableDescriptorType`, `extendedDynamicState`, `unifiedTextureLayouts`, ...), `shaderFeatures` (native `I8/I16/F16/I64/F64` types, atomics per type, storage-without-format, all 6 wave-intrinsic categories, `drawParameters`/`drawIndex` emulation flags).

**Directly answers the requested queries:**
- **Wave ops**: `wave.laneMinNum/MaxNum`, `wave.waveOpsStages`/`quadOpsStages`/`derivativeOpsStages` (which shader stages support them) + the six granular `shaderFeatures.waveQuery/waveVote/waveShuffle/waveArithmetic/waveReduction/waveQuad` booleans (`NRIDescs.h:2008-2014, 2166-2173`).
- **VRS**: `tiers.shadingRate` (0/1/2), `features.additionalShadingRates`, `features.sumShadingRateCombiner`, `other.shadingRateAttachmentTileSize` (`NRIDescs.h:2054-2057, 2105-2106, 2030`).
- **Mesh shaders**: `features.meshShader` (bool gate for the whole `NRIMeshShader` extension) + full `shaderStage.task`/`shaderStage.mesh` limit sub-structs (`NRIDescs.h:2121, 1967-1987`).
- **Ray tracing**: `tiers.rayTracing` (0-3), `shaderStage.rayTracing{shaderGroupIdentifierSize, shaderBindingTableMaxStride, recursionMaxDepth}`, `accelerationStructure{primitiveMaxNum, geometryMaxNum, instanceMaxNum, micromapSubdivisionMaxLevel}` (`NRIDescs.h:1989-2003`).
- **Bindless**: `tiers.bindless` (0/1/2, §3).

**Format capability query**: `CoreInterface::GetFormatSupport(device, Format) -> FormatSupportBits` (per-device, live HW query, `NRI.h:70`) is the authoritative source; `Format` enum's inline comment table in `NRIDescs.h:184-343` documents *expected* (not guaranteed) support bits per format as a quick static reference. `HelperInterface`'s `nriGetFormatProps(Format) -> FormatProps*` (`NRIHelper.h:46-67, 87`) gives static byte-layout metadata (bits per channel, block size, `isCompressed`/`isDepth`/`isFloat`/`isSrgb`/etc.) independent of device support — this is the CPU-side format-math table an engine would otherwise hand-roll.

**What a `RenderDeviceDesc`-shaped engine struct would likely be missing today**: `tiers.bindless` (if the engine's abstraction currently only has a single bool "supports bindless"), the `updateAfterSet` sub-limits under `descriptorSet`/`shaderStage` (separate caps from the non-UAB path — easy to miss if porting from an NVRHI model that doesn't distinguish them), and `memoryAlignment.scratchBufferOffset`/`shaderBindingTable`/`accelerationStructureOffset`/`micromapOffset` if the engine plans ray tracing later.

---

## 8. Texture upload/readback

### Row-pitch / placement rules

`TextureDataLayoutDesc{offset, rowPitch, slicePitch}` (`NRIDescs.h:1712-1716`):
- `offset` must be a multiple of `deviceDesc.memoryAlignment.uploadBufferTextureSlice`.
- `rowPitch` must be a multiple of `deviceDesc.memoryAlignment.uploadBufferTextureRow`.
- `slicePitch` must be a multiple of `deviceDesc.memoryAlignment.uploadBufferTextureSlice`.

Both `CmdUploadBufferToTexture(commandBuffer, dstTexture, dstRegion, srcBuffer, srcDataLayout)` and `CmdReadbackTextureToBuffer(commandBuffer, dstBuffer, dstDataLayout, srcTexture, srcRegion)` (`NRI.h:209-210`) take this struct — **the alignment/pitch computation is always the caller's responsibility**; NRI does not auto-pad. The Streamer (`StreamTextureData`, `Source/Shared/StreamerInterface.hpp:127-188`) and Helper (`HelperDataUpload::CopyTextureContent`, `Source/Shared/HelperInterface.hpp:294-350`) both implement the identical align-then-row-copy pattern (`Align(rowPitch, uploadBufferTextureRow)`, `Align(rowNum*alignedRowPitch, uploadBufferTextureSlice)`) as the reference implementation to copy if hand-rolling this elsewhere in engine code.

### Readback flow — matches the engine's 1-texel `PickBuffer` use case exactly

`NRISamples/Source/Readback.cpp` is a literal 1-texel-under-cursor readback sample:
```cpp
// Setup (once)
bufferDesc.size = helper::Align(4, deviceDesc.memoryAlignment.uploadBufferTextureRow);   // 1 texel, RGBA8 = 4 bytes, row-aligned
NRI.CreateBuffer(*m_Device, bufferDesc, m_ReadbackBuffer);
resourceGroupDesc.memoryLocation = nri::MemoryLocation::HOST_READBACK;
resourceGroupDesc.buffers = &m_ReadbackBuffer;
NRI.AllocateAndBindMemory(*m_Device, resourceGroupDesc, m_MemoryAllocations.data());     // :161-171

// Record (this frame): copy 1 texel from the swap chain texture under the cursor
NRI.CmdReadbackTextureToBuffer(commandBuffer, *m_ReadbackBuffer, dstDataLayoutDesc, *swapChainTexture.texture, srcRegionDesc);  // :300

// Consume (NEXT frame's PrepareFrame, after the queued-frame fence wait already happened in LatencySleep)
const uint32_t* data = (uint32_t*)NRI.MapBuffer(*m_ReadbackBuffer, 0, nri::WHOLE_SIZE);
color = *data | 0xFF000000;
NRI.UnmapBuffer(*m_ReadbackBuffer);                                                       // :189-192
```
Synchronization is entirely piggybacked on the sample's existing per-queued-frame fence wait (`NRI.Wait(*m_FrameFence, ...)` in `LatencySleep`, `Readback.cpp:181`) — there is **no dedicated readback fence**; the pattern relies on the N-queued-frame latency already guaranteeing the copy landed before the buffer is mapped again. A `PickBuffer` doing exactly this (1-texel readback per click) can port almost verbatim: single `HOST_READBACK` buffer, `Align(bytesPerTexel, uploadBufferTextureRow)` size, `CmdReadbackTextureToBuffer` this frame, `Map`/read/`Unmap` a queued-frame-count later.

---

## Surprises / concerns for the migration writeup

1. **No general deferred-destruction facility.** §2 — the only automatic "safe after N frames" teardown in all of NRI is internal to `StreamerImpl`'s dynamic-buffer regrowth (`Source/Shared/StreamerInterface.hpp:13-31, 210-231`). Every other `Destroy*`/`FreeMemory` call is immediate and unchecked against GPU-in-flight usage — porting NVRHI code that leans on refcounted automatic lifetime requires building this layer first, engine-side.
2. **`StreamConstantData`'s constant ring has no overflow protection against in-flight frames.** `StreamerInterface.hpp:59-60` wraps the constant-buffer write cursor back to `0` purely based on `constantBufferSize`, with no check against `queuedFrameNum` or a fence — the `StreamerDesc::constantBufferSize` comment (*"should be large enough to avoid overwriting data for enqueued frames"*, `NRIStreamer.h:26`) puts the entire correctness burden on the caller picking a big-enough constant size. Under-sizing it is a silent data-corruption bug, not a validated error.
3. **`CmdCopyStreamedData` has a live `// TODO` about whether the barrier it (doesn't) issue is actually safe** (`StreamerInterface.hpp:195`: *"dynamic buffer(s) is in the persistent state, including COPY_SOURCE, so there is no need to do a barrier... right? :)"*) — an NVIDIA-authored uncertainty comment in shipped v180 code; worth an empirical check (validation layer + a stress test) before depending on it in a frame-graph-heavy renderer.
4. **VK binding-shift is not applied uniformly.** `MUTABLE` and `INPUT_ATTACHMENT` descriptor types are explicitly exempted from `vkBindingOffsets` shifting (`NRIDescs.h:993` comment) — a pipeline layout mixing bindless heaps with regular typed ranges needs two different register-numbering mental models simultaneously.
5. **`HelperInterface::UploadData` is a blocking, fence-`Wait`-per-batch utility, not a streaming path** — its own header explicitly says "not for streaming!" (`NRIHelper.h:75`). Reusing it inside a hot per-frame loop (e.g. as a naive `writeBuffer` substitute) would reintroduce CPU/GPU sync stalls NVRHI's `writeBuffer`-inside-an-open-command-list avoids; Streamer (§6a) is the correct analog, Helper is init/load-screen only.
6. **D3D12 draw-parameter/draw-index emulation silently changes indirect-draw buffer layout and consumes a root constant slot** when `ENABLE_DRAW_PARAMETERS_EMULATION`/`ENABLE_DRAW_INDEX_EMULATION` are on (`NRI.hlsl:260-298`, `NRIDescs.h:1671-1691`) — needed for any D3D12 target below shader model 6.8 that wants `SV_DrawIndex`/base-vertex/base-instance semantics NVRHI-derived HLSL usually assumes are free. Budget this into the D3D12 256-byte root-signature accounting (`NRIHelper.h:151-157`).
7. **`DescriptorSet` has no native D3D12 concept** — NRI's D3D12 backend synthesizes it (`README.md:216`: `DescriptorSet | N/A | N/A | VkDescriptorSet | N/A`). Fine functionally, but means the "cached binding sets keyed on stable handles" mental model from NVRHI maps more directly to VK's native descriptor sets than to D3D12, where it's an NRI-level abstraction over root-signature-table bindings.
8. **`AllocationCallbacks::disable3rdPartyAllocationCallbacks`** (`NRIDeviceCreation.h:23`) signals NRI hands its allocator through to *vendored third-party code too* (VMA/D3D12MA, DXC if invoked, etc.) unless explicitly disabled — worth knowing if the engine has a hard "no allocations outside my tracked allocator" policy.
9. **NRISamples ships its own vendored NRI copy** at `NRISamples/External/NRIFramework/External/NRI` (also `NRI_VERSION 180`, confirmed identical version) — used all the file-path citations from the top-level `.example/NRI` checkout for source-of-truth quotes, but be aware two copies exist on disk if grepping broadly.
