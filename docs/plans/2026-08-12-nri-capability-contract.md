# NRI wrapper capability contract (v180)

**Status:** facts document — binding input to the Phase 1 device-layer task.
**Date:** 2026-08-12
**Author task:** Phase 0 / Task 7 of
`.superpowers/sdd/2026-08-12-nri-phase0-golden-harness/`
**Spec:** `docs/specs/2026-08-12-nri-adoption-design.md` § Device layer
**Sources cited:** `.example/NRI` (v180, `Include/NRI.h:41` → `NRI_VERSION 180`,
`NRI_VERSION_DATE "23 June 2026"`), `.example/NRISamples/Source/Wrapper.cpp`
(NVIDIA's own reference wrapper), and our
`ArcaneClient/src/Arcane/Render/Device{Vulkan,D3D12}.cpp`.

All NRI paths below are relative to `.example/NRI/`. All engine paths are
relative to the repo root. Line numbers are as of the on-disk clone; if the
clone is refreshed, re-verify before trusting a line number (the *function*
names are the stable anchors).

---

## 0. Why this document exists, and the one rule that generates most of it

The migration mandates the **wrapper creation path**: we create the
D3D12/VK device ourselves (keeping DRED config and the VK fault-extension
sweep) and hand it to `nriCreateDeviceFromD3D12Device` /
`nriCreateDeviceFromVKDevice`.

### THE META-RULE (Vulkan)

> In wrapper mode NRI derives its entire capability model from
> **(a) `vkGetPhysicalDeviceFeatures2` on the PHYSICAL device** and
> **(b) the extension *name lists* we hand it in the desc.**
> It never asks the logical device what was actually enabled, and it has no
> way to.

Citations:

- `Source/VK/DeviceVK.hpp:645` — `m_VK.GetPhysicalDeviceFeatures2(m_PhysicalDevice, &features);`
  runs on **both** paths (wrapper and own-device); nothing downstream of it
  distinguishes "physical device supports X" from "the app enabled X".
- `Source/VK/DeviceVK.hpp:672-695` — the whole `m_IsSupported` block is
  assigned straight off that query.
- `Source/VK/DeviceVK.hpp:104-111` — `IsExtensionSupported(const char*, Vector<const char*>&)`
  is a `strcmp` walk over the **desc-supplied list**, not a driver query.
- `Source/VK/SharedVK.h:28-31` — `PNEXTCHAIN_APPEND_FEATURES` chains a
  feature struct only `if (IsExtensionSupported(VK_<ext>_..., desiredDeviceExts) && condition)`
  — i.e. extension-gated features are keyed off our list.
- `Source/VK/DeviceVK.hpp:577-578` — `ProcessDeviceExtensions()` (the
  "filter desired against supported" step) is called **only when
  `!isWrapper`**. In wrapper mode our list is taken verbatim.

**Consequence, and the actionable form of the rule:** for every
*promoted-to-core* feature NRI reads (`VkPhysicalDeviceVulkan11/12/13/14Features`,
`VkPhysicalDeviceFeatures`), our `vkCreateDevice` must **enable everything
the physical device reports as supported** — or NRI's `nri::DeviceDesc`
over-reports capability and, for the subset NRI uses unconditionally,
generates invalid Vulkan usage on the first call.

**This is not an inference — it is literally what NVIDIA's own wrapper
sample does.** `.example/NRISamples/Source/Wrapper.cpp:340-360` queries
`VkPhysicalDeviceFeatures2` with the 1.1/1.2/1.3(/1.4) chain and then passes
**the same chain straight back** as `deviceCreateInfo.pNext` (line 355:
`deviceCreateInfo.pNext = &deviceFeatures2`). Enable-what-is-supported is
the reference implementation.

The D3D12 side has no analogue of this hazard (D3D12 has no
enable-at-create feature set — `CheckFeatureSupport` *is* the truth), so
§2's contract is about **interface versions, the Agility SDK, queues, and
what NRI deliberately does not do in wrapper mode**.

### Verdict vocabulary used in the tables

| Verdict | Meaning |
|---|---|
| **read-from-desc** | NRI takes the value from `DeviceCreation*Desc` and does not verify it. A wrong value is silently wrong. |
| **re-queried** | NRI queries the driver itself, ignoring/overwriting anything in the desc. |
| **blind** | NRI uses the capability with **no query of any kind** anywhere in its source. Must be enabled, unconditionally. |
| **assumed-from-physical** | NRI queries *physical* support and treats it as enablement (the meta-rule). Must be enabled if physically supported. |
| **degraded-path** | Genuinely optional: NRI detects absence and takes a documented slower/older path. |

---

## 1. Vulkan

Entry points: `nriCreateDeviceFromVKDevice` (`Source/Creation/Creation.cpp:995`)
→ `CreateDeviceVK` (`Source/VK/ImplVK.cpp:53`) → `DeviceVK::Create`
(`Source/VK/DeviceVK.hpp:432`). Wrapper mode is selected by
`bool isWrapper = descVK.vkDevice != nullptr;` (`DeviceVK.hpp:433`), which
also sets `m_OwnsNativeObjects = !isWrapper` (`:434`) — so NRI will **not**
destroy our `VkDevice`/`VkInstance` at `nriDestroyDevice`
(`DeviceVK.hpp:420-426`).

### 1.1 What the wrapper reads from `DeviceCreationVKDesc`

`Include/Extensions/NRIWrapperVK.h:27-43` is the struct. Field-by-field:

| Field | Treatment | Citation | Notes / failure mode if wrong |
|---|---|---|---|
| `vkDevice` | read-from-desc | `DeviceVK.hpp:433`, `:708-709` | Non-null ⇒ wrapper mode. |
| `vkInstance` | read-from-desc | `DeviceVK.hpp:461` | Not validated. All instance-level entry points are resolved against it (`:471`). |
| `vkPhysicalDevice` | read-from-desc, **required** | `Creation.cpp:1017-1018`, `DeviceVK.hpp:478` | `nullptr` ⇒ `Result::INVALID_ARGUMENT`. Every feature/property/limit query in the backend targets it. |
| `minorVersion` | **read-from-desc, never re-queried in wrapper mode** | `DeviceVK.hpp:477` (assign) vs `:504` (re-query happens only under `if (!isWrapper)`) | Header says `>= 2`. Drives (a) which promoted-feature structs get chained (`:593-598`), (b) `m_IsSupported.copyCommands2` (`:681`), (c) `ExtendedDynamicStateFeatures.extendedDynamicState = true` for `> 2` (`:669-670`), (d) **VMA's `vulkanApiVersion`** (`:208`). Too high ⇒ 1.3/1.4 structs chained on an older device, features read back as zero ⇒ the `synchronization2` hard gate fails ⇒ `Result::UNSUPPORTED`. Too low ⇒ NRI silently drops to render passes / legacy copies. |
| `vkExtensions.instanceExtensions[]` | read-from-desc | `DeviceVK.hpp:453-455`, consumed at `:471` | The **only** gate on instance-level function-pointer resolution. See §1.3. |
| `vkExtensions.deviceExtensions[]` | read-from-desc, unfiltered | `DeviceVK.hpp:573-578` | The **only** gate on device-level ext function pointers *and* on extension-feature struct chaining. See §1.4. Must be exactly what we enabled: over-report ⇒ `GET_DEVICE_FUNC` hard-fails with `UNSUPPORTED` (safe) or NRI calls an unenabled extension (unsafe); under-report ⇒ null pointers / silent degradation. |
| `vkBindingOffsets` | read-from-desc | `DeviceVK.hpp:435`; used at `PipelineLayoutVK.hpp:221` | HLSL register-space → VK binding offsets. Reference values: `{0, 128, 32, 64}` (`NRISamples/External/NRIFramework/Include/NRIFramework.h:76`). Not a device capability, but it is a required field with no default. |
| `queueFamilies[] / queueFamilyNum` | read-from-desc, **clamped against physical capacity only** | `Creation.cpp:1024-1034`, `DeviceVK.hpp:529-533`, `:758-782` | See §1.8 — this is a real trap. |
| `libraryPath` | read-from-desc, optional | `DeviceVK.hpp:444` | Defaults to `NRI_VULKAN_LOADER_NAME`. NRI loads its **own** handle to the loader and builds its **own** dispatch table (`:445`, `:1743-2007`) — independent of our `VULKAN_HPP_DEFAULT_DISPATCHER`. Leave null so both resolve the same `vulkan-1.dll`. |
| `callbackInterface` | read-from-desc | `Creation.cpp:1003`, `:1010` (`CheckAndSetDefaultCallbacks`) | Where our `CallbackInterface` → log + `RenderErrorCount` latch hooks in. |
| `enableNRIValidation` | read-from-desc | `Creation.cpp:1005`, `:606-617` | NRI's own validation layer **is** available in wrapper mode. |
| `enableMemoryZeroInitialization` | read-from-desc, AND-ed with physical support | `Creation.cpp:1006`, `DeviceVK.hpp:695` | Needs `VK_EXT_zero_initialize_device_memory` in our device-ext list to be effective; otherwise silently off. Also flips every image's `initialLayout` to `VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT` (`:1321`) — do not enable without the extension. |
| `allocationCallbacks` | read-from-desc | `Creation.cpp:1004` | Note `DeviceVK.hpp:437-438`: in wrapper mode NRI does **not** pass allocation callbacks down to Vulkan (`m_AllocationCallbackPtr` stays null) — ours would not be honored by the driver anyway. |
| *(absent: `robustness`)* | n/a | `DeviceVK.hpp:711-720` runs only under `else` (own-device) | `DeviceCreationVKDesc` has no robustness field; NRI cannot disable robustness features in wrapper mode. Robustness reporting comes from physical support (`:688-690`) and feeds `PipelineVK.hpp:62-67`. |

### 1.2 Instance extensions

NRI resolves instance-level function pointers in
`DeviceVK::ResolveInstanceDispatchTable` (`DeviceVK.hpp:1759-1809`). The
unconditional block (`:1760-1773`) is all core 1.1/1.2 — satisfied by any
1.2+ instance. The gated blocks are the contract:

| Extension | Verdict | Citation | Failure mode if absent from our list |
|---|---|---|---|
| `VK_KHR_surface` | **REQUIRED** (for any swapchain) | gate at `DeviceVK.hpp:1790-1807`; used at `SwapChainVK.hpp:80`, `:221`, and `vk.DestroySurfaceKHR` in `~SwapChainVK` | `GetPhysicalDeviceSurfaceSupportKHR`, `GetPhysicalDeviceSurfacePresentModesKHR`, `DestroySurfaceKHR`, `CreateWin32SurfaceKHR` all stay null ⇒ null-pointer call in `SwapChainVK::Create`. |
| `VK_KHR_win32_surface` | **REQUIRED** (Windows) | `DeviceVK.hpp:1795-1797`; used at `SwapChainVK.hpp:41-47` | NRI creates the surface itself from `SwapChainDesc::window.windows.hwnd`. |
| **`VK_KHR_get_surface_capabilities2`** | **REQUIRED** — easy to miss | gate at `DeviceVK.hpp:1785-1788`. **Unconditional** call sites in `SwapChainVK::Create`: `SwapChainVK.hpp:114` and `:121` (`GetPhysicalDeviceSurfaceFormats2KHR`, inside the bare `{` scope opened at `:109`) — the **first** site reached in execution order — and `:283` (`GetPhysicalDeviceSurfaceCapabilities2KHR`, bare scope opened at `:264`). *Additionally* a **conditional** site at `:103`, inside `if (allowLowLatency)` (`:94`), which is dead for us anyway (`allowLowLatency` needs `m_Desc.features.lowLatency`, i.e. `VK_NV_low_latency2` + `presentId`, `DeviceVK.hpp:1198`) | Both pointers stay null and the unconditional sites call them with no guard ⇒ **crash on first swapchain creation**. Corroborated: NVIDIA's sample lists it (`NRISamples/Source/Wrapper.cpp:259`). |
| `VK_EXT_debug_utils` | optional, safe | gate at `DeviceVK.hpp:1775-1783`; **null-guarded** at `:1642` (`SetDebugNameToTrivialObject`) and `CommandBufferVK.hpp:1604` | Absence only loses object names and command-buffer annotations. No crash. |

*(Not required: `VK_KHR_get_physical_device_properties2` — core since 1.1;
the sample only lists it on `__APPLE__`.)*

### 1.3 Device extensions

Resolved in `DeviceVK::ResolveDispatchTable` (`DeviceVK.hpp:1812-2007`).
Three macro classes matter:

- `GET_DEVICE_CORE_FUNC` (`:1716-1723`) — **hard fail** (`Result::UNSUPPORTED`)
  if the pointer does not resolve. Applied to all core 1.0/1.1/1.2 entry
  points (`:1813-1904`).
- `GET_DEVICE_OPTIONAL_CORE_FUNC` (`:1704-1714`) — tries `vkX`, then
  `vkXKHR`, then `vkXEXT`; leaves null on failure, no error.
- `GET_DEVICE_FUNC` (`:1725-1732`) — **hard fail**, but only entered when
  the extension is in our list.

| Extension | Verdict | Citation | Notes |
|---|---|---|---|
| `VK_KHR_swapchain` | **REQUIRED** for presenting | gate + hard-fail block `DeviceVK.hpp:1944-1950`; `m_Desc.features.swapChain` is a pure list check at `:1174` | Also note NRI uses `vkAcquireNextImage2KHR` (`SwapChainVK.hpp:451`), not `vkAcquireNextImageKHR`. |
| **`VK_KHR_push_descriptor`** (VK < 1.4) | **REQUIRED** for `CmdSetRootDescriptor` and root samplers | `GET_DEVICE_OPTIONAL_CORE_FUNC(CmdPushDescriptorSet)` at `DeviceVK.hpp:1932`; called **unguarded** at `CommandBufferVK.hpp:1022` (`CmdSetRootDescriptor`) and `:900` (root/immutable samplers); `VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT` at `PipelineLayoutVK.hpp:303`; `m_Desc.pipelineLayout.rootDescriptorMaxNum = props14.maxPushDescriptors` at `DeviceVK.hpp:995`, whose source struct is chained only if the ext is in our list (`:839`) | Without it: `rootDescriptorMaxNum` reports **0** and the function pointer is **null** — any pipeline layout with a root descriptor crashes. The spec's frame-graph design uses `CmdSetRootDescriptor` byte offsets for per-frame data, so this is load-bearing, not optional. Corroborated: the sample adds it for `VK_MINOR_VERSION < 4` (`Wrapper.cpp:266-267`). |
| `VK_KHR_maintenance5` (VK < 1.4) | degraded-path | `m_IsSupported.maintenance5` ← `features14.maintenance5` (`DeviceVK.hpp:673`, backfilled at `:657` only if the ext is listed); consumed at `CommandBufferVK.hpp:872-876` (`CmdBindIndexBuffer2` else `CmdBindIndexBuffer`) and as a VMA flag (`:225-226`) | Safe to omit. Sample lists it (`Wrapper.cpp:268`). |
| `VK_KHR_maintenance6` (VK < 1.4) | degraded-path | `:674`, consumed at `CommandBufferVK.hpp:928`, `:966` (`CmdBindDescriptorSets2`/`CmdPushConstants2` vs the v1 forms) | Safe to omit. Sample lists it (`Wrapper.cpp:269`). |
| `VK_KHR_synchronization2` (VK < 1.3) | **REQUIRED at VK 1.2** | hard gate at `DeviceVK.hpp:698`; feature struct chained only if listed (`:602`) | Moot for us: we run 1.3, where it is a `VkPhysicalDeviceVulkan13Features` bit instead. |
| `VK_KHR_dynamic_rendering`, `VK_KHR_copy_commands2`, `VK_KHR_maintenance4`, `VK_EXT_extended_dynamic_state` (VK < 1.3) | degraded-path at 1.2 | `:600-607` | Moot at 1.3 (core), but note `m_IsSupported.copyCommands2 = m_MinorVersion > 2 \|\| IsExtensionSupported(...)` (`:681`) — at 1.3 it is unconditionally true. |
| `VK_EXT_memory_budget` | optional, recommended | `m_IsSupported.memoryBudget` (`:685`) → `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` (`:217-218`) | Better VMA behavior under pressure. Pure list check — no feature struct. |
| `VK_KHR_swapchain_mutable_format` | optional | `:682`; consumed `SwapChainVK.hpp:365` | Needed only if we want an sRGB view over a UNORM swapchain. |
| `VK_EXT_swapchain_maintenance1` | optional | `:691`; `m_Desc.features.resizableSwapChain` (`:1177`); `SwapChainVK.hpp:276-284` | The spec already commits to destroy+recreate on resize, so omit. |
| `VK_KHR_present_id` / `VK_KHR_present_wait` | optional | `:683`; `m_Desc.features.waitableSwapChain` (`:1176`); `VkPresentIdKHR` chained at `SwapChainVK.hpp:483-490` **only if** `m_IsSupported.presentId` | **Safe by omission** precisely because the feature struct is ext-list-gated (`SharedVK.h:30`): don't list it ⇒ `presentId == false` ⇒ nothing chained into `vkQueuePresentKHR`. |
| `VK_EXT_custom_border_color` | optional | `:687`; `DeviceVK.hpp:1361-1368` | Without it, exotic border colors silently snap to the 6 standard ones. |
| `VK_EXT_mutable_descriptor_type` | optional | `m_Desc.features.mutableDescriptorType` (`:1212`); `PipelineLayoutVK.hpp:287-292` | NRI's note (`DeviceVK.hpp:1263`): needed to emulate SM 6.6 "ultimate" bindless. Not needed for the 2D port. |
| `VK_EXT_present_mode_fifo_latest_ready`, `VK_KHR_unified_image_layouts`, `VK_NV_low_latency2`, `VK_EXT_calibrated_timestamps`, `VK_KHR_acceleration_structure`/`ray_tracing_pipeline`, `VK_EXT_mesh_shader`, `VK_KHR_fragment_shading_rate`, `VK_EXT_sample_locations`, `VK_EXT_conservative_rasterization`, `VK_EXT_opacity_micromap`, `VK_EXT_memory_priority`, `VK_EXT_robustness2`, `VK_EXT_shader_atomic_float(2)`, `VK_EXT_image_sliced_view_of_3d`, `VK_KHR_shader_clock`, `VK_KHR_fragment_shader_barycentric`, `VK_EXT_fragment_shader_interlock`, `VK_KHR_maintenance7..10` | optional, safe by omission | all gated through `PNEXTCHAIN_APPEND_FEATURES`/`APPEND_EXT` on our list (`SharedVK.h:28-40`) plus `IsExtensionSupported` checks at `DeviceVK.hpp:1144-1198`, `:1941-2005` | Each simply reports `false`/tier 0. **No hard failures.** Do not add speculatively — every one added is a name NRI will then dereference function pointers for. |
| `VK_EXT_device_fault` / `VK_KHR_device_fault` / `VK_AMD_buffer_marker` (**ours**) | **inert to NRI** | Zero occurrences of any of the three names in `Source/` or `Include/` (verified by grep) | NRI never resolves, reads, or chains anything from them. Listing them in `vkExtensions.deviceExtensions` is harmless and **correct** (the list must mirror what we enabled). See §4. |

### 1.4 Promoted-to-core features (the assumed-from-physical set)

`VkPhysicalDeviceVulkan11Features` and `...Vulkan12Features` are chained
**unconditionally** (`DeviceVK.hpp:586-590`); `...Vulkan13Features` when
`minorVersion >= 3` (`:592-594`); `...Vulkan14Features` when `>= 4`
(`:596-598`). Then `:645` queries them off the physical device. Every row
below is therefore "must be enabled if the GPU supports it".

| Feature | Verdict | NRI citation | What breaks if the GPU supports it and we don't enable it |
|---|---|---|---|
| `features13.synchronization2` | **HARD REQUIRED** + used unconditionally | gate: `DeviceVK.hpp:698` (`NRI_RETURN_ON_FAILURE(... "'synchronization2' is not supported")`); use: `QueueVK.hpp:117` (`QueueSubmit2` on **every** submit), `CommandBufferVK.hpp:1556` (`CmdPipelineBarrier2` on **every** barrier), `:1572` (`CmdWriteTimestamp2`) | Note the gate tests *physical support*, so it passes trivially on 1.3 while the calls are illegal. Every submit and every barrier is invalid usage. |
| `features12.timelineSemaphore` | **BLIND** | `FenceVK.hpp:11-12` — every `nri::Fence` is a `VkSemaphore` with `VK_SEMAPHORE_TYPE_TIMELINE`. The string `timelineSemaphore` appears **nowhere** in NRI's `Source/` or `Include/` (verified by grep) | NRI never queries it. `CreateFence` is invalid on a device without it. The spec's `GpuFrameSlot` → timeline-fence move makes this the hottest path in the engine. |
| **`features12.bufferDeviceAddress`** | **assumed-from-physical — worst offender** | `m_IsSupported.deviceAddress = features12.bufferDeviceAddress` (`DeviceVK.hpp:679`) → (a) `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` (`:219-220`), (b) `GetBufferUsageFlags(..., isDeviceAddressSupported)` adds `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` to **every buffer** (`:20-24`, called from `:1284`) | **Every single `vkCreateBuffer` becomes invalid usage** (`SHADER_DEVICE_ADDRESS` usage requires the feature), and VMA will call `vkGetBufferDeviceAddress` on a device that never enabled it. This fires on the first upload ring, not in some exotic corner. |
| **`features12.hostQueryReset`** | **BLIND** | `QueryPoolVK.hpp:65-68` — `QueryPoolVK::Reset` calls `vk.ResetQueryPool` (the host-side reset, `nri::CoreInterface::ResetQueries`, `Include/NRI.h:238`) with no guard; the pointer is loaded as a hard-required core function at `DeviceVK.hpp:1901`. The string `hostQueryReset` appears **nowhere** in NRI (verified by grep) | Invalid usage the moment we call `ResetQueries` on the host (the D3D12 backend no-ops it, `Source/D3D12/ImplD3D12.cpp:525`, so a VK-only bug). Free to enable; enable it. |
| `features13.maintenance4` | assumed-from-physical | `m_IsSupported.maintenance4` (`:672`) → `VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT` (`:223-224`), `m_Desc.features.getMemoryDesc2 = true` (`:1193`), and `DeviceVK::GetMemoryDesc2` calls `vk.GetDeviceBufferMemoryRequirements` (`:1384`) / `GetDeviceImageMemoryRequirements` (`:1403`) | On a 1.3 device `features13.maintenance4` is supported, so NRI *will* take the maintenance4 path. The promoted entry points require the feature bit to be enabled (exact VUID not verified here — see §6). |
| `features12.descriptorIndexing` (+ the sub-bits we actually use) | assumed-from-physical, **conditional on use** | `m_Desc.tiers.bindless = features12.descriptorIndexing ? 1 : 0` (`:1170`); `PipelineLayoutVK.hpp:297` chains `VkDescriptorSetLayoutBindingFlagsCreateInfo` on **every** set layout when `tiers.bindless != 0`; the flags themselves come from our descs at `:224-234` (`PARTIALLY_BOUND`, `ALLOW_UPDATE_AFTER_SET`, `VARIABLE_SIZED_ARRAY`) | Chaining the struct with all-zero flags is legal, so *nothing breaks by default*. It breaks the moment we set any `DescriptorRangeBits`/`DescriptorSetBits`/`DescriptorPoolBits` update-after-bind or partially-bound flag — and `tiers.bindless == 1` is exactly what would make us reach for them. Enable `descriptorIndexing` plus `descriptorBindingPartiallyBound` / `descriptorBinding*UpdateAfterBind` / `descriptorBindingVariableDescriptorCount` / `runtimeDescriptorArray` / `shader*ArrayNonUniformIndexing` as a block. |
| `features12.drawIndirectCount` | assumed-from-physical | reported at `:1204`; `CmdDrawIndirectCount`/`CmdDrawIndexedIndirectCount` are loaded as **hard-required core** at `:1903-1904` | Reported as available; the count-buffer draw path is invalid without it. |
| `features12.samplerFilterMinmax` | assumed-from-physical | `m_Desc.features.filterOpMinMax` (`:1201`) → `DeviceVK.hpp:1343-1347` chains `VkSamplerReductionModeCreateInfo` into **every** `vkCreateSampler` when true | Benign for `WEIGHTED_AVERAGE` (the default reduction mode); invalid for MIN/MAX filter ops, which NRI now advertises. |
| `features12.shaderFloat16`, `shaderInt8`, `shaderBufferInt64Atomics`, `shaderSharedInt64Atomics`, `shaderOutputViewportIndex`, `shaderOutputLayer` | assumed-from-physical (report-honesty) | `:1216`, `:1218`, `:1223`, `:1236-1237` | These feed `nri::DeviceDesc::shaderFeatures` **and the derived `shaderModel` estimate** (`:1250-1267`). Under-enabling makes NRI advertise shader capability our device does not have. |
| `features13.robustImageAccess`, `features13.pipelineCreationCacheControl`, `features13.shaderIntegerDotProduct` | assumed-from-physical (report-honesty) | `:688`, `:1192`, `:1243` | `pipelineCreationCacheControl` gates `m_Desc.features.pipelineCacheControl`, i.e. NRI's "was this a cache miss" reporting. |
| `features11.shaderDrawParameters` | assumed-from-physical | `:1245` with NRI's own comment: *"TODO: emulation is not implemented, because >99% devices support it!"*; also `m_Desc.shaderFeatures.drawIndex` (`:1246`) | No fallback exists. Reported true from physical support; shaders using base-instance/draw-index break. |
| `features11.multiview` | assumed-from-physical | `m_Desc.other.viewMaxNum` (`:1141`), `m_Desc.features.layerBasedMultiview` (`:1178`); consumed by `CmdBeginRendering`'s `viewMask` path | Report-only for the 2D port. |
| **`VkPhysicalDeviceFeatures` (the core 1.0 block)** | assumed-from-physical | queried via `features.features` at `:645` and read at `:688` (`robustBufferAccess`), `:1179-1181` (`textureCompressionBC`/`ETC2`/`ASTC_LDR`), `:1195-1196` (`tessellationShader`, `geometryShader`), `:1202-1203` (`logicOp`, `depthBounds`), `:1209` (`pipelineStatisticsQuery`), `:1217`/`:1219`/`:1220` (`shaderInt16`/`shaderInt64`/`shaderFloat64`), `:1226-1227` (`shaderStorageImageRead/WriteWithoutFormat`), `:1266` (`shaderStorageImageMultisample`) — plus **structurally used**: `info.anisotropyEnable` from the sampler desc with no feature check (`:1333`, limit reported at `:1133`), `rasterizationState.polygonMode` from `FillMode` (`PipelineVK.hpp:192`), `depthStencilState.depthBoundsTestEnable` (`PipelineVK.hpp:232`) | This is the one that bites us hardest today because we enable **none** of it (§3). Anisotropic samplers require `samplerAnisotropy`; wireframe requires `fillModeNonSolid`; BC textures require `textureCompressionBC`; depth-bounds requires `depthBounds`; per-attachment blend requires `independentBlend`; `drawNum > 1` indirect requires `multiDrawIndirect`. |
| `features14.*` (`maintenance5/6`, `pipelineRobustness`, line rasterization, `dynamicRenderingLocalRead`) | n/a at VK 1.3 | chained only when `minorVersion >= 4` (`:596-598`); backfilled from ext structs at `:656-667` | At 1.3 these are reachable only through the corresponding KHR/EXT extensions (§1.3). |

#### The two genuine degraded paths (do not over-require)

| Feature | Citation | Degradation |
|---|---|---|
| `features13.dynamicRendering` | detect: `m_IsSupported.dynamicRendering` (`:680`); soft warning `:704-705` (`"'dynamicRendering' is not supported, using 'render passes'"`); branches at `CommandBufferVK.hpp:573`, `:627`, `:820-821`, `:1531`, `PipelineVK.hpp:296`, `:367`, `:397` | Falls back to real `VkRenderPass`/`VkFramebuffer` objects (`DeviceVK::GetOrCreateRenderPass`, `:2025`). **But**: on a 1.3 device it is physically supported ⇒ `m_IsSupported.dynamicRendering == true` ⇒ `vkCmdBeginRendering` gets called regardless of whether we enabled it. Must still be enabled. |
| `extendedDynamicState` | soft info at `:701-702`; `m_Desc.features.extendedDynamicState` (`:1213`); branch at `CommandBufferVK.hpp:861-864` | Static vertex stride + fixed viewport/scissor counts. Forced `true` for `minorVersion > 2` (`:669-670`) — correct, since VK 1.3 promoted these commands to core with no feature bit. |

### 1.5 Function pointers loaded with no extension gate (the hard-fail floor)

`ResolveDispatchTable` hard-fails device creation if any of these do not
resolve (`DeviceVK.hpp:1813-1904`). All are core ≤ 1.2, so a VK 1.2+ device
satisfies them — **but resolving a core entry point says nothing about
whether its feature bit was enabled**, which is the whole point of §1.4:

- 1.1: `BindBufferMemory2`, `BindImageMemory2`, `GetBufferMemoryRequirements2`, `GetImageMemoryRequirements2` (`:1892-1895`).
- 1.2: `CreateRenderPass2`, **`GetSemaphoreCounterValue`**, **`WaitSemaphores`** (timeline), **`ResetQueryPool`** (host query reset), **`GetBufferDeviceAddress`**, `CmdDrawIndirectCount`, `CmdDrawIndexedIndirectCount` (`:1898-1904`).

The bolded four are exactly the "blind"/"worst offender" rows above: NRI
loads them unconditionally and never checks the feature that makes calling
them legal.

Optional-core (`:1907-1939`, null-on-failure): `QueueSubmit2`,
`CmdPipelineBarrier2`, `CmdWriteTimestamp2`, the `copy_commands2` five,
`CmdBeginRendering`/`CmdEndRendering`, `CmdBindVertexBuffers2` +
`CmdSetViewportWithCount`/`CmdSetScissorWithCount`,
`GetDeviceBufferMemoryRequirements`/`GetDeviceImageMemoryRequirements`,
`CmdPushDescriptorSet`, `CmdBindIndexBuffer2`, `CmdBindDescriptorSets2`,
`CmdPushConstants2`.

### 1.6 Queue contract (a silent trap)

1. **`queueNum` is clamped against *physical family capacity*, not against
   what we created.** `Creation.cpp:1031-1033` clamps against
   `adapterDesc->queueNum[type]`, which `UpdateAdaptersVK` fills from
   `familyProps.queueCount` (`Creation.cpp:471`). NRI then calls
   `GetDeviceQueue2` for `queueIndex = 0 .. queueNum-1`
   (`DeviceVK.hpp:765-771`). If we declare more queues than we passed to
   `vkCreateDevice`, we retrieve queues that do not exist.
   **Contract: `queueFamilies[i].queueNum` must equal the `queueCount` we
   actually created for that family.**
2. `VkDeviceQueueInfo2::flags` is left 0 (`DeviceVK.hpp:766-768`) — our
   `VkDeviceQueueCreateInfo::flags` must also be 0 (it is).
3. **`nriCreateDeviceFromVKDevice` spins up a throwaway `VkInstance`.**
   `UpdateAdaptersVK` (`Creation.cpp:285`) creates its own instance at
   `apiVersion = VK_API_VERSION_1_2` (`:287-288`, `:332`) purely to match our
   physical device by LUID/UUID, then destroys it (`:489-493`). If that
   instance creation fails for any environmental reason, `adapterDesc` stays
   zeroed ⇒ `queueNum[GRAPHICS] == 0` ⇒ every declared queue is clamped to 0
   ⇒ **no queues at all**, and `DeviceVK::GetQueue` returns
   `Result::UNSUPPORTED` (`:2289-2292`) far from the cause.
   **Contract: assert `GetQueue(GRAPHICS, 0)` succeeds immediately after
   wrapping.** (Relevant to this box: see the Parsec/virtual-display GPU
   hazard.)
4. Resource sharing mode is derived from how many *distinct* families have
   been handed out via `GetQueue` (`DeviceVK.hpp:2298-2308`, consumed at
   `:1285-1287`, `:1318-1320`) — concurrent sharing kicks in automatically
   with 2+ families, matching the spec's "concurrent sharing (NRI default)".
5. Teardown: `nriDestroyDevice` does **not** destroy our device/instance
   (`DeviceVK.hpp:420-426`), but NRI's own objects (VMA allocator, render
   pass/framebuffer caches, queues) are released in `~DeviceVK`.
   **The NRI device must be destroyed before our `vkDestroyDevice`.**

