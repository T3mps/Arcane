// OffscreenVehicle: the ordered boot extracted for reuse outside test code.
// See the header for the teardown contract. Create() below is the ONE
// factory -- same shape as NriGraphPixelTest.cpp's MakeVehicle, but this one
// returns null + logs instead of REQUIRE-ing, since it is not test-only.

#include <Arcane/Host/OffscreenVehicle.hpp>

#include <Arcane/Base/Log.hpp>

namespace Arcane
{
    std::unique_ptr<OffscreenVehicle> OffscreenVehicle::Create(const HostConfig& cfg,
                                                               std::uint32_t width,
                                                               std::uint32_t height,
                                                               const NriGraphContext::NodeSet& nodes)
    {
        // Private ctor -> can't use make_unique; the partial unwinds via RAII on any
        // early return (members destruct in reverse declaration order, the shutdown
        // order).
        std::unique_ptr<OffscreenVehicle> v(new OffscreenVehicle());

        RenderDeviceDesc desc;
        desc.backend = cfg.backend;
#if defined(ARCANE_DEBUG)
        // Mirror NriGraphContext.cpp's windowed creation half EXACTLY (same
        // three flags, same Debug-only gate). An offscreen run's
        // RenderErrorCount is the WHOLE verdict an agent gets -- there is no
        // human watching a window to notice what a narrower validation
        // surface would miss. Leaving any of these three off here would make
        // --offscreen quietly weaker than windowed, silently undermining
        // every verification built on top of it. This is legal here because
        // OffscreenVehicle::Create is the FIRST device the process creates
        // (see DeviceCreationD3D12.cpp's g_d3d12DeviceCreated latch); arming
        // enableD3D12DebugLayer anywhere else, after a device already exists,
        // would tear that device down instead.
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;   // VK-only; see RenderDeviceDesc.hpp
#endif
        v->m_native = NativeDeviceOwner::Create(desc);
        if (!v->m_native) { ARC_ERROR("[offscreen] no usable adapter for the requested backend"); return nullptr; }

        v->m_nri = NriDevice::Wrap(*v->m_native);
        if (!v->m_nri) { ARC_ERROR("[offscreen] NriDevice::Wrap failed"); return nullptr; }

        // vsync is meaningless with nothing to present to; CreateOffscreen ignores
        // it and the other surface-owned knobs.
        v->m_ctx = NriGraphContext::CreateOffscreen(cfg, *v->m_nri, width, height, nodes);
        if (!v->m_ctx) { ARC_ERROR("[offscreen] CreateOffscreen failed at {}x{}", width, height); return nullptr; }

        ARC_INFO("[offscreen] vehicle ready: {}x{} backend={} -- no window, no swapchain",
                 width, height, cfg.backend == GraphicsBackend::Vulkan ? "Vulkan" : "D3D12");
        return v;
    }

    OffscreenVehicle::~OffscreenVehicle() = default;
}
