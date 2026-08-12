# NRI (v180) Deep Source Survey — Extension Ecosystem, Backend Quality, Build System, Vendoring Feasibility

Scope: `D:\dev\starworks\Arcane\.example\NRI` (pinned v180, shallow clone, tag `v180`, single commit `4b48531`)
and `D:\dev\starworks\Arcane\.example\NRISamples` (embeds `NRIFramework` as a "submodule" under `External/`).
Audience: engine team evaluating migration from NVRHI. Windows-first, premake5 + MSVC, vendors deps into
`ThirdParty/` as source or premake static libs, avoids CMake where possible.

---

## 1. Extension inventory (`Include/Extensions/*.h`)

All 12 extension headers enumerated, line counts via `wc -l`:

| Header | Lines | Category |
|---|---|---|
| `NRIDeviceCreation.h` | 95 | core-adjacent (device/adapter creation, callbacks, VK binding offsets) |
| `NRIHelper.h` | 201 | core-adjacent (utility) |
| `NRIImgui.h` | 75 | optional |
| `NRILowLatency.h` | 56 | optional (NVIDIA Reflex) |
| `NRIMeshShader.h` | 27 | optional (modern-HW feature) |
| `NRIRayTracing.h` | 396 | optional (modern-HW feature) |
| `NRIStreamer.h` | 80 | optional (QoL) |
| `NRISwapChain.h` | 209 | core-adjacent (presentation) |
| `NRIUpscaler.h` | 151 | optional (QoL, vendor-integration surface) |
| `NRIWrapperD3D11.h` | 59 | optional (interop) |
| `NRIWrapperD3D12.h` | 108 | optional (interop) |
| `NRIWrapperVK.h` | 135 | optional (interop) |

Every extension is acquired through the same generic mechanism: `nriGetInterface(device, NRI_INTERFACE(XInterface), &table)` (`Include/NRI.h:47-49`, impl `Source/Creation/Creation.cpp:626-708`). There is no separate extension-loading/linking step — all extension implementations for enabled backends are compiled directly into the one `NRI` target (see §4); "extension" only means "optional function-table", not "separate binary/plugin".

### NRIDeviceCreation (`Include/Extensions/NRIDeviceCreation.h`)
Device/adapter enumeration (`nriEnumerateAdapters`), `nriCreateDevice`/`nriDestroyDevice`, allocation callbacks, message/abort callback interface, VK binding-offset config, per-queue-family creation, and the master on/off switches for NRI validation, GAPI validation, D3D11 command-buffer emulation, D3D12 ray-tracing validation (env-var gated: `NV_ALLOW_RAYTRACING_VALIDATION=1`), memory zero-init, and enhanced-barrier opt-out. This is the true bootstrap surface — every consumer needs it; core-adjacent, not really "optional."

### NRIHelper (`Include/Extensions/NRIHelper.h`)
Grab-bag of utilities NRI itself doesn't need but almost every consumer does: `CalculateAllocationNumber`/`AllocateAndBindMemory` (grouped sub-allocation for many resources), `UploadData` (one-shot, not for streaming — see Streamer for that), `QueryVideoMemoryInfo`, DXGI⇄NRI and VK⇄NRI format conversion functions, and two `static inline` convenience functions baked directly into the header: `GetSupportedDepthFormat` and `FitPipelineLayoutSettingsIntoDeviceLimits` (a genuinely useful D3D12-root-signature-budget solver, lines 116-199, that fits descriptor tables/root constants/root descriptors into the 256-byte root-signature budget). No vendor SDK dependency.

### NRIImgui (`Include/Extensions/NRIImgui.h`)
Renders Dear ImGui draw data. The header explicitly documents "no ImGui dependency" is the design goal but that claim needs qualification: `NRIImgui.h:25-26` forward-declares `ImDrawList`/`ImTextureData` via `NonNriForwardStruct` (opaque pointer types), so the **header** doesn't `#include <imgui.h>` and NRI.dll itself is not linked against ImGui. But the *consumer* must still bring real Dear ImGui (`ImDrawVert` must be the unmodified 20-byte layout, `ImDrawIdx` the unmodified 2-byte layout — comment lines 10-13) to produce the `ImDrawList*`/`ImTextureData*` arrays passed into `CmdCopyImguiData`/`CmdDrawImgui`. So "no ImGui dependency" = NRI ships no ImGui source/binary and doesn't hard-`#include` ImGui headers, not "you don't need ImGui." Confirmed at the sample-framework level: `NRISamples/External/NRIFramework/Include/NRIFramework.h:47` does `#include "imgui.h"` directly — the actual ImGui dependency lives at the consumer/framework layer, exactly as advertised. Special support for `ImGui::Image()` via `SHADER_RESOURCE` descriptor cast, and a callback hack (`NRI_IMGUI_OVERRIDE_HDR_SCALE`, lines 70-75) to override HDR scale per-draw-call via a bit-cast float smuggled through `ImDrawCallback`.
Requires ImGui 1.92+ with `ImGuiBackendFlags_RendererHasTextures`.

### NRILowLatency (`Include/Extensions/NRILowLatency.h`)
This is NVIDIA Reflex. 4-call API: `SetLatencySleepMode`, `SetLatencyMarker`, `LatencySleep` (call once before `INPUT_SAMPLE`), `GetLatencyReport` (12 timestamp fields spanning simulation/render-submit/present/driver/OS-queue/GPU). **No NVAPI dependency visible at the Include-layer API** — the interface itself is GAPI-agnostic (`SwapChain`-scoped). Implementation almost certainly routes through NVAPI/VK low-latency extensions in the backend `.hpp` files (README lists `VK_NV_low_latency2` under supported VK extensions, `README.md:180`), but the extension header carries no explicit NVAPI type. Multi-swapchain low latency is VK-only per the header comment (line 46). Threadsafe: no.

### NRIMeshShader (`Include/Extensions/NRIMeshShader.h`)
Tiny — 2 functions (`CmdDrawMeshTasks`, `CmdDrawMeshTasksIndirect`). No dependencies beyond core.

### NRIRayTracing (`Include/Extensions/NRIRayTracing.h`)
The largest extension (396 lines) and the most API-complete of the set: pipeline creation (shader libraries/groups, recursion depth, payload/attribute size limits, `FAIL_ON_CACHE_MISS` pipeline-cache-miss gate), full Opacity Micromap (OMM) support (2/4-state formats, special-index semantics, build/copy/compaction), BLAS (triangles + AABBs geometry types, transform buffers) and TLAS (instance flags including force-opaque/force-non-opaque/micromap-disable) construction, **both VK-style bind-after-create memory binding AND D3D12-style committed/placed creation** (dual API surface, `RayTracingInterface` lines 353-366), shader-table writing (`WriteShaderGroupIdentifiers`), size queries for scratch/update buffers, and both direct and indirect dispatch (`CmdDispatchRays`/`CmdDispatchRaysIndirect`). Also exposes native-object getters (`GetAccelerationStructureNativeObject` returns `ID3D12Resource*` or `VkAccelerationStructureKHR`) for wrapper interop. This reads as feature-complete against both DXR and VK ray-tracing/ray-query extension sets, not a lowest-common-denominator subset.