---

## 2. D3D12

Entry: `nriCreateDeviceFromD3D12Device` (`Source/Creation/Creation.cpp:943`)
→ `CreateDeviceD3D12` (`Source/D3D12/ImplD3D12.cpp:52`) →
`DeviceD3D12::Create` (`Source/D3D12/DeviceD3D12.hpp:178`). Wrapper mode:
`m_IsWrapped = descD3D12.d3d12Device != nullptr` (`:179`).

### 2.1 What the wrapper reads from `DeviceCreationD3D12Desc`

`Include/Extensions/NRIWrapperD3D12.h:33-50`:

| Field | Treatment | Citation | Notes |
|---|---|---|---|
| `d3d12Device` | read-from-desc, **required** | `Creation.cpp:966-967`; `DeviceD3D12.hpp:179`, `:209` | `nullptr` ⇒ `INVALID_ARGUMENT`. |
| *(adapter)* | **re-queried** | `Creation.cpp:969-971` (`d3d12Device->GetAdapterLuid()` → `UpdateAdaptersD3D`), then `DeviceD3D12.hpp:191-199` (`CreateDXGIFactory2` + `IDXGIFactory4::EnumAdapterByLuid`) | NRI creates its own DXGI factory and re-finds our adapter by LUID. No desc field, no conflict with our adapter pick. |
| `queueFamilies[] / queueFamilyNum` | read-from-desc; queues optional | `Creation.cpp:974-984` (clamp), `DeviceD3D12.hpp:297-316` | `d3d12Queues` non-null ⇒ NRI wraps **our** `ID3D12CommandQueue`; null ⇒ **NRI creates its own** (`:309`). Clamp ceiling is a hardcoded 4 per type (`Creation.cpp:262-264`, comment: *"can't be queried in D3D"*). |
| `callbackInterface`, `allocationCallbacks`, `d3dShaderExtRegister`, `d3dZeroBufferSize`, `enableNRIValidation`, `enableMemoryZeroInitialization`, `disableD3D12EnhancedBarriers` | read-from-desc | `Creation.cpp:951-957` | The complete copy list. |
| `agsContext` | read-from-desc (AMD only) | `DeviceD3D12.hpp:206`, `:927-933` | In wrapper mode, AMD AGS is **disabled unless we supply a context** (`:930-931`). |
| `disableNVAPIInitialization` | read-from-desc (NVIDIA only) | `DeviceD3D12.hpp:204`, `:908-925` | Confusing semantics — see §2.6. |
| **`enableGraphicsAPIValidation`** | **NOT COPIED — always false in wrapper mode** | absent from `Creation.cpp:951-957`; consumed at `DeviceD3D12.hpp:185-189`, `:193`, `:257-293` | Consequences in §2.6: NRI never calls `EnableDebugLayer`, never sets break-on-severity, and never registers an `ID3D12InfoQueue1` message callback. D3D12 debug-layer output does **not** reach `CallbackInterface`. |

