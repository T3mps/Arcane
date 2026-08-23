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