### NRIStreamer (`Include/Extensions/NRIStreamer.h`)
Ring-buffer abstraction for per-frame dynamic constant/vertex/texture data upload with automatic placement (`StreamBufferData`/`StreamTextureData`/`StreamConstantData`), separate statically-sized constant-buffer ring vs. dynamically-resized general ring, `queuedFrameNum` in-flight tracking, `CmdCopyStreamedData` device-side flush, `EndStreamerFrame` host-side frame boundary. Thread safety is a CMake-time choice (`NRI_STREAMER_THREAD_SAFE`, default ON — "OFF is faster", `CMakeLists.txt:60`). No vendor deps.

### NRISwapChain (`Include/Extensions/NRISwapChain.h`)
Presentation: 4 swap-chain color formats spanning BT.709 8/10/16-bit and BT.2020 PQ 10-bit HDR, `Scaling`/`Gravity` enums lifted directly from `VkPresentScalingFlagBitsKHR`/`VkPresentGravityFlagBitsKHR`, a `Window` union covering Win32/X11/Wayland/Metal (`WindowsWindow`/`X11Window`/`WaylandWindow`/`MetalWindow`, lines 55-79 — this is the one place Metal is a first-class NRI concept, a `CAMetalLayer*` passed straight through), `WAITABLE`/`ALLOW_TEARING`/`ALLOW_LOW_LATENCY` flags, and `DisplayDesc` (chromaticity primaries, min/max/full-frame/SDR luminance, `isHDR`) queried per-swap-chain via `GetDisplayDesc`. Contains a full worked usage example in a comment block (lines 139-207) — unusually thorough for an extension header.

### NRIUpscaler — see dedicated analysis below.

### NRIWrapperD3D11 / NRIWrapperD3D12 / NRIWrapperVK
Symmetric "adopt a native object into NRI" APIs: `nriCreateDeviceFromD3D11Device`/`...D3D12Device`/`...VKDevice`, then per-object `Create<Type><API>` functions (buffer/texture/command-buffer/fence/descriptor-pool/etc.) that wrap an existing native handle. D3D11/D3D12 wrappers also accept an optional `AGSContext*` (AMD AGS) at device-creation time and a `disableNVAPIInitialization` switch — confirming NVAPI/AGS initialization is device-creation-scoped and needs to happen once per process (comment: "at least NVAPI requires calling `NvAPI_Initialize` in DLL/EXE where the device is created", `NRIWrapperD3D11.h:33`). D3D12 wrapper additionally exposes `AccelerationStructureD3D12Desc` for wrapping externally-built AS objects, and `WrapperVKInterface` exposes `GetInstanceProcAddrVK`/`GetDeviceProcAddrVK` for interop with libraries needing raw VK function pointers (used internally by the FSR/FFX upscaler backend — see §1 Upscaler).

### Upscaler (`Include/Extensions/NRIUpscaler.h` + impl `Source/Shared/UpscalerInterface.hpp`)

**Vendors covered:** NIS (NVIDIA Image Scaling — sharpener/upscaler, cross-vendor), FSR (AMD FidelityFX Super Resolution, cross-vendor), XeSS (Intel XeSS Super Resolution), DLSR (DLSS Super Resolution, NVIDIA-only), DLRR (DLSS Ray Reconstruction / denoiser, NVIDIA-only) — `NRIUpscaler.h:13-19`. The `Include/Extensions/NRIUpscaler.h` header itself is 100% vendor-agnostic (no vendor SDK types leak into the public API) — a single `UpscalerType` enum selects the backend at `CreateUpscaler` time, and generic `UpscalerGuides`/`DenoiserGuides` resource structs are shared across vendors via a union keyed on type.