### 2.2 Feature levels and interface versions

| Item | Verdict | Citation | Notes |
|---|---|---|---|
| Minimum feature level | **no requirement in wrapper mode** | NRI's own path uses `D3D_FEATURE_LEVEL_11_0` (`DeviceD3D12.hpp:222`, `:236`); wrapper mode never calls `D3D12CreateDevice` | The max supported FL is queried from the device (`:597-602`) and only feeds `m_Desc`: `descriptorSet.storageBufferMaxNum` (`:676`), `tiers.bindless = 1` requires `>= 12_0` (`:821-822`), `features.filterOpMinMax` requires `>= 11_1` (`:856`). Our 12_0 device is comfortably above the floor. |
| Highest `ID3D12DeviceN` | **re-queried** | `QueryLatestInterface` (`DeviceD3D12.hpp:3-36`), stored as `m_Version` (`:254`), logged as `"Using ID3D12Device%u"` (`:255`) | `m_Version` == the interface suffix. Without `NRI_ENABLE_AGILITY_SDK_SUPPORT` the probe list stops at `ID3D12Device8` (`:6-13` are `#if`-gated), so `m_Version <= 8` and every `>= 9/10/11/15` path below is unreachable. |
| `GetVersion() >= 8` | degraded-path | `CommandBufferD3D12.hpp:395` | Independent front/back stencil ref+mask. |
| `GetVersion() >= 9` | degraded-path | `CommandBufferD3D12.hpp:426` | (Dynamic depth bias / `RSSetDepthBias` family.) |
| `GetVersion() >= 10` | required for the enhanced-barriers resource path | `BufferD3D12.hpp:42`, `:87`; `TextureD3D12.hpp:59`, `:113` (both `GetVersion() >= 10 && features.enhancedBarriers`) | `CreateCommittedResource3`/`CreatePlacedResource2` with an initial *layout*. |
| `GetVersion() >= 11` / `>= 15` | degraded-path | `DescriptorD3D12.hpp:423`, `:432-435`, `:474`, `:494`, `:522`, `:548`, `:568` | `CreateSampler2` / `TryCreateSampler2` / UINT border colors; falls back to `CreateSampler`. |
| Windows SDK floor | **compile-time hard** | `Source/D3D12/SharedD3D12.h:9` — `static_assert(D3D12_SDK_VERSION >= 3, "Outdated Windows SDK. D3D12 Ultimate needed (Windows SDK 10.0.20348). Always prefer using latest Agility SDK!")` | Build-system constraint for the premake vendoring, not a device capability. |

