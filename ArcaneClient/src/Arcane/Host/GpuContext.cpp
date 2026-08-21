// GpuContext: the ordered engine boot extracted from main.cpp. See the header
// for the teardown contract. Create() below is the ONE factory.

#include <Arcane/Host/GpuContext.hpp>

#include <Arcane/Base/Log.hpp>

namespace Arcane
{
    namespace
    {
        // The window shape: hidden until the first presented frame (the
        // caller reveals it via Window::Show once the render vehicle that
        // owns this window's only swapchain exists). The 1280x720 default
        // matters: every golden baseline was captured at this size, so a
        // different default here would make every stage compare a dimension
        // mismatch rather than a pixel one.
        WindowDesc HostWindowDesc(const HostConfig& cfg)
        {
            WindowDesc wd;
            wd.title  = "Arcane Runtime";
            wd.vulkan = (cfg.backend == GraphicsBackend::Vulkan);
            wd.hidden = true;
            return wd;
        }
    }

    std::unique_ptr<GpuContext> GpuContext::Create(const HostConfig& cfg)
    {
        // Private ctor -> can't use make_unique; the partial unwinds via RAII on any
        // early return (members destruct in reverse declaration order, the shutdown
        // order).
        auto ctx = std::unique_ptr<GpuContext>(new GpuContext());

        // THE window of the process, created first for the reason stated in
        // the header: it destructs LAST, and the NRI swapchain the caller
        // builds over it must be gone before it is.
        if (!ctx->m_window.Create(HostWindowDesc(cfg)))
        {
            ARC_ERROR("GpuContext: window create failed");
            return nullptr;
        }

        // A CPU BATCHER, and there is no other kind: it takes no device and
        // creates no GPU object. Begin/SetLayer/Quad*/Drain/RegisterMaterial/
        // SetGlobals/MaterialDesc/Stats are the whole data-supply side of the
        // frame; the
        // caller's Batch2DNode DRAINS this instance and issues the draws
        // through NRI. One batcher, one batching algorithm.
        ctx->m_batcher = Batcher2D::Create();
        if (!ctx->m_batcher) { ARC_ERROR("GpuContext: batcher create failed"); return nullptr; }

        // ImGuiLayer has ONE flavor: context + SDL3 platform backend + event
        // tap, and no renderer -- a graph node is the renderer.
        ctx->m_imgui = ImGuiLayer::Create(ctx->m_window);
        if (!ctx->m_imgui) { ARC_ERROR("GpuContext: imgui create failed"); return nullptr; }

        ctx->m_inputDevices = InputDevices::Create();
        if (!ctx->m_inputDevices) { ARC_ERROR("GpuContext: input devices create failed"); return nullptr; }
        ctx->m_input = InputActions::Create();
        if (!ctx->m_input) { ARC_ERROR("GpuContext: input actions create failed"); return nullptr; }
        // The input-actions CONFIG is loaded by the host AFTER it opens a project, so the
        // config can resolve through the project's game:// mount (or data/ when no project).
        // GpuContext only creates the empty action system here. See HostBoot::LoadInputConfig.

        return ctx;
    }
}
