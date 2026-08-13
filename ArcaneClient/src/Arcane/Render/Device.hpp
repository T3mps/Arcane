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

    // Total NVRHI Error/Fatal diagnostics since process start (all devices).
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