### 2.3 Enhanced barriers — availability, gate, and fallback

| Aspect | Fact | Citation |
|---|---|---|
| Detection | `m_Desc.features.enhancedBarriers = options12.EnhancedBarriersSupported && !disableD3D12EnhancedBarrier` | `DeviceD3D12.hpp:509` |
| **Compile-time precondition** | The `OPTIONS12` query — and every `OPTIONS9..22` query — lives inside `#if NRI_ENABLE_AGILITY_SDK_SUPPORT`. Without it, `features.enhancedBarriers` is **never assigned** and stays `false`. | `DeviceD3D12.hpp:484-586` (block boundaries), `:509` |
| Runtime opt-out | `disableD3D12EnhancedBarriers` in the desc (default `false` ⇒ enhanced barriers **on** where available) | `NRIWrapperD3D12.h:48`, `Creation.cpp:957` |
| Fallback | **Complete.** `CommandBufferD3D12::Barrier` has a full legacy `ResourceBarrier` implementation (`:1087-1163`): buffer/texture barriers mapped access→`D3D12_RESOURCE_STATES`, per-subresource expansion when the range is partial, and a single global UAV barrier synthesised when a global barrier is UAV→UAV. Resolve/`EndRendering` has a parallel legacy path (`:544-560`, `:590-670`). | `CommandBufferD3D12.hpp:981` (enhanced) vs `:1087` (legacy) |
| Degradation cost | Legacy barriers cannot express layout-only transitions, so `Layout::*` is ignored and `Access` alone drives state; partial-subresource barriers expand to `layerNum * mipNum` barriers; `D3D12_TEXTURE_BARRIER_FLAG_DISCARD` for `Layout::UNDEFINED` (`:1079`) is unavailable. | as above |
| Cross-backend note | VK always reports `m_Desc.features.enhancedBarriers = true` (`Source/VK/DeviceVK.hpp:1194`), so the frame graph must not treat this flag as "the barrier API differs" — only as "D3D12 may be coarser". | |

**Agility SDK assumptions.** NRI's CMake defaults to Agility
`1.619.3` (`CMakeLists.txt:45-47`, with the inline note that `719` hits
`D3D12_ERROR_INVALID_REDIST` without Windows developer mode) and generates
`Include/NRIAgilitySDK.h` (`CMakeLists.txt:659-674`) containing
`__declspec(dllexport) const uint32_t D3D12SDKVersion` and
`const char* D3D12SDKPath`. That header must be compiled into **one** TU of
the process for the D3D12 runtime to load the redistributable — and per
Microsoft's Agility documentation the exports are read from the process's
**main executable**, not from a DLL. Our device creation lives in
`ArcaneClient.dll` while the process is `ArcaneRuntime`/`ArcaneEditor`, so
Phase 1 must either place that TU in each host exe or drive the version
explicitly via `D3D12GetInterface(CLSID_D3D12SDKConfiguration)`.
*(Flagged as the one Agility claim resting on external documentation rather
than NRI source — see §6.)*

### 2.4 What NRI does in wrapper mode regardless of us

| Action | Citation | Interaction with our creation half |
|---|---|---|
| Creates its own `IDXGIFactory4` and re-finds our adapter by LUID | `DeviceD3D12.hpp:191-199` | Benign. Two factories in the process; ours (`IDXGIFactory6`) still owns the swapchain. |
| `InitializePixExt()` unconditionally | `:202` | Loads the WinPix event runtime if present. Harmless; annotation only. |
| `InitializeNvExt` / `InitializeAmdExt` by vendor | `:203-206`, `:908-925`, `:927-...` | See §2.6. |
| Creates a **zero buffer** (default 4 MB, `d3dZeroBufferSize`) as a committed resource | `:355-375`, `NRIConfig.h:10` | Real VRAM cost; `CmdZeroBuffer` is implemented as copies from it. |
| Creates indirect command signatures | `:383-385` | |
| Creates a D3D12MA allocator | `:388`, `:398-424` | Coexists with anything we allocate directly (e.g. the marker heap). |
| Calls `GetQueue(GRAPHICS, 0)` during `FillDesc` for the timestamp frequency | `:604-613` | If no GRAPHICS family is declared, `other.timestampFrequencyHz` is silently 0. |
| Checks `GetDeviceRemovedReason()` at the end of create | `:392-393` | Wrapping a device that is already removed fails cleanly. |

### 2.5 DRED — confirmed untouched

**Grep over `.example/NRI/Source/` and `Include/` for `DRED`,
`AutoBreadcrumb`, `DeviceRemovedExtendedData`, `PageFault` (case-insensitive)
returns zero matches.** NRI v180 contains no DRED code at all: it never
enables, configures, queries, or reads DRED. Our
`EnableD3D12Dred()` call before `D3D12CreateDevice`
(`ArcaneClient/src/Arcane/Render/DeviceD3D12.cpp:162`, implementation
`GpuCrashD3D12.cpp:643`, incl. `SetAutoBreadcrumbsEnablement` `:661`,
`SetBreadcrumbContextEnablement` `:684`, `SetPageFaultEnablement`
`:704`/`:719`) survives the migration **verbatim and unconditionally**, and
NRI cannot clobber it — it is process-global state set before device
creation, and NRI never creates the device.

### 2.6 Diagnostics-relevant asymmetries in wrapper mode

