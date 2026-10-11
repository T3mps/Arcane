// OffscreenVehicle: the ordered boot extracted for reuse outside test code.
// See the header for the teardown contract. Create() below is the ONE
// factory -- same shape as NriGraphPixelTest.cpp's MakeVehicle, but this one
// returns null + logs instead of REQUIRE-ing, since it is not test-only.

#include <Arcane/Host/OffscreenVehicle.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Nri/NriDiagnostics.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>   // MakeRenderDeviceDesc -- render.debug.*

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

        // The same desc as NriGraphContext.cpp's windowed creation half: the
        // three validation switches come from render.debug.* (on by default
        // in Debug builds; settings arc S6-16), so --headless is never
        // quietly weaker than windowed. An offscreen run's RenderErrorCount
        // is the WHOLE verdict an agent gets -- there is no human watching a
        // window to notice what a narrower validation surface would miss.
        //
        // enableD3D12DebugLayer's legality here is guaranteed only for
        // ArcaneRuntime: RuntimeApp::MainLoop (RuntimeApp.cpp) builds exactly
        // ONE graphics device per process, windowed or offscreen, so under
        // that host this Create() call really is the first (and only)
        // device, and EnableDebugLayer's before-any-device requirement holds.
        // Inside ArcaneTests that guarantee does NOT hold -- Catch2 runs
        // cases in randomized order and other [gpu] cases create their own
        // devices, so a device may already exist by the time an offscreen
        // vehicle is built there. That is still safe: DeviceCreationD3D12.cpp's
        // g_d3d12DeviceCreated latch turns this into a no-op rather than the
        // illegal after-device call -- it DECLINES to arm the layer for the
        // later device (WARN, losing that one validation channel) instead of
        // calling EnableDebugLayer and tearing down the device that already
        // exists.
        RenderDeviceDesc desc = MakeRenderDeviceDesc();
        desc.backend = cfg.backend;   // the host's adopted backend (render.backend at boot)
        v->m_native = NativeDeviceOwner::Create(desc);
        if (!v->m_native) { ARC_ERROR("[offscreen] no usable adapter for the requested backend"); return nullptr; }

        v->m_nri = NriDevice::Wrap(*v->m_native);
        if (!v->m_nri) { ARC_ERROR("[offscreen] NriDevice::Wrap failed"); return nullptr; }

        // THE CRASH CHAIN, armed by the device's OWNER -- this vehicle, under
        // --headless in both hosts -- exactly as the windowed
        // NriGraphContext::Create arms the device IT creates, and at the same
        // point: right after the wrap, before anything below can fail on a
        // device with no device-removed observation point. CreateOffscreen
        // still never arms (it BORROWS its device); before this line nothing
        // armed a headless process at all, so a device loss latched through
        // RenderErrorLatch::NoteDeviceLost with no hook, the host stopped with
        // render-failed, and no gpu-crash report was written (G1, R109).
        //
        // Arm() refuses when a chain is already installed, and the RETURN
        // VALUE gates the Disarm in the destructor, so a vehicle built beside
        // an incumbent owner (an [gpu] test, a future second device) neither
        // displaces nor unplugs it.
        v->m_armedDiagnostics = NriDiagnostics::Arm(*v->m_nri);

        // vsync is meaningless with nothing to present to; CreateOffscreen ignores
        // it and the other surface-owned knobs.
        v->m_ctx = NriGraphContext::CreateOffscreen(cfg, *v->m_nri, width, height, nodes);
        if (!v->m_ctx) { ARC_ERROR("[offscreen] CreateOffscreen failed at {}x{}", width, height); return nullptr; }

        ARC_INFO("[offscreen] vehicle ready: {}x{} backend={} -- no window, no swapchain",
                 width, height, cfg.backend == GraphicsBackend::Vulkan ? "Vulkan" : "D3D12");
        return v;
    }

    OffscreenVehicle::~OffscreenVehicle()
    {
        // FIRST, before any member goes: the armed slots name a backend built
        // over m_nri, and a slot outliving what it names dangles -- the same
        // reason ~NriGraphContext disarms at the top of its body.
        if (m_armedDiagnostics)
            NriDiagnostics::Disarm();
    }
}
