// GpuContext: the ordered engine boot extracted from main.cpp. See the header
// for the teardown contract. This TU is a PURE refactor of the former boot
// block + the inline resize / backbuffer-framebuffer plumbing -- behavior is
// unchanged; only the ownership home moved.

#include <Arcane/Host/GpuContext.hpp>

#include <Arcane/Base/Log.hpp>

namespace Arcane
{
    std::unique_ptr<GpuContext> GpuContext::Create(const HostConfig& cfg)
    {
        // Private ctor -> can't use make_unique; the partial unwinds via RAII on any
        // early return (members destruct in reverse declaration order, the shutdown
        // order).
        auto ctx = std::unique_ptr<GpuContext>(new GpuContext());

        // window is the value member that destructs LAST (imgui/inputDevices hold
        // SDL window refs); created FIRST so its lifetime spans every consumer.
        WindowDesc wd;
        wd.title  = "Arcane Runtime";
        wd.vulkan = (cfg.backend == GraphicsBackend::Vulkan);
        if (!ctx->m_window.Create(wd)) { ARC_ERROR("GpuContext: window create failed"); return nullptr; }

        RenderDeviceDesc dd;
        dd.backend = cfg.backend;
        ctx->m_device = RenderDevice::Create(dd);
        if (!ctx->m_device) { ARC_ERROR("GpuContext: device create failed"); return nullptr; }

        ctx->m_swapchain = ctx->m_device->CreateSwapchain(ctx->m_window, cfg.vsync);
        if (!ctx->m_swapchain) { ARC_ERROR("GpuContext: swapchain create failed"); return nullptr; }

        ctx->m_shaders = ShaderLibrary::Create(ctx->m_device->Nvrhi(), cfg.backend, "shaders");
        if (!ctx->m_shaders) { ARC_ERROR("GpuContext: shader library create failed"); return nullptr; }

        ctx->m_canvas = CreateCanvas(ctx->m_device->Nvrhi(),
                                     ctx->m_swapchain->Width(),
                                     ctx->m_swapchain->Height());
        if (!ctx->m_canvas) { ARC_ERROR("GpuContext: canvas create failed"); return nullptr; }

        ctx->m_batcher = Batcher2D::Create(ctx->m_device->Nvrhi(), *ctx->m_shaders);
        if (!ctx->m_batcher) { ARC_ERROR("GpuContext: batcher create failed"); return nullptr; }

        ctx->m_tonemap = TonemapPass::Create(ctx->m_device->Nvrhi(), *ctx->m_shaders);
        if (!ctx->m_tonemap) { ARC_ERROR("GpuContext: tonemap create failed"); return nullptr; }

        // ImGuiLayer taps window events -> must outlive nothing but be destroyed
        // before the window; member order (after window) guarantees that.
        ctx->m_imgui = ImGuiLayer::Create(ctx->m_window, *ctx->m_device, *ctx->m_shaders);
        if (!ctx->m_imgui) { ARC_ERROR("GpuContext: imgui create failed"); return nullptr; }

        ctx->m_inputDevices = InputDevices::Create();
        if (!ctx->m_inputDevices) { ARC_ERROR("GpuContext: input devices create failed"); return nullptr; }
        ctx->m_input = InputActions::Create();
        if (!ctx->m_input) { ARC_ERROR("GpuContext: input actions create failed"); return nullptr; }
        // The input-actions CONFIG is loaded by the host AFTER it opens a project, so the
        // config can resolve through the project's game:// mount (or data/ when no project).
        // GpuContext only creates the empty action system here. See HostBoot::LoadInputConfig.

        // One reused command list for the whole loop. Holds an NVRHI handle ->
        // declared/destroyed before the device (see header).
        ctx->m_commandList = ctx->m_device->Nvrhi()->createCommandList();

        return ctx;
    }

    void GpuContext::OnResize(std::uint32_t w, std::uint32_t h)
    {
        // Cached backbuffer framebuffers reference the old swapchain textures -- drop
        // them, resize the swapchain to the event extent, then resize the canvas to
        // the swapchain's ACTUAL extent (the surface may clamp the requested size).
        m_framebuffers.clear();
        m_swapchain->Resize(w, h);
        m_canvas->Resize(m_swapchain->Width(), m_swapchain->Height());
    }

    Canvas* GpuContext::EnsurePost()
    {
        // Track the scene canvas' CURRENT extent (resizes are host-paced, so a
        // size check per posted frame is the whole sync story).
        if (!m_post)
            m_post = CreateCanvas(m_device->Nvrhi(),
                                  m_canvas->Width(), m_canvas->Height());
        else if (m_post->Width() != m_canvas->Width() ||
                 m_post->Height() != m_canvas->Height())
            m_post->Resize(m_canvas->Width(), m_canvas->Height());
        return m_post.get();
    }

    nvrhi::FramebufferHandle& GpuContext::FramebufferFor(nvrhi::ITexture* backbuffer)
    {
        nvrhi::FramebufferHandle& fb = m_framebuffers[backbuffer];
        if (!fb)
            fb = m_device->Nvrhi()->createFramebuffer(
                nvrhi::FramebufferDesc().addColorAttachment(backbuffer));
        return fb;
    }
}