1. **D3D12 debug-layer message routing is NOT provided.** Because
   `enableGraphicsAPIValidation` is never copied into the internal desc
   (`Creation.cpp:951-957`), the entire block at `DeviceD3D12.hpp:257-293`
   is dead in wrapper mode: no `SetBreakOnSeverity`, no known-false-positive
   deny list, and no `ID3D12InfoQueue1::RegisterMessageCallback` (`:289`).
   NRI's `CallbackInterface` will carry NRI's own messages only. If we want
   D3D12 validation text in the log + `RenderErrorCount` latch (today it
   effectively goes nowhere: `DeviceD3D12.cpp:176-185` only turns
   break-on-severity **off**), Phase 1 must register our own
   `ID3D12InfoQueue1` callback. This is a diagnostics *gap*, not a conflict.
2. **NVAPI in wrapper mode reads like a bug.**
   `DeviceD3D12.hpp:916-923`: `if (isImported && !disableNVAPIInitialization)`
   ⇒ warn `"NVAPI is disabled, because it's not loaded on the application
   side"`; `else` ⇒ call `NvAPI_Initialize()`. So the *default*
   (`disableNVAPIInitialization == false`) disables NVAPI when wrapping,
   and setting the flag to `true` makes NRI call `NvAPI_Initialize` anyway.
   Practical effect for us: with NVAPI excluded from the vendoring (per the
   spec, for redistribution reasons), `NRI_ENABLE_NVAPI` is 0, `HasNvExt()`
   is false, and `m_Desc.features.lowLatency` (`:854`) plus the NV shader
   extensions (`shaderFeatures.clock`, NV-path `atomicsI64`,
   `rayTracingPositionFetch`) are simply off. No conflict with our
   diagnostics. Flagged as an ambiguity (§6).
3. **Our marker seam survives.** `nri::CoreInterface` exposes
   `GetDeviceNativeObject`, `GetQueueNativeObject`, and
   `GetCommandBufferNativeObject` (`Include/NRI.h:272-274`, documented as
   `ID3D12GraphicsCommandList*` / `VkCommandBuffer`), which is exactly what
   `GpuCrashD3D12`/`GpuCrashVulkan` need for `WriteMarker`.

---

## 3. Cross-check against our current device creation

### 3.1 Vulkan — `ArcaneClient/src/Arcane/Render/DeviceVulkan.cpp`

Baseline: instance `apiVersion = VK_API_VERSION_1_3` (`:598-600`,
`vk::createInstance` at `:609`); instance extensions `VK_KHR_surface` +
`VK_KHR_win32_surface` (`:43-46`) plus `VK_EXT_debug_utils` when validation
is on (`:589`); device extensions `VK_KHR_swapchain` (`:53-55`) plus the
crash-diagnostics sweep's additions (`:738`, `:743`, `:747`); features
enabled = `VkPhysicalDeviceVulkan13Features{synchronization2,
dynamicRendering}` (`:768-770`) chained under
`VkPhysicalDeviceVulkan12Features{timelineSemaphore}` (`:772-774`), with
`VkDeviceCreateInfo::pNext` = the 1.2 struct (`:794-799`) and
**no `pEnabledFeatures` and no `VkPhysicalDeviceFeatures2`** — so the entire
core 1.0 feature set is `VK_FALSE`. One graphics queue, `queueCount = 1`
(`:753-757`).

| Contract item | Verdict | Our file:line | Notes |
|---|---|---|---|
| `minorVersion = 3` passed to the desc | **MISSING (new field)** — and the physical-device version is never asserted | instance at `:598-600`; physical pick at `:648-657` with **no `apiVersion` check** | We already *implicitly* require VK 1.3 (chaining `VkPhysicalDeviceVulkan13Features` at `:768` fails on a 1.2 device), but nothing asserts it. Phase 1: read `physicalDevice.getProperties().apiVersion`, require ≥ 1.3, pass that minor to NRI. |
| Instance ext `VK_KHR_surface` | **ALREADY-ENABLED** | `:44` | |
| Instance ext `VK_KHR_win32_surface` | **ALREADY-ENABLED** | `:45` | |
| Instance ext `VK_KHR_get_surface_capabilities2` | **MISSING — hard break** | `:43-46` (list) | `SwapChainVK::Create` calls `GetPhysicalDeviceSurfaceFormats2KHR` unguarded at `SwapChainVK.hpp:114` (first site reached) and `GetPhysicalDeviceSurfaceCapabilities2KHR` at `:283`. Add to `kInstanceExtensions`. |
| Device ext `VK_KHR_swapchain` | **ALREADY-ENABLED** | `:54` | |
| Device ext `VK_KHR_push_descriptor` | **MISSING — hard break for root descriptors** | `:53-55` | Required by the spec's per-frame-data design (`CmdSetRootDescriptor`). |
| Device ext `VK_KHR_maintenance5` / `VK_KHR_maintenance6` | **MISSING (optional)** | `:53-55` | Safe degradation; NVIDIA's sample enables both below 1.4. Cheap to add. |
| Device ext `VK_EXT_memory_budget` | **MISSING (optional, recommended)** | `:53-55` | Pure win for VMA under memory pressure. |
| Feature `synchronization2` | **ALREADY-ENABLED** | `:769` | Our comment at `:761-763` already reasons about it for NVRHI; the reasoning transfers. |
| Feature `dynamicRendering` | **ALREADY-ENABLED** | `:770` | |
| Feature `timelineSemaphore` | **ALREADY-ENABLED** | `:773` | |
| Feature `bufferDeviceAddress` | **MISSING — hard break on the first buffer** | `:772-774` (only `timelineSemaphore` set) | See §1.4. NRI adds `SHADER_DEVICE_ADDRESS` usage to every buffer and configures VMA for it, both from *physical* support. |
| Feature `hostQueryReset` | **MISSING — hard break on `ResetQueries`** | `:772-774` | Blind in NRI; no capability bit to check. |
| Feature `maintenance4` | **MISSING** | `:768-770` (only sync2 + dynamicRendering set) | NRI/VMA take the maintenance4 path from physical support on any 1.3 device. |
| Feature block `descriptorIndexing` + sub-bits | **MISSING (conditional)** | `:772-774` | NRI already reports `tiers.bindless = 1` off physical support; enable the block before using any update-after-bind / partially-bound / variable-sized array. |
| Features `drawIndirectCount`, `samplerFilterMinmax`, `shaderFloat16`, `shaderInt8`, `shaderOutputViewportIndex`, `shaderOutputLayer`, `shaderBuffer/SharedInt64Atomics`, `robustImageAccess`, `pipelineCreationCacheControl`, `shaderIntegerDotProduct` | **MISSING (report-honesty; some conditional-on-use)** | `:768-774` | Each is read by NRI and turned into a `nri::DeviceDesc` capability or a chained create-info struct. |
| Features `shaderDrawParameters`, `multiview` (1.1) | **MISSING** | no `VkPhysicalDeviceVulkan11Features` is chained at all | NRI explicitly has no emulation for `drawParameters` (`DeviceVK.hpp:1245`). |
| **Core `VkPhysicalDeviceFeatures` block** | **MISSING ENTIRELY** | `:794-799` — no `pEnabledFeatures`, no `VkPhysicalDeviceFeatures2` in the chain | The single largest gap. `samplerAnisotropy`, `textureCompressionBC`, `fillModeNonSolid`, `independentBlend`, `depthBounds`, `logicOp`, `pipelineStatisticsQuery`, `multiDrawIndirect`, `imageCubeArray`, `shaderInt16/64`, `shaderFloat64`, `shaderStorageImageRead/WriteWithoutFormat`, `fragmentStoresAndAtomics`, `robustBufferAccess` are all `VK_FALSE` today while NRI will report the physically-supported ones as available. |
| `vkBindingOffsets` | **MISSING (new field)** | n/a | Must be chosen to match the shader-compiler's HLSL register→VK binding convention (see `ShaderConventions.hpp`); NRI's reference is `{s=0, t=128, b=32, u=64}`. Reconciling this with our existing binding layout is a Phase 1 design decision, not a device-creation flag. |
| Queue declaration (`queueNum == created count`) | **ALREADY-CORRECT, must be preserved** | `:753-757` (`queueCount = 1`), `:816` (`getQueue(family, 0)`) | Pass `queueNum = 1`, `familyIndex = m_graphicsQueueFamily`, `queueType = GRAPHICS`. |
| Assert `GetQueue(GRAPHICS,0)` after wrap | **MISSING (new)** | n/a | Guards the throwaway-instance failure mode (§1.6.3). |
| Extension list handed to NRI must mirror what we enabled | **PATTERN ALREADY EXISTS** | `:827-832` — the same discipline was needed for NVRHI (`nvrhiDesc.deviceExtensions = deviceExtensions.data()`, with the comment *"BOTH consumers of the extension list get the filtered vector"*) | Carry the *filtered* vectors (including the diagnostics extensions) into `vkExtensions`. The existing comment at `:827-830` is exactly the right rule for NRI too. |
| Destroy NRI device before `m_device.destroy()` | **ORDERING ALREADY CORRECT** | `~DeviceVulkan` at `:884-920` releases the NVRHI handles (`:912-913`) before `m_device.destroy()` (`:914-915`) | The NRI handle slots into the same position. |
| VK crash extensions (`VK_EXT/KHR_device_fault`, `VK_AMD_buffer_marker`) | **NO CONFLICT** | sweep at `:686-751`, feature chaining at `:783-792` | NRI has zero references to any of them (§1.3, last row). They must still appear in `vkExtensions.deviceExtensions` so the list mirrors reality. |

### 3.2 D3D12 — `ArcaneClient/src/Arcane/Render/DeviceD3D12.cpp`

Baseline: `IDXGIFactory6` (`:135-139`), adapter via
`EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` (`:141-146`),
`EnableD3D12Dred()` **before** create (`:162`), `D3D12CreateDevice` at
`D3D_FEATURE_LEVEL_12_0` (`:164-169`), debug layer + break-on-severity-off
when requested (`:121-133`, `:176-185`), one `D3D12_COMMAND_LIST_TYPE_DIRECT`
queue (`:187-194`), swapchain created against that queue (`:292-294`).