**How each vendor SDK is actually wired in `Source/Shared/UpscalerInterface.hpp`** (each gated by its own CMake option/compile define — all default OFF):
- **NIS** (`NRI_ENABLE_NIS_SDK`): fully self-contained — the compute shader is compiled at NRI-build-time via ShaderMake into an embedded header blob (`NIS.cs.dxbc.h`/`.dxil.h`/`.spirv.h`, `UpscalerInterface.hpp:24-34`) and the filter-coefficient textures are baked-in constants (`Source/Shared/NIS.h`, ported from NVIDIA's public `NVIDIAImageScaling` repo). **No external DLL to ship, no vendor library to link.** This is the only upscaler with zero runtime footprint.
- **FSR** (`NRI_ENABLE_FFX_SDK`): loaded via `LoadSharedLibrary("amd_fidelityfx_dx12.dll"/"amd_fidelityfx_vk.dll")` **at runtime**, function pointers (`ffxCreateContext`/`ffxDestroyContext`/`ffxDispatch`) resolved via `GetSharedLibraryFunction` (`UpscalerInterface.hpp:791-811`). The engine must ship these prebuilt signed DLLs next to the exe (CMake copies them from `${ffx_SOURCE_DIR}/PrebuiltSignedDLL`, `CMakeLists.txt:1082-1110`). Only compile-time dependency is the `ffx_upscale.h` header. Notable code smell: a hand-patched `vkGetDeviceProcAddr` shim (`FfxVkGetDeviceProcAddr`, lines 122-141) because "FFX devs don't understand how VK works... some VK functions are retrieved with non-CORE names" — remaps `vkGetBufferMemoryRequirements2KHR` → `vkGetBufferMemoryRequirements2` and asserts on any other unexpected non-KHR/non-AMD lookup.
- **XeSS** (`NRI_ENABLE_XESS_SDK`, D3D12-only per `NRIUpscaler.h` `IsUpscalerSupported` check): statically linked against `libxess.lib` import lib (`CMakeLists.txt:454-456`), calls `xessD3D12CreateContext` directly (not dynamically loaded). Ships `libxess.dll` alongside the exe (copied post-build). XeSS methods are documented **not thread-safe** — guarded by a process-global `Lock g_xess.lock` (`UpscalerInterface.hpp:312-314`).
- **DLSR/DLRR = NGX/DLSS** (`NRI_ENABLE_NGX_SDK`): statically linked against `nvsdk_ngx_s`/`nvsdk_ngx_d` import lib (suffix picks static vs. DLL CRT, `CMakeLists.txt:356-370`) calling `nvsdk_ngx_helpers*.h` functions directly. The engine ships the actual ML-model runtime DLLs `nvngx_dlss.dll` + `nvngx_dlssd.dll` (copied post-build from `${ngx_SOURCE_DIR}/lib/Windows_x86_64/{dev,rel}/`, `CMakeLists.txt:1063-1080`). NGX methods are also documented **not thread-safe** (comment cites `nvsdk_ngx.h`), guarded by a global lock + a hand-rolled per-device refcounter array (`g_ngx.refCounters`, 32-entry fixed array, comment: "awful API borns awful solutions...", `UpscalerInterface.hpp:372-383`). `IsUpscalerSupported` gates DLSR/DLRR on `vendor == NVIDIA && tiers.rayTracing` as "an elegant way to detect an RTX GPU" (comment, line 464).

**Bottom line for what the engine must ship:** NIS = nothing extra. FSR = 1-2 prebuilt AMD DLLs. XeSS = 1 Intel DLL + static import lib at link time. DLSS = 2 NVIDIA DLLs + static NVIDIA import lib at link time, plus all four (FSR/XeSS/DLSS/DLSS-RR) require fetching a proprietary vendor SDK at NRI-build-time via CMake `FetchContent` from vendor URLs (§4) — none of these SDKs are vendored inside the NRI repo itself (confirmed: `find . -iname "*dlss*" -o -iname "*xess*" -o -iname "*fsr*"` returns nothing in `Source/`/`Include/` — only compile-time `#if NRI_ENABLE_*_SDK` guards and header includes with no fallback stub).

---

## 2. Backend implementations (`Source/*`)

File counts and approximate LOC (via `find`/`cat | wc -l` over `.cpp`/`.h`/`.hpp`):

| Backend | Files | LOC | Notes |
|---|---|---|---|
| `Source/D3D11` | 36 | 7,142 | mature, D3D11.1 baseline, NVAPI/AGS-normalized |
| `Source/D3D12` | 40 | 9,487 | mature, D3D12 Ultimate + enhanced barriers |
| `Source/VK` | 42 | 11,526 | mature, largest backend, dynamic-rendering-first with legacy render-pass fallback |
| `Source/WGPU` | 36 | 7,697 | newest/thinnest-featured backend, heavily TODO-annotated |
| `Source/Validation` | 39 | 6,316 | wraps any backend, opt-in |
| `Source/Shared` | 15 | 5,650 | cross-backend: Helper/Imgui/Streamer/Upscaler impls, NIS coefficients |
| `Source/Creation` | 1 | 1,154 | device-creation dispatch, `nriGetInterface` |
| `Source/NONE` | 1 | 1,214 | dummy/no-op backend |

### TODO/FIXME/HACK density (grep `-riE "TODO|FIXME|HACK"`, count per dir)
- D3D11: **14**
- D3D12: **28**
- VK: **34**
- WGPU: **46** (highest density relative to size: 46 in 7,697 LOC vs. VK's 34 in 11,526 LOC)
- Validation: **2**
- Shared: **16**
- NONE: **0**

**D3D12 quality notes:** TODOs are almost all narrow/precise engineering caveats, not architectural gaps — e.g. `Source/D3D12/CommandBufferD3D12.hpp:1079` "`TODO: verify that it works`" on an enhanced-barrier discard-flag heuristic, `SharedD3D12.hpp:38` a commented-out array-size validation for acceleration-structure descriptor ranges ("TODO: 0 is expected for ACCELERATION_STRUCTURE"), and several D3D12MA integration caveats (`DeviceD3D12.hpp:402-406`, alignment/residency-priority limitations acknowledged as "currently broken on D3D12MA side").

**VK quality notes:** Similarly precise. Two are notable: `Source/VK/CommandBufferVK.hpp:927` — `#if 0 // TODO: NV driver can crash if VVL is enabled...` (a code path disabled specifically to dodge an NVIDIA-driver+Vulkan-Validation-Layers interaction crash — a real-world landmine documented in the source, not hidden). `Source/VK/DeviceVK.hpp:937` — "Least Common Multiple stride across all formats: 1, 2, 4, 8, 16 // TODO: rarely used '12' fucks up the beauty of power-of-2 numbers" (informal but not concerning). `DeviceVK.hpp:1245` explicitly documents NRI does **not** emulate `shaderDrawParameters` on Vulkan because ">99% devices support it" natively (contrast with D3D12, which emulates draw-parameters via a reserved root constant — `NRIHelper.h:125-126`, `enableD3D12DrawParametersEmulation`).

**WGPU quality notes — this backend reads as materially less mature than D3D11/D3D12/VK.** Its TODOs are architectural gaps, not micro-caveats: no mesh shaders, no ray tracing/AS, no low-latency mapping, no dynamic depth-bias, no variable-rate shading, no upscaler mapping, no waitable-swapchain, no bindless/descriptor-heap-indexing (`DescriptorPoolWGPU.hpp:14`, `DescriptorSetWGPU.hpp:217`), attachment clears are draw-emulated because WebGPU has no mid-pass clear command (`CommandBufferWGPU.hpp:1259`), fences are polling-emulated over `WGPUSubmissionIndex` (no native timeline fence, `FenceWGPU.hpp:12`), and shader reflection is explicitly minimal/handwritten SPIR-V and WGSL parsers rather than using a real reflection library (`PipelineLayoutWGPU.hpp:549,700` — "not full WGSL reflection... intentionally recognizes only direct texture declarations"). WGPU is also the only backend gated behind a separately-fetched native binary (`wgpu-native` prebuilt `.dll`/`.so`/`.dylib`, not source) rather than being buildable from source inside the NRI tree.

**D3D11 → D3D12/VK feature normalization (NVAPI/AGS).** README's claim ("*default D3D11 behavior is changed to match D3D12/VK using NVAPI or AMD AGS libraries, where applicable*", `README.md:34`) is concretely implemented in `Source/D3D11/*.hpp` via parallel `NvAPI_D3D11_*` / `agsDriverExtensionsDX11_*` call pairs for every feature D3D11 lacks natively:
  - UAV-overlap hinting (`NvAPI_D3D11_BeginUAVOverlap`/`EndUAVOverlap` vs. `agsDriverExtensionsDX11_BeginUAVOverlap`/`EndUAVOverlap`, `CommandBufferD3D11.hpp:35-80`)
  - Depth-bounds test (`NvAPI_D3D11_SetDepthBoundsTest` vs. `agsDriverExtensionsDX11_SetDepthBounds`, line 154-162)
  - Variable-rate shading (`NvAPI_D3D11_RSSetViewportsPixelShadingRates`/`RSSetShadingRateResourceView`, `NvAPI_D3D11_CreateShadingRateResourceView` in `DescriptorD3D11.hpp:218` — VRS appears NVIDIA-only on D3D11, no AGS equivalent found)
  - Multi-draw-indirect emulation, since D3D11 has no native multi-draw-indirect: `NvAPI_D3D11_MultiDrawInstancedIndirect`/`MultiDrawIndexedInstancedIndirect` vs. `agsDriverExtensionsDX11_MultiDrawInstancedIndirectCountIndirect` etc. (lines 448-491)
  - View-broadcast masks for multiview (`agsDriverExtensionsDX11_SetViewBroadcastMasks`, AGS-only, line 330)
  - Vendor shader extensions / wave intrinsics / shader-clock detection, all NVAPI-only: `NvAPI_D3D11_SetNvShaderExtnSlot`, `NvAPI_D3D11_IsNvShaderExtnOpCodeSupported` probing `NV_EXTN_OP_UINT64_ATOMIC`, `_GET_SPECIAL` (clock), `_SHFL`/`_SHFL_UP`/`_SHFL_DOWN`/`_SHFL_XOR`, `_VOTE_ALL`/`_VOTE_ANY`/`_VOTE_BALLOT` (`DeviceD3D11.hpp:163-187`)
  This is real, load-bearing vendor-extension plumbing, not decorative — D3D11's modern-feature parity with D3D12/VK is *entirely* contingent on NVAPI (NVIDIA) or AGS (AMD) being present; on unrecognized/Intel GPUs those D3D11 features silently degrade to unsupported.

### macOS / Metal status
**No native Metal backend exists.** `GraphicsAPI` bitmask (`Include/NRIDescs.h:98-103`) has exactly 5 members: `NONE`, `D3D11`, `D3D12`, `VK`, `WGPU` — no `METAL`. macOS support = **VK backend over MoltenVK**, exactly as the README states ("*Metal (through MoltenVK)*", `README.md:26`) and as `Include/NRIDescs.h:102` documents inline ("Vulkan 1.4+... can be used on MacOS via MoltenVK"). Metal-specific code that does exist is purely surface/interop glue, not a rendering backend: `VK_USE_PLATFORM_METAL_EXT` ifdefs in `Source/VK/DeviceVK.hpp:286-287,1804-1805` (instance extension + `vkCreateMetalSurfaceEXT` proc lookup) and `Source/VK/SwapChainVK.hpp:70-76` (constructs `VkMetalSurfaceCreateInfoEXT` from a raw `CAMetalLayer*`, passed in via `NRISwapChain.h`'s `MetalWindow{void* caMetalLayer}`). `NRISamples/External/NRIFramework/External/MetalUtility/MetalUtility.m` is a small Objective-C shim in the samples framework for obtaining the `CAMetalLayer` from a GLFW/Cocoa window — again glue, not a renderer. One documented MoltenVK gap: `Source/VK/DispatchTable.h:113` — `VK_EXT_depth_bias_control` "offers 2 [depth bias values] but MoltenVK doesn't support it yet".

### Android / tile-based-GPU handling
No Android-specific backend or file; Android appears only twice — `Source/Creation/Creation.cpp:777,789` (`#elif defined __ANDROID__` for shared-library search-path conventions) and `Source/Shared/SharedExternal.h:255` (same). Android is served by the VK backend like Linux. **Tile-based-GPU (TBR/TBDR) support is real but implicit, not a distinct code path**: the VK backend implements the full legacy `VkRenderPass`/`VkSubpassDescription2` path (`Source/VK/DeviceVK.hpp:2137-2197`) alongside the modern `vkCmdBeginRendering` dynamic-rendering path (`Source/VK/CommandBufferVK.hpp:808` uses `vkCmdBeginRenderPass`/`VK_SUBPASS_CONTENTS_INLINE` when render-passes are in play), and exposes `TextureView::SUBPASS_INPUT` as a first-class descriptor/view type across D3D11, D3D12, VK, and Validation (`DescriptorD3D11.hpp:102`, `DescriptorD3D12.hpp:149`, `DescriptorVal.hpp:49`) — this is the on-chip/input-attachment mechanism TBDR mobile GPUs need to avoid round-tripping through VRAM between subpasses. The `InputAttachment` NRISamples sample (§6) exists specifically to exercise "dynamic rendering local read" (`VK_KHR_dynamic_rendering_local_read`, listed in README's supported-extension table) — README's "Mobile-ready... optimized for TBR/TBDR" claim is backed by real API surface (`SUBPASS_INPUT` view type + local-read extension), not just marketing copy, though there's no dedicated mobile/Android sample or CI lane visible in this checkout to prove it end-to-end.

---

## 3. The Validation layer (`Source/Validation`)

**Architecture:** pure decorator/wrapper pattern. Every validated object (`BufferVal`, `TextureVal`, `CommandBufferVal`, `DeviceVal`, etc. — 18 wrapper types, `Source/Validation/SharedVal.h:13-30`) holds an `m_Impl` pointer to the real backend object (`ObjectVal::m_Impl`, `SharedVal.h:121`) and a reference to `DeviceVal`. Enabled per-device at creation (`deviceCreationDesc.enableNRIValidation`) and gated by the `NRI_ENABLE_VALIDATION_SUPPORT` CMake option (default ON) — if the option is off, `enableNRIValidation` is silently ignored (`README.md:89`). Device creation wraps the real device in `CreateDeviceValidation` (`Source/Creation/Creation.cpp:606-616`) only when validation is requested — **zero cost when disabled**, one extra virtual-call/branch hop per API call when enabled.

**What it actually validates** (539 total `NRI_RETURN_ON_FAILURE`/`NRI_CHECK`/validation-macro call sites across the directory):
- **Parameter/argument sanity**: null checks (`'buffer' is NULL`), enum-range checks (`< XxxType::MAX_NUM`), zero-size checks (`'size' is 0`), bounds checks with computed messages (`'offset=%llu' + 'size=%llu' must be <= buffer 'size=%llu'`, `DeviceVal.hpp:280`).
- **Feature-gating**: caller can't request functionality the device doesn't support — e.g. `mutableDescriptorType`, `filterOpMinMax`, `componentSwizzle`, `drawParameters`/`drawIndex` emulation requirements, `bindless` tier for `VARIABLE_SIZED_ARRAY` ranges (`DeviceVal.hpp:224,361,381-383,396`).
- **Command-buffer state machine**: recording-state checks on essentially every `Cmd*` call (`'must be in the recording state'`/`'already in the recording state'`, `CommandBufferVal.hpp:124,142,157...`), render-pass scoping (`'must be called inside CmdBeginRendering/CmdEndRendering'`, line 239).
- **Barrier argument correctness** (parameter-level, not full state-machine tracking): null-resource checks, access-mask-vs-resource-usage compatibility (`IsAccessMaskSupported`), layout-vs-usage compatibility (`IsTextureLayoutSupported`), and a `PRESENT`-layout-specific rule requiring `AccessBits::NONE`/`StageBits::NONE` (`CommandBufferVal.hpp:88-117`).
- **Descriptor-set-layout collision detection**: duplicate `registerSpace` use across descriptor sets/root descriptors (`DeviceVal.hpp:409,420`).

**What it does NOT appear to do** (grepped explicitly, zero hits): no thread-safety instrumentation (`grep -rn "thread\|Thread"` → 0 matches in `Source/Validation`), no leak/dangling/use-after-free tracking (`grep -rn "leak\|already destroyed\|dangling"` → 0 matches). This is a **call-time API-contract validator** (arguments, enum ranges, feature gates, command-buffer state machine, barrier-argument plausibility) — it is *not* a GPU-timeline resource-state tracker (it doesn't walk the full barrier history of a resource across a frame the way D3D12's debug layer's resource-state-tracking does), not a leak detector, and not a thread-safety checker. It complements but does not replace the underlying GAPI validation layer (D3D12 debug layer / VK validation layers), which NRI exposes as a *separate* orthogonal switch (`enableGraphicsAPIValidation`, `NRIDeviceCreation.h:75`) — the two are meant to run together, not as substitutes for each other.

**For this team's dev-loop-leans-on-validation use case:** NRI's own validation layer is solid for catching API-misuse bugs early with good, specific error messages (worth the "how good is this really" question a clear "good for what it claims, narrower than it might sound" answer) — but a team relying on it as their *primary* barrier/hazard-tracking safety net (the way some engines lean on NVRHI's or a custom validation layer for exactly that) will still need the platform validation layers (VK-LayerValidation / D3D12 debug layer, both toggleable independently) for actual GPU-side correctness, and NRI's layer won't catch cross-thread misuse on its own.

---

## 4. Build system + dependencies

### CMakeLists.txt (full read, 1196 lines) — targets & structure
Targets, one static lib per backend, linked into a final `NRI` (SHARED by default, or STATIC if `NRI_STATIC_LIBRARY=ON`): `NRI_Shared` (always), `NRI_NONE`, `NRI_D3D11`, `NRI_D3D12`, `NRI_VK`, `NRI_WGPU`, `NRI_Validation` — each conditionally built per its `NRI_ENABLE_*_SUPPORT` option, all folded into `NRI` via `target_link_libraries` (`CMakeLists.txt:1000-1024`). Backend selection at runtime is a `switch(graphicsAPI)` in `Source/Creation/Creation.cpp:1084-1092` — **all enabled backends are compiled into one binary**; there is no per-backend plugin/DLL split.

**Key options** (`CMakeLists.txt:52-72`, all default per the table):
`NRI_STATIC_LIBRARY` (OFF), `NRI_ENABLE_NVTX_SUPPORT` (ON), `NRI_ENABLE_DEBUG_NAMES_AND_ANNOTATIONS` (ON), `NRI_ENABLE_NONE_SUPPORT` (ON), `NRI_ENABLE_VK_SUPPORT` (ON), `NRI_ENABLE_VALIDATION_SUPPORT` (ON), `NRI_ENABLE_NIS_SDK` (OFF), `NRI_ENABLE_IMGUI_EXTENSION` (OFF), `NRI_STREAMER_THREAD_SAFE` (ON); dependent options: `NRI_ENABLE_D3D11_SUPPORT`/`D3D12_SUPPORT` (ON if WIN32), `NRI_ENABLE_WGPU_SUPPORT` (OFF), `NRI_ENABLE_AMDAGS`/`NRI_ENABLE_NVAPI` (ON if D3D + not ARM64), `NRI_ENABLE_AGILITY_SDK_SUPPORT` (ON if D3D12), `NRI_ENABLE_XLIB_SUPPORT`/`WAYLAND_SUPPORT` (ON if headers found), `NRI_ENABLE_NGX_SDK`/`FFX_SDK`/`XESS_SDK` (all OFF by default — opt-in vendor upscalers).

**Compile definitions**: every enabled `NRI_ENABLE_*` option is passed through 1:1 as a `=1` preprocessor define (`CMakeLists.txt:96-125`), plus `WIN32_LEAN_AND_MEAN`, `NOMINMAX`, `_CRT_SECURE_NO_WARNINGS`. Compile flags are strict: `-Wextra -Werror` (Clang/GCC) / `/W4 /WX` (MSVC) on `NRI_Shared` (`CMakeLists.txt:424-444`) — **the codebase is warnings-as-errors clean**, a positive quality signal.

**Platform branches**: Linux gets `find_path` probes for `X11/Xlib.h` and `wayland-client.h` to conditionally enable those (`CMakeLists.txt:15-18`); architecture detection branches for arm64/x64/x86 drive which NVAPI/WGPU binary variant to fetch (lines 20-42); Apple branch calls `find_package(Vulkan REQUIRED)` and adds `VK_USE_PLATFORM_METAL_EXT`/`VK_ENABLE_BETA_EXTENSIONS` (lines 681,752-753).

### Dependency acquisition: pure CMake `FetchContent`, all URL-based zips — no git submodules at the NRI level
Every third-party dependency is `fetchcontent_declare(... URL https://...)`, explicitly preferring URL zips over `GIT_REPOSITORY` "*because of the fat `.git` folder*" (comment, `CMakeLists.txt:130-133`). Full dependency list with **pinned versions** (as declared in this v180 checkout):

| Dependency | Version/ref | Source | License (general knowledge — not vendored/verifiable locally, fetched at build time) |
|---|---|---|---|
| Agility SDK | 1.619.3 (via `NRI_AGILITY_SDK_VERSION_MAJOR/MINOR`) | NuGet (`Microsoft.Direct3D.D3D12`) | Microsoft proprietary redistributable |
| NVTX | v3.5.0 | GitHub zip (`NVIDIA/NVTX`) | Apache-2.0 |
| Vulkan-Headers | v1.4.354 | GitHub zip (Khronos) | Apache-2.0 / MIT dual |
| VulkanMemoryAllocator (VMA) | commit `3aa9212...` | GitHub zip (GPUOpen) | MIT |
| NVIDIA DLSS (NGX) | v310.6.0 | GitHub zip (`NVIDIA/DLSS`) | NVIDIA proprietary SDK EULA |
| AMD FidelityFX SDK | v1.1.4 | GitHub release zip | MIT (AMD FFX SDK is MIT-licensed) |
| Intel XeSS | v3.0.1 | GitHub zip (`intel/xess`) | Intel proprietary SDK license (redistributable binary) |
| D3D12MemoryAllocator (D3D12MA) | commit `1d86c11...` | GitHub zip (GPUOpen) | MIT |
| wgpu-native | v29.0.0.0 | GitHub release zip (prebuilt binary, not source) | MIT/Apache-2.0 dual (wgpu project) |
| AMD AGS SDK | v6.3.0 | GitHub zip | MIT |
| NVIDIA NVAPI | commit `9b181ea...` | GitHub zip (`NVIDIA/nvapi`) | NVIDIA proprietary SDK license — **not freely redistributable in source form**, static-link-only binary distribution historically |
| ShaderMake | commit `5daebdb...` | GitHub zip (`NVIDIA-RTX/ShaderMake`) | MIT (NVIDIA-RTX tools are generally MIT) |

(License column is general public knowledge about these SDKs, not verified against a local LICENSE file — none of these deps are vendored in this checkout; they download fresh on every clean `_Build` per the `FetchContent` declarations above. NVAPI and NGX/DLSS are the two with real proprietary-license friction — NVAPI historically forbids redistributing its *source*, only the compiled/linked result; NGX/DLSS ships binary-only runtime DLLs under NVIDIA's SDK license.)

Post-`FetchContent`, temp archive dirs are explicitly deleted (`file(REMOVE_RECURSE ... "-tmp")`, lines 304-306) — tidy but means nothing is cached/vendored for offline/reproducible builds by default.

### 1-Deploy / 2-Build / 3-PrepareSDK — what they actually do
- `1-Deploy.bat`: `mkdir _Build && cd _Build && cmake .. %*` — thin CMake-configure wrapper, forwards all CLI args (so `-DNRI_ENABLE_NGX_SDK=ON` etc. work).
- `2-Build.bat`: `cmake --build . --config Release -j N` then `--config Debug -j N` — builds both configs sequentially.
- `3-PrepareSDK.bat`: assembles a distributable SDK folder (`_NRI_SDK/`) by copying `Include/*` + `Include/Extensions/*`, `LICENSE.txt`, `README.md`, `nri.natvis`, and the built `NRI.dll`/`.lib`/`.pdb` for both configs from `_Bin/{Debug,Release}` — i.e., **the "SDK" is just headers + prebuilt binaries, no source**, meant for consumers who build NRI once centrally and distribute the binary SDK to game teams. This script is the shape a vendored ThirdParty/NRI drop would take if NVIDIA's own model were followed literally.
- No `packman` anywhere in this repo (that's an internal NVIDIA tool sometimes seen elsewhere) — confirmed by absence in CMakeLists and scripts. Acquisition is 100% public `FetchContent` from GitHub/NuGet URLs.

### C ABI surface
`NRI.h` is written through the `NriX`/`Nri(X)`/`NriPtr(X)`/`NriRef(X)` macro layer (`Include/NRIMacro.h`, 361 lines — a substantial variadic-macro-arity-counting preprocessor library, MSVC/GCC-branch-aware, max 125 args) that generates **both** a C++ (`nri::` namespace, references) and a pure-C (`Nri`-prefixed structs, pointers) view from a single struct/function definition. README documents the exact 1:1 mapping (`README.md:192-201`, e.g. `nri::Interface` ↔ `NriInterface`, C++ reference `&` ↔ C pointer `*`). Functions are `extern "C"` (`NRI_API` expands to `extern "C" __declspec(dllexport)` on Windows shared builds, `CMakeLists.txt:988-993`), so linking from a pure-C or non-MSVC-ABI-compatible consumer works.

**Interface (function-table) acquisition** is the core mechanism, not vtables/COM: `nriGetInterface(device, "InterfaceName", sizeof(InterfaceStruct), &outStruct)` (`Include/NRI.h:47-49`, impl `Source/Creation/Creation.cpp:626-708`). It string-hashes the interface name (with `nri::`/`Nri` prefix stripping so both C and C++ callers can pass either spelling), does an **exact `sizeof()` match check** against the real struct size before filling it (protects against silent ABI skew between a stale header and a newer/older `NRI.dll` — mismatched interface size returns an explicit "invalid size" error rather than corrupting memory), then does a **full null-pointer scan across every function-pointer slot** in the filled table before declaring success (`Creation.cpp:696-704`) — catches a partially-implemented backend (e.g. an extension not supported by the current GAPI) as a hard failure rather than a shipped null-deref landmine. This is a well-defended ABI-acquisition pattern.

### FEASIBILITY VERDICT — vendoring as a premake static lib without CMake

**Verdict: yes, buildable as a premake static lib from vendored source, and CMake is not load-bearing for the core D3D12(+Validation) path this team most likely wants — but three real friction points need explicit engineering decisions before vendoring.**

What premake needs to replicate, concretely:
- **Source file list**: straightforward — it's the exact file lists already enumerated per-target in `CMakeLists.txt` (`D3D12_SOURCE`, `NRI_VALIDATION_SOURCE`, `SHARED_SOURCE`, `NRI_SOURCE`, `NRI_HEADERS`, `NRI_EXTENSIONS` — all copy-pasteable into a premake `files{}` block). For a **D3D12 + Validation + NONE** slice (the most plausible minimal vendoring target for a Windows-first MSVC team not needing VK/D3D11/WGPU): `Source/Shared` (15 files) + `Source/D3D12` (40 files) + `Source/Validation` (39 files) + `Source/NONE` (1) + `Source/Creation` (1) ≈ **96 source files**, ~24,000 LOC (`9,487 + 6,316 + 5,650 + 1,214 + 1,154`).
- **Include dirs**: `Include/` (public), `Source/Shared/` (private, cross-backend). For D3D12: add `${d3d12ma_SOURCE_DIR}/include` + `/src` (D3D12MA), optionally `${nvapi_SOURCE_DIR}` + AGS `ags_lib/inc`, optionally Agility SDK's `build/native/include`.
- **Compile defines**: mechanical 1:1 copy of the `NRI_ENABLE_*=1` list (`CMakeLists.txt:96-125`) driven by which backends/options the team picks, plus `WIN32_LEAN_AND_MEAN`, `NOMINMAX`, `_CRT_SECURE_NO_WARNINGS`.
- **Link libs**: `d3d12`, `dxgi`, `dxguid` (system), optionally `nvapi64.lib` if NVAPI enabled.

Genuinely hard/friction parts:
1. **Vendor SDK acquisition for Upscaler/DLSS/FSR/XeSS is a network-fetch-at-configure-time design (`FetchContent` from GitHub/NuGet), and this team's convention is "vendor as source or premake static libs" with no CMake.** If the Upscaler extension is wanted (DLSS especially — real product value for an RTX-aligned title), someone has to manually download+pin NVIDIA/DLSS, GPUOpen/FidelityFX-SDK, and/or intel/xess zips once and commit them under `ThirdParty/`, replicating what `FetchContent` currently automates. NIS is the one exception — fully self-contained, zero extra vendoring (§1).
2. **NVAPI's license restricts redistributing its *source*** — historically NVIDIA's NVAPI SDK terms permit linking/shipping the compiled result but not republishing the SDK's headers/lib source freely; this needs a licensing check before committing `nvapi/` into a `ThirdParty/` tree that's presumably itself either public or has its own distribution terms. AGS (MIT) and D3D12MA/VMA (MIT) have no such friction.
3. **The ShaderMake tool + DXC/FXC toolchain for the `NIS`/`Imgui` shader extensions is itself a separate fetched CMake project** (`shadermake` `FetchContent`, `CMakeLists.txt:283-294`) whose own compiler-path resolution (`SHADERMAKE_DXC_PATH`/`FXC_PATH`/`DXC_VK_PATH`) isn't traced in this survey (its CMakeLists isn't present locally — only fetched at NRI-configure-time). If the team wants the `NIS` upscaler or `Imgui` extension's HLSL shaders precompiled at build time the way NRI does, they need either a working ShaderMake+DXC/FXC premake replacement, or to pre-bake the shader blobs once (offline) and vendor the resulting header blob (`NIS.cs.dxbc/dxil/spirv.h`) as static source — the latter is actually straightforward since those are just generated headers, not something requiring build-time regeneration on every build.
4. **The Agility SDK step writes a generated header** (`Include/NRIAgilitySDk.h` via CMake `file(WRITE ...)`, `CMakeLists.txt:663-676`) and copies the Agility SDK binary redistributable next to the exe — trivial to replicate as a one-time hand-written header + a vendored binary drop, not a real blocker.
5. **WGPU backend depends on a prebuilt native binary** (not source — `wgpu-native` ships as a compiled `.dll`/`.lib`), so it can't be vendored "as source" at all under this team's stated convention; it would have to be vendored as a prebuilt binary exception (same shape as AGS's `.dll`) if WGPU is wanted, or (more likely, given this team has no stated WebGPU need) simply excluded from the vendored footprint entirely — which is easy since it's already its own independent CMake target/directory.

**Net read:** for the realistic target (D3D12 primary, VK secondary if cross-platform is wanted, Validation always, NONE for headless CI/lane-check builds, WGPU excluded) — CMake is doing dependency-fetching orchestration, not anything premake structurally cannot replicate. The genuinely hard part isn't "translate CMakeLists.txt to premake5.lua" (mechanical, done above), it's "stand up a one-time manual vendoring pass for 3-6 external SDKs (some proprietary-licensed) that CMake currently fetches transparently" — a solvable, bounded, one-time cost, not an ongoing CMake dependency.

---

## 5. Versioning / cadence / support surface

**Version scheme**: single flat monotonically-increasing integer, not semver. `Include/NRI.h:41-42`: `#define NRI_VERSION 180` / `#define NRI_VERSION_DATE "23 June 2026"`. `CMakeLists.txt:7-9` parses this directly out of `NRI.h` via regex (`NRI_VERSION ([0-9]*)`) to set the CMake project version — the header is the single source of truth for the version number, and the CMake project version is entirely derived from it (no independent CMake-side version to drift). This flat-integer scheme means version *number* jumps (e.g. the task brief's noted "v1.99 → v170+") are structurally unremarkable under this scheme in a way a semver break would not be — it looks like an internal build/revision counter, not a semantic-compatibility signal. **Could not independently verify the specific v1.99→v170+ jump from this checkout**: this is a shallow single-commit clone (`git log --all` → 1 commit `4b48531 "Fixed a typo"`, `git tag` → only `v180`, `.git/shallow` confirms the graft) — no local git history exists to inspect the actual version-jump commit or changelog entries. Flagging this as unverified-locally rather than asserting a cause.

**Release cadence signals**: README states "*there is only `main` branch used for development; stable versions are in `Releases` section*" (`README.md:57-59`) — i.e., NRI follows a trunk-based-dev + tagged-releases model, not long-lived release branches. This checkout's tag `v180` embeds a date of `23 June 2026` directly in the version macro — dates are baked into the versioning scheme itself, suggesting fairly frequent/regular tagged cuts (a per-version embedded date implies each `NRI_VERSION` bump is deliberately dated, consistent with an active/frequently-released project), though exact cadence (weekly/monthly) isn't derivable from this single-commit checkout.

**Support/stability claims (README)**: positions NRI as filling a specific niche distinct from NVRHI itself — "*There is NVRHI, which offers a D3D11-like abstraction layer, implying some overhead... NRI was designed to offer a reasonably simple, low-overhead, and high-performance render interface*" (`README.md:31`) — i.e. NVIDIA's own docs frame NRI as the **lower-level, less-abstracted alternative to NVRHI**, not a drop-in replacement with equivalent ergonomics; explicit non-goals include "*high-level (D3D11-like) abstraction*" and "*automatic barriers*" (`README.md:15-19`) — both things NVRHI *does* provide. This is the single most important framing fact for a migration-from-NVRHI decision: **NRI is intentionally more explicit/lower-level than NVRHI, not a like-for-like swap** — a migration means taking on manual barrier management and less abstraction, in exchange for the extension ecosystem (upscalers, Reflex, streaming, ray tracing completeness) and closer-to-driver overhead.

**Deprecation/compatibility markers found in source**: `Include/NRIDescs.h:1811` includes `nriVersion` as a field *inside* `DeviceDesc` (queryable at runtime, not just a header macro) — suggests the API is designed for runtime version-skew detection between a consumer built against one `NRI_VERSION` and a `NRI.dll` built against another, reinforcing the interface-size-check pattern in `nriGetInterface` (§4) as the primary compatibility-shim mechanism rather than a legacy-API-preservation approach (no `_Deprecated`/`Obsolete`-suffixed types were found in either extension headers or `NRIDescs.h`).

---

## 6. NRISamples + NRIFramework

### Samples (`NRISamples/Source/*.cpp,*.c` — 19 samples total, per `README.md:41-60`)

| Sample | Demonstrates |
|---|---|
| `AsyncCompute.cpp` | parallel graphics+compute queue execution |
| `BindlessSceneViewer.cpp` | bindless GPU-driven rendering (separate `GLOBAL_DESCRIPTOR_SET`/`MATERIAL_DESCRIPTOR_SET`) |
| `Buffers.c` | buffer creation/usage (pure-C sample — proves the C ABI works in practice, not just in theory) |
| `Clear.cpp` | minimal framebuffer-clear-only rendering |
| `ClearStorage.c` | storage-resource clearing (pure C) |
| `DeviceInfo.c` | device-group enumeration/introspection (pure C) |
| `DescriptorHeapIndexing.cpp` | HLSL dynamic resources / dynamically-indexed descriptor heaps |
| `InputAttachment.cpp` | "dynamic rendering local read" (on-chip TBDR-style read of render results — the tile-GPU-relevant sample, §2) |
| `LowLatency.cpp` | Reflex/low-latency extension usage |
| `MultiThreading.cpp` | multi-threaded command-buffer recording |
| `Multisample.cpp` | MSAA rendering |
| `Multiview.cpp` | multiview in _LAYER_BASED_ mode (VK+D3D12-compatible) |
| `RayTracingBoxes.cpp` | advanced RT: many BLASes in one TLAS |
| `RayTracingTriangle.cpp` | minimal RT triangle |
| `Readback.cpp` | GPU→CPU data readback |
| `Resize.cpp` | window-resize/swapchain-resize handling |
| `Resources.c` | resource allocation patterns (pure C) |
| `SceneViewer.cpp` | mesh+material loading/rendering; also exercises programmable sample locations, shading rate, pipeline statistics |
| `Triangle.cpp` | textured triangle; also demonstrates multiview in _FLEXIBLE_ mode |
| `Wrapper.cpp` | wrapping native D3D11/D3D12/VK objects into NRI entities (the WrapperD3D11/D3D12/VK extensions in practice) |

Coverage gaps relative to the brief's ask: **no dedicated Imgui-rendering sample** (Imgui usage lives inside the shared `NRIFramework` UI overlay used by all samples, not a standalone demo), **no dedicated Upscaler sample** (DLSS/FSR/XeSS/NIS usage isn't exercised by any sample in this list — the extension exists and is implemented but untested by the sample suite in this checkout), **no dedicated Streamer-only sample** (folded into `SceneViewer`/general resource-upload paths implicitly).

### NRIFramework (`NRISamples/External/NRIFramework`)
Genuinely thin compared to a Donut-style app framework: **`Include/` + `Source/` totals only 4,594 LOC** (`Camera.h/.cpp`, `Controls.h`, `CmdLine.h`, `Helper.h`, `NRIFramework.h`, `Timer.h/.cpp`, `Utils.h/.cpp`, `SampleBase.cpp`, `DebugAllocator.cpp`). Concretely provides:
- **Windowing/input**: GLFW (`#include "GLFW/glfw3.h"`, `NRIFramework.h:46`), with platform-specific native-handle extraction macros per OS (`GLFW_EXPOSE_NATIVE_WIN32`/`_COCOA`/`_X11`/`_WAYLAND`, lines 15-30) feeding directly into NRI's `SwapChainDesc.window` union.
- **UI**: real Dear ImGui (`#include "imgui.h"`, line 47) rendered through the `NRIImgui` extension — `InitImgui`/`CmdCopyImguiData`/`CmdDrawImgui`/`DestroyImgui` wrapper methods on the sample base class (`NRIFramework.h:144-165`), full ImGui input-event remapping (`SampleBase.cpp:30+`, `RemapKey`).
- **Camera**: `Camera.h/.cpp` — a basic fly/orbit camera, not a full scene-graph camera system.
- **Asset loading**: `Utils.cpp` (glTF-style scene/material loading implied by `SceneViewer`/`BindlessSceneViewer` samples) + vendored **Detex** (`External/Detex/` — BC/ETC/EAC/BPTC texture-block decompression, DDS/KTX/HDR file parsing, `stb_image` for other formats) for texture loading, and vendored **MathLib** (`External/MathLib/` — NVIDIA's own SIMD-friendly math library, `ml.h`/`ml.hlsli` shared between C++ and HLSL).
- **Metal interop glue**: `External/MetalUtility/MetalUtility.m` — Obj-C helper to pull a `CAMetalLayer*` out of a Cocoa/GLFW window for the VK-over-MoltenVK swap-chain path.
- Version-tracked independently: `NRI_FRAMEWORK_VERSION_MAJOR/MINOR` = 0.25, dated "22 June 2026" (`NRIFramework.h:5-7`) — one day before the NRI v180 date, suggesting framework and core are versioned/released in lockstep.

**Verdict**: NRIFramework is a **sample-harness**, not an app framework in the Donut/Falcor sense — no scene graph, no material/shader system beyond what individual samples hand-roll, no render-graph, no deferred/TAA pipeline, no asset-pipeline/cooker. It's windowing + input + camera + ImGui glue + texture decoding + a math library, i.e. exactly enough to stand up the 19 standalone samples, not a reusable production rendering layer. A team evaluating NRI should not expect to inherit any framework-level rendering architecture from NRIFramework — only the low-level NRI API itself and this thin scaffolding.

---

## Surprises / concerns (cross-cutting)

1. **The Upscaler/LowLatency/RayTracing extension API surface is genuinely vendor-neutral and well-designed at the header level, but the *implementation* backing DLSS and XeSS is explicitly documented as not thread-safe with hand-rolled global locks and (for NGX) a 32-entry fixed-size refcounter array with a comment openly mocking the vendor SDK's API design ("awful API borns awful solutions...", `UpscalerInterface.hpp:382`). This is an honest, unusually self-aware quality signal, but also flags that DLSS/XeSS integration must be treated as single-threaded-per-process at the call sites regardless of NRI's own general thread-safety story.**
2. **WGPU is a real fifth backend, not mentioned in the task brief's survey list, and it is clearly the least mature** — nearly 1-in-170 LOC is a TODO marking a missing/emulated feature, versus roughly 1-in-340 for VK and 1-in-510 for D3D12. If this team has zero interest in WebGPU (Windows-first MSVC premake shop — plausible), this is a non-issue and a clean exclusion; flagging only because it changes the "how many backends does this team need to reason about" framing versus the brief's four (D3D12/VK/D3D11/NONE+Validation).
3. **The NVIDIA-VVL-crash-workaround comment in `CommandBufferVK.hpp:927`** (`#if 0 // TODO: NV driver can crash if VVL is enabled...`) is worth flagging to whoever owns the "validation gate" dev-loop directive — it's a documented instance of NVIDIA's own driver having a real bug under Vulkan Validation Layers that NRI works around by disabling a code path, not a hypothetical.
4. **No local git history** — this is a shallow single-commit clone (tag `v180` only). Anything about release cadence, the specific v1.99→v170+ version jump the task brief asked about, or historical stability could not be verified from this checkout; would need either an unshallowed clone or NVIDIA's public GitHub releases page to answer definitively.
5. **License friction is real but narrow**: exactly two dependencies (NVAPI, NGX/DLSS) carry NVIDIA proprietary SDK terms that are stricter than the MIT/Apache terms covering everything else (AGS, VMA, D3D12MA, Vulkan-Headers, NVTX, ShaderMake, FidelityFX SDK). Everything else in the dependency table is permissively licensed. This narrows the "is vendoring legally clean" question to a two-item checklist rather than a repo-wide audit.
6. **NIS is the free lunch** — the only upscaler with zero external DLL/SDK footprint (self-contained compute shader + baked coefficients). If the team wants "an upscaler" without vendor-SDK/licensing overhead at all, NIS is immediately usable; DLSS/FSR/XeSS all require the vendoring-and-licensing work described in §4.
