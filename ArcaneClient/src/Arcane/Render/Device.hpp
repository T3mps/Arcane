#pragma once

// Render module: the engine's GPU device. Creation is headless by design --
// a swapchain is created separately against a Window (next tasks), so tools
// and tests can run compute/offscreen work without any window.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <string>

namespace Arcane
{
    class Window;
    class Swapchain;

    enum class GraphicsBackend : uint8_t
    {
        D3D12,
        Vulkan,
    };

    ARCANE_API const char* ToString(GraphicsBackend backend);

    // Total render-layer Error/Fatal diagnostics since process start, across
    // all devices and EVERY producer -- not just NVRHI's own message callback:
    // the Vulkan debug messenger, the D3D12 debug layer's InfoQueue1 callback,
    // NRI's callback interface and ARC_NRI_CHECK, and anything else reporting
    // through NvrhiMessageCallback::NoteError all land in this one counter.
    // GPU tests assert this stays zero -- the machine-enforced form of the
    // "validation must stay silent" foundation rule.
    ARCANE_API uint64_t RenderErrorCount();

    // Test support ONLY -- production code must never call this (the count
    // above is documented as "since process start"). Restores the 0/0 gate
    // latch to zero; exists so a test that deliberately trips it (proving a
    // discipline macro reaches the real, shared latch rather than a fake
    // local counter) can clean up after itself instead of leaking a
    // permanent +1 into every unrelated test case's RenderErrorCount()==0
    // assertion for the rest of the process. Same idiom as
    // ResetGpuDeviceLost() (GpuInstrumentation.hpp).
    ARCANE_API void ResetRenderErrorCount();

    // Test support ONLY -- the two seams a [nri] case needs to prove that
    // NvrhiMessageCallback::NoteError reaches THIS latch (and that it does
    // NOT fire the device-removed hook). They exist because
    // NvrhiMessageCallback is a header-only singleton -- a function-local
    // static in NvrhiMessageCallback.hpp -- so a test exe that included that
    // header would drive its OWN instance while RenderErrorCount(), exported
    // from ArcaneClient.dll, kept reading the DLL's. Production code inside
    // the DLL calls NvrhiMessageCallback::Instance().NoteError directly and
    // must never reach for these.
    ARCANE_API void NoteRenderErrorForTest(const char* tag, const char* text) noexcept;

    // Installs (or, with nullptr, clears) the device-removed hook on the
    // DLL-side NvrhiMessageCallback. Last-writer-wins, exactly like the
    // class's own setter -- so a test MUST clear it before the function it
    // names goes out of scope, and must not run while a real device holds
    // the slot (no device exists in the ~[gpu] gate, which is where the one
    // caller lives).
    ARCANE_API void SetRenderDeviceRemovedHookForTest(void (*hook)()) noexcept;

    struct RenderDeviceDesc
    {
        GraphicsBackend backend = GraphicsBackend::D3D12;
#if defined(ARCANE_DEBUG)
        bool enableValidation = true;   // NVRHI validation layer + VK validation
#else
        bool enableValidation = false;
#endif
        // Opt-in: D3D12 CPU debug layer (EnableDebugLayer). Disabled by
        // default because D3D12SDKLayers.dll raises RaiseFailFastException
        // (code 0x87D) when third-party window hooks (e.g. Nahimic OSD) are
        // loaded. The NVRHI validation layer covers command-level errors;
        // enable this only when debugging D3D12 API parameter errors on a
        // machine without injected window hooks.
        bool enableD3D12DebugLayer = false;

        // Opt-in: Vulkan SYNCHRONIZATION validation, on top of the ordinary
        // VK_LAYER_KHRONOS_validation core checks `enableValidation` turns on.
        // Vulkan-only -- D3D12's debug layer has no separate sync-validation
        // switch (its closest analogue, GPU-Based Validation, is a different
        // and far costlier thing), so `enableD3D12DebugLayer` above is the
        // whole D3D12 story.
        //
        // DEFAULT FALSE, AND THE ENGINE'S OWN BOOT NEVER SETS IT. Its only
        // caller today is the `--nri-graph` frame-graph vehicle
        // (Render/Nri/NriGraphContext.cpp), which forces it on in Debug --
        // the class of defect sync validation catches (hazards in hand- or
        // graph-derived barrier placement) is exactly what core validation
        // does not. Sync validation is expensive and false-positive-prone on
        // a full engine frame, which is why it is opt-in per device rather
        // than folded into `enableValidation`: turning it on for the normal
        // boot is a separate decision nobody has made yet.
        //
        // Requires `enableValidation` (it configures the validation layer; with
        // no layer loaded there is nothing to configure) and the
        // VK_EXT_validation_features instance extension. Missing either is a
        // WARN and a degrade, never a create failure -- see
        // DeviceVulkan.cpp's CreateVulkanNativeDevice.
        bool enableSyncValidation = false;
    };

    class ARCANE_API RenderDevice
    {
    public:
        // Returns null on failure (no adapter, missing runtime, ...);
        // the failure reason is logged via ARC_ERROR.
        static std::unique_ptr<RenderDevice> Create(const RenderDeviceDesc& desc);

        virtual ~RenderDevice() = default;

        virtual GraphicsBackend Backend() const = 0;
        virtual nvrhi::IDevice* Nvrhi() const = 0;
        virtual std::string AdapterName() const = 0;

        // NOTE: there is deliberately NO GpuCrashBackend() accessor here. The
        // instrumentation that consumes the backend (Render/GpuInstrumentation.hpp)
        // must be reachable from OffscreenCanvas and PickBuffer too, and those
        // hold only an nvrhi::IDevice* -- so the device layer publishes the
        // backend to a process-wide slot instead, installed and cleared beside
        // the Diagnostics GPU-section provider it shares a lifetime with. ONE
        // access path on purpose: an accessor here would be a second one, and
        // when both existed the accessor had zero callers repo-wide.

        // The window must outlive the swapchain. For Vulkan, the window
        // must have been created with WindowDesc::vulkan = true.
        // The swapchain must be destroyed before the RenderDevice that
        // created it (its teardown uses the device's native handles).
        virtual std::unique_ptr<Swapchain> CreateSwapchain(Window& window,
                                                           bool vsync) = 0;
    };
}