| Contract item | Verdict | Our file:line | Notes |
|---|---|---|---|
| `d3d12Device` non-null | **ALREADY-SATISFIED** | `:164-169` | |
| Feature level ≥ NRI's floor (11_0) | **ALREADY-SATISFIED** (12_0) | `:164` | Also clears NRI's `tiers.bindless = 1` threshold (`DeviceD3D12.hpp:821`). |
| Pass **our** `ID3D12CommandQueue` in `QueueFamilyD3D12Desc::d3d12Queues` | **MUST-DO (new)** | `:187-194` (queue), `:292-294` (swapchain bound to it) | If we leave `d3d12Queues` null, NRI creates its own queue (`DeviceD3D12.hpp:309`) and our DXGI swapchain would be presenting on a queue NRI never submits to. |
| DRED configured before create | **ALREADY-ENABLED, NO CONFLICT** | `:162` (impl `GpuCrashD3D12.cpp:643-720`) | NRI contains no DRED code whatsoever (§2.5). |
| Marker heap (`OpenExistingHeapFromAddress` + `CreatePlacedResource` with `ALLOW_CROSS_ADAPTER`) | **NO CONFLICT** | `GpuCrashD3D12.cpp:377-386` | Created off our `ID3D12Device` after wrapping; NRI's D3D12MA is a separate allocator. `GetCommandBufferNativeObject` (`Include/NRI.h:274`) preserves the write seam. |
| Agility SDK present + `D3D12SDKVersion`/`D3D12SDKPath` exported from the host exe | **MISSING** | no Agility redistributable or export TU anywhere in the tree | Without it NRI compiles with `NRI_ENABLE_AGILITY_SDK_SUPPORT = 0` ⇒ **no enhanced barriers** (`DeviceD3D12.hpp:509` never runs), `m_Version <= 8`, no `OPTIONS9..22`-derived capabilities, no `ID3D12InfoQueue1` path. Functional but permanently degraded. |
| D3D12 debug-layer messages reaching `CallbackInterface` | **MISSING (gap, not conflict)** | `:176-185` turns break-off but installs no callback | NRI will not do it in wrapper mode (§2.6.1). |
| `enableNRIValidation` | **AVAILABLE** | n/a | Copied through (`Creation.cpp:955`); the natural replacement for `nvrhi::validation::createValidationLayer` at `:207-208`. |
| Adapter selection conflict | **NONE** | `:141-146` vs `DeviceD3D12.hpp:196-198` | NRI re-derives the adapter from our device's LUID. |
| `agsContext` / NVAPI | **N/A (by design)** | n/a | Spec excludes NVAPI from vendoring; AGS needs an explicit context in wrapper mode. Both simply report "off". |

### 3.3 Score — derived by counting the rows in §3.1 and §3.2

**§5 is the canonical actionable inventory. Use that numbered list as the
checklist; the counts here are a summary of the two tables above, nothing
more.**

§3.1 has **24 rows**, §3.2 has **10**. Every breakdown below sums to its
category total.

| Category | Rows | Held / already-correct | Missing or new | Informational |
|---|---|---|---|---|
| VK instance extensions (§3.1) | 3 | 2 | 1 (hard) | — |
| VK device extensions (§3.1) | 4 | 1 | 3 = 1 hard + 2 optional | — |
| VK features (§3.1) | 10 | 3 | 7 = 2 hard + 5 report-honesty/conditional | — |
| VK structural / wiring (§3.1) | 6 | 3 | 3 | — |
| VK diagnostics interplay (§3.1) | 1 | — | — | 1 (no conflict) |
| **§3.1 total (Vulkan)** | **24** | **9** | **14** | **1** |
| **§3.2 total (D3D12)** | **10** | **3** | **2 missing + 1 must-do wiring** | **4** |

- **Verdicted-MISSING items: 16** = 14 VK + 2 D3D12. Plus the 1 D3D12
  must-do wiring row ⇒ **17 rows Phase 1 has to touch**.
- **4 of the 16 fail at runtime immediately:**
  `VK_KHR_get_surface_capabilities2`, `VK_KHR_push_descriptor`,
  `bufferDeviceAddress`, `hostQueryReset`.
- **Zero conflicts** with the crash-diagnostics arming on either backend.

**Why §5 has 15 items and not 17.** §5 item 3 folds two device-extension
gaps into one change and item 4 folds all seven VK feature gaps into one
change (query-then-pass-back), while §5 additionally carries three
preserve-this-behaviour actions and one write-the-asserts action that are
not gaps. The three views are consistent: §3.1/§3.2 count *rows of
contract*, §3.3 sums them, §5 counts *edits to make*.

**Why §1's tables have more rows than §3.1.** §1 is the complete contract
inventory, including items that are safe by omission and therefore need no
decision from us (`VK_EXT_debug_utils`, `VK_KHR_present_id`/`present_wait`,
`VK_EXT_swapchain_maintenance1`, ray tracing, mesh shading,
`VK_EXT_robustness2`, …). §3.1 carries only the rows where our creation code
has something to do or to preserve.

---

## 4. Crash-diagnostics collision review (explicit)

| Our diagnostics mechanism | NRI interaction | Verdict |
|---|---|---|
| `EnableD3D12Dred()` before `D3D12CreateDevice` (`DeviceD3D12.cpp:162`) | Zero DRED references in NRI (grep over `Source/` + `Include/`) | **No collision.** Wrapping cannot touch DRED config. |
| D3D12 marker heap / placed buffer with `ALLOW_CROSS_ADAPTER` (`GpuCrashD3D12.cpp:377-386`) | Separate from NRI's D3D12MA allocator (`DeviceD3D12.hpp:398-424`) and its 4 MB zero buffer (`:355-375`) | **No collision.** Extra VRAM from NRI's zero buffer is the only cost. |
| `VK_EXT/KHR_device_fault` sweep + feature chaining (`DeviceVulkan.cpp:686-751`, `:783-792`) | NRI never names either extension; feature structs it does not know about are simply absent from its own chain (it builds its own `VkDeviceCreateInfo` only on the non-wrapper path) | **No collision.** Keep the sweep exactly as-is; list the winners in `vkExtensions.deviceExtensions`. |
| `VK_AMD_buffer_marker` + `vkCmdWriteBufferMarkerAMD` via `VULKAN_HPP_DEFAULT_DISPATCHER` (`GpuCrashVulkan.cpp:430-431`) | NRI builds an **independent** dispatch table over the same `VkDevice` (`DeviceVK.hpp:445`, `:1812-2007`) and never resolves the AMD marker entry point | **No collision**, but note two live dispatch tables and two loader handles for one device — by design, and the reason to leave `libraryPath` null so both resolve `vulkan-1.dll`. |
| Marker buffer + `VkDeviceMemory` allocated raw off our device (`GpuCrashVulkan.cpp:328`, `:357`) | NRI's VMA does not see them; teardown order (crash backend released before `vkDestroyDevice`, `DeviceVulkan.cpp:907-915`) is unchanged | **No collision**, provided the NRI device is destroyed in the same window as the NVRHI handles are today. |
| Device-removed observation (`ObserveDeviceRemoved`, both backends) | Upgrades from NVRHI's message-substring hook to typed `nri::Result::DEVICE_LOST` at submit/acquire/present, per the spec | **Improvement, not collision.** The `"gpu-crash: device removed"` reason string must not be reworded (`DeviceVulkan.cpp:76-81`, `DeviceD3D12.cpp:52-57`). |
| `RenderErrorCount` latch fed by the VK validation callback (`DeviceVulkan.cpp:97-110`) | Unaffected — that messenger is on **our** instance. NRI's `CallbackInterface` becomes a second feed. | **No collision.** Note NRI's own `MessageCallback` (`DeviceVK.hpp:167-200`) is installed only on the non-wrapper path (`m_Messenger`), so there is no duplicate-messenger risk. |
| D3D12 InfoQueue | NRI does nothing in wrapper mode (§2.6.1) | **Gap to close in Phase 1**, not a collision. |

---

## 5. Phase 1 must add — the actionable delta

**Vulkan (`DeviceVulkan.cpp`), in one place next to the existing F-5c sweep:**

1. Add `VK_KHR_get_surface_capabilities2` to `kInstanceExtensions`. *(hard)*
2. Add `VK_KHR_push_descriptor` to `kDeviceExtensions`. *(hard, if any root
   descriptor is used — and the frame-graph design uses them)*
3. Add `VK_KHR_maintenance5`, `VK_KHR_maintenance6`, `VK_EXT_memory_budget`
   to `kDeviceExtensions`. *(quality; matches NVIDIA's sample)*
4. Replace the hand-picked feature structs with the
   **query-then-pass-back-verbatim** pattern from
   `NRISamples/Source/Wrapper.cpp:340-360`: chain
   `VkPhysicalDeviceFeatures2` + `Vulkan11Features` + `Vulkan12Features` +
   `Vulkan13Features`, call `getFeatures2`, then use that same chain as
   `VkDeviceCreateInfo::pNext`. This closes `bufferDeviceAddress`,
   `hostQueryReset`, `maintenance4`, `descriptorIndexing`,
   `drawIndirectCount`, `samplerFilterMinmax`, `shaderDrawParameters`,
   `multiview`, the shader-type bits, **and the entire core 1.0 block** in
   one stroke, and keeps the diagnostics fault-feature struct at the tail as
   it is today.
   - If a narrower set is preferred for auditability, the minimum hard set
     is: `synchronization2`, `dynamicRendering`, `timelineSemaphore`,
     `bufferDeviceAddress`, `hostQueryReset`, `maintenance4` — plus
     `samplerAnisotropy` and `textureCompressionBC` from the 1.0 block for
     any real content.
5. Assert the physical device's `apiVersion` minor is ≥ 3, and pass that
   minor as `DeviceCreationVKDesc::minorVersion` (never a hardcoded
   constant that can drift from `VkApplicationInfo`).
6. Pass the **filtered** instance/device extension vectors into
   `vkExtensions` — the same discipline the code already documents for
   NVRHI at `DeviceVulkan.cpp:827-830`.
7. Declare exactly one GRAPHICS queue family with `queueNum = 1` and
   `familyIndex = m_graphicsQueueFamily`; immediately after wrapping,
   assert `GetQueue(GRAPHICS, 0) == Result::SUCCESS` (catches the
   throwaway-instance clamp-to-zero mode).
8. Choose `vkBindingOffsets` consistent with the shader compiler's
   HLSL→SPIR-V register mapping (NRI reference: `{0, 128, 32, 64}`).
9. Leave `libraryPath` null; leave `enableMemoryZeroInitialization` false
   unless `VK_EXT_zero_initialize_device_memory` is also enabled.

**D3D12 (`DeviceD3D12.cpp`):**

10. Pass our `ID3D12CommandQueue` explicitly via
    `QueueFamilyD3D12Desc::d3d12Queues` (else NRI creates its own and the
    DXGI swapchain is orphaned).
11. Vendor the Agility SDK redistributable and export
    `D3D12SDKVersion` / `D3D12SDKPath` **from each host executable**
    (`ArcaneRuntime`, `ArcaneEditor`), and build NRI's D3D12 backend with
    `NRI_ENABLE_AGILITY_SDK_SUPPORT` — otherwise enhanced barriers,
    `ID3D12Device10+`, and the `OPTIONS9..22` capability set are all
    permanently off.
12. Register our own `ID3D12InfoQueue1::RegisterMessageCallback` (Agility)
    to route D3D12 debug-layer messages into the log +
    `RenderErrorCount` latch — NRI deliberately does not in wrapper mode.
13. Keep `EnableD3D12Dred()` exactly where it is (before create) and
    `disableD3D12EnhancedBarriers = false`.

**Both:**

14. Write the contract as executable asserts in the device TU beside the
    existing sweep (spec § Device layer), keyed off the *physical*
    query so the assert message names the feature the GPU supports but we
    failed to enable.
15. Destroy the NRI device before our native device, in the slot the NVRHI
    handles occupy today.

---

## 6. Ambiguities and things NOT verified here

Called out deliberately rather than guessed:

1. **Exact VUID numbers.** This document states *requirements*
   (e.g. "`SHADER_DEVICE_ADDRESS` buffer usage requires the
   `bufferDeviceAddress` feature", "maintenance4 entry points require the
   `maintenance4` feature") without citing VUID identifiers. The
   requirements are certain; the identifiers were not checked against the
   spec text. Phase 1 should let the validation layer name them.
2. **`maintenance4` strictness.** NRI takes the maintenance4 path off
   physical support (`DeviceVK.hpp:672`, `:1193`, `:1384`). That the
   *feature bit* (not merely the promoted core version) gates
   `vkGetDeviceBufferMemoryRequirements` is asserted here from the
   promotion convention, not from a quoted VUID. Enabling it is free and
   makes the question moot.
3. **`NRI_AGILITY_SDK_VERSION_MAJOR` is unused in NRI's own source**
   (grep finds it only in `CMakeLists.txt:642`, `:669`). The Agility
   *runtime* pickup therefore depends entirely on the generated
   `NRIAgilitySDK.h` exports, and the claim that those exports must live in
   the **main executable** rests on Microsoft's Agility documentation, not
   on NRI source. Verify empirically in Phase 1 (log
   `"Using ID3D12Device%u"` from `DeviceD3D12.hpp:255` — a value ≥ 10 proves
   the redistributable loaded).
4. **`InitializeNvExt`'s wrapper-mode logic** (`DeviceD3D12.hpp:916-923`)
   appears inverted: the default disables NVAPI when wrapping, while
   setting `disableNVAPIInitialization = true` makes NRI initialize it
   anyway. Reported as-is; moot while NVAPI stays out of the vendoring.
5. **`disableVKRayTracing`** is not a field of `DeviceCreationVKDesc` and
   is consumed only in `ProcessDeviceExtensions` (`DeviceVK.hpp:578`), which
   the wrapper path skips. In wrapper mode, ray-tracing extensions are
   included or excluded purely by our own extension list.
6. **`vkBindingOffsets` reconciliation** with our existing shader binding
   convention (`ShaderConventions.hpp`) was out of scope for this task; it
   is a Phase 1 design item, and getting it wrong is a silent
   wrong-descriptor bug rather than a create-time failure.
7. **Descriptor-indexing sub-bit granularity.** NRI reads only the
   aggregate `features12.descriptorIndexing` (`DeviceVK.hpp:1170`) but the
   *individual* sub-features are what the layout flags at
   `PipelineLayoutVK.hpp:224-234` actually require. The exact sub-bit ↔
   `DescriptorRangeBits` mapping was not enumerated; enable the block.
8. **`m_MinorVersion` vs the instance version.** NRI uses one scalar for
   both physical-device feature chaining and VMA's `vulkanApiVersion`
   (`DeviceVK.hpp:208`). Where instance and physical-device versions differ,
   the correct value is `min(instance minor, physicalDevice minor)`; NRI
   does not resolve this for us and does not validate the field at all.
9. Line numbers are from the current gitignored `.example/NRI` clone
   (v180). Function names are the durable anchors.

---

## 7. ImGui render-backend brief (Task 8)

**Question posed by the spec's Plan-inputs section**
(`docs/specs/2026-08-12-nri-adoption-design.md:216-218`): "Dear ImGui
version vs `NRIImgui`'s ≥1.92 requirement — bump ours or port our backend
to raw NRI (decide in plan; porting ours is small and keeps the
game-debug-ImGui-in-viewport path unchanged)." This section resolves it
with facts from both trees.

**Recommendation, stated up front: PORT OURS.** Not because of a version
gap — there isn't one (§7.5) — but because `NRIImgui` hard-requires the
NRI Streamer extension, which the spec's own Non-goals section has
already deferred (`docs/specs/2026-08-12-nri-adoption-design.md:206`,
Non-goals: "Streamer adoption (until measured), ray tracing, mesh
shaders, Reflex, upscalers beyond NIS, HDR output..."). See §7.6 for the
full rationale.

### 7.1 Our vendored Dear ImGui version

`ThirdParty/imgui/imgui.h:1` (top-of-file banner): `// dear imgui, v1.92.9 WIP`.

`ThirdParty/imgui/imgui.h:32-33`:

```c
#define IMGUI_VERSION       "1.92.9 WIP"
#define IMGUI_VERSION_NUM   19281
```

We are already on **1.92.9 WIP** (`IMGUI_VERSION_NUM 19281`) — past, not
behind, `NRIImgui`'s documented ≥1.92 floor (§7.4). Also present at
`imgui.h:35`: `#define IMGUI_HAS_TEXTURES // Added
ImGuiBackendFlags_RendererHasTextures - from IMGUI_VERSION_NUM >= 19198`
— the exact numeric floor for the texture-management protocol both our
own backend and `NRIImgui` depend on (§7.2, §7.4).

### 7.2 Our ImGui backend surface — `ArcaneClient/src/Arcane/ImGui/`

Six files, all nvrhi-touching (three implementation pairs; every header
carries `nvrhi::ICommandList*`/`nvrhi::IFramebuffer*` in its public
signatures, every `.cpp` calls into `nvrhi::IDevice` or a type built from
it):

| File | Role |
|---|---|
| `ImGuiLayer.hpp` / `.cpp` | Exported facade (`ARCANE_API`) for the **editor's own OS-window ImGui context**: owns the `ImGuiContext`, wires upstream `imgui_impl_sdl3` for platform/input via `Window::SetNativeEventTap` (`ImGuiLayer.cpp:65`, `:110-115`), and owns an `ImGuiNvrhiRenderer` instance for drawing. Renders post-tonemap into the caller-supplied display-referred backbuffer framebuffer. Platform glue lives **here**, not in the renderer. |
| `ImGuiNvrhi.hpp` / `.cpp` | The actual **nvrhi renderer** — internal, not exported. `Init` sets `io.BackendFlags \|= ImGuiBackendFlags_RendererHasTextures \| ImGuiBackendFlags_RendererHasVtxOffset` (`ImGuiNvrhi.cpp:41-42`) and builds the sampler/binding-layout/input-layout/pipeline from `imgui_vs`/`imgui_ps` shaders (via `ShaderLibrary`, hot-reload-aware through `m_pipelineGeneration`, `:292-296`). `UpdateTexture`/`DestroyTexture` implement the 1.92 `ImTextureData`/`ImTextureStatus` texture lifecycle (`WantCreate`/`WantUpdates`/`WantDestroy`, `:105-142`) via `createTexture`+`writeTexture` directly on an open `nvrhi::ICommandList` — no manual barriers (`setKeepInitialState(true)`). `RenderDrawData` walks `ImDrawData`/`ImDrawList`/`ImDrawCmd` and issues `setGraphicsState`/`drawIndexed` per draw command, casting `cmd->GetTexID()` straight to `nvrhi::ITexture*` (`:246-247`) — **this cast is our engine-wide `ImTextureID` convention** (§7.3). This is the one TU that would actually change under an NRI raw-API port; the other two pairs are backend-agnostic apart from their function signatures. |
| `OffscreenImGuiLayer.hpp` / `.cpp` | **The game-debug-ImGui-in-viewport seam.** A second, self-contained `ImGuiContext` + its own `ImGuiNvrhiRenderer` + its own font atlas, with **manually injected IO** (no SDL backend — `Input{displaySize, mousePos, mouseDown[5], wheel, deltaTime, hasInput}`, `OffscreenImGuiLayer.hpp:36-44`) instead of OS events. Header comment (`:4-9`): "for hosting a SECOND ImGui layer inside an offscreen render target — e.g. a game/plugin's debug UI composited into an editor viewport, separate from the editor's own ImGui." `Context()` returns the raw `ImGuiContext*` for a plugin to adopt via `Runtime::SetImGui`/`EngineContext` (`:34`). |

### 7.3 The game-debug-ImGui-in-viewport wiring + the `ImTextureID` convention

`ArcaneEditor/src/App/EditorApp.cpp:439`:
```cpp
m_gameImgui = Arcane::OffscreenImGuiLayer::Create(m_gpu->Device(), m_gpu->Shaders());
```
— built from the **same** `RenderDevice`/`ShaderLibrary` the editor's own
`ImGuiLayer` uses (comment at `:436-438`), then composited into the
viewport panel each frame. This is the exact path the spec calls out by
name.

Separately, and engine-wide (not limited to the game-viewport seam): our
`ImGuiNvrhiRenderer::RenderDrawData` reads `cmd->GetTexID()` and casts it
straight to `nvrhi::ITexture*` (`ImGuiNvrhi.cpp:246-247`); the matching
write side is `(ImTextureID)(intptr_t)handle.Get()` at texture-create
time (`ImGuiNvrhi.cpp:125`). Editor call sites rely on this convention
directly — a grep for `ImTextureID`/`ImGui::Image` across `ArcaneEditor/`
returns 17 matches in 6 files; live call sites (not comments) include
`ArcaneEditor/src/Panels/EditorPanels.cpp:491,933`,
`ArcaneEditor/src/Documents/SpriteDocument.cpp:309`, and
`ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:2536,2579,2622,3139,3759,5357`
— viewport textures, sprite thumbnails, shader-preview thumbnails, and
the editor logo all pass a raw `nvrhi::ITexture*` cast to `ImTextureID`.
This convention is load-bearing across the editor, not a single call
site (§7.6 point 3).

### 7.4 `NRIImgui` from source — `.example/NRI`

Header: `Include/Extensions/NRIImgui.h`. Implementation:
`Source/Shared/ImguiInterface.h` (class decl) +
`Source/Shared/ImguiInterface.hpp` (impl, gated
`#if NRI_ENABLE_IMGUI_EXTENSION`, `ImguiInterface.h:5`).

**Documented requirements** (`NRIImgui.h:11-13`, verbatim):
```
- ImGui 1.92+ with "ImGuiBackendFlags_RendererHasTextures" flag ("IMGUI_DISABLE_OBSOLETE_FUNCTIONS" is recommended)
- unmodified "ImDrawVert" (20 bytes) and "ImDrawIdx" (2 bytes)
- "ImTextureID_Invalid" = 0
```
All three are already satisfied by our tree today: version §7.1;
`ImGuiBackendFlags_RendererHasTextures` is set at `ImGuiNvrhi.cpp:41`;
`ImDrawVert`/`ImDrawIdx` sizes are enforced by our own `static_assert`s
at `ImGuiNvrhi.cpp:26-30`; `ImTextureID_Invalid` is ImGui's unmodified
default of `0` (`imgui.h:352`, and we never redefine it).

**No compile-time version check exists.** `NRIImgui` does not
`#include` our `imgui.h` anywhere — confirmed by a repo-wide grep over
`.example/NRI/Source` and `Include` for `IMGUI_VERSION`/`imgui.h`/
`#include.*[Ii]mgui`, which returns only its own `#include
"ImguiInterface.h"` cross-references, zero hits on the real header.
`README.md:49` documents this deliberately: `` `NRIImgui.h` - a
light-weight *ImGui* renderer (**no *ImGui* dependency**) ``. Instead,
`Source/Shared/ImguiInterface.hpp:243-330` hand-copies minimal
struct-layout mirrors of `ImDrawVert`, `ImDrawList`, `ImDrawCmd`,
`ImTextureData`, `ImTextureRef`, etc., with the comment at `:243`:
`// Copied from Imgui // TODO: always keep in sync with latest`. The
"≥1.92" requirement is therefore a **documentation-only contract**, not
something the compiler or NRI itself can verify against our actual
vendored copy — a struct-layout drift on our side (e.g. a future
`ImDrawCmd` field reorder) would silently desync rather than fail to
build.

**What it renders — draw-data only, nothing else:**

| Capability | In `NRIImgui`? | Citation |
|---|---|---|
| Draw-data rendering (`CmdCopyImguiData` + `CmdDrawImgui`) | **yes — this is the entire scope** | `NRIImgui.h:54-65`; impl `ImguiInterface.hpp:447-643` (copy), `:645-871` (draw) |
| Platform/input glue (window events, clipboard, IME) | **no** | Zero SDL/window-system references anywhere in `ImguiInterface.hpp`/`.h`; header comment `NRIImgui.h:15-17`: "the goal of this extension is to support latest ImGui only", "designed only for rendering" |
| Font-atlas *building* / IO ownership | **no** — atlas building stays 100% ImGui's/ours | `NRIImgui` only consumes `ImTextureData` handed to it via `CopyImguiDataDesc::textures` (`NRIImgui.h:40`) — it never touches `ImGuiIO::Fonts` or calls any atlas-build API |
| Font/user-texture GPU upload | **yes**, via the NRI **Streamer** | `ImguiInterface.hpp:536-542` (`StreamTextureDataDesc` + `m_iStreamer.StreamTextureData`) |
| `ImGui::Image()`-style user textures | **yes**, but different ID protocol than ours | `NRIImgui.h:21-22`: `"ImGui::Image*" functions are supported. "ImTextureID" must be a "SHADER_RESOURCE" descriptor: ImGui::Image((ImTextureID)descriptor, ...)` — keys off an `nri::Descriptor*`, not the raw resource handle our `nvrhi::ITexture*` convention uses (§7.3) |
| `drawList->AddCallback` (ImGui user callbacks) | **no**, explicitly unsupported except one HDR-scale special case | `NRIImgui.h:18-20`; our own renderer also asserts none are used today (`ImGuiNvrhi.cpp:235-236`) — parity, not a regression either way |
| Shaders | **embedded**, not ours | Compiled bytecode baked in per-backend (`ImguiInterface.hpp:22-234`: dxbc/dxil/spirv byte arrays) or generated via `Shaders/Imgui.{fs,vs}.hlsl` + ShaderMake (`:4-16`) — **not** routed through our `ShaderLibrary`, so it does not participate in our shader hot-reload (`m_pipelineGeneration` tracking, §7.2) |
| Pipeline/descriptor model | **own, bindless-flavored** | Its own `PipelineLayout` with 2 descriptor sets — a static sampler set and a dynamic `ALLOW_UPDATE_AFTER_SET` texture set sized by `ImguiDesc::descriptorPoolSize` (default 128) — built independently of any binding-layout our engine already owns (`ImguiInterface.hpp:379-424`) |

**Threading.** `ImguiInterface.h:52,64`: the impl holds its own
`StreamerInterface m_iStreamer` and a `Lock m_Lock`. Both `CmdCopyData`
and `CmdDraw` open with `ExclusiveScope lock(m_Lock)`
(`ImguiInterface.hpp:448`, `:646`) — internally synchronized, matching
the interface doc's `// Threadsafe: yes` at `NRIImgui.h:53`. Not a
concern either way: our own renderer is single-threaded-per-context by
construction (one `ImGuiContext` = one call site per frame) and never
needed a lock.

**Dependency footprint — the load-bearing fact.**
`ImguiImpl::Create` (`ImguiInterface.hpp:350-354`) resolves the NRI
**Streamer** interface as its first act:
```cpp
Result result = nriGetInterface(m_Device, NRI_INTERFACE(StreamerInterface), &m_iStreamer);
```
and `CmdCopyData` is built entirely on it —
`m_iStreamer.StreamTextureData` (`:542`, `:558`, `:574`),
`m_iStreamer.StreamBufferData` (`:619`), `m_iStreamer.CmdCopyStreamedData`
(`:634`). There is no code path that renders `ImDrawData` without the
Streamer. `README.md:53,91-92` confirms Streamer is its own separately
gated extension (`NRIStreamer.h`, `NRI_STREAMER_THREAD_SAFE` build
flag) — adopting `NRIImgui` is not adopting one extension, it is
adopting two.

**This collides with an explicit spec ruling, not just a preference.**
`docs/specs/2026-08-12-nri-adoption-design.md:206` lists under
Non-goals: "Streamer adoption (until measured), ray tracing, mesh
shaders, Reflex, upscalers beyond NIS, HDR output (formats noted as
future direction)." `docs/research/2026-08-12-nri-survey.md:151-155`
gives the reason: "the
Streamer constant ring has no overflow protection — undersizing
`constantBufferSize` silently corrupts in-flight frames (wraps on size
only, no fence check). A live NVIDIA `\ TODO ... right? :)` questions
whether `CmdCopyStreamedData` needs a barrier. Both need empirical
validation before a frame graph leans on the Streamer." Adopting
`NRIImgui` today would pull the Streamer in through a side door — the
one subsystem the spec has already named as not-yet-safe to build on.

### 7.5 Version-delta / API-break analysis

There is **no version delta to resolve**. We are already on 1.92.9 WIP
(§7.1), past `NRIImgui`'s documented ≥1.92 floor, and our own renderer
already implements the 1.92 texture-management protocol
(`ImGuiBackendFlags_RendererHasTextures`, the `ImTextureData`/
`ImTextureStatus` lifecycle, `ImTextureID_Invalid`) — see §7.2. That
migration (from the pre-1.92 single-font-atlas-texture model to the
per-`ImTextureData` create/update/destroy protocol) already happened in
our tree; it is not a cost either branch of this decision would incur.
Concretely: **adopting `NRIImgui` would not save us an upgrade we still
owe, because we don't owe one.** The only variable this decision
actually turns on is the render-backend implementation (raw nvrhi calls
vs `NRIImgui`'s own pipeline+Streamer), not the ImGui version.

### 7.6 Recommendation: PORT OURS

1. **No version motivation either way** (§7.5) — this removes the one
   argument that could have favored adopting `NRIImgui` ("catch up to
   ≥1.92"). We're already there.
2. **The Streamer dependency is a hard blocker today, not a style
   preference.** `NRIImgui::Create` cannot construct without
   `StreamerInterface` (§7.4), and the spec has already deferred
   Streamer adoption pending the two empirical fixes in
   `docs/research/2026-08-12-nri-survey.md:151-155`. Choosing
   `NRIImgui` for Phase 1 would force Streamer adoption now, out of
   order, for the least performance-critical draw path in the engine.
3. **The `ImTextureID` protocol change would ripple wider than the
   game-viewport seam the spec named, independent of the wholesale-swap
   ruling.** The spec's binding ruling
   (`docs/specs/2026-08-12-nri-adoption-design.md:18-19`: "this is a
   COMPLETE migration. One branch, wholesale swap, no dual-RHI period")
   means ImGui's *renderer* moves off nvrhi in Phase 1 either way — that
   part is not optional and this decision does not reopen it. What *is*
   optional is whether the post-swap `ImTextureID` convention stays a
   raw handle cast (port ours) or becomes an `nri::Descriptor*` (adopt
   `NRIImgui`, per `NRIImgui.h:21-22`). Our engine-wide convention is
   used in at least 9 live call sites across 3 editor files (§7.3) plus
   the `OffscreenImGuiLayer` game-debug path; recutting that convention
   is a second, separable migration bolted onto the RHI swap for no
   reason forced by NRI itself (raw NRI has no opinion on how a host
   chooses its `ImTextureID` values — only `NRIImgui`'s own texture-set
   design does). The frame graph's node-by-node golden-verified cutover
   order (`docs/specs/2026-08-12-nri-adoption-design.md:169-173`) is
   easier to keep honest when the ImGui node's diff is "same renderer,
   different API calls" rather than "different renderer, different
   pipeline, different descriptor model, different texture-ID scheme,
   new Streamer dependency" all at once.
4. **Shader hot-reload parity.** Our renderer's pipeline cache is keyed
   off `ShaderLibrary::Generation()` (`ImGuiNvrhi.cpp:292-296`) so
   `imgui_vs`/`imgui_ps` hot-reload like every other shader in the
   engine. `NRIImgui`'s shaders are embedded bytecode baked at NRI's
   build time (§7.4) — adopting it would carve out the one UI surface
   that stops hot-reloading, for no functional gain.
5. **Keeps the game-debug-ImGui-in-viewport path unchanged**, per the
   spec's own stated reason (`docs/specs/2026-08-12-nri-adoption-design.md:216-218`)
   — `OffscreenImGuiLayer`'s manually-injected-IO / dual-context design
   (§7.2, §7.3) is orthogonal to which draw backend `ImGuiNvrhiRenderer`
   targets; porting only touches the renderer.

**What porting ours costs.** Of the six files in §7.2, only
`ImGuiNvrhi.hpp`/`.cpp` issues GPU calls (`nvrhi::IDevice::create*`,
`nvrhi::ICommandList::write*`/`setGraphicsState`/`drawIndexed`) — that
pair is the entire edit surface for a raw-NRI port: swap the
`nvrhi::` device/command-list calls for the equivalent `nri::CoreInterface`
calls (`CreateTexture`/`CreateSampler`/`CreatePipelineLayout`/
`CmdSetGraphicsState`-equivalents/`CmdDrawIndexed`), keep our own
`imgui_vs`/`imgui_ps` shaders and `ShaderLibrary` binding, keep our own
`ImTextureID` = raw-handle-cast convention, and skip the Streamer
entirely (upload via the same open-command-list `writeTexture`/
`writeBuffer`-equivalent pattern already used, per the frame graph's
general upload strategy — spec `docs/specs/2026-08-12-nri-adoption-design.md:108-111`).
`ImGuiLayer.cpp`/`OffscreenImGuiLayer.cpp` need no changes beyond
whatever type renames the device layer's own port introduces elsewhere
(`RenderDevice`/`ShaderLibrary` call shapes) — their platform-glue and
dual-context logic (§7.2) does not touch nvrhi/nri APIs directly, only
through the renderer they own.

**What Phase 1 should NOT do:** do not enable
`NRI_ENABLE_IMGUI_EXTENSION`, do not vendor `Shaders/Imgui.*.hlsl` or
its precompiled bytecode, and do not adopt the Streamer as a side
effect of this decision. Revisit `NRIImgui` only if/when the Streamer
is independently adopted (its own measured decision, not this one) —
at that point the version floor will still be satisfied and this
section's §7.4 facts remain the reference.
